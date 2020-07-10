cc		= gcc
gcc		= ${cc} -Wall
lint		= cppcheck

all: 		skew exp

skew:		skew.o Makefile
		$(gcc) -o skew skew.o -lm

skew.o:		skew.c Makefile
		$(gcc) -c skew.c

exp:		exp.o Makefile
		$(gcc) -o exp exp.o -lm -lzmq

exp.o:		exp.c Makefile
		$(gcc) -c exp.c

hwserver3:	hwserver3.c Makefile
		gcc -c hwserver3.c -o hwserver3 -lzmq

clean:
		rm -f *.o

lint:
		${lint} skew.c
