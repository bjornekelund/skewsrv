cc			= gcc
gcc			= ${cc} -Wall
lint		= cppcheck

all: 		skew exp exp2

skew:		skew.c Makefile
			$(gcc) -o skew skew.c -lm

exp:		exp.c Makefile
			$(gcc) -o exp exp.c -lm -lzmq

exp2:		exp2.c Makefile
			${gcc} exp2.c -o exp2 -lm -lzmq
clean:
			rm -f *.o *~

lint:
			${lint} skew.c
