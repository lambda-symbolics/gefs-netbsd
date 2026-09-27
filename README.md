# GEFS for NetBSD

A native NetBSD port of GEFS, the copy-on-write Bε-tree file system of
9front (by Ori Bernstein): a kernel file system (vfsops, vnode operations,
UBC), not FUSE, that NetBSD boots from and runs its root on, and that keeps a
laptop's disk idle whenever nothing changes.

Design, status and what the port taught: `doc/DESIGN.org`.  The 9front
sources were imported unmodified in the first commit; every change to them
since is marked `NetBSD:` in the code.

## Layout

    sys/fs/gefs/            the core (from 9front) and the NetBSD kernel side
      compat/               Plan 9 headers: u.h, libc.h, avl.h, fcall.h
      p9fmt.c p9err.c       print(2) and error handling, kernel and userland
      p9kern.c p9user.c     locks, memory, time, device I/O for each
      ops.c                 POSIX operations on the core (create, rename, ...)
      sync.c                commits, labels, snapshots, sweeping, the reserve
      snapadm.c             adm/ctl commands, status, snapshot schedules
      gefs_vfsops.c         devices, mounts, sync, admin ioctl, mountroot
      gefs_vnops.c          vnode operations, UBC through bmap/strategy
      gefs_work.c           the mutator and reader threads, page I/O queue
      files.gefs            to build GEFS into a kernel
    sys/modules/gefs        the module
    sys/lib/libsa/gefs.c    read-only gefs for the boot loaders (efiboot)
    sbin/mount_gefs         mount helper, mount_gefs(8)
    sbin/gefs               gefs(8): newfs_gefs, fsck_gefs, snapshots
    patches/                changes to NetBSD's own files (efiboot, the tree)
    tests/gefstest.c        core and operations tests, userland
    tests/fsxlite.c         random operations against a copy in memory
    tests/sagefs/           the boot loader's reader, in userland

## Building

Userland tools and tests (NetBSD or Linux, GCC):

    cd sbin/gefs && make -f Makefile.host
    truncate -s 512M /tmp/t.img && ./gefstest /tmp/t.img

The module, in a NetBSD 11 source tree (`sys/fs/gefs` and `sys/modules/gefs`
linked or copied into `/usr/src/sys`):

    cd /usr/src/sys/modules/gefs && make && modload ./gefs.kmod
    newfs_gefs /dev/rdk1 && mount -t gefs /dev/dk1 /mnt

Built in: add `include "fs/gefs/files.gefs"` (or apply
`patches/netbsd-11-tree.diff`) and `file-system GEFS` to a kernel
configuration.  The EFI boot loader with gefs: `patches/netbsd-11-efiboot.diff`
plus `sys/lib/libsa/gefs.[ch]`.

## Using it

    newfs_gefs /dev/rdk1                   # retention 60@m 24@h 14@d
    mount -t gefs /dev/dk1 /mnt
    gefs snap /mnt main before-upgrade     # a snapshot, taken now
    gefs fork /mnt empty obj               # a new empty mutable label
    gefs retain /mnt obj none
    mount -t gefs -o snap=obj /dev/dk1 /usr/obj
    gefs status /mnt                       # usage, retention, labels
    mount -t gefs -o ro,snap=before-upgrade /dev/dk1 /n/old

See gefs(8) and mount_gefs(8).

## State

Works in a NetBSD 11.0 VM: mount, all file types, reads and writes through
UBC (mmap, exec), rename, hard links, symlinks, device nodes, fifos, relatime,
fsync, crash consistency (the last commit survives), snapshots and forks
(taken, deleted, mounted next to main, taken on a schedule only when
something changed), booting under UEFI from a disk with only an ESP and a
gefs partition, a full file system (rm and unmount work, write says ENOSPC),
low memory, and an idle mount writes nothing.  Not yet: NFS export (gefs has
no index from inode number to entry), ctime (reported as mtime), extended
attributes.
