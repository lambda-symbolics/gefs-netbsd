#	GEFS for NetBSD, as a kernel module.

WARNS=		1
NOGCCERROR=	yes

# in a NetBSD tree this is ../Makefile.inc; SYSDIR finds it from elsewhere
SYSDIR?=	/usr/src/sys
.include "${SYSDIR}/modules/Makefile.inc"

.PATH:	${S}/fs/gefs

KMOD=	gefs
SRCS=	gefs_vfsops.c gefs_vnops.c gefs_work.c p9kern.c
# the core, from 9front
SRCS+=	p9fmt.c p9err.c avl.c hash.c pack.c blk.c cache.c tree.c snap.c \
	load.c check.c dump.c error.c sync.c ops.c snapadm.c

CPPFLAGS+=	-I${S}/fs/gefs/compat -I${S}/fs/gefs
# the core uses Plan 9 C's anonymous struct embedding; kernel stacks
# are 12 KiB, so keep GCC from inlining helpers' buffers into one frame
COPTS+=		-fplan9-extensions -fconserve-stack
CWARNFLAGS+=	-Wno-missing-braces -Wno-parentheses -Wno-sign-compare \
		-Wno-unknown-pragmas -Wno-overflow -Wno-char-subscripts

.include <bsd.kmodule.mk>
