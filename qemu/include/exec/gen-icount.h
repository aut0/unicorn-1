#ifndef GEN_ICOUNT_H
#define GEN_ICOUNT_H

#include "qemu/timer.h"

/* Helpers for instruction counting code generation.  */

//static TCGOp *icount_start_insn;
//static TCGLabel *icount_label;
//static TCGLabel *exitreq_label;

static inline void gen_tb_start(TCGContext *tcg_ctx, TranslationBlock *tb)
{
    //TCGv_i32 count, flag, imm;
    TCGv_i32 flag;
    TCGv_i64 ninsn;
    TCGv_i64 limit;
    TCGv_i64 dummy;

    tcg_ctx->exitreq_label = gen_new_label(tcg_ctx);
    flag = tcg_temp_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, flag, tcg_ctx->cpu_env,
                   offsetof(CPUState, tcg_exit_req) - ENV_OFFSET);
    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_NE, flag, 0, tcg_ctx->exitreq_label);
    tcg_temp_free_i32(tcg_ctx, flag);

    // JHW: check if we depleted our instruction budget
    tcg_ctx->icount_label = gen_new_label(tcg_ctx);
    limit = tcg_temp_new_i64(tcg_ctx);
    ninsn = tcg_temp_local_new_i64(tcg_ctx);
    tcg_gen_ld_i64(tcg_ctx, ninsn, tcg_ctx->cpu_env,
                   offsetof(CPUState, insn_count) - ENV_OFFSET);
    tcg_gen_ld_i64(tcg_ctx, limit, tcg_ctx->cpu_env,
                   offsetof(CPUState, insn_limit) - ENV_OFFSET);
    tcg_gen_brcond_i64(tcg_ctx, TCG_COND_GE, ninsn, limit,
                       tcg_ctx->icount_label);
    tcg_temp_free_i64(tcg_ctx, limit);

    // JHW update cpu->insn_count
    dummy = tcg_temp_new_i64(tcg_ctx);
    tcg_gen_movi_i64(tcg_ctx, dummy, 0xfefefefefefefefe);
    tcg_ctx->icount_op = tcg_last_op(tcg_ctx);
    tcg_gen_add_i64(tcg_ctx, ninsn, ninsn, dummy);
    tcg_temp_free_i64(tcg_ctx, dummy);
    tcg_gen_st_i64(tcg_ctx, ninsn, tcg_ctx->cpu_env,
                   offsetof(CPUState, insn_count) - ENV_OFFSET);
    tcg_temp_free_i64(tcg_ctx, ninsn);

    // JHW gen basic block tracing code if requested
    if (tcg_ctx->uc->uc_trace_bb_func != NULL) {
        TCGv_i64 pc = tcg_temp_new_i64(tcg_ctx);
        tcg_gen_movi_i64(tcg_ctx, pc, (int64_t)tb->pc);
        gen_helper_trace_tb_entry(tcg_ctx, tcg_ctx->tcg_env, pc);
        tcg_temp_free_i64(tcg_ctx, pc);
    }

#if 0
    if (!(tb->cflags & CF_USE_ICOUNT))
        return;
    }

    icount_label = gen_new_label(tcg_ctx);
    count = tcg_temp_local_new_i32(tcg_ctx);
    tcg_gen_ld_i32(tcg_ctx, count, tcg_ctx->tcg_env,
                   -ENV_OFFSET + offsetof(CPUState, icount_decr.u32));
    imm = tcg_temp_new_i32(tcg_ctx);
    /* We emit a movi with a dummy immediate argument. Keep the insn index
     * of the movi so that we later (when we know the actual insn count)
     * can update the immediate argument with the actual insn count.  */
    icount_start_insn = tcg_last_op(tcg_ctx);
    tcg_gen_movi_i32(tcg_ctx, imm, 0xdeadbeef);

    tcg_gen_sub_i32(tcg_ctx, count, count, imm);
    tcg_temp_free_i32(tcg_ctx, imm);

    tcg_gen_brcondi_i32(tcg_ctx, TCG_COND_LT, count, 0, icount_label);
    tcg_gen_st16_i32(tcg_ctx, count, tcg_ctx->tcg_env,
                     -ENV_OFFSET + offsetof(CPUState, icount_decr.u16.low));
    tcg_temp_free_i32(tcg_ctx, count);
#endif
}

static inline void gen_tb_end(TCGContext *tcg_ctx, TranslationBlock *tb, int num_insns)
{
    gen_set_label(tcg_ctx, tcg_ctx->exitreq_label);
    tcg_gen_exit_tb(tcg_ctx, tb, TB_EXIT_REQUESTED);

    if (tcg_ctx->uc->emu_count == 1) {
        assert(num_insns == 1);
    }

    tcg_set_insn_param(tcg_ctx->icount_op, 1, num_insns);
    gen_set_label(tcg_ctx, tcg_ctx->icount_label);
    tcg_gen_exit_tb(tcg_ctx, tb, TB_EXIT_ICOUNT_EXPIRED);

#if 0
    if (tb->cflags & CF_USE_ICOUNT) {
        /* Update the num_insn immediate parameter now that we know
         * the actual insn count.  */
        tcg_set_insn_param(tcg_ctx->icount_start_insn, 1, num_insns);
        gen_set_label(tcg_ctx, icount_label);
        tcg_gen_exit_tb(tcg_ctx, (uintptr_t)tb + TB_EXIT_ICOUNT_EXPIRED);
    }
#endif
}

#if 0
static inline void gen_io_start(TCGContext *tcg_ctx)
{
    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 1);
    tcg_gen_st_i32(tcg_ctx, tmp, tcg_ctx->tcg_env, -ENV_OFFSET + offsetof(CPUState, can_do_io));
    tcg_temp_free_i32(tcg_ctx, tmp);
}

static inline void gen_io_end(TCGContext *tcg_ctx)
{
    TCGv_i32 tmp = tcg_const_i32(tcg_ctx, 0);
    tcg_gen_st_i32(tcg_ctx, tmp, tcg_ctx->tcg_env, -ENV_OFFSET + offsetof(CPUState, can_do_io));
    tcg_temp_free_i32(tcg_ctx, tmp);
}
#endif

#endif
