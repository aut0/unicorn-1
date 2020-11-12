/* By Dang Hoang Vu <dang.hvu -at- gmail.com>, 2015 */

#ifndef UC_QEMU_H
#define UC_QEMU_H

struct uc_struct;

#define OPC_BUF_SIZE 640

#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "exec/cpu-common.h"
#include "exec/memory.h"

#include "qemu/thread.h"
#include "include/qom/cpu.h"

#include "vl.h"

// This struct was originally from qemu/include/exec/cpu-all.h
// Temporarily moved here since there is circular inclusion.

typedef struct {
    MemoryRegion *mr;
    void *buffer;
    hwaddr addr;
    hwaddr len;
    bool in_use;
} BounceBuffer;

// SNPS added
typedef struct uc_mmio_region {
    void                  *user_data;
    void                  *callback;
    MemoryRegion          *region;
    struct uc_mmio_region *next;
    struct uc_mmio_region *prev;
} uc_mmio_region_t;

#endif
