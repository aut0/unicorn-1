/*
 *  Physical memory access templates
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *  Copyright (c) 2015 Linaro, Inc.
 *  Copyright (c) 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of t        ptr = MAP_RAM(mr, addr1);
he GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* warning: addr must be aligned */
static inline uint32_t glue(address_space_ldl_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result,
    enum device_endian endian)
{
    uint8_t *ptr;
    uint64_t val;
    MemoryRegion *mr;
    hwaddr l = 4;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, false);
    if (l < 4 || !memory_access_is_direct(mr, false)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);

        /* I/O case */
        r = memory_region_dispatch_read(mr, addr1, &val,
                                        MO_32 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = ldl_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = ldl_be_p(ptr);
            break;
        default:
            val = ldl_p(ptr);
            break;
        }
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
    return val;
}

uint32_t glue(address_space_ldl, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldl_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_NATIVE_ENDIAN);
}

uint32_t glue(address_space_ldl_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldl_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_LITTLE_ENDIAN);
}

uint32_t glue(address_space_ldl_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldl_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_BIG_ENDIAN);
}

uint32_t glue(ldl_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldl, SUFFIX)(ARG1, addr,
                                           MEMTXATTRS_UNSPECIFIED, NULL);
}

uint32_t glue(ldl_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldl_le, SUFFIX)(ARG1, addr,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
}

uint32_t glue(ldl_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldl_be, SUFFIX)(ARG1, addr,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
}

/* warning: addr must be aligned */
static inline uint64_t glue(address_space_ldq_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result,
    enum device_endian endian)
{
    uint8_t *ptr;
    uint64_t val;
    MemoryRegion *mr;
    hwaddr l = 8;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, false);
    if (l < 8 || !memory_access_is_direct(mr, false)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);

        /* I/O case */
        r = memory_region_dispatch_read(mr, addr1, &val,
                                        MO_64 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = ldq_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = ldq_be_p(ptr);
            break;
        default:
            val = ldq_p(ptr);
            break;
        }
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
    return val;
}

uint64_t glue(address_space_ldq, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldq_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_NATIVE_ENDIAN);
}

uint64_t glue(address_space_ldq_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldq_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_LITTLE_ENDIAN);
}

uint64_t glue(address_space_ldq_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_ldq_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                    DEVICE_BIG_ENDIAN);
}

uint64_t glue(ldq_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldq, SUFFIX)(ARG1, addr,
                                           MEMTXATTRS_UNSPECIFIED, NULL);
}

uint64_t glue(ldq_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldq_le, SUFFIX)(ARG1, addr,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
}

uint64_t glue(ldq_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldq_be, SUFFIX)(ARG1, addr,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
}

uint32_t glue(address_space_ldub, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    uint8_t *ptr;
    uint64_t val;
    MemoryRegion *mr;
    hwaddr l = 1;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, false);
    if (!memory_access_is_direct(mr, false)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);

        /* I/O case */
        r = memory_region_dispatch_read(mr, addr1, &val, MO_8, attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        val = ldub_p(ptr);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
    return val;
}

uint32_t glue(ldub_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_ldub, SUFFIX)(ARG1, addr,
                                            MEMTXATTRS_UNSPECIFIED, NULL);
}

/* warning: addr must be aligned */
static inline uint32_t glue(address_space_lduw_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result,
    enum device_endian endian)
{
    uint8_t *ptr;
    uint64_t val;
    MemoryRegion *mr;
    hwaddr l = 2;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, false);
    if (l < 2 || !memory_access_is_direct(mr, false)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);

        /* I/O case */
        r = memory_region_dispatch_read(mr, addr1, &val,
                                        MO_16 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = lduw_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = lduw_be_p(ptr);
            break;
        default:
            val = lduw_p(ptr);
            break;
        }
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
    return val;
}

uint32_t glue(address_space_lduw, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_lduw_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                     DEVICE_NATIVE_ENDIAN);
}

uint32_t glue(address_space_lduw_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_lduw_internal, SUFFIX)(ARG1, addr, attrs, result,
                                                     DEVICE_LITTLE_ENDIAN);
}

uint32_t glue(address_space_lduw_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, MemTxAttrs attrs, MemTxResult *result)
{
    return glue(address_space_lduw_internal, SUFFIX)(ARG1, addr, attrs, result,
                                       DEVICE_BIG_ENDIAN);
}

uint32_t glue(lduw_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_lduw, SUFFIX)(ARG1, addr,
                                            MEMTXATTRS_UNSPECIFIED, NULL);
}

uint32_t glue(lduw_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_lduw_le, SUFFIX)(ARG1, addr,
                                               MEMTXATTRS_UNSPECIFIED, NULL);
}

uint32_t glue(lduw_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr)
{
    return glue(address_space_lduw_be, SUFFIX)(ARG1, addr,
                                               MEMTXATTRS_UNSPECIFIED, NULL);
}

/* warning: addr must be aligned. The ram page is not masked as dirty
   and the code inside is not invalidated. It is useful if the dirty
   bits are used to track modified PTEs */
void glue(address_space_stl_notdirty, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    uint8_t *ptr;
    MemoryRegion *mr;
    hwaddr l = 4;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, true);
    if (l < 4 || !memory_access_is_direct(mr, true)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);

        r = memory_region_dispatch_write(mr, addr1, val, MO_32, attrs);
    } else {
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        stl_p(ptr, val);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
}

void glue(stl_phys_notdirty, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stl_notdirty, SUFFIX)(ARG1, addr, val,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
}

/* warning: addr must be aligned */
static inline void glue(address_space_stl_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs,
    MemTxResult *result, enum device_endian endian)
{
    uint8_t *ptr;
    MemoryRegion *mr;
    hwaddr l = 4;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, true);
    if (l < 4 || !memory_access_is_direct(mr, true)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);
        r = memory_region_dispatch_write(mr, addr1, val,
                                         MO_32 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            stl_le_p(ptr, val);
            break;
        case DEVICE_BIG_ENDIAN:
            stl_be_p(ptr, val);
            break;
        default:
            stl_p(ptr, val);
            break;
        }
        INVALIDATE(mr, addr1, 4);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
}

void glue(address_space_stl, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stl_internal, SUFFIX)(ARG1, addr, val, attrs,
                                             result, DEVICE_NATIVE_ENDIAN);
}

void glue(address_space_stl_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stl_internal, SUFFIX)(ARG1, addr, val, attrs,
                                             result, DEVICE_LITTLE_ENDIAN);
}

void glue(address_space_stl_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stl_internal, SUFFIX)(ARG1, addr, val, attrs,
                                             result, DEVICE_BIG_ENDIAN);
}

void glue(stl_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stl, SUFFIX)(ARG1, addr, val,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stl_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stl_le, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stl_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stl_be, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(address_space_stb, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    uint8_t *ptr;
    MemoryRegion *mr;
    hwaddr l = 1;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, true);
    if (!memory_access_is_direct(mr, true)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);
        r = memory_region_dispatch_write(mr, addr1, val, MO_8, attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        stb_p(ptr, val);
        INVALIDATE(mr, addr1, 1);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
}

void glue(stb_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stb, SUFFIX)(ARG1, addr, val,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
}

/* warning: addr must be aligned */
static inline void glue(address_space_stw_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs,
    MemTxResult *result, enum device_endian endian)
{
    uint8_t *ptr;
    MemoryRegion *mr;
    hwaddr l = 2;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, true);
    if (l < 2 || !memory_access_is_direct(mr, true)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);
        r = memory_region_dispatch_write(mr, addr1, val,
                                         MO_16 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            stw_le_p(ptr, val);
            break;
        case DEVICE_BIG_ENDIAN:
            stw_be_p(ptr, val);
            break;
        default:
            stw_p(ptr, val);
            break;
        }
        INVALIDATE(mr, addr1, 2);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
}

void glue(address_space_stw, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stw_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                                             DEVICE_NATIVE_ENDIAN);
}

void glue(address_space_stw_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stw_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                                             DEVICE_LITTLE_ENDIAN);
}

void glue(address_space_stw_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint32_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stw_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                               DEVICE_BIG_ENDIAN);
}

void glue(stw_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stw, SUFFIX)(ARG1, addr, val,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stw_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stw_le, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stw_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint32_t val)
{
    glue(address_space_stw_be, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

static void glue(address_space_stq_internal, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint64_t val, MemTxAttrs attrs,
    MemTxResult *result, enum device_endian endian)
{
    uint8_t *ptr;
    MemoryRegion *mr;
    hwaddr l = 8;
    hwaddr addr1;
    MemTxResult r;
    // Unicorn: commented out
    //bool release_lock = false;

    // Unicorn: commented out
    //RCU_READ_LOCK();
    mr = TRANSLATE(addr, &addr1, &l, true);
    if (l < 8 || !memory_access_is_direct(mr, true)) {
        // Unicorn: commented out
        //release_lock |= prepare_mmio_access(mr);
        r = memory_region_dispatch_write(mr, addr1, val,
                                         MO_64 | devend_memop(endian), attrs);
    } else {
        /* RAM case */
        ptr = qemu_map_ram_ptr(mr->uc, mr->ram_block, addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            stq_le_p(ptr, val);
            break;
        case DEVICE_BIG_ENDIAN:
            stq_be_p(ptr, val);
            break;
        default:
            stq_p(ptr, val);
            break;
        }
        INVALIDATE(mr, addr1, 8);
        r = MEMTX_OK;
    }
    if (result) {
        *result = r;
    }
    // Unicorn: If'd out
#if 0
    if (release_lock) {
        qemu_mutex_unlock_iothread();
    }
    RCU_READ_UNLOCK();
#endif
}

void glue(address_space_stq, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint64_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stq_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                                             DEVICE_NATIVE_ENDIAN);
}

void glue(address_space_stq_le, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint64_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stq_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                                             DEVICE_LITTLE_ENDIAN);
}

void glue(address_space_stq_be, SUFFIX)(ARG1_DECL,
    hwaddr addr, uint64_t val, MemTxAttrs attrs, MemTxResult *result)
{
    glue(address_space_stq_internal, SUFFIX)(ARG1, addr, val, attrs, result,
                                             DEVICE_BIG_ENDIAN);
}

void glue(stq_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint64_t val)
{
    glue(address_space_stq, SUFFIX)(ARG1, addr, val,
                                    MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stq_le_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint64_t val)
{
    glue(address_space_stq_le, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

void glue(stq_be_phys, SUFFIX)(ARG1_DECL, hwaddr addr, uint64_t val)
{
    glue(address_space_stq_be, SUFFIX)(ARG1, addr, val,
                                       MEMTXATTRS_UNSPECIFIED, NULL);
}

#undef ARG1_DECL
#undef ARG1
#undef SUFFIX
#undef TRANSLATE
#undef INVALIDATE
#undef RCU_READ_LOCK
#undef RCU_READ_UNLOCK
