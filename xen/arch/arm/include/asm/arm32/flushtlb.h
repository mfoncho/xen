#ifndef __ASM_ARM_ARM32_FLUSHTLB_H__
#define __ASM_ARM_ARM32_FLUSHTLB_H__

/*
 * Every invalidation operation use the following patterns:
 *
 * DSB ISHST        // Ensure prior page-tables updates have completed
 * TLBI...          // Invalidate the TLB
 * DSB ISH          // Ensure the TLB invalidation has completed
 * ISB              // See explanation below
 *
 * For Xen page-tables the ISB will discard any instructions fetched
 * from the old mappings.
 *
 * For the Stage-2 page-tables the ISB ensures the completion of the DSB
 * (and therefore the TLB invalidation) before continuing. So we know
 * the TLBs cannot contain an entry for a mapping we may have removed.
 *
 * Note that for local TLB flush, using non-shareable (nsh) is sufficient
 * (see G5-9224 in ARM DDI 0487I.a).
 */
#define TLB_HELPER(name, tlbop, sh) \
static inline void name(void)       \
{                                   \
    dsb(sh ## st);                  \
    WRITE_CP32(0, tlbop);           \
    dsb(sh);                        \
    isb();                          \
}

/* Flush local TLBs, current VMID only */
TLB_HELPER(flush_guest_tlb_local, TLBIALL, nsh);

/* Flush inner shareable TLBs, current VMID only */
TLB_HELPER(flush_guest_tlb, TLBIALLIS, ish);

/* Flush local TLBs, all VMIDs, non-hypervisor mode */
TLB_HELPER(flush_all_guests_tlb_local, TLBIALLNSNH, nsh);

/* Flush innershareable TLBs, all VMIDs, non-hypervisor mode */
TLB_HELPER(flush_all_guests_tlb, TLBIALLNSNHIS, ish);

/* Flush all hypervisor mappings from the TLB of the local processor. */
TLB_HELPER(flush_xen_tlb_local, TLBIALLH, nsh);

/* Flush TLB of local processor for address va. */
static inline void __flush_xen_tlb_one_local(vaddr_t va)
{
    asm volatile(STORE_CP32(0, TLBIMVAH) : : "r" (va) : "memory");
}

/* Flush TLB of all processors in the inner-shareable domain for address va. */
static inline void __flush_xen_tlb_one(vaddr_t va)
{
    asm volatile(STORE_CP32(0, TLBIMVAHIS) : : "r" (va) : "memory");
}

#endif /* __ASM_ARM_ARM32_FLUSHTLB_H__ */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
