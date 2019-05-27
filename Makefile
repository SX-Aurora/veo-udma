NCC = /opt/nec/ve/bin/ncc

VEOSTATIC = -DVEO_STATIC=1

ALL: hello libvehello.so veorun_static

hello: hello.c veo_udma_comm.h
	gcc -g $(VEOSTATIC) -o hello hello.c -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
		-Wl,-rpath=/opt/nec/ve/veos/lib64 -lveo

libvehello.so: libvehello.c veo_udma_comm.h ve_inst.h
	$(NCC) -Wl,-zdefs -shared -fpic -pthread -g -o libvehello.so libvehello.c -lveio -lsysve -lm -lc -lpthread

veorun_static: libvehello.c veo_udma_comm.h ve_inst.h
	$(NCC) -g -pthread -o libvehello.o -c libvehello.c; \
	CFLAGS=-g /opt/nec/ve/libexec/mk_veorun_static veorun_static libvehello.o -lveio

clean:
	rm -f *.o *.so hello veorun_static
