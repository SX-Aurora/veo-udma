NCC = /opt/nec/ve/bin/ncc
GCC = gcc
#DEBUG = -g

VEOTYPE ?= VEO

ifeq ($(VEOTYPE),"VEO")
VEOINC = /opt/nec/ve/veos/include
VEOLIB = -Wl,-rpath,/opt/nec/ve/veos/lib64 -L/opt/nec/ve/veos/lib64 -lveo
else
VEOINC = $(HOME)/aveo/install/include
VEOLIB = -Wl,-rpath,$(HOME)/aveo/install/lib -L$(HOME)/aveo/install/lib -laveoVH
endif


#VEOSTATIC = -DVEO_STATIC=1

TARGETS = libveo_udma.so hello latency bandwidth bandwidth_veo test_pack

ifdef VEOSTATIC
ALL:  $(TARGETS) veorun_static
else
ALL: $(TARGETS) libveo_udma_ve.so
endif

libveo_udma.o: libveo_udma.c veo_udma.h
	$(GCC) $(DEBUG) -fpic -pthread -o $@ -c $< -I$(VEOINC)

libveo_udma.so: libveo_udma.o
	$(GCC) $(DEBUG) -shared -fpic -o $@ $<

libveo_udma_ve.o: libveo_udma_ve.c veo_udma.h
	$(NCC) $(DEBUG) -O2 -fpic -pthread -o $@ -c $<

libveo_udma_ve.so: libveo_udma_ve.o
	$(NCC) -Wl,-zdefs -shared -fpic -pthread $(DEBUG) -o $@ $< -lveio -lsysve -lm -lc -lpthread

hello: hello.c veo_udma.h libveo_udma.so
	gcc $(DEBUG) $(VEOSTATIC) -o $@ $< -I$(VEOINC) -L$(VEOLIB) \
		-L. -Wl,-rpath=$(shell pwd) $(VEOLIB) -lveo_udma

bandwidth: bandwidth.c veo_udma.h libveo_udma.so
	gcc $(DEBUG) $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=$(shell pwd) $(VEOLIB) -lveo_udma

bandwidth_veo: bandwidth_veo.c veo_udma.h libveo_udma.so
	gcc $(DEBUG) $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=$(shell pwd) $(VEOLIB) -lveo_udma

latency: latency.c veo_udma.h libveo_udma.so
	gcc $(DEBUG) $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=$(shell pwd) $(VEOLIB) -lveo_udma

test_pack: test_pack.c veo_udma.h libveo_udma.so
	gcc $(DEBUG) $(VEOSTATIC) -o $@ $< -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-L. -Wl,-rpath=$(shell pwd) $(VEOLIB) -lveo_udma

veorun_static: libveo_udma_ve.o
	CFLAGS=$(DEBUG) /opt/nec/ve/libexec/mk_veorun_static $@ $^ -lveio

clean:
	rm -f *.o *.so hello veorun_static
