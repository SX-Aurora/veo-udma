/*
  gcc -o hello hello.c -I/opt/nec/ve/veos/include -L/opt/nec/ve/veos/lib64 \
   -Wl,-rpath=/opt/nec/ve/veos/lib64 -lveo
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include <ve_offload.h>
#include "veo_udma.h"

/* variables for VEO demo */
int ve_node_number = 0;
struct veo_proc_handle *proc = NULL;
struct veo_thr_ctxt *ctx = NULL;
uint64_t handle = 0;

int veo_init()
{
	int rc;
	char *env;

	env = getenv("VE_NODE_NUMBER");
	if (env)
		ve_node_number = atoi(env);

#ifdef VEO_STATIC
	proc = veo_proc_create_static(ve_node_number, "./veorun_static");
#else
	proc = veo_proc_create(ve_node_number);
#endif
	if (proc == NULL) {
		perror("ERROR: veo_proc_create");
		return -1;
	}


#ifdef VEO_STATIC
#ifdef VEO_DEBUG
	printf("If you want to attach to the VE process, you now have 20s!\n\n"
	       "/opt/nec/ve/bin/gdb -p %d veorun_static\n\n", getpid());
	sleep(20);
#endif
	handle = 0;
#else
	handle = veo_load_library(proc, "./libveo_udma_ve.so");
	if (handle == 0) {
		perror("ERROR: veo_load_library");
		return -1;
	}
#endif
	
	ctx = veo_context_open(proc);
	if (ctx == NULL) {
		perror("ERROR: veo_context_open");
		return -1;
	}
	return 0;
}

int veo_finish()
{
	int close_status = veo_context_close(ctx);
	printf("close status = %d\n", close_status);
	return 0;
}


int main(int argc, char **argv)
{
	int i, rc, n, peer_id;
	uint64_t ve_buff;
	long *local_buff, *local_buff2;
	size_t bsize = 1024, res;
	struct timespec ts, te;
	uint64_t start, end;
	double bw;

	rc = veo_init();
	if (rc != 0)
		exit(1);

	/*
	  Initialize this contaxt as VEO UDMA communication peer.
	  NOTE: currently only one context per proc is supported!
	*/
	peer_id = veo_udma_peer_init(ve_node_number, proc, ctx, handle);
	if (peer_id < 0) {
		printf("veo_udma_peer_init failed with rc=%d\n", rc);
		exit(1);
	}

	local_buff = (long *)malloc(bsize * sizeof(long));
	local_buff2 = (long *)malloc(bsize * sizeof(long));
	// touch local buffer
	for (i = 0; i < bsize; i++)
		local_buff[i] = (long)i;
		
	rc = veo_alloc_mem(proc, &ve_buff, bsize * sizeof(long));
	if (rc != 0) {
		printf("veo_alloc_mem failed with rc=%d\n", rc);
		goto finish;
	}

        printf("calling veo_udma_pack\n");
        clock_gettime(CLOCK_REALTIME, &ts);
	/* packing data multiple times */
        for (i = 0; i < bsize * 4; i += 256) {
		res = veo_udma_pack(peer_id, &local_buff[i % bsize], ve_buff + (i % bsize) * sizeof(long), 256 * sizeof(long));
		printf("veo_udma_pack: returned %d\n", res);
	}
        res = veo_udma_pack_commit(peer_id);
        clock_gettime(CLOCK_REALTIME, &te);
        start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
        end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
        bw = (double)bsize * n/((double)(end - start)/1e9);
        bw = bw / 1e6;
        printf("bw=%f7.0 MB/s\n", res, bw);

        printf("calling veo_udma_recv\n");
        clock_gettime(CLOCK_REALTIME, &ts);
        res = veo_udma_recv(ctx, ve_buff, local_buff2, bsize * sizeof(long));
        clock_gettime(CLOCK_REALTIME, &te);
        start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
        end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
        bw = (double)bsize * n/((double)(end - start)/1e9);
        bw = bw / 1e6;
        printf("veo_udma_recv returned: %lu bw=%f7.0 MB/s\n", res, bw);

        rc = 0;
        // check local_buff content
        for (i = 0; i < bsize; i++) {
		if (local_buff[i] != local_buff2[i]) {
			rc = 1;
			break;
		}
	}
	if (rc)
		printf("Verify error: buffer contains wrong data\n");
	else
		printf("Received data is identical with the sent buffer.\n");

finish:
	veo_udma_peer_fini(peer_id);

	veo_finish();
	exit(0);
}

