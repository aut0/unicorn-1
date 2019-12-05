/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * splitted out ioport related stuffs from vl.c.
 */

/* Modified for Unicorn Engine by Nguyen Anh Quynh, 2015 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "exec/ioport.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

#include "uc_priv.h"

typedef struct MemoryRegionPortioList {
    MemoryRegion mr;
    void *portio_opaque;
    MemoryRegionPortio ports[];
} MemoryRegionPortioList;

static uint64_t unassigned_io_read(struct uc_struct* uc, void *opaque, hwaddr addr, unsigned size)
{
    return 0-1ULL;
}

static void unassigned_io_write(struct uc_struct* uc, void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static MemTxResult unassigned_io_read_with_attrs(struct uc_struct* uc, void *opaque, hwaddr addr,
                                                 uint64_t *data, unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static MemTxResult unassigned_write_with_attrs(struct uc_struct* uc, void *opaque,
                                               hwaddr addr, uint64_t data, unsigned size,
                                               MemTxAttrs attrs)
{
    return MEMTX_OK;
}


const MemoryRegionOps unassigned_io_ops = {
    unassigned_io_read,
    unassigned_io_write,
    unassigned_io_read_with_attrs,
    unassigned_write_with_attrs,
    DEVICE_NATIVE_ENDIAN,
};
