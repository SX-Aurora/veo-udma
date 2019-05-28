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
#include "veo_udma_comm.h"

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
	void *local_buff;
	size_t bsize = 1024*1024, res;
	struct timespec ts, te;
	uint64_t start, end;
	double bw;
	int do_send = 1, do_recv = 1;
	
	if (argc == 2)
		bsize = atol(argv[1]);
	else if (argc == 3) {
		if (strcmp(argv[1], "send") == 0)
			do_recv = 0;
		if (strcmp(argv[1], "recv") == 0)
			do_send = 0;
		bsize = atol(argv[2]);
	}

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

	local_buff = malloc(bsize);
	// touch local buffer
	memset(local_buff, 65, bsize);
		
	rc = veo_alloc_mem(proc, &ve_buff, bsize);
	if (rc != 0) {
		printf("veo_alloc_mem failed with rc=%d\n", rc);
		goto finish;
	}

	if (bsize < 512 * 1024)
		n = (int)(100 * 1.e6 / (double)bsize);
	else
		n = (int)(5.0 * 1.e9 / (double)bsize);
	n > 0 ? n : 1;

	if (do_send) {
		printf("calling veo_udma_send\n");
		clock_gettime(CLOCK_REALTIME, &ts);
		for (i = 0; i < n; i++)
			res = veo_udma_send(ctx, local_buff, ve_buff, bsize);
		clock_gettime(CLOCK_REALTIME, &te);
		start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
		end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
		bw = (double)bsize * n/((double)(end - start)/1e9);
		bw = bw / 1e6;
		printf("veo_udma_send returned: %lu bw=%f7.0 MB/s\n", res, bw);
	}

	if (do_recv) {
		printf("calling veo_udma_recv\n");
		clock_gettime(CLOCK_REALTIME, &ts);
		for (i = 0; i < n; i++)
			res = veo_udma_recv(ctx, ve_buff, local_buff, bsize);
		clock_gettime(CLOCK_REALTIME, &te);
		start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
		end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
		bw = (double)bsize * n/((double)(end - start)/1e9);
		bw = bw / 1e6;
		printf("veo_udma_recv returned: %lu bw=%f7.0 MB/s\n", res, bw);
	}

finish:
	veo_udma_peer_fini(peer_id);

	veo_finish();
	exit(0);
}

