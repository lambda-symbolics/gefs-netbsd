#	$NetBSD$
#
# gefs(8), newfs_gefs, fsck_gefs: the gefs core (sys/fs/gefs, Plan 9 C
# built with -fplan9-extensions) linked into a userland tool.

PROG=	gefs
SRCS=	gefs.c host.c
MAN=	gefs.8
LINKS=	${BINDIR}/gefs ${BINDIR}/newfs_gefs \
	${BINDIR}/gefs ${BINDIR}/fsck_gefs
MLINKS=	gefs.8 newfs_gefs.8 gefs.8 fsck_gefs.8

NETBSDSRCDIR?=	/usr/src	# unset when make runs outside a tree build
GEFS=	${NETBSDSRCDIR}/sys/fs/gefs
.PATH:	${GEFS}
SRCS+=	p9fmt.c p9err.c p9user.c avl.c hash.c pack.c blk.c cache.c tree.c \
	snap.c load.c ream.c check.c dump.c error.c sync.c ops.c snapadm.c

CPPFLAGS+=	-I${GEFS}/compat -I${GEFS} -I${.CURDIR}
COPTS+=		-std=gnu11 -fplan9-extensions
CWARNFLAGS+=	-Wno-missing-braces -Wno-parentheses -Wno-sign-compare \
		-Wno-unknown-pragmas -Wno-overflow -Wno-char-subscripts \
		-Wno-missing-prototypes -Wno-strict-prototypes -Wno-unused
NOGCCERROR=	yes
WARNS=		1

LDADD+=	-lpthread
DPADD+=	${LIBPTHREAD}

.include <bsd.prog.mk>
