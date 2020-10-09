TARGETS=printsvg cardart matica xidserver xid

all: ${TARGETS}
clean:
	rm -f ${TARGETS}

OPTS=-g -IAXL -IAJL -D_GNU_SOURCE --std=gnu99 -Wall

AXL/axl.o: AXL/axl.c
	make -C AXL axl.o

AJL/ajl.o: AJL/ajl.c
	make -C AJL ajl.o

matica: matica.c AXL/axl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o -lcurl

printsvg: printsvg.c AXL/axl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o -lcurl

cardart: cardart.c 
	cc -O -o $@ $< ${OPTS} -lpopt

xid: xid.c AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AJL/ajl.o

xidserver: xidserver.c AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AJL/ajl.o
