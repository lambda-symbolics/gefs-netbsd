# GEFS for NetBSD

A native NetBSD port of GEFS, the copy-on-write Bε-tree file system of
9front (by Ori Bernstein): a kernel file system (vfsops, vnode operations,
UBC), not FUSE, with the goal of running the root file system on it, and of
keeping a laptop's disk idle whenever nothing changes.

Design and status: `doc/DESIGN.org`.  The 9front sources were imported
unmodified in the first commit; every change to them since is marked
`NetBSD:` in the code.

## Layout

    sys/fs/gefs/            the core (from 9front) and the NetBSD kernel side
      compat/               Plan 9 headers: u.h, libc.h, avl.h, fcall.h
      p9fmt.c p9err.c       print(2) and error handling, kernel and userland
      p9kern.c p9user.c     locks, memory, time, device I/O for each
      ops.c                 POSIX operations on the core (create, rename, ...)
      sync.c                commits, snapshots, clearing
      gefs_vfsops.c         mount, unmount, sync, statvfs, mountroot, module
      gefs_vnops.c          vnode operations, UBC through bmap/strategy
      gefs_work.c           the mutator and reader threads
      files.gefs            to build GEFS into a kernel
    sys/modules/gefs        the module
    sbin/mount_gefs         mount helper
    usr.sbin/gefs           gefs(8): ream (newfs_gefs), check (fsck_gefs), ls
    tests/gefstest.c        core and operations tests, userland

## Building

Userland tools and tests (NetBSD or Linux, GCC):

    cd usr.sbin/gefs && make -f Makefile.host
    truncate -s 256M /tmp/t.img && ./gefstest /tmp/t.img

The module, in a NetBSD 11 source tree (`sys/fs/gefs` and `sys/modules/gefs`
linked or copied into `/usr/src/sys`):

    cd /usr/src/sys/modules/gefs && make && modload ./gefs.kmod
    newfs_gefs /dev/rld2d && mount -t gefs /dev/ld2d /mnt

Built in: add `include "fs/gefs/files.gefs"` and `file-system GEFS` to a
kernel configuration; `config netbsd root on ld3d type gefs` for a gefs root.

## State

Works in a NetBSD 11.0 VM: mount, all file types, reads and writes through
UBC (mmap, exec), rename, hard links, symlinks, device nodes, fifos, relatime,
fsync, crash consistency (the last commit survives), and an idle mount writes
nothing.  Not yet: snapshot administration from NetBSD, NFS export (gefs has
no index from inode number to entry), a boot loader reader, ctime (reported as
mtime), extended attributes.
