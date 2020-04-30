PROGS=multiselect

CFLAGS=-g -Wall -Wextra
LDLIBS=-lX11

all: ${PROGS}

install: all
	mkdir -p ${DESTDIR}/usr/bin
	cp multiselect ${DESTDIR}/usr/bin
	mkdir -p ${DESTDIR}/usr/share/man/man1
	cp multiselect.1 ${DESTDIR}/usr/share/man/man1

clean:
	rm -f ${PROGS} *.o
