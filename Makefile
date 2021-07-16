all: small

small: xcc-small.c
	gcc xcc-small.c -o sxcc

small2: xcc-small2.c
	gcc xcc-small2.c -o sxcc

lex: xcc-lex.c
	gcc xcc-lex.c -o lxcc

clean:
	rm -rf sxcc lxcc
