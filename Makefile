TARGETS=printsvg cardart matica xid8600 xid

all: ${TARGETS}
clean:
	rm -f ${TARGETS}

OPTS=-g -IAXL -IAJL -D_GNU_SOURCE --std=gnu99

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

xid8600: xid8600.c AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AJL/ajl.o
