#ifndef VEO_UDMA_COMM_INCLUDE
#define VEO_UDMA_COMM_INCLUDE

#include <errno.h>
#include <pthread.h>

#define UDMA_MAX_PROCS 8
#define UDMA_MAX_PEERS 64
#define UDMA_MAX_SPLIT 64
#define UDMA_BUFF_LEN (64 * 1024 * 1024)
#define UDMA_PACK_MAX (UDMA_BUFF_LEN / 2)

#define UDMA_DELAY_PEEK 1
#define UDMA_TIMEOUT_US (10 * 1000000)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define ALIGN8B(x) (((uint64_t)(x) + 7UL) & ~7UL)

#define DEBUG 1
#ifdef DEBUG
#define dprintf(args...) printf(args)
#endif
#define eprintf(args...) fprintf(stderr, args)

#ifdef __cplusplus
extern "C" {
#endif
struct vh_udma_proc {
	int ve_node_id;
	int count;		// how often used/ referenced
	struct veo_proc_handle *proc;
	uint64_t lib_handle;
	uint64_t ve_udma_init;	// address of function on VE
	uint64_t ve_udma_fini;	// address of function on VE
	uint64_t ve_udma_send;	// address of function on VE
	uint64_t ve_udma_recv;	// address of function on VE
};
	
struct vh_udma_comm {
	volatile size_t *len;	// address of length mailbox
	size_t buff_len;	// total buffer space length
	void *shm;		// buffer inside the shared memory segment
};

struct udma_pack {
	void *buff;		// pack buffer
	size_t len;		// filled buffer space length
	size_t buff_len;	// max buffer space length
};

struct vh_udma_peer {
	struct vh_udma_comm send;
	struct vh_udma_comm recv;
	struct udma_pack pack;
	struct veo_thr_ctxt *ctx;
	int proc_id;
	int shm_key, shm_segid;
	size_t shm_size;
	void *shm_addr;
	pthread_mutex_t lock;
};

struct ve_udma_comm {
	uint64_t len_vehva;	// start address of length mailbox (UDMA_MAX_SPLIT words)
	size_t buff_len;	// total buffer space length
	uint64_t shm_vehva;	// start of buffer space in shm segment vehva
	uint64_t buff_vehva;	// address of mirror buffer in 
	void *buff;
};
struct ve_udma_peer {
	struct ve_udma_comm send;
	struct ve_udma_comm recv;
	pthread_mutex_t lock;
};

/*
  Put len bytes from src onto the sending pack buffer,
  preceeded by destination address and length. Round up length to 8 byte boundary.

  Returns 0 if successful, negative number -ENOMEM if buffer didn't fit.
*/
static inline int _buffer_pack(struct udma_pack *pb, void *src, uint64_t dst, size_t len)
{
	uint64_t *b = (uint64_t *)((uint64_t)pb->buff + pb->len);

	if (pb->len + 2 * sizeof(uint64_t) + ALIGN8B(len) > pb->buff_len)
		return -ENOMEM;
	*b = dst;
	b++;
	*b = len;
	b++;
	memcpy((void *)b, src, len);
	pb->len += 2 * sizeof(uint64_t) + ALIGN8B(len);
	return 0;
}

static inline int _buffer_unpack(void *buff, size_t buff_len)
{
	void *dst;
	size_t len;
	uint64_t *b = (uint64_t *)buff;

	while ((char *)b < (char *)buff + buff_len) {
		dst = (void *)*b;
		b++;
		len = *b;
		b++;
		if (dst && len > 0 && ((char *)b + len <= (char *)buff + buff_len)) {
			memcpy(dst, (void *)b, len);
			b = (uint64_t *)ALIGN8B((uint64_t)b + len);
		} else {
			eprintf("buffer unpack failed: dst=%p len=%lu\n", dst, len);
			return 1;
		}
	}
	return 0;
}


int veo_udma_peer_init(int ve_node_id, struct veo_proc_handle *proc,
		       struct veo_thr_ctxt *ctx, uint64_t lib_handle);
int veo_udma_peer_fini(int peer_id);
size_t veo_udma_send(struct veo_thr_ctxt *ctx, void *src, uint64_t dst, size_t len, int pack);
size_t veo_udma_recv(struct veo_thr_ctxt *ctx, uint64_t src, void *dst, size_t len);
int veo_udma_pack(int peer, void *src, uint64_t dst, size_t len);
int veo_udma_pack_commit(int peer);

#ifdef __cplusplus
}
#endif

#endif /* VEO_UDMA_COMM_INCLUDE */
