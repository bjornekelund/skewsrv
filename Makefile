cc			= gcc
gcc			= ${cc} -Wall
lint		= cppcheck
libs		= -lm -lzmq

all: 		skew exp exp2 testmax exptx

skew:		skew.c Makefile
			$(gcc) -o skew skew.c $(libs)

exp:		exp.c Makefile
			$(gcc) -o exp exp.c $(libs)

testmax:	testmax.c Makefile
			$(gcc) -o testmax testmax.c $(libs)

exp2:		exp2.c Makefile
			${gcc} exp2.c -o exp2 $(libs)

exptx:		exptx.c Makefile
			${gcc} exptx.c -o exptx $(libs)

clean:
			rm -f *.o *~

lint:
			${lint} skew.c
