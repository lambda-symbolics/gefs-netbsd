/* the kernel has the ctype macros in libkern; userland has <ctype.h> */
#ifdef _KERNEL
#include <lib/libkern/libkern.h>
#else
#include_next <ctype.h>
#endif
