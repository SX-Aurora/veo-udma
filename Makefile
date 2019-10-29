NCC = /opt/nec/ve/bin/ncc
GCC = gcc

VEOSTATIC = -DVEO_STATIC=1

ifdef VEOSTATIC
ALL: libveo_udma.so hello test_pack veorun_static
else
ALL: libveo_udma.so hello test_pack libveo_udma_ve.so
endif

libveo_udma.o: libveo_udma.c veo_udma.h
	$(GCC) -g -fpic -pthread -o $@ -c $< -I/opt/nec/ve/veos/include

libveo_udma.so: libveo_udma.o
	$(GCC) -g -shared -fpic -o $@ $<

libveo_udma_ve.o: libveo_udma_ve.c veo_udma.h
	$(NCC) -g -O2 -fpic -pthread -o $@ -c $<

libveo_udma_ve.so: libveo_udma_ve.o
	$(NCC) -Wl,-zdefs -shared -fpic -pthread -g -o $@ $< -lveio -lsysve -lm -lc -lpthread

hello: hello.c veo_udma.h libveo_udma.so
	gcc -g $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=/opt/nec/ve/veos/lib64 -Wl,-rpath=$(shell pwd) \
		-lveo -lveo_udma
latency: latency.c veo_udma.h libveo_udma.so
	gcc -g $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=/opt/nec/ve/veos/lib64 -Wl,-rpath=$(shell pwd) \
		-lveo -lveo_udma

test_pack: test_pack.c veo_udma.h libveo_udma.so
	gcc -g $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=/opt/nec/ve/veos/lib64 -Wl,-rpath=$(shell pwd) \
		-lveo -lveo_udma

veorun_static: libveo_udma_ve.o
	CFLAGS=-g /opt/nec/ve/libexec/mk_veorun_static $@ $^ -lveio

clean:
	rm -f *.o *.so hello veorun_static
