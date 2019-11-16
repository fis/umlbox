# umlbox

umlbox is a UML-based (UserMode Linux-based) solution for sandboxing
applications.

## Original README

You can use any UML kernel which supports the initrd.gz and hostfs
filesystems to run UMLBox. On Debian, the user-mode-linux package
includes such a kernel.  Alternatively, you may extract Linux 3.7 to
umlbox/linux-3.7 (substitute for another version in Makefile if you
prefer), and a suitable kernel will be built for you. Other versions
of Linux should work as well, add the flag LINUX=<dir> to your `make`
line to use another version.

Use `make` or `make all` to build umlbox and an included kernel. Use
`make nokernel` to build only the non-kernel components, to use
another UML kernel.  In either case, `make install` installs umlbox
(ignore error output if you're not installing the kernel), and you may
use `make install PREFIX=<some prefix>` to install to a custom prefix.

## Fork-specific details

This fork of https://github.com/GregorR/umlbox has been mildly
modified to support the #esoteric bot, HackEso.

At least in the past, it's been possible to use this software in
conjunction with the Debian `user-mode-linux` packaging, which you can
build yourself from sources in order to customize the configuration.
The steps (preferrably in an empty directory) are approximately:

```shell
$ apt-get source user-mode-linux
$ sudo apt-get build-dep user-mode-linux
$ cd user-mode-linux-4.9-1um/
$ debian/rules unpack
$ debian/rules patch
$ cp config.amd64 config.amd64.orig
$ cp ../hackeso-uml-4.9-1um.config config.amd64
$ # at this point, my notes say to "patch in debian/rules patch"
$ vi debian/changelog  # add 2: in front of version to prevent upgrades
$ dpkg-buildpackage -rfakeroot -nc -uc
```

You will end up with a .deb in the parent directory. In the HackEso
setup, this would get installed in the HackEso container.

Then build umlbox with `make nokernel`.
