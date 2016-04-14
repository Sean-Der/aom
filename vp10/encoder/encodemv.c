/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <math.h>

#include "vp10/common/common.h"
#include "vp10/common/entropymode.h"

#include "vp10/encoder/cost.h"
#include "vp10/encoder/encodemv.h"
#include "vp10/encoder/subexp.h"

#include "vpx_dsp/vpx_dsp_common.h"

static struct vp10_token mv_joint_encodings[MV_JOINTS];
static struct vp10_token mv_class_encodings[MV_CLASSES];
static struct vp10_token mv_fp_encodings[MV_FP_SIZE];
static struct vp10_token mv_class0_encodings[CLASS0_SIZE];

void vp10_entropy_mv_init(void) {
  vp10_tokens_from_tree(mv_joint_encodings, vp10_mv_joint_tree);
  vp10_tokens_from_tree(mv_class_encodings, vp10_mv_class_tree);
  vp10_tokens_from_tree(mv_class0_encodings, vp10_mv_class0_tree);
  vp10_tokens_from_tree(mv_fp_encodings, vp10_mv_fp_tree);
}

static void encode_mv_component(vp10_writer* w, int comp,
                                const nmv_component* mvcomp, int usehp) {
  int offset;
  const int sign = comp < 0;
  const int mag = sign ? -comp : comp;
  const int mv_class = vp10_get_mv_class(mag - 1, &offset);
  const int d = offset >> 3;                // int mv data
  const int fr = (offset >> 1) & 3;         // fractional mv data
  const int hp = offset & 1;                // high precision mv data

  assert(comp != 0);

  // Sign
  vp10_write(w, sign, mvcomp->sign);

  // Class
  vp10_write_token(w, vp10_mv_class_tree, mvcomp->classes,
                  &mv_class_encodings[mv_class]);

  // Integer bits
  if (mv_class == MV_CLASS_0) {
    vp10_write_token(w, vp10_mv_class0_tree, mvcomp->class0,
                    &mv_class0_encodings[d]);
  } else {
    int i;
    const int n = mv_class + CLASS0_BITS - 1;  // number of bits
    for (i = 0; i < n; ++i)
      vp10_write(w, (d >> i) & 1, mvcomp->bits[i]);
  }

  // Fractional bits
  vp10_write_token(w, vp10_mv_fp_tree,
                  mv_class == MV_CLASS_0 ?  mvcomp->class0_fp[d] : mvcomp->fp,
                  &mv_fp_encodings[fr]);

  // High precision bit
  if (usehp)
    vp10_write(w, hp,
              mv_class == MV_CLASS_0 ? mvcomp->class0_hp : mvcomp->hp);
}


static void build_nmv_component_cost_table(int *mvcost,
                                           const nmv_component* const mvcomp,
                                           int usehp) {
  int i, v;
  int sign_cost[2], class_cost[MV_CLASSES], class0_cost[CLASS0_SIZE];
  int bits_cost[MV_OFFSET_BITS][2];
  int class0_fp_cost[CLASS0_SIZE][MV_FP_SIZE], fp_cost[MV_FP_SIZE];
  int class0_hp_cost[2], hp_cost[2];

  sign_cost[0] = vp10_cost_zero(mvcomp->sign);
  sign_cost[1] = vp10_cost_one(mvcomp->sign);
  vp10_cost_tokens(class_cost, mvcomp->classes, vp10_mv_class_tree);
  vp10_cost_tokens(class0_cost, mvcomp->class0, vp10_mv_class0_tree);
  for (i = 0; i < MV_OFFSET_BITS; ++i) {
    bits_cost[i][0] = vp10_cost_zero(mvcomp->bits[i]);
    bits_cost[i][1] = vp10_cost_one(mvcomp->bits[i]);
  }

  for (i = 0; i < CLASS0_SIZE; ++i)
    vp10_cost_tokens(class0_fp_cost[i], mvcomp->class0_fp[i], vp10_mv_fp_tree);
  vp10_cost_tokens(fp_cost, mvcomp->fp, vp10_mv_fp_tree);

  if (usehp) {
    class0_hp_cost[0] = vp10_cost_zero(mvcomp->class0_hp);
    class0_hp_cost[1] = vp10_cost_one(mvcomp->class0_hp);
    hp_cost[0] = vp10_cost_zero(mvcomp->hp);
    hp_cost[1] = vp10_cost_one(mvcomp->hp);
  }
  mvcost[0] = 0;
  for (v = 1; v <= MV_MAX; ++v) {
    int z, c, o, d, e, f, cost = 0;
    z = v - 1;
    c = vp10_get_mv_class(z, &o);
    cost += class_cost[c];
    d = (o >> 3);               /* int mv data */
    f = (o >> 1) & 3;           /* fractional pel mv data */
    e = (o & 1);                /* high precision mv data */
    if (c == MV_CLASS_0) {
      cost += class0_cost[d];
    } else {
      int i, b;
      b = c + CLASS0_BITS - 1;  /* number of bits */
      for (i = 0; i < b; ++i)
        cost += bits_cost[i][((d >> i) & 1)];
    }
    if (c == MV_CLASS_0) {
      cost += class0_fp_cost[d][f];
    } else {
      cost += fp_cost[f];
    }
    if (usehp) {
      if (c == MV_CLASS_0) {
        cost += class0_hp_cost[e];
      } else {
        cost += hp_cost[e];
      }
    }
    mvcost[v] = cost + sign_cost[0];
    mvcost[-v] = cost + sign_cost[1];
  }
}

static void update_mv(vp10_writer *w, const unsigned int ct[2], vpx_prob *cur_p,
                      vpx_prob upd_p) {
  (void) upd_p;
  vp10_cond_prob_diff_update(w, cur_p, ct);
}

static void write_mv_update(const vpx_tree_index *tree,
                            vpx_prob probs[/*n - 1*/],
                            const unsigned int counts[/*n - 1*/],
                            int n, vp10_writer *w) {
  int i;
  unsigned int branch_ct[32][2];

  // Assuming max number of probabilities <= 32
  assert(n <= 32);

  vp10_tree_probs_from_distribution(tree, branch_ct, counts);
  for (i = 0; i < n - 1; ++i)
    update_mv(w, branch_ct[i], &probs[i], MV_UPDATE_PROB);
}

void vp10_write_nmv_probs(VP10_COMMON *cm, int usehp, vp10_writer *w,
                          nmv_context_counts *const nmv_counts) {
  int i, j;
#if CONFIG_REF_MV
  int nmv_ctx = 0;
  for (nmv_ctx = 0; nmv_ctx < NMV_CONTEXTS; ++nmv_ctx) {
    nmv_context *const mvc = &cm->fc->nmvc[nmv_ctx];
    nmv_context_counts *const counts = &nmv_counts[nmv_ctx];
    write_mv_update(vp10_mv_joint_tree, mvc->joints, counts->joints,
                    MV_JOINTS, w);

    vp10_cond_prob_diff_update(w, &mvc->zero_rmv, counts->zero_rmv);

    for (i = 0; i < 2; ++i) {
      nmv_component *comp = &mvc->comps[i];
      nmv_component_counts *comp_counts = &counts->comps[i];

      update_mv(w, comp_counts->sign, &comp->sign, MV_UPDATE_PROB);
      write_mv_update(vp10_mv_class_tree, comp->classes, comp_counts->classes,
                      MV_CLASSES, w);
      write_mv_update(vp10_mv_class0_tree, comp->class0, comp_counts->class0,
                      CLASS0_SIZE, w);
      for (j = 0; j < MV_OFFSET_BITS; ++j)
        update_mv(w, comp_counts->bits[j], &comp->bits[j], MV_UPDATE_PROB);
    }

    for (i = 0; i < 2; ++i) {
      for (j = 0; j < CLASS0_SIZE; ++j)
        write_mv_update(vp10_mv_fp_tree, mvc->comps[i].class0_fp[j],
                        counts->comps[i].class0_fp[j], MV_FP_SIZE, w);

      write_mv_update(vp10_mv_fp_tree, mvc->comps[i].fp, counts->comps[i].fp,
                      MV_FP_SIZE, w);
    }

    if (usehp) {
      for (i = 0; i < 2; ++i) {
        update_mv(w, counts->comps[i].class0_hp, &mvc->comps[i].class0_hp,
                  MV_UPDATE_PROB);
        update_mv(w, counts->comps[i].hp, &mvc->comps[i].hp, MV_UPDATE_PROB);
      }
    }
  }
#else
  nmv_context *const mvc = &cm->fc->nmvc;
  nmv_context_counts *const counts = nmv_counts;

  write_mv_update(vp10_mv_joint_tree, mvc->joints, counts->joints, MV_JOINTS, w);

  for (i = 0; i < 2; ++i) {
    nmv_component *comp = &mvc->comps[i];
    nmv_component_counts *comp_counts = &counts->comps[i];

    update_mv(w, comp_counts->sign, &comp->sign, MV_UPDATE_PROB);
    write_mv_update(vp10_mv_class_tree, comp->classes, comp_counts->classes,
                    MV_CLASSES, w);
    write_mv_update(vp10_mv_class0_tree, comp->class0, comp_counts->class0,
                    CLASS0_SIZE, w);
    for (j = 0; j < MV_OFFSET_BITS; ++j)
      update_mv(w, comp_counts->bits[j], &comp->bits[j], MV_UPDATE_PROB);
  }

  for (i = 0; i < 2; ++i) {
    for (j = 0; j < CLASS0_SIZE; ++j)
      write_mv_update(vp10_mv_fp_tree, mvc->comps[i].class0_fp[j],
                      counts->comps[i].class0_fp[j], MV_FP_SIZE, w);

    write_mv_update(vp10_mv_fp_tree, mvc->comps[i].fp, counts->comps[i].fp,
                    MV_FP_SIZE, w);
  }

  if (usehp) {
    for (i = 0; i < 2; ++i) {
      update_mv(w, counts->comps[i].class0_hp, &mvc->comps[i].class0_hp,
                MV_UPDATE_PROB);
      update_mv(w, counts->comps[i].hp, &mvc->comps[i].hp, MV_UPDATE_PROB);
    }
  }
#endif
}

void vp10_encode_mv(VP10_COMP* cpi, vp10_writer* w,
                   const MV* mv, const MV* ref,
#if CONFIG_REF_MV
                   int is_compound,
#endif
                   const nmv_context* mvctx, int usehp) {
  const MV diff = {mv->row - ref->row,
                   mv->col - ref->col};
  const MV_JOINT_TYPE j = vp10_get_mv_joint(&diff);
  usehp = usehp && vp10_use_mv_hp(ref);

#if CONFIG_REF_MV && !CONFIG_EXT_INTER
  if (is_compound) {
    vpx_write(w, (j == MV_JOINT_ZERO), mvctx->zero_rmv);
    if (j == MV_JOINT_ZERO)
      return;
  } else {
    if (j == MV_JOINT_ZERO)
      assert(0);
  }
#endif

#if CONFIG_REF_MV && CONFIG_EXT_INTER
  (void)is_compound;
#endif

  vp10_write_token(w, vp10_mv_joint_tree, mvctx->joints, &mv_joint_encodings[j]);
  if (mv_joint_vertical(j))
    encode_mv_component(w, diff.row, &mvctx->comps[0], usehp);

  if (mv_joint_horizontal(j))
    encode_mv_component(w, diff.col, &mvctx->comps[1], usehp);

  // If auto_mv_step_size is enabled then keep track of the largest
  // motion vector component used.
  if (cpi->sf.mv.auto_mv_step_size) {
    unsigned int maxv = VPXMAX(abs(mv->row), abs(mv->col)) >> 3;
    cpi->max_mv_magnitude = VPXMAX(maxv, cpi->max_mv_magnitude);
  }
}

void vp10_build_nmv_cost_table(int *mvjoint, int *mvcost[2],
                              const nmv_context* ctx, int usehp) {
  vp10_cost_tokens(mvjoint, ctx->joints, vp10_mv_joint_tree);
  build_nmv_component_cost_table(mvcost[0], &ctx->comps[0], usehp);
  build_nmv_component_cost_table(mvcost[1], &ctx->comps[1], usehp);
}

#if CONFIG_EXT_INTER
static void inc_mvs(const MB_MODE_INFO *mbmi, const MB_MODE_INFO_EXT *mbmi_ext,
                    const int_mv mvs[2],
#if CONFIG_REF_MV
                    const int_mv pred_mvs[2],
#endif
                    nmv_context_counts *nmv_counts) {
  int i;
  PREDICTION_MODE mode = mbmi->mode;
  int mv_idx = (mode == NEWFROMNEARMV);
#if !CONFIG_REF_MV
  nmv_context_counts *counts = nmv_counts;
#endif

  if (mode == NEWMV || mode == NEWFROMNEARMV || mode == NEW_NEWMV) {
    for (i = 0; i < 1 + has_second_ref(mbmi); ++i) {
      const MV *ref = &mbmi_ext->ref_mvs[mbmi->ref_frame[i]][mv_idx].as_mv;
      const MV diff = {mvs[i].as_mv.row - ref->row,
                       mvs[i].as_mv.col - ref->col};
#if CONFIG_REF_MV
      int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[i]],
                                 mbmi_ext->ref_mv_stack[mbmi->ref_frame[i]]);
      nmv_context_counts *counts = &nmv_counts[nmv_ctx];
      (void)pred_mvs;
#endif
      vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
    }
  } else if (mode == NEAREST_NEWMV || mode == NEAR_NEWMV) {
    const MV *ref = &mbmi_ext->ref_mvs[mbmi->ref_frame[1]][0].as_mv;
    const MV diff = {mvs[1].as_mv.row - ref->row,
                     mvs[1].as_mv.col - ref->col};
#if CONFIG_REF_MV
    int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[1]],
                               mbmi_ext->ref_mv_stack[mbmi->ref_frame[1]]);
    nmv_context_counts *counts = &nmv_counts[nmv_ctx];
#endif
    vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
  } else if (mode == NEW_NEARESTMV || mode == NEW_NEARMV) {
    const MV *ref = &mbmi_ext->ref_mvs[mbmi->ref_frame[0]][0].as_mv;
    const MV diff = {mvs[0].as_mv.row - ref->row,
                     mvs[0].as_mv.col - ref->col};
#if CONFIG_REF_MV
    int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[0]],
                               mbmi_ext->ref_mv_stack[mbmi->ref_frame[0]]);
    nmv_context_counts *counts = &nmv_counts[nmv_ctx];
#endif
    vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
  }
}

static void inc_mvs_sub8x8(const MODE_INFO *mi,
                           int block,
                           const int_mv mvs[2],
#if CONFIG_REF_MV
                           const MB_MODE_INFO_EXT *mbmi_ext,
#endif
                           nmv_context_counts *nmv_counts) {
  int i;
  PREDICTION_MODE mode = mi->bmi[block].as_mode;
#if CONFIG_REF_MV
  const MB_MODE_INFO *mbmi = &mi->mbmi;
#else
  nmv_context_counts *counts = nmv_counts;
#endif

  if (mode == NEWMV || mode == NEWFROMNEARMV || mode == NEW_NEWMV) {
    for (i = 0; i < 1 + has_second_ref(&mi->mbmi); ++i) {
      const MV *ref = &mi->bmi[block].ref_mv[i].as_mv;
      const MV diff = {mvs[i].as_mv.row - ref->row,
                       mvs[i].as_mv.col - ref->col};
#if CONFIG_REF_MV
      int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[i]],
                                 mbmi_ext->ref_mv_stack[mbmi->ref_frame[i]]);
      nmv_context_counts *counts = &nmv_counts[nmv_ctx];
#endif
      vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
    }
  } else if (mode == NEAREST_NEWMV || mode == NEAR_NEWMV) {
    const MV *ref = &mi->bmi[block].ref_mv[1].as_mv;
    const MV diff = {mvs[1].as_mv.row - ref->row,
                     mvs[1].as_mv.col - ref->col};
#if CONFIG_REF_MV
    int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[1]],
                               mbmi_ext->ref_mv_stack[mbmi->ref_frame[1]]);
    nmv_context_counts *counts = &nmv_counts[nmv_ctx];
#endif
    vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
  } else if (mode == NEW_NEARESTMV || mode == NEW_NEARMV) {
    const MV *ref = &mi->bmi[block].ref_mv[0].as_mv;
    const MV diff = {mvs[0].as_mv.row - ref->row,
                     mvs[0].as_mv.col - ref->col};
#if CONFIG_REF_MV
    int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[0]],
                               mbmi_ext->ref_mv_stack[mbmi->ref_frame[0]]);
    nmv_context_counts *counts = &nmv_counts[nmv_ctx];
#endif
    vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
  }
}
#else
static void inc_mvs(const MB_MODE_INFO *mbmi, const MB_MODE_INFO_EXT *mbmi_ext,
                    const int_mv mvs[2],
#if CONFIG_REF_MV
                    const int_mv pred_mvs[2],
#endif
                    nmv_context_counts *nmv_counts) {
  int i;
#if !CONFIG_REF_MV
  nmv_context_counts *counts = nmv_counts;
#endif

  for (i = 0; i < 1 + has_second_ref(mbmi); ++i) {
#if CONFIG_REF_MV
    int nmv_ctx = vp10_nmv_ctx(mbmi_ext->ref_mv_count[mbmi->ref_frame[i]],
                               mbmi_ext->ref_mv_stack[mbmi->ref_frame[i]]);
    nmv_context_counts *counts = &nmv_counts[nmv_ctx];
    const MV *ref = &pred_mvs[i].as_mv;
#else
    const MV *ref = &mbmi_ext->ref_mvs[mbmi->ref_frame[i]][0].as_mv;
#endif
    const MV diff = {mvs[i].as_mv.row - ref->row,
                     mvs[i].as_mv.col - ref->col};
    vp10_inc_mv(&diff, counts, vp10_use_mv_hp(ref));
  }
}
#endif  // CONFIG_EXT_INTER

void vp10_update_mv_count(ThreadData *td) {
  const MACROBLOCKD *xd = &td->mb.e_mbd;
  const MODE_INFO *mi = xd->mi[0];
  const MB_MODE_INFO *const mbmi = &mi->mbmi;
  const MB_MODE_INFO_EXT *mbmi_ext = td->mb.mbmi_ext;

  if (mbmi->sb_type < BLOCK_8X8) {
    const int num_4x4_w = num_4x4_blocks_wide_lookup[mbmi->sb_type];
    const int num_4x4_h = num_4x4_blocks_high_lookup[mbmi->sb_type];
    int idx, idy;

    for (idy = 0; idy < 2; idy += num_4x4_h) {
      for (idx = 0; idx < 2; idx += num_4x4_w) {
        const int i = idy * 2 + idx;

#if CONFIG_EXT_INTER
        if (have_newmv_in_inter_mode(mi->bmi[i].as_mode))
          inc_mvs_sub8x8(mi, i, mi->bmi[i].as_mv,
#if CONFIG_REF_MV
                         mbmi_ext,
                         td->counts->mv);
#else
                         &td->counts->mv);
#endif
#else
        if (mi->bmi[i].as_mode == NEWMV)
          inc_mvs(mbmi, mbmi_ext, mi->bmi[i].as_mv,
#if CONFIG_REF_MV
                  mi->bmi[i].pred_mv_s8,
                  td->counts->mv);
#else
                  &td->counts->mv);
#endif
#endif  // CONFIG_EXT_INTER
      }
    }
  } else {
#if CONFIG_EXT_INTER
    if (have_newmv_in_inter_mode(mbmi->mode))
#else
    if (mbmi->mode == NEWMV)
#endif  // CONFIG_EXT_INTER
      inc_mvs(mbmi, mbmi_ext, mbmi->mv,
#if CONFIG_REF_MV
              mbmi->pred_mv,
              td->counts->mv);
#else
              &td->counts->mv);
#endif
  }
}

