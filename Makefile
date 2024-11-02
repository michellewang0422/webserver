CC=gcc
CFLAGS=-g -O2 -Wall
LDLIBS=-lpthread

all: echoserver webserver

echoserver: echoserver.c

webserver: webserver.c


clean:
	rm -rf *.o *~ *.dSYM echoserver webserver
