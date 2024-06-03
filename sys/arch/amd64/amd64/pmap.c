/*
 * Copyright (c) 2023-2024 Ian Marco Moffett and the Osmora Team.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Hyra nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/physseg.h>
#include <sys/cdefs.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <machine/tlb.h>
#include <machine/lapic.h>
#include <machine/sysvec.h>
#include <machine/idt.h>
#include <assert.h>
#include <string.h>

/*
 * Page-Table Entry (PTE) flags
 *
 * See Intel SDM Vol 3A, Section 4.5, Table 4-19
 */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000
#define PTE_P           __BIT(0)        /* Present */
#define PTE_RW          __BIT(1)        /* Writable */
#define PTE_US          __BIT(2)        /* User r/w allowed */
#define PTE_PWT         __BIT(3)        /* Page-level write-through */
#define PTE_PCD         __BIT(4)        /* Page-level cache disable */
#define PTE_ACC         __BIT(5)        /* Accessed */
#define PTE_DIRTY       __BIT(6)        /* Dirty (written-to page) */
#define PTE_PAT         __BIT(7)
#define PTE_GLOBAL      __BIT(8)
#define PTE_NX          __BIT(63)       /* Execute-disable */

__attr(interrupt) static void
tlb_shootdown_isr(void *sf)
{
    struct cpu_info *ci = this_cpu();
    struct intr_info *intr_info = ci->tlb_shootdown;

    /* Setup interrupt information if needed */
    if (ci->tlb_shootdown == NULL) {
        intr_info = intr_info_alloc("TLB-Shootdown", "LAPIC-IPI");
        intr_info->affinity = ci->id;

        ci->tlb_shootdown = intr_info;
        intr_register(intr_info);
    }

    ++intr_info->count;
    tlb_flush(ci->tlb_flush_ptr);

    ci->tlb_flush_ptr = 0;
    lapic_send_eoi();
}

static void
tlb_shootdown(vaddr_t flush_va)
{
    struct cpu_info *curcpu, *ci = NULL;
    size_t idx = 0;

    curcpu = this_cpu();
    while ((ci = cpu_get(idx++)) != NULL) {
        if (ci->id == curcpu->id)
            continue;

        ci->tlb_flush_ptr = flush_va;
    }

    lapic_send_ipi(0, IPI_SHORTHAND_OTHERS, SYSVEC_TLB);
}

/*
 * Convert pmap prot flags to PTE flags.
 */
static uint64_t
pmap_prot_to_pte(vm_prot_t prot)
{
    uint64_t pte_flags = PTE_P | PTE_NX;

    if (__TEST(prot, PROT_WRITE))
        pte_flags |= PTE_RW;
    if (__TEST(prot, PROT_EXEC))
        pte_flags &= ~(PTE_NX);
    if (__TEST(prot, PROT_USER))
        pte_flags |= PTE_US;

    return pte_flags;
}

/*
 * Returns index for a specific pagemap level.
 *
 * @level: Requested level.
 * @va: Virtual address.
 */
static size_t
pmap_get_level_index(uint8_t level, vaddr_t va)
{
    /* TODO: Make this bullshit assertion better */
    __assert(level <= 4 && level != 0);

    switch (level) {
    case 4:
        return (va >> 39) & 0x1FF;
    case 3:
        return (va >> 30) & 0x1FF;
    case 2:
        return (va >> 21) & 0x1FF;
    case 1:
        return (va >> 12) & 0x1FF;
    default:        /* Should not be reachable */
        return 0;
    }
}

static inline uintptr_t *
pmap_extract(uint8_t level, vaddr_t va, uintptr_t *pmap, bool allocate)
{
    uintptr_t next;
    uintptr_t level_alloc;
    size_t idx;

    idx = pmap_get_level_index(level, va);

    if (__TEST(pmap[idx], PTE_P)) {
        next = pmap[idx] & PTE_ADDR_MASK;
        return PHYS_TO_VIRT(next);
    }

    if (!allocate) {
        return 0;
    }

    /*
     * TODO: If we are out of pageframes here, we don't want to panic
     *       here. We need to have some sort of clean error reporting.
     */
    level_alloc = vm_alloc_pageframe(1);
    __assert(level_alloc != 0);

    /* Zero the memory */
    memset(PHYS_TO_VIRT(level_alloc), 0, vm_get_page_size());

    pmap[idx] = level_alloc | (PTE_P | PTE_RW | PTE_US);
    return PHYS_TO_VIRT(level_alloc);
}

/*
 * Modify a page table by writing `val' to it.
 *
 * @ctx: Virtual memory context.
 * @vas: Virtual address space.
 * @va: Virtual address.
 * @alloc: True to alloc new entries.
 * @res: Result
 */
static int
pmap_get_tbl(struct vm_ctx *ctx, struct vas vas, vaddr_t va, bool alloc,
             uintptr_t **res)
{
    uintptr_t *pml4 = PHYS_TO_VIRT(vas.top_level);
    uintptr_t *pdpt, *pd, *tbl;
    int status = 0;

    pdpt = pmap_extract(4, va, pml4, alloc);
    if (pdpt == NULL) {
        status = 1;
        goto done;
    }

    pd = pmap_extract(3, va, pdpt, alloc);
    if (pd == NULL) {
        status = 1;
        goto done;
    }

    tbl = pmap_extract(2, va, pd, alloc);
    if (tbl == NULL) {
        status = 1;
        goto done;
    }

    *res = tbl;
done:
    return status;
}

/*
 * Flush a virtual address.
 *
 * @va: VA to flush from TLB.
 */
static void
pmap_flush(vaddr_t va)
{
    /*
     * Do TLB shootdown if multiple CPUs are listed.
     *
     * XXX: Some might not be listed during early
     *      startup.
     */
    if (cpu_count() > 1) {
        tlb_shootdown(va);
    }

    tlb_flush(va);
}

/*
 * Modify a page table by writing `val' to it.
 *
 * TODO: Ensure operations here are serialized.
 */
static int
pmap_modify_tbl(struct vm_ctx *ctx, struct vas vas, vaddr_t va, size_t val)
{
    uintptr_t *tbl;
    int status;

    if ((status = pmap_get_tbl(ctx, vas, va, true, &tbl) != 0)) {
        return status;
    }

    /* Map our page */
    tbl[pmap_get_level_index(1, va)] = val;
    pmap_flush(va);
    return 0;
}

int
pmap_set_cache(struct vm_ctx *ctx, struct vas vas, vaddr_t va, int type)
{
    uintptr_t *tbl;
    int status;
    size_t idx;

    if ((status = pmap_get_tbl(ctx, vas, va, false, &tbl)) != 0) {
        return status;
    }

    idx = pmap_get_level_index(1, va);

    /* Set the policy based on the type */
    switch (type) {
    case VM_CACHE_UC:
        tbl[idx] |= PTE_PCD;
        tbl[idx] &= ~(PTE_PWT);
        break;
    case VM_CACHE_WT:
        tbl[idx] &= ~(PTE_PCD);
        tbl[idx] |= PTE_PWT;
        break;
    default:
        /* Invalid type */
        return 1;
    }

    pmap_flush(va);
    return 0;
}

int
pmap_map(struct vm_ctx *ctx, struct vas vas, vaddr_t va, paddr_t pa,
         vm_prot_t prot)
{
    uint32_t flags = pmap_prot_to_pte(prot);

    return pmap_modify_tbl(ctx, vas, va, (pa | flags));
}

int
pmap_unmap(struct vm_ctx *ctx, struct vas vas, vaddr_t va)
{
    return pmap_modify_tbl(ctx, vas, va, 0);
}

int
pmap_create_vas(struct vm_ctx *ctx, struct vas *res)
{
    struct vas current_vas = pmap_read_vas();
    struct vas new_vas = {0};
    uint64_t *src, *dest;

    /*
     * We want to allocate a zeroed pageframe
     * and copy the higher half to it. The lower
     * half can remain zero for userland.
     */
    new_vas.top_level = vm_alloc_pageframe(1);

    if (new_vas.top_level == 0) {
        return -1;
    }

    src = PHYS_TO_VIRT(current_vas.top_level);
    dest = PHYS_TO_VIRT(new_vas.top_level);

    /*
     * Copy the top half and zero the bottom
     * half.
     */
    for (size_t i = 0; i < 512; ++i) {
        if (i < 256) {
            dest[i] = 0;
            continue;
        }
        dest[i] = src[i];
    }

    *res = new_vas;
    return 0;
}

void
pmap_switch_vas(struct vm_ctx *ctx, struct vas vas)
{
    uintptr_t cr3_val = vas.cr3_flags | vas.top_level;

    __ASMV("mov %0, %%cr3"
           :
           : "r" (cr3_val)
           : "memory");
}

/*
 * TODO: During the mapping of a virtual address, a level
 *       may be allocated. This function does not handle the
 *       freeing of allocated levels. We should keep track
 *       of levels allocated and free them here.
 */
int
pmap_free_vas(struct vm_ctx *ctx, struct vas vas)
{
    vm_free_pageframe(vas.top_level, 1);
    return 0;
}

struct vas
pmap_read_vas(void)
{
    struct vas vas;
    uintptr_t cr3_raw;

    __ASMV("mov %%cr3, %0"
           : "=r" (cr3_raw)
    );

    vas.cr3_flags = cr3_raw & ~PTE_ADDR_MASK;
    vas.top_level = cr3_raw & PTE_ADDR_MASK;
    vas.use_l5_paging = false;      /* TODO: Check for support */
    vas.lock.lock = 0;
    return vas;
}

int
pmap_init(struct vm_ctx *ctx)
{
    idt_set_desc(SYSVEC_TLB, IDT_INT_GATE_FLAGS,
                 (uintptr_t)tlb_shootdown_isr, 0);

    return 0;
}
