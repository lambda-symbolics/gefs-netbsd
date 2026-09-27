# sagefs: the libsa gefs reader in userland (Linux or NetBSD)
OS!=	uname -s
OS?=	$(shell uname -s)
EXTRA=	$(if $(filter Linux,$(OS)),-Ilinux,)
sagefs: sagefs.c ../../sys/lib/libsa/gefs.c stand.h ls.h
	$(CC) -O1 -g -Wall -Wno-unused -I. $(EXTRA) -o $@ sagefs.c ../../sys/lib/libsa/gefs.c
clean:
	rm -f sagefs
