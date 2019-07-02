/*
  Compile:

  /opt/nec/ve/bin/ncc -shared -fpic -o libvehello.so libvehello.c

*/

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>

#include <vhshm.h>
#include <vedma.h>

#include "ve_inst.h"
#include "veo_udma.h"

int shm_key, shm_segid;
uint64_t shm_vehva = 0;		// VEHVA of remote shared memory segment
size_t shm_size = 0;		// remote shared memory segment size
void *shm_remote_addr = NULL;	// remote address

/*
  The following variables are thread local because each Context Thread can be a peer!
*/
//__thread int this_peer = -1;			// local peer ID
struct ve_udma_peer *udma_peer;	// this peer's UDMA comm struct
//__thread struct ve_udma_peer *udma_peer;	// this peer's UDMA comm struct


static inline long getusrcc() {
	asm("smir %s0, %usrcc");
}

/* following functions imply that the VE runs with 1400MHz,
   for their purpose the exact frequency doesn't actually matter */
static inline int64_t usrcc_diff_us(long ts)
{
	return (getusrcc() - ts) / 1400;
}

static inline void busy_sleep_us(long us)
{
	long ts = getusrcc();
	do {} while (getusrcc() - ts < us * 1400);
}

/*
  Initialize VH-SHM segment, map it as VEHVA.
*/
static int vhshm_register(int key, size_t size)
{
	struct shmid_ds ds;
	uint64_t remote_vehva = 0;
	void *remote_addr = NULL;
	int err = 0;

	//TODO: unregister and release old segment if reinitializing a new one
	shm_key = key;
	shm_size = size;
	dprintf("VE: (shm_key = %d, size = %d)\n", shm_key, shm_size);

	//
	// determine shm segment ID from its key
	//
	shm_segid = vh_shmget(shm_key, shm_size, SHM_HUGETLB);
	if (shm_segid == -1) {
		eprintf("VE: vh_shmget key=%d failed, reason: %s\n", key, strerror(errno));
		return -EINVAL;
	}
	dprintf("VE: shm_segid = %d\n", shm_segid);

	//
	// attach shared memory VH address space and register it to DMAATB,
	// the region is accessible for DMA unter its VEHVA remote_vehva
	//
        shm_remote_addr = vh_shmat(shm_segid, NULL, 0, (void **)&shm_vehva);
	if (shm_remote_addr == NULL) {
		eprintf("VE: (remote_addr == NULL)\n");
		return -ENOMEM;
	}
	if (shm_vehva == (uint64_t)-1) {
		eprintf("VE: failed to attach to shm segment %d, shm_vehva=-1\n", shm_segid);
		return -ENOMEM;
	}
	return 0;
}

int ve_udma_init(struct vh_udma_peer *vh_up)
{
	int err, j;
	int key = vh_up->shm_key;
	size_t size = vh_up->shm_size;
	uint64_t vh_shm_base = (uint64_t)vh_up->shm_addr;
	struct ve_udma_peer *ve_up;

	ve_up = (struct ve_udma_peer *)malloc(sizeof(struct ve_udma_peer));
	if (ve_up == NULL) {
		eprintf("VE: malloc failed for peer struct.\n");
		return -ENOMEM;
	}
	dprintf("ve allocated ve_up=%p\n", (void *)ve_up);
	udma_peer = ve_up;

	// find and register shm segment, if not done, yet
	if (shm_vehva == 0) {
		err = vhshm_register(key, size);
		if (err) {
			free(udma_peer);
			udma_peer = NULL;
			eprintf("VE: vh_shm_register failed, err=%d.\n", err);
			return err;
		}
	}
	
	// now fill the ve_udma_peer structure
	ve_up->send.len_vehva = shm_vehva +((uint64_t)vh_up->recv.len - vh_shm_base);
	ve_up->send.shm_vehva = shm_vehva +((uint64_t)vh_up->recv.shm - vh_shm_base);
	ve_up->recv.len_vehva = shm_vehva +((uint64_t)vh_up->send.len - vh_shm_base);
	ve_up->recv.shm_vehva = shm_vehva +((uint64_t)vh_up->send.shm - vh_shm_base);

	// Initialize DMA
	err = ve_dma_init();
	if (err) {
		eprintf("Failed to initialize DMA\n");
		return err;
	}
	char *buff_base;
	uint64_t buff_base_vehva;
	size_t align_64mb = 64 * 1024 * 1024;
	size_t buff_size = 2 * UDMA_BUFF_LEN;
	buff_size = (buff_size + align_64mb - 1) & ~(align_64mb - 1);
	
	// allocate read and write buffers in one call
	posix_memalign((void **)&buff_base, align_64mb, buff_size);
	if (buff_base == NULL) {
		eprintf("VE: allocating udma buffer failed! buffsize=%lu\n", buff_size);
		return -ENOMEM;
	}
	dprintf("ve allocated buff at %p\n", buff_base);
	//busy_sleep_us(1*1000*1000);
	buff_base_vehva = ve_register_mem_to_dmaatb(buff_base, buff_size);
	if (buff_base_vehva == (uint64_t)-1) {
		eprintf("VE: mapping udma buffer failed! buffsize=%lu\n", buff_size);
		return -ENOMEM;
	}
	dprintf("ve_register_mem_to_dmaatb succeeded for %p\n", buff_base);
	ve_up->send.buff = buff_base;
	buff_base += UDMA_BUFF_LEN;
	ve_up->send.buff_vehva = buff_base_vehva;
	buff_base_vehva += UDMA_BUFF_LEN;
	ve_up->recv.buff = buff_base;
	buff_base += UDMA_BUFF_LEN;
	ve_up->recv.buff_vehva = buff_base_vehva;
	buff_base_vehva += UDMA_BUFF_LEN;
	return 0;
}

void ve_udma_fini()
{
	int err;

	// unregister local buffer from DMAATB
	err = ve_unregister_mem_from_dmaatb(udma_peer->send.buff_vehva);
	if (err)
		eprintf("VE: Failed to unregister local buffer from DMAATB\n");

	// detach VH sysV shm segment
	if (shm_remote_addr) {
		err = vh_shmdt(shm_remote_addr);
		if (err)
			eprintf("VE: Failed to detach from VH sysV shm\n");
		else {
			shm_remote_addr = NULL;
			shm_vehva = 0;
		}
	}
}

#define SPLITBUFF(base, idx, size) (void *)((char *)base + idx * size)
#define SPLITADDR(base, idx, size) (base + idx * size)
#define SPLITLEN(base, idx) (void *)(base + idx * sizeof(size_t))

size_t ve_udma_send(void *src, size_t len, int split, size_t split_size)
{
	int j, jr, err;
	int64_t lenp = len, tlen;
	int64_t tlenr[UDMA_MAX_SPLIT] = {0};
	struct ve_udma_peer *ve_up = udma_peer;
	char *srcp = (char *)src;
	uint64_t srcr[UDMA_MAX_SPLIT];
	long ts = getusrcc();
	ve_dma_handle_t handle[UDMA_MAX_SPLIT];

	j = 0; jr = -1;
	while(lenp > 0 || (jr >= 0 && tlenr[jr] > 0)) {
		if (tlenr[j] == 0 && lenp > 0) {
			err = 0;
			while(ve_inst_lhm(SPLITLEN(ve_up->send.len_vehva, j)) > 0) {
				if (usrcc_diff_us(ts) > UDMA_TIMEOUT_US) {
					eprintf("VE: timeout waiting for VH recv. "
						"len=%ul of %ul, split=%d, split_sz=%ul\n",
						lenp, len, split, split_size);
					err = -ETIME;
					break;
				}
			}
			if (err)
				break;
			tlen = MIN(split_size, lenp);
			memcpy(SPLITBUFF(ve_up->send.buff, j, split_size), (void *)srcp, tlen);

			// dma from shm to buff
			err = ve_dma_post(SPLITADDR(ve_up->send.shm_vehva, j, split_size),
					  SPLITADDR(ve_up->send.buff_vehva, j, split_size),
					  (int)tlen, &handle[j]);
			if (err) {
				if (err == -EAGAIN)
					continue;
				eprintf("VE: ve_dma_post has failed! err = %d\n", err);
				ve_inst_shm(SPLITLEN(ve_up->send.len_vehva, j), 0);
				break;
			}
			tlenr[j] = tlen;
			if (jr == -1)
				jr = j;
		
			lenp -= tlen;
			srcp += tlen;
			j = (j + 1) % split;
		}

		if (jr >= 0 && tlenr[jr] > 0) {
			err = ve_dma_poll(&handle[jr]);

			if (err == 0) { // DMA completed normally
				ve_inst_shm(SPLITLEN(ve_up->send.len_vehva, jr), tlenr[jr]);
				tlenr[jr] = 0;
				jr = (jr + 1) % split;
				ts = getusrcc();
			} else if (err != -EAGAIN) {
				eprintf("VE: ve_dma_poll returned an error: 0x%x\n", err);
				break;
			} else {
				if (usrcc_diff_us(ts) > UDMA_TIMEOUT_US) {
					eprintf("VE: timeout waiting for DMA descriptor. "
						"len=%u of %u, split=%d, split_sz=%u, "
						"jr=%d, tlen=%u\n",
						lenp, len, split, split_size, jr, tlenr[jr]);
					err = -ETIME;
					break;
				}
			}
		}
	}
	return len - lenp;
}

size_t ve_udma_recv(void *dst, size_t len, int split, size_t split_size)
{
	int j, jr, err;
	int64_t lenp = len, tlen;
	int64_t tlenr[UDMA_MAX_SPLIT] = {0};
	struct ve_udma_peer *ve_up = udma_peer;
	char *dstp = (char *)dst;
	uint64_t dstr[UDMA_MAX_SPLIT];
	long ts = getusrcc();
	ve_dma_handle_t handle[UDMA_MAX_SPLIT];

	j = 0; jr = -1;
	while(lenp > 0 || (jr >= 0 && tlenr[jr] > 0)) {
		if (tlenr[j] == 0 && lenp > 0) {
			err = 0;
			// wait for len signal to be set
			while((tlen = ve_inst_lhm(SPLITLEN(ve_up->recv.len_vehva, j))) == 0) {
				if (usrcc_diff_us(ts) > UDMA_TIMEOUT_US) {
					eprintf("VE: timeout waiting for tlen. "
						"len=%l of %ul, split=%d, split_sz=%ul\n",
						lenp, len, split, split_size);
					err = -ETIME;
					break;
				}
			}
			if (err)
				break;
			if (tlen > lenp) {
				eprintf("VE: stopping veo-udma: something's wrong:"
					" tlen=%l > lenp=%l\n",
					tlen, lenp);
				err = -EINVAL;
				break;
			}
			// dma from shm to buff
			err = ve_dma_post(SPLITADDR(ve_up->recv.buff_vehva, j, split_size),
					  SPLITADDR(ve_up->recv.shm_vehva, j, split_size),
					  (int)tlen, &handle[j]);
			if (err) {
				if (err == -EAGAIN)
					continue;
				eprintf("VE: ve_dma_post has failed! err = %d\n", err);
				ve_inst_shm(SPLITLEN(ve_up->recv.len_vehva, j), 0);
				break;
			}
			dstr[j] = (uint64_t)dstp;
			tlenr[j] = tlen;
			if (jr == -1)
				jr = j;
		
			lenp -= tlen;
			dstp += tlen;
			j = (j + 1) % split;
		}

		if (jr >= 0 && tlenr[jr] > 0) {
			err = ve_dma_poll(&handle[jr]);

			if (err == 0) { // DMA completed normally
				memcpy((void *)dstr[jr],
				       SPLITBUFF(ve_up->recv.buff, jr, split_size), tlenr[jr]);
				ve_inst_shm(SPLITLEN(ve_up->recv.len_vehva, jr), 0);
				tlenr[jr] = 0;
				jr = (jr + 1) % split;
				ts = getusrcc();
			} else if (err != -EAGAIN) {
				eprintf("VE: ve_dma_poll returned an error: 0x%x\n", err);
				break;
			} else {
				if (usrcc_diff_us(ts) > UDMA_TIMEOUT_US) {
					eprintf("VE: timeout waiting for DMA descriptor. "
						"len=%u of %u, split=%d, split_sz=%u, "
						"jr=%d, tlen=%u\n",
						lenp, len, split, split_size, jr, tlenr[jr]);
					err = -ETIME;
					break;
				}
			}
		}
	}
	return len - lenp;
}



/* 
   C version of some offloaded function.
   Actually this should also be fine with a fortran function.
*/
int my_func(int *n, double *a, double *b)
{
	int i;
	for(i = 0; i < *n; i++) {
		b[i] = a[i] * 2;
	}
	return 0;
}
