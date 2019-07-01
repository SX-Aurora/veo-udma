#ifndef VEO_UDMA_COMM_INCLUDE
#define VEO_UDMA_COMM_INCLUDE

#define UDMA_MAX_PROCS 8
#define UDMA_MAX_PEERS 64
#define UDMA_MAX_SPLIT 64
#define UDMA_BUFF_LEN (64 * 1024 * 1024)

#define UDMA_DELAY_PEEK 1
#define UDMA_TIMEOUT_US (10 * 1000000)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

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
	size_t *len;		// address of length mailbox
	size_t buff_len;	// total buffer space length
	void *shm;		// buffer inside the shared memory segment
};
struct vh_udma_peer {
	struct vh_udma_comm send;
	struct vh_udma_comm recv;
	struct veo_thr_ctxt *ctx;
	int proc_id;
	int shm_key, shm_segid;
	size_t shm_size;
	void *shm_addr;
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
};

int veo_udma_peer_init(int ve_node_id, struct veo_proc_handle *proc,
		       struct veo_thr_ctxt *ctx, uint64_t lib_handle);
int veo_udma_peer_fini(int peer_id);
size_t veo_udma_send(struct veo_thr_ctxt *ctx, void *src, uint64_t dst, size_t len);
size_t veo_udma_recv(struct veo_thr_ctxt *ctx, uint64_t src, void *dst, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VEO_UDMA_COMM_INCLUDE */
