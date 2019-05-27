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

/* variables for veo_udma_comm */
int udma_num_procs = 0;
int udma_num_peers = 0;
struct vh_udma_proc *udma_procs[UDMA_MAX_PROCS];
struct vh_udma_peer *udma_peers[UDMA_MAX_PEERS];

/* variables for VEO demo */
int ve_node_number = 0;
struct veo_proc_handle *proc = NULL;
struct veo_thr_ctxt *ctx = NULL;
uint64_t handle = 0;

/*
  Returns: the segment ID of the shm segment.
 */
int vh_shm_init(int key, size_t size, void **local_addr)
{
	int err = 0;
	struct shmid_ds ds;
	
	int segid = shmget(key, size, IPC_CREAT | SHM_HUGETLB | S_IRWXU); 
	if (segid == -1)
		return -1;
	*local_addr = shmat(segid, NULL, 0);
	dprintf("local_addr: %p\n", *local_addr);
	if (*local_addr == (void *) -1) {
		eprintf("shmat failed!? releasing shm segment. key=%d\n", key);
		shmctl(segid, IPC_RMID, NULL);
		segid = -1;
	}
	return segid;
}

int vh_shm_fini(int segid, void *local_addr)
{
	int err = 0;

	if (local_addr != (void *)-1) {
		err = shmdt(local_addr);
		if (err < 0) {
			eprintf("Failed to detach from SHM segment at %p\n", local_addr);
			return err;
		}
	}
	if (segid != -1) {
		err = shmctl(segid, IPC_RMID, NULL);
		if (err < 0)
			eprintf("Failed to remove SHM segment ID %d\n", segid);
	}
	return err;
}

void vh_udma_proc_setup(struct vh_udma_proc *pp, int ve_node_id,
			struct veo_proc_handle *proc, uint64_t lib_handle)
{
	int err;

	pp->ve_node_id = ve_node_id;
	pp->count = 0;
	pp->proc = proc;
	pp->lib_handle = lib_handle;
	// find function addresses
	pp->ve_udma_init = veo_get_sym(pp->proc, pp->lib_handle, "ve_udma_init");
	pp->ve_udma_fini = veo_get_sym(pp->proc, pp->lib_handle, "ve_udma_fini");
	pp->ve_udma_send = veo_get_sym(pp->proc, pp->lib_handle, "ve_udma_send");
	pp->ve_udma_recv = veo_get_sym(pp->proc, pp->lib_handle, "ve_udma_recv");
}
	

int ve_udma_setup(struct vh_udma_peer *up)
{
	uint64_t req;
	int64_t res;
	int err;

	// call VE side init function
	struct veo_args *argp = veo_args_alloc();
	veo_args_set_stack(argp, VEO_INTENT_IN, 0, (char *)up, sizeof(struct vh_udma_peer));
	req = veo_call_async(ctx, udma_procs[up->proc_id]->ve_udma_init, argp);
	err = veo_call_wait_result(ctx, req, (uint64_t *)&res);
	if (err)
		eprintf("veo_call_wait_result err=%d\n", err);
	veo_args_free(argp);
	return err != 0 ? err : (int)res;
}

int ve_udma_close(struct vh_udma_peer *up)
{
	uint64_t req;
	int64_t res;
	int err;

	// call VE side init function
	struct veo_args *argp = veo_args_alloc();
	req = veo_call_async(up->ctx, udma_procs[up->proc_id]->ve_udma_fini, argp);
	err = veo_call_wait_result(ctx, req, (uint64_t *)&res);
	if (err)
		eprintf("veo_call_wait_result err=%d\n", err);
	veo_args_free(argp);
	return err != 0 ? err : (int)res;
}

/*
  VH side UDMA communication init.
  - each context should be a peer!
  - VH has no peer ID. 
*/
int veo_udma_peer_init(int ve_node_id, struct veo_proc_handle *proc,
		       struct veo_thr_ctxt *ctx, uint64_t lib_handle)
{
	int rc, i, peer_id;
	int num_buffs = 1 + 2 * UDMA_NUM_BUFFS;
	char *mb_offs = NULL;
	char *buff_offs = NULL;
	struct vh_udma_peer *up;
	int proc_id = -1;

	/* do you have a peer for this proc already? */
	for (i = 0; i < udma_num_procs; i++) {
		if (udma_procs[i]->proc == proc) {
			proc_id = i;
			break;
		}
	}
	if (proc_id < 0) {
		proc_id = udma_num_procs++;
		struct vh_udma_proc *pp = \
			(struct vh_udma_proc *)malloc(sizeof(struct vh_udma_proc));
		vh_udma_proc_setup(pp, ve_node_id, proc, lib_handle);
		udma_procs[proc_id] = pp;
	}
	udma_procs[proc_id]->count++;

	up = (struct vh_udma_peer *)malloc(sizeof(struct vh_udma_peer));
	peer_id = udma_num_peers++;
	udma_peers[peer_id] = up;
	up->proc_id = proc_id;
	up->ctx = ctx;
	up->shm_key = getpid() * UDMA_MAX_PEERS + udma_num_peers;
	up->shm_size = num_buffs * UDMA_BUFF_LEN;
	up->shm_segid = vh_shm_init(up->shm_key, up->shm_size, &up->shm_addr);
	if (up->shm_segid == -1)
		return vh_shm_fini(up->shm_segid, up->shm_addr);

	mb_offs = (char *)up->shm_addr;
	buff_offs = (char *)up->shm_addr + UDMA_BUFF_LEN;
	// set mailbox addresses
	for (i = 0; i < UDMA_NUM_BUFFS; i++) {
		up->send[i].len = (size_t *)mb_offs;
		*(up->send[i].len) = 0;
		mb_offs += sizeof(size_t *);
		up->recv[i].len = (size_t *)mb_offs;
		*(up->recv[i].len) = 0;
		mb_offs += sizeof(size_t *);
		up->send[i].shm = (void *)buff_offs;
		buff_offs += UDMA_BUFF_LEN;
		up->recv[i].shm = (void *)buff_offs;
		buff_offs += UDMA_BUFF_LEN;
	}
	rc = ve_udma_setup(up);
	return rc;
}

int veo_udma_peer_fini(int peer_id)
{
	int rc;

	struct vh_udma_peer *up = udma_peers[peer_id];
	rc = ve_udma_close(up);
	if (rc) {
		eprintf("ve_udma_close failed for peer %d, rc=%d\n", peer_id, rc);
		return rc;
	}
	rc = vh_shm_fini(up->shm_segid, up->shm_addr);
	if (rc) {
		eprintf("vh_shm_fini failed for peer %d, rc=%d\n", peer_id, rc);
		return rc;
	}
	udma_procs[up->proc_id]->count--;
	if (udma_procs[up->proc_id]->count == 0) {
		free(udma_procs[up->proc_id]);
		udma_procs[up->proc_id] = NULL;
	}
	free(udma_peers[peer_id]);
	udma_peers[peer_id] = NULL;
	return 0;
}

/*
  Sent buffer from VH to VE
*/
size_t veo_udma_send(struct veo_thr_ctxt *ctx, void *src, uint64_t dst, size_t len)
{
	size_t tlen, lenp = len;
	uint64_t req, retval = 0, dstp = dst;
	int i, rc, err = 0;
	char *srcp = (char *)src;
	void *mp;
	struct vh_udma_peer *up = NULL;

	for (i = 0; i < udma_num_peers; i++) {
		if (udma_peers[i]->ctx == ctx) {
			up = udma_peers[i];
			break;
		}
	}
	if (!up) {
		eprintf("veo_udma_recv ctx not found!\n");
		return 0;
	}

	struct veo_args *argp = veo_args_alloc();
	veo_args_set_u64(argp, 0, (uint64_t)dst);
	veo_args_set_u64(argp, 1, (uint64_t)len);
	req = veo_call_async(ctx, udma_procs[up->proc_id]->ve_udma_recv, argp);
	i = 0;
	while (lenp > 0) {
		tlen = MIN(UDMA_BUFF_LEN, lenp);
		mp = memcpy(up->send[i].shm, (void *)srcp, tlen);
		*(up->send[i].len) = tlen;
		dstp += tlen;
		srcp += tlen;
		lenp -= tlen;
		i = (i + 1) % UDMA_NUM_BUFFS;
		// poll until len field is set to 0
		int peek_delay = UDMA_DELAY_PEEK;
		while (*(up->send[i].len) > 0) {
			if (peek_delay == 0) {
				// peek at request, did it bail out?
				rc = veo_call_peek_result(ctx, req, &retval);
				if (rc != VEO_COMMAND_UNFINISHED &&	\
				    (size_t)retval != len) {
					err = 1;
					break;
				}
				peek_delay = UDMA_DELAY_PEEK;
			}
			--peek_delay;
		}
		if (err)
			break;
	}
	if (!err) {
		rc = veo_call_wait_result(ctx, req, &retval);
	}
	veo_args_free(argp);
	return (size_t)retval;
}

/*
  Recv buffer from VE to VH
*/
size_t veo_udma_recv(struct veo_thr_ctxt *ctx, uint64_t src, void *dst, size_t len)
{
	size_t tlen, lenp = len;
	uint64_t req, retval = 0;
	int i, j, rc, err = 0;
	char *dstp = (char *)dst;
	void *mp;
	struct vh_udma_peer *up = NULL;

	for (i = 0; i < udma_num_peers; i++)
		if (udma_peers[i]->ctx == ctx) {
			up = udma_peers[i];
			break;
		}
	if (!up) {
		printf("veo_udma_recv ctx not found!\n");
		return 0;
	}

	struct veo_args *argp = veo_args_alloc();
	veo_args_set_u64(argp, 0, (uint64_t)src);
	veo_args_set_u64(argp, 1, (uint64_t)len);
	req = veo_call_async(ctx, udma_procs[up->proc_id]->ve_udma_send, argp);
	j = 0;
	while (lenp > 0) {
		int peek_delay = UDMA_DELAY_PEEK;
		while ((tlen = *(up->recv[j].len)) == 0) {
			if (peek_delay == 0) {
				rc = veo_call_peek_result(ctx, req, &retval);
				if (rc != VEO_COMMAND_UNFINISHED &&	\
				    (size_t)retval != len) {
					err = 1;
					break;
				}
				peek_delay = UDMA_DELAY_PEEK;
			}
			--peek_delay;
		}
		if (err)
			break;
		memcpy((void *)dstp, up->recv[j].shm, tlen);
		*(up->recv[j].len) = 0;
		dstp += tlen;
		lenp -= tlen;
		j = (j + 1) % UDMA_NUM_BUFFS;
	}
	if (!err) {
		rc = veo_call_wait_result(ctx, req, &retval);
	}
	veo_args_free(argp);
	return (size_t)retval;
}

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
	handle = veo_load_library(proc, "./libvehello.so");
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
	int i, rc, n;
	uint64_t ve_buff;
	void *local_buff;
	size_t bsize = 1024*1024, res;
	struct timespec ts, te;
	uint64_t start, end;
	double bw;

	if (argc > 1)
		bsize = atol(argv[1]);

	rc = veo_init();
	if (rc != 0)
		exit(1);
	rc = veo_udma_peer_init(ve_node_number, proc, ctx, handle);
	if (rc != 0) {
		printf("veo_udma_init failed with rc=%d\n", rc);
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
	printf("calling veo_udma_send\n");
	n = (int)(5.0 * 1.e9 / (double)bsize);
        n > 0 ? n : 1;
	clock_gettime(CLOCK_REALTIME, &ts);
	for (i = 0; i < n; i++)
		res = veo_udma_send(ctx, local_buff, ve_buff, bsize);
	clock_gettime(CLOCK_REALTIME, &te);
	start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
	end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
	bw = (double)bsize * n/((double)(end - start)/1e9);
	bw = bw / 1e6;
	printf("veo_udma_send returned: %lu bw=%f MB/s\n", res, bw);

	printf("calling veo_udma_recv\n");
	n = (int)(5.0 * 1.e9 / (double)bsize);
        n > 0 ? n : 1;
	clock_gettime(CLOCK_REALTIME, &ts);
	for (i = 0; i < n; i++)
		res = veo_udma_recv(ctx, ve_buff, local_buff, bsize);
	clock_gettime(CLOCK_REALTIME, &te);
	start = ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
	end = te.tv_sec * 1000 * 1000 * 1000 + te.tv_nsec;
	bw = (double)bsize * n/((double)(end - start)/1e9);
	bw = bw / 1e6;
	printf("veo_udma_recv returned: %lu bw=%f MB/s\n", res, bw);

finish:
	for (i = 0; i < udma_num_peers; i++)
		rc = veo_udma_peer_fini(i);

	veo_finish();
	exit(0);
}

