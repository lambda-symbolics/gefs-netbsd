/*
 * Plan 9 <u.h> for the NetBSD port of gefs: the basic types, for the kernel
 * and for userland.  The core is compiled with -fplan9-extensions.
 */
#ifndef GEFS_U_H
#define GEFS_U_H

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/stdarg.h>
#else
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#endif

#define nil		((void*)0)
typedef unsigned char	uchar;
typedef unsigned short	ushort;
typedef unsigned int	uint;
typedef unsigned long	ulong;
typedef signed char	schar;
typedef long long	vlong;
typedef unsigned long long uvlong;
typedef uintptr_t	uintptr;
typedef intptr_t	intptr;
typedef size_t		usize;
typedef uint		Rune;
typedef unsigned char	u8int;
typedef unsigned short	u16int;
typedef unsigned int	u32int;
typedef unsigned long long u64int;
typedef signed char	s8int;
typedef signed short	s16int;
typedef signed int	s32int;
typedef signed long long s64int;

/* atomically accessed values; see the a*() functions in <libc.h> */
#define Along	long
#define Avlong	vlong
#define Aptr	void*

#define nelem(x)	(sizeof(x)/sizeof((x)[0]))
#define USED(...)	((void)0)
#define SET(...)	((void)0)

#endif
