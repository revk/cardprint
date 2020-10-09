all: printsvg cardart

OPTS=-g -IAXL -IAJL -D_GNU_SOURCE --std=gnu99

AXL/axl.o: AXL/axl.c
	make -C AXL axl.o

printsvg: printsvg.c AXL/axl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o -lcurl

cardart: cardart.c 
	cc -O -o $@ $< ${OPTS} -lpopt
