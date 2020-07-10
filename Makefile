cc		= gcc
gcc		= ${cc} -Wall
lint		= cppcheck

all: 		skew

skew:		skew.o Makefile
		$(gcc) -o skew skew.o -lm

skew.o:		skew.c Makefile
		$(gcc) -c skew.c

clean:
		rm -f *.o

lint:
		${lint} skew.c
