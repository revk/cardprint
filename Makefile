TARGETS=xidsvg xidserver xidsvg.o

all: ${TARGETS}
clean:
	rm -f ${TARGETS}
pull:
	git pull
	git submodule update --recursive

OPTS=-fPIC -g -IAXL -IAJL -D_GNU_SOURCE --std=gnu99 -Wall

AXL/axl.c:
	git submodule update --init AXL

AXL/axl.o: AXL/axl.c AXL
	make -C AXL axl.o

AJL/ajl.c:
	git submodule update --init AJL

AJL/ajl.o: AJL/ajl.c AJL
	make -C AJL ajl.o

xidsvg: xidsvg.c AXL/axl.o AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -lpopt AXL/axl.o AJL/ajl.o -lcurl -pthread -lssl

xidsvg.o: xidsvg.c AXL/axl.h AJL/ajl.h
	cc -O -c -o $@ $< ${OPTS} -DLIB

xidserver: xidserver.c AJL/ajl.o
	cc -O -o $@ $< ${OPTS} -I/usr/include/PCSC -lpopt AJL/ajl.o -pthread -lssl -lpng -lm -lusb-1.0 -lpcsclite
