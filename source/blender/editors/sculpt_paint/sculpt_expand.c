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
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_linklist_stack.h"
#include "BLI_math.h"
#include "BLI_task.h"

#include "BLT_translation.h"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_brush.h"
#include "BKE_ccg.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_multires.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "ED_view3d.h"
#include "paint_intern.h"
#include "sculpt_intern.h"

#include "IMB_colormanagement.h"
#include "IMB_imbuf.h"

#include "bmesh.h"

#include <math.h>
#include <stdlib.h>

#define SCULPT_EXPAND_VERTEX_NONE -1

enum {
  SCULPT_EXPAND_MODAL_CONFIRM = 1,
  SCULPT_EXPAND_MODAL_CANCEL,
  SCULPT_EXPAND_MODAL_INVERT,
  SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE,
  SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE,
  SCULPT_EXPAND_MODAL_FALLOFF_CYCLE,
  SCULPT_EXPAND_MODAL_RECURSION_STEP,
  SCULPT_EXPAND_MODAL_MOVE_TOGGLE,
  SCULPT_EXPAND_MODAL_FALLOFF_GEODESICS,
  SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY,
  SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL,
  SCULPT_EXPAND_MODAL_SNAP_TOGGLE,
};

static EnumPropertyItem prop_sculpt_expand_falloff_type_items[] = {
    {SCULPT_EXPAND_FALLOFF_GEODESICS, "GEODESICS", 0, "Surface", ""},
    {SCULPT_EXPAND_FALLOFF_TOPOLOGY, "TOPOLOGY", 0, "Topology", ""},
    {SCULPT_EXPAND_FALLOFF_NORMALS, "NORMALS", 0, "Normals", ""},
    {SCULPT_EXPAND_FALLOFF_SPHERICAL, "SPHERICAL", 0, "Spherical", ""},
    {SCULPT_EXPAND_FALLOFF_BOUNDARY_TOPOLOGY, "BOUNDARY_TOPOLOGY`", 0, "Boundary Topology", ""},
    {0, NULL, 0, NULL, NULL},
};

static EnumPropertyItem prop_sculpt_expand_target_type_items[] = {
    {SCULPT_EXPAND_TARGET_MASK, "MASK", 0, "Mask", ""},
    {SCULPT_EXPAND_TARGET_FACE_SETS, "FACE_SETS", 0, "Face Sets", ""},
    {SCULPT_EXPAND_TARGET_COLORS, "COLOR", 0, "Color", ""},
    {0, NULL, 0, NULL, NULL},
};

static float *sculpt_expand_geodesic_falloff_create(Sculpt *sd, Object *ob, const int vertex)
{
  return SCULPT_geodesic_from_vertex_and_symm(sd, ob, vertex, FLT_MAX);
}

typedef struct ExpandFloodFillData {
  float original_normal[3];
  float edge_sensitivity;
  float *dists;
  float *edge_factor;
} ExpandFloodFillData;

static bool mask_expand_topology_floodfill_cb(
    SculptSession *UNUSED(ss), int from_v, int to_v, bool is_duplicate, void *userdata)
{
  ExpandFloodFillData *data = userdata;
  if (!is_duplicate) {
    const float to_it = data->dists[from_v] + 1.0f;
    data->dists[to_v] = to_it;
  }
  else {
    data->dists[to_v] = data->dists[from_v];
  }
  return true;
}

static float *sculpt_expand_topology_falloff_create(Sculpt *sd, Object *ob, const int vertex)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);
  float *dists = MEM_calloc_arrayN(sizeof(float), totvert, "topology dist");

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);
  SCULPT_floodfill_add_initial_with_symmetry(sd, ob, ss, &flood, vertex, FLT_MAX);

  ExpandFloodFillData fdata;
  fdata.dists = dists;

  SCULPT_floodfill_execute(ss, &flood, mask_expand_topology_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  return dists;
}

static bool mask_expand_normal_floodfill_cb(
    SculptSession *ss, int from_v, int to_v, bool is_duplicate, void *userdata)
{
  ExpandFloodFillData *data = userdata;
  if (!is_duplicate) {
    float current_normal[3], prev_normal[3];
    SCULPT_vertex_normal_get(ss, to_v, current_normal);
    SCULPT_vertex_normal_get(ss, from_v, prev_normal);
    const float from_edge_factor = data->edge_factor[from_v];
    data->edge_factor[to_v] = dot_v3v3(current_normal, prev_normal) * from_edge_factor;
    data->dists[to_v] = dot_v3v3(data->original_normal, current_normal) *
                        powf(from_edge_factor, data->edge_sensitivity);
    CLAMP(data->dists[to_v], 0.0f, 1.0f);
  }
  else {
    /* PBVH_GRIDS duplicate handling. */
    data->edge_factor[to_v] = data->edge_factor[from_v];
    data->dists[to_v] = data->dists[from_v];
  }

  return true;
}

static float *sculpt_expand_normal_falloff_create(Sculpt *sd,
                                                  Object *ob,
                                                  const int vertex,
                                                  const float edge_sensitivity)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);
  float *dists = MEM_malloc_arrayN(sizeof(float), totvert, "normal dist");
  float *edge_factor = MEM_callocN(sizeof(float) * totvert, "mask edge factor");
  for (int i = 0; i < totvert; i++) {
    edge_factor[i] = 1.0f;
  }

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);
  SCULPT_floodfill_add_initial_with_symmetry(sd, ob, ss, &flood, vertex, FLT_MAX);

  ExpandFloodFillData fdata;
  fdata.dists = dists;
  fdata.edge_factor = edge_factor;
  fdata.edge_sensitivity = edge_sensitivity;
  SCULPT_vertex_normal_get(ss, vertex, fdata.original_normal);

  SCULPT_floodfill_execute(ss, &flood, mask_expand_normal_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  for (int i = 0; i < totvert; i++) {
    dists[i] = FLT_MAX;
  }

  for (int repeat = 0; repeat < 2; repeat++) {
    for (int i = 0; i < totvert; i++) {
      float avg = 0.0f;
      SculptVertexNeighborIter ni;
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, i, ni) {
        avg += dists[ni.index];
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
      dists[i] = avg / ni.size;
    }
  }

  MEM_SAFE_FREE(edge_factor);

  return dists;
}

static float *sculpt_expand_spherical_falloff_create(Sculpt *sd, Object *ob, const int vertex)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);

  float *dists = MEM_malloc_arrayN(sizeof(float), totvert, "spherical dist");
  for (int i = 0; i < totvert; i++) {
    dists[i] = FLT_MAX;
  }
  const char symm = SCULPT_mesh_symmetry_xyz_get(ob);

  for (char symm_it = 0; symm_it <= symm; symm_it++) {
    if (!SCULPT_is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    int v = SCULPT_EXPAND_VERTEX_NONE;
    if (symm_it == 0) {
      v = vertex;
    }
    else {
      float location[3];
      flip_v3_v3(location, SCULPT_vertex_co_get(ss, vertex), symm_it);
      v = SCULPT_nearest_vertex_get(sd, ob, location, FLT_MAX, false);
    }
    if (v != -1) {
      const float *co = SCULPT_vertex_co_get(ss, v);
      for (int i = 0; i < totvert; i++) {
        dists[i] = min_ff(dists[i], len_v3v3(co, SCULPT_vertex_co_get(ss, i)));
      }
    }
  }

  return dists;
}

static float *sculpt_expand_boundary_topology_falloff_create(Sculpt *sd,
                                                             Object *ob,
                                                             const int vertex)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);
  float *dists = MEM_calloc_arrayN(sizeof(float), totvert, "spherical dist");
  BLI_bitmap *visited_vertices = BLI_BITMAP_NEW(totvert, "visited vertices");
  GSQueue *queue = BLI_gsqueue_new(sizeof(int));

  const char symm = SCULPT_mesh_symmetry_xyz_get(ob);
  for (char symm_it = 0; symm_it <= symm; symm_it++) {
    if (!SCULPT_is_symmetry_iteration_valid(symm_it, symm)) {
      continue;
    }
    int v = SCULPT_EXPAND_VERTEX_NONE;
    if (symm_it == 0) {
      v = vertex;
    }
    else {
      float location[3];
      flip_v3_v3(location, SCULPT_vertex_co_get(ss, vertex), symm_it);
      v = SCULPT_nearest_vertex_get(sd, ob, location, FLT_MAX, false);
    }

    SculptBoundary *boundary = SCULPT_boundary_data_init(ob, NULL, v, FLT_MAX);
    for (int i = 0; i < boundary->num_vertices; i++) {
      BLI_gsqueue_push(queue, &boundary->vertices[i]);
      BLI_BITMAP_ENABLE(visited_vertices, boundary->vertices[i]);
    }
    SCULPT_boundary_data_free(boundary);
  }

  if (BLI_gsqueue_is_empty(queue)) {
    return dists;
  }

  while (!BLI_gsqueue_is_empty(queue)) {
    int v;
    BLI_gsqueue_pop(queue, &v);
    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, v, ni) {
      if (BLI_BITMAP_TEST(visited_vertices, ni.index)) {
        continue;
      }
      dists[ni.index] = dists[v] + 1.0f;
      BLI_BITMAP_ENABLE(visited_vertices, ni.index);
      BLI_gsqueue_push(queue, &ni.index);
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  }

  for (int i = 0; i < totvert; i++) {
    if (BLI_BITMAP_TEST(visited_vertices, i)) {
      continue;
    }
    dists[i] = FLT_MAX;
  }

  BLI_gsqueue_free(queue);
  MEM_freeN(visited_vertices);
  return dists;
}

static void sculpt_expand_update_max_falloff_factor(SculptSession *ss, ExpandCache *expand_cache)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  expand_cache->max_falloff_factor = -FLT_MAX;
  for (int i = 0; i < totvert; i++) {
    if (expand_cache->falloff_factor[i] == FLT_MAX) {
      continue;
    }
    expand_cache->max_falloff_factor = max_ff(expand_cache->max_falloff_factor,
                                              expand_cache->falloff_factor[i]);
  }
}

static void sculpt_expand_update_max_face_falloff_factor(SculptSession *ss,
                                                         ExpandCache *expand_cache)
{
  const int totface = ss->totfaces;
  expand_cache->max_face_falloff_factor = -FLT_MAX;
  for (int i = 0; i < totface; i++) {
    if (expand_cache->face_falloff_factor[i] == FLT_MAX) {
      continue;
    }
    expand_cache->max_face_falloff_factor = max_ff(expand_cache->max_face_falloff_factor,
                                                   expand_cache->face_falloff_factor[i]);
  }
}

static void sculpt_expand_mesh_face_falloff_from_vertex_falloff(Mesh *mesh,
                                                                ExpandCache *expand_cache)
{
  if (expand_cache->face_falloff_factor) {
    MEM_freeN(expand_cache->face_falloff_factor);
  }
  expand_cache->face_falloff_factor = MEM_malloc_arrayN(
      mesh->totpoly, sizeof(float), "face falloff factors");

  for (int p = 0; p < mesh->totpoly; p++) {
    MPoly *poly = &mesh->mpoly[p];
    float accum = 0.0f;
    for (int l = 0; l < poly->totloop; l++) {
      MLoop *loop = &mesh->mloop[l + poly->loopstart];
      accum += expand_cache->falloff_factor[loop->v];
    }
    expand_cache->face_falloff_factor[p] = accum / poly->totloop;
  }
}

static void sculpt_expand_falloff_factors_from_vertex_and_symm_create(
    ExpandCache *expand_cache,
    Sculpt *sd,
    Object *ob,
    const int vertex,
    eSculptExpandFalloffType falloff_type)
{
  if (expand_cache->falloff_factor) {
    MEM_freeN(expand_cache->falloff_factor);
  }

  switch (falloff_type) {
    case SCULPT_EXPAND_FALLOFF_GEODESICS:
      expand_cache->falloff_factor = sculpt_expand_geodesic_falloff_create(sd, ob, vertex);
      break;
    case SCULPT_EXPAND_FALLOFF_TOPOLOGY:
      expand_cache->falloff_factor = sculpt_expand_topology_falloff_create(sd, ob, vertex);
      break;
    case SCULPT_EXPAND_FALLOFF_NORMALS:
      expand_cache->falloff_factor = sculpt_expand_normal_falloff_create(sd, ob, vertex, 300.0f);
      break;
    case SCULPT_EXPAND_FALLOFF_SPHERICAL:
      expand_cache->falloff_factor = sculpt_expand_spherical_falloff_create(sd, ob, vertex);
      break;
    case SCULPT_EXPAND_FALLOFF_BOUNDARY_TOPOLOGY:
      expand_cache->falloff_factor = sculpt_expand_boundary_topology_falloff_create(
          sd, ob, vertex);
      break;
  }

  expand_cache->falloff_factor_type = falloff_type;

  SculptSession *ss = ob->sculpt;
  sculpt_expand_update_max_falloff_factor(ss, expand_cache);

  if (expand_cache->target == SCULPT_EXPAND_TARGET_FACE_SETS) {
    sculpt_expand_mesh_face_falloff_from_vertex_falloff(ob->data, expand_cache);
    sculpt_expand_update_max_face_falloff_factor(ss, expand_cache);
  }
}

static void sculpt_expand_cache_free(ExpandCache *expand_cache)
{
  if (expand_cache->snap_enabled_face_sets) {
    BLI_gset_free(expand_cache->snap_enabled_face_sets, NULL);
  }
  MEM_SAFE_FREE(expand_cache->nodes);
  MEM_SAFE_FREE(expand_cache->falloff_factor);
  MEM_SAFE_FREE(expand_cache->face_falloff_factor);
  MEM_SAFE_FREE(expand_cache->initial_mask);
  MEM_SAFE_FREE(expand_cache->initial_face_sets);
  MEM_SAFE_FREE(expand_cache->initial_color);
  MEM_SAFE_FREE(expand_cache);
}

static void sculpt_mask_expand_cancel(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  const bool create_face_set = RNA_boolean_get(op->ptr, "create_face_set");

  MEM_freeN(op->customdata);

  for (int n = 0; n < ss->filter_cache->totnode; n++) {
    PBVHNode *node = ss->filter_cache->nodes[n];
    if (create_face_set) {
      for (int i = 0; i < ss->totfaces; i++) {
        ss->face_sets[i] = ss->filter_cache->prev_face_set[i];
      }
    }
    else {
      PBVHVertexIter vd;
      BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_UNIQUE)
      {
        *vd.mask = ss->filter_cache->prev_mask[vd.index];
      }
      BKE_pbvh_vertex_iter_end;
    }

    BKE_pbvh_node_mark_redraw(node);
  }

  if (!create_face_set) {
    SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);
  }
  SCULPT_filter_cache_free(ss);
  SCULPT_undo_push_end();
  SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
  ED_workspace_status_text(C, NULL);
}

static void sculpt_expand_cancel(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;

  sculpt_expand_cache_free(ss->expand_cache);
  ED_workspace_status_text(C, NULL);
}

static bool sculpt_expand_state_get(SculptSession *ss, ExpandCache *expand_cache, const int i)
{

  bool enabled = false;

  if (expand_cache->snap) {
    const int face_set = SCULPT_vertex_face_set_get(ss, i);
    enabled = BLI_gset_haskey(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(face_set));
  }
  else {
    enabled = expand_cache->falloff_factor[i] <= expand_cache->active_factor;
  }

  if (expand_cache->invert) {
    enabled = !enabled;
  }
  return enabled;
}

static bool sculpt_expand_face_state_get(SculptSession *ss, ExpandCache *expand_cache, const int f)
{
  bool enabled = false;
  if (expand_cache->snap_enabled_face_sets) {
    const int face_set = ss->face_sets[f];
    enabled = BLI_gset_haskey(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(face_set));
  }
  else {
    enabled = expand_cache->face_falloff_factor[f] <= expand_cache->active_factor;
  }
  if (expand_cache->invert) {
    enabled = !enabled;
  }
  return enabled;
}

static float sculpt_expand_gradient_falloff_get(ExpandCache *expand_cache, const int i)
{
  if (!expand_cache->falloff_gradient) {
    return 1.0f;
  }

  if (expand_cache->invert) {
    return (expand_cache->falloff_factor[i] - expand_cache->active_factor) /
           (expand_cache->max_falloff_factor - expand_cache->active_factor);
  }

  return 1.0f - (expand_cache->falloff_factor[i] / expand_cache->active_factor);
}

static void sculpt_expand_mask_update_task_cb(void *__restrict userdata,
                                              const int i,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  ExpandCache *expand_cache = ss->expand_cache;

  bool any_changed = false;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_ALL)
  {
    const float initial_mask = *vd.mask;
    const bool enabled = sculpt_expand_state_get(ss, expand_cache, vd.index);

    float new_mask;

    if (enabled) {
      new_mask = sculpt_expand_gradient_falloff_get(expand_cache, vd.index);
    }
    else {
      new_mask = 0.0f;
    }

    if (expand_cache->preserve) {
      new_mask = max_ff(new_mask, expand_cache->initial_mask[vd.index]);
    }

    if (new_mask == initial_mask) {
      continue;
    }

    *vd.mask = clamp_f(new_mask, 0.0f, 1.0f);
    any_changed = true;
    if (vd.mvert) {
      vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
    }
  }
  BKE_pbvh_vertex_iter_end;
  if (any_changed) {
    BKE_pbvh_node_mark_update_mask(node);
  }
}

static void sculpt_expand_face_sets_update_task_cb(void *__restrict userdata,
                                                   const int i,
                                                   const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  ExpandCache *expand_cache = ss->expand_cache;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_ALL)
  {
    const bool enabled = sculpt_expand_state_get(ss, expand_cache, vd.index);

    if (!enabled) {
      continue;
    }

    if (expand_cache->falloff_gradient) {
      SCULPT_vertex_face_set_increase(ss, vd.index, expand_cache->next_face_set);
    }
    else {
      SCULPT_vertex_face_set_set(ss, vd.index, expand_cache->next_face_set);
    }

    if (vd.mvert) {
      vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
    }
  }
  BKE_pbvh_vertex_iter_end;

  BKE_pbvh_node_mark_update_mask(node);
}

static void sculpt_expand_face_sets_update(SculptSession *ss, ExpandCache *expand_cache)
{
  const int totface = ss->totfaces;
  for (int f = 0; f < totface; f++) {
    const bool enabled = sculpt_expand_face_state_get(ss, expand_cache, f);
    if (!enabled) {
      continue;
    }
    if (expand_cache->preserve) {
      ss->face_sets[f] += expand_cache->next_face_set;
    }
    else {
      ss->face_sets[f] = expand_cache->next_face_set;
    }
  }

  for (int i = 0; i < expand_cache->totnode; i++) {
    BKE_pbvh_node_mark_update_mask(expand_cache->nodes[i]);
  }
}

static void sculpt_expand_colors_update_task_cb(void *__restrict userdata,
                                                const int i,
                                                const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  ExpandCache *expand_cache = ss->expand_cache;

  bool any_changed = false;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_ALL)
  {
    float initial_color[4];
    copy_v4_v4(initial_color, vd.col);

    const bool enabled = sculpt_expand_state_get(ss, expand_cache, vd.index);
    float fade;

    if (enabled) {
      fade = sculpt_expand_gradient_falloff_get(expand_cache, vd.index);
    }
    else {
      fade = 0.0f;
    }

    fade *= 1.0f - *vd.mask;
    fade = clamp_f(fade, 0.0f, 1.0f);

    float final_color[4];
    float final_fill_color[4];
    mul_v4_v4fl(final_fill_color, expand_cache->fill_color, fade);
    IMB_blend_color_float(final_color,
                          expand_cache->initial_color[vd.index],
                          final_fill_color,
                          expand_cache->blend_mode);

    if (equals_v4v4(initial_color, final_color)) {
      continue;
    }

    copy_v4_v4(vd.col, final_color);
    any_changed = true;
    if (vd.mvert) {
      vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
    }
  }
  BKE_pbvh_vertex_iter_end;
  if (any_changed) {
    BKE_pbvh_node_mark_update_color(node);
  }
}

static void sculpt_expand_flush_updates(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  for (int i = 0; i < ss->expand_cache->totnode; i++) {
    BKE_pbvh_node_mark_redraw(ss->expand_cache->nodes[i]);
  }

  switch (ss->expand_cache->target) {
    case SCULPT_EXPAND_TARGET_MASK:
      SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);
      break;
    case SCULPT_EXPAND_TARGET_FACE_SETS:
      SCULPT_flush_update_step(C, SCULPT_UPDATE_MASK);
      break;
    case SCULPT_EXPAND_TARGET_COLORS:
      SCULPT_flush_update_step(C, SCULPT_UPDATE_COLOR);
      break;

    default:
      break;
  }
}

static void sculpt_expand_initial_state_store(Object *ob, ExpandCache *expand_cache)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);
  const int totface = ss->totfaces;

  expand_cache->initial_mask = MEM_malloc_arrayN(totvert, sizeof(float), "initial mask");
  for (int i = 0; i < totvert; i++) {
    expand_cache->initial_mask[i] = SCULPT_vertex_mask_get(ss, i);
  }

  expand_cache->initial_face_sets = MEM_malloc_arrayN(totvert, sizeof(int), "initial face set");
  for (int i = 0; i < totface; i++) {
    expand_cache->initial_face_sets[i] = ss->face_sets[i];
  }

  if (expand_cache->target == SCULPT_EXPAND_TARGET_COLORS) {
    expand_cache->initial_color = MEM_malloc_arrayN(totvert, sizeof(float[4]), "initial colors");
    for (int i = 0; i < totvert; i++) {
      copy_v4_v4(expand_cache->initial_color[i], SCULPT_vertex_color_get(ss, i));
    }
  }
}

static void sculpt_expand_face_sets_restore(SculptSession *ss, ExpandCache *expand_cache)
{
  const int totfaces = ss->totfaces;
  for (int i = 0; i < totfaces; i++) {
    ss->face_sets[i] = expand_cache->initial_face_sets[i];
  }
}

static void sculpt_expand_update_for_vertex(bContext *C, Object *ob, const int vertex)
{
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  ExpandCache *expand_cache = ss->expand_cache;

  /* Update the active factor in the cache. */
  if (vertex == SCULPT_EXPAND_VERTEX_NONE) {
    expand_cache->active_factor = expand_cache->max_falloff_factor;
  }
  else {
    expand_cache->active_factor = expand_cache->falloff_factor[vertex];
  }

  if (expand_cache->target == SCULPT_EXPAND_TARGET_FACE_SETS) {
    sculpt_expand_face_sets_restore(ss, expand_cache);
  }

  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .nodes = expand_cache->nodes,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, expand_cache->totnode);

  switch (expand_cache->target) {
    case SCULPT_EXPAND_TARGET_MASK:
      BLI_task_parallel_range(
          0, expand_cache->totnode, &data, sculpt_expand_mask_update_task_cb, &settings);
      break;
    case SCULPT_EXPAND_TARGET_FACE_SETS:
      /*
      BLI_task_parallel_range(
          0, expand_cache->totnode, &data, sculpt_expand_face_sets_update_task_cb, &settings);
      */
      sculpt_expand_face_sets_update(ss, expand_cache);
      break;
    case SCULPT_EXPAND_TARGET_COLORS:
      BLI_task_parallel_range(
          0, expand_cache->totnode, &data, sculpt_expand_colors_update_task_cb, &settings);
      break;
  }

  sculpt_expand_flush_updates(C);
}

static int sculpt_expand_target_vertex_update_and_get(bContext *C,
                                                      Object *ob,
                                                      const float mouse[2])
{
  SculptSession *ss = ob->sculpt;
  SculptCursorGeometryInfo sgi;
  if (SCULPT_cursor_geometry_info_update(C, &sgi, mouse, false)) {
    return SCULPT_active_vertex_get(ss);
  }
  else {
    return SCULPT_EXPAND_VERTEX_NONE;
  }
}

static void sculpt_expand_finish(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  SCULPT_undo_push_end();

  switch (ss->expand_cache->target) {
    case SCULPT_EXPAND_TARGET_MASK:
      SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
      break;
    case SCULPT_EXPAND_TARGET_FACE_SETS:
      SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
      break;
    case SCULPT_EXPAND_TARGET_COLORS:
      SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_COLOR);
      break;
  }

  sculpt_expand_cache_free(ss->expand_cache);
  ED_workspace_status_text(C, NULL);
}

static BLI_bitmap *sculpt_expand_bitmap_from_enabled(SculptSession *ss, ExpandCache *expand_cache)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  BLI_bitmap *enabled_vertices = BLI_BITMAP_NEW(totvert, "enabled vertices");
  for (int i = 0; i < totvert; i++) {
    const bool enabled = sculpt_expand_state_get(ss, expand_cache, i);
    BLI_BITMAP_SET(enabled_vertices, i, enabled);
  }
  return enabled_vertices;
}

static void sculpt_expand_resursion_step_add(Object *ob, ExpandCache *expand_cache)
{
  SculptSession *ss = ob->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);
  GSet *initial_vertices = BLI_gset_int_new("initial_vertices");
  BLI_bitmap *enabled_vertices = sculpt_expand_bitmap_from_enabled(ss, expand_cache);

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);

  for (int i = 0; i < totvert; i++) {
    SculptVertexNeighborIter ni;
    if (!BLI_BITMAP_TEST(enabled_vertices, i)) {
      continue;
    }

    bool is_expand_boundary = false;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, i, ni) {
      if (!BLI_BITMAP_TEST(enabled_vertices, ni.index)) {
        is_expand_boundary = true;
      }
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    if (is_expand_boundary) {
      BLI_gset_add(initial_vertices, POINTER_FROM_INT(i));
      SCULPT_floodfill_add_initial(&flood, i);
    }
  }

  MEM_SAFE_FREE(expand_cache->falloff_factor);
  MEM_SAFE_FREE(expand_cache->face_falloff_factor);
  MEM_freeN(enabled_vertices);

  float *dists = MEM_calloc_arrayN(sizeof(float), totvert, "topology dist");
  ExpandFloodFillData fdata;
  fdata.dists = dists;

  SCULPT_floodfill_execute(ss, &flood, mask_expand_topology_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  expand_cache->falloff_factor = SCULPT_geodesic_distances_create(ob, initial_vertices, FLT_MAX);

  sculpt_expand_update_max_falloff_factor(ss, expand_cache);

  BLI_gset_free(initial_vertices, NULL);

  if (expand_cache->target == SCULPT_EXPAND_TARGET_FACE_SETS) {
    sculpt_expand_mesh_face_falloff_from_vertex_falloff(ob->data, expand_cache);
    sculpt_expand_update_max_face_falloff_factor(ss, expand_cache);
  }
}

static void sculpt_expand_set_initial_components_for_mouse(bContext *C,
                                                           Object *ob,
                                                           ExpandCache *expand_cache,
                                                           const float mouse[2])
{
  SculptSession *ss = ob->sculpt;
  int initial_vertex = sculpt_expand_target_vertex_update_and_get(C, ob, mouse);
  if (initial_vertex == SCULPT_EXPAND_VERTEX_NONE) {
    /* Cursor not over the mesh, for creating valid initial falloffs, fallback to the last active
     * vertex in the sculpt session. */
    initial_vertex = SCULPT_active_vertex_get(ss);
  }
  copy_v2_v2(ss->expand_cache->initial_mouse, mouse);
  expand_cache->initial_active_vertex = initial_vertex;
  expand_cache->initial_active_face_set = SCULPT_active_face_set_get(ss);
  if (expand_cache->modify_active) {
    expand_cache->next_face_set = SCULPT_active_face_set_get(ss);
  }
  else {
    expand_cache->next_face_set = ED_sculpt_face_sets_find_next_available_id(ob->data);
  }
}

static void sculpt_expand_move_propagation_origin(bContext *C,
                                                  Object *ob,
                                                  const wmEvent *event,
                                                  ExpandCache *expand_cache)
{
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;

  const float mouse[2] = {event->mval[0], event->mval[1]};
  float move_disp[2];
  sub_v2_v2v2(move_disp, mouse, expand_cache->initial_mouse_move);

  float new_mouse[2];
  add_v2_v2v2(new_mouse, move_disp, expand_cache->original_mouse_move);

  sculpt_expand_set_initial_components_for_mouse(C, ob, expand_cache, new_mouse);
  sculpt_expand_falloff_factors_from_vertex_and_symm_create(expand_cache,
                                                            sd,
                                                            ob,
                                                            expand_cache->initial_active_vertex,
                                                            expand_cache->falloff_factor_type);
}

static void sculpt_expand_snap_initialize_from_enabled(SculptSession *ss,
                                                       ExpandCache *expand_cache)
{
  const bool prev_snap_state = expand_cache->snap;
  const bool prev_invert_state = expand_cache->invert;
  expand_cache->snap = false;
  expand_cache->invert = false;

  BLI_bitmap *enabled_vertices = sculpt_expand_bitmap_from_enabled(ss, expand_cache);

  const int totface = ss->totfaces;
  for (int i = 0; i < totface; i++) {
    const int face_set = expand_cache->initial_face_sets[i];
    BLI_gset_add(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(face_set));
  }

  for (int p = 0; p < totface; p++) {
    MPoly *poly = &ss->mpoly[p];
    bool any_disabled = false;
    for (int l = 0; l < poly->totloop; l++) {
      MLoop *loop = &ss->mloop[l + poly->loopstart];
      if (!BLI_BITMAP_TEST(enabled_vertices, loop->v)) {
        any_disabled = true;
      }
    }
    if (any_disabled) {
      const int face_set = expand_cache->initial_face_sets[p];
      if (BLI_gset_haskey(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(face_set))) {
        BLI_gset_remove(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(face_set), NULL);
      }
    }
  }

  MEM_freeN(enabled_vertices);
  expand_cache->snap = prev_snap_state;
  expand_cache->invert = prev_invert_state;
}

static int sculpt_expand_modal(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;

  const float mouse[2] = {event->mval[0], event->mval[1]};
  const int target_expand_vertex = sculpt_expand_target_vertex_update_and_get(C, ob, mouse);

  ExpandCache *expand_cache = ss->expand_cache;
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case SCULPT_EXPAND_MODAL_INVERT: {
        expand_cache->invert = !expand_cache->invert;
        break;
      }
      case SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE: {
        expand_cache->preserve = !expand_cache->preserve;
        break;
      }
      case SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE: {
        expand_cache->falloff_gradient = !expand_cache->falloff_gradient;
        break;
      }
      case SCULPT_EXPAND_MODAL_SNAP_TOGGLE: {
        if (expand_cache->snap) {
          expand_cache->snap = false;
          if (expand_cache->snap_enabled_face_sets) {
            BLI_gset_free(expand_cache->snap_enabled_face_sets, NULL);
            expand_cache->snap_enabled_face_sets = NULL;
          }
        }
        else {
          expand_cache->snap = true;
          if (!expand_cache->snap_enabled_face_sets) {
            expand_cache->snap_enabled_face_sets = BLI_gset_int_new("snap face sets");
          }
          sculpt_expand_snap_initialize_from_enabled(ss, expand_cache);
        }
      } break;
      case SCULPT_EXPAND_MODAL_MOVE_TOGGLE: {
        if (expand_cache->move) {
          expand_cache->move = false;
        }
        else {
          expand_cache->move = true;
          copy_v2_v2(expand_cache->initial_mouse_move, mouse);
          copy_v2_v2(expand_cache->original_mouse_move, expand_cache->initial_mouse);
        }
        break;
      }
      case SCULPT_EXPAND_MODAL_RECURSION_STEP: {
        sculpt_expand_resursion_step_add(ob, expand_cache);
        break;
      }
      case SCULPT_EXPAND_MODAL_CONFIRM: {
        sculpt_expand_update_for_vertex(C, ob, target_expand_vertex);
        sculpt_expand_finish(C);
        return OPERATOR_FINISHED;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_GEODESICS: {
        sculpt_expand_falloff_factors_from_vertex_and_symm_create(
            expand_cache,
            sd,
            ob,
            expand_cache->initial_active_vertex,
            SCULPT_EXPAND_FALLOFF_GEODESICS);
        break;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY: {
        sculpt_expand_falloff_factors_from_vertex_and_symm_create(
            expand_cache,
            sd,
            ob,
            expand_cache->initial_active_vertex,
            SCULPT_EXPAND_FALLOFF_TOPOLOGY);
        break;
      }
      case SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL: {
        sculpt_expand_falloff_factors_from_vertex_and_symm_create(
            expand_cache,
            sd,
            ob,
            expand_cache->initial_active_vertex,
            SCULPT_EXPAND_FALLOFF_SPHERICAL);
        break;
      }
    }
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (expand_cache->move) {
    sculpt_expand_move_propagation_origin(C, ob, event, expand_cache);
  }

  if (expand_cache->snap) {
    const int active_face_set_id = expand_cache->initial_face_sets[ss->active_face_index];
    if (!BLI_gset_haskey(expand_cache->snap_enabled_face_sets,
                         POINTER_FROM_INT(active_face_set_id))) {
      BLI_gset_add(expand_cache->snap_enabled_face_sets, POINTER_FROM_INT(active_face_set_id));
    }
  }

  sculpt_expand_update_for_vertex(C, ob, target_expand_vertex);

  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_expand_delete_face_set_id(
    Mesh *mesh, MeshElemMap *pmap, int *face_sets, const int totface, const int delete_id)
{

  BLI_LINKSTACK_DECLARE(queue, int);
  BLI_LINKSTACK_DECLARE(queue_next, int);

  BLI_LINKSTACK_INIT(queue);
  BLI_LINKSTACK_INIT(queue_next);

  for (int i = 0; i < totface; i++) {
    if (face_sets[i] == delete_id) {
      BLI_LINKSTACK_PUSH(queue, i);
    }
  }

  while (BLI_LINKSTACK_SIZE(queue)) {
    int f_index;
    while (f_index = BLI_LINKSTACK_POP(queue)) {

      int other_id = delete_id;
      const MPoly *c_poly = &mesh->mpoly[f_index];
      for (int l = 0; l < c_poly->totloop; l++) {
        const MLoop *c_loop = &mesh->mloop[c_poly->loopstart + l];
        const MeshElemMap *vert_map = &pmap[c_loop->v];
        for (int i = 0; i < vert_map->count; i++) {

          const int neighbor_face_index = vert_map->indices[i];
          if (face_sets[neighbor_face_index] != delete_id) {
            other_id = face_sets[neighbor_face_index];
          }
        }
      }

      if (other_id != delete_id) {
        face_sets[f_index] = other_id;
      }
      else {
        BLI_LINKSTACK_PUSH(queue_next, f_index);
      }
    }

    BLI_LINKSTACK_SWAP(queue, queue_next);
  }

  BLI_LINKSTACK_FREE(queue);
  BLI_LINKSTACK_FREE(queue_next);
}

static void sculpt_expand_cache_initial_config_set(Sculpt *sd,
                                                   Object *ob,
                                                   ExpandCache *expand_cache,
                                                   wmOperator *op)
{

  expand_cache->invert = RNA_boolean_get(op->ptr, "invert");
  expand_cache->preserve = RNA_boolean_get(op->ptr, "use_mask_preserve");
  expand_cache->falloff_gradient = RNA_boolean_get(op->ptr, "use_falloff_gradient");
  expand_cache->target = RNA_enum_get(op->ptr, "target");
  expand_cache->modify_active = RNA_boolean_get(op->ptr, "use_modify_active");
  expand_cache->expand_from_active = RNA_boolean_get(op->ptr, "use_expand_from_active");

  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);
  copy_v4_fl(expand_cache->fill_color, 1.0f);
  copy_v3_v3(expand_cache->fill_color, BKE_brush_color_get(ss->scene, brush));
  IMB_colormanagement_srgb_to_scene_linear_v3(expand_cache->fill_color);

  expand_cache->blend_mode = brush->blend;
}

static int sculpt_expand_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;

  /* Create and configure the Expand Cache. */
  ss->expand_cache = MEM_callocN(sizeof(ExpandCache), "expand cache");
  sculpt_expand_cache_initial_config_set(sd, ob, ss->expand_cache, op);

  /* Update object. */
  const bool needs_colors = ss->expand_cache->target == SCULPT_EXPAND_TARGET_COLORS;

  if (needs_colors) {
    BKE_sculpt_color_layer_create_if_needed(ob);
    depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  }

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, needs_colors);
  SCULPT_vertex_random_access_ensure(ss);
  SCULPT_boundary_info_ensure(ob);
  SCULPT_undo_push_begin(ob, "expand");

  /* Set the initial element for expand from the event position. */
  const float mouse[2] = {event->mval[0], event->mval[1]};
  sculpt_expand_set_initial_components_for_mouse(C, ob, ss->expand_cache, mouse);

  /* Cache PBVH nodes. */
  BKE_pbvh_search_gather(
      ss->pbvh, NULL, NULL, &ss->expand_cache->nodes, &ss->expand_cache->totnode);

  /* Store initial state. */
  sculpt_expand_initial_state_store(ob, ss->expand_cache);

  if (ss->expand_cache->modify_active) {
    sculpt_expand_delete_face_set_id(ob->data,
                                     ss->pmap,
                                     ss->expand_cache->initial_face_sets,
                                     ss->totfaces,
                                     ss->expand_cache->next_face_set);
  }

  /* Initialize the factors. */
  eSculptExpandFalloffType falloff_type = SCULPT_EXPAND_FALLOFF_GEODESICS;
  if (SCULPT_vertex_is_boundary(ss, ss->expand_cache->initial_active_vertex)) {
    falloff_type = SCULPT_EXPAND_FALLOFF_BOUNDARY_TOPOLOGY;
  }

  sculpt_expand_falloff_factors_from_vertex_and_symm_create(
      ss->expand_cache, sd, ob, ss->expand_cache->initial_active_vertex, falloff_type);

  /* Initial update. */
  sculpt_expand_update_for_vertex(C, ob, ss->expand_cache->initial_active_vertex);

  const char *status_str = TIP_(
      "Move the mouse to expand from the active vertex. LMB: confirm, ESC/RMB: "
      "cancel");
  ED_workspace_status_text(C, status_str);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void sculpt_expand_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {SCULPT_EXPAND_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {SCULPT_EXPAND_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {SCULPT_EXPAND_MODAL_INVERT, "INVERT", 0, "Invert", ""},
      {SCULPT_EXPAND_MODAL_PRESERVE_TOGGLE, "PRESERVE", 0, "Toggle Preserve Previous Mask", ""},
      {SCULPT_EXPAND_MODAL_GRADIENT_TOGGLE, "GRADIENT", 0, "Toggle Gradient", ""},
      {SCULPT_EXPAND_MODAL_RECURSION_STEP,
       "RECURSION_STEP",
       0,
       "Do a recursion step in the falloff from current boundary",
       ""},
      {SCULPT_EXPAND_MODAL_MOVE_TOGGLE, "MOVE_TOGGLE", 0, "Move the origin of the expand", ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_GEODESICS,
       "FALLOFF_GEODESICS",
       0,
       "Move the origin of the expand",
       ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_TOPOLOGY,
       "FALLOFF_TOPOLOGY",
       0,
       "Move the origin of the expand",
       ""},
      {SCULPT_EXPAND_MODAL_FALLOFF_SPHERICAL,
       "FALLOFF_SPHERICAL",
       0,
       "Move the origin of the expand",
       ""},
      {SCULPT_EXPAND_MODAL_SNAP_TOGGLE, "SNAP_TOGGLE", 0, "Snap expand to Face Sets", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const char *name = "Sculpt Expand Modal";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);

  /* This function is called for each spacetype, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_expand");
}

void SCULPT_OT_expand(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Expand";
  ot->idname = "SCULPT_OT_expand";
  ot->description = "Generic sculpt expand operator";

  /* API callbacks. */
  ot->invoke = sculpt_expand_invoke;
  ot->modal = sculpt_expand_modal;
  ot->cancel = sculpt_expand_cancel;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "target",
               prop_sculpt_expand_target_type_items,
               SCULPT_EXPAND_TARGET_FACE_SETS,
               "Data Target",
               "Data that is going to be modified in the expand operation");

  ot->prop = RNA_def_boolean(
      ot->srna, "invert", true, "Invert", "Invert the expand active elements");
  ot->prop = RNA_def_boolean(ot->srna,
                             "use_mask_preserve",
                             false,
                             "Preserve Previous Mask",
                             "Preserve the previous mask");
  ot->prop = RNA_def_boolean(
      ot->srna, "use_falloff_gradient", false, "Falloff Gradient", "Expand Using a Falloff");

  ot->prop = RNA_def_boolean(
      ot->srna, "use_modify_active", true, "Modify Active", "Modify Active");

  ot->prop = RNA_def_boolean(
      ot->srna, "use_expand_from_active", false, "Expand From Active", "Expand From Active");
}