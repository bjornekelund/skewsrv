cc		= gcc
gcc		= ${cc} -Wall
lint		= cppcheck
libs		= -lm -lzmq

all: 		skewsrv exp exp2 exp3 testmax exptx skewday

skewsrv:	skewsrv.c Makefile
		$(gcc) -o skewsrv skewsrv.c $(libs)

skewday:	skewday.c Makefile
		$(gcc) -o skewday skewday.c $(libs)

exp:		exp.c Makefile
		$(gcc) -o exp exp.c $(libs)

testmax:	testmax.c Makefile
		$(gcc) -o testmax testmax.c $(libs)

exp2:		exp2.c Makefile
		${gcc} exp2.c -o exp2 $(libs)

exp3:		exp3.c Makefile
		${gcc} exp3.c -o exp3 $(libs)

exptx:		exptx.c Makefile
		${gcc} exptx.c -o exptx $(libs)

clean:
		rm -f skewsrv exp exp2 exp3 testmax exptx skewday
		rm -f *.o *~

lint:
			${lint} skewsrv.c skewday.c
