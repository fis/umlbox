/*
 * Copyright (C) 2011 Gregor Richards
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/random.h>
#include <linux/reboot.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "config.pb-c.h"

#define DEFAULT_PATH "/usr/local/bin:/bin:/usr/bin"
#define DEFAULT_SHELL "/bin/bash"

static void handle_random(size_t len, uint8_t *data);
static void handle_tty_raw(const char *dev);
static void handle_mount(const Mount *mnt);
static bool handle_run(const Run *run);
static void handle_sigchld(int sig, siginfo_t *info, void *ctx);

static void fail(const char *msg);
static void open_to(int new_fd, const char *path, int flags, int fallback_fd);
static ssize_t readall(int fd, void *buf, size_t count);
static void mkdirs(const char *dir);
static void hostify(const char *cwd, bool user, uid_t uid, gid_t gid);
static void set_limits(size_t n_limit, Limit **limit);
static void dump_config(uint32_t len, const Config *cfg);

static int in_init = 1; // used to modify behavior of fail for children

#define MUST(msg, err, func, ...) ({ \
  __typeof__(err) ret = func(__VA_ARGS__); \
  if (ret == err) fail(msg); \
  ret; })

int main()
{
  srandom(time(NULL));

  // prepare initial environment

  MUST("mknod /console", -1, mknod, "/console", 0644 | S_IFCHR, makedev(5, 1));
  open_to(0, "/console", O_RDONLY, -1);
  open_to(1, "/console", O_WRONLY, -1);
  open_to(2, "/console", O_WRONLY, -1);

  printf("umlbox init\n");

  MUST("mknod /ubda", -1, mknod, "/ubda", 0644 | S_IFBLK, makedev(98, 0));
  MUST("mknod /null", -1, mknod, "/null", 0644 | S_IFCHR, makedev(1, 3));
  {
    char dev[sizeof "/ttyXX"];
    for (int i = 1; i < 16; i++) {
      snprintf(dev, sizeof dev, "/tty%d", i);
      MUST("mknod /ttyN", -1, mknod, dev, 0644 | S_IFCHR, makedev(4, i));
    }
  }

  if (clearenv() != 0)
    fail("clearenv");
  setenv("PATH", DEFAULT_PATH, 1);
  setenv("TERM", "linux", 1);
  setenv("HOME", "/tmp", 1);

  MUST("mkdir /host", -1, mkdir, "/host", 0777u);

  {
    struct sigaction act = {
      .sa_sigaction = handle_sigchld,
      .sa_flags = SA_SIGINFO,
    };
    MUST("sigaction", -1, sigaction, SIGCHLD, &act, NULL);
  }

  // parse the configuration

  Config *cfg;
  {
    int fd = MUST("open /ubda", -1, open, "/ubda", O_RDONLY);

    uint32_t hdr[2];
    MUST("read /ubda header", -1, readall, fd, hdr, sizeof hdr);
    if (hdr[0] != 0xdeadbeefu) {
      printf("unexpected header: %#08x != 0xdeadbeef\n", (unsigned) hdr[0]);
      errno = EINVAL, fail("bad /ubda header");
    }

    uint8_t *data = MUST("malloc config", (void *) 0, malloc, hdr[1]);
    MUST("read config", -1, readall, fd, data, hdr[1]);
    cfg = config__unpack(0, hdr[1], data);
    if (!cfg)
      errno = EINVAL, fail("bad config");

    close(fd);
    dump_config(hdr[1], cfg);
  }

  // execute all the actions

  if (cfg->random.len > 0)
    handle_random(cfg->random.len, cfg->random.data);

  for (size_t i = 0; i < cfg->n_tty_raw; i++)
    handle_tty_raw(cfg->tty_raw[i]);

  for (size_t i = 0; i < cfg->n_mount; i++)
    handle_mount(cfg->mount[i]);

  for (size_t i = 0; i < cfg->n_run; i++) {
    bool timed_out = handle_run(cfg->run[i]);
    if (timed_out)
      break;
  }

  sync();
  reboot(LINUX_REBOOT_CMD_POWER_OFF);
  return 0;
}

static void handle_random(size_t len, uint8_t *data) {
  MUST("mknod /random", -1, mknod, "/random", 0644 | S_IFCHR, makedev(1, 8));
  int fd = MUST("open /random", -1, open, "/random", O_RDONLY);

  struct rand_pool_info *info = MUST("malloc rand_pool_info", (void *) 0, malloc, sizeof *info + len);
  info->entropy_count = 8 * len;
  info->buf_size = len;
  memcpy(info->buf, data, len);
  MUST("ioctl /random", -1, ioctl, fd, RNDADDENTROPY, info);
  free(info);

  MUST("close /random", -1, close, fd);
}

static void handle_tty_raw(const char *dev) {
  printf("umlbox tty_raw: %s\n", dev);

  int fd = MUST("open tty_raw", -1, open, dev, O_RDWR);

  struct termios tio;
  MUST("tcgetattr", -1, tcgetattr, fd, &tio);
  cfmakeraw(&tio);
  MUST("tcsetattr", -1, tcsetattr, fd, TCSANOW, &tio);

  close(fd);
}

static void handle_mount(const Mount *mnt) {
  char target[sizeof "/host/" + strlen(mnt->target)];
  snprintf(target, sizeof target, "/host%s%s", *mnt->target == '/' ? "" : "/", mnt->target);

  printf("umlbox mount: %s\n", target);

  unsigned long flags = 0;
  if (mnt->ro) flags |= MS_RDONLY;
  if (mnt->nosuid) flags |= MS_NOSUID;

  mkdirs(target);
  MUST("mount", -1, mount, mnt->source, target, mnt->fstype, flags, *mnt->data ? mnt->data : NULL);
}

static bool handle_run(const Run *run) {
  printf("umlbox run: %s\n", run->cmd);

  sigset_t orig_mask, chld_mask;
  sigemptyset(&chld_mask);
  sigaddset(&chld_mask, SIGCHLD);
  MUST("sigprocmask (block SIGCHLD)", -1, sigprocmask, SIG_BLOCK, &chld_mask, &orig_mask);

  uid_t uid = run->uid;
  gid_t gid = run->gid;
  if (run->user) {
    if (uid == 0) uid = random() % 995000 + 5000;
    if (gid == 0) gid = random() % 995000 + 5000;
  }

  pid_t cat = -1;
  int cat_pipe[2];
  if (run->cat_output) {
    MUST("pipe (cat)", -1, pipe, cat_pipe);

    cat = MUST("fork", -1, fork);
    if (cat == 0) {
      in_init = 0;

      MUST("dup2 (cat -> in)", -1, dup2, cat_pipe[0], 0);
      open_to(1, *run->output ? run->output : "/null", O_WRONLY, -1);
      open_to(2, *run->error ? run->error : 0, O_WRONLY, 1);
      if (cat_pipe[0] > 2) close(cat_pipe[0]);
      if (cat_pipe[1] > 2) close(cat_pipe[1]);

      hostify(run->cwd, run->user, uid, gid);

      char *argv[2] = {"cat", NULL};
      MUST("execvp", -1, execvp, "cat", argv);
      exit(0);
    }
  }

  pid_t child = MUST("fork", -1, fork);
  if (child == 0) {
    in_init = 0;

    open_to(0, *run->input ? run->input : "/null", O_RDONLY, -1);
    if (cat != -1) {
      if (cat_pipe[1] != 1) MUST("dup2 (cat -> out)", -1, dup2, cat_pipe[1], 1);
      if (cat_pipe[1] != 2) MUST("dup2 (cat -> err)", -1, dup2, cat_pipe[1], 2);
      if (cat_pipe[0] > 2) close(cat_pipe[0]);
      if (cat_pipe[1] > 2) close(cat_pipe[1]);
    } else {
      open_to(1, *run->output ? run->output : "/null", O_WRONLY, -1);
      open_to(2, *run->error ? run->error : 0, O_WRONLY, 1);
    }

    for (size_t i = 0; i < run->n_env; i++)
      setenv(run->env[i]->key, run->env[i]->value, /* overwrite= */ 1);

    hostify(run->cwd, run->user, uid, gid);
    set_limits(run->n_limit, run->limit);

    char **argv = MUST("malloc argv", (void *) 0, malloc, (run->n_arg + 3) * sizeof *argv);
    argv[0] = DEFAULT_SHELL;
    argv[1] = run->cmd;
    for (size_t i = 0; i < run->n_arg; i++)
      argv[2 + i] = run->arg[i];
    argv[2 + run->n_arg] = 0;

    char *cmd = run->cmd;
    char *path = 0;
    if (strchr(cmd, '/') == 0) {
      path = getenv("PATH");
      if (!path) path = DEFAULT_PATH;
      cmd = MUST("malloc (cmd)", (void *) 0, malloc, strlen(path) + strlen(run->cmd) + 2);
    }
    int exec_errno = 0;
    do {
      if (path) {
        char *colon = strchr(path, ':');
        if (colon) {
          sprintf(cmd, "%.*s/%s", (int) (colon - path), path, run->cmd);
          path = colon + 1;
        } else {
          sprintf(cmd, "%s/%s", path, run->cmd);
          path += strlen(path);
        }
      }
      execv(cmd, argv + 1);
      if (errno == ENOEXEC) {
        argv[1] = cmd;
        execv(DEFAULT_SHELL, argv);
        if (exec_errno != EACCES) exec_errno = errno;
        break;
      }
      if (exec_errno != EACCES) exec_errno = errno;
    } while (path && *path);
    printf("%s? %s\n", run->cmd, strerror(exec_errno));
    exit(1);
  }

  if (run->daemon) {
    MUST("sigprocmask (unblock SIGCHLD)", -1, sigprocmask, SIG_SETMASK, &orig_mask, 0);
    return false;
  }

  if (cat != -1) {
    close(cat_pipe[0]);
    close(cat_pipe[1]);
  }

  bool timed_out = false;
  struct pollfd timeout_fd = { .fd = 0, .events = POLLIN };

  bool child_running = 1, cat_running = run->cat_output;
  while (true) {
    pid_t waited = waitpid(-1, 0, WNOHANG);
    if (waited == -1)
      fail("wait");
    if (waited != 0) {
      if (waited == child) child_running = false;
      if (waited == cat) cat_running = false;
      if (!child_running && !cat_running)
        break;
      continue;
    }

    int ret = ppoll(&timeout_fd, 1, 0, &orig_mask);
    if (ret == -1 && errno == EINTR) {
      continue;
    }
    if (ret == -1)
      fail("poll (timeout signal)");

    unsigned char hard_timeout[2];
    MUST("read (timeout signal)", -1, read, 0, hard_timeout, 2);
    timed_out = true;
    if (*hard_timeout == 'Y')
      break;
    if (child_running)
      MUST("kill", -1, kill, child, SIGTERM);
  }

  MUST("sigprocmask (unblock SIGCHLD)", -1, sigprocmask, SIG_SETMASK, &orig_mask, 0);
  return timed_out;
}

static void handle_sigchld(int sig, siginfo_t *info, void *ctx) {
  // poll already interrupted by this signal, no action needed
}

// utilities

static void fail(const char *msg) {
  printf("umlbox: %s: %s\n", msg, strerror(errno));
  if (in_init)
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
  exit(1);
}

static void open_to(int new_fd, const char *path, int flags, int fallback_fd) {
  int fd = fallback_fd;
  bool do_open = path && *path;
  if (do_open) fd = MUST("open", -1, open, path, flags);
  if (fd != -1 && fd != new_fd) {
    MUST("dup2", -1, dup2, fd, new_fd);
    if (do_open) close(fd);
  }
}

static ssize_t readall(int fd, void* buf, size_t count) {
  char* at = (char*) buf;
  size_t got = 0;
  while (got < count) {
    ssize_t chunk = read(fd, at, count - got);
    if (chunk < 0)
      return -1;
    if (chunk == 0) {
      errno = EPIPE;
      return -1;
    }
    at += chunk;
    got -= chunk;
  }
  return count;
}

static void mkdirs(const char *dir) {
  MUST("chdir /", -1, chdir, "/");

  char buf[strlen(dir)+1];
  strcpy(buf, dir);

  char *tail = buf;
  do {
    char *part = tail;
    char *slash = strchr(tail, '/');
    if (slash) {
      *slash = 0;
      tail = slash + 1;
    } else {
      tail = 0;
    }
    if (!*part)
      continue;
    mkdir(part, 0777);
    MUST("chdir", -1, chdir, part);
  } while (tail);

  MUST("chdir /", -1, chdir, "/");
}

static void hostify(const char *cwd, bool user, uid_t uid, gid_t gid) {
  MUST("chdir root", -1, chdir, "/host");
  MUST("chroot", -1, chroot, ".");
  if (*cwd) MUST("chdir cwd", -1, chdir, cwd);

  if (user) {
    MUST("setgid", -1, setgid, gid);
    MUST("setuid", -1, setuid, uid);
  }
}

static void set_limits(size_t n_limit, Limit **limit) {
  static const struct {
    bool valid;
    int resource;
  } resource_map[] = {
    [LIMIT__RESOURCE__AS] = { true, RLIMIT_AS },
    [LIMIT__RESOURCE__CORE] = { true, RLIMIT_CORE },
    [LIMIT__RESOURCE__CPU] = { true, RLIMIT_CPU },
    [LIMIT__RESOURCE__DATA] = { true, RLIMIT_DATA },
    [LIMIT__RESOURCE__FSIZE] = { true, RLIMIT_FSIZE },
    [LIMIT__RESOURCE__MEMLOCK] = { true, RLIMIT_MEMLOCK },
    [LIMIT__RESOURCE__NOFILE] = { true, RLIMIT_NOFILE },
    [LIMIT__RESOURCE__NPROC] = { true, RLIMIT_NPROC },
    [LIMIT__RESOURCE__STACK] = { true, RLIMIT_STACK },
  };
  static const int n_resource_map = sizeof resource_map / sizeof *resource_map;

  struct rlimit rlim;
  for (size_t i = 0; i < n_limit; i++) {
    Limit *l = limit[i];
    if (l->resource < 0 || l->resource >= n_resource_map || !resource_map[l->resource].valid)
      errno = EINVAL, fail("set_limits");
    rlim.rlim_cur = l->soft >= 0 ? l->soft : RLIM_INFINITY;
    rlim.rlim_max = l->hard >= 0 ? l->hard : RLIM_INFINITY;
    MUST("setrlimit", -1, setrlimit, resource_map[l->resource].resource, &rlim);
  }
}

static void dump_config(uint32_t len, const Config *cfg) {
  printf("umlbox config: %u bytes:\n", len);

  for (size_t i = 0; i < cfg->n_tty_raw; i++)
    printf("- tty_raw: %s\n", cfg->tty_raw[i]);

  for (size_t i = 0; i < cfg->n_mount; i++) {
    const Mount *m = cfg->mount[i];
    printf("- mount: %s ('%s', '%s', '%s', %d, %d)\n", m->target, m->source, m->fstype, m->data, m->ro, m->nosuid);
  }

  for (size_t i = 0; i < cfg->n_run; i++)
    printf("- run: %s\n", cfg->run[i]->cmd);
}
