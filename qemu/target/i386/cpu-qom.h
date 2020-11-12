/*
 * QEMU x86 CPU
 *
 * Copyright (c) 2012 SUSE LINUX Products GmbH
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
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */
#ifndef QEMU_I386_CPU_QOM_H
#define QEMU_I386_CPU_QOM_H

#include "qom/cpu.h"
#include "cpu.h"
#include "qapi/error.h"

#ifdef TARGET_X86_64
#define TYPE_X86_CPU "x86_64-cpu"
#else
#define TYPE_X86_CPU "i386-cpu"
#endif

#define X86_CPU_CLASS(uc, klass) \
    OBJECT_CLASS_CHECK(uc, X86CPUClass, (klass), TYPE_X86_CPU)
#define X86_CPU(uc, obj) ((X86CPU *)obj)
#define X86_CPU_GET_CLASS(uc, obj) \
    OBJECT_GET_CLASS(uc, X86CPUClass, (obj), TYPE_X86_CPU)

typedef struct X86CPUModel X86CPUModel;

/**
 * X86CPUClass:
 * @cpu_def: CPU model definition
 * @kvm_required: Whether CPU model requires KVM to be enabled.
 * @parent_realize: The parent class' realize handler.
 * @parent_reset: The parent class' reset handler.
 *
 * An x86 CPU model or family.
 */
typedef struct X86CPUClass {
    /*< private >*/
    CPUClass parent_class;
    /*< public >*/

    /* Should be eventually replaced by subclass-specific property defaults. */
    X86CPUModel *model;

    bool kvm_required;

    /* Optional description of CPU model.
     * If unavailable, cpu_def->model_id is used */
    const char *model_description;

    DeviceRealize parent_realize;
    void (*parent_reset)(CPUState *cpu);
} X86CPUClass;

typedef struct X86CPU X86CPU;

#endif
