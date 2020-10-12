/*
 *  Common CPU TLB handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* Modified for Unicorn Engine by Nguyen Anh Quynh, 2015 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "exec/cpu_ldst.h"
#include "exec/cputlb.h"
#include "exec/memory-internal.h"
#include "exec/ram_addr.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "qemu/atomic.h"
#include "qemu/atomic128.h"
#include "accel/tcg/translate-all.h"

#include "uc_priv.h"

/* DEBUG defines, enable DEBUG_TLB_LOG to log to the CPU_LOG_MMU target */
/* #define DEBUG_TLB */
/* #define DEBUG_TLB_LOG */

//#define DEBUG_TLB

#ifdef DEBUG_TLB
# define DEBUG_TLB_GATE 1
# ifdef DEBUG_TLB_LOG
#  define DEBUG_TLB_LOG_GATE 1
# else
#  define DEBUG_TLB_LOG_GATE 0
# endif
#else
# define DEBUG_TLB_GATE 0
# define DEBUG_TLB_LOG_GATE 0
#endif

#define tlb_debug(fmt, ...) do { \
    if (DEBUG_TLB_LOG_GATE) { \
        qemu_log_mask(CPU_LOG_MMU, "%s: " fmt, __func__, \
                      ## __VA_ARGS__); \
    } else if (DEBUG_TLB_GATE) { \
        fprintf(stderr, "%s: " fmt, __func__, ## __VA_ARGS__); \
    } \
} while (0)

static void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr);
static bool tlb_is_dirty_ram(CPUTLBEntry *tlbe);
//static ram_addr_t qemu_ram_addr_from_host_nofail(struct uc_struct *uc, void *ptr);
static void tlb_add_large_page(CPUArchState *env, target_ulong vaddr,
                               target_ulong size, int mmu_idx);
static void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr);

void tlb_init(CPUState *cpu)
{
}

/* This is OK because CPU architectures generally permit an
 * implementation to drop entries from the TLB at any time, so
 * flushing more entries than required is only an efficiency issue,
 * not a correctness issue.
 */
void tlb_flush(CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;

    memset(env->tlb_table, -1, sizeof(env->tlb_table));
    memset(env->tlb_v_table, -1, sizeof(env->tlb_v_table));
    cpu_tb_jmp_cache_clear(cpu);

    env->vtlb_index = 0;
    memset(env->tlb_flush_addr, -1, sizeof(env->tlb_flush_addr));
    memset(env->tlb_flush_mask,  0, sizeof(env->tlb_flush_mask));

    memset(env->iotlb, -1, sizeof(env->iotlb));
    memset(env->iotlb_v, -1, sizeof(env->iotlb_v));
}

static void tlb_flush_one_mmu(CPUState *cpu, int mmu_idx)
{
    CPUArchState *env = cpu->env_ptr;

    memset(env->tlb_table[mmu_idx], -1, sizeof(env->tlb_table[0]));
    memset(env->tlb_v_table[mmu_idx], -1, sizeof(env->tlb_v_table[0]));

    env->tlb_flush_addr[mmu_idx] = -1;
    env->tlb_flush_mask[mmu_idx] = 0;

    cpu_tb_jmp_cache_clear(cpu);
}

void tlb_flush_page(CPUState *cpu, uint64_t addr)
{
    CPUArchState *env = cpu->env_ptr;
    int mmu_idx;

    tlb_debug("page : 0x%lx\n", addr);

    /* Check if we need to flush due to large pages.  */
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        target_ulong flush_mask = env->tlb_flush_mask[mmu_idx];
        target_ulong flush_addr = env->tlb_flush_addr[mmu_idx];
        if ((addr & flush_mask) == flush_addr) {
            tlb_debug("forcing full flush ("
                      TARGET_FMT_lx "/" TARGET_FMT_lx ")\n",
                      flush_addr, flush_mask);
            tlb_flush(cpu); // full flush of all TLBs
            return;
        }
    }

    addr &= TARGET_PAGE_MASK;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_flush_entry(tlb_entry(env, mmu_idx, addr), addr);
    }

    /* check whether there are entries that need to be flushed in the vtlb */
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        int k;
        for (k = 0; k < CPU_VTLB_SIZE; k++) {
            tlb_flush_entry(&env->tlb_v_table[mmu_idx][k], addr);
        }
    }

    tb_flush_jmp_cache(cpu, addr);
}

// JHW these need to execute on all cores belonging to a cluster
void tlb_flush_all_cpus_synced(CPUState *cpu) {
    uc_engine *uc = cpu->uc;
    g_assert(uc->tlb_cluster_flush != NULL);
    uc->tlb_cluster_flush(cpu);
}

void tlb_flush_page_all_cpus_synced(CPUState *cpu, target_ulong addr) {
    uc_engine *uc = cpu->uc;
    g_assert(uc->tlb_cluster_flush_page != NULL);
    uc->tlb_cluster_flush_page(cpu, addr);
}

void tlb_flush_by_mmuidx_all_cpus_synced(CPUState *cpu, uint16_t idxmap) {
    uc_engine *uc = cpu->uc;
    g_assert(uc->tlb_cluster_flush_mmuidx != NULL);
    uc->tlb_cluster_flush_mmuidx(cpu, idxmap);
}

void tlb_flush_page_by_mmuidx_all_cpus_synced(CPUState *cpu, target_ulong addr, uint16_t idxmap) {
    uc_engine *uc = cpu->uc;
    g_assert(uc->tlb_cluster_flush_page_mmuidx != NULL);
    uc->tlb_cluster_flush_page_mmuidx(cpu, addr, idxmap);
}

void tlb_reset_dirty_range(CPUTLBEntry *tlb_entry, uintptr_t start,
                           uintptr_t length)
{
    uintptr_t addr;

    if (tlb_is_dirty_ram(tlb_entry)) {
        addr = (tlb_addr_write(tlb_entry) & TARGET_PAGE_MASK) + tlb_entry->addend;
        if ((addr - start) < length) {
            tlb_entry->addr_write |= TLB_NOTDIRTY;
        }
    }
}

void tlb_reset_dirty(CPUState *cpu, ram_addr_t start1, ram_addr_t length)
{
    CPUArchState *env;

    int mmu_idx;

    env = cpu->env_ptr;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        unsigned int i;

        for (i = 0; i < CPU_TLB_SIZE; i++) {
            tlb_reset_dirty_range(&env->tlb_table[mmu_idx][i],
                                  start1, length);
        }

        for (i = 0; i < CPU_VTLB_SIZE; i++) {
            tlb_reset_dirty_range(&env->tlb_v_table[mmu_idx][i],
                                  start1, length);
        }
    }
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
void tlb_set_dirty(CPUState *cpu, target_ulong vaddr)
{
    CPUArchState *env = cpu->env_ptr;
    int mmu_idx;

    vaddr &= TARGET_PAGE_MASK;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_set_dirty1(tlb_entry(env, mmu_idx, vaddr), vaddr);
    }

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        int k;
        for (k = 0; k < CPU_VTLB_SIZE; k++) {
            tlb_set_dirty1(&env->tlb_v_table[mmu_idx][k], vaddr);
        }
    }
}

/* Add a new TLB entry. At most one entry for a given virtual address
   is permitted. Only a single TARGET_PAGE_SIZE region is mapped, the
   supplied size is only used by tlb_flush_page.  */
void tlb_set_page_with_attrs(CPUState *cpu, target_ulong vaddr,
                             hwaddr paddr, MemTxAttrs attrs, int prot,
                             int mmu_idx, target_ulong size)
{
    CPUArchState *env = cpu->env_ptr;
    MemoryRegionSection *section;
    unsigned int index;
    target_ulong address;
    target_ulong code_address;
    uintptr_t addend;
    CPUTLBEntry *te;
    hwaddr iotlb, xlat, sz, paddr_page;
    target_ulong vaddr_page;
    unsigned vidx = env->vtlb_index++ % CPU_VTLB_SIZE;
    int asidx = cpu_asidx_from_attrs(cpu, attrs);
    unsigned char* dmiptr = NULL;
    int newprot = prot;

    if (size < TARGET_PAGE_SIZE) {
        sz = TARGET_PAGE_SIZE;
    } else {
        if (size > TARGET_PAGE_SIZE) {
            tlb_add_large_page(env, vaddr, size, mmu_idx);
        }
        sz = size;
    }
    vaddr_page = vaddr & TARGET_PAGE_MASK;
    paddr_page = paddr & TARGET_PAGE_MASK;

    section = address_space_translate_for_iotlb(cpu, asidx, paddr_page,
                                                &xlat, &sz, attrs, &prot);
    assert(sz >= TARGET_PAGE_SIZE);

    tlb_debug("vaddr=" TARGET_FMT_lx " paddr=0x" TARGET_FMT_plx
              " prot=%x idx=%d\n",
              vaddr, paddr, prot, mmu_idx);

    address = vaddr_page;
    if (size < TARGET_PAGE_SIZE) {
        /*
         * Slow-path the TLB entries; we will repeat the MMU check and TLB
         * fill on every access.
         */
        address |= TLB_RECHECK;
    }

//    if (!memory_region_is_ram(section->mr) &&
//        !memory_region_is_romd(section->mr)) {
//        /* IO memory case */
//        address |= TLB_MMIO;
//        addend = 0;
//    } else {
//        /* TLB_MMIO for rom/romd handled below */
//        addend = (uintptr_t)((char*)memory_region_get_ram_ptr(section->mr) + xlat);
//    }

    newprot = prot;
    if (env->uc->get_dmi_ptr != NULL &&
        env->uc->get_dmi_ptr(env->uc->dmi_opaque, paddr_page, &dmiptr, &newprot)) {
        address &= ~TLB_MMIO;
        addend = (uintptr_t)dmiptr;
        prot = prot & newprot; // don't take more than we're allowed to
    } else if (memory_region_is_ram(section->mr) ||
               memory_region_is_romd(section->mr)) {
        address &= ~TLB_MMIO;
        addend = (uintptr_t)((char*)memory_region_get_ram_ptr(section->mr) + xlat);
    } else {
        address |= TLB_MMIO;
        addend = 0;
    }

    code_address = address;
    iotlb = memory_region_section_get_iotlb(cpu, section, vaddr_page,
                                            paddr_page, xlat, prot, &address);

    index = (vaddr_page >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    te = &env->tlb_table[mmu_idx][index];

    /* do not discard the translation in te, evict it into a victim tlb */
    env->tlb_v_table[mmu_idx][vidx] = *te;
    env->iotlb_v[mmu_idx][vidx] = env->iotlb[mmu_idx][index];

    /* refill the tlb */
    /*
     * At this point iotlb contains a physical section number in the lower
     * TARGET_PAGE_BITS, and either
     *  + the ram_addr_t of the page base of the target RAM (if NOTDIRTY or ROM)
     *  + the offset within section->mr of the page base (otherwise)
     * We subtract the vaddr_page (which is page aligned and thus won't
     * disturb the low bits) to give an offset which can be added to the
     * (non-page-aligned) vaddr of the eventual memory access to get
     * the MemoryRegion offset for the access. Note that the vaddr we
     * subtract here is that of the page base, and not the same as the
     * vaddr we add back in io_readx()/io_writex()/get_page_addr_code().
     */
    env->iotlb[mmu_idx][index].addr = iotlb - vaddr_page;
    env->iotlb[mmu_idx][index].attrs = attrs;

    /* link iotlb back to tlb to enable dmi */
    env->iotlb[mmu_idx][index].phys = paddr_page;
    env->iotlb[mmu_idx][index].p2v = te;

    te->addend = addend - vaddr_page;
    if (prot & PAGE_READ) {
        te->addr_read = address;
    } else {
        te->addr_read = -1;
    }

    if (prot & PAGE_EXEC) {
        te->addr_code = code_address | TLB_NOTPROTECTED;
    } else {
        te->addr_code = -1;
    }

    if (prot & PAGE_WRITE) {
        te->addr_write = address | TLB_NOTDIRTY;
    } else {
        te->addr_write = -1;
    }

//    if (prot & PAGE_WRITE) {
//        if ((memory_region_is_ram(section->mr) && section->readonly)
//            || memory_region_is_romd(section->mr)) {
//            /* Write access calls the I/O callback.  */
//            te->addr_write = address | TLB_MMIO;
//        } else if (memory_region_is_ram(section->mr)) {
//            te->addr_write = address /*| TLB_NOTDIRTY*/;
//        } else {
//            te->addr_write = address;
//        }
//    } else {
//        te->addr_write = -1;
//    }
}

/* Add a new TLB entry, but without specifying the memory
 * transaction attributes to be used.
 */
void tlb_set_page(CPUState *cpu, target_ulong vaddr,
                  hwaddr paddr, int prot,
                  int mmu_idx, target_ulong size)
{
    tlb_set_page_with_attrs(cpu, vaddr, paddr, MEMTXATTRS_UNSPECIFIED,
                            prot, mmu_idx, size);
}

#ifdef JHW
static ram_addr_t qemu_ram_addr_from_host_nofail(struct uc_struct *uc, void *ptr)
{
    ram_addr_t ram_addr;

    ram_addr = qemu_ram_addr_from_host(uc, ptr);
    if (ram_addr == RAM_ADDR_INVALID) {
        //error_report("Bad ram pointer %p", ptr);
        return RAM_ADDR_INVALID;
    }

    return ram_addr;
}
#endif

/* Return true if ADDR is present in the victim tlb, and has been copied
   back to the main tlb.  */
static bool victim_tlb_hit(CPUArchState *env, size_t mmu_idx, size_t index,
                           size_t elt_ofs, target_ulong page)
{
    size_t vidx;
    for (vidx = 0; vidx < CPU_VTLB_SIZE; ++vidx) {
        CPUTLBEntry *vtlb = &env->tlb_v_table[mmu_idx][vidx];
        target_ulong cmp;

        /* elt_ofs might correspond to .addr_write, so use atomic_read */
#if TCG_OVERSIZED_GUEST
        cmp = *(target_ulong *)((uintptr_t)vtlb + elt_ofs);
#else
        cmp = atomic_read((target_ulong *)((uintptr_t)vtlb + elt_ofs));
#endif

        if (cmp == page) {
            /* Found entry in victim tlb, swap tlb and iotlb.  */
            CPUTLBEntry tmptlb, *tlb = &env->tlb_table[mmu_idx][index];
            CPUIOTLBEntry tmpio, *io = &env->iotlb[mmu_idx][index];
            CPUIOTLBEntry *vio = &env->iotlb_v[mmu_idx][vidx];

            tmptlb = *tlb; *tlb = *vtlb; *vtlb = tmptlb;
            tmpio = *io; *io = *vio; *vio = tmpio;
            return true;
        }
    }
    return false;
}

/* Macro to call the above, with local variables from the use context.  */
#define VICTIM_TLB_HIT(TY, ADDR) \
  victim_tlb_hit(env, mmu_idx, index, offsetof(CPUTLBEntry, TY), \
                 (ADDR) & TARGET_PAGE_MASK)

/* NOTE: this function can trigger an exception */
/* NOTE2: the returned address is not exactly the physical address: it
 * is actually a ram_addr_t (in system mode; the user mode emulation
 * version of this function returns a guest virtual address).
 */
tb_page_addr_t get_page_addr_code(CPUArchState *env, target_ulong addr)
{
    uintptr_t mmu_idx = cpu_mmu_index(env, true);
    uintptr_t index = tlb_index(env, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(env, mmu_idx, addr);

    if (unlikely(!tlb_hit(entry->addr_code, addr))) {
        if (!VICTIM_TLB_HIT(addr_code, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, 0, MMU_INST_FETCH, mmu_idx, 0);
            index = tlb_index(env, mmu_idx, addr);
            entry = tlb_entry(env, mmu_idx, addr);
        }
        assert(tlb_hit(entry->addr_code, addr));
    }

    if (unlikely(entry->addr_code & (TLB_RECHECK | TLB_MMIO))) {
        /*
         * Return -1 if we can't translate and execute from an entire
         * page of RAM here, which will cause us to execute by loading
         * and translating one insn at a time, without caching:
         *  - TLB_RECHECK: means the MMU protection covers a smaller range
         *    than a target page, so we must redo the MMU check every insn
         *  - TLB_MMIO: region is not backed by RAM
         */
        return -1;
    }

    CPUIOTLBEntry *ioentry = &env->iotlb[mmu_idx][index];
    hwaddr phys = (ioentry->addr & TARGET_PAGE_MASK) + addr;
    return phys;
}

static void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr)
{
    if (tlb_addr_write(tlb_entry) == (vaddr | TLB_NOTDIRTY)) {
        tlb_entry->addr_write = vaddr;
    }
}

/* Our TLB does not support large pages, so remember the area covered by
   large pages and trigger a full TLB flush if these are invalidated.  */
static void tlb_add_large_page(CPUArchState *env, target_ulong vaddr,
                               target_ulong size, int mmu_idx)
{
    target_ulong mask = ~(size - 1);

    if (env->tlb_flush_addr[mmu_idx] == (target_ulong)-1) {
        env->tlb_flush_addr[mmu_idx] = vaddr & mask;
        env->tlb_flush_mask[mmu_idx] = mask;
        return;
    }

    /* Extend the existing region to include the new page.
       This is a compromise between unnecessary flushes and the cost
       of maintaining a full variable size TLB.  */
    mask &= env->tlb_flush_mask[mmu_idx];
    while (((env->tlb_flush_addr[mmu_idx] ^ vaddr) & mask) != 0) {
        mask <<= 1;
    }

    env->tlb_flush_addr[mmu_idx] &= mask;
    env->tlb_flush_mask[mmu_idx] = mask;
}

static bool tlb_is_dirty_ram(CPUTLBEntry *tlbe)
{
    return (tlb_addr_write(tlbe) & (TLB_INVALID_MASK|TLB_MMIO|TLB_NOTDIRTY)) == 0;
}

void tlb_flush_by_mmuidx(CPUState *cpu, uint16_t idxmap)
{
    CPUArchState *env = cpu->env_ptr;
    unsigned long mmu_idx_bitmask = idxmap;
    int mmu_idx;

    tlb_debug("start\n");

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if (test_bit(mmu_idx, &mmu_idx_bitmask)) {
            tlb_debug("%d\n", mmu_idx);

            memset(env->tlb_table[mmu_idx], -1, sizeof(env->tlb_table[0]));
            memset(env->tlb_v_table[mmu_idx], -1, sizeof(env->tlb_v_table[0]));

            env->tlb_flush_addr[mmu_idx] = (target_ulong)-1;
            env->tlb_flush_mask[mmu_idx] = 0;
        }
    }

    cpu_tb_jmp_cache_clear(cpu);
}

static inline void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr)
{
    if (tlb_hit_page(tlb_entry->addr_read, addr) ||
        tlb_hit_page(tlb_addr_write(tlb_entry), addr) ||
        tlb_hit_page(tlb_entry->addr_code, addr)) {
        memset(tlb_entry, -1, sizeof(*tlb_entry));
    }
}

void tlb_flush_page_by_mmuidx(CPUState *cpu, uint64_t addr, uint16_t idxmap)
{
    CPUArchState *env = cpu->env_ptr;
    unsigned long mmu_idx_bitmap = idxmap;
    int i, page, mmu_idx;

    tlb_debug("addr 0x%lx\n", addr);

    /* Check if we need to flush due to large pages.  */
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        target_ulong flush_mask = env->tlb_flush_mask[mmu_idx];
        target_ulong flush_addr = env->tlb_flush_addr[mmu_idx];
        if ((addr & flush_mask) == flush_addr) {
            tlb_debug("forced full flush ("
                      TARGET_FMT_lx "/" TARGET_FMT_lx ")\n",
                      flush_addr, flush_mask);

            tlb_flush_one_mmu(cpu, mmu_idx);
        }
    }

    addr &= TARGET_PAGE_MASK;
    page = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if (test_bit(mmu_idx, &mmu_idx_bitmap)) {
            tlb_flush_entry(&env->tlb_table[mmu_idx][page], addr);
            /* check whether there are vltb entries that need to be flushed */
            for (i = 0; i < CPU_VTLB_SIZE; i++) {
                tlb_flush_entry(&env->tlb_v_table[mmu_idx][i], addr);
            }
        }
    }

    tb_flush_jmp_cache(cpu, addr);
}

static uint64_t io_readx(CPUArchState *env, CPUIOTLBEntry *iotlbentry,
                         int mmu_idx,
                         target_ulong addr, uintptr_t retaddr,
                         bool recheck, int size)
{
    CPUState *cpu = ENV_GET_CPU(env);
    hwaddr mr_offset;
    MemoryRegionSection *section;
    MemoryRegion *mr;
    uint64_t val;
    MemTxResult r;

    if (recheck) {
        /*
         * This is a TLB_RECHECK access, where the MMU protection
         * covers a smaller range than a target page, and we must
         * repeat the MMU check here. This tlb_fill() call might
         * longjump out if this access should cause a guest exception.
         */
        int index;
        target_ulong tlb_addr;

        tlb_fill(cpu, addr, size, MMU_DATA_LOAD, mmu_idx, retaddr);

        index = tlb_index(env, mmu_idx, addr);
        tlb_addr = env->tlb_table[mmu_idx][index].addr_read;
        if (!(tlb_addr & ~(TARGET_PAGE_MASK | TLB_RECHECK))) {
            /* RAM access */
            uintptr_t haddr = addr + env->tlb_table[mmu_idx][index].addend;

            return ldn_p((void *)haddr, size);
        }
        /* Fall through for handling IO accesses */
    }

    section = iotlb_to_section(cpu, iotlbentry->addr, iotlbentry->attrs);
    mr = section->mr;
    mr_offset = (iotlbentry->addr & TARGET_PAGE_MASK) + addr;
    cpu->mem_io_pc = retaddr;
    if (mr != &cpu->uc->io_mem_rom && mr != &cpu->uc->io_mem_notdirty && !cpu->can_do_io) {
        cpu_io_recompile(cpu, retaddr);
    }

    cpu->mem_io_vaddr = addr;
    r = memory_region_dispatch_read(mr, mr_offset,
                                    &val, size, iotlbentry->attrs);
    if (r != MEMTX_OK) {
        hwaddr physaddr = mr_offset +
            section->offset_within_address_space -
            section->offset_within_region;

        cpu_transaction_failed(cpu, physaddr, addr, size, MMU_DATA_LOAD,
                               mmu_idx, iotlbentry->attrs, r, retaddr);
    }
    return val;
}

static void io_writex(CPUArchState *env, CPUIOTLBEntry *iotlbentry,
                      int mmu_idx,
                      uint64_t val, target_ulong addr,
                      uintptr_t retaddr, bool recheck, int size)
{
    CPUState *cpu = ENV_GET_CPU(env);
    hwaddr mr_offset;
    MemoryRegionSection *section;
    MemoryRegion *mr;
    MemTxResult r;

    if (recheck) {
        /*
         * This is a TLB_RECHECK access, where the MMU protection
         * covers a smaller range than a target page, and we must
         * repeat the MMU check here. This tlb_fill() call might
         * longjump out if this access should cause a guest exception.
         */
        int index;
        target_ulong tlb_addr;

        tlb_fill(cpu, addr, size, MMU_DATA_STORE, mmu_idx, retaddr);

        index = tlb_index(env, mmu_idx, addr);
        tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
        if (!(tlb_addr & ~(TARGET_PAGE_MASK | TLB_RECHECK))) {
            /* RAM access */
            uintptr_t haddr = addr + env->tlb_table[mmu_idx][index].addend;

            stn_p((void *)haddr, size, val);
            return;
        }
        /* Fall through for handling IO accesses */
    }

    section = iotlb_to_section(cpu, iotlbentry->addr, iotlbentry->attrs);
    mr = section->mr;
    mr_offset = (iotlbentry->addr & TARGET_PAGE_MASK) + addr;
    if (mr != &cpu->uc->io_mem_rom && mr != &cpu->uc->io_mem_notdirty && mr != &cpu->uc->io_mem_watch && !cpu->can_do_io) {
        cpu_io_recompile(cpu, retaddr);
    }

    cpu->mem_io_vaddr = addr;
    cpu->mem_io_pc = retaddr;
    r = memory_region_dispatch_write(mr, mr_offset,
                                     val, size, iotlbentry->attrs);
    if (r != MEMTX_OK) {
        hwaddr physaddr = mr_offset +
            section->offset_within_address_space -
            section->offset_within_region;

        cpu_transaction_failed(cpu, physaddr, addr, size, MMU_DATA_STORE,
                               mmu_idx, iotlbentry->attrs, r, retaddr);
    } else {
        int index;

        hwaddr physaddr = mr_offset +
            section->offset_within_address_space -
            section->offset_within_region;

        tb_invalidate_phys_page_fast(cpu->uc, physaddr, size);

        // jhw: mark the page dirty by clearing the not-dirty bit; this also
        // re-enables direct memory access from generated code for this page.
        index = tlb_index(env, mmu_idx, addr);
        env->tlb_table[mmu_idx][index].addr_write &= ~TLB_NOTDIRTY;
    }
}

/* Probe for whether the specified guest write access is permitted.
 * If it is not permitted then an exception will be taken in the same
 * way as if this were a real write access (and we will not return).
 * Otherwise the function will return, and there will be a valid
 * entry in the TLB for this access.
 */
void probe_write(CPUArchState *env, target_ulong addr, int size, int mmu_idx,
                 uintptr_t retaddr)
{
    uintptr_t index = tlb_index(env, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(env, mmu_idx, addr);

    if (!tlb_hit(tlb_addr_write(entry), addr)) {
        /* TLB entry is for a different page */
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, size, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }
    }
}

/* Probe for a read-modify-write atomic operation.  Do not allow unaligned
 * operations, or io operations to proceed.  Return the host address.  */
static void *atomic_mmu_lookup(CPUArchState *env, target_ulong addr,
                               TCGMemOpIdx oi, uintptr_t retaddr)
{
    size_t mmu_idx = get_mmuidx(oi);
    uintptr_t index = tlb_index(env, mmu_idx, addr);
    CPUTLBEntry *tlbe = tlb_entry(env, mmu_idx, addr);
    target_ulong tlb_addr = tlb_addr_write(tlbe);
    TCGMemOp mop = get_memop(oi);
    int a_bits = get_alignment_bits(mop);
    int s_bits = mop & MO_SIZE;

    /* Adjust the given return address.  */
    retaddr -= GETPC_ADJ;

    /* Enforce guest required alignment.  */
    if (unlikely(a_bits > 0 && (addr & ((1 << a_bits) - 1)))) {
        /* ??? Maybe indicate atomic op to cpu_unaligned_access */
        cpu_unaligned_access(ENV_GET_CPU(env), addr, MMU_DATA_STORE,
                             mmu_idx, retaddr);
    }

    /* Enforce qemu required alignment.  */
    if (unlikely(addr & ((1 << s_bits) - 1))) {
        /* We get here if guest alignment was not requested,
           or was not enforced by cpu_unaligned_access above.
           We might widen the access and emulate, but for now
           mark an exception and exit the cpu loop.  */
        //goto stop_the_world;
        return NULL;
    }

    /* Check TLB entry and enforce page permissions.  */
    if (!tlb_hit(tlb_addr, addr)) {
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, 1 << s_bits, MMU_DATA_STORE,
                     mmu_idx, retaddr);
            index = tlb_index(env, mmu_idx, addr);
            tlbe = tlb_entry(env, mmu_idx, addr);
        }
        tlb_addr = tlb_addr_write(tlbe);
    }

    /* Check notdirty */
    if (unlikely(tlb_addr & TLB_NOTDIRTY)) {
        tlb_set_dirty(ENV_GET_CPU(env), addr);
        tlb_addr = tlb_addr & ~TLB_NOTDIRTY;
    }

    /* Notice an IO access or a needs-MMU-lookup access */
    if (unlikely(tlb_addr & (TLB_MMIO | TLB_RECHECK))) {
        /* There's really nothing that can be done to
           support this apart from stop-the-world.  */
        //goto stop_the_world;
        return NULL;
    }

    /* Let the guest notice RMW on a write-only page.  */
    if (unlikely(tlbe->addr_read != tlb_addr)) {
        tlb_fill(ENV_GET_CPU(env), addr, 1 << s_bits, MMU_DATA_LOAD,
                 mmu_idx, retaddr);
        /* Since we don't support reads and writes to different addresses,
           and we do have the proper page loaded for write, this shouldn't
           ever return.  But just in case, handle via stop-the-world.  */
        //goto stop_the_world;
        return NULL;
    }

    return (void *)((uintptr_t)addr + tlbe->addend);

#ifdef JHW
stop_the_world:
    cpu_loop_exit_atomic(ENV_GET_CPU(env), retaddr);
#endif
}

#ifdef JHW
static void dmi_invalidate_page(CPUState *cpu, target_ulong page_addr)
{
    CPUArchState *env = cpu->env_ptr;
    int mmu_idx, idx;

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        for (idx = 0; idx < CPU_TLB_SIZE; idx++) {
            CPUTLBEntry* entry = &env->tlb_table[mmu_idx][idx];
            if (tlb_hit_page(entry->addr_code, page_addr) ||
                tlb_hit_page(entry->addr_read, page_addr) ||
                tlb_hit_page(entry->addr_write, page_addr)) {
                entry->addr_read  |= TLB_MMIO;
                entry->addr_write |= TLB_MMIO;
                entry->addr_code  |= TLB_MMIO;
                entry->addend = 0;
            }
        }
    }

    /* check whether there are entries that need to be flushed in the vtlb */
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        for (idx = 0; idx < CPU_VTLB_SIZE; idx++) {
            CPUTLBEntry* entry = &env->tlb_v_table[mmu_idx][idx];
            if (tlb_hit_page(entry->addr_code, page_addr) ||
                tlb_hit_page(entry->addr_read, page_addr) ||
                tlb_hit_page(entry->addr_write, page_addr)) {
                entry->addr_read  |= TLB_MMIO;
                entry->addr_write |= TLB_MMIO;
                entry->addr_code  |= TLB_MMIO;
                entry->addend = 0;
            }
        }
    }

    // JHW todo: check if this is really enough
    tb_flush_jmp_cache(cpu, page_addr);
}
#endif

static target_ulong lookup_virt_addr(CPUIOTLBEntry* iotlbe);
static target_ulong lookup_virt_addr(CPUIOTLBEntry* iotlbe) {
    CPUTLBEntry* tlbe = iotlbe->p2v;
    if (iotlbe->phys == -1 || tlbe == NULL)
        return -1;

    if (tlbe->addr_read != -1)
        return tlbe->addr_read & TARGET_PAGE_MASK;
    if (tlbe->addr_write != -1)
        return tlbe->addr_write & TARGET_PAGE_MASK;
    if (tlbe->addr_code != -1)
        return tlbe->addr_code & TARGET_PAGE_MASK;

    return -1;
}

void dmi_invalidate(CPUState *cpu, uint64_t start, uint64_t end) {
    CPUArchState *env = cpu->env_ptr;
    int mmu_idx, idx;

    if (start == 0 && end == ~0ull) {
        tlb_flush(cpu);
        return;
    }

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        for (idx = 0; idx < CPU_TLB_SIZE; idx++) {
            CPUIOTLBEntry* entry = &env->iotlb[mmu_idx][idx];
            target_ulong vaddr = lookup_virt_addr(entry);
            if (entry->phys >= start && entry->phys < end && vaddr != -1)
                tlb_flush_page(cpu, vaddr);
        }

        for (idx = 0; idx < CPU_VTLB_SIZE; idx++) {
            CPUIOTLBEntry* entry = &env->iotlb_v[mmu_idx][idx];
            target_ulong vaddr = lookup_virt_addr(entry);
            if (entry->phys >= start && entry->phys < end && vaddr != -1)
                tlb_flush_page(cpu, vaddr);
        }
    }
}

#ifdef TARGET_WORDS_BIGENDIAN
# define TGT_BE(X)  (X)
# define TGT_LE(X)  BSWAP(X)
#else
# define TGT_BE(X)  BSWAP(X)
# define TGT_LE(X)  (X)
#endif

#define MMUSUFFIX _mmu

#define DATA_SIZE 1
#include "softmmu_template.h"

#define DATA_SIZE 2
#include "softmmu_template.h"

#define DATA_SIZE 4
#include "softmmu_template.h"

#define DATA_SIZE 8
#include "softmmu_template.h"

/* First set of helpers allows passing in of OI and RETADDR.  This makes
   them callable from other helpers.  */

#define EXTRA_ARGS     , TCGMemOpIdx oi, uintptr_t retaddr
#define ATOMIC_NAME(X) \
    HELPER(glue(glue(glue(atomic_ ## X, SUFFIX), END), _mmu))
#define ATOMIC_MMU_LOOKUP  atomic_mmu_lookup(env, addr, oi, retaddr)
#define ATOMIC_MMU_CLEANUP do { } while (0)

#define DATA_SIZE 1
#include "atomic_template.h"

#define DATA_SIZE 2
#include "atomic_template.h"

#define DATA_SIZE 4
#include "atomic_template.h"

#ifdef CONFIG_ATOMIC64
#define DATA_SIZE 8
#include "atomic_template.h"
#endif

#if HAVE_CMPXCHG128 || HAVE_ATOMIC128
#define DATA_SIZE 16
#include "atomic_template.h"
#endif

/* Second set of helpers are directly callable from TCG as helpers.  */

#undef EXTRA_ARGS
#undef ATOMIC_NAME
#undef ATOMIC_MMU_LOOKUP
#define EXTRA_ARGS         , TCGMemOpIdx oi
#define ATOMIC_NAME(X)     HELPER(glue(glue(atomic_ ## X, SUFFIX), END))
#define ATOMIC_MMU_LOOKUP  atomic_mmu_lookup(env, addr, oi, GETPC())

#define DATA_SIZE 1
#include "atomic_template.h"

#define DATA_SIZE 2
#include "atomic_template.h"

#define DATA_SIZE 4
#include "atomic_template.h"

#ifdef CONFIG_ATOMIC64
#define DATA_SIZE 8
#include "atomic_template.h"
#endif

/* Code access functions.  */

#undef MMUSUFFIX
#define MMUSUFFIX _cmmu
#undef GETPC
#define GETPC() ((uintptr_t)0)
#define SOFTMMU_CODE_ACCESS

#define DATA_SIZE 1
#include "softmmu_template.h"

#define DATA_SIZE 2
#include "softmmu_template.h"

#define DATA_SIZE 4
#include "softmmu_template.h"

#define DATA_SIZE 8
#include "softmmu_template.h"
