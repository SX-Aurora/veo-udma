# User DMA for VEO Buffer Transfers

VEO uses by default system DMA descriptors controlled by VEOS for
transfering data between Host (VH) and Vector Engine (VE). The system
DMA descriptors belong to a DMA engine that is CPU-wide and controlled
by the VE operating system (VEOS). It requires physical addresses on
both sides, and VEOS (root), which handles the data transfer on behalf
of the user's pseudo-process, has to do the virtual to physical
translations for VH and VE side on the fly. This is definitely slower
than using registered buffers and not having to do the translations on
the fly.

User DMA uses an engines that are available on each vector core and
are controlled by the user process on the VE side. An API for
accessing User DMA is part of *libsysve*, with documentation
[here](https://veos-sxarr-nec.github.io/libsysve/group__vedma.html).
The good news: this is using registered buffers, memory inside a
shared memory segment on VH, and is not shared by all cores. The bad
news: user DMA is controlled from the VE side, while VEO is a VH
controlled paradigm. So in order to use this feature, we need to
innitiate the transfer from the VH side and run a piece of code
handling all DMA related actions on the VE.



## Usage

### Source Code

Each thread context needs to be initialized as a udma communication peer:

```c
#include "veo_udma.h"

int peer_id = veo_udma_peer_init(ve_node_number, proc, ctx, handle);
```

The do something like:
```c
res = veo_udma_send(ctx, local_buff, ve_buff, bsize);

res = veo_udma_recv(ctx, ve_buff, local_buff, bsize);
```
The returned value is the number of transfered bytes.

These are synchronous calls like `veo_read_mem()` and
`veo_write_mem()`. The difference is that the first argument of the of
the user DMA based calls is a *thread context* while the original
calls need only the *proc handle*.

Finally unregister the peer (this will free the shared memory segment!).
```c
veo_udma_peer_fini(peer_id);
```

### Linking

Currently veo-udma only works when the VE side of the code is
staically linked into **veorun**. The *Makefile* produces a VE binary
called *veorun_static*, use this one with VEO (like in the example
program *hello*). You can use entirely static *veorun* by linking your
additional kernels into the new veorun, in that case add
*libveo_udma_ve.o* to the `mk_veorun_static` command (again, look at
the *Makefile* in this repository!).

The VH side of the VEO program must link against *libveo_udma.so*.


## Limitations

Currently this only works with static linking of veorun. There is
something about libveio maybe, that breaks the dynamic
linking/loading, I have no idea.

Only one context per VEO proc can be used. The reason is that thread
local variables are handled wrongly by mk_veorun_static and they
should actually be filtered out. Once this is fixed, multiple contexts
per proc will work.



## How it works

Each peer allocates a shared memory segment of 64MB which is mapped on
the VE side, too. The user buffers that need to be transfered are not
registered and not living inside the shared memory segment! Their
memory is copied into the user DMA buffer(s), transfered, then copied
to the right destination. The buffer is split virtually in pieces such
that the big transfer can overlap memcpys and DMA transfers.

In https://github.com/SX-Aurora/veo-udma/blob/master/RESULTS.splits I
did a study of performance dependence on splits and split sizes. The
results of this are used (sort of) inside the code to tune the
splits/overlaps depending on the transfer size. Now the transfer
results should be quite optimal:

```
[focht@aurora1 veo_comm]$ ./scan_perf.sh
 buff MB   send MB/s   recv MB/s
       1      4418      4546
       2      6244      6540
       4      7913      8404
       8      9126      9583
      16     10117      9747
      32      8917     10377
      64      8603     10499
     256      8616      9536
    1024      8612     10407
```
