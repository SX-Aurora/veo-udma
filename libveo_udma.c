#include <assert.h>
#include <errno.h>
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

/* variables for veo_udma_comm */
int udma_num_procs = 0;
int udma_num_peers = 0;
struct vh_udma_proc *udma_procs[UDMA_MAX_PROCS];
struct vh_udma_peer *udma_peers[UDMA_MAX_PEERS];


/*
  Returns: the segment ID of the shm segment.
 */
static int vh_shm_init(int key, size_t size, void **local_addr)
{
	int err = 0;
	struct shmid_ds ds;
	
	int segid = shmget(key, size, IPC_CREAT | SHM_HUGETLB | S_IRWXU); 
	if (segid == -1) {
		eprintf("[vh_shm_init] shmget failed: %s\n", strerror(errno));
		return -errno;
	}
	*local_addr = shmat(segid, NULL, 0);
	dprintf("local_addr: %p\n", *local_addr);
	if (*local_addr == (void *) -1) {
		eprintf("[vh_shm_init] shmat failed: %s\n"
			"Releasing shm segment. key=%d\n", strerror(errno), key);
		shmctl(segid, IPC_RMID, NULL);
		segid = -errno;
	}
	return segid;
}

static int vh_shm_fini(int segid, void *local_addr)
{
	int err = 0;

        dprintf("vh_shm_fini segid=%d\n", segid);
	if (local_addr != (void *)-1) {
		err = shmdt(local_addr);
		if (err < 0) {
			eprintf("[vh_shm_fini] Failed to detach from SHM segment %d at %p\n",
				segid, local_addr);
			return err;
		}
	}
	if (segid != -1) {
		err = shmctl(segid, IPC_RMID, NULL);
		if (err < 0)
			eprintf("[vh_shm_fini] Failed to remove SHM segment ID %d\n", segid);
	}
        dprintf("vh_shm_fini shmctl RMID returned %d\n", err);
	return err;
}

static void vh_udma_proc_setup(struct vh_udma_proc *pp, int ve_node_id,
			       struct veo_proc_handle *proc, uint64_t lib_handle)
{
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
	
static int ve_udma_setup(struct vh_udma_peer *up)
{
	uint64_t req;
	int64_t res;
	int err;

	// call VE side init function
	struct veo_args *argp = veo_args_alloc();
	veo_args_set_stack(argp, VEO_INTENT_IN, 0, (char *)up, sizeof(struct vh_udma_peer));
	req = veo_call_async(up->ctx, udma_procs[up->proc_id]->ve_udma_init, argp);
	err = veo_call_wait_result(up->ctx, req, (uint64_t *)&res);
	if (err)
		eprintf("veo-udma setup on VE has failed.\n"
			"ve_udma_setup veo_call_wait_result err=%d\n", err);
	veo_args_free(argp);
	return err != 0 ? err : (int)res;
}

static int ve_udma_close(struct vh_udma_peer *up)
{
	uint64_t req;
	int64_t res;
	int err;

	// call VE side init function
	struct veo_args *argp = veo_args_alloc();
	req = veo_call_async(up->ctx, udma_procs[up->proc_id]->ve_udma_fini, argp);
	err = veo_call_wait_result(up->ctx, req, (uint64_t *)&res);
	if (err)
		eprintf("veo-udma finish failed on VE side.\n"
			"veo_call_wait_result err=%d\n", err);
	veo_args_free(argp);
	return err != 0 ? err : (int)res;
}

/*
  VH side UDMA communication init.
  NOTE: Only one thread context is supported per proc due to a limitation
  in the linking of the static veorun.
  
  Returns: peer_id (can be 0) or a negative number, in case of an error.
*/
int veo_udma_peer_init(int ve_node_id, struct veo_proc_handle *proc,
		       struct veo_thr_ctxt *ctx, uint64_t lib_handle)
{
	int rc, i, peer_id;
	char *mb_offs = NULL;
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
		if (!pp) {
			eprintf("veo_udma_peer_init malloc failed.\n");
			return -ENOMEM;
		}
		vh_udma_proc_setup(pp, ve_node_id, proc, lib_handle);
		udma_procs[proc_id] = pp;
	}
	udma_procs[proc_id]->count++;

	up = (struct vh_udma_peer *)malloc(sizeof(struct vh_udma_peer));
	if (!up) {
		eprintf("veo_udma_peer_init malloc peer struct failed.\n");
		return -ENOMEM;
	}
	peer_id = udma_num_peers++;
	udma_peers[peer_id] = up;
	up->proc_id = proc_id;
	up->ctx = ctx;
	up->shm_key = getpid() * UDMA_MAX_PEERS + udma_num_peers;
	up->shm_size = 2 * UDMA_BUFF_LEN;
	up->shm_segid = vh_shm_init(up->shm_key, up->shm_size, &up->shm_addr);
	if (up->shm_segid == -1) {
		rc = vh_shm_fini(up->shm_segid, up->shm_addr);
		return rc ? rc : -ENOMEM;
	}

	up->send.shm = up->shm_addr;
	mb_offs = (char *)up->send.shm + UDMA_BUFF_LEN \
		- (UDMA_MAX_SPLIT * sizeof(size_t) + 2 * sizeof(uint64_t));
	up->send.buff_len = mb_offs - (char *)up->send.shm;
	up->send.len = (size_t *)mb_offs;

	up->recv.shm = (void *)((char *)up->shm_addr + UDMA_BUFF_LEN);
	mb_offs = (char *)up->recv.shm + UDMA_BUFF_LEN \
		- (UDMA_MAX_SPLIT * sizeof(size_t) + 2 * sizeof(uint64_t));
	up->recv.buff_len = mb_offs - (char *)up->recv.shm;
	up->recv.len = (size_t *)mb_offs;
        pthread_mutex_init(&up->lock, NULL);
	
	rc = ve_udma_setup(up);
	return rc ? rc : peer_id;
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
  Tune splitting of buffers and overlap of transfers and memcopy.
  The values in the tables are measured.
*/
struct tune_split {
	size_t transfer;
	size_t size;
	int split;
};
static int num_tune_send = 5;
static struct tune_split tune_send[] = \
{
	{  524288,  524288,  1},
	{ 1048576,  524288,  2},
	{16777216, 1048576,  4},
	{33554432, 2097152,  4},
	{67108864, 4194304,  8}
	
};
static int num_tune_recv = 6;
static struct tune_split tune_recv[] = \
{
	{   4194304,  262144,  8},
	{   8388608,  262144, 16},
	{  16777216, 1048576,  4},
	{  33554432, 1048576,  2},
	{  67108864,   65536, 32},
	{ 134217728,   65536, 16}
};

static int calc_split_send(size_t len, size_t *split_size)
{
	int i, itune = 0, split = 0;
	size_t size = 0;
	char *env = getenv("UDMA_SPLIT_SEND");
	if (env)
		split = atoi(env);
	env = getenv("UDMA_SPLIT_SIZE_SEND");
	if (env)
		size = atol(env);
	if (split && split_size)
		if (split * size > (UDMA_BUFF_LEN - UDMA_MAX_SPLIT * 8)) {
			eprintf("ERROR: split * split_size > %u\n",
				(UDMA_BUFF_LEN - UDMA_MAX_SPLIT * 8));
		} else {
			*split_size = size;
			return split;
		}

	for (i = 1; i < num_tune_send; i++) {
		if (len >= tune_send[i].transfer)
			itune = i;
		else
			break;
	}
	*split_size = tune_send[itune].size;
	return tune_send[itune].split;
}

static int calc_split_recv(size_t len, size_t *split_size)
{
	int i, itune = 0, split = 0;
	size_t size = 0;
	char *env = getenv("UDMA_SPLIT_RECV");
	if (env)
		split = atoi(env);
	env = getenv("UDMA_SPLIT_SIZE_RECV");
	if (env)
		size = atol(env);
	if (split && split_size)
		if (split * size > (UDMA_BUFF_LEN - UDMA_MAX_SPLIT * 8)) {
			eprintf("ERROR: split * split_size > %u\n",
				(UDMA_BUFF_LEN - UDMA_MAX_SPLIT * 8));
		} else {
			*split_size = size;
			return split;
		}
	
	for (i = 1; i < num_tune_recv; i++) {
		if (len >= tune_recv[i].transfer)
			itune = i;
		else
			break;
	}
	*split_size = tune_recv[itune].size;
	return tune_recv[itune].split;
}

#define SPLITBUFF(base, idx, size) (void *)((char *)base + idx * size)

/*
  Sent buffer from VH to VE
*/
size_t veo_udma_send(struct veo_thr_ctxt *ctx, void *src, uint64_t dst, size_t len)
{
	size_t tlen, lenp = len, split_size;
	uint64_t req, retval = 0, dstp = dst;
	int i, rc, split, err = 0;
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
	if (pthread_mutex_trylock(&up->lock) != 0) {
		eprintf("veo_udma_send found mutex locked!?\n");
		return 0;
	}
	split = calc_split_send(len, &split_size);

	struct veo_args *argp = veo_args_alloc();
	veo_args_set_u64(argp, 0, (uint64_t)dst);
	veo_args_set_u64(argp, 1, (uint64_t)len);
	veo_args_set_i32(argp, 2, split);
	veo_args_set_u64(argp, 3, (uint64_t)split_size);
	req = veo_call_async(ctx, udma_procs[up->proc_id]->ve_udma_recv, argp);
	i = 0;
	while (lenp > 0) {
		tlen = MIN(split_size, lenp);
		mp = memcpy(SPLITBUFF(up->send.shm, i, split_size), (void *)srcp, tlen);
		*(up->send.len + i) = tlen;
		dstp += tlen;
		srcp += tlen;
		lenp -= tlen;
		i = (i + 1) % split;
		// poll until len field is set to 0
		while (*(volatile size_t *)(up->send.len + i) > 0) {
			// peek at request, did it bail out?
			rc = veo_call_peek_result(ctx, req, &retval);
			if (rc != VEO_COMMAND_UNFINISHED &&	\
			    (size_t)retval != len) {
				err = 1;
				break;
			}
		}
		if (err)
			break;
	}
	if (!err) {
		rc = veo_call_wait_result(ctx, req, &retval);
	}
	veo_args_free(argp);
	pthread_mutex_unlock(&up->lock);
	return (size_t)retval;
}

/*
  Recv buffer from VE to VH
*/
size_t veo_udma_recv(struct veo_thr_ctxt *ctx, uint64_t src, void *dst, size_t len)
{
	size_t tlen, lenp = len, split_size;
	uint64_t req, retval = 0;
	int i, j, rc, split, err = 0;
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
	if (pthread_mutex_trylock(&up->lock) != 0) {
		eprintf("veo_udma_recv found mutex locked!?\n");
		return 0;
	}

	split = calc_split_recv(len, &split_size);

	struct veo_args *argp = veo_args_alloc();
	veo_args_set_u64(argp, 0, (uint64_t)src);
	veo_args_set_u64(argp, 1, (uint64_t)len);
	veo_args_set_i32(argp, 2, split);
	veo_args_set_u64(argp, 3, (uint64_t)split_size);
	req = veo_call_async(ctx, udma_procs[up->proc_id]->ve_udma_send, argp);
	j = 0;
	while (lenp > 0) {
		while ((tlen = *(up->recv.len +j)) == 0) {
			rc = veo_call_peek_result(ctx, req, &retval);
			if (rc != VEO_COMMAND_UNFINISHED &&	\
			    (size_t)retval != len) {
				err = 1;
				break;
			}
		}
		if (err)
			break;
		memcpy((void *)dstp, SPLITBUFF(up->recv.shm, j, split_size), tlen);
		*(volatile size_t *)(up->recv.len + j) = 0;
		dstp += tlen;
		lenp -= tlen;
		j = (j + 1) % split;
	}
	if (!err) {
		rc = veo_call_wait_result(ctx, req, &retval);
	}
	veo_args_free(argp);
	pthread_mutex_unlock(&up->lock);
	return (size_t)retval;
}
