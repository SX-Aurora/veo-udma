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
#include "veo_udma_comm.h"

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


long getusrcc() {
        asm("smir %s0, %usrcc");
}

void busy_sleep_us(long us)
{
	long ts = getusrcc();
	do {} while (getusrcc() - ts < us * 1400);
}

/*
  Initialize VH-SHM segment, map it as VEHVA.
*/
int vhshm_register(int key, size_t size)
{
	struct shmid_ds ds;
	int err = 0;

	//TODO: unregister and release old segment if reinitializing a new one
	shm_key = key;
	shm_size = size;

	printf("VE: (shm_key = %d, size = %d)\n", shm_key, shm_size);

	//
	// determine shm segment ID from its key
	//
	shm_segid = vh_shmget(shm_key, shm_size, SHM_HUGETLB);
	if (shm_segid == -1) {
		printf("VE: vh_shmget key=%d failed, reason: %s\n", key, strerror(errno));
		return -1;
	}

	printf("VE: shm_segid = %d\n", shm_segid);

	uint64_t remote_vehva = 0;
	void *remote_addr = NULL;

	//
	// attach shared memory VH address space and register it to DMAATB,
	// the region is accessible for DMA unter its VEHVA remote_vehva
	//
        shm_remote_addr = vh_shmat(shm_segid, NULL, 0, (void **)&shm_vehva);

	if (shm_remote_addr == NULL) {
		printf("VE: (remote_addr == NULL)\n");
		return 3;
	}
	if (shm_vehva == (uint64_t)-1) {
		printf("VE: (shm_vehva == -1)\n");
		return 4;
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
	if (ve_up == NULL)
		return -ENOMEM;
	printf("ve allocated ve_up=%p\n", (void *)ve_up);
	udma_peer = ve_up;

	// find and register shm segment, if not done, yet
	if (shm_vehva == 0) {
		err = vhshm_register(key, size);
		if (err) {
			free(udma_peer);
			udma_peer = NULL;
			return err;
		}
	}
	printf("veshm_register ok\n");
	
	// now fill the ve_udma_peer structure
	for (j = 0; j < UDMA_NUM_BUFFS; j++) {
		ve_up->send[j].len_vehva = shm_vehva +((uint64_t)vh_up->recv[j].len - vh_shm_base);
		ve_up->send[j].shm_vehva = shm_vehva +((uint64_t)vh_up->recv[j].shm - vh_shm_base);
		ve_up->recv[j].len_vehva = shm_vehva +((uint64_t)vh_up->send[j].len - vh_shm_base);
		ve_up->recv[j].shm_vehva = shm_vehva +((uint64_t)vh_up->send[j].shm - vh_shm_base);
	}

	printf("ve_up base settings ok\n");

	// Initialize DMA
	err = ve_dma_init();
	if (err) {
		printf("Failed to initialize DMA\n");
		return err;
	}
	char *buff_base;
	uint64_t buff_base_vehva;
	size_t align_64mb = 64 * 1024 * 1024;
	size_t buff_size = 2 * UDMA_NUM_BUFFS * UDMA_BUFF_LEN;
	buff_size = (buff_size + align_64mb - 1) & ~(align_64mb - 1);
	
	// allocate read and write buffers in one call
	posix_memalign((void **)&buff_base, align_64mb, buff_size);
	if (buff_base == NULL) {
		printf("allocating ve udma buffer failed! buffsize=%lu\n", buff_size);
		return -ENOMEM;
	}
	printf("ve allocated buff at %p\n", buff_base);
	//busy_sleep_us(1*1000*1000);
	buff_base_vehva = ve_register_mem_to_dmaatb(buff_base, buff_size);
	if (buff_base_vehva == (uint64_t)-1) {
		printf("mapping ve udma buffer failed! buffsize=%lu\n", buff_size);
		return -ENOMEM;
	}
	printf("ve_register_mem_to_dmaatb succeeded for %p\n", buff_base);
	for (j = 0; j < UDMA_NUM_BUFFS; j++) {
		ve_up->send[j].buff = buff_base;
		buff_base += UDMA_BUFF_LEN;
		ve_up->send[j].buff_vehva = buff_base_vehva;
		buff_base_vehva += UDMA_BUFF_LEN;
		ve_up->recv[j].buff = buff_base;
		buff_base += UDMA_BUFF_LEN;
		ve_up->recv[j].buff_vehva = buff_base_vehva;
		buff_base_vehva += UDMA_BUFF_LEN;
	}

	printf("ve_udma_init done\n");
	return 0;
}

int ve_udma_fini()
{
	int err;

	// unregister local buffer from DMAATB
	err = ve_unregister_mem_from_dmaatb(udma_peer->send[0].buff_vehva);
	if (err)
		printf("Failed to unregister local buffer from DMAATB\n");

	// detach VH sysV shm segment
	if (shm_remote_addr) {
		err = vh_shmdt(shm_remote_addr);
		if (err)
			printf("Failed to detach from VH sysV shm\n");
		else {
			shm_remote_addr = NULL;
			shm_vehva = 0;
		}
	}

}

size_t ve_udma_send(void *src, size_t len)
{
	int j, jr, err;
	size_t lenp = len, tlen;
	size_t tlenr[UDMA_NUM_BUFFS] = {0};
	struct ve_udma_peer *ve_up = udma_peer;
	char *srcp = (char *)src;
	uint64_t srcr[UDMA_NUM_BUFFS];
	ve_dma_handle_t handle[UDMA_NUM_BUFFS];

	j = 0; jr = -1;
	while(lenp > 0 || (jr >= 0 && tlenr[jr] > 0)) {
		if (tlenr[j] == 0 && lenp > 0) {
			while(ve_inst_lhm((void *)ve_up->send[j].len_vehva) > 0);
			tlen = MIN(UDMA_BUFF_LEN, lenp);
			// dma from shm to buff
			err = ve_dma_post(ve_up->send[j].shm_vehva, ve_up->send[j].buff_vehva,
					  (int)tlen, &handle[j]);
			if (err) {
				if (err == -EAGAIN)
					continue;
				printf("ve_dma_post has failed! err = %d\n", err);
				ve_inst_shm((void *)ve_up->send[j].len_vehva, 0);
				break;
			}
			srcr[j] = (uint64_t)srcp;
			tlenr[j] = tlen;
			if (jr == -1)
				jr = j;
		
			lenp -= tlen;
			srcp += tlen;
			j = (j + 1) % UDMA_NUM_BUFFS;
		}

		if (jr >= 0 && tlenr[jr] > 0) {
			err = ve_dma_poll(&handle[jr]);

			if (err == 0) { // DMA completed normally
				ve_inst_shm((void *)ve_up->send[jr].len_vehva, tlenr[jr]);
				tlenr[jr] = 0;
				jr = (jr + 1) % UDMA_NUM_BUFFS;
			} else if (err != -EAGAIN) {
				printf("ve_dma_poll returned an error: 0x%x\n", err);
				break;
			}
		}
	}
	return len - lenp;
}

size_t ve_udma_recv1(void *dst, size_t len)
{
	int err;
	size_t lenp = len, tlen;
	struct ve_udma_peer *ve_up = udma_peer;
	char *dstp = (char *)dst;

	while(lenp > 0) {
		// wait for len signal to be set
		while((tlen = ve_inst_lhm((void *)ve_up->recv[0].len_vehva)) == 0);
		// dma from shm to buff
		err = ve_dma_post_wait(ve_up->recv[0].buff_vehva, ve_up->recv[0].shm_vehva, tlen);
		if (err) {
			printf("ve_dma_post_wait has failed! err = %d\n", err);
			ve_inst_shm((void *)ve_up->recv[0].len_vehva, 0);
			break;
		}
		memcpy((void *)dstp, ve_up->recv[0].buff, tlen);
		// set len to 0
		ve_inst_shm((void *)ve_up->recv[0].len_vehva, 0);
		lenp -= tlen;
		dstp += tlen;
	}
	return len - lenp;
}

size_t ve_udma_recv(void *dst, size_t len)
{
	int j, jr, err;
	size_t lenp = len, tlen;
	size_t tlenr[UDMA_NUM_BUFFS] = {0};
	struct ve_udma_peer *ve_up = udma_peer;
	char *dstp = (char *)dst;
	uint64_t dstr[UDMA_NUM_BUFFS];
	ve_dma_handle_t handle[UDMA_NUM_BUFFS];

	j = 0; jr = -1;
	while(lenp > 0 || (jr >= 0 && tlenr[jr] > 0)) {
		if (tlenr[j] == 0 && lenp > 0) {
			// wait for len signal to be set
			while((tlen = ve_inst_lhm((void *)ve_up->recv[j].len_vehva)) == 0);
			// dma from shm to buff
			err = ve_dma_post(ve_up->recv[j].buff_vehva, ve_up->recv[j].shm_vehva,
					  (int)tlen, &handle[j]);
			if (err) {
				if (err == -EAGAIN)
					continue;
				printf("ve_dma_post has failed! err = %d\n", err);
				ve_inst_shm((void *)ve_up->recv[j].len_vehva, 0);
				break;
			}
			dstr[j] = (uint64_t)dstp;
			tlenr[j] = tlen;
			if (jr == -1)
				jr = j;
		
			lenp -= tlen;
			dstp += tlen;
			j = (j + 1) % UDMA_NUM_BUFFS;
		}

		if (jr >= 0 && tlenr[jr] > 0) {
			err = ve_dma_poll(&handle[jr]);

			if (err == 0) { // DMA completed normally
				memcpy((void *)dstr[jr], ve_up->recv[jr].buff, tlenr[jr]);
				ve_inst_shm((void *)ve_up->recv[jr].len_vehva, 0);
				tlenr[jr] = 0;
				jr = (jr + 1) % UDMA_NUM_BUFFS;
			} else if (err != -EAGAIN) {
				printf("ve_dma_poll returned an error: 0x%x\n", err);
				break;
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
