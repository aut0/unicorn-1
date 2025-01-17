/*
 *  ARM translation
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *  Copyright (c) 2005-2007 CodeSourcery
 *  Copyright (c) 2007 OpenedHand, Ltd.
 *  Copyright (c) 2015 Nguyen Anh Quynh (Unicorn engine)
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "unicorn/platform.h"

#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "tcg-op.h"
#include "tcg-op-gvec.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "arm_ldst.h"
#include "exec/semihost.h"

#include "exec/helper-proto.h"
#include "exec/helper-gen.h"

#include "exec/gen-icount.h"

#define ENABLE_ARCH_4T    arm_dc_feature(s, ARM_FEATURE_V4T)
#define ENABLE_ARCH_5     arm_dc_feature(s, ARM_FEATURE_V5)
/* currently all emulated v5 cores are also v5TE, so don't bother */
#define ENABLE_ARCH_5TE   arm_dc_feature(s, ARM_FEATURE_V5)
#define ENABLE_ARCH_5J    dc_isar_feature(aa32_jazelle, s)
#define ENABLE_ARCH_6     arm_dc_feature(s, ARM_FEATURE_V6)
#define ENABLE_ARCH_6K    arm_dc_feature(s, ARM_FEATURE_V6K)
#define ENABLE_ARCH_6T2   arm_dc_feature(s, ARM_FEATURE_THUMB2)
#define ENABLE_ARCH_7     arm_dc_feature(s, ARM_FEATURE_V7)
#define ENABLE_ARCH_8     arm_dc_feature(s, ARM_FEATURE_V8)

#define ARCH(x) do { if (!ENABLE_ARCH_##x) goto illegal_op; } while(0)

#include "translate.h"

#if defined(CONFIG_USER_ONLY)
#define IS_USER(s) 1
#else
#define IS_USER(s) (s->user)
#endif

static const char *regnames[] =
    { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
      "r8", "r9", "r10", "r11", "r12", "r13", "r14", "pc" };

/* Function prototypes for gen_ functions calling Neon helpers.  */
typedef void NeonGenThreeOpEnvFn(TCGContext *, TCGv_i32, TCGv_env, TCGv_i32,
                                 TCGv_i32, TCGv_i32);
/* Function prototypes for gen_ functions for fix point conversions */
typedef void VFPGenFixPointFn(TCGContext *, TCGv_i32, TCGv_i32, TCGv_i32, TCGv_ptr);

/* initialize TCG globals.  */
void arm_translate_init(struct uc_struct *uc)
{
    int i;
    TCGContext *tcg_ctx = uc->tcg_ctx;

    tcg_ctx->cpu_env = tcg_global_reg_new_ptr(uc->tcg_ctx, TCG_AREG0, "env");
    tcg_ctx->tcg_env = tcg_ctx->cpu_env;

    for (i = 0; i < 16; i++) {
        tcg_ctx->cpu_R[i] = tcg_global_mem_new_i32(uc->tcg_ctx, tcg_ctx->cpu_env,
                                          offsetof(CPUARMState, regs[i]),
                                          regnames[i]);
    }
    tcg_ctx->cpu_CF = tcg_global_mem_new_i32(uc->tcg_ctx, tcg_ctx->cpu_env, offsetof(CPUARMState, CF), "CF");
    tcg_ctx->cpu_NF = tcg_global_mem_new_i32(uc->tcg_ctx, tcg_ctx->cpu_env, offsetof(CPUARMState, NF), "NF");
    tcg_ctx->cpu_VF = tcg_global_mem_new_i32(uc->tcg_ctx, tcg_ctx->cpu_env, offsetof(CPUARMState, VF), "VF");
    tcg_ctx->cpu_ZF = tcg_global_mem_new_i32(uc->tcg_ctx, tcg_ctx->cpu_env, offsetof(CPUARMState, ZF), "ZF");

    tcg_ctx->cpu_exclusive_addr = tcg_global_mem_new_i64(uc->tcg_ctx, tcg_ctx->cpu_env,
        offsetof(CPUARMState, exclusive_addr), "exclusive_addr");
    tcg_ctx->cpu_exclusive_val = tcg_global_mem_new_i64(uc->tcg_ctx, tcg_ctx->cpu_env,
        offsetof(CPUARMState, exclusive_val), "exclusive_val");

    a64_translate_init(uc);
}

/* Flags for the disas_set_da_iss info argument:
 * lower bits hold the Rt register number, higher bits are flags.
 */
typedef enum ISSInfo {
    ISSNone = 0,
    ISSRegMask = 0x1f,
    ISSInvalid = (1 << 5),
    ISSIsAcqRel = (1 << 6),
    ISSIsWrite = (1 << 7),
    ISSIs16Bit = (1 << 8),
} ISSInfo;

/* Save the syndrome information for a Data Abort */
static void disas_set_da_iss(DisasContext *s, MemOp memop, ISSInfo issinfo)
{
    uint32_t syn;
    int sas = memop & MO_SIZE;
    bool sse = memop & MO_SIGN;
    bool is_acqrel = issinfo & ISSIsAcqRel;
    bool is_write = issinfo & ISSIsWrite;
    bool is_16bit = issinfo & ISSIs16Bit;
    int srt = issinfo & ISSRegMask;

    if (issinfo & ISSInvalid) {
        /* Some callsites want to conditionally provide ISS info,
         * eg "only if this was not a writeback"
         */
        return;
    }

    if (srt == 15) {
        /* For AArch32, insns where the src/dest is R15 never generate
         * ISS information. Catching that here saves checking at all
         * the call sites.
         */
        return;
    }

    syn = syn_data_abort_with_iss(0, sas, sse, srt, 0, is_acqrel,
                                  0, 0, 0, is_write, 0, is_16bit);
    disas_set_insn_syndrome(s, syn);
}

static inline int get_a32_user_mem_index(DisasContext *s)
{
    /* Return the core mmu_idx to use for A32/T32 "unprivileged load/store"
     * insns:
     *  if PL2, UNPREDICTABLE (we choose to implement as if PL0)
     *  otherwise, access as if at PL0.
     */
    switch (s->mmu_idx) {
    case ARMMMUIdx_E2:        /* this one is UNPREDICTABLE */
    case ARMMMUIdx_E10_0:
    case ARMMMUIdx_E10_1:
    case ARMMMUIdx_E10_1_PAN:
        return arm_to_core_mmu_idx(ARMMMUIdx_E10_0);
    case ARMMMUIdx_SE3:
    case ARMMMUIdx_SE10_0:
    case ARMMMUIdx_SE10_1:
    case ARMMMUIdx_SE10_1_PAN:
        return arm_to_core_mmu_idx(ARMMMUIdx_SE10_0);
    case ARMMMUIdx_MUser:
    case ARMMMUIdx_MPriv:
        return arm_to_core_mmu_idx(ARMMMUIdx_MUser);
    case ARMMMUIdx_MUserNegPri:
    case ARMMMUIdx_MPrivNegPri:
        return arm_to_core_mmu_idx(ARMMMUIdx_MUserNegPri);
    case ARMMMUIdx_MSUser:
    case ARMMMUIdx_MSPriv:
        return arm_to_core_mmu_idx(ARMMMUIdx_MSUser);
    case ARMMMUIdx_MSUserNegPri:
    case ARMMMUIdx_MSPrivNegPri:
        return arm_to_core_mmu_idx(ARMMMUIdx_MSUserNegPri);
    default:
        g_assert_not_reached();
    }
}

static inline TCGv_i32 load_cpu_offset(DisasContext *s, int offset)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, offset);
    return tmp;
}

#define load_cpu_field(s, name) load_cpu_offset(s, offsetof(CPUARMState, name))

static inline void store_cpu_offset(DisasContext *s, TCGv_i32 var, int offset)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    tcg_gen_st_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
    tcg_temp_free_i32(tcg_ctx, var);
}

#define store_cpu_field(s, var, name) \
    store_cpu_offset(s, var, offsetof(CPUARMState, name))

/* The architectural value of PC.  */
static uint32_t read_pc(DisasContext *s)
{
    return s->pc_curr + (s->thumb ? 4 : 8);
}

/* Set a variable to the value of a CPU register.  */
static void load_reg_var(DisasContext *s, TCGv_i32 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (reg == 15) {
        tcg_gen_movi_i32(tcg_ctx, var, read_pc(s));
    } else {
        tcg_gen_mov_i32(tcg_ctx, var, tcg_ctx->cpu_R[reg]);
    }
}

/* Create a new temporary and set it to the value of a CPU register.  */
static inline TCGv_i32 load_reg(DisasContext *s, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    load_reg_var(s, tmp, reg);
    return tmp;
}

/*
 * Create a new temp, REG + OFS, except PC is ALIGN(PC, 4).
 * This is used for load/store for which use of PC implies (literal),
 * or ADD that implies ADR.
 */
static TCGv_i32 add_reg_for_lit(DisasContext *s, int reg, int ofs)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    if (reg == 15) {
        tcg_gen_movi_i32(tcg_ctx, tmp, (read_pc(s) & ~3) + ofs);
    } else {
        tcg_gen_addi_i32(tcg_ctx, tmp, tcg_ctx->cpu_R[reg], ofs);
    }
    return tmp;
}

/* Set a CPU register.  The source must be a temporary and will be
   marked as dead.  */
static void store_reg(DisasContext *s, int reg, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (reg == 15) {
        /* In Thumb mode, we must ignore bit 0.
         * In ARM mode, for ARMv4 and ARMv5, it is UNPREDICTABLE if bits [1:0]
         * are not 0b00, but for ARMv6 and above, we must ignore bits [1:0].
         * We choose to ignore [1:0] in ARM mode for all architecture versions.
         */
        tcg_gen_andi_i32(tcg_ctx, var, var, s->thumb ? ~1 : ~3);
        s->base.is_jmp = DISAS_JUMP;
    }
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[reg], var);
    tcg_temp_free_i32(tcg_ctx, var);
}

/*
 * Variant of store_reg which applies v8M stack-limit checks before updating
 * SP. If the check fails this will result in an exception being taken.
 * We disable the stack checks for CONFIG_USER_ONLY because we have
 * no idea what the stack limits should be in that case.
 * If stack checking is not being done this just acts like store_reg().
 */
static void store_sp_checked(DisasContext *s, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

#ifndef CONFIG_USER_ONLY
    if (s->v8m_stackcheck) {
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, var);
    }
#endif
    store_reg(s, 13, var);
}

/* Value extensions.  */
#define gen_uxtb(var) tcg_gen_ext8u_i32(tcg_ctx, var, var)
#define gen_uxth(var) tcg_gen_ext16u_i32(tcg_ctx, var, var)
#define gen_sxtb(var) tcg_gen_ext8s_i32(tcg_ctx, var, var)
#define gen_sxth(var) tcg_gen_ext16s_i32(tcg_ctx, var, var)

#define gen_sxtb16(var) gen_helper_sxtb16(tcg_ctx, var, var)
#define gen_uxtb16(var) gen_helper_uxtb16(tcg_ctx, var, var)


static inline void gen_set_cpsr(DisasContext *s, TCGv_i32 var, uint32_t mask)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp_mask = tcg_const_i32(tcg_ctx, mask);
    gen_helper_cpsr_write(tcg_ctx, tcg_ctx->cpu_env, var, tmp_mask);
    tcg_temp_free_i32(tcg_ctx, tmp_mask);
}
/* Set NZCV flags from the high 4 bits of var.  */
#define gen_set_nzcv(s, var) gen_set_cpsr(s, var, CPSR_NZCV)

static void gen_exception_internal(DisasContext *s, int excp)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tcg_excp = tcg_const_i32(tcg_ctx, excp);

    assert(excp_is_internal(excp));
    gen_helper_exception_internal(tcg_ctx, tcg_ctx->cpu_env, tcg_excp);
    tcg_temp_free_i32(tcg_ctx, tcg_excp);
}

static void gen_step_complete_exception(DisasContext *s)
{
    /* We just completed step of an insn. Move from Active-not-pending
     * to Active-pending, and then also take the swstep exception.
     * This corresponds to making the (IMPDEF) choice to prioritize
     * swstep exceptions over asynchronous exceptions taken to an exception
     * level where debug is disabled. This choice has the advantage that
     * we do not need to maintain internal state corresponding to the
     * ISV/EX syndrome bits between completion of the step and generation
     * of the exception, and our syndrome information is always correct.
     */
    gen_ss_advance(s);
    gen_swstep_exception(s, 1, s->is_ldex);
    s->base.is_jmp = DISAS_NORETURN;
}

static void gen_singlestep_exception(DisasContext *s)
{
    /* Generate the right kind of exception for singlestep, which is
     * either the architectural singlestep or EXCP_DEBUG for QEMU's
     * gdb singlestepping.
     */
    if (s->ss_active) {
        gen_step_complete_exception(s);
    } else {
        gen_exception_internal(s, EXCP_DEBUG);
    }
}

static inline bool is_singlestepping(DisasContext *s)
{
    /* Return true if we are singlestepping either because of
     * architectural singlestep or QEMU gdbstub singlestep. This does
     * not include the command line '-singlestep' mode which is rather
     * misnamed as it only means "one instruction per TB" and doesn't
     * affect the code we generate.
     */
    return s->base.singlestep_enabled || s->ss_active;
}

static void gen_smul_dual(DisasContext *s, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp1 = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 tmp2 = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ext16s_i32(tcg_ctx, tmp1, a);
    tcg_gen_ext16s_i32(tcg_ctx, tmp2, b);
    tcg_gen_mul_i32(tcg_ctx, tmp1, tmp1, tmp2);
    tcg_temp_free_i32(tcg_ctx, tmp2);
    tcg_gen_sari_i32(tcg_ctx, a, a, 16);
    tcg_gen_sari_i32(tcg_ctx, b, b, 16);
    tcg_gen_mul_i32(tcg_ctx, b, b, a);
    tcg_gen_mov_i32(tcg_ctx, a, tmp1);
    tcg_temp_free_i32(tcg_ctx, tmp1);
}

/* Byteswap each halfword.  */
static void gen_rev16(DisasContext *s, TCGv_i32 dest, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 mask = tcg_const_i32(tcg_ctx, 0x00ff00ff);
    tcg_gen_shri_i32(tcg_ctx, tmp, var, 8);
    tcg_gen_and_i32(tcg_ctx, tmp, tmp, mask);
    tcg_gen_and_i32(tcg_ctx, var, var, mask);
    tcg_gen_shli_i32(tcg_ctx, var, var, 8);
    tcg_gen_or_i32(tcg_ctx, dest, var, tmp);
    tcg_temp_free_i32(tcg_ctx, mask);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

/* Byteswap low halfword and sign extend.  */
static void gen_revsh(DisasContext *s, TCGv_i32 dest, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ext16u_i32(tcg_ctx, var, var);
    tcg_gen_bswap16_i32(tcg_ctx, var, var);
    tcg_gen_ext16s_i32(tcg_ctx, dest, var);
}

/* Swap low and high halfwords.  */
static void gen_swap_half(DisasContext *s, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_rotri_i32(tcg_ctx, var, var, 16);
}

/* Dual 16-bit add.  Result placed in t0 and t1 is marked as dead.
    tmp = (t0 ^ t1) & 0x8000;
    t0 &= ~0x8000;
    t1 &= ~0x8000;
    t0 = (t0 + t1) ^ tmp;
 */

static void gen_add16(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_xor_i32(tcg_ctx, tmp, t0, t1);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0x8000);
    tcg_gen_andi_i32(tcg_ctx, t0, t0, ~0x8000);
    tcg_gen_andi_i32(tcg_ctx, t1, t1, ~0x8000);
    tcg_gen_add_i32(tcg_ctx, t0, t0, t1);
    tcg_gen_xor_i32(tcg_ctx, dest, t0, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

/* Set N and Z flags from var.  */
static inline void gen_logic_CC(DisasContext *s, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_NF, var);
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_ZF, var);
}

/* dest = T0 + T1 + CF. */
static void gen_add_carry(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_add_i32(tcg_ctx, dest, t0, t1);
    tcg_gen_add_i32(tcg_ctx, dest, dest, tcg_ctx->cpu_CF);
}

/* dest = T0 - T1 + CF - 1.  */
static void gen_sub_carry(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_sub_i32(tcg_ctx, dest, t0, t1);
    tcg_gen_add_i32(tcg_ctx, dest, dest, tcg_ctx->cpu_CF);
    tcg_gen_subi_i32(tcg_ctx, dest, dest, 1);
}

/* dest = T0 + T1. Compute C, N, V and Z flags */
static void gen_add_CC(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_movi_i32(tcg_ctx, tmp, 0);
    tcg_gen_add2_i32(tcg_ctx, tcg_ctx->cpu_NF, tcg_ctx->cpu_CF, t0, tmp, t1, tmp);
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_ZF, tcg_ctx->cpu_NF);
    tcg_gen_xor_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF, t0);
    tcg_gen_xor_i32(tcg_ctx, tmp, t0, t1);
    tcg_gen_andc_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_VF, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_gen_mov_i32(tcg_ctx, dest, tcg_ctx->cpu_NF);
}

/* dest = T0 + T1 + CF.  Compute C, N, V and Z flags */
static void gen_adc_CC(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    if (TCG_TARGET_HAS_add2_i32) {
        tcg_gen_movi_i32(tcg_ctx, tmp, 0);
        tcg_gen_add2_i32(tcg_ctx, tcg_ctx->cpu_NF, tcg_ctx->cpu_CF, t0, tmp, tcg_ctx->cpu_CF, tmp);
        tcg_gen_add2_i32(tcg_ctx, tcg_ctx->cpu_NF, tcg_ctx->cpu_CF, tcg_ctx->cpu_NF, tcg_ctx->cpu_CF, t1, tmp);
    } else {
        TCGv_i64 q0 = tcg_temp_new_i64(tcg_ctx);
        TCGv_i64 q1 = tcg_temp_new_i64(tcg_ctx);
        tcg_gen_extu_i32_i64(tcg_ctx, q0, t0);
        tcg_gen_extu_i32_i64(tcg_ctx, q1, t1);
        tcg_gen_add_i64(tcg_ctx, q0, q0, q1);
        tcg_gen_extu_i32_i64(tcg_ctx, q1, tcg_ctx->cpu_CF);
        tcg_gen_add_i64(tcg_ctx, q0, q0, q1);
        tcg_gen_extr_i64_i32(tcg_ctx, tcg_ctx->cpu_NF, tcg_ctx->cpu_CF, q0);
        tcg_temp_free_i64(tcg_ctx, q0);
        tcg_temp_free_i64(tcg_ctx, q1);
    }
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_ZF, tcg_ctx->cpu_NF);
    tcg_gen_xor_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF, t0);
    tcg_gen_xor_i32(tcg_ctx, tmp, t0, t1);
    tcg_gen_andc_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_VF, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_gen_mov_i32(tcg_ctx, dest, tcg_ctx->cpu_NF);
}

/* dest = T0 - T1. Compute C, N, V and Z flags */
static void gen_sub_CC(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    tcg_gen_sub_i32(tcg_ctx, tcg_ctx->cpu_NF, t0, t1);
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_ZF, tcg_ctx->cpu_NF);
    tcg_gen_setcond_i32(tcg_ctx, TCG_COND_GEU, tcg_ctx->cpu_CF, t0, t1);
    tcg_gen_xor_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF, t0);
    tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_xor_i32(tcg_ctx, tmp, t0, t1);
    tcg_gen_and_i32(tcg_ctx, tcg_ctx->cpu_VF, tcg_ctx->cpu_VF, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_gen_mov_i32(tcg_ctx, dest, tcg_ctx->cpu_NF);
}

/* dest = T0 + ~T1 + CF.  Compute C, N, V and Z flags */
static void gen_sbc_CC(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_not_i32(tcg_ctx, tmp, t1);
    gen_adc_CC(s, dest, t0, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
}

#define GEN_SHIFT(name)                                               \
static void gen_##name(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)       \
{                                                                     \
    TCGContext *tcg_ctx = s->uc->tcg_ctx; \
    TCGv_i32 tmp1, tmp2, tmp3;                                        \
    tmp1 = tcg_temp_new_i32(tcg_ctx);                                        \
    tcg_gen_andi_i32(tcg_ctx, tmp1, t1, 0xff);                                 \
    tmp2 = tcg_const_i32(tcg_ctx, 0);                                          \
    tmp3 = tcg_const_i32(tcg_ctx, 0x1f);                                       \
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_GTU, tmp2, tmp1, tmp3, tmp2, t0);    \
    tcg_temp_free_i32(tcg_ctx, tmp3);                                          \
    tcg_gen_andi_i32(tcg_ctx, tmp1, tmp1, 0x1f);                               \
    tcg_gen_##name##_i32(tcg_ctx, dest, tmp2, tmp1);                           \
    tcg_temp_free_i32(tcg_ctx, tmp2);                                          \
    tcg_temp_free_i32(tcg_ctx, tmp1);                                          \
}
GEN_SHIFT(shl)
GEN_SHIFT(shr)
#undef GEN_SHIFT

static void gen_sar(DisasContext *s, TCGv_i32 dest, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp1, tmp2;
    tmp1 = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_andi_i32(tcg_ctx, tmp1, t1, 0xff);
    tmp2 = tcg_const_i32(tcg_ctx, 0x1f);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_GTU, tmp1, tmp1, tmp2, tmp2, tmp1);
    tcg_temp_free_i32(tcg_ctx, tmp2);
    tcg_gen_sar_i32(tcg_ctx, dest, t0, tmp1);
    tcg_temp_free_i32(tcg_ctx, tmp1);
}

static void shifter_out_im(DisasContext *s, TCGv_i32 var, int shift)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_extract_i32(tcg_ctx, tcg_ctx->cpu_CF, var, shift, 1);
}

/* Shift by immediate.  Includes special handling for shift == 0.  */
static inline void gen_arm_shift_im(DisasContext *s, TCGv_i32 var, int shiftop,
                                    int shift, int flags)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (shiftop) {
    case 0: /* LSL */
        if (shift != 0) {
            if (flags)
                shifter_out_im(s, var, 32 - shift);
            tcg_gen_shli_i32(tcg_ctx, var, var, shift);
        }
        break;
    case 1: /* LSR */
        if (shift == 0) {
            if (flags) {
                tcg_gen_shri_i32(tcg_ctx, tcg_ctx->cpu_CF, var, 31);
            }
            tcg_gen_movi_i32(tcg_ctx, var, 0);
        } else {
            if (flags)
                shifter_out_im(s, var, shift - 1);
            tcg_gen_shri_i32(tcg_ctx, var, var, shift);
        }
        break;
    case 2: /* ASR */
        if (shift == 0)
            shift = 32;
        if (flags)
            shifter_out_im(s, var, shift - 1);
        if (shift == 32)
          shift = 31;
        tcg_gen_sari_i32(tcg_ctx, var, var, shift);
        break;
    case 3: /* ROR/RRX */
        if (shift != 0) {
            if (flags)
                shifter_out_im(s, var, shift - 1);
            tcg_gen_rotri_i32(tcg_ctx, var, var, shift); break;
        } else {
            TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_shli_i32(tcg_ctx, tmp, tcg_ctx->cpu_CF, 31);
            if (flags)
                shifter_out_im(s, var, 0);
            tcg_gen_shri_i32(tcg_ctx, var, var, 1);
            tcg_gen_or_i32(tcg_ctx, var, var, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
        }
    }
}

static inline void gen_arm_shift_reg(DisasContext *s, TCGv_i32 var, int shiftop,
                                     TCGv_i32 shift, int flags)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (flags) {
        switch (shiftop) {
        case 0: gen_helper_shl_cc(tcg_ctx, var, tcg_ctx->cpu_env, var, shift); break;
        case 1: gen_helper_shr_cc(tcg_ctx, var, tcg_ctx->cpu_env, var, shift); break;
        case 2: gen_helper_sar_cc(tcg_ctx, var, tcg_ctx->cpu_env, var, shift); break;
        case 3: gen_helper_ror_cc(tcg_ctx, var, tcg_ctx->cpu_env, var, shift); break;
        }
    } else {
        switch (shiftop) {
        case 0:
            gen_shl(s, var, var, shift);
            break;
        case 1:
            gen_shr(s, var, var, shift);
            break;
        case 2:
            gen_sar(s, var, var, shift);
            break;
        case 3: tcg_gen_andi_i32(tcg_ctx, shift, shift, 0x1f);
                tcg_gen_rotr_i32(tcg_ctx, var, var, shift); break;
        }
    }
    tcg_temp_free_i32(tcg_ctx, shift);
}

/*
 * Generate a conditional based on ARM condition code cc.
 * This is common between ARM and Aarch64 targets.
 */
void arm_test_cc(DisasContext *s, DisasCompare *cmp, int cc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 value;
    TCGCond cond;
    bool global = true;

    switch (cc) {
    case 0: /* eq: Z */
    case 1: /* ne: !Z */
        cond = TCG_COND_EQ;
        value = tcg_ctx->cpu_ZF;
        break;

    case 2: /* cs: C */
    case 3: /* cc: !C */
        cond = TCG_COND_NE;
        value = tcg_ctx->cpu_CF;
        break;

    case 4: /* mi: N */
    case 5: /* pl: !N */
        cond = TCG_COND_LT;
        value = tcg_ctx->cpu_NF;
        break;

    case 6: /* vs: V */
    case 7: /* vc: !V */
        cond = TCG_COND_LT;
        value = tcg_ctx->cpu_VF;
        break;

    case 8: /* hi: C && !Z */
    case 9: /* ls: !C || Z -> !(C && !Z) */
        cond = TCG_COND_NE;
        value = tcg_temp_new_i32(tcg_ctx);
        global = false;
        /* CF is 1 for C, so -CF is an all-bits-set mask for C;
           ZF is non-zero for !Z; so AND the two subexpressions.  */
        tcg_gen_neg_i32(tcg_ctx, value, tcg_ctx->cpu_CF);
        tcg_gen_and_i32(tcg_ctx, value, value, tcg_ctx->cpu_ZF);
        break;

    case 10: /* ge: N == V -> N ^ V == 0 */
    case 11: /* lt: N != V -> N ^ V != 0 */
        /* Since we're only interested in the sign bit, == 0 is >= 0.  */
        cond = TCG_COND_GE;
        value = tcg_temp_new_i32(tcg_ctx);
        global = false;
        tcg_gen_xor_i32(tcg_ctx, value, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF);
        break;

    case 12: /* gt: !Z && N == V */
    case 13: /* le: Z || N != V */
        cond = TCG_COND_NE;
        value = tcg_temp_new_i32(tcg_ctx);
        global = false;
        /* (N == V) is equal to the sign bit of ~(NF ^ VF).  Propagate
         * the sign bit then AND with ZF to yield the result.  */
        tcg_gen_xor_i32(tcg_ctx, value, tcg_ctx->cpu_VF, tcg_ctx->cpu_NF);
        tcg_gen_sari_i32(tcg_ctx, value, value, 31);
        tcg_gen_andc_i32(tcg_ctx, value, tcg_ctx->cpu_ZF, value);
        break;

    case 14: /* always */
    case 15: /* always */
        /* Use the ALWAYS condition, which will fold early.
         * It doesn't matter what we use for the value.  */
        cond = TCG_COND_ALWAYS;
        value = tcg_ctx->cpu_ZF;
        goto no_invert;

    default:
        fprintf(stderr, "Bad condition code 0x%x\n", cc);
        abort();
    }

    if (cc & 1) {
        cond = tcg_invert_cond(cond);
    }

 no_invert:
    cmp->cond = cond;
    cmp->value = value;
    cmp->value_global = global;
}

void arm_free_cc(DisasContext *s, DisasCompare *cmp)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (!cmp->value_global) {
        tcg_temp_free_i32(tcg_ctx, cmp->value);
    }
}

void arm_jump_cc(DisasContext *s, DisasCompare *cmp, TCGLabel *label)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    tcg_gen_brcondi_i32(tcg_ctx, cmp->cond, cmp->value, 0, label);
}

void arm_gen_test_cc(DisasContext *s, int cc, TCGLabel *label)
{
    DisasCompare cmp;
    arm_test_cc(s, &cmp, cc);
    arm_jump_cc(s, &cmp, label);
    arm_free_cc(s, &cmp);
}

static inline void gen_set_condexec(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (s->condexec_mask) {
        uint32_t val = (s->condexec_cond << 4) | (s->condexec_mask >> 1);
        TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_movi_i32(tcg_ctx, tmp, val);
        store_cpu_field(s, tmp, condexec_bits);
    }
}

static inline void gen_set_pc_im(DisasContext *s, target_ulong val)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[15], val);
}

/* Set PC and Thumb state from var.  var is marked as dead.  */
static inline void gen_bx(DisasContext *s, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    s->base.is_jmp = DISAS_JUMP;
    tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[15], var, ~1);
    tcg_gen_andi_i32(tcg_ctx, var, var, 1);
    store_cpu_field(s, var, thumb);
}

/*
 * Set PC and Thumb state from var. var is marked as dead.
 * For M-profile CPUs, include logic to detect exception-return
 * branches and handle them. This is needed for Thumb POP/LDM to PC, LDR to PC,
 * and BX reg, and no others, and happens only for code in Handler mode.
 * The Security Extension also requires us to check for the FNC_RETURN
 * which signals a function return from non-secure state; this can happen
 * in both Handler and Thread mode.
 * To avoid having to do multiple comparisons in inline generated code,
 * we make the check we do here loose, so it will match for EXC_RETURN
 * in Thread mode. For system emulation do_v7m_exception_exit() checks
 * for these spurious cases and returns without doing anything (giving
 * the same behaviour as for a branch to a non-magic address).
 *
 * In linux-user mode it is unclear what the right behaviour for an
 * attempted FNC_RETURN should be, because in real hardware this will go
 * directly to Secure code (ie not the Linux kernel) which will then treat
 * the error in any way it chooses. For QEMU we opt to make the FNC_RETURN
 * attempt behave the way it would on a CPU without the security extension,
 * which is to say "like a normal branch". That means we can simply treat
 * all branches as normal with no magic address behaviour.
 */
static inline void gen_bx_excret(DisasContext *s, TCGv_i32 var)
{
    /* Generate the same code here as for a simple bx, but flag via
     * s->base.is_jmp that we need to do the rest of the work later.
     */
    gen_bx(s, var);
#ifndef CONFIG_USER_ONLY
    if (arm_dc_feature(s, ARM_FEATURE_M_SECURITY) ||
        (s->v7m_handler_mode && arm_dc_feature(s, ARM_FEATURE_M))) {
        s->base.is_jmp = DISAS_BX_EXCRET;
    }
#endif
}

static inline void gen_bx_excret_final_code(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    /* Generate the code to finish possible exception return and end the TB */
    TCGLabel *excret_label = gen_new_label(tcg_ctx);
    uint32_t min_magic;

    if (arm_dc_feature(s, ARM_FEATURE_M_SECURITY)) {
        /* Covers FNC_RETURN and EXC_RETURN magic */
        min_magic = FNC_RETURN_MIN_MAGIC;
    } else {
        /* EXC_RETURN magic only */
        min_magic = EXC_RETURN_MIN_MAGIC;
    }

    /* Is the new PC value in the magic range indicating exception return? */
    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_GEU, tcg_ctx->cpu_R[15], min_magic, excret_label);
    /* No: end the TB as we would for a DISAS_JMP */
    if (is_singlestepping(s)) {
        gen_singlestep_exception(s);
    } else {
        tcg_gen_exit_tb(tcg_ctx, NULL, 0);
    }
    gen_set_label(tcg_ctx, excret_label);
    /* Yes: this is an exception return.
     * At this point in runtime env->regs[15] and env->thumb will hold
     * the exception-return magic number, which do_v7m_exception_exit()
     * will read. Nothing else will be able to see those values because
     * the cpu-exec main loop guarantees that we will always go straight
     * from raising the exception to the exception-handling code.
     *
     * gen_ss_advance(s) does nothing on M profile currently but
     * calling it is conceptually the right thing as we have executed
     * this instruction (compare SWI, HVC, SMC handling).
     */
    gen_ss_advance(s);
    gen_exception_internal(s, EXCP_EXCEPTION_EXIT);
}

static inline void gen_bxns(DisasContext *s, int rm)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 var = load_reg(s, rm);

    /* The bxns helper may raise an EXCEPTION_EXIT exception, so in theory
     * we need to sync state before calling it, but:
     *  - we don't need to do gen_set_pc_im() because the bxns helper will
     *    always set the PC itself
     *  - we don't need to do gen_set_condexec() because BXNS is UNPREDICTABLE
     *    unless it's outside an IT block or the last insn in an IT block,
     *    so we know that condexec == 0 (already set at the top of the TB)
     *    is correct in the non-UNPREDICTABLE cases, and we can choose
     *    "zeroes the IT bits" as our UNPREDICTABLE behaviour otherwise.
     */
    gen_helper_v7m_bxns(tcg_ctx, tcg_ctx->cpu_env, var);
    tcg_temp_free_i32(tcg_ctx, var);
    s->base.is_jmp = DISAS_EXIT;
}

static inline void gen_blxns(DisasContext *s, int rm)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 var = load_reg(s, rm);

    /* We don't need to sync condexec state, for the same reason as bxns.
     * We do however need to set the PC, because the blxns helper reads it.
     * The blxns helper may throw an exception.
     */
    gen_set_pc_im(s, s->base.pc_next);
    gen_helper_v7m_blxns(tcg_ctx, tcg_ctx->cpu_env, var);
    tcg_temp_free_i32(tcg_ctx, var);
    s->base.is_jmp = DISAS_EXIT;
}

/* Variant of store_reg which uses branch&exchange logic when storing
   to r15 in ARM architecture v7 and above. The source must be a temporary
   and will be marked as dead. */
static inline void store_reg_bx(DisasContext *s, int reg, TCGv_i32 var)
{
    if (reg == 15 && ENABLE_ARCH_7) {
        gen_bx(s, var);
    } else {
        store_reg(s, reg, var);
    }
}

/* Variant of store_reg which uses branch&exchange logic when storing
 * to r15 in ARM architecture v5T and above. This is used for storing
 * the results of a LDR/LDM/POP into r15, and corresponds to the cases
 * in the ARM ARM which use the LoadWritePC() pseudocode function. */
static inline void store_reg_from_load(DisasContext *s, int reg, TCGv_i32 var)
{
    if (reg == 15 && ENABLE_ARCH_5) {
        gen_bx_excret(s, var);
    } else {
        store_reg(s, reg, var);
    }
}

#ifdef CONFIG_USER_ONLY
#define IS_USER_ONLY 1
#else
#define IS_USER_ONLY 0
#endif

/* Abstractions of "generate code to do a guest load/store for
 * AArch32", where a vaddr is always 32 bits (and is zero
 * extended if we're a 64 bit core) and  data is also
 * 32 bits unless specifically doing a 64 bit access.
 * These functions work like tcg_gen_qemu_{ld,st}* except
 * that the address argument is TCGv_i32 rather than TCGv.
 */

static inline TCGv gen_aa32_addr(DisasContext *s, TCGv_i32 a32, MemOp op)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv addr = tcg_temp_new(tcg_ctx);
    tcg_gen_extu_i32_tl(tcg_ctx, addr, a32);

    /* Not needed for user-mode BE32, where we use MO_BE instead.  */
    if (!IS_USER_ONLY && s->sctlr_b && (op & MO_SIZE) < MO_32) {
        tcg_gen_xori_tl(tcg_ctx, addr, addr, 4 - (1 << (op & MO_SIZE)));
    }
    return addr;
}

static void gen_aa32_ld_i32(DisasContext *s, TCGv_i32 val, TCGv_i32 a32,
                            int index, MemOp opc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv addr;

    if (arm_dc_feature(s, ARM_FEATURE_M) &&
        !arm_dc_feature(s, ARM_FEATURE_M_MAIN)) {
        opc |= MO_ALIGN;
    }

    addr = gen_aa32_addr(s, a32, opc);
    tcg_gen_qemu_ld_i32(s->uc, val, addr, index, opc);
    tcg_temp_free(tcg_ctx, addr);
}

static void gen_aa32_st_i32(DisasContext *s, TCGv_i32 val, TCGv_i32 a32,
                            int index, MemOp opc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv addr;

    if (arm_dc_feature(s, ARM_FEATURE_M) &&
        !arm_dc_feature(s, ARM_FEATURE_M_MAIN)) {
        opc |= MO_ALIGN;
    }

    addr = gen_aa32_addr(s, a32, opc);
    tcg_gen_qemu_st_i32(s->uc, val, addr, index, opc);
    tcg_temp_free(tcg_ctx, addr);
}

#define DO_GEN_LD(SUFF, OPC)                                             \
static inline void gen_aa32_ld##SUFF(DisasContext *s, TCGv_i32 val,      \
                                     TCGv_i32 a32, int index)            \
{                                                                        \
    gen_aa32_ld_i32(s, val, a32, index, OPC | s->be_data);               \
}

#define DO_GEN_ST(SUFF, OPC)                                             \
static inline void gen_aa32_st##SUFF(DisasContext *s, TCGv_i32 val,      \
                                     TCGv_i32 a32, int index)            \
{                                                                        \
    gen_aa32_st_i32(s, val, a32, index, OPC | s->be_data);               \
}

static inline void gen_aa32_frob64(DisasContext *s, TCGv_i64 val)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    /* Not needed for user-mode BE32, where we use MO_BE instead.  */
    if (!IS_USER_ONLY && s->sctlr_b) {
        tcg_gen_rotri_i64(tcg_ctx, val, val, 32);
    }
}

static void gen_aa32_ld_i64(DisasContext *s, TCGv_i64 val, TCGv_i32 a32,
                            int index, MemOp opc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv addr = gen_aa32_addr(s, a32, opc);
    tcg_gen_qemu_ld_i64(s->uc, val, addr, index, opc);
    gen_aa32_frob64(s, val);
    tcg_temp_free(tcg_ctx, addr);
}

static inline void gen_aa32_ld64(DisasContext *s, TCGv_i64 val,
                                 TCGv_i32 a32, int index)
{
    gen_aa32_ld_i64(s, val, a32, index, MO_Q | s->be_data);
}

static void gen_aa32_st_i64(DisasContext *s, TCGv_i64 val, TCGv_i32 a32,
                            int index, MemOp opc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv addr = gen_aa32_addr(s, a32, opc);

    /* Not needed for user-mode BE32, where we use MO_BE instead.  */
    if (!IS_USER_ONLY && s->sctlr_b) {
        TCGv_i64 tmp = tcg_temp_new_i64(tcg_ctx);
        tcg_gen_rotri_i64(tcg_ctx, tmp, val, 32);
        tcg_gen_qemu_st_i64(s->uc, tmp, addr, index, opc);
        tcg_temp_free_i64(tcg_ctx, tmp);
    } else {
        tcg_gen_qemu_st_i64(s->uc, val, addr, index, opc);
    }
    tcg_temp_free(tcg_ctx, addr);
}

static inline void gen_aa32_st64(DisasContext *s, TCGv_i64 val,
                                 TCGv_i32 a32, int index)
{
    gen_aa32_st_i64(s, val, a32, index, MO_Q | s->be_data);
}

DO_GEN_LD(8u, MO_UB)
DO_GEN_LD(16u, MO_UW)
DO_GEN_LD(32u, MO_UL)
DO_GEN_ST(8, MO_UB)
DO_GEN_ST(16, MO_UW)
DO_GEN_ST(32, MO_UL)

static inline void gen_hvc(DisasContext *s, int imm16)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    /* The pre HVC helper handles cases when HVC gets trapped
     * as an undefined insn by runtime configuration (ie before
     * the insn really executes).
     */
    gen_set_pc_im(s, s->pc_curr);
    gen_helper_pre_hvc(tcg_ctx, tcg_ctx->cpu_env);
    /* Otherwise we will treat this as a real exception which
     * happens after execution of the insn. (The distinction matters
     * for the PC value reported to the exception handler and also
     * for single stepping.)
     */
    s->svc_imm = imm16;
    gen_set_pc_im(s, s->base.pc_next);
    s->base.is_jmp = DISAS_HVC;
}

static inline void gen_smc(DisasContext *s)
{
    /* As with HVC, we may take an exception either before or after
     * the insn executes.
     */
    TCGv_i32 tmp;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    gen_set_pc_im(s, s->pc_curr);
    tmp = tcg_const_i32(tcg_ctx, syn_aa32_smc());
    gen_helper_pre_smc(tcg_ctx, tcg_ctx->cpu_env, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    gen_set_pc_im(s, s->base.pc_next);
    s->base.is_jmp = DISAS_SMC;
}

static void gen_exception_internal_insn(DisasContext *s, uint32_t pc, int excp)
{
    gen_set_condexec(s);
    gen_set_pc_im(s, pc);
    gen_exception_internal(s, excp);
    s->base.is_jmp = DISAS_NORETURN;
}

static void gen_exception_insn(DisasContext *s, int offset, int excp,
                               int syn, uint32_t target_el)
{
    gen_set_condexec(s);
    gen_set_pc_im(s, s->base.pc_next - offset);
    gen_exception(s, excp, syn, target_el);
    s->base.is_jmp = DISAS_NORETURN;
}

static void gen_exception_bkpt_insn(DisasContext *s, uint32_t syn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tcg_syn;

    gen_set_condexec(s);
    gen_set_pc_im(s, s->pc_curr);
    tcg_syn = tcg_const_i32(tcg_ctx, syn);
    gen_helper_exception_bkpt_insn(tcg_ctx, tcg_ctx->cpu_env, tcg_syn);
    tcg_temp_free_i32(tcg_ctx, tcg_syn);
    s->base.is_jmp = DISAS_NORETURN;
}

static void unallocated_encoding(DisasContext *s)
{
    /* Unallocated and reserved encodings are uncategorized */
    gen_exception_insn(s, s->pc_curr, EXCP_UDEF, syn_uncategorized(),
                       default_exception_el(s));
}

/* Force a TB lookup after an instruction that changes the CPU state.  */
static inline void gen_lookup_tb(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[15], s->base.pc_next);
    s->base.is_jmp = DISAS_EXIT;
}

static inline void gen_hlt(DisasContext *s, int imm)
{
    /* HLT. This has two purposes.
     * Architecturally, it is an external halting debug instruction.
     * Since QEMU doesn't implement external debug, we treat this as
     * it is required for halting debug disabled: it will UNDEF.
     * Secondly, "HLT 0x3C" is a T32 semihosting trap instruction,
     * and "HLT 0xF000" is an A32 semihosting syscall. These traps
     * must trigger semihosting even for ARMv7 and earlier, where
     * HLT was an undefined encoding.
     * In system mode, we don't allow userspace access to
     * semihosting, to provide some semblance of security
     * (and for consistency with our 32-bit semihosting).
     */
    if (semihosting_enabled(s->uc) &&
#ifndef CONFIG_USER_ONLY
        s->current_el != 0 &&
#endif
        (imm == (s->thumb ? 0x3c : 0xf000))) {
        gen_exception_internal_insn(s, s->pc_curr, EXCP_SEMIHOST);
        return;
    }

    unallocated_encoding(s);
}

static TCGv_ptr get_fpstatus_ptr(TCGContext *tcg_ctx, int neon)
{
    TCGv_ptr statusptr = tcg_temp_new_ptr(tcg_ctx);
    int offset;
    if (neon) {
        offset = offsetof(CPUARMState, vfp.standard_fp_status);
    } else {
        offset = offsetof(CPUARMState, vfp.fp_status);
    }
    tcg_gen_addi_ptr(tcg_ctx, statusptr, tcg_ctx->cpu_env, offset);
    return statusptr;
}

static inline long vfp_reg_offset(bool dp, unsigned reg)
{
    if (dp) {
        return offsetof(CPUARMState, vfp.zregs[reg >> 1].d[reg & 1]);
    } else {
        long ofs = offsetof(CPUARMState, vfp.zregs[reg >> 2].d[(reg >> 1) & 1]);
        if (reg & 1) {
            ofs += offsetof(CPU_DoubleU, l.upper);
        } else {
            ofs += offsetof(CPU_DoubleU, l.lower);
        }
        return ofs;
    }
}

/* Return the offset of a 32-bit piece of a NEON register.
   zero is the least significant end of the register.  */
static inline long
neon_reg_offset (int reg, int n)
{
    int sreg;
    sreg = reg * 2 + n;
    return vfp_reg_offset(0, sreg);
}

/* Return the offset of a 2**SIZE piece of a NEON register, at index ELE,
 * where 0 is the least significant end of the register.
 */
static inline long
neon_element_offset(int reg, int element, MemOp size)
{
    int element_size = 1 << size;
    int ofs = element * element_size;
#ifdef HOST_WORDS_BIGENDIAN
    /* Calculate the offset assuming fully little-endian,
     * then XOR to account for the order of the 8-byte units.
     */
    if (element_size < 8) {
        ofs ^= 8 - element_size;
    }
#endif
    return neon_reg_offset(reg, 0) + ofs;
}

static TCGv_i32 neon_load_reg(DisasContext *s, int reg, int pass)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, tmp, tcg_ctx->cpu_env, neon_reg_offset(reg, pass));
    return tmp;
}

static void neon_load_element(DisasContext *s, TCGv_i32 var, int reg, int ele, MemOp mop)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    long offset = neon_element_offset(reg, ele, mop & MO_SIZE);

    switch (mop) {
    case MO_UB:
        tcg_gen_ld8u_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_UW:
        tcg_gen_ld16u_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_UL:
        tcg_gen_ld_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    default:
        g_assert_not_reached();
    }
}

static void neon_load_element64(DisasContext *s, TCGv_i64 var, int reg, int ele, MemOp mop)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    long offset = neon_element_offset(reg, ele, mop & MO_SIZE);

    switch (mop) {
    case MO_UB:
        tcg_gen_ld8u_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_UW:
        tcg_gen_ld16u_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_UL:
        tcg_gen_ld32u_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_Q:
        tcg_gen_ld_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    default:
        g_assert_not_reached();
    }
}

static void neon_store_reg(DisasContext *s, int reg, int pass, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_st_i32(tcg_ctx, var, tcg_ctx->cpu_env, neon_reg_offset(reg, pass));
    tcg_temp_free_i32(tcg_ctx, var);
}

static void neon_store_element(DisasContext *s, int reg, int ele, MemOp size, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    long offset = neon_element_offset(reg, ele, size);

    switch (size) {
    case MO_8:
        tcg_gen_st8_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_16:
        tcg_gen_st16_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_32:
        tcg_gen_st_i32(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    default:
        g_assert_not_reached();
    }
}

static void neon_store_element64(DisasContext *s, int reg, int ele, MemOp size, TCGv_i64 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    long offset = neon_element_offset(reg, ele, size);

    switch (size) {
    case MO_8:
        tcg_gen_st8_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_16:
        tcg_gen_st16_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_32:
        tcg_gen_st32_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    case MO_64:
        tcg_gen_st_i64(tcg_ctx, var, tcg_ctx->cpu_env, offset);
        break;
    default:
        g_assert_not_reached();
    }
}

static inline void neon_load_reg64(DisasContext *s, TCGv_i64 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ld_i64(tcg_ctx, var, tcg_ctx->cpu_env, vfp_reg_offset(1, reg));
}

static inline void neon_store_reg64(DisasContext *s, TCGv_i64 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_st_i64(tcg_ctx, var, tcg_ctx->cpu_env, vfp_reg_offset(1, reg));
}

static inline void neon_load_reg32(DisasContext *s, TCGv_i32 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ld_i32(tcg_ctx, var, tcg_ctx->cpu_env, vfp_reg_offset(false, reg));
}

static inline void neon_store_reg32(DisasContext *s, TCGv_i32 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_st_i32(tcg_ctx, var, tcg_ctx->cpu_env, vfp_reg_offset(false, reg));
}

static TCGv_ptr vfp_reg_ptr(DisasContext *s, bool dp, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr ret = tcg_temp_new_ptr(tcg_ctx);
    tcg_gen_addi_ptr(tcg_ctx, ret, tcg_ctx->cpu_env, vfp_reg_offset(dp, reg));
    return ret;
}

#define ARM_CP_RW_BIT   (1 << 20)

/* Include the VFP and Neon decoders */
#include "translate-vfp.inc.c"
#include "translate-neon.inc.c"

static inline void iwmmxt_load_reg(DisasContext *s, TCGv_i64 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ld_i64(tcg_ctx, var, tcg_ctx->cpu_env, offsetof(CPUARMState, iwmmxt.regs[reg]));
}

static inline void iwmmxt_store_reg(DisasContext *s, TCGv_i64 var, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_st_i64(tcg_ctx, var, tcg_ctx->cpu_env, offsetof(CPUARMState, iwmmxt.regs[reg]));
}

static inline TCGv_i32 iwmmxt_load_creg(DisasContext *s, int reg)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 var = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, var, tcg_ctx->cpu_env, offsetof(CPUARMState, iwmmxt.cregs[reg]));
    return var;
}

static inline void iwmmxt_store_creg(DisasContext *s, int reg, TCGv_i32 var)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_st_i32(tcg_ctx, var, tcg_ctx->cpu_env, offsetof(CPUARMState, iwmmxt.cregs[reg]));
    tcg_temp_free_i32(tcg_ctx, var);
}

static inline void gen_op_iwmmxt_movq_wRn_M0(DisasContext *s, int rn)
{
    iwmmxt_store_reg(s, s->M0, rn);
}

static inline void gen_op_iwmmxt_movq_M0_wRn(DisasContext *s, int rn)
{
    iwmmxt_load_reg(s, s->M0, rn);
}

static inline void gen_op_iwmmxt_orq_M0_wRn(DisasContext *s, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    iwmmxt_load_reg(s, s->V1, rn);
    tcg_gen_or_i64(tcg_ctx, s->M0, s->M0, s->V1);
}

static inline void gen_op_iwmmxt_andq_M0_wRn(DisasContext *s, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    iwmmxt_load_reg(s, s->V1, rn);
    tcg_gen_and_i64(tcg_ctx, s->M0, s->M0, s->V1);
}

static inline void gen_op_iwmmxt_xorq_M0_wRn(DisasContext *s, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    iwmmxt_load_reg(s, s->V1, rn);
    tcg_gen_xor_i64(tcg_ctx, s->M0, s->M0, s->V1);
}

#define IWMMXT_OP(name) \
static inline void gen_op_iwmmxt_##name##_M0_wRn(DisasContext *s, int rn) \
{ \
    TCGContext *tcg_ctx = s->uc->tcg_ctx; \
    iwmmxt_load_reg(s, s->V1, rn); \
    gen_helper_iwmmxt_##name(tcg_ctx, s->M0, s->M0, s->V1); \
}

#define IWMMXT_OP_ENV(name) \
static inline void gen_op_iwmmxt_##name##_M0_wRn(DisasContext *s, int rn) \
{ \
    TCGContext *tcg_ctx = s->uc->tcg_ctx; \
    iwmmxt_load_reg(s, s->V1, rn); \
    gen_helper_iwmmxt_##name(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, s->V1); \
}

#define IWMMXT_OP_ENV_SIZE(name) \
IWMMXT_OP_ENV(name##b) \
IWMMXT_OP_ENV(name##w) \
IWMMXT_OP_ENV(name##l)

#define IWMMXT_OP_ENV1(name) \
static inline void gen_op_iwmmxt_##name##_M0(DisasContext *s) \
{ \
    TCGContext *tcg_ctx = s->uc->tcg_ctx; \
    gen_helper_iwmmxt_##name(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0); \
}

IWMMXT_OP(maddsq)
IWMMXT_OP(madduq)
IWMMXT_OP(sadb)
IWMMXT_OP(sadw)
IWMMXT_OP(mulslw)
IWMMXT_OP(mulshw)
IWMMXT_OP(mululw)
IWMMXT_OP(muluhw)
IWMMXT_OP(macsw)
IWMMXT_OP(macuw)

IWMMXT_OP_ENV_SIZE(unpackl)
IWMMXT_OP_ENV_SIZE(unpackh)

IWMMXT_OP_ENV1(unpacklub)
IWMMXT_OP_ENV1(unpackluw)
IWMMXT_OP_ENV1(unpacklul)
IWMMXT_OP_ENV1(unpackhub)
IWMMXT_OP_ENV1(unpackhuw)
IWMMXT_OP_ENV1(unpackhul)
IWMMXT_OP_ENV1(unpacklsb)
IWMMXT_OP_ENV1(unpacklsw)
IWMMXT_OP_ENV1(unpacklsl)
IWMMXT_OP_ENV1(unpackhsb)
IWMMXT_OP_ENV1(unpackhsw)
IWMMXT_OP_ENV1(unpackhsl)

IWMMXT_OP_ENV_SIZE(cmpeq)
IWMMXT_OP_ENV_SIZE(cmpgtu)
IWMMXT_OP_ENV_SIZE(cmpgts)

IWMMXT_OP_ENV_SIZE(mins)
IWMMXT_OP_ENV_SIZE(minu)
IWMMXT_OP_ENV_SIZE(maxs)
IWMMXT_OP_ENV_SIZE(maxu)

IWMMXT_OP_ENV_SIZE(subn)
IWMMXT_OP_ENV_SIZE(addn)
IWMMXT_OP_ENV_SIZE(subu)
IWMMXT_OP_ENV_SIZE(addu)
IWMMXT_OP_ENV_SIZE(subs)
IWMMXT_OP_ENV_SIZE(adds)

IWMMXT_OP_ENV(avgb0)
IWMMXT_OP_ENV(avgb1)
IWMMXT_OP_ENV(avgw0)
IWMMXT_OP_ENV(avgw1)

IWMMXT_OP_ENV(packuw)
IWMMXT_OP_ENV(packul)
IWMMXT_OP_ENV(packuq)
IWMMXT_OP_ENV(packsw)
IWMMXT_OP_ENV(packsl)
IWMMXT_OP_ENV(packsq)

static void gen_op_iwmmxt_set_mup(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    tmp = load_cpu_field(s, iwmmxt.cregs[ARM_IWMMXT_wCon]);
    tcg_gen_ori_i32(tcg_ctx, tmp, tmp, 2);
    store_cpu_field(s, tmp, iwmmxt.cregs[ARM_IWMMXT_wCon]);
}

static void gen_op_iwmmxt_set_cup(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    tmp = load_cpu_field(s, iwmmxt.cregs[ARM_IWMMXT_wCon]);
    tcg_gen_ori_i32(tcg_ctx, tmp, tmp, 1);
    store_cpu_field(s, tmp, iwmmxt.cregs[ARM_IWMMXT_wCon]);
}

static void gen_op_iwmmxt_setpsr_nz(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    gen_helper_iwmmxt_setpsr_nz(tcg_ctx, tmp, s->M0);
    store_cpu_field(s, tmp, iwmmxt.cregs[ARM_IWMMXT_wCASF]);
}

static inline void gen_op_iwmmxt_addl_M0_wRn(DisasContext *s, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    iwmmxt_load_reg(s, s->V1, rn);
    tcg_gen_ext32u_i64(tcg_ctx, s->V1, s->V1);
    tcg_gen_add_i64(tcg_ctx, s->M0, s->M0, s->V1);
}

static inline int gen_iwmmxt_address(DisasContext *s, uint32_t insn,
                                     TCGv_i32 dest)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int rd;
    uint32_t offset;
    TCGv_i32 tmp;

    rd = (insn >> 16) & 0xf;
    tmp = load_reg(s, rd);

    offset = (insn & 0xff) << ((insn >> 7) & 2);
    if (insn & (1 << 24)) {
        /* Pre indexed */
        if (insn & (1 << 23))
            tcg_gen_addi_i32(tcg_ctx, tmp, tmp, offset);
        else
            tcg_gen_addi_i32(tcg_ctx, tmp, tmp, 0-offset);
        tcg_gen_mov_i32(tcg_ctx, dest, tmp);
        if (insn & (1 << 21))
            store_reg(s, rd, tmp);
        else
            tcg_temp_free_i32(tcg_ctx, tmp);
    } else if (insn & (1 << 21)) {
        /* Post indexed */
        tcg_gen_mov_i32(tcg_ctx, dest, tmp);
        if (insn & (1 << 23))
            tcg_gen_addi_i32(tcg_ctx, tmp, tmp, offset);
        else
            tcg_gen_addi_i32(tcg_ctx, tmp, tmp, 0-offset);
        store_reg(s, rd, tmp);
    } else if (!(insn & (1 << 23)))
        return 1;
    return 0;
}

static inline int gen_iwmmxt_shift(DisasContext *s, uint32_t insn, uint32_t mask, TCGv_i32 dest)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int rd = (insn >> 0) & 0xf;
    TCGv_i32 tmp;

    if (insn & (1 << 8)) {
        if (rd < ARM_IWMMXT_wCGR0 || rd > ARM_IWMMXT_wCGR3) {
            return 1;
        } else {
            tmp = iwmmxt_load_creg(s, rd);
        }
    } else {
        tmp = tcg_temp_new_i32(tcg_ctx);
        iwmmxt_load_reg(s, s->V0, rd);
        tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->V0);
    }
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, mask);
    tcg_gen_mov_i32(tcg_ctx, dest, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    return 0;
}

/* Disassemble an iwMMXt instruction.  Returns nonzero if an error occurred
   (ie. an undefined instruction).  */
static int disas_iwmmxt_insn(DisasContext *s, uint32_t insn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int rd, wrd;
    int rdhi, rdlo, rd0, rd1, i;
    TCGv_i32 addr;
    TCGv_i32 tmp, tmp2, tmp3;

    if ((insn & 0x0e000e00) == 0x0c000000) {
        if ((insn & 0x0fe00ff0) == 0x0c400000) {
            wrd = insn & 0xf;
            rdlo = (insn >> 12) & 0xf;
            rdhi = (insn >> 16) & 0xf;
            if (insn & ARM_CP_RW_BIT) {         /* TMRRC */
                iwmmxt_load_reg(s, s->V0, wrd);
                tcg_gen_extrl_i64_i32(tcg_ctx, tcg_ctx->cpu_R[rdlo], s->V0);
                tcg_gen_extrh_i64_i32(tcg_ctx, tcg_ctx->cpu_R[rdhi], s->V0);
            } else {                    /* TMCRR */
                tcg_gen_concat_i32_i64(tcg_ctx, s->V0, tcg_ctx->cpu_R[rdlo], tcg_ctx->cpu_R[rdhi]);
                iwmmxt_store_reg(s, s->V0, wrd);
                gen_op_iwmmxt_set_mup(s);
            }
            return 0;
        }

        wrd = (insn >> 12) & 0xf;
        addr = tcg_temp_new_i32(tcg_ctx);
        if (gen_iwmmxt_address(s, insn, addr)) {
            tcg_temp_free_i32(tcg_ctx, addr);
            return 1;
        }
        if (insn & ARM_CP_RW_BIT) {
            if ((insn >> 28) == 0xf) {          /* WLDRW wCx */
                tmp = tcg_temp_new_i32(tcg_ctx);
                gen_aa32_ld32u(s, tmp, addr, get_mem_index(s));
                iwmmxt_store_creg(s, wrd, tmp);
            } else {
                i = 1;
                if (insn & (1 << 8)) {
                    if (insn & (1 << 22)) {     /* WLDRD */
                        gen_aa32_ld64(s, s->M0, addr, get_mem_index(s));
                        i = 0;
                    } else {                /* WLDRW wRd */
                        tmp = tcg_temp_new_i32(tcg_ctx);
                        gen_aa32_ld32u(s, tmp, addr, get_mem_index(s));
                    }
                } else {
                    tmp = tcg_temp_new_i32(tcg_ctx);
                    if (insn & (1 << 22)) {     /* WLDRH */
                        gen_aa32_ld16u(s, tmp, addr, get_mem_index(s));
                    } else {                /* WLDRB */
                        gen_aa32_ld8u(s, tmp, addr, get_mem_index(s));
                    }
                }
                if (i) {
                    tcg_gen_extu_i32_i64(tcg_ctx, s->M0, tmp);
                    tcg_temp_free_i32(tcg_ctx, tmp);
                }
                gen_op_iwmmxt_movq_wRn_M0(s, wrd);
            }
        } else {
            if ((insn >> 28) == 0xf) {          /* WSTRW wCx */
                tmp = iwmmxt_load_creg(s, wrd);
                gen_aa32_st32(s, tmp, addr, get_mem_index(s));
            } else {
                gen_op_iwmmxt_movq_M0_wRn(s, wrd);
                tmp = tcg_temp_new_i32(tcg_ctx);
                if (insn & (1 << 8)) {
                    if (insn & (1 << 22)) {     /* WSTRD */
                        gen_aa32_st64(s, s->M0, addr, get_mem_index(s));
                    } else {                /* WSTRW wRd */
                        tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
                        gen_aa32_st32(s, tmp, addr, get_mem_index(s));
                    }
                } else {
                    if (insn & (1 << 22)) {     /* WSTRH */
                        tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
                        gen_aa32_st16(s, tmp, addr, get_mem_index(s));
                    } else {                /* WSTRB */
                        tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
                        gen_aa32_st8(s, tmp, addr, get_mem_index(s));
                    }
                }
            }
            tcg_temp_free_i32(tcg_ctx, tmp);
        }
        tcg_temp_free_i32(tcg_ctx, addr);
        return 0;
    }

    if ((insn & 0x0f000000) != 0x0e000000)
        return 1;

    switch (((insn >> 12) & 0xf00) | ((insn >> 4) & 0xff)) {
    case 0x000:                     /* WOR */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 0) & 0xf;
        rd1 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        gen_op_iwmmxt_orq_M0_wRn(s, rd1);
        gen_op_iwmmxt_setpsr_nz(s);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x011:                     /* TMCR */
        if (insn & 0xf)
            return 1;
        rd = (insn >> 12) & 0xf;
        wrd = (insn >> 16) & 0xf;
        switch (wrd) {
        case ARM_IWMMXT_wCID:
        case ARM_IWMMXT_wCASF:
            break;
        case ARM_IWMMXT_wCon:
            gen_op_iwmmxt_set_cup(s);
            /* Fall through.  */
        case ARM_IWMMXT_wCSSF:
            tmp = iwmmxt_load_creg(s, wrd);
            tmp2 = load_reg(s, rd);
            tcg_gen_andc_i32(tcg_ctx, tmp, tmp, tmp2);
            tcg_temp_free_i32(tcg_ctx, tmp2);
            iwmmxt_store_creg(s, wrd, tmp);
            break;
        case ARM_IWMMXT_wCGR0:
        case ARM_IWMMXT_wCGR1:
        case ARM_IWMMXT_wCGR2:
        case ARM_IWMMXT_wCGR3:
            gen_op_iwmmxt_set_cup(s);
            tmp = load_reg(s, rd);
            iwmmxt_store_creg(s, wrd, tmp);
            break;
        default:
            return 1;
        }
        break;
    case 0x100:                     /* WXOR */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 0) & 0xf;
        rd1 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        gen_op_iwmmxt_xorq_M0_wRn(s, rd1);
        gen_op_iwmmxt_setpsr_nz(s);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x111:                     /* TMRC */
        if (insn & 0xf)
            return 1;
        rd = (insn >> 12) & 0xf;
        wrd = (insn >> 16) & 0xf;
        tmp = iwmmxt_load_creg(s, wrd);
        store_reg(s, rd, tmp);
        break;
    case 0x300:                     /* WANDN */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 0) & 0xf;
        rd1 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tcg_gen_neg_i64(tcg_ctx, s->M0, s->M0);
        gen_op_iwmmxt_andq_M0_wRn(s, rd1);
        gen_op_iwmmxt_setpsr_nz(s);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x200:                     /* WAND */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 0) & 0xf;
        rd1 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        gen_op_iwmmxt_andq_M0_wRn(s, rd1);
        gen_op_iwmmxt_setpsr_nz(s);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x810: case 0xa10:             /* WMADD */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 0) & 0xf;
        rd1 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        if (insn & (1 << 21))
            gen_op_iwmmxt_maddsq_M0_wRn(s, rd1);
        else
            gen_op_iwmmxt_madduq_M0_wRn(s, rd1);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x10e: case 0x50e: case 0x90e: case 0xd0e: /* WUNPCKIL */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            gen_op_iwmmxt_unpacklb_M0_wRn(s, rd1);
            break;
        case 1:
            gen_op_iwmmxt_unpacklw_M0_wRn(s, rd1);
            break;
        case 2:
            gen_op_iwmmxt_unpackll_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x10c: case 0x50c: case 0x90c: case 0xd0c: /* WUNPCKIH */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            gen_op_iwmmxt_unpackhb_M0_wRn(s, rd1);
            break;
        case 1:
            gen_op_iwmmxt_unpackhw_M0_wRn(s, rd1);
            break;
        case 2:
            gen_op_iwmmxt_unpackhl_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x012: case 0x112: case 0x412: case 0x512: /* WSAD */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        if (insn & (1 << 22))
            gen_op_iwmmxt_sadw_M0_wRn(s, rd1);
        else
            gen_op_iwmmxt_sadb_M0_wRn(s, rd1);
        if (!(insn & (1 << 20)))
            gen_op_iwmmxt_addl_M0_wRn(s, wrd);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x010: case 0x110: case 0x210: case 0x310: /* WMUL */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        if (insn & (1 << 21)) {
            if (insn & (1 << 20))
                gen_op_iwmmxt_mulshw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_mulslw_M0_wRn(s, rd1);
        } else {
            if (insn & (1 << 20))
                gen_op_iwmmxt_muluhw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_mululw_M0_wRn(s, rd1);
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x410: case 0x510: case 0x610: case 0x710: /* WMAC */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        if (insn & (1 << 21))
            gen_op_iwmmxt_macsw_M0_wRn(s, rd1);
        else
            gen_op_iwmmxt_macuw_M0_wRn(s, rd1);
        if (!(insn & (1 << 20))) {
            iwmmxt_load_reg(s, s->V1, wrd);
            tcg_gen_add_i64(tcg_ctx, s->M0, s->M0, s->V1);
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x006: case 0x406: case 0x806: case 0xc06: /* WCMPEQ */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            gen_op_iwmmxt_cmpeqb_M0_wRn(s, rd1);
            break;
        case 1:
            gen_op_iwmmxt_cmpeqw_M0_wRn(s, rd1);
            break;
        case 2:
            gen_op_iwmmxt_cmpeql_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x800: case 0x900: case 0xc00: case 0xd00: /* WAVG2 */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        if (insn & (1 << 22)) {
            if (insn & (1 << 20))
                gen_op_iwmmxt_avgw1_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_avgw0_M0_wRn(s, rd1);
        } else {
            if (insn & (1 << 20))
                gen_op_iwmmxt_avgb1_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_avgb0_M0_wRn(s, rd1);
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x802: case 0x902: case 0xa02: case 0xb02: /* WALIGNR */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = iwmmxt_load_creg(s, ARM_IWMMXT_wCGR0 + ((insn >> 20) & 3));
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 7);
        iwmmxt_load_reg(s, s->V1, rd1);
        gen_helper_iwmmxt_align(tcg_ctx, s->M0, s->M0, s->V1, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x601: case 0x605: case 0x609: case 0x60d: /* TINSR */
        if (((insn >> 6) & 3) == 3)
            return 1;
        rd = (insn >> 12) & 0xf;
        wrd = (insn >> 16) & 0xf;
        tmp = load_reg(s, rd);
        gen_op_iwmmxt_movq_M0_wRn(s, wrd);
        switch ((insn >> 6) & 3) {
        case 0:
            tmp2 = tcg_const_i32(tcg_ctx, 0xff);
            tmp3 = tcg_const_i32(tcg_ctx, (insn & 7) << 3);
            break;
        case 1:
            tmp2 = tcg_const_i32(tcg_ctx, 0xffff);
            tmp3 = tcg_const_i32(tcg_ctx, (insn & 3) << 4);
            break;
        case 2:
            tmp2 = tcg_const_i32(tcg_ctx, 0xffffffff);
            tmp3 = tcg_const_i32(tcg_ctx, (insn & 1) << 5);
            break;
        default:
            tmp2 = NULL;
            tmp3 = NULL;
        }
        gen_helper_iwmmxt_insr(tcg_ctx, s->M0, s->M0, tmp, tmp2, tmp3);
        tcg_temp_free_i32(tcg_ctx, tmp3);
        tcg_temp_free_i32(tcg_ctx, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x107: case 0x507: case 0x907: case 0xd07: /* TEXTRM */
        rd = (insn >> 12) & 0xf;
        wrd = (insn >> 16) & 0xf;
        if (rd == 15 || ((insn >> 22) & 3) == 3)
            return 1;
        gen_op_iwmmxt_movq_M0_wRn(s, wrd);
        tmp = tcg_temp_new_i32(tcg_ctx);
        switch ((insn >> 22) & 3) {
        case 0:
            tcg_gen_shri_i64(tcg_ctx, s->M0, s->M0, (insn & 7) << 3);
            tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
            if (insn & 8) {
                tcg_gen_ext8s_i32(tcg_ctx, tmp, tmp);
            } else {
                tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0xff);
            }
            break;
        case 1:
            tcg_gen_shri_i64(tcg_ctx, s->M0, s->M0, (insn & 3) << 4);
            tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
            if (insn & 8) {
                tcg_gen_ext16s_i32(tcg_ctx, tmp, tmp);
            } else {
                tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0xffff);
            }
            break;
        case 2:
            tcg_gen_shri_i64(tcg_ctx, s->M0, s->M0, (insn & 1) << 5);
            tcg_gen_extrl_i64_i32(tcg_ctx, tmp, s->M0);
            break;
        }
        store_reg(s, rd, tmp);
        break;
    case 0x117: case 0x517: case 0x917: case 0xd17: /* TEXTRC */
        if ((insn & 0x000ff008) != 0x0003f000 || ((insn >> 22) & 3) == 3)
            return 1;
        tmp = iwmmxt_load_creg(s, ARM_IWMMXT_wCASF);
        switch ((insn >> 22) & 3) {
        case 0:
            tcg_gen_shri_i32(tcg_ctx, tmp, tmp, ((insn & 7) << 2) + 0);
            break;
        case 1:
            tcg_gen_shri_i32(tcg_ctx, tmp, tmp, ((insn & 3) << 3) + 4);
            break;
        case 2:
            tcg_gen_shri_i32(tcg_ctx, tmp, tmp, ((insn & 1) << 4) + 12);
            break;
        }
        tcg_gen_shli_i32(tcg_ctx, tmp, tmp, 28);
        gen_set_nzcv(s, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        break;
    case 0x401: case 0x405: case 0x409: case 0x40d: /* TBCST */
        if (((insn >> 6) & 3) == 3)
            return 1;
        rd = (insn >> 12) & 0xf;
        wrd = (insn >> 16) & 0xf;
        tmp = load_reg(s, rd);
        switch ((insn >> 6) & 3) {
        case 0:
            gen_helper_iwmmxt_bcstb(tcg_ctx, s->M0, tmp);
            break;
        case 1:
            gen_helper_iwmmxt_bcstw(tcg_ctx, s->M0, tmp);
            break;
        case 2:
            gen_helper_iwmmxt_bcstl(tcg_ctx, s->M0, tmp);
            break;
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x113: case 0x513: case 0x913: case 0xd13: /* TANDC */
        if ((insn & 0x000ff00f) != 0x0003f000 || ((insn >> 22) & 3) == 3)
            return 1;
        tmp = iwmmxt_load_creg(s, ARM_IWMMXT_wCASF);
        tmp2 = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_mov_i32(tcg_ctx, tmp2, tmp);
        switch ((insn >> 22) & 3) {
        case 0:
            for (i = 0; i < 7; i ++) {
                tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 4);
                tcg_gen_and_i32(tcg_ctx, tmp, tmp, tmp2);
            }
            break;
        case 1:
            for (i = 0; i < 3; i ++) {
                tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 8);
                tcg_gen_and_i32(tcg_ctx, tmp, tmp, tmp2);
            }
            break;
        case 2:
            tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 16);
            tcg_gen_and_i32(tcg_ctx, tmp, tmp, tmp2);
            break;
        }
        gen_set_nzcv(s, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp);
        break;
    case 0x01c: case 0x41c: case 0x81c: case 0xc1c: /* WACC */
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            gen_helper_iwmmxt_addcb(tcg_ctx, s->M0, s->M0);
            break;
        case 1:
            gen_helper_iwmmxt_addcw(tcg_ctx, s->M0, s->M0);
            break;
        case 2:
            gen_helper_iwmmxt_addcl(tcg_ctx, s->M0, s->M0);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x115: case 0x515: case 0x915: case 0xd15: /* TORC */
        if ((insn & 0x000ff00f) != 0x0003f000 || ((insn >> 22) & 3) == 3)
            return 1;
        tmp = iwmmxt_load_creg(s, ARM_IWMMXT_wCASF);
        tmp2 = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_mov_i32(tcg_ctx, tmp2, tmp);
        switch ((insn >> 22) & 3) {
        case 0:
            for (i = 0; i < 7; i ++) {
                tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 4);
                tcg_gen_or_i32(tcg_ctx, tmp, tmp, tmp2);
            }
            break;
        case 1:
            for (i = 0; i < 3; i ++) {
                tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 8);
                tcg_gen_or_i32(tcg_ctx, tmp, tmp, tmp2);
            }
            break;
        case 2:
            tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 16);
            tcg_gen_or_i32(tcg_ctx, tmp, tmp, tmp2);
            break;
        }
        gen_set_nzcv(s, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp);
        break;
    case 0x103: case 0x503: case 0x903: case 0xd03: /* TMOVMSK */
        rd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        if ((insn & 0xf) != 0 || ((insn >> 22) & 3) == 3)
            return 1;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_temp_new_i32(tcg_ctx);
        switch ((insn >> 22) & 3) {
        case 0:
            gen_helper_iwmmxt_msbb(tcg_ctx, tmp, s->M0);
            break;
        case 1:
            gen_helper_iwmmxt_msbw(tcg_ctx, tmp, s->M0);
            break;
        case 2:
            gen_helper_iwmmxt_msbl(tcg_ctx, tmp, s->M0);
            break;
        }
        store_reg(s, rd, tmp);
        break;
    case 0x106: case 0x306: case 0x506: case 0x706: /* WCMPGT */
    case 0x906: case 0xb06: case 0xd06: case 0xf06:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            if (insn & (1 << 21))
                gen_op_iwmmxt_cmpgtsb_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_cmpgtub_M0_wRn(s, rd1);
            break;
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_cmpgtsw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_cmpgtuw_M0_wRn(s, rd1);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_cmpgtsl_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_cmpgtul_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x00e: case 0x20e: case 0x40e: case 0x60e: /* WUNPCKEL */
    case 0x80e: case 0xa0e: case 0xc0e: case 0xe0e:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpacklsb_M0(s);
            else
                gen_op_iwmmxt_unpacklub_M0(s);
            break;
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpacklsw_M0(s);
            else
                gen_op_iwmmxt_unpackluw_M0(s);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpacklsl_M0(s);
            else
                gen_op_iwmmxt_unpacklul_M0(s);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x00c: case 0x20c: case 0x40c: case 0x60c: /* WUNPCKEH */
    case 0x80c: case 0xa0c: case 0xc0c: case 0xe0c:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpackhsb_M0(s);
            else
                gen_op_iwmmxt_unpackhub_M0(s);
            break;
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpackhsw_M0(s);
            else
                gen_op_iwmmxt_unpackhuw_M0(s);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_unpackhsl_M0(s);
            else
                gen_op_iwmmxt_unpackhul_M0(s);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x204: case 0x604: case 0xa04: case 0xe04: /* WSRL */
    case 0x214: case 0x614: case 0xa14: case 0xe14:
        if (((insn >> 22) & 3) == 0)
            return 1;
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_temp_new_i32(tcg_ctx);
        if (gen_iwmmxt_shift(s, insn, 0xff, tmp)) {
            tcg_temp_free_i32(tcg_ctx, tmp);
            return 1;
        }
        switch ((insn >> 22) & 3) {
        case 1:
            gen_helper_iwmmxt_srlw(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 2:
            gen_helper_iwmmxt_srll(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 3:
            gen_helper_iwmmxt_srlq(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x004: case 0x404: case 0x804: case 0xc04: /* WSRA */
    case 0x014: case 0x414: case 0x814: case 0xc14:
        if (((insn >> 22) & 3) == 0)
            return 1;
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_temp_new_i32(tcg_ctx);
        if (gen_iwmmxt_shift(s, insn, 0xff, tmp)) {
            tcg_temp_free_i32(tcg_ctx, tmp);
            return 1;
        }
        switch ((insn >> 22) & 3) {
        case 1:
            gen_helper_iwmmxt_sraw(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 2:
            gen_helper_iwmmxt_sral(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 3:
            gen_helper_iwmmxt_sraq(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x104: case 0x504: case 0x904: case 0xd04: /* WSLL */
    case 0x114: case 0x514: case 0x914: case 0xd14:
        if (((insn >> 22) & 3) == 0)
            return 1;
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_temp_new_i32(tcg_ctx);
        if (gen_iwmmxt_shift(s, insn, 0xff, tmp)) {
            tcg_temp_free_i32(tcg_ctx, tmp);
            return 1;
        }
        switch ((insn >> 22) & 3) {
        case 1:
            gen_helper_iwmmxt_sllw(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 2:
            gen_helper_iwmmxt_slll(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 3:
            gen_helper_iwmmxt_sllq(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x304: case 0x704: case 0xb04: case 0xf04: /* WROR */
    case 0x314: case 0x714: case 0xb14: case 0xf14:
        if (((insn >> 22) & 3) == 0)
            return 1;
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_temp_new_i32(tcg_ctx);
        switch ((insn >> 22) & 3) {
        case 1:
            if (gen_iwmmxt_shift(s, insn, 0xf, tmp)) {
                tcg_temp_free_i32(tcg_ctx, tmp);
                return 1;
            }
            gen_helper_iwmmxt_rorw(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 2:
            if (gen_iwmmxt_shift(s, insn, 0x1f, tmp)) {
                tcg_temp_free_i32(tcg_ctx, tmp);
                return 1;
            }
            gen_helper_iwmmxt_rorl(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        case 3:
            if (gen_iwmmxt_shift(s, insn, 0x3f, tmp)) {
                tcg_temp_free_i32(tcg_ctx, tmp);
                return 1;
            }
            gen_helper_iwmmxt_rorq(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
            break;
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x116: case 0x316: case 0x516: case 0x716: /* WMIN */
    case 0x916: case 0xb16: case 0xd16: case 0xf16:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            if (insn & (1 << 21))
                gen_op_iwmmxt_minsb_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_minub_M0_wRn(s, rd1);
            break;
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_minsw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_minuw_M0_wRn(s, rd1);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_minsl_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_minul_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x016: case 0x216: case 0x416: case 0x616: /* WMAX */
    case 0x816: case 0xa16: case 0xc16: case 0xe16:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 0:
            if (insn & (1 << 21))
                gen_op_iwmmxt_maxsb_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_maxub_M0_wRn(s, rd1);
            break;
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_maxsw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_maxuw_M0_wRn(s, rd1);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_maxsl_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_maxul_M0_wRn(s, rd1);
            break;
        case 3:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x002: case 0x102: case 0x202: case 0x302: /* WALIGNI */
    case 0x402: case 0x502: case 0x602: case 0x702:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_const_i32(tcg_ctx, (insn >> 20) & 3);
        iwmmxt_load_reg(s, s->V1, rd1);
        gen_helper_iwmmxt_align(tcg_ctx, s->M0, s->M0, s->V1, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    case 0x01a: case 0x11a: case 0x21a: case 0x31a: /* WSUB */
    case 0x41a: case 0x51a: case 0x61a: case 0x71a:
    case 0x81a: case 0x91a: case 0xa1a: case 0xb1a:
    case 0xc1a: case 0xd1a: case 0xe1a: case 0xf1a:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 20) & 0xf) {
        case 0x0:
            gen_op_iwmmxt_subnb_M0_wRn(s, rd1);
            break;
        case 0x1:
            gen_op_iwmmxt_subub_M0_wRn(s, rd1);
            break;
        case 0x3:
            gen_op_iwmmxt_subsb_M0_wRn(s, rd1);
            break;
        case 0x4:
            gen_op_iwmmxt_subnw_M0_wRn(s, rd1);
            break;
        case 0x5:
            gen_op_iwmmxt_subuw_M0_wRn(s, rd1);
            break;
        case 0x7:
            gen_op_iwmmxt_subsw_M0_wRn(s, rd1);
            break;
        case 0x8:
            gen_op_iwmmxt_subnl_M0_wRn(s, rd1);
            break;
        case 0x9:
            gen_op_iwmmxt_subul_M0_wRn(s, rd1);
            break;
        case 0xb:
            gen_op_iwmmxt_subsl_M0_wRn(s, rd1);
            break;
        default:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x01e: case 0x11e: case 0x21e: case 0x31e: /* WSHUFH */
    case 0x41e: case 0x51e: case 0x61e: case 0x71e:
    case 0x81e: case 0x91e: case 0xa1e: case 0xb1e:
    case 0xc1e: case 0xd1e: case 0xe1e: case 0xf1e:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        tmp = tcg_const_i32(tcg_ctx, ((insn >> 16) & 0xf0) | (insn & 0x0f));
        gen_helper_iwmmxt_shufh(tcg_ctx, s->M0, tcg_ctx->cpu_env, s->M0, tmp);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x018: case 0x118: case 0x218: case 0x318: /* WADD */
    case 0x418: case 0x518: case 0x618: case 0x718:
    case 0x818: case 0x918: case 0xa18: case 0xb18:
    case 0xc18: case 0xd18: case 0xe18: case 0xf18:
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 20) & 0xf) {
        case 0x0:
            gen_op_iwmmxt_addnb_M0_wRn(s, rd1);
            break;
        case 0x1:
            gen_op_iwmmxt_addub_M0_wRn(s, rd1);
            break;
        case 0x3:
            gen_op_iwmmxt_addsb_M0_wRn(s, rd1);
            break;
        case 0x4:
            gen_op_iwmmxt_addnw_M0_wRn(s, rd1);
            break;
        case 0x5:
            gen_op_iwmmxt_adduw_M0_wRn(s, rd1);
            break;
        case 0x7:
            gen_op_iwmmxt_addsw_M0_wRn(s, rd1);
            break;
        case 0x8:
            gen_op_iwmmxt_addnl_M0_wRn(s, rd1);
            break;
        case 0x9:
            gen_op_iwmmxt_addul_M0_wRn(s, rd1);
            break;
        case 0xb:
            gen_op_iwmmxt_addsl_M0_wRn(s, rd1);
            break;
        default:
            return 1;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x008: case 0x108: case 0x208: case 0x308: /* WPACK */
    case 0x408: case 0x508: case 0x608: case 0x708:
    case 0x808: case 0x908: case 0xa08: case 0xb08:
    case 0xc08: case 0xd08: case 0xe08: case 0xf08:
        if (!(insn & (1 << 20)) || ((insn >> 22) & 3) == 0)
            return 1;
        wrd = (insn >> 12) & 0xf;
        rd0 = (insn >> 16) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        gen_op_iwmmxt_movq_M0_wRn(s, rd0);
        switch ((insn >> 22) & 3) {
        case 1:
            if (insn & (1 << 21))
                gen_op_iwmmxt_packsw_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_packuw_M0_wRn(s, rd1);
            break;
        case 2:
            if (insn & (1 << 21))
                gen_op_iwmmxt_packsl_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_packul_M0_wRn(s, rd1);
            break;
        case 3:
            if (insn & (1 << 21))
                gen_op_iwmmxt_packsq_M0_wRn(s, rd1);
            else
                gen_op_iwmmxt_packuq_M0_wRn(s, rd1);
            break;
        }
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        gen_op_iwmmxt_set_cup(s);
        break;
    case 0x201: case 0x203: case 0x205: case 0x207:
    case 0x209: case 0x20b: case 0x20d: case 0x20f:
    case 0x211: case 0x213: case 0x215: case 0x217:
    case 0x219: case 0x21b: case 0x21d: case 0x21f:
        wrd = (insn >> 5) & 0xf;
        rd0 = (insn >> 12) & 0xf;
        rd1 = (insn >> 0) & 0xf;
        if (rd0 == 0xf || rd1 == 0xf)
            return 1;
        gen_op_iwmmxt_movq_M0_wRn(s, wrd);
        tmp = load_reg(s, rd0);
        tmp2 = load_reg(s, rd1);
        switch ((insn >> 16) & 0xf) {
        case 0x0:                   /* TMIA */
            gen_helper_iwmmxt_muladdsl(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        case 0x8:                   /* TMIAPH */
            gen_helper_iwmmxt_muladdsw(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        case 0xc: case 0xd: case 0xe: case 0xf:     /* TMIAxy */
            if (insn & (1 << 16))
                tcg_gen_shri_i32(tcg_ctx, tmp, tmp, 16);
            if (insn & (1 << 17))
                tcg_gen_shri_i32(tcg_ctx, tmp2, tmp2, 16);
            gen_helper_iwmmxt_muladdswl(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        default:
            tcg_temp_free_i32(tcg_ctx, tmp2);
            tcg_temp_free_i32(tcg_ctx, tmp);
            return 1;
        }
        tcg_temp_free_i32(tcg_ctx, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp);
        gen_op_iwmmxt_movq_wRn_M0(s, wrd);
        gen_op_iwmmxt_set_mup(s);
        break;
    default:
        return 1;
    }

    return 0;
}

/* Disassemble an XScale DSP instruction.  Returns nonzero if an error occurred
   (ie. an undefined instruction).  */
static int disas_dsp_insn(DisasContext *s, uint32_t insn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int acc, rd0, rd1, rdhi, rdlo;
    TCGv_i32 tmp, tmp2;

    if ((insn & 0x0ff00f10) == 0x0e200010) {
        /* Multiply with Internal Accumulate Format */
        rd0 = (insn >> 12) & 0xf;
        rd1 = insn & 0xf;
        acc = (insn >> 5) & 7;

        if (acc != 0)
            return 1;

        tmp = load_reg(s, rd0);
        tmp2 = load_reg(s, rd1);
        switch ((insn >> 16) & 0xf) {
        case 0x0:                   /* MIA */
            gen_helper_iwmmxt_muladdsl(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        case 0x8:                   /* MIAPH */
            gen_helper_iwmmxt_muladdsw(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        case 0xc:                   /* MIABB */
        case 0xd:                   /* MIABT */
        case 0xe:                   /* MIATB */
        case 0xf:                   /* MIATT */
            if (insn & (1 << 16))
                tcg_gen_shri_i32(tcg_ctx, tmp, tmp, 16);
            if (insn & (1 << 17))
                tcg_gen_shri_i32(tcg_ctx, tmp2, tmp2, 16);
            gen_helper_iwmmxt_muladdswl(tcg_ctx, s->M0, s->M0, tmp, tmp2);
            break;
        default:
            return 1;
        }
        tcg_temp_free_i32(tcg_ctx, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp);

        gen_op_iwmmxt_movq_wRn_M0(s, acc);
        return 0;
    }

    if ((insn & 0x0fe00ff8) == 0x0c400000) {
        /* Internal Accumulator Access Format */
        rdhi = (insn >> 16) & 0xf;
        rdlo = (insn >> 12) & 0xf;
        acc = insn & 7;

        if (acc != 0)
            return 1;

        if (insn & ARM_CP_RW_BIT) {         /* MRA */
            iwmmxt_load_reg(s, s->V0, acc);
            tcg_gen_extrl_i64_i32(tcg_ctx, tcg_ctx->cpu_R[rdlo], s->V0);
            tcg_gen_extrh_i64_i32(tcg_ctx, tcg_ctx->cpu_R[rdhi], s->V0);
            tcg_gen_andi_i32(tcg_ctx, tcg_ctx->cpu_R[rdhi], tcg_ctx->cpu_R[rdhi], (1 << (40 - 32)) - 1);
        } else {                    /* MAR */
            tcg_gen_concat_i32_i64(tcg_ctx, s->V0, tcg_ctx->cpu_R[rdlo], tcg_ctx->cpu_R[rdhi]);
            iwmmxt_store_reg(s, s->V0, acc);
        }
        return 0;
    }

    return 1;
}

#define VFP_REG_SHR(x, n) (((n) > 0) ? (x) >> (n) : (x) << -(n))
#define VFP_DREG(reg, insn, bigbit, smallbit) do { \
    if (dc_isar_feature(aa32_simd_r32, s)) { \
        reg = (((insn) >> (bigbit)) & 0x0f) \
              | (((insn) >> ((smallbit) - 4)) & 0x10); \
    } else { \
        if (insn & (1 << (smallbit))) \
            return 1; \
        reg = ((insn) >> (bigbit)) & 0x0f; \
    }} while (0)

#define VFP_DREG_D(reg, insn) VFP_DREG(reg, insn, 12, 22)
#define VFP_DREG_N(reg, insn) VFP_DREG(reg, insn, 16,  7)
#define VFP_DREG_M(reg, insn) VFP_DREG(reg, insn,  0,  5)

static inline bool use_goto_tb(DisasContext *s, target_ulong dest)
{
#ifndef CONFIG_USER_ONLY
    return (s->base.tb->pc & TARGET_PAGE_MASK) == (dest & TARGET_PAGE_MASK) ||
           ((s->base.pc_next - 1) & TARGET_PAGE_MASK) == (dest & TARGET_PAGE_MASK);
#else
    return true;
#endif
}

static void gen_goto_ptr(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_lookup_and_goto_ptr(tcg_ctx);
}

/* This will end the TB but doesn't guarantee we'll return to
 * cpu_loop_exec. Any live exit_requests will be processed as we
 * enter the next TB.
 */
static void gen_goto_tb(DisasContext *s, int n, target_ulong dest)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (use_goto_tb(s, dest)) {
        tcg_gen_goto_tb(tcg_ctx, n);
        gen_set_pc_im(s, dest);
        tcg_gen_exit_tb(tcg_ctx, s->base.tb, n);
    } else {
        gen_set_pc_im(s, dest);
        gen_goto_ptr(s);
    }
    s->base.is_jmp = DISAS_NORETURN;
}

static inline void gen_jmp(DisasContext *s, uint32_t dest)
{
    if (unlikely(is_singlestepping(s))) {
        /* An indirect jump so that we still trigger the debug exception.  */
        gen_set_pc_im(s, dest);
        s->base.is_jmp = DISAS_JUMP;
    } else {
        gen_goto_tb(s, 0, dest);
    }
}

static inline void gen_mulxy(DisasContext *s, TCGv_i32 t0, TCGv_i32 t1, int x, int y)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (x)
        tcg_gen_sari_i32(tcg_ctx, t0, t0, 16);
    else
        gen_sxth(t0);
    if (y)
        tcg_gen_sari_i32(tcg_ctx, t1, t1, 16);
    else
        gen_sxth(t1);
    tcg_gen_mul_i32(tcg_ctx, t0, t0, t1);
}

/* Return the mask of PSR bits set by a MSR instruction.  */
static uint32_t msr_mask(DisasContext *s, int flags, int spsr)
{
    uint32_t mask = 0;

    if (flags & (1 << 0)) {
        mask |= 0xff;
    }
    if (flags & (1 << 1)) {
        mask |= 0xff00;
    }
    if (flags & (1 << 2)) {
        mask |= 0xff0000;
    }
    if (flags & (1 << 3)) {
        mask |= 0xff000000;
    }

    /* Mask out undefined and reserved bits.  */
    mask &= aarch32_cpsr_valid_mask(s->features, s->isar);

    /* Mask out execution state.  */
    if (!spsr) {
        mask &= ~CPSR_EXEC;
    }

    /* Mask out privileged bits.  */
    if (IS_USER(s)) {
        mask &= CPSR_USER;
    }
    return mask;
}

/* Returns nonzero if access to the PSR is not permitted. Marks t0 as dead. */
static int gen_set_psr(DisasContext *s, uint32_t mask, int spsr, TCGv_i32 t0)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    if (spsr) {
        /* ??? This is also undefined in system mode.  */
        if (IS_USER(s))
            return 1;

        tmp = load_cpu_field(s, spsr);
        tcg_gen_andi_i32(tcg_ctx, tmp, tmp, ~mask);
        tcg_gen_andi_i32(tcg_ctx, t0, t0, mask);
        tcg_gen_or_i32(tcg_ctx, tmp, tmp, t0);
        store_cpu_field(s, tmp, spsr);
    } else {
        gen_set_cpsr(s, t0, mask);
    }
    tcg_temp_free_i32(tcg_ctx, t0);
    gen_lookup_tb(s);
    return 0;
}

/* Returns nonzero if access to the PSR is not permitted.  */
static int gen_set_psr_im(DisasContext *s, uint32_t mask, int spsr, uint32_t val)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_movi_i32(tcg_ctx, tmp, val);
    return gen_set_psr(s, mask, spsr, tmp);
}

static bool msr_banked_access_decode(DisasContext *s, int r, int sysm, int rn,
                                     int *tgtmode, int *regno)
{
    /* Decode the r and sysm fields of MSR/MRS banked accesses into
     * the target mode and register number, and identify the various
     * unpredictable cases.
     * MSR (banked) and MRS (banked) are CONSTRAINED UNPREDICTABLE if:
     *  + executed in user mode
     *  + using R15 as the src/dest register
     *  + accessing an unimplemented register
     *  + accessing a register that's inaccessible at current PL/security state*
     *  + accessing a register that you could access with a different insn
     * We choose to UNDEF in all these cases.
     * Since we don't know which of the various AArch32 modes we are in
     * we have to defer some checks to runtime.
     * Accesses to Monitor mode registers from Secure EL1 (which implies
     * that EL3 is AArch64) must trap to EL3.
     *
     * If the access checks fail this function will emit code to take
     * an exception and return false. Otherwise it will return true,
     * and set *tgtmode and *regno appropriately.
     */
    int exc_target = default_exception_el(s);

    /* These instructions are present only in ARMv8, or in ARMv7 with the
     * Virtualization Extensions.
     */
    if (!arm_dc_feature(s, ARM_FEATURE_V8) &&
        !arm_dc_feature(s, ARM_FEATURE_EL2)) {
        goto undef;
    }

    if (IS_USER(s) || rn == 15) {
        goto undef;
    }

    /* The table in the v8 ARM ARM section F5.2.3 describes the encoding
     * of registers into (r, sysm).
     */
    if (r) {
        /* SPSRs for other modes */
        switch (sysm) {
        case 0xe: /* SPSR_fiq */
            *tgtmode = ARM_CPU_MODE_FIQ;
            break;
        case 0x10: /* SPSR_irq */
            *tgtmode = ARM_CPU_MODE_IRQ;
            break;
        case 0x12: /* SPSR_svc */
            *tgtmode = ARM_CPU_MODE_SVC;
            break;
        case 0x14: /* SPSR_abt */
            *tgtmode = ARM_CPU_MODE_ABT;
            break;
        case 0x16: /* SPSR_und */
            *tgtmode = ARM_CPU_MODE_UND;
            break;
        case 0x1c: /* SPSR_mon */
            *tgtmode = ARM_CPU_MODE_MON;
            break;
        case 0x1e: /* SPSR_hyp */
            *tgtmode = ARM_CPU_MODE_HYP;
            break;
        default: /* unallocated */
            goto undef;
        }
        /* We arbitrarily assign SPSR a register number of 16. */
        *regno = 16;
    } else {
        /* general purpose registers for other modes */
        switch (sysm) {
        case 0x0: /* 0b00xxx : r8_usr ... r14_usr */
        case 0x1:
        case 0x2:
        case 0x3:
        case 0x4:
        case 0x5:
        case 0x6:
            *tgtmode = ARM_CPU_MODE_USR;
            *regno = sysm + 8;
            break;
        case 0x8: /* 0b01xxx : r8_fiq ... r14_fiq */
        case 0x9:
        case 0xa:
        case 0xb:
        case 0xc:
        case 0xd:
        case 0xe:
            *tgtmode = ARM_CPU_MODE_FIQ;
            *regno = sysm;
            break;
        case 0x10: /* 0b1000x : r14_irq, r13_irq */
        case 0x11:
            *tgtmode = ARM_CPU_MODE_IRQ;
            *regno = sysm & 1 ? 13 : 14;
            break;
        case 0x12: /* 0b1001x : r14_svc, r13_svc */
        case 0x13:
            *tgtmode = ARM_CPU_MODE_SVC;
            *regno = sysm & 1 ? 13 : 14;
            break;
        case 0x14: /* 0b1010x : r14_abt, r13_abt */
        case 0x15:
            *tgtmode = ARM_CPU_MODE_ABT;
            *regno = sysm & 1 ? 13 : 14;
            break;
        case 0x16: /* 0b1011x : r14_und, r13_und */
        case 0x17:
            *tgtmode = ARM_CPU_MODE_UND;
            *regno = sysm & 1 ? 13 : 14;
            break;
        case 0x1c: /* 0b1110x : r14_mon, r13_mon */
        case 0x1d:
            *tgtmode = ARM_CPU_MODE_MON;
            *regno = sysm & 1 ? 13 : 14;
            break;
        case 0x1e: /* 0b1111x : elr_hyp, r13_hyp */
        case 0x1f:
            *tgtmode = ARM_CPU_MODE_HYP;
            /* Arbitrarily pick 17 for ELR_Hyp (which is not a banked LR!) */
            *regno = sysm & 1 ? 13 : 17;
            break;
        default: /* unallocated */
            goto undef;
        }
    }

    /* Catch the 'accessing inaccessible register' cases we can detect
     * at translate time.
     */
    switch (*tgtmode) {
    case ARM_CPU_MODE_MON:
        if (!arm_dc_feature(s, ARM_FEATURE_EL3) || s->ns) {
            goto undef;
        }
        if (s->current_el == 1) {
            /* If we're in Secure EL1 (which implies that EL3 is AArch64)
             * then accesses to Mon registers trap to EL3
             */
            exc_target = 3;
            goto undef;
        }
        break;
    case ARM_CPU_MODE_HYP:
        /*
         * SPSR_hyp and r13_hyp can only be accessed from Monitor mode
         * (and so we can forbid accesses from EL2 or below). elr_hyp
         * can be accessed also from Hyp mode, so forbid accesses from
         * EL0 or EL1.
         */
        if (!arm_dc_feature(s, ARM_FEATURE_EL2) || s->current_el < 2 ||
            (s->current_el < 3 && *regno != 17)) {
            goto undef;
        }
        break;
    default:
        break;
    }

    return true;

undef:
    /* If we get here then some access check did not pass */
    gen_exception_insn(s, s->pc_curr, EXCP_UDEF,
                       syn_uncategorized(), exc_target);
    return false;
}

static void gen_msr_banked(DisasContext *s, int r, int sysm, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tcg_reg, tcg_tgtmode, tcg_regno;
    int tgtmode = 0, regno = 0;

    if (!msr_banked_access_decode(s, r, sysm, rn, &tgtmode, &regno)) {
        return;
    }

    /* Sync state because msr_banked() can raise exceptions */
    gen_set_condexec(s);
    gen_set_pc_im(s, s->pc_curr);
    tcg_reg = load_reg(s, rn);
    tcg_tgtmode = tcg_const_i32(tcg_ctx, tgtmode);
    tcg_regno = tcg_const_i32(tcg_ctx, regno);
    gen_helper_msr_banked(tcg_ctx, tcg_ctx->cpu_env, tcg_reg, tcg_tgtmode, tcg_regno);
    tcg_temp_free_i32(tcg_ctx, tcg_tgtmode);
    tcg_temp_free_i32(tcg_ctx, tcg_regno);
    tcg_temp_free_i32(tcg_ctx, tcg_reg);
    s->base.is_jmp = DISAS_UPDATE;
}

static void gen_mrs_banked(DisasContext *s, int r, int sysm, int rn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tcg_reg, tcg_tgtmode, tcg_regno;
    int tgtmode = 0, regno = 0;

    if (!msr_banked_access_decode(s, r, sysm, rn, &tgtmode, &regno)) {
        return;
    }

    /* Sync state because mrs_banked() can raise exceptions */
    gen_set_condexec(s);
    gen_set_pc_im(s, s->pc_curr);
    tcg_reg = tcg_temp_new_i32(tcg_ctx);
    tcg_tgtmode = tcg_const_i32(tcg_ctx, tgtmode);
    tcg_regno = tcg_const_i32(tcg_ctx, regno);
    gen_helper_mrs_banked(tcg_ctx, tcg_reg, tcg_ctx->cpu_env, tcg_tgtmode, tcg_regno);
    tcg_temp_free_i32(tcg_ctx, tcg_tgtmode);
    tcg_temp_free_i32(tcg_ctx, tcg_regno);
    store_reg(s, rn, tcg_reg);
    s->base.is_jmp = DISAS_UPDATE;
}

/* Store value to PC as for an exception return (ie don't
 * mask bits). The subsequent call to gen_helper_cpsr_write_eret()
 * will do the masking based on the new value of the Thumb bit.
 */
static void store_pc_exc_ret(DisasContext *s, TCGv_i32 pc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[15], pc);
    tcg_temp_free_i32(tcg_ctx, pc);
}

/* Generate a v6 exception return.  Marks both values as dead.  */
static void gen_rfe(DisasContext *s, TCGv_i32 pc, TCGv_i32 cpsr)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    store_pc_exc_ret(s, pc);
    /* The cpsr_write_eret helper will mask the low bits of PC
     * appropriately depending on the new Thumb bit, so it must
     * be called after storing the new PC.
     */
    gen_helper_cpsr_write_eret(tcg_ctx, tcg_ctx->cpu_env, cpsr);
    tcg_temp_free_i32(tcg_ctx, cpsr);
    /* Must exit loop to check un-masked IRQs */
    s->base.is_jmp = DISAS_EXIT;
}

/* Generate an old-style exception return. Marks pc as dead. */
static void gen_exception_return(DisasContext *s, TCGv_i32 pc)
{
    gen_rfe(s, pc, load_cpu_field(s, spsr));
}

#define CPU_V001 s->V0, s->V0, s->V1

static int gen_neon_unzip(DisasContext *s, int rd, int rm, int size, int q)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr pd, pm;

    if (!q && size == 2) {
        return 1;
    }
    pd = vfp_reg_ptr(s, true, rd);
    pm = vfp_reg_ptr(s, true, rm);
    if (q) {
        switch (size) {
        case 0:
            gen_helper_neon_qunzip8(tcg_ctx, pd, pm);
            break;
        case 1:
            gen_helper_neon_qunzip16(tcg_ctx, pd, pm);
            break;
        case 2:
            gen_helper_neon_qunzip32(tcg_ctx, pd, pm);
            break;
        default:
            abort();
        }
    } else {
        switch (size) {
        case 0:
            gen_helper_neon_unzip8(tcg_ctx, pd, pm);
            break;
        case 1:
            gen_helper_neon_unzip16(tcg_ctx, pd, pm);
            break;
        default:
            abort();
        }
    }
    tcg_temp_free_ptr(tcg_ctx, pd);
    tcg_temp_free_ptr(tcg_ctx, pm);
    return 0;
}

static int gen_neon_zip(DisasContext *s, int rd, int rm, int size, int q)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_ptr pd, pm;

    if (!q && size == 2) {
        return 1;
    }
    pd = vfp_reg_ptr(s, true, rd);
    pm = vfp_reg_ptr(s, true, rm);
    if (q) {
        switch (size) {
        case 0:
            gen_helper_neon_qzip8(tcg_ctx, pd, pm);
            break;
        case 1:
            gen_helper_neon_qzip16(tcg_ctx, pd, pm);
            break;
        case 2:
            gen_helper_neon_qzip32(tcg_ctx, pd, pm);
            break;
        default:
            abort();
        }
    } else {
        switch (size) {
        case 0:
            gen_helper_neon_zip8(tcg_ctx, pd, pm);
            break;
        case 1:
            gen_helper_neon_zip16(tcg_ctx, pd, pm);
            break;
        default:
            abort();
        }
    }
    tcg_temp_free_ptr(tcg_ctx, pd);
    tcg_temp_free_ptr(tcg_ctx, pm);
    return 0;
}

static void gen_neon_trn_u8(DisasContext *s, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 rd, tmp;

    rd = tcg_temp_new_i32(tcg_ctx);
    tmp = tcg_temp_new_i32(tcg_ctx);

    tcg_gen_shli_i32(tcg_ctx, rd, t0, 8);
    tcg_gen_andi_i32(tcg_ctx, rd, rd, 0xff00ff00);
    tcg_gen_andi_i32(tcg_ctx, tmp, t1, 0x00ff00ff);
    tcg_gen_or_i32(tcg_ctx, rd, rd, tmp);

    tcg_gen_shri_i32(tcg_ctx, t1, t1, 8);
    tcg_gen_andi_i32(tcg_ctx, t1, t1, 0x00ff00ff);
    tcg_gen_andi_i32(tcg_ctx, tmp, t0, 0xff00ff00);
    tcg_gen_or_i32(tcg_ctx, t1, t1, tmp);
    tcg_gen_mov_i32(tcg_ctx, t0, rd);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, rd);
}

static void gen_neon_trn_u16(DisasContext *s, TCGv_i32 t0, TCGv_i32 t1)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 rd, tmp;

    rd = tcg_temp_new_i32(tcg_ctx);
    tmp = tcg_temp_new_i32(tcg_ctx);

    tcg_gen_shli_i32(tcg_ctx, rd, t0, 16);
    tcg_gen_andi_i32(tcg_ctx, tmp, t1, 0xffff);
    tcg_gen_or_i32(tcg_ctx, rd, rd, tmp);
    tcg_gen_shri_i32(tcg_ctx, t1, t1, 16);
    tcg_gen_andi_i32(tcg_ctx, tmp, t0, 0xffff0000);
    tcg_gen_or_i32(tcg_ctx, t1, t1, tmp);
    tcg_gen_mov_i32(tcg_ctx, t0, rd);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, rd);
}

static inline void gen_neon_narrow(DisasContext *s, int size, TCGv_i32 dest, TCGv_i64 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (size) {
    case 0: gen_helper_neon_narrow_u8(tcg_ctx, dest, src); break;
    case 1: gen_helper_neon_narrow_u16(tcg_ctx, dest, src); break;
    case 2: tcg_gen_extrl_i64_i32(tcg_ctx, dest, src); break;
    default: abort();
    }
}

static inline void gen_neon_narrow_sats(DisasContext *s, int size, TCGv_i32 dest, TCGv_i64 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (size) {
    case 0: gen_helper_neon_narrow_sat_s8(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 1: gen_helper_neon_narrow_sat_s16(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 2: gen_helper_neon_narrow_sat_s32(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    default: abort();
    }
}

static inline void gen_neon_narrow_satu(DisasContext *s, int size, TCGv_i32 dest, TCGv_i64 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (size) {
    case 0: gen_helper_neon_narrow_sat_u8(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 1: gen_helper_neon_narrow_sat_u16(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 2: gen_helper_neon_narrow_sat_u32(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    default: abort();
    }
}

static inline void gen_neon_unarrow_sats(DisasContext *s, int size, TCGv_i32 dest, TCGv_i64 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (size) {
    case 0: gen_helper_neon_unarrow_sat8(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 1: gen_helper_neon_unarrow_sat16(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    case 2: gen_helper_neon_unarrow_sat32(tcg_ctx, dest, tcg_ctx->cpu_env, src); break;
    default: abort();
    }
}

static inline void gen_neon_widen(DisasContext *s, TCGv_i64 dest, TCGv_i32 src, int size, int u)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (u) {
        switch (size) {
        case 0: gen_helper_neon_widen_u8(tcg_ctx, dest, src); break;
        case 1: gen_helper_neon_widen_u16(tcg_ctx, dest, src); break;
        case 2: tcg_gen_extu_i32_i64(tcg_ctx, dest, src); break;
        default: abort();
        }
    } else {
        switch (size) {
        case 0: gen_helper_neon_widen_s8(tcg_ctx, dest, src); break;
        case 1: gen_helper_neon_widen_s16(tcg_ctx, dest, src); break;
        case 2: tcg_gen_ext_i32_i64(tcg_ctx, dest, src); break;
        default: abort();
        }
    }
    tcg_temp_free_i32(tcg_ctx, src);
}

static inline void gen_neon_addl(DisasContext *s, int size)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    switch (size) {
    case 0: gen_helper_neon_addl_u16(tcg_ctx, CPU_V001); break;
    case 1: gen_helper_neon_addl_u32(tcg_ctx, CPU_V001); break;
    case 2: tcg_gen_add_i64(tcg_ctx, CPU_V001); break;
    default: abort();
    }
}

static void gen_neon_narrow_op(DisasContext *s, int op, int u, int size,
                               TCGv_i32 dest, TCGv_i64 src)
{
    if (op) {
        if (u) {
            gen_neon_unarrow_sats(s, size, dest, src);
        } else {
            gen_neon_narrow(s, size, dest, src);
        }
    } else {
        if (u) {
            gen_neon_narrow_satu(s, size, dest, src);
        } else {
            gen_neon_narrow_sats(s, size, dest, src);
        }
    }
}

/* Symbolic constants for op fields for Neon 2-register miscellaneous.
 * The values correspond to bits [17:16,10:7]; see the ARM ARM DDI0406B
 * table A7-13.
 */
#define NEON_2RM_VREV64 0
#define NEON_2RM_VREV32 1
#define NEON_2RM_VREV16 2
#define NEON_2RM_VPADDL 4
#define NEON_2RM_VPADDL_U 5
#define NEON_2RM_AESE 6 /* Includes AESD */
#define NEON_2RM_AESMC 7 /* Includes AESIMC */
#define NEON_2RM_VCLS 8
#define NEON_2RM_VCLZ 9
#define NEON_2RM_VCNT 10
#define NEON_2RM_VMVN 11
#define NEON_2RM_VPADAL 12
#define NEON_2RM_VPADAL_U 13
#define NEON_2RM_VQABS 14
#define NEON_2RM_VQNEG 15
#define NEON_2RM_VCGT0 16
#define NEON_2RM_VCGE0 17
#define NEON_2RM_VCEQ0 18
#define NEON_2RM_VCLE0 19
#define NEON_2RM_VCLT0 20
#define NEON_2RM_SHA1H 21
#define NEON_2RM_VABS 22
#define NEON_2RM_VNEG 23
#define NEON_2RM_VCGT0_F 24
#define NEON_2RM_VCGE0_F 25
#define NEON_2RM_VCEQ0_F 26
#define NEON_2RM_VCLE0_F 27
#define NEON_2RM_VCLT0_F 28
#define NEON_2RM_VABS_F 30
#define NEON_2RM_VNEG_F 31
#define NEON_2RM_VSWP 32
#define NEON_2RM_VTRN 33
#define NEON_2RM_VUZP 34
#define NEON_2RM_VZIP 35
#define NEON_2RM_VMOVN 36 /* Includes VQMOVN, VQMOVUN */
#define NEON_2RM_VQMOVN 37 /* Includes VQMOVUN */
#define NEON_2RM_VSHLL 38
#define NEON_2RM_SHA1SU1 39 /* Includes SHA256SU0 */
#define NEON_2RM_VRINTN 40
#define NEON_2RM_VRINTX 41
#define NEON_2RM_VRINTA 42
#define NEON_2RM_VRINTZ 43
#define NEON_2RM_VCVT_F16_F32 44
#define NEON_2RM_VRINTM 45
#define NEON_2RM_VCVT_F32_F16 46
#define NEON_2RM_VRINTP 47
#define NEON_2RM_VCVTAU 48
#define NEON_2RM_VCVTAS 49
#define NEON_2RM_VCVTNU 50
#define NEON_2RM_VCVTNS 51
#define NEON_2RM_VCVTPU 52
#define NEON_2RM_VCVTPS 53
#define NEON_2RM_VCVTMU 54
#define NEON_2RM_VCVTMS 55
#define NEON_2RM_VRECPE 56
#define NEON_2RM_VRSQRTE 57
#define NEON_2RM_VRECPE_F 58
#define NEON_2RM_VRSQRTE_F 59
#define NEON_2RM_VCVT_FS 60
#define NEON_2RM_VCVT_FU 61
#define NEON_2RM_VCVT_SF 62
#define NEON_2RM_VCVT_UF 63

static bool neon_2rm_is_v8_op(int op)
{
    /* Return true if this neon 2reg-misc op is ARMv8 and up */
    switch (op) {
    case NEON_2RM_VRINTN:
    case NEON_2RM_VRINTA:
    case NEON_2RM_VRINTM:
    case NEON_2RM_VRINTP:
    case NEON_2RM_VRINTZ:
    case NEON_2RM_VRINTX:
    case NEON_2RM_VCVTAU:
    case NEON_2RM_VCVTAS:
    case NEON_2RM_VCVTNU:
    case NEON_2RM_VCVTNS:
    case NEON_2RM_VCVTPU:
    case NEON_2RM_VCVTPS:
    case NEON_2RM_VCVTMU:
    case NEON_2RM_VCVTMS:
        return true;
    default:
        return false;
    }
}

/* Each entry in this array has bit n set if the insn allows
 * size value n (otherwise it will UNDEF). Since unallocated
 * op values will have no bits set they always UNDEF.
 */
static const uint8_t neon_2rm_sizes[] = {
    [NEON_2RM_VREV64] = 0x7,
    [NEON_2RM_VREV32] = 0x3,
    [NEON_2RM_VREV16] = 0x1,
    [NEON_2RM_VPADDL] = 0x7,
    [NEON_2RM_VPADDL_U] = 0x7,
    [NEON_2RM_AESE] = 0x1,
    [NEON_2RM_AESMC] = 0x1,
    [NEON_2RM_VCLS] = 0x7,
    [NEON_2RM_VCLZ] = 0x7,
    [NEON_2RM_VCNT] = 0x1,
    [NEON_2RM_VMVN] = 0x1,
    [NEON_2RM_VPADAL] = 0x7,
    [NEON_2RM_VPADAL_U] = 0x7,
    [NEON_2RM_VQABS] = 0x7,
    [NEON_2RM_VQNEG] = 0x7,
    [NEON_2RM_VCGT0] = 0x7,
    [NEON_2RM_VCGE0] = 0x7,
    [NEON_2RM_VCEQ0] = 0x7,
    [NEON_2RM_VCLE0] = 0x7,
    [NEON_2RM_VCLT0] = 0x7,
    [NEON_2RM_SHA1H] = 0x4,
    [NEON_2RM_VABS] = 0x7,
    [NEON_2RM_VNEG] = 0x7,
    [NEON_2RM_VCGT0_F] = 0x4,
    [NEON_2RM_VCGE0_F] = 0x4,
    [NEON_2RM_VCEQ0_F] = 0x4,
    [NEON_2RM_VCLE0_F] = 0x4,
    [NEON_2RM_VCLT0_F] = 0x4,
    [NEON_2RM_VABS_F] = 0x4,
    [NEON_2RM_VNEG_F] = 0x4,
    [NEON_2RM_VSWP] = 0x1,
    [NEON_2RM_VTRN] = 0x7,
    [NEON_2RM_VUZP] = 0x7,
    [NEON_2RM_VZIP] = 0x7,
    [NEON_2RM_VMOVN] = 0x7,
    [NEON_2RM_VQMOVN] = 0x7,
    [NEON_2RM_VSHLL] = 0x7,
    [NEON_2RM_SHA1SU1] = 0x4,
    [NEON_2RM_VRINTN] = 0x4,
    [NEON_2RM_VRINTX] = 0x4,
    [NEON_2RM_VRINTA] = 0x4,
    [NEON_2RM_VRINTZ] = 0x4,
    [NEON_2RM_VCVT_F16_F32] = 0x2,
    [NEON_2RM_VRINTM] = 0x4,
    [NEON_2RM_VCVT_F32_F16] = 0x2,
    [NEON_2RM_VRINTP] = 0x4,
    [NEON_2RM_VCVTAU] = 0x4,
    [NEON_2RM_VCVTAS] = 0x4,
    [NEON_2RM_VCVTNU] = 0x4,
    [NEON_2RM_VCVTNS] = 0x4,
    [NEON_2RM_VCVTPU] = 0x4,
    [NEON_2RM_VCVTPS] = 0x4,
    [NEON_2RM_VCVTMU] = 0x4,
    [NEON_2RM_VCVTMS] = 0x4,
    [NEON_2RM_VRECPE] = 0x4,
    [NEON_2RM_VRSQRTE] = 0x4,
    [NEON_2RM_VRECPE_F] = 0x4,
    [NEON_2RM_VRSQRTE_F] = 0x4,
    [NEON_2RM_VCVT_FS] = 0x4,
    [NEON_2RM_VCVT_FU] = 0x4,
    [NEON_2RM_VCVT_SF] = 0x4,
    [NEON_2RM_VCVT_UF] = 0x4,
};

static void gen_gvec_fn3_qc(TCGContext *s, uint32_t rd_ofs, uint32_t rn_ofs, uint32_t rm_ofs,
                            uint32_t opr_sz, uint32_t max_sz,
                            gen_helper_gvec_3_ptr *fn)
{
    TCGv_ptr qc_ptr = tcg_temp_new_ptr(s);

    tcg_gen_addi_ptr(s, qc_ptr, s->cpu_env, offsetof(CPUARMState, vfp.qc));
    tcg_gen_gvec_3_ptr(s, rd_ofs, rn_ofs, rm_ofs, qc_ptr,
                       opr_sz, max_sz, 0, fn);
    tcg_temp_free_ptr(s, qc_ptr);
}

void gen_gvec_sqrdmlah_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                          uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static gen_helper_gvec_3_ptr * const fns[2] = {
        gen_helper_gvec_qrdmlah_s16, gen_helper_gvec_qrdmlah_s32
    };
    tcg_debug_assert(vece >= 1 && vece <= 2);
    gen_gvec_fn3_qc(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, fns[vece - 1]);
}

void gen_gvec_sqrdmlsh_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                          uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static gen_helper_gvec_3_ptr * const fns[2] = {
        gen_helper_gvec_qrdmlsh_s16, gen_helper_gvec_qrdmlsh_s32
    };
    tcg_debug_assert(vece >= 1 && vece <= 2);
    gen_gvec_fn3_qc(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, fns[vece - 1]);
}

#define GEN_CMP0(NAME, COND)                                            \
    static void gen_##NAME##0_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a) \
    {                                                                   \
        tcg_gen_setcondi_i32(s, COND, d, a, 0);                         \
        tcg_gen_neg_i32(s, d, d);                                       \
    }                                                                   \
    static void gen_##NAME##0_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a)  \
    {                                                                   \
        tcg_gen_setcondi_i64(s, COND, d, a, 0);                         \
        tcg_gen_neg_i64(s, d, d);                                       \
    }                                                                   \
    static void gen_##NAME##0_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a) \
    {                                                                   \
        TCGv_vec zero = tcg_const_zeros_vec_matching(s, d);             \
        tcg_gen_cmp_vec(s, COND, vece, d, a, zero);                     \
        tcg_temp_free_vec(s, zero);                                     \
    }                                                                   \
    void gen_gvec_##NAME##0(TCGContext *s, unsigned vece, uint32_t d, uint32_t m,      \
                            uint32_t opr_sz, uint32_t max_sz)           \
    {                                                                   \
        const GVecGen2 op[4] = {                                        \
            { .fno = gen_helper_gvec_##NAME##0_b,                       \
              .fniv = gen_##NAME##0_vec,                                \
              .opt_opc = vecop_list_cmp,                                \
              .vece = MO_8 },                                           \
            { .fno = gen_helper_gvec_##NAME##0_h,                       \
              .fniv = gen_##NAME##0_vec,                                \
              .opt_opc = vecop_list_cmp,                                \
              .vece = MO_16 },                                          \
            { .fni4 = gen_##NAME##0_i32,                                \
              .fniv = gen_##NAME##0_vec,                                \
              .opt_opc = vecop_list_cmp,                                \
              .vece = MO_32 },                                          \
            { .fni8 = gen_##NAME##0_i64,                                \
              .fniv = gen_##NAME##0_vec,                                \
              .opt_opc = vecop_list_cmp,                                \
              .prefer_i64 = TCG_TARGET_REG_BITS == 64,                  \
              .vece = MO_64 },                                          \
        };                                                              \
        tcg_gen_gvec_2(s, d, m, opr_sz, max_sz, &op[vece]);             \
    }


static const TCGOpcode vecop_list_cmp[] = {
    INDEX_op_cmp_vec, 0
};

GEN_CMP0(ceq, TCG_COND_EQ)
GEN_CMP0(cle, TCG_COND_LE)
GEN_CMP0(cge, TCG_COND_GE)
GEN_CMP0(clt, TCG_COND_LT)
GEN_CMP0(cgt, TCG_COND_GT)
#undef GEN_CMP0

static void gen_ssra8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_vec_sar8i_i64(s, a, a, shift);
    tcg_gen_vec_add8_i64(s, d, d, a);
}

static void gen_ssra16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_vec_sar16i_i64(s, a, a, shift);
    tcg_gen_vec_add16_i64(s, d, d, a);
}

static void gen_ssra32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t shift)
{
    tcg_gen_sari_i32(s, a, a, shift);
    tcg_gen_add_i32(s, d, d, a);
}

static void gen_ssra64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_sari_i64(s, a, a, shift);
    tcg_gen_add_i64(s, d, d, a);
}

static void gen_ssra_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    tcg_gen_sari_vec(s, vece, a, a, sh);
    tcg_gen_add_vec(s, vece, d, d, a);
}

void gen_gvec_ssra(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                   int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sari_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_ssra8_i64,
          .fniv = gen_ssra_vec,
          .fno = gen_helper_gvec_ssra_b,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_ssra16_i64,
          .fniv = gen_ssra_vec,
          .fno = gen_helper_gvec_ssra_h,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_ssra32_i32,
          .fniv = gen_ssra_vec,
          .fno = gen_helper_gvec_ssra_s,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_ssra64_i64,
          .fniv = gen_ssra_vec,
          .fno = gen_helper_gvec_ssra_b,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_64 },
    };
    /* tszimm encoding produces immediates in the range [1..esize]. */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    /*
     * Shifts larger than the element size are architecturally valid.
     * Signed results in all sign bits.
     */
    shift = MIN(shift, (8 << vece) - 1);
    tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
}

static void gen_usra8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_vec_shr8i_i64(s, a, a, shift);
    tcg_gen_vec_add8_i64(s, d, d, a);
}

static void gen_usra16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_vec_shr16i_i64(s, a, a, shift);
    tcg_gen_vec_add16_i64(s, d, d, a);
}

static void gen_usra32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t shift)
{
    tcg_gen_shri_i32(s, a, a, shift);
    tcg_gen_add_i32(s, d, d, a);
}

static void gen_usra64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_shri_i64(s, a, a, shift);
    tcg_gen_add_i64(s, d, d, a);
}

static void gen_usra_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    tcg_gen_shri_vec(s, vece, a, a, sh);
    tcg_gen_add_vec(s, vece, d, d, a);
}

void gen_gvec_usra(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                   int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_usra8_i64,
          .fniv = gen_usra_vec,
          .fno = gen_helper_gvec_usra_b,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8, },
        { .fni8 = gen_usra16_i64,
          .fniv = gen_usra_vec,
          .fno = gen_helper_gvec_usra_h,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16, },
        { .fni4 = gen_usra32_i32,
          .fniv = gen_usra_vec,
          .fno = gen_helper_gvec_usra_s,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32, },
        { .fni8 = gen_usra64_i64,
          .fniv = gen_usra_vec,
          .fno = gen_helper_gvec_usra_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_64, },
    };
    /* tszimm encoding produces immediates in the range [1..esize]. */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    /*
     * Shifts larger than the element size are architecturally valid.
     * Unsigned results in all zeros as input to accumulate: nop.
     */
    if (shift < (8 << vece)) {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    } else {
        /* Nop, but we do need to clear the tail. */
        tcg_gen_gvec_mov(s, vece, rd_ofs, rd_ofs, opr_sz, max_sz);
    }
}

/*
 * Shift one less than the requested amount, and the low bit is
 * the rounding bit.  For the 8 and 16-bit operations, because we
 * mask the low bit, we can perform a normal integer shift instead
 * of a vector shift.
 */
static void gen_srshr8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, sh - 1);
    tcg_gen_andi_i64(s, t, t, dup_const(MO_8, 1));
    tcg_gen_vec_sar8i_i64(s, d, a, sh);
    tcg_gen_vec_add8_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srshr16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, sh - 1);
    tcg_gen_andi_i64(s, t, t, dup_const(MO_16, 1));
    tcg_gen_vec_sar16i_i64(s, d, a, sh);
    tcg_gen_vec_add16_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srshr32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t sh)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    tcg_gen_extract_i32(s, t, a, sh - 1, 1);
    tcg_gen_sari_i32(s, d, a, sh);
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_srshr64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_extract_i64(s, t, a, sh - 1, 1);
    tcg_gen_sari_i64(s, d, a, sh);
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srshr_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    TCGv_vec ones = tcg_temp_new_vec_matching(s, d);

    tcg_gen_shri_vec(s, vece, t, a, sh - 1);
    tcg_gen_dupi_vec(s, vece, ones, 1);
    tcg_gen_and_vec(s, vece, t, t, ones);
    tcg_gen_sari_vec(s, vece, d, a, sh);
    tcg_gen_add_vec(s, vece, d, d, t);

    tcg_temp_free_vec(s, t);
    tcg_temp_free_vec(s, ones);
}

void gen_gvec_srshr(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                    int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_srshr8_i64,
          .fniv = gen_srshr_vec,
          .fno = gen_helper_gvec_srshr_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_srshr16_i64,
          .fniv = gen_srshr_vec,
          .fno = gen_helper_gvec_srshr_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_srshr32_i32,
          .fniv = gen_srshr_vec,
          .fno = gen_helper_gvec_srshr_s,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_srshr64_i64,
          .fniv = gen_srshr_vec,
          .fno = gen_helper_gvec_srshr_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [1..esize] */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    if (shift == (8 << vece)) {
        /*
         * Shifts larger than the element size are architecturally valid.
         * Signed results in all sign bits.  With rounding, this produces
         *   (-1 + 1) >> 1 == 0, or (0 + 1) >> 1 == 0.
         * I.e. always zero.
         */
        tcg_gen_gvec_dup_imm(s, vece, rd_ofs, opr_sz, max_sz, 0);
    } else {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    }
}

static void gen_srsra8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    gen_srshr8_i64(s, t, a, sh);
    tcg_gen_vec_add8_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srsra16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    gen_srshr16_i64(s, t, a, sh);
    tcg_gen_vec_add16_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srsra32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t sh)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    gen_srshr32_i32(s, t, a, sh);
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_srsra64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    gen_srshr64_i64(s, t, a, sh);
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_srsra_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);

    gen_srshr_vec(s, vece, t, a, sh);
    tcg_gen_add_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_srsra(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                    int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_srsra8_i64,
          .fniv = gen_srsra_vec,
          .fno = gen_helper_gvec_srsra_b,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_8 },
        { .fni8 = gen_srsra16_i64,
          .fniv = gen_srsra_vec,
          .fno = gen_helper_gvec_srsra_h,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_16 },
        { .fni4 = gen_srsra32_i32,
          .fniv = gen_srsra_vec,
          .fno = gen_helper_gvec_srsra_s,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_32 },
        { .fni8 = gen_srsra64_i64,
          .fniv = gen_srsra_vec,
          .fno = gen_helper_gvec_srsra_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [1..esize] */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    /*
     * Shifts larger than the element size are architecturally valid.
     * Signed results in all sign bits.  With rounding, this produces
     *   (-1 + 1) >> 1 == 0, or (0 + 1) >> 1 == 0.
     * I.e. always zero.  With accumulation, this leaves D unchanged.
     */
    if (shift == (8 << vece)) {
        /* Nop, but we do need to clear the tail. */
        tcg_gen_gvec_mov(s, vece, rd_ofs, rd_ofs, opr_sz, max_sz);
    } else {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    }
}

static void gen_urshr8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, sh - 1);
    tcg_gen_andi_i64(s, t, t, dup_const(MO_8, 1));
    tcg_gen_vec_shr8i_i64(s, d, a, sh);
    tcg_gen_vec_add8_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_urshr16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, sh - 1);
    tcg_gen_andi_i64(s, t, t, dup_const(MO_16, 1));
    tcg_gen_vec_shr16i_i64(s, d, a, sh);
    tcg_gen_vec_add16_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_urshr32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t sh)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    tcg_gen_extract_i32(s, t, a, sh - 1, 1);
    tcg_gen_shri_i32(s, d, a, sh);
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_urshr64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_extract_i64(s, t, a, sh - 1, 1);
    tcg_gen_shri_i64(s, d, a, sh);
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_urshr_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t shift)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    TCGv_vec ones = tcg_temp_new_vec_matching(s, d);

    tcg_gen_shri_vec(s, vece, t, a, shift - 1);
    tcg_gen_dupi_vec(s, vece, ones, 1);
    tcg_gen_and_vec(s, vece, t, t, ones);
    tcg_gen_shri_vec(s, vece, d, a, shift);
    tcg_gen_add_vec(s, vece, d, d, t);

    tcg_temp_free_vec(s, t);
    tcg_temp_free_vec(s, ones);
}

void gen_gvec_urshr(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                    int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_urshr8_i64,
          .fniv = gen_urshr_vec,
          .fno = gen_helper_gvec_urshr_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_urshr16_i64,
          .fniv = gen_urshr_vec,
          .fno = gen_helper_gvec_urshr_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_urshr32_i32,
          .fniv = gen_urshr_vec,
          .fno = gen_helper_gvec_urshr_s,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_urshr64_i64,
          .fniv = gen_urshr_vec,
          .fno = gen_helper_gvec_urshr_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [1..esize] */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    if (shift == (8 << vece)) {
        /*
         * Shifts larger than the element size are architecturally valid.
         * Unsigned results in zero.  With rounding, this produces a
         * copy of the most significant bit.
         */
        tcg_gen_gvec_shri(s, vece, rd_ofs, rm_ofs, shift - 1, opr_sz, max_sz);
    } else {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    }
}

static void gen_ursra8_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    if (sh == 8) {
        tcg_gen_vec_shr8i_i64(s, t, a, 7);
    } else {
        gen_urshr8_i64(s, t, a, sh);
    }
    tcg_gen_vec_add8_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_ursra16_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    if (sh == 16) {
        tcg_gen_vec_shr16i_i64(s, t, a, 15);
    } else {
        gen_urshr16_i64(s, t, a, sh);
    }
    tcg_gen_vec_add16_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_ursra32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t sh)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    if (sh == 32) {
        tcg_gen_shri_i32(s, t, a, 31);
    } else {
        gen_urshr32_i32(s, t, a, sh);
    }
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_ursra64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t sh)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    if (sh == 64) {
        tcg_gen_shri_i64(s, t, a, 63);
    } else {
        gen_urshr64_i64(s, t, a, sh);
    }
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_ursra_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);

    if (sh == (8 << vece)) {
        tcg_gen_shri_vec(s, vece, t, a, sh - 1);
    } else {
        gen_urshr_vec(s, vece, t, a, sh);
    }
    tcg_gen_add_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_ursra(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                    int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen2i ops[4] = {
        { .fni8 = gen_ursra8_i64,
          .fniv = gen_ursra_vec,
          .fno = gen_helper_gvec_ursra_b,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_8 },
        { .fni8 = gen_ursra16_i64,
          .fniv = gen_ursra_vec,
          .fno = gen_helper_gvec_ursra_h,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_16 },
        { .fni4 = gen_ursra32_i32,
          .fniv = gen_ursra_vec,
          .fno = gen_helper_gvec_ursra_s,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_32 },
        { .fni8 = gen_ursra64_i64,
          .fniv = gen_ursra_vec,
          .fno = gen_helper_gvec_ursra_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [1..esize] */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
}

static void gen_shr8_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    uint64_t mask = dup_const(MO_8, 0xff >> shift);
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, shift);
    tcg_gen_andi_i64(s, t, t, mask);
    tcg_gen_andi_i64(s, d, d, ~mask);
    tcg_gen_or_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_shr16_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    uint64_t mask = dup_const(MO_16, 0xffff >> shift);
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shri_i64(s, t, a, shift);
    tcg_gen_andi_i64(s, t, t, mask);
    tcg_gen_andi_i64(s, d, d, ~mask);
    tcg_gen_or_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_shr32_ins_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t shift)
{
    tcg_gen_shri_i32(s, a, a, shift);
    tcg_gen_deposit_i32(s, d, d, a, 0, 32 - shift);
}

static void gen_shr64_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_shri_i64(s, a, a, shift);
    tcg_gen_deposit_i64(s, d, d, a, 0, 64 - shift);
}

static void gen_shr_ins_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    TCGv_vec m = tcg_temp_new_vec_matching(s, d);

    tcg_gen_dupi_vec(s, vece, m, MAKE_64BIT_MASK((8 << vece) - sh, sh));
    tcg_gen_shri_vec(s, vece, t, a, sh);
    tcg_gen_and_vec(s, vece, d, d, m);
    tcg_gen_or_vec(s, vece, d, d, t);

    tcg_temp_free_vec(s, t);
    tcg_temp_free_vec(s, m);
}

void gen_gvec_sri(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                  int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = { INDEX_op_shri_vec, 0 };
    const GVecGen2i ops[4] = {
        { .fni8 = gen_shr8_ins_i64,
          .fniv = gen_shr_ins_vec,
          .fno = gen_helper_gvec_sri_b,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_shr16_ins_i64,
          .fniv = gen_shr_ins_vec,
          .fno = gen_helper_gvec_sri_h,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_shr32_ins_i32,
          .fniv = gen_shr_ins_vec,
          .fno = gen_helper_gvec_sri_s,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_shr64_ins_i64,
          .fniv = gen_shr_ins_vec,
          .fno = gen_helper_gvec_sri_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [1..esize]. */
    tcg_debug_assert(shift > 0);
    tcg_debug_assert(shift <= (8 << vece));

    /* Shift of esize leaves destination unchanged. */
    if (shift < (8 << vece)) {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    } else {
        /* Nop, but we do need to clear the tail. */
        tcg_gen_gvec_mov(s, vece, rd_ofs, rd_ofs, opr_sz, max_sz);
    }
}

static void gen_shl8_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    uint64_t mask = dup_const(MO_8, 0xff << shift);
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shli_i64(s, t, a, shift);
    tcg_gen_andi_i64(s, t, t, mask);
    tcg_gen_andi_i64(s, d, d, ~mask);
    tcg_gen_or_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_shl16_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    uint64_t mask = dup_const(MO_16, 0xffff << shift);
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_shli_i64(s, t, a, shift);
    tcg_gen_andi_i64(s, t, t, mask);
    tcg_gen_andi_i64(s, d, d, ~mask);
    tcg_gen_or_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_shl32_ins_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, int32_t shift)
{
    tcg_gen_deposit_i32(s, d, d, a, shift, 32 - shift);
}

static void gen_shl64_ins_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, int64_t shift)
{
    tcg_gen_deposit_i64(s, d, d, a, shift, 64 - shift);
}

static void gen_shl_ins_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, int64_t sh)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    TCGv_vec m = tcg_temp_new_vec_matching(s, d);

    tcg_gen_shli_vec(s, vece, t, a, sh);
    tcg_gen_dupi_vec(s, vece, m, MAKE_64BIT_MASK(0, sh));
    tcg_gen_and_vec(s, vece, d, d, m);
    tcg_gen_or_vec(s, vece, d, d, t);

    tcg_temp_free_vec(s, t);
    tcg_temp_free_vec(s, m);
}

void gen_gvec_sli(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rm_ofs,
                  int64_t shift, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = { INDEX_op_shli_vec, 0 };
    const GVecGen2i ops[4] = {
        { .fni8 = gen_shl8_ins_i64,
          .fniv = gen_shl_ins_vec,
          .fno = gen_helper_gvec_sli_b,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_shl16_ins_i64,
          .fniv = gen_shl_ins_vec,
          .fno = gen_helper_gvec_sli_h,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_shl32_ins_i32,
          .fniv = gen_shl_ins_vec,
          .fno = gen_helper_gvec_sli_s,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_shl64_ins_i64,
          .fniv = gen_shl_ins_vec,
          .fno = gen_helper_gvec_sli_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };

    /* tszimm encoding produces immediates in the range [0..esize-1]. */
    tcg_debug_assert(shift >= 0);
    tcg_debug_assert(shift < (8 << vece));

    if (shift == 0) {
        tcg_gen_gvec_mov(s, vece, rd_ofs, rm_ofs, opr_sz, max_sz);
    } else {
        tcg_gen_gvec_2i(s, rd_ofs, rm_ofs, opr_sz, max_sz, shift, &ops[vece]);
    }
}

static void gen_mla8_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    gen_helper_neon_mul_u8(s, a, a, b);
    gen_helper_neon_add_u8(s, d, d, a);
}

static void gen_mls8_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    gen_helper_neon_mul_u8(s, a, a, b);
    gen_helper_neon_sub_u8(s, d, d, a);
}

static void gen_mla16_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    gen_helper_neon_mul_u16(s, a, a, b);
    gen_helper_neon_add_u16(s, d, d, a);
}

static void gen_mls16_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    gen_helper_neon_mul_u16(s, a, a, b);
    gen_helper_neon_sub_u16(s, d, d, a);
}

static void gen_mla32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    tcg_gen_mul_i32(s, a, a, b);
    tcg_gen_add_i32(s, d, d, a);
}

static void gen_mls32_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    tcg_gen_mul_i32(s, a, a, b);
    tcg_gen_sub_i32(s, d, d, a);
}

static void gen_mla64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    tcg_gen_mul_i64(s, a, a, b);
    tcg_gen_add_i64(s, d, d, a);
}

static void gen_mls64_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    tcg_gen_mul_i64(s, a, a, b);
    tcg_gen_sub_i64(s, d, d, a);
}

static void gen_mla_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_mul_vec(s, vece, a, a, b);
    tcg_gen_add_vec(s, vece, d, d, a);
}

static void gen_mls_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_mul_vec(s, vece, a, a, b);
    tcg_gen_sub_vec(s, vece, d, d, a);
}

/* Note that while NEON does not support VMLA and VMLS as 64-bit ops,
 * these tables are shared with AArch64 which does support them.
 */
void gen_gvec_mla(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                  uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_mul_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fni4 = gen_mla8_i32,
          .fniv = gen_mla_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni4 = gen_mla16_i32,
          .fniv = gen_mla_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_mla32_i32,
          .fniv = gen_mla_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_mla64_i64,
          .fniv = gen_mla_vec,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

void gen_gvec_mls(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                  uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_mul_vec, INDEX_op_sub_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fni4 = gen_mls8_i32,
          .fniv = gen_mls_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni4 = gen_mls16_i32,
          .fniv = gen_mls_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_mls32_i32,
          .fniv = gen_mls_vec,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_mls64_i64,
          .fniv = gen_mls_vec,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .load_dest = true,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

/* CMTST : test is "if (X & Y != 0)". */
static void gen_cmtst_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    tcg_gen_and_i32(s, d, a, b);
    tcg_gen_setcondi_i32(s, TCG_COND_NE, d, d, 0);
    tcg_gen_neg_i32(s, d, d);
}

void gen_cmtst_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    tcg_gen_and_i64(s, d, a, b);
    tcg_gen_setcondi_i64(s, TCG_COND_NE, d, d, 0);
    tcg_gen_neg_i64(s, d, d);
}

static void gen_cmtst_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_and_vec(s, vece, d, a, b);
    tcg_gen_dupi_vec(s, vece, a, 0);
    tcg_gen_cmp_vec(s, TCG_COND_NE, vece, d, d, a);
}

void gen_gvec_cmtst(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                    uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = { INDEX_op_cmp_vec, 0 };
    static const GVecGen3 ops[4] = {
        { .fni4 = gen_helper_neon_tst_u8,
          .fniv = gen_cmtst_vec,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni4 = gen_helper_neon_tst_u16,
          .fniv = gen_cmtst_vec,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_cmtst_i32,
          .fniv = gen_cmtst_vec,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_cmtst_i64,
          .fniv = gen_cmtst_vec,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

void gen_ushl_i32(TCGContext *tcg_ctx, TCGv_i32 dst, TCGv_i32 src, TCGv_i32 shift)
{
    TCGv_i32 lval = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 rval = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 lsh = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 rsh = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 zero = tcg_const_i32(tcg_ctx, 0);
    TCGv_i32 max = tcg_const_i32(tcg_ctx, 32);

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_ext8s_i32(tcg_ctx, lsh, shift);
    tcg_gen_neg_i32(tcg_ctx, rsh, lsh);
    tcg_gen_shl_i32(tcg_ctx, lval, src, lsh);
    tcg_gen_shr_i32(tcg_ctx, rval, src, rsh);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LTU, dst, lsh, max, lval, zero);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LTU, dst, rsh, max, rval, dst);

    tcg_temp_free_i32(tcg_ctx, lval);
    tcg_temp_free_i32(tcg_ctx, rval);
    tcg_temp_free_i32(tcg_ctx, lsh);
    tcg_temp_free_i32(tcg_ctx, rsh);
    tcg_temp_free_i32(tcg_ctx, zero);
    tcg_temp_free_i32(tcg_ctx, max);
}

void gen_ushl_i64(TCGContext *tcg_ctx, TCGv_i64 dst, TCGv_i64 src, TCGv_i64 shift)
{
    TCGv_i64 lval = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rval = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 lsh = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rsh = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 zero = tcg_const_i64(tcg_ctx, 0);
    TCGv_i64 max = tcg_const_i64(tcg_ctx, 64);

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_ext8s_i64(tcg_ctx, lsh, shift);
    tcg_gen_neg_i64(tcg_ctx, rsh, lsh);
    tcg_gen_shl_i64(tcg_ctx, lval, src, lsh);
    tcg_gen_shr_i64(tcg_ctx, rval, src, rsh);
    tcg_gen_movcond_i64(tcg_ctx, TCG_COND_LTU, dst, lsh, max, lval, zero);
    tcg_gen_movcond_i64(tcg_ctx, TCG_COND_LTU, dst, rsh, max, rval, dst);

    tcg_temp_free_i64(tcg_ctx, lval);
    tcg_temp_free_i64(tcg_ctx, rval);
    tcg_temp_free_i64(tcg_ctx, lsh);
    tcg_temp_free_i64(tcg_ctx, rsh);
    tcg_temp_free_i64(tcg_ctx, zero);
    tcg_temp_free_i64(tcg_ctx, max);
}

static void gen_ushl_vec(TCGContext *tcg_ctx, unsigned vece, TCGv_vec dst,
                         TCGv_vec src, TCGv_vec shift)
{
    TCGv_vec lval = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec rval = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec lsh = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec rsh = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec msk, max;

    tcg_gen_neg_vec(tcg_ctx, vece, rsh, shift);
    if (vece == MO_8) {
        tcg_gen_mov_vec(tcg_ctx, lsh, shift);
    } else {
        msk = tcg_temp_new_vec_matching(tcg_ctx, dst);
        tcg_gen_dupi_vec(tcg_ctx, vece, msk, 0xff);
        tcg_gen_and_vec(tcg_ctx, vece, lsh, shift, msk);
        tcg_gen_and_vec(tcg_ctx, vece, rsh, rsh, msk);
        tcg_temp_free_vec(tcg_ctx, msk);
    }

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_shlv_vec(tcg_ctx, vece, lval, src, lsh);
    tcg_gen_shrv_vec(tcg_ctx, vece, rval, src, rsh);

    max = tcg_temp_new_vec_matching(tcg_ctx, dst);
    tcg_gen_dupi_vec(tcg_ctx, vece, max, 8 << vece);

    /*
     * The choice of LT (signed) and GEU (unsigned) are biased toward
     * the instructions of the x86_64 host.  For MO_8, the whole byte
     * is significant so we must use an unsigned compare; otherwise we
     * have already masked to a byte and so a signed compare works.
     * Other tcg hosts have a full set of comparisons and do not care.
     */
    if (vece == MO_8) {
        tcg_gen_cmp_vec(tcg_ctx, TCG_COND_GEU, vece, lsh, lsh, max);
        tcg_gen_cmp_vec(tcg_ctx, TCG_COND_GEU, vece, rsh, rsh, max);
        tcg_gen_andc_vec(tcg_ctx, vece, lval, lval, lsh);
        tcg_gen_andc_vec(tcg_ctx, vece, rval, rval, rsh);
    } else {
        tcg_gen_cmp_vec(tcg_ctx, TCG_COND_LT, vece, lsh, lsh, max);
        tcg_gen_cmp_vec(tcg_ctx, TCG_COND_LT, vece, rsh, rsh, max);
        tcg_gen_and_vec(tcg_ctx, vece, lval, lval, lsh);
        tcg_gen_and_vec(tcg_ctx, vece, rval, rval, rsh);
    }
    tcg_gen_or_vec(tcg_ctx, vece, dst, lval, rval);

    tcg_temp_free_vec(tcg_ctx, max);
    tcg_temp_free_vec(tcg_ctx, lval);
    tcg_temp_free_vec(tcg_ctx, rval);
    tcg_temp_free_vec(tcg_ctx, lsh);
    tcg_temp_free_vec(tcg_ctx, rsh);
}

void gen_gvec_ushl(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_neg_vec, INDEX_op_shlv_vec,
        INDEX_op_shrv_vec, INDEX_op_cmp_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_ushl_vec,
          .fno = gen_helper_gvec_ushl_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fniv = gen_ushl_vec,
          .fno = gen_helper_gvec_ushl_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_ushl_i32,
          .fniv = gen_ushl_vec,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_ushl_i64,
          .fniv = gen_ushl_vec,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

void gen_sshl_i32(TCGContext *tcg_ctx, TCGv_i32 dst, TCGv_i32 src, TCGv_i32 shift)
{
    TCGv_i32 lval = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 rval = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 lsh = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 rsh = tcg_temp_new_i32(tcg_ctx);
    TCGv_i32 zero = tcg_const_i32(tcg_ctx, 0);
    TCGv_i32 max = tcg_const_i32(tcg_ctx, 31);

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_ext8s_i32(tcg_ctx, lsh, shift);
    tcg_gen_neg_i32(tcg_ctx, rsh, lsh);
    tcg_gen_shl_i32(tcg_ctx, lval, src, lsh);
    tcg_gen_umin_i32(tcg_ctx, rsh, rsh, max);
    tcg_gen_sar_i32(tcg_ctx, rval, src, rsh);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LEU, lval, lsh, max, lval, zero);
    tcg_gen_movcond_i32(tcg_ctx, TCG_COND_LT, dst, lsh, zero, rval, lval);

    tcg_temp_free_i32(tcg_ctx, lval);
    tcg_temp_free_i32(tcg_ctx, rval);
    tcg_temp_free_i32(tcg_ctx, lsh);
    tcg_temp_free_i32(tcg_ctx, rsh);
    tcg_temp_free_i32(tcg_ctx, zero);
    tcg_temp_free_i32(tcg_ctx, max);
}

void gen_sshl_i64(TCGContext *tcg_ctx, TCGv_i64 dst, TCGv_i64 src, TCGv_i64 shift)
{
    TCGv_i64 lval = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rval = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 lsh = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 rsh = tcg_temp_new_i64(tcg_ctx);
    TCGv_i64 zero = tcg_const_i64(tcg_ctx, 0);
    TCGv_i64 max = tcg_const_i64(tcg_ctx, 63);

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_ext8s_i64(tcg_ctx, lsh, shift);
    tcg_gen_neg_i64(tcg_ctx, rsh, lsh);
    tcg_gen_shl_i64(tcg_ctx, lval, src, lsh);
    tcg_gen_umin_i64(tcg_ctx, rsh, rsh, max);
    tcg_gen_sar_i64(tcg_ctx, rval, src, rsh);
    tcg_gen_movcond_i64(tcg_ctx, TCG_COND_LEU, lval, lsh, max, lval, zero);
    tcg_gen_movcond_i64(tcg_ctx, TCG_COND_LT, dst, lsh, zero, rval, lval);

    tcg_temp_free_i64(tcg_ctx, lval);
    tcg_temp_free_i64(tcg_ctx, rval);
    tcg_temp_free_i64(tcg_ctx, lsh);
    tcg_temp_free_i64(tcg_ctx, rsh);
    tcg_temp_free_i64(tcg_ctx, zero);
    tcg_temp_free_i64(tcg_ctx, max);
}

static void gen_sshl_vec(TCGContext *tcg_ctx, unsigned vece, TCGv_vec dst,
                         TCGv_vec src, TCGv_vec shift)
{
    TCGv_vec lval = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec rval = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec lsh = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec rsh = tcg_temp_new_vec_matching(tcg_ctx, dst);
    TCGv_vec tmp = tcg_temp_new_vec_matching(tcg_ctx, dst);

    /*
     * Rely on the TCG guarantee that out of range shifts produce
     * unspecified results, not undefined behaviour (i.e. no trap).
     * Discard out-of-range results after the fact.
     */
    tcg_gen_neg_vec(tcg_ctx, vece, rsh, shift);
    if (vece == MO_8) {
        tcg_gen_mov_vec(tcg_ctx, lsh, shift);
    } else {
        tcg_gen_dupi_vec(tcg_ctx, vece, tmp, 0xff);
        tcg_gen_and_vec(tcg_ctx, vece, lsh, shift, tmp);
        tcg_gen_and_vec(tcg_ctx, vece, rsh, rsh, tmp);
    }

    /* Bound rsh so out of bound right shift gets -1.  */
    tcg_gen_dupi_vec(tcg_ctx, vece, tmp, (8 << vece) - 1);
    tcg_gen_umin_vec(tcg_ctx, vece, rsh, rsh, tmp);
    tcg_gen_cmp_vec(tcg_ctx, TCG_COND_GT, vece, tmp, lsh, tmp);

    tcg_gen_shlv_vec(tcg_ctx, vece, lval, src, lsh);
    tcg_gen_sarv_vec(tcg_ctx, vece, rval, src, rsh);

    /* Select in-bound left shift.  */
    tcg_gen_andc_vec(tcg_ctx, vece, lval, lval, tmp);

    /* Select between left and right shift.  */
    if (vece == MO_8) {
        tcg_gen_dupi_vec(tcg_ctx, vece, tmp, 0);
        tcg_gen_cmpsel_vec(tcg_ctx, TCG_COND_LT, vece, dst, lsh, tmp, rval, lval);
    } else {
        tcg_gen_dupi_vec(tcg_ctx, vece, tmp, 0x80);
        tcg_gen_cmpsel_vec(tcg_ctx, TCG_COND_LT, vece, dst, lsh, tmp, lval, rval);
    }

    tcg_temp_free_vec(tcg_ctx, lval);
    tcg_temp_free_vec(tcg_ctx, rval);
    tcg_temp_free_vec(tcg_ctx, lsh);
    tcg_temp_free_vec(tcg_ctx, rsh);
    tcg_temp_free_vec(tcg_ctx, tmp);
}

void gen_gvec_sshl(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_neg_vec, INDEX_op_umin_vec, INDEX_op_shlv_vec,
        INDEX_op_sarv_vec, INDEX_op_cmp_vec, INDEX_op_cmpsel_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_sshl_vec,
          .fno = gen_helper_gvec_sshl_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fniv = gen_sshl_vec,
          .fno = gen_helper_gvec_sshl_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_sshl_i32,
          .fniv = gen_sshl_vec,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_sshl_i64,
          .fniv = gen_sshl_vec,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_uqadd_vec(TCGContext *s, unsigned vece, TCGv_vec t, TCGv_vec sat,
                          TCGv_vec a, TCGv_vec b)
{
    TCGv_vec x = tcg_temp_new_vec_matching(s, t);
    tcg_gen_add_vec(s, vece, x, a, b);
    tcg_gen_usadd_vec(s, vece, t, a, b);
    tcg_gen_cmp_vec(s, TCG_COND_NE, vece, x, x, t);
    tcg_gen_or_vec(s, vece, sat, sat, x);
    tcg_temp_free_vec(s, x);
}

void gen_gvec_uqadd_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                       uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_usadd_vec, INDEX_op_cmp_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen4 ops[4] = {
        { .fniv = gen_uqadd_vec,
          .fno = gen_helper_gvec_uqadd_b,
          .write_aofs = true,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fniv = gen_uqadd_vec,
          .fno = gen_helper_gvec_uqadd_h,
          .write_aofs = true,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fniv = gen_uqadd_vec,
          .fno = gen_helper_gvec_uqadd_s,
          .write_aofs = true,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fniv = gen_uqadd_vec,
          .fno = gen_helper_gvec_uqadd_d,
          .write_aofs = true,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_4(s, rd_ofs, offsetof(CPUARMState, vfp.qc),
                   rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_sqadd_vec(TCGContext *s, unsigned vece, TCGv_vec t, TCGv_vec sat,
                          TCGv_vec a, TCGv_vec b)
{
    TCGv_vec x = tcg_temp_new_vec_matching(s, t);
    tcg_gen_add_vec(s, vece, x, a, b);
    tcg_gen_ssadd_vec(s, vece, t, a, b);
    tcg_gen_cmp_vec(s, TCG_COND_NE, vece, x, x, t);
    tcg_gen_or_vec(s, vece, sat, sat, x);
    tcg_temp_free_vec(s, x);
}

void gen_gvec_sqadd_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                       uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_ssadd_vec, INDEX_op_cmp_vec, INDEX_op_add_vec, 0
    };
    static const GVecGen4 ops[4] = {
        { .fniv = gen_sqadd_vec,
          .fno = gen_helper_gvec_sqadd_b,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_8 },
        { .fniv = gen_sqadd_vec,
          .fno = gen_helper_gvec_sqadd_h,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_16 },
        { .fniv = gen_sqadd_vec,
          .fno = gen_helper_gvec_sqadd_s,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_32 },
        { .fniv = gen_sqadd_vec,
          .fno = gen_helper_gvec_sqadd_d,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_64 },
    };
    tcg_gen_gvec_4(s, rd_ofs, offsetof(CPUARMState, vfp.qc),
                   rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_uqsub_vec(TCGContext *s, unsigned vece, TCGv_vec t, TCGv_vec sat,
                          TCGv_vec a, TCGv_vec b)
{
    TCGv_vec x = tcg_temp_new_vec_matching(s, t);
    tcg_gen_sub_vec(s, vece, x, a, b);
    tcg_gen_ussub_vec(s, vece, t, a, b);
    tcg_gen_cmp_vec(s, TCG_COND_NE, vece, x, x, t);
    tcg_gen_or_vec(s, vece, sat, sat, x);
    tcg_temp_free_vec(s, x);
}

void gen_gvec_uqsub_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                       uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_ussub_vec, INDEX_op_cmp_vec, INDEX_op_sub_vec, 0
    };
    static const GVecGen4 ops[4] = {
        { .fniv = gen_uqsub_vec,
          .fno = gen_helper_gvec_uqsub_b,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_8 },
        { .fniv = gen_uqsub_vec,
          .fno = gen_helper_gvec_uqsub_h,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_16 },
        { .fniv = gen_uqsub_vec,
          .fno = gen_helper_gvec_uqsub_s,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_32 },
        { .fniv = gen_uqsub_vec,
          .fno = gen_helper_gvec_uqsub_d,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_64 },
    };
    tcg_gen_gvec_4(s, rd_ofs, offsetof(CPUARMState, vfp.qc),
                   rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_sqsub_vec(TCGContext *s, unsigned vece, TCGv_vec t, TCGv_vec sat,
                          TCGv_vec a, TCGv_vec b)
{
    TCGv_vec x = tcg_temp_new_vec_matching(s, t);
    tcg_gen_sub_vec(s, vece, x, a, b);
    tcg_gen_sssub_vec(s, vece, t, a, b);
    tcg_gen_cmp_vec(s, TCG_COND_NE, vece, x, x, t);
    tcg_gen_or_vec(s, vece, sat, sat, x);
    tcg_temp_free_vec(s, x);
}

void gen_gvec_sqsub_qc(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                       uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sssub_vec, INDEX_op_cmp_vec, INDEX_op_sub_vec, 0
    };
    static const GVecGen4 ops[4] = {
        { .fniv = gen_sqsub_vec,
          .fno = gen_helper_gvec_sqsub_b,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_8 },
        { .fniv = gen_sqsub_vec,
          .fno = gen_helper_gvec_sqsub_h,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_16 },
        { .fniv = gen_sqsub_vec,
          .fno = gen_helper_gvec_sqsub_s,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_32 },
        { .fniv = gen_sqsub_vec,
          .fno = gen_helper_gvec_sqsub_d,
          .opt_opc = vecop_list,
          .write_aofs = true,
          .vece = MO_64 },
    };
    tcg_gen_gvec_4(s, rd_ofs, offsetof(CPUARMState, vfp.qc),
                   rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_sabd_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    tcg_gen_sub_i32(s, t, a, b);
    tcg_gen_sub_i32(s, d, b, a);
    tcg_gen_movcond_i32(s, TCG_COND_LT, d, a, b, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_sabd_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_sub_i64(s, t, a, b);
    tcg_gen_sub_i64(s, d, b, a);
    tcg_gen_movcond_i64(s, TCG_COND_LT, d, a, b, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_sabd_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);

    tcg_gen_smin_vec(s, vece, t, a, b);
    tcg_gen_smax_vec(s, vece, d, a, b);
    tcg_gen_sub_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_sabd(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sub_vec, INDEX_op_smin_vec, INDEX_op_smax_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_sabd_vec,
          .fno = gen_helper_gvec_sabd_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fniv = gen_sabd_vec,
          .fno = gen_helper_gvec_sabd_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_sabd_i32,
          .fniv = gen_sabd_vec,
          .fno = gen_helper_gvec_sabd_s,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_sabd_i64,
          .fniv = gen_sabd_vec,
          .fno = gen_helper_gvec_sabd_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_uabd_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i32 t = tcg_temp_new_i32(s);

    tcg_gen_sub_i32(s, t, a, b);
    tcg_gen_sub_i32(s, d, b, a);
    tcg_gen_movcond_i32(s, TCG_COND_LTU, d, a, b, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_uabd_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    TCGv_i64 t = tcg_temp_new_i64(s);

    tcg_gen_sub_i64(s, t, a, b);
    tcg_gen_sub_i64(s, d, b, a);
    tcg_gen_movcond_i64(s, TCG_COND_LTU, d, a, b, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_uabd_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);

    tcg_gen_umin_vec(s, vece, t, a, b);
    tcg_gen_umax_vec(s, vece, d, a, b);
    tcg_gen_sub_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_uabd(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sub_vec, INDEX_op_umin_vec, INDEX_op_umax_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_uabd_vec,
          .fno = gen_helper_gvec_uabd_b,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fniv = gen_uabd_vec,
          .fno = gen_helper_gvec_uabd_h,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_uabd_i32,
          .fniv = gen_uabd_vec,
          .fno = gen_helper_gvec_uabd_s,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_uabd_i64,
          .fniv = gen_uabd_vec,
          .fno = gen_helper_gvec_uabd_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_saba_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i32 t = tcg_temp_new_i32(s);
    gen_sabd_i32(s, t, a, b);
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_saba_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    TCGv_i64 t = tcg_temp_new_i64(s);
    gen_sabd_i64(s, t, a, b);
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_saba_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    gen_sabd_vec(s, vece, t, a, b);
    tcg_gen_add_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_saba(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sub_vec, INDEX_op_add_vec,
        INDEX_op_smin_vec, INDEX_op_smax_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_saba_vec,
          .fno = gen_helper_gvec_saba_b,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_8 },
        { .fniv = gen_saba_vec,
          .fno = gen_helper_gvec_saba_h,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_16 },
        { .fni4 = gen_saba_i32,
          .fniv = gen_saba_vec,
          .fno = gen_helper_gvec_saba_s,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_32 },
        { .fni8 = gen_saba_i64,
          .fniv = gen_saba_vec,
          .fno = gen_helper_gvec_saba_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

static void gen_uaba_i32(TCGContext *s, TCGv_i32 d, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i32 t = tcg_temp_new_i32(s);
    gen_uabd_i32(s, t, a, b);
    tcg_gen_add_i32(s, d, d, t);
    tcg_temp_free_i32(s, t);
}

static void gen_uaba_i64(TCGContext *s, TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    TCGv_i64 t = tcg_temp_new_i64(s);
    gen_uabd_i64(s, t, a, b);
    tcg_gen_add_i64(s, d, d, t);
    tcg_temp_free_i64(s, t);
}

static void gen_uaba_vec(TCGContext *s, unsigned vece, TCGv_vec d, TCGv_vec a, TCGv_vec b)
{
    TCGv_vec t = tcg_temp_new_vec_matching(s, d);
    gen_uabd_vec(s, vece, t, a, b);
    tcg_gen_add_vec(s, vece, d, d, t);
    tcg_temp_free_vec(s, t);
}

void gen_gvec_uaba(TCGContext *s, unsigned vece, uint32_t rd_ofs, uint32_t rn_ofs,
                   uint32_t rm_ofs, uint32_t opr_sz, uint32_t max_sz)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_sub_vec, INDEX_op_add_vec,
        INDEX_op_umin_vec, INDEX_op_umax_vec, 0
    };
    static const GVecGen3 ops[4] = {
        { .fniv = gen_uaba_vec,
          .fno = gen_helper_gvec_uaba_b,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_8 },
        { .fniv = gen_uaba_vec,
          .fno = gen_helper_gvec_uaba_h,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_16 },
        { .fni4 = gen_uaba_i32,
          .fniv = gen_uaba_vec,
          .fno = gen_helper_gvec_uaba_s,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_32 },
        { .fni8 = gen_uaba_i64,
          .fniv = gen_uaba_vec,
          .fno = gen_helper_gvec_uaba_d,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .opt_opc = vecop_list,
          .load_dest = true,
          .vece = MO_64 },
    };
    tcg_gen_gvec_3(s, rd_ofs, rn_ofs, rm_ofs, opr_sz, max_sz, &ops[vece]);
}

/* Translate a NEON data processing instruction.  Return nonzero if the
   instruction is invalid.
   We process data in a mixture of 32-bit and 64-bit chunks.
   Mostly we use 32-bit chunks so we can use normal scalar instructions.  */

static int disas_neon_data_insn(DisasContext *s, uint32_t insn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int op;
    int q;
    int rd, rm, rd_ofs, rm_ofs;
    int size;
    int pass;
    int u;
    int vec_size;
    TCGv_i32 tmp, tmp2, tmp3;

    if (!arm_dc_feature(s, ARM_FEATURE_NEON)) {
        return 1;
    }

    /* FIXME: this access check should not take precedence over UNDEF
     * for invalid encodings; we will generate incorrect syndrome information
     * for attempts to execute invalid vfp/neon encodings with FP disabled.
     */
    if (s->fp_excp_el) {
        gen_exception_insn(s, s->pc_curr, EXCP_UDEF,
                           syn_simd_access_trap(1, 0xe, false), s->fp_excp_el);
        return 0;
    }

    if (!s->vfp_enabled)
      return 1;
    q = (insn & (1 << 6)) != 0;
    u = (insn >> 24) & 1;
    VFP_DREG_D(rd, insn);
    VFP_DREG_M(rm, insn);
    size = (insn >> 20) & 3;
    vec_size = q ? 16 : 8;
    rd_ofs = neon_reg_offset(rd, 0);
    rm_ofs = neon_reg_offset(rm, 0);

    if ((insn & (1 << 23)) == 0) {
        /* Three register same length: handled by decodetree */
        return 1;
    } else if (insn & (1 << 4)) {
        /* Two registers and shift or reg and imm: handled by decodetree */
        return 1;
    } else { /* (insn & 0x00800010 == 0x00800000) */
        if (size != 3) {
            /*
             * Three registers of different lengths, or two registers and
             * a scalar: handled by decodetree
             */
            return 1;
        } else { /* size == 3 */
            if (!u) {
                /* Extract: handled by decodetree */
                return 1;
            } else if ((insn & (1 << 11)) == 0) {
                /* Two register misc.  */
                op = ((insn >> 12) & 0x30) | ((insn >> 7) & 0xf);
                size = (insn >> 18) & 3;
                /* UNDEF for unknown op values and bad op-size combinations */
                if ((neon_2rm_sizes[op] & (1 << size)) == 0) {
                    return 1;
                }
                if (neon_2rm_is_v8_op(op) &&
                    !arm_dc_feature(s, ARM_FEATURE_V8)) {
                    return 1;
                }
                if ((op != NEON_2RM_VMOVN && op != NEON_2RM_VQMOVN) &&
                    q && ((rm | rd) & 1)) {
                    return 1;
                }
                switch (op) {
                case NEON_2RM_VREV64:
                    for (pass = 0; pass < (q ? 2 : 1); pass++) {
                        tmp = neon_load_reg(s, rm, pass * 2);
                        tmp2 = neon_load_reg(s, rm, pass * 2 + 1);
                        switch (size) {
                        case 0: tcg_gen_bswap32_i32(tcg_ctx, tmp, tmp); break;
                        case 1: gen_swap_half(s, tmp); break;
                        case 2: /* no-op */ break;
                        default: abort();
                        }
                        neon_store_reg(s, rd, pass * 2 + 1, tmp);
                        if (size == 2) {
                            neon_store_reg(s, rd, pass * 2, tmp2);
                        } else {
                            switch (size) {
                            case 0: tcg_gen_bswap32_i32(tcg_ctx, tmp2, tmp2); break;
                            case 1: gen_swap_half(s, tmp2); break;
                            default: abort();
                            }
                            neon_store_reg(s, rd, pass * 2, tmp2);
                        }
                    }
                    break;
                case NEON_2RM_VPADDL: case NEON_2RM_VPADDL_U:
                case NEON_2RM_VPADAL: case NEON_2RM_VPADAL_U:
                    for (pass = 0; pass < q + 1; pass++) {
                        tmp = neon_load_reg(s, rm, pass * 2);
                        gen_neon_widen(s, s->V0, tmp, size, op & 1);
                        tmp = neon_load_reg(s, rm, pass * 2 + 1);
                        gen_neon_widen(s, s->V1, tmp, size, op & 1);
                        switch (size) {
                        case 0: gen_helper_neon_paddl_u16(tcg_ctx, CPU_V001); break;
                        case 1: gen_helper_neon_paddl_u32(tcg_ctx, CPU_V001); break;
                        case 2: tcg_gen_add_i64(tcg_ctx, CPU_V001); break;
                        default: abort();
                        }
                        if (op >= NEON_2RM_VPADAL) {
                            /* Accumulate.  */
                            neon_load_reg64(s, s->V1, rd + pass);
                            gen_neon_addl(s, size);
                        }
                        neon_store_reg64(s, s->V0, rd + pass);
                    }
                    break;
                case NEON_2RM_VTRN:
                    if (size == 2) {
                        int n;
                        for (n = 0; n < (q ? 4 : 2); n += 2) {
                            tmp = neon_load_reg(s, rm, n);
                            tmp2 = neon_load_reg(s, rd, n + 1);
                            neon_store_reg(s, rm, n, tmp2);
                            neon_store_reg(s, rd, n + 1, tmp);
                        }
                    } else {
                        goto elementwise;
                    }
                    break;
                case NEON_2RM_VUZP:
                    if (gen_neon_unzip(s, rd, rm, size, q)) {
                        return 1;
                    }
                    break;
                case NEON_2RM_VZIP:
                    if (gen_neon_zip(s, rd, rm, size, q)) {
                        return 1;
                    }
                    break;
                case NEON_2RM_VMOVN: case NEON_2RM_VQMOVN:
                    /* also VQMOVUN; op field and mnemonics don't line up */
                    if (rm & 1) {
                        return 1;
                    }
                    tmp2 = NULL;
                    for (pass = 0; pass < 2; pass++) {
                        neon_load_reg64(s, s->V0, rm + pass);
                        tmp = tcg_temp_new_i32(tcg_ctx);
                        gen_neon_narrow_op(s, op == NEON_2RM_VMOVN, q, size,
                                           tmp, s->V0);
                        if (pass == 0) {
                            tmp2 = tmp;
                        } else {
                            neon_store_reg(s, rd, 0, tmp2);
                            neon_store_reg(s, rd, 1, tmp);
                        }
                    }
                    break;
                case NEON_2RM_VSHLL:
                    if (q || (rd & 1)) {
                        return 1;
                    }
                    tmp = neon_load_reg(s, rm, 0);
                    tmp2 = neon_load_reg(s, rm, 1);
                    for (pass = 0; pass < 2; pass++) {
                        if (pass == 1)
                            tmp = tmp2;
                        gen_neon_widen(s, s->V0, tmp, size, 1);
                        tcg_gen_shli_i64(tcg_ctx, s->V0, s->V0, 8 << size);
                        neon_store_reg64(s, s->V0, rd + pass);
                    }
                    break;
                case NEON_2RM_VCVT_F16_F32:
                {
                    TCGv_ptr fpst;
                    TCGv_i32 ahp;

                    if (!dc_isar_feature(aa32_fp16_spconv, s) ||
                        q || (rm & 1)) {
                        return 1;
                    }
                    fpst = get_fpstatus_ptr(tcg_ctx, true);
                    ahp = get_ahp_flag(s);
                    tmp = neon_load_reg(s, rm, 0);
                    gen_helper_vfp_fcvt_f32_to_f16(tcg_ctx, tmp, tmp, fpst, ahp);
                    tmp2 = neon_load_reg(s, rm, 1);
                    gen_helper_vfp_fcvt_f32_to_f16(tcg_ctx, tmp2, tmp2, fpst, ahp);
                    tcg_gen_shli_i32(tcg_ctx, tmp2, tmp2, 16);
                    tcg_gen_or_i32(tcg_ctx, tmp2, tmp2, tmp);
                    tcg_temp_free_i32(tcg_ctx, tmp);
                    tmp = neon_load_reg(s, rm, 2);
                    gen_helper_vfp_fcvt_f32_to_f16(tcg_ctx, tmp, tmp, fpst, ahp);
                    tmp3 = neon_load_reg(s, rm, 3);
                    neon_store_reg(s, rd, 0, tmp2);
                    gen_helper_vfp_fcvt_f32_to_f16(tcg_ctx, tmp3, tmp3, fpst, ahp);
                    tcg_gen_shli_i32(tcg_ctx, tmp3, tmp3, 16);
                    tcg_gen_or_i32(tcg_ctx, tmp3, tmp3, tmp);
                    neon_store_reg(s, rd, 1, tmp3);
                    tcg_temp_free_i32(tcg_ctx, tmp);
                    tcg_temp_free_i32(tcg_ctx, ahp);
                    tcg_temp_free_ptr(tcg_ctx, fpst);
                    break;
                }
                case NEON_2RM_VCVT_F32_F16:
                {
                    TCGv_ptr fpst;
                    TCGv_i32 ahp;
                    if (!dc_isar_feature(aa32_fp16_spconv, s) ||
                        q || (rd & 1)) {
                        return 1;
                    }
                    fpst = get_fpstatus_ptr(tcg_ctx, true);
                    ahp = get_ahp_flag(s);
                    tmp3 = tcg_temp_new_i32(tcg_ctx);
                    tmp = neon_load_reg(s, rm, 0);
                    tmp2 = neon_load_reg(s, rm, 1);
                    tcg_gen_ext16u_i32(tcg_ctx, tmp3, tmp);
                    gen_helper_vfp_fcvt_f16_to_f32(tcg_ctx, tmp3, tmp3, fpst, ahp);
                    neon_store_reg(s, rd, 0, tmp3);
                    tcg_gen_shri_i32(tcg_ctx, tmp, tmp, 16);
                    gen_helper_vfp_fcvt_f16_to_f32(tcg_ctx, tmp, tmp, fpst, ahp);
                    neon_store_reg(s, rd, 1, tmp);
                    tmp3 = tcg_temp_new_i32(tcg_ctx);
                    tcg_gen_ext16u_i32(tcg_ctx, tmp3, tmp2);
                    gen_helper_vfp_fcvt_f16_to_f32(tcg_ctx, tmp3, tmp3, fpst, ahp);
                    neon_store_reg(s, rd, 2, tmp3);
                    tcg_gen_shri_i32(tcg_ctx, tmp2, tmp2, 16);
                    gen_helper_vfp_fcvt_f16_to_f32(tcg_ctx, tmp2, tmp2, fpst, ahp);
                    neon_store_reg(s, rd, 3, tmp2);
                    tcg_temp_free_i32(tcg_ctx, ahp);
                    tcg_temp_free_ptr(tcg_ctx, fpst);
                    break;
                }
                case NEON_2RM_AESE: case NEON_2RM_AESMC:
                    if (!dc_isar_feature(aa32_aes, s) || ((rm | rd) & 1)) {
                        return 1;
                    }

                    /*
                     * Bit 6 is the lowest opcode bit; it distinguishes
                     * between encryption (AESE/AESMC) and decryption
                     * (AESD/AESIMC).
                     */
                    if (op == NEON_2RM_AESE) {
                        tcg_gen_gvec_3_ool(tcg_ctx, vfp_reg_offset(true, rd),
                                           vfp_reg_offset(true, rd),
                                           vfp_reg_offset(true, rm),
                                           16, 16, extract32(insn, 6, 1),
                                           gen_helper_crypto_aese);
                    } else {
                        tcg_gen_gvec_2_ool(tcg_ctx, vfp_reg_offset(true, rd),
                                           vfp_reg_offset(true, rm),
                                           16, 16, extract32(insn, 6, 1),
                                           gen_helper_crypto_aesmc);
                    }
                    break;
                case NEON_2RM_SHA1H:
                    if (!dc_isar_feature(aa32_sha1, s) || ((rm | rd) & 1)) {
                        return 1;
                    }
                    tcg_gen_gvec_2_ool(tcg_ctx, rd_ofs, rm_ofs, 16, 16, 0,
                                       gen_helper_crypto_sha1h);
                    break;
                case NEON_2RM_SHA1SU1:
                    if ((rm | rd) & 1) {
                            return 1;
                    }
                    /* bit 6 (q): set -> SHA256SU0, cleared -> SHA1SU1 */
                    if (q) {
                        if (!dc_isar_feature(aa32_sha2, s)) {
                            return 1;
                        }
                    } else if (!dc_isar_feature(aa32_sha1, s)) {
                        return 1;
                    }
                    tcg_gen_gvec_2_ool(tcg_ctx, rd_ofs, rm_ofs, 16, 16, 0,
                                       q ? gen_helper_crypto_sha256su0
                                       : gen_helper_crypto_sha1su1);
                    break;
                case NEON_2RM_VMVN:
                    tcg_gen_gvec_not(tcg_ctx, 0, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VNEG:
                    tcg_gen_gvec_neg(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VABS:
                    tcg_gen_gvec_abs(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;

                case NEON_2RM_VCEQ0:
                    gen_gvec_ceq0(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VCGT0:
                    gen_gvec_cgt0(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VCLE0:
                    gen_gvec_cle0(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VCGE0:
                    gen_gvec_cge0(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;
                case NEON_2RM_VCLT0:
                    gen_gvec_clt0(tcg_ctx, size, rd_ofs, rm_ofs, vec_size, vec_size);
                    break;

                default:
                elementwise:
                    for (pass = 0; pass < (q ? 4 : 2); pass++) {
                        tmp = neon_load_reg(s, rm, pass);
                        switch (op) {
                        case NEON_2RM_VREV32:
                            switch (size) {
                            case 0: tcg_gen_bswap32_i32(tcg_ctx, tmp, tmp); break;
                            case 1: gen_swap_half(s, tmp); break;
                            default: abort();
                            }
                            break;
                        case NEON_2RM_VREV16:
                            gen_rev16(s, tmp, tmp);
                            break;
                        case NEON_2RM_VCLS:
                            switch (size) {
                            case 0: gen_helper_neon_cls_s8(tcg_ctx, tmp, tmp); break;
                            case 1: gen_helper_neon_cls_s16(tcg_ctx, tmp, tmp); break;
                            case 2: gen_helper_neon_cls_s32(tcg_ctx, tmp, tmp); break;
                            default: abort();
                            }
                            break;
                        case NEON_2RM_VCLZ:
                            switch (size) {
                            case 0: gen_helper_neon_clz_u8(tcg_ctx, tmp, tmp); break;
                            case 1: gen_helper_neon_clz_u16(tcg_ctx, tmp, tmp); break;
                            case 2: tcg_gen_clzi_i32(tcg_ctx, tmp, tmp, 32); break;
                            default: abort();
                            }
                            break;
                        case NEON_2RM_VCNT:
                            gen_helper_neon_cnt_u8(tcg_ctx, tmp, tmp);
                            break;
                        case NEON_2RM_VQABS:
                            switch (size) {
                            case 0:
                                gen_helper_neon_qabs_s8(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            case 1:
                                gen_helper_neon_qabs_s16(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            case 2:
                                gen_helper_neon_qabs_s32(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            default: abort();
                            }
                            break;
                        case NEON_2RM_VQNEG:
                            switch (size) {
                            case 0:
                                gen_helper_neon_qneg_s8(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            case 1:
                                gen_helper_neon_qneg_s16(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            case 2:
                                gen_helper_neon_qneg_s32(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
                                break;
                            default: abort();
                            }
                            break;
                        case NEON_2RM_VCGT0_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            tmp2 = tcg_const_i32(tcg_ctx, 0);
                            gen_helper_neon_cgt_f32(tcg_ctx, tmp, tmp, tmp2, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tmp2);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCGE0_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            tmp2 = tcg_const_i32(tcg_ctx, 0);
                            gen_helper_neon_cge_f32(tcg_ctx, tmp, tmp, tmp2, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tmp2);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCEQ0_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            tmp2 = tcg_const_i32(tcg_ctx, 0);
                            gen_helper_neon_ceq_f32(tcg_ctx, tmp, tmp, tmp2, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tmp2);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCLE0_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            tmp2 = tcg_const_i32(tcg_ctx, 0);
                            gen_helper_neon_cge_f32(tcg_ctx, tmp, tmp2, tmp, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tmp2);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCLT0_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            tmp2 = tcg_const_i32(tcg_ctx, 0);
                            gen_helper_neon_cgt_f32(tcg_ctx, tmp, tmp2, tmp, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tmp2);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VABS_F:
                            gen_helper_vfp_abss(tcg_ctx, tmp, tmp);
                            break;
                        case NEON_2RM_VNEG_F:
                            gen_helper_vfp_negs(tcg_ctx, tmp, tmp);
                            break;
                        case NEON_2RM_VSWP:
                            tmp2 = neon_load_reg(s, rd, pass);
                            neon_store_reg(s, rm, pass, tmp2);
                            break;
                        case NEON_2RM_VTRN:
                            tmp2 = neon_load_reg(s, rd, pass);
                            switch (size) {
                            case 0: gen_neon_trn_u8(s, tmp, tmp2); break;
                            case 1: gen_neon_trn_u16(s, tmp, tmp2); break;
                            default: abort();
                            }
                            neon_store_reg(s, rm, pass, tmp2);
                            break;
                        case NEON_2RM_VRINTN:
                        case NEON_2RM_VRINTA:
                        case NEON_2RM_VRINTM:
                        case NEON_2RM_VRINTP:
                        case NEON_2RM_VRINTZ:
                        {
                            TCGv_i32 tcg_rmode;
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            int rmode;

                            if (op == NEON_2RM_VRINTZ) {
                                rmode = FPROUNDING_ZERO;
                            } else {
                                rmode = fp_decode_rm[((op & 0x6) >> 1) ^ 1];
                            }

                            tcg_rmode = tcg_const_i32(tcg_ctx, arm_rmode_to_sf(rmode));
                            gen_helper_set_neon_rmode(tcg_ctx, tcg_rmode, tcg_rmode,
                                                      tcg_ctx->cpu_env);
                            gen_helper_rints(tcg_ctx, tmp, tmp, fpstatus);
                            gen_helper_set_neon_rmode(tcg_ctx, tcg_rmode, tcg_rmode,
                                                      tcg_ctx->cpu_env);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            tcg_temp_free_i32(tcg_ctx, tcg_rmode);
                            break;
                        }
                        case NEON_2RM_VRINTX:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_rints_exact(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCVTAU:
                        case NEON_2RM_VCVTAS:
                        case NEON_2RM_VCVTNU:
                        case NEON_2RM_VCVTNS:
                        case NEON_2RM_VCVTPU:
                        case NEON_2RM_VCVTPS:
                        case NEON_2RM_VCVTMU:
                        case NEON_2RM_VCVTMS:
                        {
                            bool is_signed = !extract32(insn, 7, 1);
                            TCGv_ptr fpst = get_fpstatus_ptr(tcg_ctx, 1);
                            TCGv_i32 tcg_rmode, tcg_shift;
                            int rmode = fp_decode_rm[extract32(insn, 8, 2)];

                            tcg_shift = tcg_const_i32(tcg_ctx, 0);
                            tcg_rmode = tcg_const_i32(tcg_ctx, arm_rmode_to_sf(rmode));
                            gen_helper_set_neon_rmode(tcg_ctx, tcg_rmode, tcg_rmode,
                                                      tcg_ctx->cpu_env);

                            if (is_signed) {
                                gen_helper_vfp_tosls(tcg_ctx, tmp, tmp,
                                                     tcg_shift, fpst);
                            } else {
                                gen_helper_vfp_touls(tcg_ctx, tmp, tmp,
                                                     tcg_shift, fpst);
                            }

                            gen_helper_set_neon_rmode(tcg_ctx, tcg_rmode, tcg_rmode,
                                                      tcg_ctx->cpu_env);
                            tcg_temp_free_i32(tcg_ctx, tcg_rmode);
                            tcg_temp_free_i32(tcg_ctx, tcg_shift);
                            tcg_temp_free_ptr(tcg_ctx, fpst);
                            break;
                        }
                        case NEON_2RM_VRECPE:
                            gen_helper_recpe_u32(tcg_ctx, tmp, tmp);
                            break;
                        case NEON_2RM_VRSQRTE:
                            gen_helper_rsqrte_u32(tcg_ctx, tmp, tmp);
                            break;
                        case NEON_2RM_VRECPE_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_recpe_f32(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VRSQRTE_F:
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_rsqrte_f32(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCVT_FS: /* VCVT.F32.S32 */
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_vfp_sitos(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCVT_FU: /* VCVT.F32.U32 */
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_vfp_uitos(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCVT_SF: /* VCVT.S32.F32 */
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_vfp_tosizs(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        case NEON_2RM_VCVT_UF: /* VCVT.U32.F32 */
                        {
                            TCGv_ptr fpstatus = get_fpstatus_ptr(tcg_ctx, 1);
                            gen_helper_vfp_touizs(tcg_ctx, tmp, tmp, fpstatus);
                            tcg_temp_free_ptr(tcg_ctx, fpstatus);
                            break;
                        }
                        default:
                            /* Reserved op values were caught by the
                             * neon_2rm_sizes[] check earlier.
                             */
                            abort();
                        }
                        neon_store_reg(s, rd, pass, tmp);
                    }
                    break;
                }
            } else {
                /* VTBL, VTBX, VDUP: handled by decodetree */
                return 1;
            }
        }
    }
    return 0;
}

static int disas_coproc_insn(DisasContext *s, uint32_t insn)
{
    int cpnum, is64, crn, crm, opc1, opc2, isread, rt, rt2;
    const ARMCPRegInfo *ri;
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    cpnum = (insn >> 8) & 0xf;

    /* First check for coprocessor space used for XScale/iwMMXt insns */
    if (arm_dc_feature(s, ARM_FEATURE_XSCALE) && (cpnum < 2)) {
        if (extract32(s->c15_cpar, cpnum, 1) == 0) {
            return 1;
        }
        if (arm_dc_feature(s, ARM_FEATURE_IWMMXT)) {
            return disas_iwmmxt_insn(s, insn);
        } else if (arm_dc_feature(s, ARM_FEATURE_XSCALE)) {
            return disas_dsp_insn(s, insn);
        }
        return 1;
    }

    /* Otherwise treat as a generic register access */
    is64 = (insn & (1 << 25)) == 0;
    if (!is64 && ((insn & (1 << 4)) == 0)) {
        /* cdp */
        return 1;
    }

    crm = insn & 0xf;
    if (is64) {
        crn = 0;
        opc1 = (insn >> 4) & 0xf;
        opc2 = 0;
        rt2 = (insn >> 16) & 0xf;
    } else {
        crn = (insn >> 16) & 0xf;
        opc1 = (insn >> 21) & 7;
        opc2 = (insn >> 5) & 7;
        rt2 = 0;
    }
    isread = (insn >> 20) & 1;
    rt = (insn >> 12) & 0xf;

    ri = get_arm_cp_reginfo(s->cp_regs,
            ENCODE_CP_REG(cpnum, is64, s->ns, crn, crm, opc1, opc2));
    if (ri) {
        /* Check access permissions */
        if (!cp_access_ok(s->current_el, ri, isread)) {
            return 1;
        }

        if (s->hstr_active || ri->accessfn ||
            (arm_dc_feature(s, ARM_FEATURE_XSCALE) && cpnum < 14)) {
            /* Emit code to perform further access permissions checks at
             * runtime; this may result in an exception.
             * Note that on XScale all cp0..c13 registers do an access check
             * call in order to handle c15_cpar.
             */
            TCGv_ptr tmpptr;
            TCGv_i32 tcg_syn, tcg_isread;
            uint32_t syndrome;

            /* Note that since we are an implementation which takes an
             * exception on a trapped conditional instruction only if the
             * instruction passes its condition code check, we can take
             * advantage of the clause in the ARM ARM that allows us to set
             * the COND field in the instruction to 0xE in all cases.
             * We could fish the actual condition out of the insn (ARM)
             * or the condexec bits (Thumb) but it isn't necessary.
             */
            switch (cpnum) {
            case 14:
                if (is64) {
                    syndrome = syn_cp14_rrt_trap(1, 0xe, opc1, crm, rt, rt2,
                                                 isread, false);
                } else {
                    syndrome = syn_cp14_rt_trap(1, 0xe, opc1, opc2, crn, crm,
                                                rt, isread, false);
                }
                break;
            case 15:
                if (is64) {
                    syndrome = syn_cp15_rrt_trap(1, 0xe, opc1, crm, rt, rt2,
                                                 isread, false);
                } else {
                    syndrome = syn_cp15_rt_trap(1, 0xe, opc1, opc2, crn, crm,
                                                rt, isread, false);
                }
                break;
            default:
                /* ARMv8 defines that only coprocessors 14 and 15 exist,
                 * so this can only happen if this is an ARMv7 or earlier CPU,
                 * in which case the syndrome information won't actually be
                 * guest visible.
                 */
                assert(!arm_dc_feature(s, ARM_FEATURE_V8));
                syndrome = syn_uncategorized();
                break;
            }

            gen_set_condexec(s);
            gen_set_pc_im(s, s->pc_curr);
            tmpptr = tcg_const_ptr(tcg_ctx, ri);
            tcg_syn = tcg_const_i32(tcg_ctx, syndrome);
            tcg_isread = tcg_const_i32(tcg_ctx, isread);
            gen_helper_access_check_cp_reg(tcg_ctx, tcg_ctx->cpu_env, tmpptr, tcg_syn,
                                           tcg_isread);
            tcg_temp_free_ptr(tcg_ctx, tmpptr);
            tcg_temp_free_i32(tcg_ctx, tcg_syn);
            tcg_temp_free_i32(tcg_ctx, tcg_isread);
        } else if (ri->type & ARM_CP_RAISES_EXC) {
            /*
             * The readfn or writefn might raise an exception;
             * synchronize the CPU state in case it does.
             */
            gen_set_condexec(s);
            gen_set_pc_im(s, s->pc_curr);
        }

        /* Handle special cases first */
        switch (ri->type & ~(ARM_CP_FLAG_MASK & ~ARM_CP_SPECIAL)) {
        case ARM_CP_NOP:
            return 0;
        case ARM_CP_WFI:
            if (isread) {
                return 1;
            }
            gen_set_pc_im(s, s->base.pc_next);
            s->base.is_jmp = DISAS_WFI;
            return 0;
        default:
            break;
        }

        // Unicorn: if'd out
#if 0
        if ((tb_cflags(s->base.tb) & CF_USE_ICOUNT) && (ri->type & ARM_CP_IO)) {
            gen_io_start();
        }
#endif
        gen_set_pc_im(s, s->pc_curr); // SNPS added

        if (isread) {
            /* Read */
            if (is64) {
                TCGv_i64 tmp64;
                TCGv_i32 tmp;
                if (ri->type & ARM_CP_CONST) {
                    tmp64 = tcg_const_i64(tcg_ctx, ri->resetvalue);
                } else if (ri->readfn) {
                    TCGv_ptr tmpptr;
                    tmp64 = tcg_temp_new_i64(tcg_ctx);
                    tmpptr = tcg_const_ptr(tcg_ctx, ri);
                    gen_helper_get_cp_reg64(tcg_ctx, tmp64, tcg_ctx->cpu_env, tmpptr);
                    tcg_temp_free_ptr(tcg_ctx, tmpptr);
                } else {
                    tmp64 = tcg_temp_new_i64(tcg_ctx);
                    tcg_gen_ld_i64(tcg_ctx, tmp64, tcg_ctx->cpu_env, ri->fieldoffset);
                }
                tmp = tcg_temp_new_i32(tcg_ctx);
                tcg_gen_extrl_i64_i32(tcg_ctx, tmp, tmp64);
                store_reg(s, rt, tmp);
                tmp = tcg_temp_new_i32(tcg_ctx);
                tcg_gen_extrh_i64_i32(tcg_ctx, tmp, tmp64);
                tcg_temp_free_i64(tcg_ctx, tmp64);
                store_reg(s, rt2, tmp);
            } else {
                TCGv_i32 tmp;
                if (ri->type & ARM_CP_CONST) {
                    tmp = tcg_const_i32(tcg_ctx, ri->resetvalue);
                } else if (ri->readfn) {
                    TCGv_ptr tmpptr;
                    tmp = tcg_temp_new_i32(tcg_ctx);
                    tmpptr = tcg_const_ptr(tcg_ctx, ri);
                    gen_helper_get_cp_reg(tcg_ctx, tmp, tcg_ctx->cpu_env, tmpptr);
                    tcg_temp_free_ptr(tcg_ctx, tmpptr);
                } else {
                    tmp = load_cpu_offset(s, ri->fieldoffset);
                }
                if (rt == 15) {
                    /* Destination register of r15 for 32 bit loads sets
                     * the condition codes from the high 4 bits of the value
                     */
                    gen_set_nzcv(s, tmp);
                    tcg_temp_free_i32(tcg_ctx, tmp);
                } else {
                    store_reg(s, rt, tmp);
                }
            }
        } else {
            /* Write */
            if (ri->type & ARM_CP_CONST) {
                /* If not forbidden by access permissions, treat as WI */
                return 0;
            }

            if (is64) {
                TCGv_i32 tmplo, tmphi;
                TCGv_i64 tmp64 = tcg_temp_new_i64(tcg_ctx);
                tmplo = load_reg(s, rt);
                tmphi = load_reg(s, rt2);
                tcg_gen_concat_i32_i64(tcg_ctx, tmp64, tmplo, tmphi);
                tcg_temp_free_i32(tcg_ctx, tmplo);
                tcg_temp_free_i32(tcg_ctx, tmphi);
                if (ri->writefn) {
                    TCGv_ptr tmpptr = tcg_const_ptr(tcg_ctx, ri);
                    gen_helper_set_cp_reg64(tcg_ctx, tcg_ctx->cpu_env, tmpptr, tmp64);
                    tcg_temp_free_ptr(tcg_ctx, tmpptr);
                } else {
                    tcg_gen_st_i64(tcg_ctx, tmp64, tcg_ctx->cpu_env, ri->fieldoffset);
                }
                tcg_temp_free_i64(tcg_ctx, tmp64);
            } else {
                if (ri->writefn) {
                    TCGv_i32 tmp;
                    TCGv_ptr tmpptr;
                    tmp = load_reg(s, rt);
                    tmpptr = tcg_const_ptr(tcg_ctx, ri);
                    gen_helper_set_cp_reg(tcg_ctx, tcg_ctx->cpu_env, tmpptr, tmp);
                    tcg_temp_free_ptr(tcg_ctx, tmpptr);
                    tcg_temp_free_i32(tcg_ctx, tmp);
                } else {
                    TCGv_i32 tmp = load_reg(s, rt);
                    store_cpu_offset(s, tmp, ri->fieldoffset);
                }
            }
        }

        if ((tb_cflags(s->base.tb) & CF_USE_ICOUNT) && (ri->type & ARM_CP_IO)) {
            /* I/O operations must end the TB here (whether read or write) */
            // Unicorn: commented out
            //gen_io_end();
           gen_lookup_tb(s);
        } else if (!isread && !(ri->type & ARM_CP_SUPPRESS_TB_END)) {
            /* We default to ending the TB on a coprocessor register write,
             * but allow this to be suppressed by the register definition
             * (usually only necessary to work around guest bugs).
             */
            gen_lookup_tb(s);
        }

        return 0;
    }

    /* Unknown register; this might be a guest error or a QEMU
     * unimplemented feature.
     */
    if (is64) {
        qemu_log_mask(LOG_UNIMP, "%s access to unsupported AArch32 "
                      "64 bit system register cp:%d opc1: %d crm:%d "
                      "(%s)\n",
                      isread ? "read" : "write", cpnum, opc1, crm,
                      s->ns ? "non-secure" : "secure");
    } else {
        qemu_log_mask(LOG_UNIMP, "%s access to unsupported AArch32 "
                      "system register cp:%d opc1:%d crn:%d crm:%d opc2:%d "
                      "(%s)\n",
                      isread ? "read" : "write", cpnum, opc1, crn, crm, opc2,
                      s->ns ? "non-secure" : "secure");
    }

    return 1;
}


/* Store a 64-bit value to a register pair.  Clobbers val.  */
static void gen_storeq_reg(DisasContext *s, int rlow, int rhigh, TCGv_i64 val)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_extrl_i64_i32(tcg_ctx, tmp, val);
    store_reg(s, rlow, tmp);
    tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_extrh_i64_i32(tcg_ctx, tmp, val);
    store_reg(s, rhigh, tmp);
}

/* load and add a 64-bit value from a register pair.  */
static void gen_addq(DisasContext *s, TCGv_i64 val, int rlow, int rhigh)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i64 tmp;
    TCGv_i32 tmpl;
    TCGv_i32 tmph;

    /* Load 64-bit value rd:rn.  */
    tmpl = load_reg(s, rlow);
    tmph = load_reg(s, rhigh);
    tmp = tcg_temp_new_i64(tcg_ctx);
    tcg_gen_concat_i32_i64(tcg_ctx, tmp, tmpl, tmph);
    tcg_temp_free_i32(tcg_ctx, tmpl);
    tcg_temp_free_i32(tcg_ctx, tmph);
    tcg_gen_add_i64(tcg_ctx, val, val, tmp);
    tcg_temp_free_i64(tcg_ctx, tmp);
}

/* Set N and Z flags from hi|lo.  */
static void gen_logicq_cc(DisasContext *s, TCGv_i32 lo, TCGv_i32 hi)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_NF, hi);
    tcg_gen_or_i32(tcg_ctx, tcg_ctx->cpu_ZF, lo, hi);
}

/* Load/Store exclusive instructions are implemented by remembering
   the value/address loaded, and seeing if these are the same
   when the store is performed.  This should be sufficient to implement
   the architecturally mandated semantics, and avoids having to monitor
   regular stores.  The compare vs the remembered value is done during
   the cmpxchg operation, but we must compare the addresses manually.  */
static void gen_load_exclusive(DisasContext *s, int rt, int rt2,
                               TCGv_i32 addr, int size)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
    MemOp opc = size | MO_ALIGN | s->be_data;

    s->is_ldex = true;

    if (size == 3) {
        TCGv_i32 tmp2 = tcg_temp_new_i32(tcg_ctx);
        TCGv_i64 t64 = tcg_temp_new_i64(tcg_ctx);

        /* For AArch32, architecturally the 32-bit word at the lowest
         * address is always Rt and the one at addr+4 is Rt2, even if
         * the CPU is big-endian. That means we don't want to do a
         * gen_aa32_ld_i64(), which invokes gen_aa32_frob64() as if
         * for an architecturally 64-bit access, but instead do a
         * 64-bit access using MO_BE if appropriate and then split
         * the two halves.
         * This only makes a difference for BE32 user-mode, where
         * frob64() must not flip the two halves of the 64-bit data
         * but this code must treat BE32 user-mode like BE32 system.
         */
        TCGv taddr = gen_aa32_addr(s, addr, opc);

        tcg_gen_qemu_ld_i64(s->uc, t64, taddr, get_mem_index(s), opc);
        tcg_temp_free(tcg_ctx, taddr);
        tcg_gen_mov_i64(tcg_ctx, tcg_ctx->cpu_exclusive_val, t64);
        if (s->be_data == MO_BE) {
            tcg_gen_extr_i64_i32(tcg_ctx, tmp2, tmp, t64);
        } else {
            tcg_gen_extr_i64_i32(tcg_ctx, tmp, tmp2, t64);
        }
        tcg_temp_free_i64(tcg_ctx, t64);

        store_reg(s, rt2, tmp2);
    } else {
        gen_aa32_ld_i32(s, tmp, addr, get_mem_index(s), opc);
        tcg_gen_extu_i32_i64(tcg_ctx, tcg_ctx->cpu_exclusive_val, tmp);
    }

    store_reg(s, rt, tmp);
    tcg_gen_extu_i32_i64(tcg_ctx, tcg_ctx->cpu_exclusive_addr, addr);
}

static void gen_clrex(DisasContext *s)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_movi_i64(tcg_ctx, tcg_ctx->cpu_exclusive_addr, -1);
}

static void gen_store_exclusive(DisasContext *s, int rd, int rt, int rt2,
                                TCGv_i32 addr, int size)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1, t2;
    TCGv_i64 extaddr;
    TCGv taddr;
    TCGLabel *done_label;
    TCGLabel *fail_label;
    MemOp opc = size | MO_ALIGN | s->be_data;

    /* if (env->exclusive_addr == addr && env->exclusive_val == [addr]) {
         [addr] = {Rt};
         {Rd} = 0;
       } else {
         {Rd} = 1;
       } */
    fail_label = gen_new_label(tcg_ctx);
    done_label = gen_new_label(tcg_ctx);
    extaddr = tcg_temp_new_i64(tcg_ctx);
    tcg_gen_extu_i32_i64(tcg_ctx, extaddr, addr);
    tcg_gen_brcond_i64(tcg_ctx, TCG_COND_NE, extaddr, tcg_ctx->cpu_exclusive_addr, fail_label);
    tcg_temp_free_i64(tcg_ctx, extaddr);

    taddr = gen_aa32_addr(s, addr, opc);
    t0 = tcg_temp_new_i32(tcg_ctx);
    t1 = load_reg(s, rt);
    if (size == 3) {
        TCGv_i64 o64 = tcg_temp_new_i64(tcg_ctx);
        TCGv_i64 n64 = tcg_temp_new_i64(tcg_ctx);

        t2 = load_reg(s, rt2);
        /* For AArch32, architecturally the 32-bit word at the lowest
         * address is always Rt and the one at addr+4 is Rt2, even if
         * the CPU is big-endian. Since we're going to treat this as a
         * single 64-bit BE store, we need to put the two halves in the
         * opposite order for BE to LE, so that they end up in the right
         * places.
         * We don't want gen_aa32_frob64() because that does the wrong
         * thing for BE32 usermode.
         */
        if (s->be_data == MO_BE) {
            tcg_gen_concat_i32_i64(tcg_ctx, n64, t2, t1);
        } else {
            tcg_gen_concat_i32_i64(tcg_ctx, n64, t1, t2);
        }
        tcg_temp_free_i32(tcg_ctx, t2);

        tcg_gen_atomic_cmpxchg_i64(tcg_ctx, o64, taddr, tcg_ctx->cpu_exclusive_val, n64,
                                   get_mem_index(s), opc);
        tcg_temp_free_i64(tcg_ctx, n64);

        tcg_gen_setcond_i64(tcg_ctx, TCG_COND_NE, o64, o64, tcg_ctx->cpu_exclusive_val);
        tcg_gen_extrl_i64_i32(tcg_ctx, t0, o64);

        tcg_temp_free_i64(tcg_ctx, o64);
    } else {
        t2 = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_extrl_i64_i32(tcg_ctx, t2, tcg_ctx->cpu_exclusive_val);
        tcg_gen_atomic_cmpxchg_i32(tcg_ctx, t0, taddr, t2, t1, get_mem_index(s), opc);
        tcg_gen_setcond_i32(tcg_ctx, TCG_COND_NE, t0, t0, t2);
        tcg_temp_free_i32(tcg_ctx, t2);
    }
    tcg_temp_free_i32(tcg_ctx, t1);
    tcg_temp_free(tcg_ctx, taddr);
    tcg_gen_mov_i32(tcg_ctx, tcg_ctx->cpu_R[rd], t0);
    tcg_temp_free_i32(tcg_ctx, t0);
    tcg_gen_br(tcg_ctx, done_label);

    gen_set_label(tcg_ctx, fail_label);
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[rd], 1);
    gen_set_label(tcg_ctx, done_label);
    tcg_gen_movi_i64(tcg_ctx, tcg_ctx->cpu_exclusive_addr, -1);
}

/* gen_srs:
 * @env: CPUARMState
 * @s: DisasContext
 * @mode: mode field from insn (which stack to store to)
 * @amode: addressing mode (DA/IA/DB/IB), encoded as per P,U bits in ARM insn
 * @writeback: true if writeback bit set
 *
 * Generate code for the SRS (Store Return State) insn.
 */
static void gen_srs(DisasContext *s,
                    uint32_t mode, uint32_t amode, bool writeback)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int32_t offset;
    TCGv_i32 addr, tmp;
    bool undef = false;

    /* SRS is:
     * - trapped to EL3 if EL3 is AArch64 and we are at Secure EL1
     *   and specified mode is monitor mode
     * - UNDEFINED in Hyp mode
     * - UNPREDICTABLE in User or System mode
     * - UNPREDICTABLE if the specified mode is:
     * -- not implemented
     * -- not a valid mode number
     * -- a mode that's at a higher exception level
     * -- Monitor, if we are Non-secure
     * For the UNPREDICTABLE cases we choose to UNDEF.
     */
    if (s->current_el == 1 && !s->ns && mode == ARM_CPU_MODE_MON) {
        gen_exception_insn(s, s->pc_curr, EXCP_UDEF, syn_uncategorized(), 3);
        return;
    }

    if (s->current_el == 0 || s->current_el == 2) {
        undef = true;
    }

    switch (mode) {
    case ARM_CPU_MODE_USR:
    case ARM_CPU_MODE_FIQ:
    case ARM_CPU_MODE_IRQ:
    case ARM_CPU_MODE_SVC:
    case ARM_CPU_MODE_ABT:
    case ARM_CPU_MODE_UND:
    case ARM_CPU_MODE_SYS:
        break;
    case ARM_CPU_MODE_HYP:
        if (s->current_el == 1 || !arm_dc_feature(s, ARM_FEATURE_EL2)) {
            undef = true;
        }
        break;
    case ARM_CPU_MODE_MON:
        /* No need to check specifically for "are we non-secure" because
         * we've already made EL0 UNDEF and handled the trap for S-EL1;
         * so if this isn't EL3 then we must be non-secure.
         */
        if (s->current_el != 3) {
            undef = true;
        }
        break;
    default:
        undef = true;
    }

    if (undef) {
        unallocated_encoding(s);
        return;
    }

    addr = tcg_temp_new_i32(tcg_ctx);
    tmp = tcg_const_i32(tcg_ctx, mode);
    /* get_r13_banked() will raise an exception if called from System mode */
    gen_set_condexec(s);
    gen_set_pc_im(s, s->pc_curr);
    gen_helper_get_r13_banked(tcg_ctx, addr, tcg_ctx->cpu_env, tmp);
    tcg_temp_free_i32(tcg_ctx, tmp);
    switch (amode) {
    case 0: /* DA */
        offset = -4;
        break;
    case 1: /* IA */
        offset = 0;
        break;
    case 2: /* DB */
        offset = -8;
        break;
    case 3: /* IB */
        offset = 4;
        break;
    default:
        abort();
    }
    tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
    tmp = load_reg(s, 14);
    gen_aa32_st32(s, tmp, addr, get_mem_index(s));
    tcg_temp_free_i32(tcg_ctx, tmp);
    tmp = load_cpu_field(s, spsr);
    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
    gen_aa32_st32(s, tmp, addr, get_mem_index(s));
    tcg_temp_free_i32(tcg_ctx, tmp);
    if (writeback) {
        switch (amode) {
        case 0:
            offset = -8;
            break;
        case 1:
            offset = 4;
            break;
        case 2:
            offset = -4;
            break;
        case 3:
            offset = 0;
            break;
        default:
            abort();
        }
        tcg_gen_addi_i32(tcg_ctx, addr, addr, offset);
        tmp = tcg_const_i32(tcg_ctx, mode);
        gen_helper_set_r13_banked(tcg_ctx, tcg_ctx->cpu_env, tmp, addr);
        tcg_temp_free_i32(tcg_ctx, tmp);
    }
    tcg_temp_free_i32(tcg_ctx, addr);
    s->base.is_jmp = DISAS_UPDATE;
}

/* Generate a label used for skipping this instruction */
static void arm_gen_condlabel(DisasContext *s)
{
    if (!s->condjmp) {
        TCGContext *tcg_ctx = s->uc->tcg_ctx;

        s->condlabel = gen_new_label(tcg_ctx);
        s->condjmp = 1;
    }
}

/* Skip this instruction if the ARM condition is false */
static void arm_skip_unless(DisasContext *s, uint32_t cond)
{
    arm_gen_condlabel(s);
    arm_gen_test_cc(s, cond ^ 1, s->condlabel);
}

/*
 * Constant expanders for the decoders.
 */

static int negate(DisasContext *s, int x)
{
    return -x;
}

static int plus_2(DisasContext *s, int x)
{
    return x + 2;
}

static int times_2(DisasContext *s, int x)
{
    return x * 2;
}

static int times_4(DisasContext *s, int x)
{
    return x * 4;
}

/* Return only the rotation part of T32ExpandImm.  */
static int t32_expandimm_rot(DisasContext *s, int x)
{
    return x & 0xc00 ? extract32(x, 7, 5) : 0;
}

/* Return the unrotated immediate from T32ExpandImm.  */
static int t32_expandimm_imm(DisasContext *s, int x)
{
    int imm = extract32(x, 0, 8);

    switch (extract32(x, 8, 4)) {
    case 0: /* XY */
        /* Nothing to do.  */
        break;
    case 1: /* 00XY00XY */
        imm *= 0x00010001;
        break;
    case 2: /* XY00XY00 */
        imm *= 0x01000100;
        break;
    case 3: /* XYXYXYXY */
        imm *= 0x01010101;
        break;
    default:
        /* Rotated constant.  */
        imm |= 0x80;
        break;
    }
    return imm;
}

static int t32_branch24(DisasContext *s, int x)
{
    /* Convert J1:J2 at x[22:21] to I2:I1, which involves I=J^~S.  */
    x ^= !(x < 0) * (3 << 21);
    /* Append the final zero.  */
    return x << 1;
}

static int t16_setflags(DisasContext *s)
{
    return s->condexec_mask == 0;
}

static int t16_push_list(DisasContext *s, int x)
{
    return (x & 0xff) | (x & 0x100) << (14 - 8);
}

static int t16_pop_list(DisasContext *s, int x)
{
    return (x & 0xff) | (x & 0x100) << (15 - 8);
}

/*
 * Include the generated decoders.
 */

#include "decode-a32.inc.c"
#include "decode-a32-uncond.inc.c"
#include "decode-t32.inc.c"
#include "decode-t16.inc.c"

/* Helpers to swap operands for reverse-subtract.  */
static void gen_rsb(DisasContext *s, TCGv_i32 dst, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_sub_i32(tcg_ctx, dst, b, a);
}

static void gen_rsb_CC(DisasContext *s, TCGv_i32 dst, TCGv_i32 a, TCGv_i32 b)
{
    gen_sub_CC(s, dst, b, a);
}

static void gen_rsc(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    gen_sub_carry(s, dest, b, a);
}

static void gen_rsc_CC(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    gen_sbc_CC(s, dest, b, a);
}

static void gen_add_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_add_i32(tcg_ctx, dest, a, b);
}

static void gen_and_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_and_i32(tcg_ctx, dest, a, b);
}

static void gen_andc_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_andc_i32(tcg_ctx, dest, a, b);
}

static void gen_mov_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_mov_i32(tcg_ctx, dest, a);
}

static void gen_not_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_not_i32(tcg_ctx, dest, a);
}

static void gen_or_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_or_i32(tcg_ctx, dest, a, b);
}

static void gen_orc_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_orc_i32(tcg_ctx, dest, a, b);
}

static void gen_sub_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_sub_i32(tcg_ctx, dest, a, b);
}

static void gen_xor_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_xor_i32(tcg_ctx, dest, a, b);
}

static void gen_ext8s_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ext8s_i32(tcg_ctx, dest, src);
}

static void gen_ext16s_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ext16s_i32(tcg_ctx, dest, src);
}

static void gen_ext8u_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ext8u_i32(tcg_ctx, dest, src);
}

static void gen_ext16u_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_ext16u_i32(tcg_ctx, dest, src);
}

static void gen_bswap32_i32(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_bswap32_i32(tcg_ctx, dest, src);
}

static void gen_ssat_dectree(DisasContext *s, TCGv_i32 dest, TCGv_env env, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_ssat(tcg_ctx, dest, env, a, b);
}

static void gen_usat_dectree(DisasContext *s, TCGv_i32 dest, TCGv_env env, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_usat(tcg_ctx, dest, env, a, b);
}

static void gen_ssat16_dectree(DisasContext *s, TCGv_i32 dest, TCGv_env env, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_ssat16(tcg_ctx, dest, env, a, b);
}

static void gen_usat16_dectree(DisasContext *s, TCGv_i32 dest, TCGv_env env, TCGv_i32 a, TCGv_i32 b)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_usat16(tcg_ctx, dest, env, a, b);
}

static void gen_sxtb16_dectree(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_sxtb16(tcg_ctx, dest, src);
}

static void gen_uxtb16_dectree(DisasContext *s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_uxtb16(tcg_ctx, dest, src);
}

static void gen_rbit_dectree(DisasContext* s, TCGv_i32 dest, TCGv_i32 src)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    gen_helper_rbit(tcg_ctx, dest, src);
}

/*
 * Helpers for the data processing routines.
 *
 * After the computation store the results back.
 * This may be suppressed altogether (STREG_NONE), require a runtime
 * check against the stack limits (STREG_SP_CHECK), or generate an
 * exception return.  Oh, or store into a register.
 *
 * Always return true, indicating success for a trans_* function.
 */
typedef enum {
   STREG_NONE,
   STREG_NORMAL,
   STREG_SP_CHECK,
   STREG_EXC_RET,
} StoreRegKind;

static bool store_reg_kind(DisasContext *s, int rd,
                            TCGv_i32 val, StoreRegKind kind)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    switch (kind) {
    case STREG_NONE:
        tcg_temp_free_i32(tcg_ctx, val);
        return true;
    case STREG_NORMAL:
        /* See ALUWritePC: Interworking only from a32 mode. */
        if (s->thumb) {
            store_reg(s, rd, val);
        } else {
            store_reg_bx(s, rd, val);
        }
        return true;
    case STREG_SP_CHECK:
        store_sp_checked(s, val);
        return true;
    case STREG_EXC_RET:
        gen_exception_return(s, val);
        return true;
    }
    g_assert_not_reached();
}

/*
 * Data Processing (register)
 *
 * Operate, with set flags, one register source,
 * one immediate shifted register source, and a destination.
 */
static bool op_s_rrr_shi(DisasContext *s, arg_s_rrr_shi *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp1, tmp2;

    tmp2 = load_reg(s, a->rm);
    gen_arm_shift_im(s, tmp2, a->shty, a->shim, logic_cc);
    tmp1 = load_reg(s, a->rn);

    gen(s, tmp1, tmp1, tmp2);
    tcg_temp_free_i32(tcg_ctx, tmp2);

    if (logic_cc) {
        gen_logic_CC(s, tmp1);
    }
    return store_reg_kind(s, a->rd, tmp1, kind);
}

static bool op_s_rxr_shi(DisasContext *s, arg_s_rrr_shi *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGv_i32 tmp;

    tmp = load_reg(s, a->rm);
    gen_arm_shift_im(s, tmp, a->shty, a->shim, logic_cc);

    gen(s, tmp, tmp);
    if (logic_cc) {
        gen_logic_CC(s, tmp);
    }
    return store_reg_kind(s, a->rd, tmp, kind);
}

/*
 * Data-processing (register-shifted register)
 *
 * Operate, with set flags, one register source,
 * one register shifted register source, and a destination.
 */
static bool op_s_rrr_shr(DisasContext *s, arg_s_rrr_shr *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp1, tmp2;

    tmp1 = load_reg(s, a->rs);
    tmp2 = load_reg(s, a->rm);
    gen_arm_shift_reg(s, tmp2, a->shty, tmp1, logic_cc);
    tmp1 = load_reg(s, a->rn);

    gen(s, tmp1, tmp1, tmp2);
    tcg_temp_free_i32(tcg_ctx, tmp2);

    if (logic_cc) {
        gen_logic_CC(s, tmp1);
    }
    return store_reg_kind(s, a->rd, tmp1, kind);
}

static bool op_s_rxr_shr(DisasContext *s, arg_s_rrr_shr *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGv_i32 tmp1, tmp2;

    tmp1 = load_reg(s, a->rs);
    tmp2 = load_reg(s, a->rm);
    gen_arm_shift_reg(s, tmp2, a->shty, tmp1, logic_cc);

    gen(s, tmp2, tmp2);
    if (logic_cc) {
        gen_logic_CC(s, tmp2);
    }
    return store_reg_kind(s, a->rd, tmp2, kind);
}

/*
 * Data-processing (immediate)
 *
 * Operate, with set flags, one register source,
 * one rotated immediate, and a destination.
 *
 * Note that logic_cc && a->rot setting CF based on the msb of the
 * immediate is the reason why we must pass in the unrotated form
 * of the immediate.
 */
static bool op_s_rri_rot(DisasContext *s, arg_s_rri_rot *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp1, tmp2;
    uint32_t imm;

    imm = ror32(a->imm, a->rot);
    if (logic_cc && a->rot) {
        tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_CF, imm >> 31);
    }
    tmp2 = tcg_const_i32(tcg_ctx, imm);
    tmp1 = load_reg(s, a->rn);

    gen(s, tmp1, tmp1, tmp2);
    tcg_temp_free_i32(tcg_ctx, tmp2);

    if (logic_cc) {
        gen_logic_CC(s, tmp1);
    }
    return store_reg_kind(s, a->rd, tmp1, kind);
}

static bool op_s_rxi_rot(DisasContext *s, arg_s_rri_rot *a,
                         void (*gen)(DisasContext *, TCGv_i32, TCGv_i32),
                         int logic_cc, StoreRegKind kind)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    uint32_t imm;

    imm = ror32(a->imm, a->rot);
    if (logic_cc && a->rot) {
        tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_CF, imm >> 31);
    }
    tmp = tcg_const_i32(tcg_ctx, imm);

    gen(s, tmp, tmp);
    if (logic_cc) {
        gen_logic_CC(s, tmp);
    }
    return store_reg_kind(s, a->rd, tmp, kind);
}

#define DO_ANY3(NAME, OP, L, K)                                         \
    static bool trans_##NAME##_rrri(DisasContext *s, arg_s_rrr_shi *a)  \
    { StoreRegKind k = (K); return op_s_rrr_shi(s, a, OP, L, k); }      \
    static bool trans_##NAME##_rrrr(DisasContext *s, arg_s_rrr_shr *a)  \
    { StoreRegKind k = (K); return op_s_rrr_shr(s, a, OP, L, k); }      \
    static bool trans_##NAME##_rri(DisasContext *s, arg_s_rri_rot *a)   \
    { StoreRegKind k = (K); return op_s_rri_rot(s, a, OP, L, k); }

#define DO_ANY2(NAME, OP, L, K)                                         \
    static bool trans_##NAME##_rxri(DisasContext *s, arg_s_rrr_shi *a)  \
    { StoreRegKind k = (K); return op_s_rxr_shi(s, a, OP, L, k); }      \
    static bool trans_##NAME##_rxrr(DisasContext *s, arg_s_rrr_shr *a)  \
    { StoreRegKind k = (K); return op_s_rxr_shr(s, a, OP, L, k); }      \
    static bool trans_##NAME##_rxi(DisasContext *s, arg_s_rri_rot *a)   \
    { StoreRegKind k = (K); return op_s_rxi_rot(s, a, OP, L, k); }

#define DO_CMP2(NAME, OP, L)                                            \
    static bool trans_##NAME##_xrri(DisasContext *s, arg_s_rrr_shi *a)  \
    { return op_s_rrr_shi(s, a, OP, L, STREG_NONE); }                   \
    static bool trans_##NAME##_xrrr(DisasContext *s, arg_s_rrr_shr *a)  \
    { return op_s_rrr_shr(s, a, OP, L, STREG_NONE); }                   \
    static bool trans_##NAME##_xri(DisasContext *s, arg_s_rri_rot *a)   \
    { return op_s_rri_rot(s, a, OP, L, STREG_NONE); }

DO_ANY3(AND, gen_and_i32, a->s, STREG_NORMAL)
DO_ANY3(EOR, gen_xor_i32, a->s, STREG_NORMAL)
DO_ANY3(ORR, gen_or_i32, a->s, STREG_NORMAL)
DO_ANY3(BIC, gen_andc_i32, a->s, STREG_NORMAL)

DO_ANY3(RSB, a->s ? gen_rsb_CC : gen_rsb, false, STREG_NORMAL)
DO_ANY3(ADC, a->s ? gen_adc_CC : gen_add_carry, false, STREG_NORMAL)
DO_ANY3(SBC, a->s ? gen_sbc_CC : gen_sub_carry, false, STREG_NORMAL)
DO_ANY3(RSC, a->s ? gen_rsc_CC : gen_rsc, false, STREG_NORMAL)

DO_CMP2(TST, gen_and_i32, true)
DO_CMP2(TEQ, gen_xor_i32, true)
DO_CMP2(CMN, gen_add_CC, false)
DO_CMP2(CMP, gen_sub_CC, false)

DO_ANY3(ADD, a->s ? gen_add_CC : gen_add_i32, false,
        a->rd == 13 && a->rn == 13 ? STREG_SP_CHECK : STREG_NORMAL)

/*
 * Note for the computation of StoreRegKind we return out of the
 * middle of the functions that are expanded by DO_ANY3, and that
 * we modify a->s via that parameter before it is used by OP.
 */
DO_ANY3(SUB, a->s ? gen_sub_CC : gen_sub_i32, false,
        ({
            StoreRegKind ret = STREG_NORMAL;
            if (a->rd == 15 && a->s) {
                /*
                 * See ALUExceptionReturn:
                 * In User mode, UNPREDICTABLE; we choose UNDEF.
                 * In Hyp mode, UNDEFINED.
                 */
                if (IS_USER(s) || s->current_el == 2) {
                    unallocated_encoding(s);
                    return true;
                }
                /* There is no writeback of nzcv to PSTATE.  */
                a->s = 0;
                ret = STREG_EXC_RET;
            } else if (a->rd == 13 && a->rn == 13) {
                ret = STREG_SP_CHECK;
            }
            ret;
        }))

DO_ANY2(MOV, gen_mov_i32, a->s,
        ({
            StoreRegKind ret = STREG_NORMAL;
            if (a->rd == 15 && a->s) {
                /*
                 * See ALUExceptionReturn:
                 * In User mode, UNPREDICTABLE; we choose UNDEF.
                 * In Hyp mode, UNDEFINED.
                 */
                if (IS_USER(s) || s->current_el == 2) {
                    unallocated_encoding(s);
                    return true;
                }
                /* There is no writeback of nzcv to PSTATE.  */
                a->s = 0;
                ret = STREG_EXC_RET;
            } else if (a->rd == 13) {
                ret = STREG_SP_CHECK;
            }
            ret;
        }))

DO_ANY2(MVN, gen_not_i32, a->s, STREG_NORMAL)

/*
 * ORN is only available with T32, so there is no register-shifted-register
 * form of the insn.  Using the DO_ANY3 macro would create an unused function.
 */
static bool trans_ORN_rrri(DisasContext *s, arg_s_rrr_shi *a)
{
    return op_s_rrr_shi(s, a, gen_orc_i32, a->s, STREG_NORMAL);
}

static bool trans_ORN_rri(DisasContext *s, arg_s_rri_rot *a)
{
    return op_s_rri_rot(s, a, gen_orc_i32, a->s, STREG_NORMAL);
}

#undef DO_ANY3
#undef DO_ANY2
#undef DO_CMP2

static bool trans_ADR(DisasContext *s, arg_ri *a)
{
    store_reg_bx(s, a->rd, add_reg_for_lit(s, 15, a->imm));
    return true;
}

static bool trans_MOVW(DisasContext *s, arg_MOVW *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!ENABLE_ARCH_6T2) {
        return false;
    }

    tmp = tcg_const_i32(tcg_ctx, a->imm);
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_MOVT(DisasContext *s, arg_MOVW *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!ENABLE_ARCH_6T2) {
        return false;
    }

    tmp = load_reg(s, a->rd);
    tcg_gen_ext16u_i32(tcg_ctx, tmp, tmp);
    tcg_gen_ori_i32(tcg_ctx, tmp, tmp, a->imm << 16);
    store_reg(s, a->rd, tmp);
    return true;
}

/*
 * Multiply and multiply accumulate
 */

static bool op_mla(DisasContext *s, arg_s_rrrr *a, bool add)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    tcg_gen_mul_i32(tcg_ctx, t1, t1, t2);
    tcg_temp_free_i32(tcg_ctx, t2);
    if (add) {
        t2 = load_reg(s, a->ra);
        tcg_gen_add_i32(tcg_ctx, t1, t1, t2);
        tcg_temp_free_i32(tcg_ctx, t2);
    }
    if (a->s) {
        gen_logic_CC(s, t1);
    }
    store_reg(s, a->rd, t1);
    return true;
}

static bool trans_MUL(DisasContext *s, arg_MUL *a)
{
    return op_mla(s, a, false);
}

static bool trans_MLA(DisasContext *s, arg_MLA *a)
{
    return op_mla(s, a, true);
}

static bool trans_MLS(DisasContext *s, arg_MLS *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    tcg_gen_mul_i32(tcg_ctx, t1, t1, t2);
    tcg_temp_free_i32(tcg_ctx, t2);
    t2 = load_reg(s, a->ra);
    tcg_gen_sub_i32(tcg_ctx, t1, t2, t1);
    tcg_temp_free_i32(tcg_ctx, t2);
    store_reg(s, a->rd, t1);
    return true;
}

static bool op_mlal(DisasContext *s, arg_s_rrrr *a, bool uns, bool add)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1, t2, t3;

    t0 = load_reg(s, a->rm);
    t1 = load_reg(s, a->rn);
    if (uns) {
        tcg_gen_mulu2_i32(tcg_ctx, t0, t1, t0, t1);
    } else {
        tcg_gen_muls2_i32(tcg_ctx, t0, t1, t0, t1);
    }
    if (add) {
        t2 = load_reg(s, a->ra);
        t3 = load_reg(s, a->rd);
        tcg_gen_add2_i32(tcg_ctx, t0, t1, t0, t1, t2, t3);
        tcg_temp_free_i32(tcg_ctx, t2);
        tcg_temp_free_i32(tcg_ctx, t3);
    }
    if (a->s) {
        gen_logicq_cc(s, t0, t1);
    }
    store_reg(s, a->ra, t0);
    store_reg(s, a->rd, t1);
    return true;
}

static bool trans_UMULL(DisasContext *s, arg_UMULL *a)
{
    return op_mlal(s, a, true, false);
}

static bool trans_SMULL(DisasContext *s, arg_SMULL *a)
{
    return op_mlal(s, a, false, false);
}

static bool trans_UMLAL(DisasContext *s, arg_UMLAL *a)
{
    return op_mlal(s, a, true, true);
}

static bool trans_SMLAL(DisasContext *s, arg_SMLAL *a)
{
    return op_mlal(s, a, false, true);
}

static bool trans_UMAAL(DisasContext *s, arg_UMAAL *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1, t2, zero;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    t0 = load_reg(s, a->rm);
    t1 = load_reg(s, a->rn);
    tcg_gen_mulu2_i32(tcg_ctx, t0, t1, t0, t1);
    zero = tcg_const_i32(tcg_ctx, 0);
    t2 = load_reg(s, a->ra);
    tcg_gen_add2_i32(tcg_ctx, t0, t1, t0, t1, t2, zero);
    tcg_temp_free_i32(tcg_ctx, t2);
    t2 = load_reg(s, a->rd);
    tcg_gen_add2_i32(tcg_ctx, t0, t1, t0, t1, t2, zero);
    tcg_temp_free_i32(tcg_ctx, t2);
    tcg_temp_free_i32(tcg_ctx, zero);
    store_reg(s, a->ra, t0);
    store_reg(s, a->rd, t1);
    return true;
}

/*
 * Saturating addition and subtraction
 */

static bool op_qaddsub(DisasContext *s, arg_rrr *a, bool add, bool doub)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_5TE) {
        return false;
    }

    t0 = load_reg(s, a->rm);
    t1 = load_reg(s, a->rn);
    if (doub) {
        gen_helper_add_saturate(tcg_ctx, t1, tcg_ctx->cpu_env, t1, t1);
    }
    if (add) {
        gen_helper_add_saturate(tcg_ctx, t0, tcg_ctx->cpu_env, t0, t1);
    } else {
        gen_helper_sub_saturate(tcg_ctx, t0, tcg_ctx->cpu_env, t0, t1);
    }
    tcg_temp_free_i32(tcg_ctx, t1);
    store_reg(s, a->rd, t0);
    return true;
}

#define DO_QADDSUB(NAME, ADD, DOUB) \
static bool trans_##NAME(DisasContext *s, arg_rrr *a)    \
{                                                        \
    return op_qaddsub(s, a, ADD, DOUB);                  \
}

DO_QADDSUB(QADD, true, false)
DO_QADDSUB(QSUB, false, false)
DO_QADDSUB(QDADD, true, true)
DO_QADDSUB(QDSUB, false, true)

#undef DO_QADDSUB

/*
 * Halfword multiply and multiply accumulate
 */

static bool op_smlaxxx(DisasContext *s, arg_rrrr *a,
                       int add_long, bool nt, bool mt)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1, tl, th;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_5TE) {
        return false;
    }

    t0 = load_reg(s, a->rn);
    t1 = load_reg(s, a->rm);
    gen_mulxy(s, t0, t1, nt, mt);
    tcg_temp_free_i32(tcg_ctx, t1);

    switch (add_long) {
    case 0:
        store_reg(s, a->rd, t0);
        break;
    case 1:
        t1 = load_reg(s, a->ra);
        gen_helper_add_setq(tcg_ctx, t0, tcg_ctx->cpu_env, t0, t1);
        tcg_temp_free_i32(tcg_ctx, t1);
        store_reg(s, a->rd, t0);
        break;
    case 2:
        tl = load_reg(s, a->ra);
        th = load_reg(s, a->rd);
        t1 = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_sari_i32(tcg_ctx, t1, t0, 31);
        tcg_gen_add2_i32(tcg_ctx, tl, th, tl, th, t0, t1);
        tcg_temp_free_i32(tcg_ctx, t0);
        tcg_temp_free_i32(tcg_ctx, t1);
        store_reg(s, a->ra, tl);
        store_reg(s, a->rd, th);
        break;
    default:
        g_assert_not_reached();
    }
    return true;
}

#define DO_SMLAX(NAME, add, nt, mt) \
static bool trans_##NAME(DisasContext *s, arg_rrrr *a)     \
{                                                          \
    return op_smlaxxx(s, a, add, nt, mt);                  \
}

DO_SMLAX(SMULBB, 0, 0, 0)
DO_SMLAX(SMULBT, 0, 0, 1)
DO_SMLAX(SMULTB, 0, 1, 0)
DO_SMLAX(SMULTT, 0, 1, 1)

DO_SMLAX(SMLABB, 1, 0, 0)
DO_SMLAX(SMLABT, 1, 0, 1)
DO_SMLAX(SMLATB, 1, 1, 0)
DO_SMLAX(SMLATT, 1, 1, 1)

DO_SMLAX(SMLALBB, 2, 0, 0)
DO_SMLAX(SMLALBT, 2, 0, 1)
DO_SMLAX(SMLALTB, 2, 1, 0)
DO_SMLAX(SMLALTT, 2, 1, 1)

#undef DO_SMLAX

static bool op_smlawx(DisasContext *s, arg_rrrr *a, bool add, bool mt)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1;

    if (!ENABLE_ARCH_5TE) {
        return false;
    }

    t0 = load_reg(s, a->rn);
    t1 = load_reg(s, a->rm);
    /*
     * Since the nominal result is product<47:16>, shift the 16-bit
     * input up by 16 bits, so that the result is at product<63:32>.
     */
    if (mt) {
        tcg_gen_andi_i32(tcg_ctx, t1, t1, 0xffff0000);
    } else {
        tcg_gen_shli_i32(tcg_ctx, t1, t1, 16);
    }
    tcg_gen_muls2_i32(tcg_ctx, t0, t1, t0, t1);
    tcg_temp_free_i32(tcg_ctx, t0);
    if (add) {
        t0 = load_reg(s, a->ra);
        gen_helper_add_setq(tcg_ctx, t1, tcg_ctx->cpu_env, t1, t0);
        tcg_temp_free_i32(tcg_ctx, t0);
    }
    store_reg(s, a->rd, t1);
    return true;
}

#define DO_SMLAWX(NAME, add, mt) \
static bool trans_##NAME(DisasContext *s, arg_rrrr *a)     \
{                                                          \
    return op_smlawx(s, a, add, mt);                       \
}

DO_SMLAWX(SMULWB, 0, 0)
DO_SMLAWX(SMULWT, 0, 1)
DO_SMLAWX(SMLAWB, 1, 0)
DO_SMLAWX(SMLAWT, 1, 1)

#undef DO_SMLAWX

/*
 * MSR (immediate) and hints
 */

static bool trans_YIELD(DisasContext *s, arg_YIELD *a)
{
    /*
     * When running single-threaded TCG code, use the helper to ensure that
     * the next round-robin scheduled vCPU gets a crack.  When running in
     * MTTCG we don't generate jumps to the helper as it won't affect the
     * scheduling of other vCPUs.
     */
    if (!(tb_cflags(s->base.tb) & CF_PARALLEL)) {
        gen_set_pc_im(s, s->base.pc_next);
        s->base.is_jmp = DISAS_YIELD;
    }
    return true;
}

static bool trans_WFE(DisasContext *s, arg_WFE *a)
{
    /*
     * When running single-threaded TCG code, use the helper to ensure that
     * the next round-robin scheduled vCPU gets a crack.  In MTTCG mode we
     * just skip this instruction.  Currently the SEV/SEVL instructions,
     * which are *one* of many ways to wake the CPU from WFE, are not
     * implemented so we can't sleep like WFI does.
     */
    if (!(tb_cflags(s->base.tb) & CF_PARALLEL)) {
        gen_set_pc_im(s, s->base.pc_next);
        s->base.is_jmp = DISAS_WFE;
    }
    return true;
}

static bool trans_WFI(DisasContext *s, arg_WFI *a)
{
    /* For WFI, halt the vCPU until an IRQ. */
    gen_set_pc_im(s, s->base.pc_next);
    s->base.is_jmp = DISAS_WFI;
    return true;
}

static bool trans_NOP(DisasContext *s, arg_NOP *a)
{
    return true;
}

static bool trans_MSR_imm(DisasContext *s, arg_MSR_imm *a)
{
    uint32_t val = ror32(a->imm, a->rot * 2);
    uint32_t mask = msr_mask(s, a->mask, a->r);

    if (gen_set_psr_im(s, mask, a->r, val)) {
        unallocated_encoding(s);
    }
    return true;
}

/*
 * Miscellaneous instructions
 */

static bool trans_MRS_bank(DisasContext *s, arg_MRS_bank *a)
{
    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    gen_mrs_banked(s, a->r, a->sysm, a->rd);
    return true;
}

static bool trans_MSR_bank(DisasContext *s, arg_MSR_bank *a)
{
    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    gen_msr_banked(s, a->r, a->sysm, a->rn);
    return true;
}

static bool trans_MRS_reg(DisasContext *s, arg_MRS_reg *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (a->r) {
        if (IS_USER(s)) {
            unallocated_encoding(s);
            return true;
        }
        tmp = load_cpu_field(s, spsr);
    } else {
        tmp = tcg_temp_new_i32(tcg_ctx);
        gen_helper_cpsr_read(tcg_ctx, tmp, tcg_ctx->cpu_env);
    }
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_MSR_reg(DisasContext *s, arg_MSR_reg *a)
{
    TCGv_i32 tmp;
    uint32_t mask = msr_mask(s, a->mask, a->r);

    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    tmp = load_reg(s, a->rn);
    if (gen_set_psr(s, mask, a->r, tmp)) {
        unallocated_encoding(s);
    }
    return true;
}

static bool trans_MRS_v7m(DisasContext *s, arg_MRS_v7m *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    tmp = tcg_const_i32(tcg_ctx, a->sysm);
    gen_helper_v7m_mrs(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp);
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_MSR_v7m(DisasContext *s, arg_MSR_v7m *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, reg;

    if (!arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    addr = tcg_const_i32(tcg_ctx, (a->mask << 10) | a->sysm);
    reg = load_reg(s, a->rn);
    gen_helper_v7m_msr(tcg_ctx, tcg_ctx->cpu_env, addr, reg);
    tcg_temp_free_i32(tcg_ctx, addr);
    tcg_temp_free_i32(tcg_ctx, reg);
    gen_lookup_tb(s);
    return true;
}

/*
 * Cyclic Redundancy Check
 */

static bool op_crc32(DisasContext *s, arg_rrr *a, bool c, MemOp sz)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2, t3;

    if (!dc_isar_feature(aa32_crc32, s)) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    switch (sz) {
    case MO_8:
        gen_uxtb(t2);
        break;
    case MO_16:
        gen_uxth(t2);
        break;
    case MO_32:
        break;
    default:
        g_assert_not_reached();
    }
    t3 = tcg_const_i32(tcg_ctx, 1 << sz);
    if (c) {
        gen_helper_crc32c(tcg_ctx, t1, t1, t2, t3);
    } else {
        gen_helper_crc32(tcg_ctx, t1, t1, t2, t3);
    }
    tcg_temp_free_i32(tcg_ctx, t2);
    tcg_temp_free_i32(tcg_ctx, t3);
    store_reg(s, a->rd, t1);
    return true;
}

#define DO_CRC32(NAME, c, sz) \
static bool trans_##NAME(DisasContext *s, arg_rrr *a)  \
    { return op_crc32(s, a, c, sz); }

DO_CRC32(CRC32B, false, MO_8)
DO_CRC32(CRC32H, false, MO_16)
DO_CRC32(CRC32W, false, MO_32)
DO_CRC32(CRC32CB, true, MO_8)
DO_CRC32(CRC32CH, true, MO_16)
DO_CRC32(CRC32CW, true, MO_32)

#undef DO_CRC32

static bool trans_BX(DisasContext *s, arg_BX *a)
{
    if (!ENABLE_ARCH_4T) {
        return false;
    }
    gen_bx_excret(s, load_reg(s, a->rm));
    return true;
}

static bool trans_BXJ(DisasContext *s, arg_BXJ *a)
{
    if (!ENABLE_ARCH_5J || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    /* Trivial implementation equivalent to bx.  */
    gen_bx(s, load_reg(s, a->rm));
    return true;
}

static bool trans_BLX_r(DisasContext *s, arg_BLX_r *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!ENABLE_ARCH_5) {
        return false;
    }
    tmp = load_reg(s, a->rm);
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], s->base.pc_next | s->thumb);
    gen_bx(s, tmp);
    return true;
}

/*
 * BXNS/BLXNS: only exist for v8M with the security extensions,
 * and always UNDEF if NonSecure.  We don't implement these in
 * the user-only mode either (in theory you can use them from
 * Secure User mode but they are too tied in to system emulation).
 */
static bool trans_BXNS(DisasContext *s, arg_BXNS *a)
{
    if (!s->v8m_secure || IS_USER_ONLY) {
        unallocated_encoding(s);
    } else {
        gen_bxns(s, a->rm);
    }
    return true;
}

static bool trans_BLXNS(DisasContext *s, arg_BLXNS *a)
{
    if (!s->v8m_secure || IS_USER_ONLY) {
        unallocated_encoding(s);
    } else {
        gen_blxns(s, a->rm);
    }
    return true;
}

static bool trans_CLZ(DisasContext *s, arg_CLZ *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!ENABLE_ARCH_5) {
        return false;
    }
    tmp = load_reg(s, a->rm);
    tcg_gen_clzi_i32(tcg_ctx, tmp, tmp, 32);
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_ERET(DisasContext *s, arg_ERET *a)
{
    TCGv_i32 tmp;

    if (!arm_dc_feature(s, ARM_FEATURE_V7VE)) {
        return false;
    }
    if (IS_USER(s)) {
        unallocated_encoding(s);
        return true;
    }
    if (s->current_el == 2) {
        /* ERET from Hyp uses ELR_Hyp, not LR */
        tmp = load_cpu_field(s, elr_el[2]);
    } else {
        tmp = load_reg(s, 14);
    }
    gen_exception_return(s, tmp);
    return true;
}

static bool trans_HLT(DisasContext *s, arg_HLT *a)
{
    gen_hlt(s, a->imm);
    return true;
}

static bool trans_BKPT(DisasContext *s, arg_BKPT *a)
{
    if (!ENABLE_ARCH_5) {
        return false;
    }
    gen_exception_bkpt_insn(s, syn_aa32_bkpt(a->imm, false));
    return true;
}

static bool trans_HVC(DisasContext *s, arg_HVC *a)
{
    if (!ENABLE_ARCH_7 || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (IS_USER(s)) {
        unallocated_encoding(s);
    } else {
        gen_hvc(s, a->imm);
    }
    return true;
}

static bool trans_SMC(DisasContext *s, arg_SMC *a)
{
    if (!ENABLE_ARCH_6K || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (IS_USER(s)) {
        unallocated_encoding(s);
    } else {
        gen_smc(s);
    }
    return true;
}

static bool trans_SG(DisasContext *s, arg_SG *a)
{
    if (!arm_dc_feature(s, ARM_FEATURE_M) ||
        !arm_dc_feature(s, ARM_FEATURE_V8)) {
        return false;
    }
    /*
     * SG (v8M only)
     * The bulk of the behaviour for this instruction is implemented
     * in v7m_handle_execute_nsc(), which deals with the insn when
     * it is executed by a CPU in non-secure state from memory
     * which is Secure & NonSecure-Callable.
     * Here we only need to handle the remaining cases:
     *  * in NS memory (including the "security extension not
     *    implemented" case) : NOP
     *  * in S memory but CPU already secure (clear IT bits)
     * We know that the attribute for the memory this insn is
     * in must match the current CPU state, because otherwise
     * get_phys_addr_pmsav8 would have generated an exception.
     */
    if (s->v8m_secure) {
        /* Like the IT insn, we don't need to generate any code */
        s->condexec_cond = 0;
        s->condexec_mask = 0;
    }
    return true;
}

static bool trans_TT(DisasContext *s, arg_TT *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, tmp;

    if (!arm_dc_feature(s, ARM_FEATURE_M) ||
        !arm_dc_feature(s, ARM_FEATURE_V8)) {
        return false;
    }
    if (a->rd == 13 || a->rd == 15 || a->rn == 15) {
        /* We UNDEF for these UNPREDICTABLE cases */
        unallocated_encoding(s);
        return true;
    }
    if (a->A && !s->v8m_secure) {
        /* This case is UNDEFINED.  */
        unallocated_encoding(s);
        return true;
    }

    addr = load_reg(s, a->rn);
    tmp = tcg_const_i32(tcg_ctx, (a->A << 1) | a->T);
    gen_helper_v7m_tt(tcg_ctx, tmp, tcg_ctx->cpu_env, addr, tmp);
    tcg_temp_free_i32(tcg_ctx, addr);
    store_reg(s, a->rd, tmp);
    return true;
}

/*
 * Load/store register index
 */

static ISSInfo make_issinfo(DisasContext *s, int rd, bool p, bool w)
{
    ISSInfo ret;

    /* ISS not valid if writeback */
    if (p && !w) {
        ret = rd;
        if (s->base.pc_next - s->pc_curr == 2) {
            ret |= ISSIs16Bit;
        }
    } else {
        ret = ISSInvalid;
    }
    return ret;
}

static TCGv_i32 op_addr_rr_pre(DisasContext *s, arg_ldst_rr *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr = load_reg(s, a->rn);

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    if (a->p) {
        TCGv_i32 ofs = load_reg(s, a->rm);
        gen_arm_shift_im(s, ofs, a->shtype, a->shimm, 0);
        if (a->u) {
            tcg_gen_add_i32(tcg_ctx, addr, addr, ofs);
        } else {
            tcg_gen_sub_i32(tcg_ctx, addr, addr, ofs);
        }
        tcg_temp_free_i32(tcg_ctx, ofs);
    }
    return addr;
}

static void op_addr_rr_post(DisasContext *s, arg_ldst_rr *a,
                            TCGv_i32 addr, int address_offset)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (!a->p) {
        TCGv_i32 ofs = load_reg(s, a->rm);
        gen_arm_shift_im(s, ofs, a->shtype, a->shimm, 0);
        if (a->u) {
            tcg_gen_add_i32(tcg_ctx, addr, addr, ofs);
        } else {
            tcg_gen_sub_i32(tcg_ctx, addr, addr, ofs);
        }
        tcg_temp_free_i32(tcg_ctx, ofs);
    } else if (!a->w) {
        tcg_temp_free_i32(tcg_ctx, addr);
        return;
    }
    tcg_gen_addi_i32(tcg_ctx, addr, addr, address_offset);
    store_reg(s, a->rn, addr);
}

static bool op_load_rr(DisasContext *s, arg_ldst_rr *a,
                       MemOp mop, int mem_idx)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    ISSInfo issinfo = make_issinfo(s, a->rt, a->p, a->w);
    TCGv_i32 addr, tmp;

    addr = op_addr_rr_pre(s, a);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, mop | s->be_data);
    disas_set_da_iss(s, mop, issinfo);

    /*
     * Perform base writeback before the loaded value to
     * ensure correct behavior with overlapping index registers.
     */
    op_addr_rr_post(s, a, addr, 0);
    store_reg_from_load(s, a->rt, tmp);
    return true;
}

static bool op_store_rr(DisasContext *s, arg_ldst_rr *a,
                        MemOp mop, int mem_idx)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    ISSInfo issinfo = make_issinfo(s, a->rt, a->p, a->w) | ISSIsWrite;
    TCGv_i32 addr, tmp;

    addr = op_addr_rr_pre(s, a);

    tmp = load_reg(s, a->rt);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, mop | s->be_data);
    disas_set_da_iss(s, mop, issinfo);
    tcg_temp_free_i32(tcg_ctx, tmp);

    op_addr_rr_post(s, a, addr, 0);
    return true;
}

static bool trans_LDRD_rr(DisasContext *s, arg_ldst_rr *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int mem_idx = get_mem_index(s);
    TCGv_i32 addr, tmp;

    if (!ENABLE_ARCH_5TE) {
        return false;
    }
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    addr = op_addr_rr_pre(s, a);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    store_reg(s, a->rt, tmp);

    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    store_reg(s, a->rt + 1, tmp);

    /* LDRD w/ base writeback is undefined if the registers overlap.  */
    op_addr_rr_post(s, a, addr, -4);
    return true;
}

static bool trans_STRD_rr(DisasContext *s, arg_ldst_rr *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int mem_idx = get_mem_index(s);
    TCGv_i32 addr, tmp;

    if (!ENABLE_ARCH_5TE) {
        return false;
    }
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    addr = op_addr_rr_pre(s, a);

    tmp = load_reg(s, a->rt);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    tcg_temp_free_i32(tcg_ctx, tmp);

    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);

    tmp = load_reg(s, a->rt + 1);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    tcg_temp_free_i32(tcg_ctx, tmp);

    op_addr_rr_post(s, a, addr, -4);
    return true;
}

/*
 * Load/store immediate index
 */

static TCGv_i32 op_addr_ri_pre(DisasContext *s, arg_ldst_ri *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int ofs = a->imm;

    if (!a->u) {
        ofs = -ofs;
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        /*
         * Stackcheck. Here we know 'addr' is the current SP;
         * U is set if we're moving SP up, else down. It is
         * UNKNOWN whether the limit check triggers when SP starts
         * below the limit and ends up above it; we chose to do so.
         */
        if (!a->u) {
            TCGv_i32 newsp = tcg_temp_new_i32(tcg_ctx);
            tcg_gen_addi_i32(tcg_ctx, newsp, tcg_ctx->cpu_R[13], ofs);
            gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, newsp);
            tcg_temp_free_i32(tcg_ctx, newsp);
        } else {
            gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, tcg_ctx->cpu_R[13]);
        }
    }

    return add_reg_for_lit(s, a->rn, a->p ? ofs : 0);
}

static void op_addr_ri_post(DisasContext *s, arg_ldst_ri *a,
                            TCGv_i32 addr, int address_offset)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (!a->p) {
        if (a->u) {
            address_offset += a->imm;
        } else {
            address_offset -= a->imm;
        }
    } else if (!a->w) {
        tcg_temp_free_i32(tcg_ctx, addr);
        return;
    }
    tcg_gen_addi_i32(tcg_ctx, addr, addr, address_offset);
    store_reg(s, a->rn, addr);
}

static bool op_load_ri(DisasContext *s, arg_ldst_ri *a,
                       MemOp mop, int mem_idx)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    ISSInfo issinfo = make_issinfo(s, a->rt, a->p, a->w);
    TCGv_i32 addr, tmp;

    addr = op_addr_ri_pre(s, a);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, mop | s->be_data);
    disas_set_da_iss(s, mop, issinfo);

    /*
     * Perform base writeback before the loaded value to
     * ensure correct behavior with overlapping index registers.
     */
    op_addr_ri_post(s, a, addr, 0);
    store_reg_from_load(s, a->rt, tmp);
    return true;
}

static bool op_store_ri(DisasContext *s, arg_ldst_ri *a,
                        MemOp mop, int mem_idx)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    ISSInfo issinfo = make_issinfo(s, a->rt, a->p, a->w) | ISSIsWrite;
    TCGv_i32 addr, tmp;

    addr = op_addr_ri_pre(s, a);

    tmp = load_reg(s, a->rt);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, mop | s->be_data);
    disas_set_da_iss(s, mop, issinfo);
    tcg_temp_free_i32(tcg_ctx, tmp);

    op_addr_ri_post(s, a, addr, 0);
    return true;
}

static bool op_ldrd_ri(DisasContext *s, arg_ldst_ri *a, int rt2)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int mem_idx = get_mem_index(s);
    TCGv_i32 addr, tmp;

    addr = op_addr_ri_pre(s, a);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    store_reg(s, a->rt, tmp);

    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);

    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    store_reg(s, rt2, tmp);

    /* LDRD w/ base writeback is undefined if the registers overlap.  */
    op_addr_ri_post(s, a, addr, -4);
    return true;
}

static bool trans_LDRD_ri_a32(DisasContext *s, arg_ldst_ri *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1)) {
        return false;
    }
    return op_ldrd_ri(s, a, a->rt + 1);
}

static bool trans_LDRD_ri_t32(DisasContext *s, arg_ldst_ri2 *a)
{
    arg_ldst_ri b = {
        .u = a->u, .w = a->w, .p = a->p,
        .rn = a->rn, .rt = a->rt, .imm = a->imm
    };
    return op_ldrd_ri(s, &b, a->rt2);
}

static bool op_strd_ri(DisasContext *s, arg_ldst_ri *a, int rt2)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int mem_idx = get_mem_index(s);
    TCGv_i32 addr, tmp;

    addr = op_addr_ri_pre(s, a);

    tmp = load_reg(s, a->rt);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    tcg_temp_free_i32(tcg_ctx, tmp);

    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);

    tmp = load_reg(s, rt2);
    gen_aa32_st_i32(s, tmp, addr, mem_idx, MO_UL | s->be_data);
    tcg_temp_free_i32(tcg_ctx, tmp);

    op_addr_ri_post(s, a, addr, -4);
    return true;
}

static bool trans_STRD_ri_a32(DisasContext *s, arg_ldst_ri *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1)) {
        return false;
    }
    return op_strd_ri(s, a, a->rt + 1);
}

static bool trans_STRD_ri_t32(DisasContext *s, arg_ldst_ri2 *a)
{
    arg_ldst_ri b = {
        .u = a->u, .w = a->w, .p = a->p,
        .rn = a->rn, .rt = a->rt, .imm = a->imm
    };
    return op_strd_ri(s, &b, a->rt2);
}

#define DO_LDST(NAME, WHICH, MEMOP) \
static bool trans_##NAME##_ri(DisasContext *s, arg_ldst_ri *a)        \
{                                                                     \
    return op_##WHICH##_ri(s, a, MEMOP, get_mem_index(s));            \
}                                                                     \
static bool trans_##NAME##T_ri(DisasContext *s, arg_ldst_ri *a)       \
{                                                                     \
    return op_##WHICH##_ri(s, a, MEMOP, get_a32_user_mem_index(s));   \
}                                                                     \
static bool trans_##NAME##_rr(DisasContext *s, arg_ldst_rr *a)        \
{                                                                     \
    return op_##WHICH##_rr(s, a, MEMOP, get_mem_index(s));            \
}                                                                     \
static bool trans_##NAME##T_rr(DisasContext *s, arg_ldst_rr *a)       \
{                                                                     \
    return op_##WHICH##_rr(s, a, MEMOP, get_a32_user_mem_index(s));   \
}

DO_LDST(LDR, load, MO_UL)
DO_LDST(LDRB, load, MO_UB)
DO_LDST(LDRH, load, MO_UW)
DO_LDST(LDRSB, load, MO_SB)
DO_LDST(LDRSH, load, MO_SW)

DO_LDST(STR, store, MO_UL)
DO_LDST(STRB, store, MO_UB)
DO_LDST(STRH, store, MO_UW)

#undef DO_LDST

/*
 * Synchronization primitives
 */

static bool op_swp(DisasContext *s, arg_SWP *a, MemOp opc)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, tmp;
    TCGv taddr;

    opc |= s->be_data;
    addr = load_reg(s, a->rn);
    taddr = gen_aa32_addr(s, addr, opc);
    tcg_temp_free_i32(tcg_ctx, addr);

    tmp = load_reg(s, a->rt2);
    tcg_gen_atomic_xchg_i32(tcg_ctx, tmp, taddr, tmp, get_mem_index(s), opc);
    tcg_temp_free(tcg_ctx, taddr);

    store_reg(s, a->rt, tmp);
    return true;
}

static bool trans_SWP(DisasContext *s, arg_SWP *a)
{
    return op_swp(s, a, MO_UL | MO_ALIGN);
}

static bool trans_SWPB(DisasContext *s, arg_SWP *a)
{
    return op_swp(s, a, MO_UB);
}

/*
 * Load/Store Exclusive and Load-Acquire/Store-Release
 */

static bool op_strex(DisasContext *s, arg_STREX *a, MemOp mop, bool rel)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr;
    /* Some cases stopped being UNPREDICTABLE in v8A (but not v8M) */
    bool v8a = ENABLE_ARCH_8 && !arm_dc_feature(s, ARM_FEATURE_M);

    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rd == 15 || a->rn == 15 || a->rt == 15
        || a->rd == a->rn || a->rd == a->rt
        || (!v8a && s->thumb && (a->rd == 13 || a->rt == 13))
        || (mop == MO_64
            && (a->rt2 == 15
                || a->rd == a->rt2
                || (!v8a && s->thumb && a->rt2 == 13)))) {
        unallocated_encoding(s);
        return true;
    }

    if (rel) {
        tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_STRL);
    }

    addr = tcg_temp_local_new_i32(tcg_ctx);
    load_reg_var(s, addr, a->rn);
    tcg_gen_addi_i32(tcg_ctx, addr, addr, a->imm);

    gen_store_exclusive(s, a->rd, a->rt, a->rt2, addr, mop);
    tcg_temp_free_i32(tcg_ctx, addr);
    return true;
}

static bool trans_STREX(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    return op_strex(s, a, MO_32, false);
}

static bool trans_STREXD_a32(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_6K) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    a->rt2 = a->rt + 1;
    return op_strex(s, a, MO_64, false);
}

static bool trans_STREXD_t32(DisasContext *s, arg_STREX *a)
{
    return op_strex(s, a, MO_64, false);
}

static bool trans_STREXB(DisasContext *s, arg_STREX *a)
{
    if (s->thumb ? !ENABLE_ARCH_7 : !ENABLE_ARCH_6K) {
        return false;
    }
    return op_strex(s, a, MO_8, false);
}

static bool trans_STREXH(DisasContext *s, arg_STREX *a)
{
    if (s->thumb ? !ENABLE_ARCH_7 : !ENABLE_ARCH_6K) {
        return false;
    }
    return op_strex(s, a, MO_16, false);
}

static bool trans_STLEX(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_strex(s, a, MO_32, true);
}

static bool trans_STLEXD_a32(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    a->rt2 = a->rt + 1;
    return op_strex(s, a, MO_64, true);
}

static bool trans_STLEXD_t32(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_strex(s, a, MO_64, true);
}

static bool trans_STLEXB(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_strex(s, a, MO_8, true);
}

static bool trans_STLEXH(DisasContext *s, arg_STREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_strex(s, a, MO_16, true);
}

static bool op_stl(DisasContext *s, arg_STL *a, MemOp mop)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, tmp;

    if (!ENABLE_ARCH_8) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rn == 15 || a->rt == 15) {
        unallocated_encoding(s);
        return true;
    }

    addr = load_reg(s, a->rn);
    tmp = load_reg(s, a->rt);
    tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_STRL);
    gen_aa32_st_i32(s, tmp, addr, get_mem_index(s), mop | s->be_data);
    disas_set_da_iss(s, mop, a->rt | ISSIsAcqRel | ISSIsWrite);

    tcg_temp_free_i32(tcg_ctx, tmp);
    tcg_temp_free_i32(tcg_ctx, addr);
    return true;
}

static bool trans_STL(DisasContext *s, arg_STL *a)
{
    return op_stl(s, a, MO_UL);
}

static bool trans_STLB(DisasContext *s, arg_STL *a)
{
    return op_stl(s, a, MO_UB);
}

static bool trans_STLH(DisasContext *s, arg_STL *a)
{
    return op_stl(s, a, MO_UW);
}

static bool op_ldrex(DisasContext *s, arg_LDREX *a, MemOp mop, bool acq)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr;
    /* Some cases stopped being UNPREDICTABLE in v8A (but not v8M) */
    bool v8a = ENABLE_ARCH_8 && !arm_dc_feature(s, ARM_FEATURE_M);

    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rn == 15 || a->rt == 15
        || (!v8a && s->thumb && a->rt == 13)
        || (mop == MO_64
            && (a->rt2 == 15 || a->rt == a->rt2
                || (!v8a && s->thumb && a->rt2 == 13)))) {
        unallocated_encoding(s);
        return true;
    }

    addr = tcg_temp_local_new_i32(tcg_ctx);
    load_reg_var(s, addr, a->rn);
    tcg_gen_addi_i32(tcg_ctx, addr, addr, a->imm);

    gen_load_exclusive(s, a->rt, a->rt2, addr, mop);
    tcg_temp_free_i32(tcg_ctx, addr);

    if (acq) {
        tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_LDAQ);
    }
    return true;
}

static bool trans_LDREX(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    return op_ldrex(s, a, MO_32, false);
}

static bool trans_LDREXD_a32(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_6K) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    a->rt2 = a->rt + 1;
    return op_ldrex(s, a, MO_64, false);
}

static bool trans_LDREXD_t32(DisasContext *s, arg_LDREX *a)
{
    return op_ldrex(s, a, MO_64, false);
}

static bool trans_LDREXB(DisasContext *s, arg_LDREX *a)
{
    if (s->thumb ? !ENABLE_ARCH_7 : !ENABLE_ARCH_6K) {
        return false;
    }
    return op_ldrex(s, a, MO_8, false);
}

static bool trans_LDREXH(DisasContext *s, arg_LDREX *a)
{
    if (s->thumb ? !ENABLE_ARCH_7 : !ENABLE_ARCH_6K) {
        return false;
    }
    return op_ldrex(s, a, MO_16, false);
}

static bool trans_LDAEX(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_ldrex(s, a, MO_32, true);
}

static bool trans_LDAEXD_a32(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rt & 1) {
        unallocated_encoding(s);
        return true;
    }
    a->rt2 = a->rt + 1;
    return op_ldrex(s, a, MO_64, true);
}

static bool trans_LDAEXD_t32(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_ldrex(s, a, MO_64, true);
}

static bool trans_LDAEXB(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_ldrex(s, a, MO_8, true);
}

static bool trans_LDAEXH(DisasContext *s, arg_LDREX *a)
{
    if (!ENABLE_ARCH_8) {
        return false;
    }
    return op_ldrex(s, a, MO_16, true);
}

static bool op_lda(DisasContext *s, arg_LDA *a, MemOp mop)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, tmp;

    if (!ENABLE_ARCH_8) {
        return false;
    }
    /* We UNDEF for these UNPREDICTABLE cases.  */
    if (a->rn == 15 || a->rt == 15) {
        unallocated_encoding(s);
        return true;
    }

    addr = load_reg(s, a->rn);
    tmp = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld_i32(s, tmp, addr, get_mem_index(s), mop | s->be_data);
    disas_set_da_iss(s, mop, a->rt | ISSIsAcqRel);
    tcg_temp_free_i32(tcg_ctx, addr);

    store_reg(s, a->rt, tmp);
    tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_STRL);
    return true;
}

static bool trans_LDA(DisasContext *s, arg_LDA *a)
{
    return op_lda(s, a, MO_UL);
}

static bool trans_LDAB(DisasContext *s, arg_LDA *a)
{
    return op_lda(s, a, MO_UB);
}

static bool trans_LDAH(DisasContext *s, arg_LDA *a)
{
    return op_lda(s, a, MO_UW);
}

/*
 * Media instructions
 */

static bool trans_USADA8(DisasContext *s, arg_USADA8 *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    if (!ENABLE_ARCH_6) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    gen_helper_usad8(tcg_ctx, t1, t1, t2);
    tcg_temp_free_i32(tcg_ctx, t2);
    if (a->ra != 15) {
        t2 = load_reg(s, a->ra);
        tcg_gen_add_i32(tcg_ctx, t1, t1, t2);
        tcg_temp_free_i32(tcg_ctx, t2);
    }
    store_reg(s, a->rd, t1);
    return true;
}

static bool op_bfx(DisasContext *s, arg_UBFX *a, bool u)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    int width = a->widthm1 + 1;
    int shift = a->lsb;

    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    if (shift + width > 32) {
        /* UNPREDICTABLE; we choose to UNDEF */
        unallocated_encoding(s);
        return true;
    }

    tmp = load_reg(s, a->rn);
    if (u) {
        tcg_gen_extract_i32(tcg_ctx, tmp, tmp, shift, width);
    } else {
        tcg_gen_sextract_i32(tcg_ctx, tmp, tmp, shift, width);
    }
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_SBFX(DisasContext *s, arg_SBFX *a)
{
    return op_bfx(s, a, false);
}

static bool trans_UBFX(DisasContext *s, arg_UBFX *a)
{
    return op_bfx(s, a, true);
}

static bool trans_BFCI(DisasContext *s, arg_BFCI *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;
    int msb = a->msb, lsb = a->lsb;
    int width;

    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    if (msb < lsb) {
        /* UNPREDICTABLE; we choose to UNDEF */
        unallocated_encoding(s);
        return true;
    }

    width = msb + 1 - lsb;
    if (a->rn == 15) {
        /* BFC */
        tmp = tcg_const_i32(tcg_ctx, 0);
    } else {
        /* BFI */
        tmp = load_reg(s, a->rn);
    }
    if (width != 32) {
        TCGv_i32 tmp2 = load_reg(s, a->rd);
        tcg_gen_deposit_i32(tcg_ctx, tmp, tmp2, tmp, lsb, width);
        tcg_temp_free_i32(tcg_ctx, tmp2);
    }
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_UDF(DisasContext *s, arg_UDF *a)
{
    unallocated_encoding(s);
    return true;
}

/*
 * Parallel addition and subtraction
 */

static bool op_par_addsub(DisasContext *s, arg_rrr *a,
                          void (*gen)(TCGContext *, TCGv_i32, TCGv_i32, TCGv_i32))
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    t0 = load_reg(s, a->rn);
    t1 = load_reg(s, a->rm);

    gen(tcg_ctx, t0, t0, t1);

    tcg_temp_free_i32(tcg_ctx, t1);
    store_reg(s, a->rd, t0);
    return true;
}

static bool op_par_addsub_ge(DisasContext *s, arg_rrr *a,
                             void (*gen)(TCGContext *,
                                         TCGv_i32, TCGv_i32,
                                         TCGv_i32, TCGv_ptr))
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t0, t1;
    TCGv_ptr ge;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    t0 = load_reg(s, a->rn);
    t1 = load_reg(s, a->rm);

    ge = tcg_temp_new_ptr(tcg_ctx);
    tcg_gen_addi_ptr(tcg_ctx, ge, tcg_ctx->cpu_env, offsetof(CPUARMState, GE));
    gen(tcg_ctx, t0, t0, t1, ge);

    tcg_temp_free_ptr(tcg_ctx, ge);
    tcg_temp_free_i32(tcg_ctx, t1);
    store_reg(s, a->rd, t0);
    return true;
}

#define DO_PAR_ADDSUB(NAME, helper) \
static bool trans_##NAME(DisasContext *s, arg_rrr *a)   \
{                                                       \
    return op_par_addsub(s, a, helper);                 \
}

#define DO_PAR_ADDSUB_GE(NAME, helper) \
static bool trans_##NAME(DisasContext *s, arg_rrr *a)   \
{                                                       \
    return op_par_addsub_ge(s, a, helper);              \
}

DO_PAR_ADDSUB_GE(SADD16, gen_helper_sadd16)
DO_PAR_ADDSUB_GE(SASX, gen_helper_saddsubx)
DO_PAR_ADDSUB_GE(SSAX, gen_helper_ssubaddx)
DO_PAR_ADDSUB_GE(SSUB16, gen_helper_ssub16)
DO_PAR_ADDSUB_GE(SADD8, gen_helper_sadd8)
DO_PAR_ADDSUB_GE(SSUB8, gen_helper_ssub8)

DO_PAR_ADDSUB_GE(UADD16, gen_helper_uadd16)
DO_PAR_ADDSUB_GE(UASX, gen_helper_uaddsubx)
DO_PAR_ADDSUB_GE(USAX, gen_helper_usubaddx)
DO_PAR_ADDSUB_GE(USUB16, gen_helper_usub16)
DO_PAR_ADDSUB_GE(UADD8, gen_helper_uadd8)
DO_PAR_ADDSUB_GE(USUB8, gen_helper_usub8)

DO_PAR_ADDSUB(QADD16, gen_helper_qadd16)
DO_PAR_ADDSUB(QASX, gen_helper_qaddsubx)
DO_PAR_ADDSUB(QSAX, gen_helper_qsubaddx)
DO_PAR_ADDSUB(QSUB16, gen_helper_qsub16)
DO_PAR_ADDSUB(QADD8, gen_helper_qadd8)
DO_PAR_ADDSUB(QSUB8, gen_helper_qsub8)

DO_PAR_ADDSUB(UQADD16, gen_helper_uqadd16)
DO_PAR_ADDSUB(UQASX, gen_helper_uqaddsubx)
DO_PAR_ADDSUB(UQSAX, gen_helper_uqsubaddx)
DO_PAR_ADDSUB(UQSUB16, gen_helper_uqsub16)
DO_PAR_ADDSUB(UQADD8, gen_helper_uqadd8)
DO_PAR_ADDSUB(UQSUB8, gen_helper_uqsub8)

DO_PAR_ADDSUB(SHADD16, gen_helper_shadd16)
DO_PAR_ADDSUB(SHASX, gen_helper_shaddsubx)
DO_PAR_ADDSUB(SHSAX, gen_helper_shsubaddx)
DO_PAR_ADDSUB(SHSUB16, gen_helper_shsub16)
DO_PAR_ADDSUB(SHADD8, gen_helper_shadd8)
DO_PAR_ADDSUB(SHSUB8, gen_helper_shsub8)

DO_PAR_ADDSUB(UHADD16, gen_helper_uhadd16)
DO_PAR_ADDSUB(UHASX, gen_helper_uhaddsubx)
DO_PAR_ADDSUB(UHSAX, gen_helper_uhsubaddx)
DO_PAR_ADDSUB(UHSUB16, gen_helper_uhsub16)
DO_PAR_ADDSUB(UHADD8, gen_helper_uhadd8)
DO_PAR_ADDSUB(UHSUB8, gen_helper_uhsub8)

#undef DO_PAR_ADDSUB
#undef DO_PAR_ADDSUB_GE

/*
 * Packing, unpacking, saturation, and reversal
 */

static bool trans_PKH(DisasContext *s, arg_PKH *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tn, tm;
    int shift = a->imm;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    tn = load_reg(s, a->rn);
    tm = load_reg(s, a->rm);
    if (a->tb) {
        /* PKHTB */
        if (shift == 0) {
            shift = 31;
        }
        tcg_gen_sari_i32(tcg_ctx, tm, tm, shift);
        tcg_gen_deposit_i32(tcg_ctx, tn, tn, tm, 0, 16);
    } else {
        /* PKHBT */
        tcg_gen_shli_i32(tcg_ctx, tm, tm, shift);
        tcg_gen_deposit_i32(tcg_ctx, tn, tm, tn, 0, 16);
    }
    tcg_temp_free_i32(tcg_ctx, tm);
    store_reg(s, a->rd, tn);
    return true;
}

static bool op_sat(DisasContext *s, arg_sat *a,
                   void (*gen)(DisasContext *, TCGv_i32, TCGv_env, TCGv_i32, TCGv_i32))
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp, satimm;
    int shift = a->imm;

    if (!ENABLE_ARCH_6) {
        return false;
    }

    tmp = load_reg(s, a->rn);
    if (a->sh) {
        tcg_gen_sari_i32(tcg_ctx, tmp, tmp, shift ? shift : 31);
    } else {
        tcg_gen_shli_i32(tcg_ctx, tmp, tmp, shift);
    }

    satimm = tcg_const_i32(tcg_ctx, a->satimm);
    gen(s, tmp, tcg_ctx->cpu_env, tmp, satimm);
    tcg_temp_free_i32(tcg_ctx, satimm);

    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_SSAT(DisasContext *s, arg_sat *a)
{
    return op_sat(s, a, gen_ssat_dectree);
}

static bool trans_USAT(DisasContext *s, arg_sat *a)
{
    return op_sat(s, a, gen_usat_dectree);
}

static bool trans_SSAT16(DisasContext *s, arg_sat *a)
{
    if (s->thumb && !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)) {
        return false;
    }
    return op_sat(s, a, gen_ssat16_dectree);
}

static bool trans_USAT16(DisasContext *s, arg_sat *a)
{
    if (s->thumb && !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)) {
        return false;
    }
    return op_sat(s, a, gen_usat16_dectree);
}

static bool op_xta(DisasContext *s, arg_rrr_rot *a,
                   void (*gen_extract)(DisasContext *, TCGv_i32, TCGv_i32),
                   void (*gen_add)(DisasContext*, TCGv_i32, TCGv_i32, TCGv_i32))
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    if (!ENABLE_ARCH_6) {
        return false;
    }

    tmp = load_reg(s, a->rm);
    /*
     * TODO: In many cases we could do a shift instead of a rotate.
     * Combined with a simple extend, that becomes an extract.
     */
    tcg_gen_rotri_i32(tcg_ctx, tmp, tmp, a->rot * 8);
    gen_extract(s, tmp, tmp);

    if (a->rn != 15) {
        TCGv_i32 tmp2 = load_reg(s, a->rn);
        gen_add(s, tmp, tmp, tmp2);
        tcg_temp_free_i32(tcg_ctx, tmp2);
    }
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_SXTAB(DisasContext *s, arg_rrr_rot *a)
{
    return op_xta(s, a, gen_ext8s_i32, gen_add_i32);
}

static bool trans_SXTAH(DisasContext *s, arg_rrr_rot *a)
{
    return op_xta(s, a, gen_ext16s_i32, gen_add_i32);
}

static bool trans_SXTAB16(DisasContext *s, arg_rrr_rot *a)
{
    if (s->thumb && !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)) {
        return false;
    }
    return op_xta(s, a, gen_sxtb16_dectree, gen_add16);
}

static bool trans_UXTAB(DisasContext *s, arg_rrr_rot *a)
{
    return op_xta(s, a, gen_ext8u_i32, gen_add_i32);
}

static bool trans_UXTAH(DisasContext *s, arg_rrr_rot *a)
{
    return op_xta(s, a, gen_ext16u_i32, gen_add_i32);
}

static bool trans_UXTAB16(DisasContext *s, arg_rrr_rot *a)
{
    if (s->thumb && !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)) {
        return false;
    }
    return op_xta(s, a, gen_uxtb16_dectree, gen_add16);
}

static bool trans_SEL(DisasContext *s, arg_rrr *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2, t3;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    t3 = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, t3, tcg_ctx->cpu_env, offsetof(CPUARMState, GE));
    gen_helper_sel_flags(tcg_ctx, t1, t3, t1, t2);
    tcg_temp_free_i32(tcg_ctx, t3);
    tcg_temp_free_i32(tcg_ctx, t2);
    store_reg(s, a->rd, t1);
    return true;
}

static bool op_rr(DisasContext *s, arg_rr *a,
                  void (*gen)(DisasContext* s, TCGv_i32, TCGv_i32))
{
    TCGv_i32 tmp;

    tmp = load_reg(s, a->rm);
    gen(s, tmp, tmp);
    store_reg(s, a->rd, tmp);
    return true;
}

static bool trans_REV(DisasContext *s, arg_rr *a)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    return op_rr(s, a, gen_bswap32_i32);
}

static bool trans_REV16(DisasContext *s, arg_rr *a)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    return op_rr(s, a, gen_rev16);
}

static bool trans_REVSH(DisasContext *s, arg_rr *a)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    return op_rr(s, a, gen_revsh);
}

static bool trans_RBIT(DisasContext *s, arg_rr *a)
{
    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    return op_rr(s, a, gen_rbit_dectree);
}

/*
 * Signed multiply, signed and unsigned divide
 */

static bool op_smlad(DisasContext *s, arg_rrrr *a, bool m_swap, bool sub)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    if (!ENABLE_ARCH_6) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    if (m_swap) {
        gen_swap_half(s, t2);
    }
    gen_smul_dual(s, t1, t2);

    if (sub) {
        /* This subtraction cannot overflow. */
        tcg_gen_sub_i32(tcg_ctx, t1, t1, t2);
    } else {
        /*
         * This addition cannot overflow 32 bits; however it may
         * overflow considered as a signed operation, in which case
         * we must set the Q flag.
         */
        gen_helper_add_setq(tcg_ctx, t1, tcg_ctx->cpu_env, t1, t2);
    }
    tcg_temp_free_i32(tcg_ctx, t2);

    if (a->ra != 15) {
        t2 = load_reg(s, a->ra);
        gen_helper_add_setq(tcg_ctx, t1, tcg_ctx->cpu_env, t1, t2);
        tcg_temp_free_i32(tcg_ctx, t2);
    }
    store_reg(s, a->rd, t1);
    return true;
}

static bool trans_SMLAD(DisasContext *s, arg_rrrr *a)
{
    return op_smlad(s, a, false, false);
}

static bool trans_SMLADX(DisasContext *s, arg_rrrr *a)
{
    return op_smlad(s, a, true, false);
}

static bool trans_SMLSD(DisasContext *s, arg_rrrr *a)
{
    return op_smlad(s, a, false, true);
}

static bool trans_SMLSDX(DisasContext *s, arg_rrrr *a)
{
    return op_smlad(s, a, true, true);
}

static bool op_smlald(DisasContext *s, arg_rrrr *a, bool m_swap, bool sub)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;
    TCGv_i64 l1, l2;

    if (!ENABLE_ARCH_6) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    if (m_swap) {
        gen_swap_half(s, t2);
    }
    gen_smul_dual(s, t1, t2);

    l1 = tcg_temp_new_i64(tcg_ctx);
    l2 = tcg_temp_new_i64(tcg_ctx);
    tcg_gen_ext_i32_i64(tcg_ctx, l1, t1);
    tcg_gen_ext_i32_i64(tcg_ctx, l2, t2);
    tcg_temp_free_i32(tcg_ctx, t1);
    tcg_temp_free_i32(tcg_ctx, t2);

    if (sub) {
        tcg_gen_sub_i64(tcg_ctx, l1, l1, l2);
    } else {
        tcg_gen_add_i64(tcg_ctx, l1, l1, l2);
    }
    tcg_temp_free_i64(tcg_ctx, l2);

    gen_addq(s, l1, a->ra, a->rd);
    gen_storeq_reg(s, a->ra, a->rd, l1);
    tcg_temp_free_i64(tcg_ctx, l1);
    return true;
}

static bool trans_SMLALD(DisasContext *s, arg_rrrr *a)
{
    return op_smlald(s, a, false, false);
}

static bool trans_SMLALDX(DisasContext *s, arg_rrrr *a)
{
    return op_smlald(s, a, true, false);
}

static bool trans_SMLSLD(DisasContext *s, arg_rrrr *a)
{
    return op_smlald(s, a, false, true);
}

static bool trans_SMLSLDX(DisasContext *s, arg_rrrr *a)
{
    return op_smlald(s, a, true, true);
}

static bool op_smmla(DisasContext *s, arg_rrrr *a, bool round, bool sub)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    if (s->thumb
        ? !arm_dc_feature(s, ARM_FEATURE_THUMB_DSP)
        : !ENABLE_ARCH_6) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    tcg_gen_muls2_i32(tcg_ctx, t2, t1, t1, t2);

    if (a->ra != 15) {
        TCGv_i32 t3 = load_reg(s, a->ra);
        if (sub) {
            /*
             * For SMMLS, we need a 64-bit subtract.  Borrow caused by
             * a non-zero multiplicand lowpart, and the correct result
             * lowpart for rounding.
             */
            TCGv_i32 zero = tcg_const_i32(tcg_ctx, 0);
            tcg_gen_sub2_i32(tcg_ctx, t2, t1, zero, t3, t2, t1);
            tcg_temp_free_i32(tcg_ctx, zero);
        } else {
            tcg_gen_add_i32(tcg_ctx, t1, t1, t3);
        }
        tcg_temp_free_i32(tcg_ctx, t3);
    }
    if (round) {
        /*
         * Adding 0x80000000 to the 64-bit quantity means that we have
         * carry in to the high word when the low word has the msb set.
         */
        tcg_gen_shri_i32(tcg_ctx, t2, t2, 31);
        tcg_gen_add_i32(tcg_ctx, t1, t1, t2);
    }
    tcg_temp_free_i32(tcg_ctx, t2);
    store_reg(s, a->rd, t1);
    return true;
}

static bool trans_SMMLA(DisasContext *s, arg_rrrr *a)
{
    return op_smmla(s, a, false, false);
}

static bool trans_SMMLAR(DisasContext *s, arg_rrrr *a)
{
    return op_smmla(s, a, true, false);
}

static bool trans_SMMLS(DisasContext *s, arg_rrrr *a)
{
    return op_smmla(s, a, false, true);
}

static bool trans_SMMLSR(DisasContext *s, arg_rrrr *a)
{
    return op_smmla(s, a, true, true);
}

static bool op_div(DisasContext *s, arg_rrr *a, bool u)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 t1, t2;

    if (s->thumb
        ? !dc_isar_feature(aa32_thumb_div, s)
        : !dc_isar_feature(aa32_arm_div, s)) {
        return false;
    }

    t1 = load_reg(s, a->rn);
    t2 = load_reg(s, a->rm);
    if (u) {
        gen_helper_udiv(tcg_ctx, t1, t1, t2);
    } else {
        gen_helper_sdiv(tcg_ctx, t1, t1, t2);
    }
    tcg_temp_free_i32(tcg_ctx, t2);
    store_reg(s, a->rd, t1);
    return true;
}

static bool trans_SDIV(DisasContext *s, arg_rrr *a)
{
    return op_div(s, a, false);
}

static bool trans_UDIV(DisasContext *s, arg_rrr *a)
{
    return op_div(s, a, true);
}

/*
 * Block data transfer
 */

static TCGv_i32 op_addr_block_pre(DisasContext *s, arg_ldst_block *a, int n)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr = load_reg(s, a->rn);

    if (a->b) {
        if (a->i) {
            /* pre increment */
            tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
        } else {
            /* pre decrement */
            tcg_gen_addi_i32(tcg_ctx, addr, addr, -(n * 4));
        }
    } else if (!a->i && n != 1) {
        /* post decrement */
        tcg_gen_addi_i32(tcg_ctx, addr, addr, -((n - 1) * 4));
    }

    if (s->v8m_stackcheck && a->rn == 13 && a->w) {
        /*
         * If the writeback is incrementing SP rather than
         * decrementing it, and the initial SP is below the
         * stack limit but the final written-back SP would
         * be above, then then we must not perform any memory
         * accesses, but it is IMPDEF whether we generate
         * an exception. We choose to do so in this case.
         * At this point 'addr' is the lowest address, so
         * either the original SP (if incrementing) or our
         * final SP (if decrementing), so that's what we check.
         */
        gen_helper_v8m_stackcheck(tcg_ctx, tcg_ctx->cpu_env, addr);
    }

    return addr;
}

static void op_addr_block_post(DisasContext *s, arg_ldst_block *a,
                               TCGv_i32 addr, int n)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;

    if (a->w) {
        /* write back */
        if (!a->b) {
            if (a->i) {
                /* post increment */
                tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
            } else {
                /* post decrement */
                tcg_gen_addi_i32(tcg_ctx, addr, addr, -(n * 4));
            }
        } else if (!a->i && n != 1) {
            /* pre decrement */
            tcg_gen_addi_i32(tcg_ctx, addr, addr, -((n - 1) * 4));
        }
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }
}

static bool op_stm(DisasContext *s, arg_ldst_block *a, int min_n)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int i, j, n, list, mem_idx;
    bool user = a->u;
    TCGv_i32 addr, tmp, tmp2;

    if (user) {
        /* STM (user) */
        if (IS_USER(s)) {
            /* Only usable in supervisor mode.  */
            unallocated_encoding(s);
            return true;
        }
    }

    list = a->list;
    n = ctpop16(list);
    if (n < min_n || a->rn == 15) {
        unallocated_encoding(s);
        return true;
    }

    addr = op_addr_block_pre(s, a, n);
    mem_idx = get_mem_index(s);

    for (i = j = 0; i < 16; i++) {
        if (!(list & (1 << i))) {
            continue;
        }

        if (user && i != 15) {
            tmp = tcg_temp_new_i32(tcg_ctx);
            tmp2 = tcg_const_i32(tcg_ctx, i);
            gen_helper_get_user_reg(tcg_ctx, tmp, tcg_ctx->cpu_env, tmp2);
            tcg_temp_free_i32(tcg_ctx, tmp2);
        } else {
            tmp = load_reg(s, i);
        }
        gen_aa32_st32(s, tmp, addr, mem_idx);
        tcg_temp_free_i32(tcg_ctx, tmp);

        /* No need to add after the last transfer.  */
        if (++j != n) {
            tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
        }
    }

    op_addr_block_post(s, a, addr, n);
    return true;
}

static bool trans_STM(DisasContext *s, arg_ldst_block *a)
{
    /* BitCount(list) < 1 is UNPREDICTABLE */
    return op_stm(s, a, 1);
}

static bool trans_STM_t32(DisasContext *s, arg_ldst_block *a)
{
    /* Writeback register in register list is UNPREDICTABLE for T32.  */
    if (a->w && (a->list & (1 << a->rn))) {
        unallocated_encoding(s);
        return true;
    }
    /* BitCount(list) < 2 is UNPREDICTABLE */
    return op_stm(s, a, 2);
}

static bool do_ldm(DisasContext *s, arg_ldst_block *a, int min_n)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    int i, j, n, list, mem_idx;
    bool loaded_base;
    bool user = a->u;
    bool exc_return = false;
    TCGv_i32 addr, tmp, tmp2, loaded_var;

    if (user) {
        /* LDM (user), LDM (exception return) */
        if (IS_USER(s)) {
            /* Only usable in supervisor mode.  */
            unallocated_encoding(s);
            return true;
        }
        if (extract32(a->list, 15, 1)) {
            exc_return = true;
            user = false;
        } else {
            /* LDM (user) does not allow writeback.  */
            if (a->w) {
                unallocated_encoding(s);
                return true;
            }
        }
    }

    list = a->list;
    n = ctpop16(list);
    if (n < min_n || a->rn == 15) {
        unallocated_encoding(s);
        return true;
    }

    addr = op_addr_block_pre(s, a, n);
    mem_idx = get_mem_index(s);
    loaded_base = false;
    loaded_var = NULL;

    for (i = j = 0; i < 16; i++) {
        if (!(list & (1 << i))) {
            continue;
        }

        tmp = tcg_temp_new_i32(tcg_ctx);
        gen_aa32_ld32u(s, tmp, addr, mem_idx);
        if (user) {
            tmp2 = tcg_const_i32(tcg_ctx, i);
            gen_helper_set_user_reg(tcg_ctx, tcg_ctx->cpu_env, tmp2, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp2);
            tcg_temp_free_i32(tcg_ctx, tmp);
        } else if (i == a->rn) {
            loaded_var = tmp;
            loaded_base = true;
        } else if (i == 15 && exc_return) {
            store_pc_exc_ret(s, tmp);
        } else {
            store_reg_from_load(s, i, tmp);
        }

        /* No need to add after the last transfer.  */
        if (++j != n) {
            tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
        }
    }

    op_addr_block_post(s, a, addr, n);

    if (loaded_base) {
        /* Note that we reject base == pc above.  */
        store_reg(s, a->rn, loaded_var);
    }

    if (exc_return) {
        /* Restore CPSR from SPSR.  */
        tmp = load_cpu_field(s, spsr);
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start(tcg_ctx);
        }
        gen_helper_cpsr_write_eret(tcg_ctx, tcg_ctx->cpu_env, tmp);
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_end(tcg_ctx);
        }
        tcg_temp_free_i32(tcg_ctx, tmp);
        /* Must exit loop to check un-masked IRQs */
        s->base.is_jmp = DISAS_EXIT;
    }
    return true;
}

static bool trans_LDM_a32(DisasContext *s, arg_ldst_block *a)
{
    /*
     * Writeback register in register list is UNPREDICTABLE
     * for ArchVersion() >= 7.  Prior to v7, A32 would write
     * an UNKNOWN value to the base register.
     */
    if (ENABLE_ARCH_7 && a->w && (a->list & (1 << a->rn))) {
        unallocated_encoding(s);
        return true;
    }
    /* BitCount(list) < 1 is UNPREDICTABLE */
    return do_ldm(s, a, 1);
}

static bool trans_LDM_t32(DisasContext *s, arg_ldst_block *a)
{
    /* Writeback register in register list is UNPREDICTABLE for T32. */
    if (a->w && (a->list & (1 << a->rn))) {
        unallocated_encoding(s);
        return true;
    }
    /* BitCount(list) < 2 is UNPREDICTABLE */
    return do_ldm(s, a, 1); // SNPS changed
}

static bool trans_LDM_t16(DisasContext *s, arg_ldst_block *a)
{
    /* Writeback is conditional on the base register not being loaded.  */
    a->w = !(a->list & (1 << a->rn));
    /* BitCount(list) < 1 is UNPREDICTABLE */
    return do_ldm(s, a, 1);
}

/*
 * Branch, branch with link
 */

static bool trans_B(DisasContext *s, arg_i *a)
{
    gen_jmp(s, read_pc(s) + a->imm);
    return true;
}

static bool trans_B_cond_thumb(DisasContext *s, arg_ci *a)
{
    /* This has cond from encoding, required to be outside IT block.  */
    if (a->cond >= 0xe) {
        return false;
    }
    if (s->condexec_mask) {
        unallocated_encoding(s);
        return true;
    }
    arm_skip_unless(s, a->cond);
    gen_jmp(s, read_pc(s) + a->imm);
    return true;
}

static bool trans_BL(DisasContext *s, arg_i *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], s->base.pc_next | s->thumb);
    gen_jmp(s, read_pc(s) + a->imm);
    return true;
}

static bool trans_BLX_i(DisasContext *s, arg_BLX_i *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    /* For A32, ARCH(5) is checked near the start of the uncond block. */
    if (s->thumb && (a->imm & 2)) {
        return false;
    }
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], s->base.pc_next | s->thumb);
    tmp = tcg_const_i32(tcg_ctx, !s->thumb);
    store_cpu_field(s, tmp, thumb);
    gen_jmp(s, (read_pc(s) & ~3) + a->imm);
    return true;
}

static bool trans_BL_BLX_prefix(DisasContext *s, arg_BL_BLX_prefix *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    assert(!arm_dc_feature(s, ARM_FEATURE_THUMB2));
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], read_pc(s) + (a->imm << 12));
    return true;
}

static bool trans_BL_suffix(DisasContext *s, arg_BL_suffix *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);

    assert(!arm_dc_feature(s, ARM_FEATURE_THUMB2));
    tcg_gen_addi_i32(tcg_ctx, tmp, tcg_ctx->cpu_R[14], (a->imm << 1) | 1);
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], s->base.pc_next | 1);
    gen_bx(s, tmp);
    return true;
}

static bool trans_BLX_suffix(DisasContext *s, arg_BLX_suffix *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp;

    assert(!arm_dc_feature(s, ARM_FEATURE_THUMB2));
    if (!ENABLE_ARCH_5) {
        return false;
    }
    tmp = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_addi_i32(tcg_ctx, tmp, tcg_ctx->cpu_R[14], a->imm << 1);
    tcg_gen_andi_i32(tcg_ctx, tmp, tmp, 0xfffffffc);
    tcg_gen_movi_i32(tcg_ctx, tcg_ctx->cpu_R[14], s->base.pc_next | 1);
    gen_bx(s, tmp);
    return true;
}

static bool op_tbranch(DisasContext *s, arg_tbranch *a, bool half)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, tmp;

    tmp = load_reg(s, a->rm);
    if (half) {
        tcg_gen_add_i32(tcg_ctx, tmp, tmp, tmp);
    }
    addr = load_reg(s, a->rn);
    tcg_gen_add_i32(tcg_ctx, addr, addr, tmp);

    gen_aa32_ld_i32(s, tmp, addr, get_mem_index(s),
                    half ? MO_UW | s->be_data : MO_UB);
    tcg_temp_free_i32(tcg_ctx, addr);

    tcg_gen_add_i32(tcg_ctx, tmp, tmp, tmp);
    tcg_gen_addi_i32(tcg_ctx, tmp, tmp, read_pc(s));
    store_reg(s, 15, tmp);
    return true;
}

static bool trans_TBB(DisasContext *s, arg_tbranch *a)
{
    return op_tbranch(s, a, false);
}

static bool trans_TBH(DisasContext *s, arg_tbranch *a)
{
    return op_tbranch(s, a, true);
}

static bool trans_CBZ(DisasContext *s, arg_CBZ *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp = load_reg(s, a->rn);

    arm_gen_condlabel(s);
    tcg_gen_brcondi_i32(tcg_ctx, a->nz ? TCG_COND_EQ : TCG_COND_NE,
                        tmp, 0, s->condlabel);
    tcg_temp_free_i32(tcg_ctx, tmp);
    gen_jmp(s, read_pc(s) + a->imm);
    return true;
}

/*
 * Supervisor call
 */

static bool trans_SVC(DisasContext *s, arg_SVC *a)
{
    gen_set_pc_im(s, s->base.pc_next);
    s->svc_imm = a->imm;
    s->base.is_jmp = DISAS_SWI;
    return true;
}

/*
 * Unconditional system instructions
 */

static bool trans_RFE(DisasContext *s, arg_RFE *a)
{
    static const int8_t pre_offset[4] = {
        /* DA */ -4, /* IA */ 0, /* DB */ -8, /* IB */ 4
    };
    static const int8_t post_offset[4] = {
        /* DA */ -8, /* IA */ 4, /* DB */ -4, /* IB */ 0
    };
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 addr, t1, t2;

    if (!ENABLE_ARCH_6 || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (IS_USER(s)) {
        unallocated_encoding(s);
        return true;
    }

    addr = load_reg(s, a->rn);
    tcg_gen_addi_i32(tcg_ctx, addr, addr, pre_offset[a->pu]);

    /* Load PC into tmp and CPSR into tmp2.  */
    t1 = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld32u(s, t1, addr, get_mem_index(s));
    tcg_gen_addi_i32(tcg_ctx, addr, addr, 4);
    t2 = tcg_temp_new_i32(tcg_ctx);
    gen_aa32_ld32u(s, t2, addr, get_mem_index(s));

    if (a->w) {
        /* Base writeback.  */
        tcg_gen_addi_i32(tcg_ctx, addr, addr, post_offset[a->pu]);
        store_reg(s, a->rn, addr);
    } else {
        tcg_temp_free_i32(tcg_ctx, addr);
    }
    gen_rfe(s, t1, t2);
    return true;
}

static bool trans_SRS(DisasContext *s, arg_SRS *a)
{
    if (!ENABLE_ARCH_6 || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    gen_srs(s, a->mode, a->pu, a->w);
    return true;
}

static bool trans_CPS(DisasContext *s, arg_CPS *a)
{
    uint32_t mask, val;

    if (!ENABLE_ARCH_6 || arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (IS_USER(s)) {
        /* Implemented as NOP in user mode.  */
        return true;
    }
    /* TODO: There are quite a lot of UNPREDICTABLE argument combinations. */

    mask = val = 0;
    if (a->imod & 2) {
        if (a->A) {
            mask |= CPSR_A;
        }
        if (a->I) {
            mask |= CPSR_I;
        }
        if (a->F) {
            mask |= CPSR_F;
        }
        if (a->imod & 1) {
            val |= mask;
        }
    }
    if (a->M) {
        mask |= CPSR_M;
        val |= a->mode;
    }
    if (mask) {
        gen_set_psr_im(s, mask, 0, val);
    }
    return true;
}

static bool trans_CPS_v7m(DisasContext *s, arg_CPS_v7m *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    TCGv_i32 tmp, addr;

    if (!arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    if (IS_USER(s)) {
        /* Implemented as NOP in user mode.  */
        return true;
    }

    tmp = tcg_const_i32(tcg_ctx, a->im);
    /* FAULTMASK */
    if (a->F) {
        addr = tcg_const_i32(tcg_ctx, 19);
        gen_helper_v7m_msr(tcg_ctx, tcg_ctx->cpu_env, addr, tmp);
        tcg_temp_free_i32(tcg_ctx, addr);
    }
    /* PRIMASK */
    if (a->I) {
        addr = tcg_const_i32(tcg_ctx, 16);
        gen_helper_v7m_msr(tcg_ctx, tcg_ctx->cpu_env, addr, tmp);
        tcg_temp_free_i32(tcg_ctx, addr);
    }
    tcg_temp_free_i32(tcg_ctx, tmp);
    gen_lookup_tb(s);
    return true;
}

/*
 * Clear-Exclusive, Barriers
 */

static bool trans_CLREX(DisasContext *s, arg_CLREX *a)
{
    if (s->thumb
        ? !ENABLE_ARCH_7 && !arm_dc_feature(s, ARM_FEATURE_M)
        : !ENABLE_ARCH_6K) {
        return false;
    }
    gen_clrex(s);
    return true;
}

static bool trans_DSB(DisasContext *s, arg_DSB *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (!ENABLE_ARCH_7 && !arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_SC);
    return true;
}

static bool trans_DMB(DisasContext *s, arg_DMB *a)
{
    return trans_DSB(s, NULL);
}

static bool trans_ISB(DisasContext *s, arg_ISB *a)
{
    if (!ENABLE_ARCH_7 && !arm_dc_feature(s, ARM_FEATURE_M)) {
        return false;
    }
    /*
     * We need to break the TB after this insn to execute
     * self-modifying code correctly and also to take
     * any pending interrupts immediately.
     */
    gen_goto_tb(s, 0, s->base.pc_next);
    return true;
}

static bool trans_SB(DisasContext *s, arg_SB *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (!dc_isar_feature(aa32_sb, s)) {
        return false;
    }
    /*
     * TODO: There is no speculation barrier opcode
     * for TCG; MB and end the TB instead.
     */
    tcg_gen_mb(tcg_ctx, TCG_MO_ALL | TCG_BAR_SC);
    gen_goto_tb(s, 0, s->base.pc_next);
    return true;
}

static bool trans_SETEND(DisasContext *s, arg_SETEND *a)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    if (!ENABLE_ARCH_6) {
        return false;
    }
    if (a->E != (s->be_data == MO_BE)) {
        gen_helper_setend(tcg_ctx, tcg_ctx->cpu_env);
        s->base.is_jmp = DISAS_UPDATE;
    }
    return true;
}

/*
 * Preload instructions
 * All are nops, contingent on the appropriate arch level.
 */

static bool trans_PLD(DisasContext *s, arg_PLD *a)
{
    return ENABLE_ARCH_5TE;
}

static bool trans_PLDW(DisasContext *s, arg_PLD *a)
{
    return arm_dc_feature(s, ARM_FEATURE_V7MP);
}

static bool trans_PLI(DisasContext *s, arg_PLD *a)
{
    return ENABLE_ARCH_7;
}

/*
 * If-then
 */

static bool trans_IT(DisasContext *s, arg_IT *a)
{
    int cond_mask = a->cond_mask;

    /*
     * No actual code generated for this insn, just setup state.
     *
     * Combinations of firstcond and mask which set up an 0b1111
     * condition are UNPREDICTABLE; we take the CONSTRAINED
     * UNPREDICTABLE choice to treat 0b1111 the same as 0b1110,
     * i.e. both meaning "execute always".
     */
    s->condexec_cond = (cond_mask >> 4) & 0xe;
    s->condexec_mask = cond_mask & 0x1f;
    return true;
}

/*
 * Legacy decoder.
 */

static void disas_arm_insn(DisasContext *s, unsigned int insn)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    unsigned int cond = insn >> 28;

    /* M variants do not implement ARM mode; this must raise the INVSTATE
     * UsageFault exception.
     */
    if (arm_dc_feature(s, ARM_FEATURE_M)) {
        gen_exception_insn(s, s->pc_curr, EXCP_INVSTATE, syn_uncategorized(),
                           default_exception_el(s));
        return;
    }

    // Unicorn: trace this instruction on request
    if (HOOK_EXISTS_BOUNDED(s->uc, UC_HOOK_CODE, s->pc_curr)) {
        gen_uc_tracecode(tcg_ctx, 4, UC_HOOK_CODE_IDX, s->uc, s->pc_curr);
        // the callback might want to stop emulation immediately
        check_exit_request(tcg_ctx);
    }

    if (cond == 0xf) {
        /* In ARMv3 and v4 the NV condition is UNPREDICTABLE; we
         * choose to UNDEF. In ARMv5 and above the space is used
         * for miscellaneous unconditional instructions.
         */
        ARCH(5);

        /* Unconditional instructions.  */
        /* TODO: Perhaps merge these into one decodetree output file.  */
        if (disas_a32_uncond(s, insn) ||
            disas_vfp_uncond(s, insn) ||
            disas_neon_dp(s, insn) ||
            disas_neon_ls(s, insn) ||
            disas_neon_shared(s, insn)) {
            return;
        }
        /* fall back to legacy decoder */

        if (((insn >> 25) & 7) == 1) {
            /* NEON Data processing.  */
            if (disas_neon_data_insn(s, insn)) {
                goto illegal_op;
            }
            return;
        }
        if ((insn & 0x0e000f00) == 0x0c000100) {
            if (arm_dc_feature(s, ARM_FEATURE_IWMMXT)) {
                /* iWMMXt register transfer.  */
                if (extract32(s->c15_cpar, 1, 1)) {
                    if (!disas_iwmmxt_insn(s, insn)) {
                        return;
                    }
                }
            }
        }
        goto illegal_op;
    }
    if (cond != 0xe) {
        /* if not always execute, we generate a conditional jump to
           next instruction */
        arm_skip_unless(s, cond);
    }

    /* TODO: Perhaps merge these into one decodetree output file.  */
    if (disas_a32(s, insn) ||
        disas_vfp(s, insn)) {
        return;
    }
    /* fall back to legacy decoder */

    switch ((insn >> 24) & 0xf) {
    case 0xc:
    case 0xd:
    case 0xe:
        if (((insn >> 8) & 0xe) == 10) {
            /* VFP, but failed disas_vfp.  */
            goto illegal_op;
        }
        if (disas_coproc_insn(s, insn)) {
            /* Coprocessor.  */
            goto illegal_op;
        }
        break;
    default:
    illegal_op:
        unallocated_encoding(s);
        break;
    }
}

static bool thumb_insn_is_16bit(DisasContext *s, uint32_t pc, uint32_t insn)
{
    /*
     * Return true if this is a 16 bit instruction. We must be precise
     * about this (matching the decode).
     */
    if ((insn >> 11) < 0x1d) {
        /* Definitely a 16-bit instruction */
        return true;
    }

    /* Top five bits 0b11101 / 0b11110 / 0b11111 : this is the
     * first half of a 32-bit Thumb insn. Thumb-1 cores might
     * end up actually treating this as two 16-bit insns, though,
     * if it's half of a bl/blx pair that might span a page boundary.
     */
    if (arm_dc_feature(s, ARM_FEATURE_THUMB2) ||
        arm_dc_feature(s, ARM_FEATURE_M)) {
        /* Thumb2 cores (including all M profile ones) always treat
         * 32-bit insns as 32-bit.
         */
        return false;
    }

    if ((insn >> 11) == 0x1e && pc - s->page_start < TARGET_PAGE_SIZE - 3) {
        /* 0b1111_0xxx_xxxx_xxxx : BL/BLX prefix, and the suffix
         * is not on the next page; we merge this into a 32-bit
         * insn.
         */
        return false;
    }
    /* 0b1110_1xxx_xxxx_xxxx : BLX suffix (or UNDEF);
     * 0b1111_1xxx_xxxx_xxxx : BL suffix;
     * 0b1111_0xxx_xxxx_xxxx : BL/BLX prefix on the end of a page
     *  -- handle as single 16 bit insn
     */
    return true;
}

/* Translate a 32-bit thumb instruction. */
static void disas_thumb2_insn(DisasContext *s, uint32_t insn)
{
    /*
     * ARMv6-M supports a limited subset of Thumb2 instructions.
     * Other Thumb1 architectures allow only 32-bit
     * combined BL/BLX prefix and suffix.
     */
    if (arm_dc_feature(s, ARM_FEATURE_M) &&
        !arm_dc_feature(s, ARM_FEATURE_V7)) {
        int i;
        bool found = false;
        static const uint32_t armv6m_insn[] = {0xf3808000 /* msr */,
                                               0xf3b08040 /* dsb */,
                                               0xf3b08050 /* dmb */,
                                               0xf3b08060 /* isb */,
                                               0xf3e08000 /* mrs */,
                                               0xf000d000 /* bl */};
        static const uint32_t armv6m_mask[] = {0xffe0d000,
                                               0xfff0d0f0,
                                               0xfff0d0f0,
                                               0xfff0d0f0,
                                               0xffe0d000,
                                               0xf800d000};

        for (i = 0; i < ARRAY_SIZE(armv6m_insn); i++) {
            if ((insn & armv6m_mask[i]) == armv6m_insn[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            goto illegal_op;
        }
    } else if ((insn & 0xf800e800) != 0xf000e800)  {
        ARCH(6T2);
    }

    if ((insn & 0xef000000) == 0xef000000) {
        /*
         * T32 encodings 0b111p_1111_qqqq_qqqq_qqqq_qqqq_qqqq_qqqq
         * transform into
         * A32 encodings 0b1111_001p_qqqq_qqqq_qqqq_qqqq_qqqq_qqqq
         */
        uint32_t a32_insn = (insn & 0xe2ffffff) |
            ((insn & (1 << 28)) >> 4) | (1 << 28);

        if (disas_neon_dp(s, a32_insn)) {
            return;
        }
    }

    if ((insn & 0xff100000) == 0xf9000000) {
        /*
         * T32 encodings 0b1111_1001_ppp0_qqqq_qqqq_qqqq_qqqq_qqqq
         * transform into
         * A32 encodings 0b1111_0100_ppp0_qqqq_qqqq_qqqq_qqqq_qqqq
         */
        uint32_t a32_insn = (insn & 0x00ffffff) | 0xf4000000;

        if (disas_neon_ls(s, a32_insn)) {
            return;
        }
    }

    /*
     * TODO: Perhaps merge these into one decodetree output file.
     * Note disas_vfp is written for a32 with cond field in the
     * top nibble.  The t32 encoding requires 0xe in the top nibble.
     */
    if (disas_t32(s, insn) ||
        disas_vfp_uncond(s, insn) ||
        disas_neon_shared(s, insn) ||
        ((insn >> 28) == 0xe && disas_vfp(s, insn))) {
        return;
    }
    /* fall back to legacy decoder */

    switch ((insn >> 25) & 0xf) {
    case 0: case 1: case 2: case 3:
        /* 16-bit instructions.  Should never happen.  */
        abort();
    case 6: case 7: case 14: case 15:
        /* Coprocessor.  */
        if (arm_dc_feature(s, ARM_FEATURE_M)) {
            /* 0b111x_11xx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx */
            if (extract32(insn, 24, 2) == 3) {
                goto illegal_op; /* op0 = 0b11 : unallocated */
            }

            if (((insn >> 8) & 0xe) == 10 &&
                dc_isar_feature(aa32_fpsp_v2, s)) {
                /* FP, and the CPU supports it */
                goto illegal_op;
            } else {
                /* All other insns: NOCP */
                gen_exception_insn(s, s->pc_curr, EXCP_NOCP,
                                   syn_uncategorized(),
                                   default_exception_el(s));
            }
            break;
        }
        if (((insn >> 24) & 3) == 3) {
            /* Translate into the equivalent ARM encoding.  */
            insn = (insn & 0xe2ffffff) | ((insn & (1 << 28)) >> 4) | (1 << 28);
            if (disas_neon_data_insn(s, insn)) {
                goto illegal_op;
            }
        } else if (((insn >> 8) & 0xe) == 10) {
            /* VFP, but failed disas_vfp.  */
            goto illegal_op;
        } else {
            if (insn & (1 << 28))
                goto illegal_op;
            if (disas_coproc_insn(s, insn)) {
                goto illegal_op;
            }
        }
        break;
    case 12:
        goto illegal_op;
    default:
        goto illegal_op;
    illegal_op:
        unallocated_encoding(s);
    }
}

static void disas_thumb_insn(DisasContext *s, uint32_t insn)
{
    if (!disas_t16(s, insn)) {
        unallocated_encoding(s);
    }
}

static bool insn_crosses_page(CPUARMState *env, DisasContext *s)
{
    /* Return true if the insn at dc->base.pc_next might cross a page boundary.
     * (False positives are OK, false negatives are not.)
     * We know this is a Thumb insn, and our caller ensures we are
     * only called if dc->base.pc_next is less than 4 bytes from the page
     * boundary, so we cross the page if the first 16 bits indicate
     * that this is a 32 bit insn.
     */
    uint16_t insn = arm_lduw_code(env, s->base.pc_next, s->sctlr_b);

    return !thumb_insn_is_16bit(s, s->base.pc_next, insn);
}

static void arm_tr_init_disas_context(DisasContextBase *dcbase, CPUState *cs)
{
    TCGContext *tcg_ctx = cs->uc->tcg_ctx;
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    CPUARMState *env = cs->env_ptr;
    ARMCPU *cpu = env_archcpu(env);
    uint32_t tb_flags = dc->base.tb->flags;
    uint32_t condexec, core_mmu_idx;

    dc->uc = cs->uc;
    dc->isar = &cpu->isar;
    dc->condjmp = 0;

    dc->aarch64 = 0;
    /* If we are coming from secure EL0 in a system with a 32-bit EL3, then
     * there is no secure EL1, so we route exceptions to EL3.
     */
    dc->secure_routed_to_el3 = arm_feature(env, ARM_FEATURE_EL3) &&
                               !arm_el_is_aa64(env, 3);
    dc->thumb = FIELD_EX32(tb_flags, TBFLAG_AM32, THUMB);
    dc->be_data = FIELD_EX32(tb_flags, TBFLAG_ANY, BE_DATA) ? MO_BE : MO_LE;
    condexec = FIELD_EX32(tb_flags, TBFLAG_AM32, CONDEXEC);
    dc->condexec_mask = (condexec & 0xf) << 1;
    dc->condexec_cond = condexec >> 4;

    core_mmu_idx = FIELD_EX32(tb_flags, TBFLAG_ANY, MMUIDX);
    dc->mmu_idx = core_to_arm_mmu_idx(env, core_mmu_idx);
    dc->current_el = arm_mmu_idx_to_el(dc->mmu_idx);
#if !defined(CONFIG_USER_ONLY)
    dc->user = (dc->current_el == 0);
#endif
    dc->fp_excp_el = FIELD_EX32(tb_flags, TBFLAG_ANY, FPEXC_EL);

    if (arm_feature(env, ARM_FEATURE_M)) {
        dc->vfp_enabled = 1;
        dc->be_data = MO_TE;
        dc->v7m_handler_mode = FIELD_EX32(tb_flags, TBFLAG_M32, HANDLER);
        dc->v8m_secure = arm_feature(env, ARM_FEATURE_M_SECURITY) &&
            regime_is_secure(env, dc->mmu_idx);
        dc->v8m_stackcheck = FIELD_EX32(tb_flags, TBFLAG_M32, STACKCHECK);
        dc->v8m_fpccr_s_wrong =
            FIELD_EX32(tb_flags, TBFLAG_M32, FPCCR_S_WRONG);
        dc->v7m_new_fp_ctxt_needed =
            FIELD_EX32(tb_flags, TBFLAG_M32, NEW_FP_CTXT_NEEDED);
        dc->v7m_lspact = FIELD_EX32(tb_flags, TBFLAG_M32, LSPACT);
    } else {
        dc->be_data =
            FIELD_EX32(tb_flags, TBFLAG_ANY, BE_DATA) ? MO_BE : MO_LE;
        dc->debug_target_el =
            FIELD_EX32(tb_flags, TBFLAG_ANY, DEBUG_TARGET_EL);
        dc->sctlr_b = FIELD_EX32(tb_flags, TBFLAG_A32, SCTLR_B);
        dc->hstr_active = FIELD_EX32(tb_flags, TBFLAG_A32, HSTR_ACTIVE);
        dc->ns = FIELD_EX32(tb_flags, TBFLAG_A32, NS);
        dc->vfp_enabled = FIELD_EX32(tb_flags, TBFLAG_A32, VFPEN);
        if (arm_feature(env, ARM_FEATURE_XSCALE)) {
            dc->c15_cpar = FIELD_EX32(tb_flags, TBFLAG_A32, XSCALE_CPAR);
        } else {
            dc->vec_len = FIELD_EX32(tb_flags, TBFLAG_A32, VECLEN);
            dc->vec_stride = FIELD_EX32(tb_flags, TBFLAG_A32, VECSTRIDE);
        }
    }
    dc->cp_regs = cpu->cp_regs;
    dc->features = env->features;

    /* Single step state. The code-generation logic here is:
     *  SS_ACTIVE == 0:
     *   generate code with no special handling for single-stepping (except
     *   that anything that can make us go to SS_ACTIVE == 1 must end the TB;
     *   this happens anyway because those changes are all system register or
     *   PSTATE writes).
     *  SS_ACTIVE == 1, PSTATE.SS == 1: (active-not-pending)
     *   emit code for one insn
     *   emit code to clear PSTATE.SS
     *   emit code to generate software step exception for completed step
     *   end TB (as usual for having generated an exception)
     *  SS_ACTIVE == 1, PSTATE.SS == 0: (active-pending)
     *   emit code to generate a software step exception
     *   end the TB
     */
    dc->ss_active = FIELD_EX32(tb_flags, TBFLAG_ANY, SS_ACTIVE);
    dc->pstate_ss = FIELD_EX32(tb_flags, TBFLAG_ANY, PSTATE_SS);
    dc->is_ldex = false;

    dc->page_start = dc->base.pc_first & TARGET_PAGE_MASK;

    /* If architectural single step active, limit to 1.  */
    if (is_singlestepping(dc)) {
        dc->base.max_insns = 1;
    }

    /* ARM is a fixed-length ISA.  Bound the number of insns to execute
       to those left on the page.  */
    if (!dc->thumb) {
        int bound = -(dc->base.pc_first | TARGET_PAGE_MASK) / 4;
        dc->base.max_insns = MIN(dc->base.max_insns, bound);
    }

    dc->V0 = tcg_temp_new_i64(tcg_ctx);
    dc->V1 = tcg_temp_new_i64(tcg_ctx);
    /* FIXME: dc->M0 can probably be the same as dc->V0.  */
    dc->M0 = tcg_temp_new_i64(tcg_ctx);
}

static void arm_tr_tb_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = cpu->uc->tcg_ctx;

    /* A note on handling of the condexec (IT) bits:
     *
     * We want to avoid the overhead of having to write the updated condexec
     * bits back to the CPUARMState for every instruction in an IT block. So:
     * (1) if the condexec bits are not already zero then we write
     * zero back into the CPUARMState now. This avoids complications trying
     * to do it at the end of the block. (For example if we don't do this
     * it's hard to identify whether we can safely skip writing condexec
     * at the end of the TB, which we definitely want to do for the case
     * where a TB doesn't do anything with the IT state at all.)
     * (2) if we are going to leave the TB then we call gen_set_condexec()
     * which will write the correct value into CPUARMState if zero is wrong.
     * This is done both for leaving the TB at the end, and for leaving
     * it because of an exception we know will happen, which is done in
     * gen_exception_insn(). The latter is necessary because we need to
     * leave the TB with the PC/IT state just prior to execution of the
     * instruction which caused the exception.
     * (3) if we leave the TB unexpectedly (eg a data abort on a load)
     * then the CPUARMState will be wrong and we need to reset it.
     * This is handled in the same way as restoration of the
     * PC in these situations; we save the value of the condexec bits
     * for each PC via tcg_gen_insn_start(), and restore_state_to_opc()
     * then uses this to restore them after an exception.
     *
     * Note that there are no instructions which can read the condexec
     * bits, and none which can write non-static values to them, so
     * we don't need to care about whether CPUARMState is correct in the
     * middle of a TB.
     */

    /* Reset the conditional execution bits immediately. This avoids
       complications trying to do it at the end of the block.  */
    if (dc->condexec_mask || dc->condexec_cond) {
        TCGv_i32 tmp = tcg_temp_new_i32(tcg_ctx);
        tcg_gen_movi_i32(tcg_ctx, tmp, 0);
        store_cpu_field(dc, tmp, condexec_bits);
    }
}

static void arm_tr_insn_start(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = cpu->uc->tcg_ctx;

    tcg_gen_insn_start(tcg_ctx, dc->base.pc_next,
                       (dc->condexec_cond << 4) | (dc->condexec_mask >> 1),
                       0);
    dc->insn_start = tcg_last_op(tcg_ctx);
}

static bool arm_tr_breakpoint_check(DisasContextBase *dcbase, CPUState *cpu,
                                    const CPUBreakpoint *bp)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = cpu->uc->tcg_ctx;

    if (bp->flags & BP_CALL) { // SNPS added
        gen_set_condexec(dc);
        gen_set_pc_im(dc, dc->base.pc_next);
        gen_helper_call_breakpoints(tcg_ctx, tcg_ctx->cpu_env);
        dc->base.is_jmp = DISAS_TOO_MANY;
    } else
    if (bp->flags & BP_CPU) {
        gen_set_condexec(dc);
        gen_set_pc_im(dc, dc->base.pc_next);
        gen_helper_check_breakpoints(tcg_ctx, tcg_ctx->cpu_env);
        /* End the TB early; it's likely not going to be executed */
        dc->base.is_jmp = DISAS_TOO_MANY;
    } else {
        gen_exception_internal_insn(dc, dc->base.pc_next, EXCP_DEBUG);
        /* The address covered by the breakpoint must be
           included in [tb->pc, tb->pc + tb->size) in order
           to for it to be properly cleared -- thus we
           increment the PC here so that the logic setting
           tb->size below does the right thing.  */
        /* TODO: Advance PC by correct instruction length to
         * avoid disassembler error messages */
        dc->base.pc_next += 2;
        dc->base.is_jmp = DISAS_NORETURN;
    }

    return true;
}

static bool arm_pre_translate_insn(DisasContext *dc)
{
#ifdef CONFIG_USER_ONLY
    /* Intercept jump to the magic kernel page.  */
    if (dc->base.pc_next >= 0xffff0000) {
        /* We always get here via a jump, so know we are not in a
           conditional execution block.  */
        gen_exception_internal(dc, EXCP_KERNEL_TRAP);
        dc->base.is_jmp = DISAS_NORETURN;
        return true;
    }
#endif

    // Unicorn: end address tells us to stop emulation
    if (dc->base.pc_next == dc->uc->addr_end) {
        // imitate WFI instruction to halt emulation
        dc->base.is_jmp = DISAS_WFI;
        return true;
    }

    if (dc->ss_active && !dc->pstate_ss) {
        /* Singlestep state is Active-pending.
         * If we're in this state at the start of a TB then either
         *  a) we just took an exception to an EL which is being debugged
         *     and this is the first insn in the exception handler
         *  b) debug exceptions were masked and we just unmasked them
         *     without changing EL (eg by clearing PSTATE.D)
         * In either case we're going to take a swstep exception in the
         * "did not step an insn" case, and so the syndrome ISV and EX
         * bits should be zero.
         */
        assert(dc->base.num_insns == 1);
        gen_swstep_exception(dc, 0, 0);
        dc->base.is_jmp = DISAS_NORETURN;
        return true;
    }

    return false;
}

static void arm_post_translate_insn(DisasContext *dc)
{
    if (dc->condjmp && !dc->base.is_jmp) {
        TCGContext *tcg_ctx = dc->uc->tcg_ctx;
        gen_set_label(tcg_ctx, dc->condlabel);
        dc->condjmp = 0;
    }
    translator_loop_temp_check(&dc->base);
}

static void arm_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    CPUARMState *env = cpu->env_ptr;
    unsigned int insn;

    if (arm_pre_translate_insn(dc)) {
        return;
    }

    dc->pc_curr = dc->base.pc_next;
    insn = arm_ldl_code(env, dc->base.pc_next, dc->sctlr_b);
    dc->insn = insn;
    dc->base.pc_next += 4;
    disas_arm_insn(dc, insn);

    arm_post_translate_insn(dc);

    /* ARM is a fixed-length ISA.  We performed the cross-page check
       in init_disas_context by adjusting max_insns.  */
}

static bool thumb_insn_is_unconditional(DisasContext *s, uint32_t insn)
{
    /* Return true if this Thumb insn is always unconditional,
     * even inside an IT block. This is true of only a very few
     * instructions: BKPT, HLT, and SG.
     *
     * A larger class of instructions are UNPREDICTABLE if used
     * inside an IT block; we do not need to detect those here, because
     * what we do by default (perform the cc check and update the IT
     * bits state machine) is a permitted CONSTRAINED UNPREDICTABLE
     * choice for those situations.
     *
     * insn is either a 16-bit or a 32-bit instruction; the two are
     * distinguishable because for the 16-bit case the top 16 bits
     * are zeroes, and that isn't a valid 32-bit encoding.
     */
    if ((insn & 0xffffff00) == 0xbe00) {
        /* BKPT */
        return true;
    }

    if ((insn & 0xffffffc0) == 0xba80 && arm_dc_feature(s, ARM_FEATURE_V8) &&
        !arm_dc_feature(s, ARM_FEATURE_M)) {
        /* HLT: v8A only. This is unconditional even when it is going to
         * UNDEF; see the v8A ARM ARM DDI0487B.a H3.3.
         * For v7 cores this was a plain old undefined encoding and so
         * honours its cc check. (We might be using the encoding as
         * a semihosting trap, but we don't change the cc check behaviour
         * on that account, because a debugger connected to a real v7A
         * core and emulating semihosting traps by catching the UNDEF
         * exception would also only see cases where the cc check passed.
         * No guest code should be trying to do a HLT semihosting trap
         * in an IT block anyway.
         */
        return true;
    }

    if (insn == 0xe97fe97f && arm_dc_feature(s, ARM_FEATURE_V8) &&
        arm_dc_feature(s, ARM_FEATURE_M)) {
        /* SG: v8M only */
        return true;
    }

    return false;
}

static void thumb_tr_translate_insn(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    CPUARMState *env = cpu->env_ptr;
    TCGContext *tcg_ctx = dc->uc->tcg_ctx;
    uint32_t insn;
    bool is_16bit;

    if (arm_pre_translate_insn(dc)) {
        return;
    }

    dc->pc_curr = dc->base.pc_next;
    insn = arm_lduw_code(env, dc->base.pc_next, dc->sctlr_b);
    is_16bit = thumb_insn_is_16bit(dc, dc->base.pc_next, insn);
    dc->base.pc_next += 2;
    if (!is_16bit) {
        uint32_t insn2 = arm_lduw_code(env, dc->base.pc_next, dc->sctlr_b);

        insn = insn << 16 | insn2;
        dc->base.pc_next += 2;
    }
    dc->insn = insn;

    if (dc->condexec_mask && !thumb_insn_is_unconditional(dc, insn)) {
        uint32_t cond = dc->condexec_cond;

        /*
         * Conditionally skip the insn. Note that both 0xe and 0xf mean
         * "always"; 0xf is not "never".
         */
        if (cond < 0x0e) {
            arm_skip_unless(dc, cond);
        }
    }

    // Unicorn: trace this instruction on request
    const uint32_t insn_size = is_16bit ? 2 : 4;
    if (HOOK_EXISTS_BOUNDED(dc->uc, UC_HOOK_CODE, dc->base.pc_next - insn_size)) {
        gen_uc_tracecode(tcg_ctx, insn_size, UC_HOOK_CODE_IDX, dc->uc, dc->base.pc_next - insn_size);
        // the callback might want to stop emulation immediately
        check_exit_request(tcg_ctx);
    }

    if (is_16bit) {
        disas_thumb_insn(dc, insn);
    } else {
        disas_thumb2_insn(dc, insn);
    }

    /* Advance the Thumb condexec condition.  */
    if (dc->condexec_mask) {
        dc->condexec_cond = ((dc->condexec_cond & 0xe) |
                             ((dc->condexec_mask >> 4) & 1));
        dc->condexec_mask = (dc->condexec_mask << 1) & 0x1f;
        if (dc->condexec_mask == 0) {
            dc->condexec_cond = 0;
        }
    }

    arm_post_translate_insn(dc);

    /* Thumb is a variable-length ISA.  Stop translation when the next insn
     * will touch a new page.  This ensures that prefetch aborts occur at
     * the right place.
     *
     * We want to stop the TB if the next insn starts in a new page,
     * or if it spans between this page and the next. This means that
     * if we're looking at the last halfword in the page we need to
     * see if it's a 16-bit Thumb insn (which will fit in this TB)
     * or a 32-bit Thumb insn (which won't).
     * This is to avoid generating a silly TB with a single 16-bit insn
     * in it at the end of this page (which would execute correctly
     * but isn't very efficient).
     */
    if (dc->base.is_jmp == DISAS_NEXT
        && (dc->base.pc_next - dc->page_start >= TARGET_PAGE_SIZE
            || (dc->base.pc_next - dc->page_start >= TARGET_PAGE_SIZE - 3
                && insn_crosses_page(env, dc)))) {
        dc->base.is_jmp = DISAS_TOO_MANY;
    }
}

static void arm_tr_tb_stop(DisasContextBase *dcbase, CPUState *cpu)
{
    DisasContext *dc = container_of(dcbase, DisasContext, base);
    TCGContext *tcg_ctx = cpu->uc->tcg_ctx;

    if (tb_cflags(dc->base.tb) & CF_LAST_IO && dc->condjmp) {
        /* FIXME: This can theoretically happen with self-modifying code. */
        cpu_abort(cpu, "IO on conditional branch instruction");
    }

    /* At this stage dc->condjmp will only be set when the skipped
       instruction was a conditional branch or trap, and the PC has
       already been written.  */
    gen_set_condexec(dc);
    if (dc->base.is_jmp == DISAS_BX_EXCRET) {
        /* Exception return branches need some special case code at the
         * end of the TB, which is complex enough that it has to
         * handle the single-step vs not and the condition-failed
         * insn codepath itself.
         */
        gen_bx_excret_final_code(dc);
    } else if (unlikely(is_singlestepping(dc))) {
        /* Unconditional and "condition passed" instruction codepath. */
        switch (dc->base.is_jmp) {
        case DISAS_SWI:
            gen_ss_advance(dc);
            gen_exception(dc, EXCP_SWI, syn_aa32_svc(dc->svc_imm, dc->thumb),
                          default_exception_el(dc));
            break;
        case DISAS_HVC:
            gen_ss_advance(dc);
            gen_exception(dc, EXCP_HVC, syn_aa32_hvc(dc->svc_imm), 2);
            break;
        case DISAS_SMC:
            gen_ss_advance(dc);
            gen_exception(dc, EXCP_SMC, syn_aa32_smc(), 3);
            break;
        case DISAS_NEXT:
        case DISAS_TOO_MANY:
        case DISAS_UPDATE:
            gen_set_pc_im(dc, dc->base.pc_next);
            /* fall through */
        default:
            /* FIXME: Single stepping a WFI insn will not halt the CPU. */
            gen_singlestep_exception(dc);
            break;
        case DISAS_NORETURN:
            break;
        }
    } else {
        /* While branches must always occur at the end of an IT block,
           there are a few other things that can cause us to terminate
           the TB in the middle of an IT block:
            - Exception generating instructions (bkpt, swi, undefined).
            - Page boundaries.
            - Hardware watchpoints.
           Hardware breakpoints have already been handled and skip this code.
         */
        switch(dc->base.is_jmp) {
        case DISAS_NEXT:
        case DISAS_TOO_MANY:
            gen_goto_tb(dc, 1, dc->base.pc_next);
            break;
        case DISAS_JUMP:
            gen_goto_ptr(dc);
            break;
        case DISAS_UPDATE:
            gen_set_pc_im(dc, dc->base.pc_next);
            /* fall through */
        default:
            /* indicate that the hash table must be used to find the next TB */
            tcg_gen_exit_tb(tcg_ctx, NULL, 0);
            break;
        case DISAS_NORETURN:
            /* nothing more to generate */
            break;
        case DISAS_WFI:
        {
            TCGv_i32 tmp = tcg_const_i32(tcg_ctx, (dc->thumb &&
                                          !(dc->insn & (1U << 31))) ? 2 : 4);

            gen_helper_wfi(tcg_ctx, tcg_ctx->cpu_env, tmp);
            tcg_temp_free_i32(tcg_ctx, tmp);
            /* The helper doesn't necessarily throw an exception, but we
             * must go back to the main loop to check for interrupts anyway.
             */
            tcg_gen_exit_tb(tcg_ctx, NULL, 0);
            break;
        }
        case DISAS_WFE:
            // SNPS removed: gen_helper_wfe(tcg_ctx, tcg_ctx->cpu_env);
            break;
        case DISAS_YIELD:
            // SNPS removed: gen_helper_yield(tcg_ctx, tcg_ctx->cpu_env);
            break;
        case DISAS_SWI:
            gen_exception(dc, EXCP_SWI, syn_aa32_svc(dc->svc_imm, dc->thumb),
                          default_exception_el(dc));
            break;
        case DISAS_HVC:
            gen_exception(dc, EXCP_HVC, syn_aa32_hvc(dc->svc_imm), 2);
            break;
        case DISAS_SMC:
            gen_exception(dc, EXCP_SMC, syn_aa32_smc(), 3);
            break;
        }
    }

    if (dc->condjmp) {
        /* "Condition failed" instruction codepath for the branch/trap insn */
        gen_set_label(tcg_ctx, dc->condlabel);
        gen_set_condexec(dc);
    if (unlikely(is_singlestepping(dc))) {
            gen_set_pc_im(dc, dc->base.pc_next);
            gen_singlestep_exception(dc);
        } else {
            gen_goto_tb(dc, 1, dc->base.pc_next);
        }
    }
}

static void arm_tr_disas_log(const DisasContextBase *dcbase, CPUState *cpu)
{
    // Unicorn: if'd out
#if 0
    DisasContext *dc = container_of(dcbase, DisasContext, base);

    qemu_log("IN: %s\n", lookup_symbol(dc->base.pc_first));
    log_target_disas(cpu, dc->base.pc_first, dc->base.tb->size);
#endif
}

static const TranslatorOps arm_translator_ops = {
    .init_disas_context = arm_tr_init_disas_context,
    .tb_start           = arm_tr_tb_start,
    .insn_start         = arm_tr_insn_start,
    .breakpoint_check   = arm_tr_breakpoint_check,
    .translate_insn     = arm_tr_translate_insn,
    .tb_stop            = arm_tr_tb_stop,
    .disas_log          = arm_tr_disas_log,
};

static const TranslatorOps thumb_translator_ops = {
    .init_disas_context = arm_tr_init_disas_context,
    .tb_start           = arm_tr_tb_start,
    .insn_start         = arm_tr_insn_start,
    .breakpoint_check   = arm_tr_breakpoint_check,
    .translate_insn     = thumb_tr_translate_insn,
    .tb_stop            = arm_tr_tb_stop,
    .disas_log          = arm_tr_disas_log,
};

/* generate intermediate code for basic block 'tb'.  */
void gen_intermediate_code(CPUState *cpu, TranslationBlock *tb, int max_insns)
{
    DisasContext dc = {};
    const TranslatorOps *ops = &arm_translator_ops;

    if (FIELD_EX32(tb->flags, TBFLAG_AM32, THUMB)) {
        ops = &thumb_translator_ops;
    }
#ifdef TARGET_AARCH64
    if (FIELD_EX32(tb->flags, TBFLAG_ANY, AARCH64_STATE)) {
        ops = &aarch64_translator_ops;
    }
#endif

    translator_loop(ops, &dc.base, cpu, tb, max_insns);
}

#if 0

void arm_cpu_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf,
                        int flags)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    int i;

    if (is_a64(env)) {
        aarch64_cpu_dump_state(cs, f, cpu_fprintf, flags);
        return;
    }

    for (i = 0; i < 16; i++) {
        cpu_fprintf(f, "R%02d=%08x", i, env->regs[i]);
        if ((i % 4) == 3) {
            cpu_fprintf(f, "\n");
        } else {
            cpu_fprintf(f, " ");
        }
    }

    if (arm_feature(env, ARM_FEATURE_M)) {
        uint32_t xpsr = xpsr_read(env);
        const char *mode;
        const char *ns_status = "";

        if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            ns_status = env->v7m.secure ? "S " : "NS ";
        }

        if (xpsr & XPSR_EXCP) {
            mode = "handler";
        } else {
            if (env->v7m.control[env->v7m.secure] & R_V7M_CONTROL_NPRIV_MASK) {
                mode = "unpriv-thread";
            } else {
                mode = "priv-thread";
            }
        }

        cpu_fprintf(f, "XPSR=%08x %c%c%c%c %c %s%s\n",
                    xpsr,
                    xpsr & XPSR_N ? 'N' : '-',
                    xpsr & XPSR_Z ? 'Z' : '-',
                    xpsr & XPSR_C ? 'C' : '-',
                    xpsr & XPSR_V ? 'V' : '-',
                    xpsr & XPSR_T ? 'T' : 'A',
                    ns_status,
                    mode);
    } else {
        uint32_t psr = cpsr_read(env);
        const char *ns_status = "";

        if (arm_feature(env, ARM_FEATURE_EL3) &&
            (psr & CPSR_M) != ARM_CPU_MODE_MON) {
            ns_status = env->cp15.scr_el3 & SCR_NS ? "NS " : "S ";
        }

        cpu_fprintf(f, "PSR=%08x %c%c%c%c %c %s%s%d\n",
                    psr,
                    psr & CPSR_N ? 'N' : '-',
                    psr & CPSR_Z ? 'Z' : '-',
                    psr & CPSR_C ? 'C' : '-',
                    psr & CPSR_V ? 'V' : '-',
                    psr & CPSR_T ? 'T' : 'A',
                    ns_status,
                    aarch32_mode_name(psr), (psr & 0x10) ? 32 : 26);
    }

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 0;
        if (cpu_isar_feature(aa32_simd_r32, cpu)) {
            numvfpregs = 32;
        } else if (arm_feature(env, ARM_FEATURE_VFP)) {
            numvfpregs = 16;
        }
        for (i = 0; i < numvfpregs; i++) {
            uint64_t v = *aa32_vfp_dreg(env, i);
            cpu_fprintf(f, "s%02d=%08x s%02d=%08x d%02d=%016" PRIx64 "\n",
                        i * 2, (uint32_t)v,
                        i * 2 + 1, (uint32_t)(v >> 32),
                        i, v);
        }
        cpu_fprintf(f, "FPSCR: %08x\n", vfp_get_fpscr(env));
    }
}
#endif

void restore_state_to_opc(CPUARMState *env, TranslationBlock *tb,
                          target_ulong *data)
{
    if (is_a64(env)) {
        env->pc = data[0];
        env->condexec_bits = 0;
        env->exception.syndrome = data[2] << ARM_INSN_START_WORD2_SHIFT;
    } else {
        env->regs[15] = data[0];
        env->condexec_bits = data[1];
        env->exception.syndrome = data[2] << ARM_INSN_START_WORD2_SHIFT;
    }
}
