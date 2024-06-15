# for nominal debugging compilation
CFLAGS = -g -O2 -pipe -Wall
# for optimal size compilation
# CFLAGS = -s -Os -DCPU=386 -D__i386__ -m386 -malign-functions=0 -malign-jumps=0 \
# -malign-loops=0 -fomit-frame-pointer -fno-builtin -fno-strength-reduce \
# -fno-inline -pipe -Wall
STRIP = @ls -al $@;sstrip $@;ls -al $@
XLIBS = -L/usr/X11R6/lib -lX11 

all: uem ue ux

uem: uem.c Makefile
	gcc $(CFLAGS) -o uem uem.c
	$(STRIP)

ue: ue.c Makefile
	gcc $(CFLAGS) -o ue ue.c
	$(STRIP)

ux: ux.c Makefile
	gcc $(CFLAGS) -o ux ux.c $(XLIBS)
	$(STRIP)

clean:
	-rm -f *.o *.bak *~ ux ue uem
