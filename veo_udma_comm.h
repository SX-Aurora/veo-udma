#ifndef VEO_UDMA_COMM_INCLUDE
#define VEO_UDMA_COMM_INCLUDE

#define UDMA_MAX_PROCS 8
#define UDMA_MAX_PEERS 64
#define UDMA_MAX_SPLIT 32
#define UDMA_BUFF_LEN (1 * 1024 * 1024)
#define UDMA_NUM_BUFFS 2

#define UDMA_DELAY_PEEK 1

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define DEBUG 1
#ifdef DEBUG
#define dprintf(args...) printf(args)
#endif
#define eprintf(args...) fprintf(stderr, args)


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
	size_t *len;	// address of length mailbox
	void *shm;	// buffer inside the shared memory segment
};
struct vh_udma_peer {
	struct vh_udma_comm send[UDMA_NUM_BUFFS];
	struct vh_udma_comm recv[UDMA_NUM_BUFFS];
	struct veo_thr_ctxt *ctx;
	int proc_id;
	int shm_key, shm_segid;
	size_t shm_size;
	void *shm_addr;
};

struct ve_udma_comm {
	uint64_t len_vehva;	// address of length mailbox
	uint64_t shm_vehva;
	uint64_t buff_vehva;
	void *buff;
};
struct ve_udma_peer {
	struct ve_udma_comm send[UDMA_NUM_BUFFS];
	struct ve_udma_comm recv[UDMA_NUM_BUFFS];
};

#endif /* VEO_UDMA_COMM_INCLUDE */
