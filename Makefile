cc		= gcc
gcc		= ${cc} -Wall
lint		= cppcheck

all: 		skew exp exp2

skew:		skew.o Makefile
			$(gcc) -o skew skew.o -lm

skew.o:		skew.c Makefile
			$(gcc) -c skew.c

exp:		exp.c Makefile
			$(gcc) -o exp exp.c -lm -lzmq
			
exp2:		exp2.c Makefile
			${gcc} exp2.c -o exp2 -lm -lzmq
clean:
		rm -f *.o

lint:
		${lint} skew.c
