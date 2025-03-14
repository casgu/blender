/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2017, Blender Foundation.
 */

/** \file
 * \ingroup modifiers
 */

#include "BLI_alloca.h"
#include "BLI_math.h"
#include "BLI_math_geom.h"
#include "BLI_task.h"

#include "BLT_translation.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_bvhutils.h"
#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_editmesh.h"
#include "BKE_lib_id.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_wrapper.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "BLO_read_write.h"

#include "RNA_access.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "MEM_guardedalloc.h"

#include "MOD_ui_common.h"
#include "MOD_util.h"

typedef struct SDefAdjacency {
  struct SDefAdjacency *next;
  uint index;
} SDefAdjacency;

typedef struct SDefAdjacencyArray {
  SDefAdjacency *first;
  uint num; /* Careful, this is twice the number of polygons (avoids an extra loop) */
} SDefAdjacencyArray;

/**
 * Polygons per edge (only 2, any more will exit calculation).
 */
typedef struct SDefEdgePolys {
  uint polys[2], num;
} SDefEdgePolys;

typedef struct SDefBindCalcData {
  BVHTreeFromMesh *const treeData;
  const SDefAdjacencyArray *const vert_edges;
  const SDefEdgePolys *const edge_polys;
  SDefVert *const bind_verts;
  const MLoopTri *const looptri;
  const MPoly *const mpoly;
  const MEdge *const medge;
  const MLoop *const mloop;
  /** Coordinates to bind to, transformed into local space (compatible with `vertexCos`). */
  float (*const targetCos)[3];
  /** Coordinates to bind (reference to the modifiers input argument). */
  float (*const vertexCos)[3];
  float imat[4][4];
  const float falloff;
  int success;
  /** Vertex group lookup data. */
  const MDeformVert *const dvert;
  int const defgrp_index;
  bool const invert_vgroup;
  bool const sparse_bind;
} SDefBindCalcData;

/**
 * This represents the relationship between a point (a source coordinate)
 * and the face-corner it's being bound to (from the target mesh).
 *
 * \note Some of these values could be de-duplicated however these are only
 * needed once when running bind, so optimizing this structure isn't a priority.
 */
typedef struct SDefBindPoly {
  /** Coordinates copied directly from the modifiers input. */
  float (*coords)[3];
  /** Coordinates projected into 2D space using `normal`. */
  float (*coords_v2)[2];
  /** The point being queried projected into 2D space using `normal`. */
  float point_v2[2];
  float weight_angular;
  float weight_dist_proj;
  float weight_dist;
  float weight;
  /** Distances from the centroid to edges flanking the corner vertex, used to penalize
   *  small or long and narrow faces in favor of bigger and more square ones. */
  float scales[2];
  /** Distance weight from the corner vertex to the chord line, used to penalize
   *  cases with the three consecutive vertices being nearly in line. */
  float scale_mid;
  /** Center of `coords` */
  float centroid[3];
  /** Center of `coords_v2` */
  float centroid_v2[2];
  /**
   * The calculated normal of coords (could be shared between faces).
   */
  float normal[3];
  /** Vectors pointing from the centroid to the midpoints of the two edges
   *  flanking the corner vertex. */
  float cent_edgemid_vecs_v2[2][2];
  /** Angle between the cent_edgemid_vecs_v2 vectors. */
  float edgemid_angle;
  /** Angles between the centroid-to-point and cent_edgemid_vecs_v2 vectors.
   *  Positive values measured towards the corner; clamped non-negative. */
  float point_edgemid_angles[2];
  /** Angles between the centroid-to-corner and cent_edgemid_vecs_v2 vectors. */
  float corner_edgemid_angles[2];
  /** Weight of the bind mode based on the corner and two adjacent vertices,
   *  versus the one based on the centroid and the dominant edge. */
  float dominant_angle_weight;
  /** Index of the input polygon. */
  uint index;
  /** Number of vertices in this face. */
  uint numverts;
  /**
   * This polygons loop-start.
   * \note that we could look this up from the polygon.
   */
  uint loopstart;
  uint edge_inds[2];
  uint edge_vert_inds[2];
  /** The index of this corner in the face (starting at zero). */
  uint corner_ind;
  uint dominant_edge;
  /** When true `point_v2` is inside `coords_v2`. */
  bool inside;
} SDefBindPoly;

typedef struct SDefBindWeightData {
  SDefBindPoly *bind_polys;
  uint numpoly;
  uint numbinds;
} SDefBindWeightData;

typedef struct SDefDeformData {
  const SDefVert *const bind_verts;
  float (*const targetCos)[3];
  float (*const vertexCos)[3];
  const MDeformVert *const dvert;
  int const defgrp_index;
  bool const invert_vgroup;
  float const strength;
} SDefDeformData;

/* Bind result values */
enum {
  MOD_SDEF_BIND_RESULT_SUCCESS = 1,
  MOD_SDEF_BIND_RESULT_GENERIC_ERR = 0,
  MOD_SDEF_BIND_RESULT_MEM_ERR = -1,
  MOD_SDEF_BIND_RESULT_NONMANY_ERR = -2,
  MOD_SDEF_BIND_RESULT_CONCAVE_ERR = -3,
  MOD_SDEF_BIND_RESULT_OVERLAP_ERR = -4,
};

/* Infinite weight flags */
enum {
  MOD_SDEF_INFINITE_WEIGHT_ANGULAR = (1 << 0),
  MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ = (1 << 1),
  MOD_SDEF_INFINITE_WEIGHT_DIST = (1 << 2),
};

static void initData(ModifierData *md)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(smd, modifier));

  MEMCPY_STRUCT_AFTER(smd, DNA_struct_default_get(SurfaceDeformModifierData), modifier);
}

static void requiredDataMask(Object *UNUSED(ob),
                             ModifierData *md,
                             CustomData_MeshMasks *r_cddata_masks)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  /* Ask for vertex groups if we need them. */
  if (smd->defgrp_name[0] != '\0') {
    r_cddata_masks->vmask |= CD_MASK_MDEFORMVERT;
  }
}

static void freeData(ModifierData *md)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  if (smd->verts) {
    for (int i = 0; i < smd->num_bind_verts; i++) {
      if (smd->verts[i].binds) {
        for (int j = 0; j < smd->verts[i].numbinds; j++) {
          MEM_SAFE_FREE(smd->verts[i].binds[j].vert_inds);
          MEM_SAFE_FREE(smd->verts[i].binds[j].vert_weights);
        }

        MEM_SAFE_FREE(smd->verts[i].binds);
      }
    }

    MEM_SAFE_FREE(smd->verts);
  }
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
  const SurfaceDeformModifierData *smd = (const SurfaceDeformModifierData *)md;
  SurfaceDeformModifierData *tsmd = (SurfaceDeformModifierData *)target;

  BKE_modifier_copydata_generic(md, target, flag);

  if (smd->verts) {
    tsmd->verts = MEM_dupallocN(smd->verts);

    for (int i = 0; i < smd->num_bind_verts; i++) {
      if (smd->verts[i].binds) {
        tsmd->verts[i].binds = MEM_dupallocN(smd->verts[i].binds);

        for (int j = 0; j < smd->verts[i].numbinds; j++) {
          if (smd->verts[i].binds[j].vert_inds) {
            tsmd->verts[i].binds[j].vert_inds = MEM_dupallocN(smd->verts[i].binds[j].vert_inds);
          }

          if (smd->verts[i].binds[j].vert_weights) {
            tsmd->verts[i].binds[j].vert_weights = MEM_dupallocN(
                smd->verts[i].binds[j].vert_weights);
          }
        }
      }
    }
  }
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  walk(userData, ob, (ID **)&smd->target, IDWALK_NOP);
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;
  if (smd->target != NULL) {
    DEG_add_object_relation(
        ctx->node, smd->target, DEG_OB_COMP_GEOMETRY, "Surface Deform Modifier");
  }
}

static void freeAdjacencyMap(SDefAdjacencyArray *const vert_edges,
                             SDefAdjacency *const adj_ref,
                             SDefEdgePolys *const edge_polys)
{
  MEM_freeN(edge_polys);

  MEM_freeN(adj_ref);

  MEM_freeN(vert_edges);
}

static int buildAdjacencyMap(const MPoly *poly,
                             const MEdge *edge,
                             const MLoop *const mloop,
                             const uint numpoly,
                             const uint numedges,
                             SDefAdjacencyArray *const vert_edges,
                             SDefAdjacency *adj,
                             SDefEdgePolys *const edge_polys)
{
  const MLoop *loop;

  /* Find polygons adjacent to edges. */
  for (int i = 0; i < numpoly; i++, poly++) {
    loop = &mloop[poly->loopstart];

    for (int j = 0; j < poly->totloop; j++, loop++) {
      if (edge_polys[loop->e].num == 0) {
        edge_polys[loop->e].polys[0] = i;
        edge_polys[loop->e].polys[1] = -1;
        edge_polys[loop->e].num++;
      }
      else if (edge_polys[loop->e].num == 1) {
        edge_polys[loop->e].polys[1] = i;
        edge_polys[loop->e].num++;
      }
      else {
        return MOD_SDEF_BIND_RESULT_NONMANY_ERR;
      }
    }
  }

  /* Find edges adjacent to vertices */
  for (int i = 0; i < numedges; i++, edge++) {
    adj->next = vert_edges[edge->v1].first;
    adj->index = i;
    vert_edges[edge->v1].first = adj;
    vert_edges[edge->v1].num += edge_polys[i].num;
    adj++;

    adj->next = vert_edges[edge->v2].first;
    adj->index = i;
    vert_edges[edge->v2].first = adj;
    vert_edges[edge->v2].num += edge_polys[i].num;
    adj++;
  }

  return MOD_SDEF_BIND_RESULT_SUCCESS;
}

BLI_INLINE void sortPolyVertsEdge(uint *indices,
                                  const MLoop *const mloop,
                                  const uint edge,
                                  const uint num)
{
  bool found = false;

  for (int i = 0; i < num; i++) {
    if (mloop[i].e == edge) {
      found = true;
    }
    if (found) {
      *indices = mloop[i].v;
      indices++;
    }
  }

  /* Fill in remaining vertex indices that occur before the edge */
  for (int i = 0; mloop[i].e != edge; i++) {
    *indices = mloop[i].v;
    indices++;
  }
}

BLI_INLINE void sortPolyVertsTri(uint *indices,
                                 const MLoop *const mloop,
                                 const uint loopstart,
                                 const uint num)
{
  for (int i = loopstart; i < num; i++) {
    *indices = mloop[i].v;
    indices++;
  }

  for (int i = 0; i < loopstart; i++) {
    *indices = mloop[i].v;
    indices++;
  }
}

BLI_INLINE uint nearestVert(SDefBindCalcData *const data, const float point_co[3])
{
  BVHTreeNearest nearest = {
      .dist_sq = FLT_MAX,
      .index = -1,
  };
  const MPoly *poly;
  const MEdge *edge;
  const MLoop *loop;
  float t_point[3];
  float max_dist = FLT_MAX;
  float dist;
  uint index = 0;

  mul_v3_m4v3(t_point, data->imat, point_co);

  BLI_bvhtree_find_nearest(
      data->treeData->tree, t_point, &nearest, data->treeData->nearest_callback, data->treeData);

  poly = &data->mpoly[data->looptri[nearest.index].poly];
  loop = &data->mloop[poly->loopstart];

  for (int i = 0; i < poly->totloop; i++, loop++) {
    edge = &data->medge[loop->e];
    dist = dist_squared_to_line_segment_v3(
        point_co, data->targetCos[edge->v1], data->targetCos[edge->v2]);

    if (dist < max_dist) {
      max_dist = dist;
      index = loop->e;
    }
  }

  edge = &data->medge[index];
  if (len_squared_v3v3(point_co, data->targetCos[edge->v1]) <
      len_squared_v3v3(point_co, data->targetCos[edge->v2])) {
    return edge->v1;
  }

  return edge->v2;
}

BLI_INLINE int isPolyValid(const float coords[][2], const uint nr)
{
  float prev_co[2], prev_prev_co[2];
  float curr_vec[2], prev_vec[2];

  if (!is_poly_convex_v2(coords, nr)) {
    return MOD_SDEF_BIND_RESULT_CONCAVE_ERR;
  }

  copy_v2_v2(prev_prev_co, coords[nr - 2]);
  copy_v2_v2(prev_co, coords[nr - 1]);
  sub_v2_v2v2(prev_vec, prev_co, coords[nr - 2]);
  normalize_v2(prev_vec);

  for (int i = 0; i < nr; i++) {
    sub_v2_v2v2(curr_vec, coords[i], prev_co);

    /* Check overlap between directly adjacent vertices. */
    const float curr_len = normalize_v2(curr_vec);
    if (curr_len < FLT_EPSILON) {
      return MOD_SDEF_BIND_RESULT_OVERLAP_ERR;
    }

    /* Check overlap between vertices skipping one. */
    if (len_squared_v2v2(prev_prev_co, coords[i]) < FLT_EPSILON * FLT_EPSILON) {
      return MOD_SDEF_BIND_RESULT_OVERLAP_ERR;
    }

    /* Check for adjacent parallel edges. */
    if (1.0f - dot_v2v2(prev_vec, curr_vec) < FLT_EPSILON) {
      return MOD_SDEF_BIND_RESULT_CONCAVE_ERR;
    }

    copy_v2_v2(prev_prev_co, prev_co);
    copy_v2_v2(prev_co, coords[i]);
    copy_v2_v2(prev_vec, curr_vec);
  }

  return MOD_SDEF_BIND_RESULT_SUCCESS;
}

static void freeBindData(SDefBindWeightData *const bwdata)
{
  SDefBindPoly *bpoly = bwdata->bind_polys;

  if (bwdata->bind_polys) {
    for (int i = 0; i < bwdata->numpoly; bpoly++, i++) {
      MEM_SAFE_FREE(bpoly->coords);
      MEM_SAFE_FREE(bpoly->coords_v2);
    }

    MEM_freeN(bwdata->bind_polys);
  }

  MEM_freeN(bwdata);
}

BLI_INLINE float computeAngularWeight(const float point_angle, const float edgemid_angle)
{
  return sinf(min_ff(point_angle / edgemid_angle, 1) * M_PI_2);
}

BLI_INLINE SDefBindWeightData *computeBindWeights(SDefBindCalcData *const data,
                                                  const float point_co[3])
{
  const uint nearest = nearestVert(data, point_co);
  const SDefAdjacency *const vert_edges = data->vert_edges[nearest].first;
  const SDefEdgePolys *const edge_polys = data->edge_polys;

  const SDefAdjacency *vedge;
  const MPoly *poly;
  const MLoop *loop;

  SDefBindWeightData *bwdata;
  SDefBindPoly *bpoly;

  const float world[3] = {0.0f, 0.0f, 1.0f};
  float avg_point_dist = 0.0f;
  float tot_weight = 0.0f;
  int inf_weight_flags = 0;

  bwdata = MEM_callocN(sizeof(*bwdata), "SDefBindWeightData");
  if (bwdata == NULL) {
    data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
    return NULL;
  }

  bwdata->numpoly = data->vert_edges[nearest].num / 2;

  bpoly = MEM_calloc_arrayN(bwdata->numpoly, sizeof(*bpoly), "SDefBindPoly");
  if (bpoly == NULL) {
    freeBindData(bwdata);
    data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
    return NULL;
  }

  bwdata->bind_polys = bpoly;

  /* Loop over all adjacent edges,
   * and build the #SDefBindPoly data for each poly adjacent to those. */
  for (vedge = vert_edges; vedge; vedge = vedge->next) {
    uint edge_ind = vedge->index;

    for (int i = 0; i < edge_polys[edge_ind].num; i++) {
      {
        bpoly = bwdata->bind_polys;

        for (int j = 0; j < bwdata->numpoly; bpoly++, j++) {
          /* If coords isn't allocated, we have reached the first uninitialized `bpoly`. */
          if ((bpoly->index == edge_polys[edge_ind].polys[i]) || (!bpoly->coords)) {
            break;
          }
        }
      }

      /* Check if poly was already created by another edge or still has to be initialized */
      if (!bpoly->coords) {
        float angle;
        float axis[3];
        float tmp_vec_v2[2];
        int is_poly_valid;

        bpoly->index = edge_polys[edge_ind].polys[i];
        bpoly->coords = NULL;
        bpoly->coords_v2 = NULL;

        /* Copy poly data */
        poly = &data->mpoly[bpoly->index];
        loop = &data->mloop[poly->loopstart];

        bpoly->numverts = poly->totloop;
        bpoly->loopstart = poly->loopstart;

        bpoly->coords = MEM_malloc_arrayN(
            poly->totloop, sizeof(*bpoly->coords), "SDefBindPolyCoords");
        if (bpoly->coords == NULL) {
          freeBindData(bwdata);
          data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
          return NULL;
        }

        bpoly->coords_v2 = MEM_malloc_arrayN(
            poly->totloop, sizeof(*bpoly->coords_v2), "SDefBindPolyCoords_v2");
        if (bpoly->coords_v2 == NULL) {
          freeBindData(bwdata);
          data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
          return NULL;
        }

        for (int j = 0; j < poly->totloop; j++, loop++) {
          copy_v3_v3(bpoly->coords[j], data->targetCos[loop->v]);

          /* Find corner and edge indices within poly loop array */
          if (loop->v == nearest) {
            bpoly->corner_ind = j;
            bpoly->edge_vert_inds[0] = (j == 0) ? (poly->totloop - 1) : (j - 1);
            bpoly->edge_vert_inds[1] = (j == poly->totloop - 1) ? (0) : (j + 1);

            bpoly->edge_inds[0] = data->mloop[poly->loopstart + bpoly->edge_vert_inds[0]].e;
            bpoly->edge_inds[1] = loop->e;
          }
        }

        /* Compute polygons parametric data. */
        mid_v3_v3_array(bpoly->centroid, bpoly->coords, poly->totloop);
        normal_poly_v3(bpoly->normal, bpoly->coords, poly->totloop);

        /* Compute poly skew angle and axis */
        angle = angle_normalized_v3v3(bpoly->normal, world);

        cross_v3_v3v3(axis, bpoly->normal, world);
        normalize_v3(axis);

        /* Map coords onto 2d normal plane. */
        map_to_plane_axis_angle_v2_v3v3fl(bpoly->point_v2, point_co, axis, angle);

        zero_v2(bpoly->centroid_v2);
        for (int j = 0; j < poly->totloop; j++) {
          map_to_plane_axis_angle_v2_v3v3fl(bpoly->coords_v2[j], bpoly->coords[j], axis, angle);
          madd_v2_v2fl(bpoly->centroid_v2, bpoly->coords_v2[j], 1.0f / poly->totloop);
        }

        is_poly_valid = isPolyValid(bpoly->coords_v2, poly->totloop);

        if (is_poly_valid != MOD_SDEF_BIND_RESULT_SUCCESS) {
          freeBindData(bwdata);
          data->success = is_poly_valid;
          return NULL;
        }

        bpoly->inside = isect_point_poly_v2(
            bpoly->point_v2, bpoly->coords_v2, poly->totloop, false);

        /* Initialize weight components */
        bpoly->weight_angular = 1.0f;
        bpoly->weight_dist_proj = len_v2v2(bpoly->centroid_v2, bpoly->point_v2);
        bpoly->weight_dist = len_v3v3(bpoly->centroid, point_co);

        avg_point_dist += bpoly->weight_dist;

        /* Common vertex coordinates. */
        const float *const vert0_v2 = bpoly->coords_v2[bpoly->edge_vert_inds[0]];
        const float *const vert1_v2 = bpoly->coords_v2[bpoly->edge_vert_inds[1]];
        const float *const corner_v2 = bpoly->coords_v2[bpoly->corner_ind];

        /* Compute centroid to mid-edge vectors */
        mid_v2_v2v2(bpoly->cent_edgemid_vecs_v2[0], vert0_v2, corner_v2);
        mid_v2_v2v2(bpoly->cent_edgemid_vecs_v2[1], vert1_v2, corner_v2);

        sub_v2_v2(bpoly->cent_edgemid_vecs_v2[0], bpoly->centroid_v2);
        sub_v2_v2(bpoly->cent_edgemid_vecs_v2[1], bpoly->centroid_v2);

        normalize_v2(bpoly->cent_edgemid_vecs_v2[0]);
        normalize_v2(bpoly->cent_edgemid_vecs_v2[1]);

        /* Compute poly scales with respect to the two edges. */
        bpoly->scales[0] = dist_to_line_v2(bpoly->centroid_v2, vert0_v2, corner_v2);
        bpoly->scales[1] = dist_to_line_v2(bpoly->centroid_v2, vert1_v2, corner_v2);

        /* Compute the angle between the edge mid vectors. */
        bpoly->edgemid_angle = angle_normalized_v2v2(bpoly->cent_edgemid_vecs_v2[0],
                                                     bpoly->cent_edgemid_vecs_v2[1]);

        /* Compute the angles between the corner and the edge mid vectors. The angles
         * are computed signed in order to correctly clamp point_edgemid_angles later. */
        float corner_angles[2];

        sub_v2_v2v2(tmp_vec_v2, corner_v2, bpoly->centroid_v2);
        normalize_v2(tmp_vec_v2);

        corner_angles[0] = angle_signed_v2v2(tmp_vec_v2, bpoly->cent_edgemid_vecs_v2[0]);
        corner_angles[1] = angle_signed_v2v2(tmp_vec_v2, bpoly->cent_edgemid_vecs_v2[1]);

        bpoly->corner_edgemid_angles[0] = fabsf(corner_angles[0]);
        bpoly->corner_edgemid_angles[1] = fabsf(corner_angles[1]);

        /* Verify that the computed values are valid (the polygon isn't somehow
         * degenerate despite having passed isPolyValid). */
        if (bpoly->scales[0] < FLT_EPSILON || bpoly->scales[1] < FLT_EPSILON ||
            bpoly->edgemid_angle < FLT_EPSILON || bpoly->corner_edgemid_angles[0] < FLT_EPSILON ||
            bpoly->corner_edgemid_angles[1] < FLT_EPSILON) {
          freeBindData(bwdata);
          data->success = MOD_SDEF_BIND_RESULT_GENERIC_ERR;
          return NULL;
        }

        /* Check for infinite weights, and compute angular data otherwise. */
        if (bpoly->weight_dist < FLT_EPSILON) {
          inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ;
          inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST;
        }
        else if (bpoly->weight_dist_proj < FLT_EPSILON) {
          inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ;
        }
        else {
          /* Compute angles between the point and the edge mid vectors. */
          float cent_point_vec[2], point_angles[2];

          sub_v2_v2v2(cent_point_vec, bpoly->point_v2, bpoly->centroid_v2);
          normalize_v2(cent_point_vec);

          point_angles[0] = angle_signed_v2v2(cent_point_vec, bpoly->cent_edgemid_vecs_v2[0]) *
                            signf(corner_angles[0]);
          point_angles[1] = angle_signed_v2v2(cent_point_vec, bpoly->cent_edgemid_vecs_v2[1]) *
                            signf(corner_angles[1]);

          if (point_angles[0] <= 0 && point_angles[1] <= 0) {
            /* If the point is outside the corner formed by the edge mid vectors,
             * choose to clamp the closest side and flip the other. */
            if (point_angles[0] < point_angles[1]) {
              point_angles[0] = bpoly->edgemid_angle - point_angles[1];
            }
            else {
              point_angles[1] = bpoly->edgemid_angle - point_angles[0];
            }
          }

          bpoly->point_edgemid_angles[0] = max_ff(0, point_angles[0]);
          bpoly->point_edgemid_angles[1] = max_ff(0, point_angles[1]);

          /* Compute the distance scale for the corner. The base value is the orthogonal
           * distance from the corner to the chord, scaled by sqrt(2) to preserve the old
           * values in case of a square grid. This doesn't use the centroid because the
           * LOOPTRI method only uses these three vertices. */
          bpoly->scale_mid = area_tri_v2(vert0_v2, corner_v2, vert1_v2) /
                             len_v2v2(vert0_v2, vert1_v2) * sqrtf(2);

          if (bpoly->inside) {
            /* When inside, interpolate to centroid-based scale close to the center. */
            float min_dist = min_ff(bpoly->scales[0], bpoly->scales[1]);

            bpoly->scale_mid = interpf(bpoly->scale_mid,
                                       (bpoly->scales[0] + bpoly->scales[1]) / 2,
                                       min_ff(bpoly->weight_dist_proj / min_dist, 1));
          }

          /* Verify that the additional computed values are valid. */
          if (bpoly->scale_mid < FLT_EPSILON ||
              bpoly->point_edgemid_angles[0] + bpoly->point_edgemid_angles[1] < FLT_EPSILON) {
            freeBindData(bwdata);
            data->success = MOD_SDEF_BIND_RESULT_GENERIC_ERR;
            return NULL;
          }
        }
      }
    }
  }

  avg_point_dist /= bwdata->numpoly;

  /* If weights 1 and 2 are not infinite, loop over all adjacent edges again,
   * and build adjacency dependent angle data (depends on all polygons having been computed) */
  if (!inf_weight_flags) {
    for (vedge = vert_edges; vedge; vedge = vedge->next) {
      SDefBindPoly *bpolys[2];
      const SDefEdgePolys *epolys;
      float ang_weights[2];
      uint edge_ind = vedge->index;
      uint edge_on_poly[2];

      epolys = &edge_polys[edge_ind];

      /* Find bind polys corresponding to the edge's adjacent polys */
      bpoly = bwdata->bind_polys;

      for (int i = 0, j = 0; (i < bwdata->numpoly) && (j < epolys->num); bpoly++, i++) {
        if (ELEM(bpoly->index, epolys->polys[0], epolys->polys[1])) {
          bpolys[j] = bpoly;

          if (bpoly->edge_inds[0] == edge_ind) {
            edge_on_poly[j] = 0;
          }
          else {
            edge_on_poly[j] = 1;
          }

          j++;
        }
      }

      /* Compute angular weight component */
      if (epolys->num == 1) {
        ang_weights[0] = computeAngularWeight(bpolys[0]->point_edgemid_angles[edge_on_poly[0]],
                                              bpolys[0]->edgemid_angle);
        bpolys[0]->weight_angular *= ang_weights[0] * ang_weights[0];
      }
      else if (epolys->num == 2) {
        ang_weights[0] = computeAngularWeight(bpolys[0]->point_edgemid_angles[edge_on_poly[0]],
                                              bpolys[0]->edgemid_angle);
        ang_weights[1] = computeAngularWeight(bpolys[1]->point_edgemid_angles[edge_on_poly[1]],
                                              bpolys[1]->edgemid_angle);

        bpolys[0]->weight_angular *= ang_weights[0] * ang_weights[1];
        bpolys[1]->weight_angular *= ang_weights[0] * ang_weights[1];
      }
    }
  }

  /* Compute scaling and falloff:
   * - Scale all weights if no infinite weight is found.
   * - Scale only un-projected weight if projected weight is infinite.
   * - Scale none if both are infinite. */
  if (!inf_weight_flags) {
    bpoly = bwdata->bind_polys;

    for (int i = 0; i < bwdata->numpoly; bpoly++, i++) {
      float corner_angle_weights[2];
      float scale_weight, sqr, inv_sqr;

      corner_angle_weights[0] = bpoly->point_edgemid_angles[0] / bpoly->corner_edgemid_angles[0];
      corner_angle_weights[1] = bpoly->point_edgemid_angles[1] / bpoly->corner_edgemid_angles[1];

      if (isnan(corner_angle_weights[0]) || isnan(corner_angle_weights[1])) {
        freeBindData(bwdata);
        data->success = MOD_SDEF_BIND_RESULT_GENERIC_ERR;
        return NULL;
      }

      /* Find which edge the point is closer to */
      if (corner_angle_weights[0] < corner_angle_weights[1]) {
        bpoly->dominant_edge = 0;
        bpoly->dominant_angle_weight = corner_angle_weights[0];
      }
      else {
        bpoly->dominant_edge = 1;
        bpoly->dominant_angle_weight = corner_angle_weights[1];
      }

      /* Check for invalid weights just in case computations fail. */
      if (bpoly->dominant_angle_weight < 0 || bpoly->dominant_angle_weight > 1) {
        freeBindData(bwdata);
        data->success = MOD_SDEF_BIND_RESULT_GENERIC_ERR;
        return NULL;
      }

      bpoly->dominant_angle_weight = sinf(bpoly->dominant_angle_weight * M_PI_2);

      /* Compute quadratic angular scale interpolation weight */
      {
        const float edge_angle_a = bpoly->point_edgemid_angles[bpoly->dominant_edge];
        const float edge_angle_b = bpoly->point_edgemid_angles[!bpoly->dominant_edge];
        /* Clamp so skinny faces with near zero `edgemid_angle`
         * won't cause numeric problems. see T81988. */
        scale_weight = edge_angle_a / max_ff(edge_angle_a, bpoly->edgemid_angle);
        scale_weight /= scale_weight + (edge_angle_b / max_ff(edge_angle_b, bpoly->edgemid_angle));
      }

      sqr = scale_weight * scale_weight;
      inv_sqr = 1.0f - scale_weight;
      inv_sqr *= inv_sqr;
      scale_weight = sqr / (sqr + inv_sqr);

      BLI_assert(scale_weight >= 0 && scale_weight <= 1);

      /* Compute interpolated scale (no longer need the individual scales,
       * so simply storing the result over the scale in index zero) */
      bpoly->scales[0] = interpf(bpoly->scale_mid,
                                 interpf(bpoly->scales[!bpoly->dominant_edge],
                                         bpoly->scales[bpoly->dominant_edge],
                                         scale_weight),
                                 bpoly->dominant_angle_weight);

      /* Scale the point distance weights, and introduce falloff */
      bpoly->weight_dist_proj /= bpoly->scales[0];
      bpoly->weight_dist_proj = powf(bpoly->weight_dist_proj, data->falloff);

      bpoly->weight_dist /= avg_point_dist;
      bpoly->weight_dist = powf(bpoly->weight_dist, data->falloff);

      /* Re-check for infinite weights, now that all scalings and interpolations are computed */
      if (bpoly->weight_dist < FLT_EPSILON) {
        inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ;
        inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST;
      }
      else if (bpoly->weight_dist_proj < FLT_EPSILON) {
        inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ;
      }
      else if (bpoly->weight_angular < FLT_EPSILON) {
        inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_ANGULAR;
      }
    }
  }
  else if (!(inf_weight_flags & MOD_SDEF_INFINITE_WEIGHT_DIST)) {
    bpoly = bwdata->bind_polys;

    for (int i = 0; i < bwdata->numpoly; bpoly++, i++) {
      /* Scale the point distance weight by average point distance, and introduce falloff */
      bpoly->weight_dist /= avg_point_dist;
      bpoly->weight_dist = powf(bpoly->weight_dist, data->falloff);

      /* Re-check for infinite weights, now that all scalings and interpolations are computed */
      if (bpoly->weight_dist < FLT_EPSILON) {
        inf_weight_flags |= MOD_SDEF_INFINITE_WEIGHT_DIST;
      }
    }
  }

  /* Final loop, to compute actual weights */
  bpoly = bwdata->bind_polys;

  for (int i = 0; i < bwdata->numpoly; bpoly++, i++) {
    /* Weight computation from components */
    if (inf_weight_flags & MOD_SDEF_INFINITE_WEIGHT_DIST) {
      bpoly->weight = bpoly->weight_dist < FLT_EPSILON ? 1.0f : 0.0f;
    }
    else if (inf_weight_flags & MOD_SDEF_INFINITE_WEIGHT_DIST_PROJ) {
      bpoly->weight = bpoly->weight_dist_proj < FLT_EPSILON ? 1.0f / bpoly->weight_dist : 0.0f;
    }
    else if (inf_weight_flags & MOD_SDEF_INFINITE_WEIGHT_ANGULAR) {
      bpoly->weight = bpoly->weight_angular < FLT_EPSILON ?
                          1.0f / bpoly->weight_dist_proj / bpoly->weight_dist :
                          0.0f;
    }
    else {
      bpoly->weight = 1.0f / bpoly->weight_angular / bpoly->weight_dist_proj / bpoly->weight_dist;
    }

    /* Apply after other kinds of scaling so the faces corner angle is always
     * scaled in a uniform way, preventing heavily sub-divided triangle fans
     * from having a lop-sided influence on the weighting, see T81988. */
    bpoly->weight *= bpoly->edgemid_angle / M_PI;

    tot_weight += bpoly->weight;
  }

  bpoly = bwdata->bind_polys;

  for (int i = 0; i < bwdata->numpoly; bpoly++, i++) {
    bpoly->weight /= tot_weight;

    /* Evaluate if this poly is relevant to bind */
    /* Even though the weights should add up to 1.0,
     * the losses of weights smaller than epsilon here
     * should be negligible... */
    if (bpoly->weight >= FLT_EPSILON) {
      if (bpoly->inside) {
        bwdata->numbinds += 1;
      }
      else {
        if (bpoly->dominant_angle_weight < FLT_EPSILON ||
            1.0f - bpoly->dominant_angle_weight < FLT_EPSILON) {
          bwdata->numbinds += 1;
        }
        else {
          bwdata->numbinds += 2;
        }
      }
    }
  }

  return bwdata;
}

BLI_INLINE float computeNormalDisplacement(const float point_co[3],
                                           const float point_co_proj[3],
                                           const float normal[3])
{
  float disp_vec[3];
  float normal_dist;

  sub_v3_v3v3(disp_vec, point_co, point_co_proj);
  normal_dist = len_v3(disp_vec);

  if (dot_v3v3(disp_vec, normal) < 0) {
    normal_dist *= -1;
  }

  return normal_dist;
}

static void bindVert(void *__restrict userdata,
                     const int index,
                     const TaskParallelTLS *__restrict UNUSED(tls))
{
  SDefBindCalcData *const data = (SDefBindCalcData *)userdata;
  float point_co[3];
  float point_co_proj[3];

  SDefBindWeightData *bwdata;
  SDefVert *sdvert = data->bind_verts + index;
  SDefBindPoly *bpoly;
  SDefBind *sdbind;

  sdvert->vertex_idx = index;

  if (data->success != MOD_SDEF_BIND_RESULT_SUCCESS) {
    sdvert->binds = NULL;
    sdvert->numbinds = 0;
    return;
  }

  if (data->sparse_bind) {
    float weight = 0.0f;

    if (data->dvert && data->defgrp_index != -1) {
      weight = BKE_defvert_find_weight(&data->dvert[index], data->defgrp_index);
    }

    if (data->invert_vgroup) {
      weight = 1.0f - weight;
    }

    if (weight <= 0) {
      sdvert->binds = NULL;
      sdvert->numbinds = 0;
      return;
    }
  }

  copy_v3_v3(point_co, data->vertexCos[index]);
  bwdata = computeBindWeights(data, point_co);

  if (bwdata == NULL) {
    sdvert->binds = NULL;
    sdvert->numbinds = 0;
    return;
  }

  sdvert->binds = MEM_calloc_arrayN(bwdata->numbinds, sizeof(*sdvert->binds), "SDefVertBindData");
  if (sdvert->binds == NULL) {
    data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
    sdvert->numbinds = 0;
    return;
  }

  sdvert->numbinds = bwdata->numbinds;

  sdbind = sdvert->binds;

  bpoly = bwdata->bind_polys;

  for (int i = 0; i < bwdata->numbinds; bpoly++) {
    if (bpoly->weight >= FLT_EPSILON) {
      if (bpoly->inside) {
        const MLoop *loop = &data->mloop[bpoly->loopstart];

        sdbind->influence = bpoly->weight;
        sdbind->numverts = bpoly->numverts;

        sdbind->mode = MOD_SDEF_MODE_NGON;
        sdbind->vert_weights = MEM_malloc_arrayN(
            bpoly->numverts, sizeof(*sdbind->vert_weights), "SDefNgonVertWeights");
        if (sdbind->vert_weights == NULL) {
          data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
          return;
        }

        sdbind->vert_inds = MEM_malloc_arrayN(
            bpoly->numverts, sizeof(*sdbind->vert_inds), "SDefNgonVertInds");
        if (sdbind->vert_inds == NULL) {
          data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
          return;
        }

        interp_weights_poly_v2(
            sdbind->vert_weights, bpoly->coords_v2, bpoly->numverts, bpoly->point_v2);

        /* Re-project vert based on weights and original poly verts,
         * to reintroduce poly non-planarity */
        zero_v3(point_co_proj);
        for (int j = 0; j < bpoly->numverts; j++, loop++) {
          madd_v3_v3fl(point_co_proj, bpoly->coords[j], sdbind->vert_weights[j]);
          sdbind->vert_inds[j] = loop->v;
        }

        sdbind->normal_dist = computeNormalDisplacement(point_co, point_co_proj, bpoly->normal);

        sdbind++;
        i++;
      }
      else {
        float tmp_vec[3];
        float cent[3], norm[3];
        float v1[3], v2[3], v3[3];

        if (1.0f - bpoly->dominant_angle_weight >= FLT_EPSILON) {
          sdbind->influence = bpoly->weight * (1.0f - bpoly->dominant_angle_weight);
          sdbind->numverts = bpoly->numverts;

          sdbind->mode = MOD_SDEF_MODE_CENTROID;
          sdbind->vert_weights = MEM_malloc_arrayN(
              3, sizeof(*sdbind->vert_weights), "SDefCentVertWeights");
          if (sdbind->vert_weights == NULL) {
            data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
            return;
          }

          sdbind->vert_inds = MEM_malloc_arrayN(
              bpoly->numverts, sizeof(*sdbind->vert_inds), "SDefCentVertInds");
          if (sdbind->vert_inds == NULL) {
            data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
            return;
          }

          sortPolyVertsEdge(sdbind->vert_inds,
                            &data->mloop[bpoly->loopstart],
                            bpoly->edge_inds[bpoly->dominant_edge],
                            bpoly->numverts);

          copy_v3_v3(v1, data->targetCos[sdbind->vert_inds[0]]);
          copy_v3_v3(v2, data->targetCos[sdbind->vert_inds[1]]);
          copy_v3_v3(v3, bpoly->centroid);

          mid_v3_v3v3v3(cent, v1, v2, v3);
          normal_tri_v3(norm, v1, v2, v3);

          add_v3_v3v3(tmp_vec, point_co, bpoly->normal);

          /* We are sure the line is not parallel to the plane.
           * Checking return value just to avoid warning... */
          if (!isect_line_plane_v3(point_co_proj, point_co, tmp_vec, cent, norm)) {
            BLI_assert(false);
          }

          interp_weights_tri_v3(sdbind->vert_weights, v1, v2, v3, point_co_proj);

          sdbind->normal_dist = computeNormalDisplacement(point_co, point_co_proj, bpoly->normal);

          sdbind++;
          i++;
        }

        if (bpoly->dominant_angle_weight >= FLT_EPSILON) {
          sdbind->influence = bpoly->weight * bpoly->dominant_angle_weight;
          sdbind->numverts = bpoly->numverts;

          sdbind->mode = MOD_SDEF_MODE_LOOPTRI;
          sdbind->vert_weights = MEM_malloc_arrayN(
              3, sizeof(*sdbind->vert_weights), "SDefTriVertWeights");
          if (sdbind->vert_weights == NULL) {
            data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
            return;
          }

          sdbind->vert_inds = MEM_malloc_arrayN(
              bpoly->numverts, sizeof(*sdbind->vert_inds), "SDefTriVertInds");
          if (sdbind->vert_inds == NULL) {
            data->success = MOD_SDEF_BIND_RESULT_MEM_ERR;
            return;
          }

          sortPolyVertsTri(sdbind->vert_inds,
                           &data->mloop[bpoly->loopstart],
                           bpoly->edge_vert_inds[0],
                           bpoly->numverts);

          copy_v3_v3(v1, data->targetCos[sdbind->vert_inds[0]]);
          copy_v3_v3(v2, data->targetCos[sdbind->vert_inds[1]]);
          copy_v3_v3(v3, data->targetCos[sdbind->vert_inds[2]]);

          mid_v3_v3v3v3(cent, v1, v2, v3);
          normal_tri_v3(norm, v1, v2, v3);

          add_v3_v3v3(tmp_vec, point_co, bpoly->normal);

          /* We are sure the line is not parallel to the plane.
           * Checking return value just to avoid warning... */
          if (!isect_line_plane_v3(point_co_proj, point_co, tmp_vec, cent, norm)) {
            BLI_assert(false);
          }

          interp_weights_tri_v3(sdbind->vert_weights, v1, v2, v3, point_co_proj);

          sdbind->normal_dist = computeNormalDisplacement(point_co, point_co_proj, bpoly->normal);

          sdbind++;
          i++;
        }
      }
    }
  }

  freeBindData(bwdata);
}

/* Remove vertices without bind data from the bind array. */
static void compactSparseBinds(SurfaceDeformModifierData *smd)
{
  smd->num_bind_verts = 0;

  for (uint i = 0; i < smd->num_mesh_verts; i++) {
    if (smd->verts[i].numbinds > 0) {
      smd->verts[smd->num_bind_verts++] = smd->verts[i];
    }
  }

  smd->verts = MEM_reallocN_id(
      smd->verts, sizeof(*smd->verts) * smd->num_bind_verts, "SDefBindVerts (sparse)");
}

static bool surfacedeformBind(Object *ob,
                              SurfaceDeformModifierData *smd_orig,
                              SurfaceDeformModifierData *smd_eval,
                              float (*vertexCos)[3],
                              uint numverts,
                              uint tnumpoly,
                              uint tnumverts,
                              Mesh *target,
                              Mesh *mesh)
{
  BVHTreeFromMesh treeData = {NULL};
  const MVert *mvert = target->mvert;
  const MPoly *mpoly = target->mpoly;
  const MEdge *medge = target->medge;
  const MLoop *mloop = target->mloop;
  uint tnumedges = target->totedge;
  int adj_result;
  SDefAdjacencyArray *vert_edges;
  SDefAdjacency *adj_array;
  SDefEdgePolys *edge_polys;

  vert_edges = MEM_calloc_arrayN(tnumverts, sizeof(*vert_edges), "SDefVertEdgeMap");
  if (vert_edges == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    return false;
  }

  adj_array = MEM_malloc_arrayN(tnumedges, 2 * sizeof(*adj_array), "SDefVertEdge");
  if (adj_array == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    MEM_freeN(vert_edges);
    return false;
  }

  edge_polys = MEM_calloc_arrayN(tnumedges, sizeof(*edge_polys), "SDefEdgeFaceMap");
  if (edge_polys == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    MEM_freeN(vert_edges);
    MEM_freeN(adj_array);
    return false;
  }

  smd_orig->verts = MEM_malloc_arrayN(numverts, sizeof(*smd_orig->verts), "SDefBindVerts");
  if (smd_orig->verts == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    freeAdjacencyMap(vert_edges, adj_array, edge_polys);
    return false;
  }

  BKE_bvhtree_from_mesh_get(&treeData, target, BVHTREE_FROM_LOOPTRI, 2);
  if (treeData.tree == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    freeAdjacencyMap(vert_edges, adj_array, edge_polys);
    MEM_freeN(smd_orig->verts);
    smd_orig->verts = NULL;
    return false;
  }

  adj_result = buildAdjacencyMap(
      mpoly, medge, mloop, tnumpoly, tnumedges, vert_edges, adj_array, edge_polys);

  if (adj_result == MOD_SDEF_BIND_RESULT_NONMANY_ERR) {
    BKE_modifier_set_error(
        ob, (ModifierData *)smd_eval, "Target has edges with more than two polygons");
    freeAdjacencyMap(vert_edges, adj_array, edge_polys);
    free_bvhtree_from_mesh(&treeData);
    MEM_freeN(smd_orig->verts);
    smd_orig->verts = NULL;
    return false;
  }

  smd_orig->num_mesh_verts = numverts;
  smd_orig->numpoly = tnumpoly;

  int defgrp_index;
  MDeformVert *dvert;
  MOD_get_vgroup(ob, mesh, smd_orig->defgrp_name, &dvert, &defgrp_index);
  const bool invert_vgroup = (smd_orig->flags & MOD_SDEF_INVERT_VGROUP) != 0;
  const bool sparse_bind = (smd_orig->flags & MOD_SDEF_SPARSE_BIND) != 0;

  SDefBindCalcData data = {
      .treeData = &treeData,
      .vert_edges = vert_edges,
      .edge_polys = edge_polys,
      .mpoly = mpoly,
      .medge = medge,
      .mloop = mloop,
      .looptri = BKE_mesh_runtime_looptri_ensure(target),
      .targetCos = MEM_malloc_arrayN(tnumverts, sizeof(float[3]), "SDefTargetBindVertArray"),
      .bind_verts = smd_orig->verts,
      .vertexCos = vertexCos,
      .falloff = smd_orig->falloff,
      .success = MOD_SDEF_BIND_RESULT_SUCCESS,
      .dvert = dvert,
      .defgrp_index = defgrp_index,
      .invert_vgroup = invert_vgroup,
      .sparse_bind = sparse_bind,
  };

  if (data.targetCos == NULL) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    freeData((ModifierData *)smd_orig);
    return false;
  }

  invert_m4_m4(data.imat, smd_orig->mat);

  for (int i = 0; i < tnumverts; i++) {
    mul_v3_m4v3(data.targetCos[i], smd_orig->mat, mvert[i].co);
  }

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = (numverts > 10000);
  BLI_task_parallel_range(0, numverts, &data, bindVert, &settings);

  MEM_freeN(data.targetCos);

  if (sparse_bind) {
    compactSparseBinds(smd_orig);
  }
  else {
    smd_orig->num_bind_verts = numverts;
  }

  if (data.success == MOD_SDEF_BIND_RESULT_MEM_ERR) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Out of memory");
    freeData((ModifierData *)smd_orig);
  }
  else if (data.success == MOD_SDEF_BIND_RESULT_NONMANY_ERR) {
    BKE_modifier_set_error(
        ob, (ModifierData *)smd_eval, "Target has edges with more than two polygons");
    freeData((ModifierData *)smd_orig);
  }
  else if (data.success == MOD_SDEF_BIND_RESULT_CONCAVE_ERR) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Target contains concave polygons");
    freeData((ModifierData *)smd_orig);
  }
  else if (data.success == MOD_SDEF_BIND_RESULT_OVERLAP_ERR) {
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Target contains overlapping vertices");
    freeData((ModifierData *)smd_orig);
  }
  else if (data.success == MOD_SDEF_BIND_RESULT_GENERIC_ERR) {
    /* I know this message is vague, but I could not think of a way
     * to explain this with a reasonably sized message.
     * Though it shouldn't really matter all that much,
     * because this is very unlikely to occur */
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "Target contains invalid polygons");
    freeData((ModifierData *)smd_orig);
  }
  else if (smd_orig->num_bind_verts == 0 || !smd_orig->verts) {
    data.success = MOD_SDEF_BIND_RESULT_GENERIC_ERR;
    BKE_modifier_set_error(ob, (ModifierData *)smd_eval, "No vertices were bound");
    freeData((ModifierData *)smd_orig);
  }

  freeAdjacencyMap(vert_edges, adj_array, edge_polys);
  free_bvhtree_from_mesh(&treeData);

  return data.success == 1;
}

static void deformVert(void *__restrict userdata,
                       const int index,
                       const TaskParallelTLS *__restrict UNUSED(tls))
{
  const SDefDeformData *const data = (SDefDeformData *)userdata;
  const SDefBind *sdbind = data->bind_verts[index].binds;
  const int num_binds = data->bind_verts[index].numbinds;
  const unsigned int vertex_idx = data->bind_verts[index].vertex_idx;
  float *const vertexCos = data->vertexCos[vertex_idx];
  float norm[3], temp[3], offset[3];

  /* Retrieve the value of the weight vertex group if specified. */
  float weight = 1.0f;

  if (data->dvert && data->defgrp_index != -1) {
    weight = BKE_defvert_find_weight(&data->dvert[vertex_idx], data->defgrp_index);

    if (data->invert_vgroup) {
      weight = 1.0f - weight;
    }
  }

  /* Check if this vertex will be deformed. If it is not deformed we return and avoid
   * unnecessary calculations. */
  if (weight == 0.0f) {
    return;
  }

  zero_v3(offset);

  /* Allocate a `coords_buffer` that fits all the temp-data. */
  int max_verts = 0;
  for (int j = 0; j < num_binds; j++) {
    max_verts = MAX2(max_verts, sdbind[j].numverts);
  }

  const bool big_buffer = max_verts > 256;
  float(*coords_buffer)[3];

  if (UNLIKELY(big_buffer)) {
    coords_buffer = MEM_malloc_arrayN(max_verts, sizeof(*coords_buffer), __func__);
  }
  else {
    coords_buffer = BLI_array_alloca(coords_buffer, max_verts);
  }

  for (int j = 0; j < num_binds; j++, sdbind++) {
    for (int k = 0; k < sdbind->numverts; k++) {
      copy_v3_v3(coords_buffer[k], data->targetCos[sdbind->vert_inds[k]]);
    }

    normal_poly_v3(norm, coords_buffer, sdbind->numverts);
    zero_v3(temp);

    switch (sdbind->mode) {
      /* ---------- looptri mode ---------- */
      case MOD_SDEF_MODE_LOOPTRI: {
        madd_v3_v3fl(temp, data->targetCos[sdbind->vert_inds[0]], sdbind->vert_weights[0]);
        madd_v3_v3fl(temp, data->targetCos[sdbind->vert_inds[1]], sdbind->vert_weights[1]);
        madd_v3_v3fl(temp, data->targetCos[sdbind->vert_inds[2]], sdbind->vert_weights[2]);
        break;
      }

      /* ---------- ngon mode ---------- */
      case MOD_SDEF_MODE_NGON: {
        for (int k = 0; k < sdbind->numverts; k++) {
          madd_v3_v3fl(temp, coords_buffer[k], sdbind->vert_weights[k]);
        }
        break;
      }

      /* ---------- centroid mode ---------- */
      case MOD_SDEF_MODE_CENTROID: {
        float cent[3];
        mid_v3_v3_array(cent, coords_buffer, sdbind->numverts);

        madd_v3_v3fl(temp, data->targetCos[sdbind->vert_inds[0]], sdbind->vert_weights[0]);
        madd_v3_v3fl(temp, data->targetCos[sdbind->vert_inds[1]], sdbind->vert_weights[1]);
        madd_v3_v3fl(temp, cent, sdbind->vert_weights[2]);
        break;
      }
    }

    /* Apply normal offset (generic for all modes) */
    madd_v3_v3fl(temp, norm, sdbind->normal_dist);

    madd_v3_v3fl(offset, temp, sdbind->influence);
  }
  /* Subtract the vertex coord to get the deformation offset. */
  sub_v3_v3(offset, vertexCos);

  /* Add the offset to start coord multiplied by the strength and weight values. */
  madd_v3_v3fl(vertexCos, offset, data->strength * weight);

  if (UNLIKELY(big_buffer)) {
    MEM_freeN(coords_buffer);
  }
}

static void surfacedeformModifier_do(ModifierData *md,
                                     const ModifierEvalContext *ctx,
                                     float (*vertexCos)[3],
                                     uint numverts,
                                     Object *ob,
                                     Mesh *mesh)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;
  Mesh *target;
  uint tnumverts, tnumpoly;

  /* Exit function if bind flag is not set (free bind data if any). */
  if (!(smd->flags & MOD_SDEF_BIND)) {
    if (smd->verts != NULL) {
      if (!DEG_is_active(ctx->depsgraph)) {
        BKE_modifier_set_error(ob, md, "Attempt to bind from inactive dependency graph");
        return;
      }
      ModifierData *md_orig = BKE_modifier_get_original(md);
      freeData(md_orig);
    }
    return;
  }

  Object *ob_target = smd->target;
  target = BKE_modifier_get_evaluated_mesh_from_evaluated_object(ob_target, false);
  if (!target) {
    BKE_modifier_set_error(ob, md, "No valid target mesh");
    return;
  }

  tnumverts = BKE_mesh_wrapper_vert_len(target);
  tnumpoly = BKE_mesh_wrapper_poly_len(target);

  /* If not bound, execute bind. */
  if (smd->verts == NULL) {
    if (!DEG_is_active(ctx->depsgraph)) {
      BKE_modifier_set_error(ob, md, "Attempt to unbind from inactive dependency graph");
      return;
    }

    SurfaceDeformModifierData *smd_orig = (SurfaceDeformModifierData *)BKE_modifier_get_original(
        md);
    float tmp_mat[4][4];

    invert_m4_m4(tmp_mat, ob->obmat);
    mul_m4_m4m4(smd_orig->mat, tmp_mat, ob_target->obmat);

    /* Avoid converting edit-mesh data, binding is an exception. */
    BKE_mesh_wrapper_ensure_mdata(target);

    if (!surfacedeformBind(
            ob, smd_orig, smd, vertexCos, numverts, tnumpoly, tnumverts, target, mesh)) {
      smd->flags &= ~MOD_SDEF_BIND;
    }
    /* Early abort, this is binding 'call', no need to perform whole evaluation. */
    return;
  }

  /* Poly count checks */
  if (smd->num_mesh_verts != numverts) {
    BKE_modifier_set_error(
        ob, md, "Vertices changed from %u to %u", smd->num_mesh_verts, numverts);
    return;
  }
  if (smd->numpoly != tnumpoly) {
    BKE_modifier_set_error(
        ob, md, "Target polygons changed from %u to %u", smd->numpoly, tnumpoly);
    return;
  }

  /* Early out if modifier would not affect input at all - still *after* the sanity checks
   * (and potential binding) above. */
  if (smd->strength == 0.0f) {
    return;
  }

  int defgrp_index;
  MDeformVert *dvert;
  MOD_get_vgroup(ob, mesh, smd->defgrp_name, &dvert, &defgrp_index);
  const bool invert_vgroup = (smd->flags & MOD_SDEF_INVERT_VGROUP) != 0;

  /* Actual vertex location update starts here */
  SDefDeformData data = {
      .bind_verts = smd->verts,
      .targetCos = MEM_malloc_arrayN(tnumverts, sizeof(float[3]), "SDefTargetVertArray"),
      .vertexCos = vertexCos,
      .dvert = dvert,
      .defgrp_index = defgrp_index,
      .invert_vgroup = invert_vgroup,
      .strength = smd->strength,
  };

  if (data.targetCos != NULL) {
    BKE_mesh_wrapper_vert_coords_copy_with_mat4(target, data.targetCos, tnumverts, smd->mat);

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    settings.use_threading = (smd->num_bind_verts > 10000);
    BLI_task_parallel_range(0, smd->num_bind_verts, &data, deformVert, &settings);

    MEM_freeN(data.targetCos);
  }
}

static void deformVerts(ModifierData *md,
                        const ModifierEvalContext *ctx,
                        Mesh *mesh,
                        float (*vertexCos)[3],
                        int numVerts)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;
  Mesh *mesh_src = NULL;

  if (smd->defgrp_name[0] != '\0') {
    /* Only need to use mesh_src when a vgroup is used. */
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, NULL, mesh, NULL, numVerts, false, false);
  }

  surfacedeformModifier_do(md, ctx, vertexCos, numVerts, ctx->object, mesh_src);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static void deformVertsEM(ModifierData *md,
                          const ModifierEvalContext *ctx,
                          struct BMEditMesh *em,
                          Mesh *mesh,
                          float (*vertexCos)[3],
                          int numVerts)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;
  Mesh *mesh_src = NULL;

  if (smd->defgrp_name[0] != '\0') {
    /* Only need to use mesh_src when a vgroup is used. */
    mesh_src = MOD_deform_mesh_eval_get(ctx->object, em, mesh, NULL, numVerts, false, false);
  }

  surfacedeformModifier_do(md, ctx, vertexCos, numVerts, ctx->object, mesh_src);

  if (!ELEM(mesh_src, NULL, mesh)) {
    BKE_id_free(NULL, mesh_src);
  }
}

static bool isDisabled(const Scene *UNUSED(scene), ModifierData *md, bool UNUSED(useRenderParams))
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  /* The object type check is only needed here in case we have a placeholder
   * object assigned (because the library containing the mesh is missing).
   *
   * In other cases it should be impossible to have a type mismatch.
   */
  return (smd->target == NULL || smd->target->type != OB_MESH) &&
         !(smd->verts != NULL && !(smd->flags & MOD_SDEF_BIND));
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *col;
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  PointerRNA target_ptr = RNA_pointer_get(ptr, "target");

  bool is_bound = RNA_boolean_get(ptr, "is_bound");

  uiLayoutSetPropSep(layout, true);

  col = uiLayoutColumn(layout, false);
  uiLayoutSetActive(col, !is_bound);
  uiItemR(col, ptr, "target", 0, NULL, ICON_NONE);
  uiItemR(col, ptr, "falloff", 0, NULL, ICON_NONE);

  uiItemR(layout, ptr, "strength", 0, NULL, ICON_NONE);

  modifier_vgroup_ui(layout, ptr, &ob_ptr, "vertex_group", "invert_vertex_group", NULL);

  col = uiLayoutColumn(layout, false);
  uiLayoutSetEnabled(col, !is_bound);
  uiLayoutSetActive(col, !is_bound && RNA_string_length(ptr, "vertex_group") != 0);
  uiItemR(col, ptr, "use_sparse_bind", 0, NULL, ICON_NONE);

  uiItemS(layout);

  col = uiLayoutColumn(layout, false);
  if (is_bound) {
    uiItemO(col, IFACE_("Unbind"), ICON_NONE, "OBJECT_OT_surfacedeform_bind");
  }
  else {
    uiLayoutSetActive(col, !RNA_pointer_is_null(&target_ptr));
    uiItemO(col, IFACE_("Bind"), ICON_NONE, "OBJECT_OT_surfacedeform_bind");
  }
  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_SurfaceDeform, panel_draw);
}

static void blendWrite(BlendWriter *writer, const ModifierData *md)
{
  const SurfaceDeformModifierData *smd = (const SurfaceDeformModifierData *)md;

  BLO_write_struct_array(writer, SDefVert, smd->num_bind_verts, smd->verts);

  if (smd->verts) {
    for (int i = 0; i < smd->num_bind_verts; i++) {
      BLO_write_struct_array(writer, SDefBind, smd->verts[i].numbinds, smd->verts[i].binds);

      if (smd->verts[i].binds) {
        for (int j = 0; j < smd->verts[i].numbinds; j++) {
          BLO_write_uint32_array(
              writer, smd->verts[i].binds[j].numverts, smd->verts[i].binds[j].vert_inds);

          if (ELEM(smd->verts[i].binds[j].mode, MOD_SDEF_MODE_CENTROID, MOD_SDEF_MODE_LOOPTRI)) {
            BLO_write_float3_array(writer, 1, smd->verts[i].binds[j].vert_weights);
          }
          else {
            BLO_write_float_array(
                writer, smd->verts[i].binds[j].numverts, smd->verts[i].binds[j].vert_weights);
          }
        }
      }
    }
  }
}

static void blendRead(BlendDataReader *reader, ModifierData *md)
{
  SurfaceDeformModifierData *smd = (SurfaceDeformModifierData *)md;

  BLO_read_data_address(reader, &smd->verts);

  if (smd->verts) {
    for (int i = 0; i < smd->num_bind_verts; i++) {
      BLO_read_data_address(reader, &smd->verts[i].binds);

      if (smd->verts[i].binds) {
        for (int j = 0; j < smd->verts[i].numbinds; j++) {
          BLO_read_uint32_array(
              reader, smd->verts[i].binds[j].numverts, &smd->verts[i].binds[j].vert_inds);

          if (ELEM(smd->verts[i].binds[j].mode, MOD_SDEF_MODE_CENTROID, MOD_SDEF_MODE_LOOPTRI)) {
            BLO_read_float3_array(reader, 1, &smd->verts[i].binds[j].vert_weights);
          }
          else {
            BLO_read_float_array(
                reader, smd->verts[i].binds[j].numverts, &smd->verts[i].binds[j].vert_weights);
          }
        }
      }
    }
  }
}

ModifierTypeInfo modifierType_SurfaceDeform = {
    /* name */ "SurfaceDeform",
    /* structName */ "SurfaceDeformModifierData",
    /* structSize */ sizeof(SurfaceDeformModifierData),
    /* srna */ &RNA_SurfaceDeformModifier,
    /* type */ eModifierTypeType_OnlyDeform,
    /* flags */ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode,
    /* icon */ ICON_MOD_MESHDEFORM,

    /* copyData */ copyData,

    /* deformVerts */ deformVerts,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ deformVertsEM,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ NULL,
    /* modifyGeometrySet */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ requiredDataMask,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ blendWrite,
    /* blendRead */ blendRead,
};
