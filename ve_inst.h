#ifdef __cplusplus
extern "C"
{
#endif
	
static inline void ve_inst_fenceSF(void)
{
    asm ("fencem 1":::"memory");
}

static inline void ve_inst_fenceLF(void)
{
    asm ("fencem 2":::"memory");
}

static inline void ve_inst_fenceLSF(void)
{
    asm ("fencem 3":::"memory");
}

static inline uint64_t ve_inst_lhm(void *vehva)
{
    uint64_t volatile ret;
    asm volatile ("lhm.l %0,0(%1)":"=r"(ret):"r"(vehva));
    return ret;
}

static inline void ve_inst_shm(void *vehva, uint64_t value)
{
    asm ("shm.l %0,0(%1)"::"r"(value),"r"(vehva));
}
#ifdef __cplusplus
}
#endif
