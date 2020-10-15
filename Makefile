TARGETS=xidsvg printsvg cardart matica xidserver xidsvg.o

all: ${TARGETS}
clean:
	rm -f ${TARGETS}

OPTS=-fPIC -g -IAXL -IAJL -D_GNU_SOURCE --std=gnu99 -Wall

AXL/axl.c:
	git submodule init AXL
	git submodule update --remote AXL

AXL/axl.o: AXL/axl.c AXL
	make -C AXL axl.o

AJL/ajl.c:
	git submodule init AJL
	git submodule update --remote AJL

AJL/ajl.o: AJL/ajl.c AJL
	make -C AJL ajl.o

matica: matica.c AXL/axl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o -lcurl

printsvg: printsvg.c AXL/axl.o AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o AJL/ajl.o -lcurl

xidsvg: xidsvg.c AXL/axl.o AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o AJL/ajl.o -lcurl -pthread -lssl

xidsvg.o: xidsvg.c AXL/axl.h AJL/ajl.h
	cc -O -c -o $@ $< ${OPTS} -DLIB

cardart: cardart.c 
	cc -O -o $@ $< ${OPTS} -lpopt

xidserver: xidserver.c AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -I/usr/include/PCSC -lpopt AJL/ajl.o -pthread -lssl -lpng -lm -lusb-1.0 -lpcsclite -lpthread

