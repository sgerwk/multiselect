PROGS=multiselect

CFLAGS=-g -Wall -Wextra
LDLIBS=-lX11

all: ${PROGS}

clean:
	rm -f ${PROGS} *.o
