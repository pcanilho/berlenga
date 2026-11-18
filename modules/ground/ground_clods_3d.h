/**************************************************************************/
/*  ground_clods_3d.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/


#pragma once

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "ground_lump_draw.h"
#include "ground_lump_scan.h"
#include "ground_lump_store.h"
#include "ground_surface_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;

// Clods (docs/ideas/ground-detail.md, "Mud clods"): lumps of clay standing on the ground near the camera, as real
// geometry. Two sources: clay thrown by tyres and boots that lands as a lump (GroundSurface3D::track_mud with no
// print: the bigger landings stand up instead of lying as a film), and lumps torn out of a rut's rim where a tyre or
// a boot heaves sticky soil (the print field's rim grows; plastic clay holds together, sand crumbles and slurry
// flows: the soil's stickiness, docs/ideas/mud.md stage 3). Sizes log-normal (soil aggregates, Gardner 1956).
// Each clod is one of a few shared angular shapes in its clay's colour; a later print crushes it, standing water
// slakes it (Emerson 1967). The path tracer instances them (RenderingServer.multimesh_set_rt_instanced).
class GroundClods3D : public Node3D {
	GDCLASS(GroundClods3D, Node3D);

public:
	static constexpr int SHAPES = 8;

private:
	NodePath surface_path;
	bool enabled = true;
	float break_share = 0.2f; // of the soil heaved into a rim, the share torn out as clods in fully sticky soil
	float radius = 12.0f; // m round the camera that is watched
	int max_clods = 30000;
	float update_rate = 6.0f; // Hz for the rim scan and the checks; landings are taken every frame
	int seed = 0;

	using Clod = GroundLump;
	LocalVector<Clod> clods;
	uint32_t next_slot = 0;

	GroundLumpScan scan;
	LocalVector<GroundLumpScan::Grown> grown;
	double since_scan = 0.0;
	int water_wait = 0;
	RandomPCG rng;
	LocalVector<GroundSurface3D::ClodDrop> drops;

	Ref<ArrayMesh> shapes[SHAPES];
	GroundLumpDraw lump_draw;
	GroundLumpStore store; // clods kept while the camera is away (with the kept ruts)
	LocalVector<GroundLump> back;
	Ref<Material> material;
	bool draw_dirty = false;
	int spawned_total = 0;
	int landed_total = 0;
	double update_msec_total = 0.0;
	double update_msec_worst = 0.0;
	int update_runs = 0;

	GroundSurface3D *_surface() const;
	void _ensure_draw();
	void _free_draw();
	void _land(GroundSurface3D *p_surface);
	void _watch(GroundSurface3D *p_surface);
	void _scan(GroundSurface3D *p_surface);
	void _spawn(GroundSurface3D *p_surface, const Vector3 &p_at, const Vector2 &p_out, float p_size, float p_throw, const Color &p_color);
	void _check(GroundSurface3D *p_surface, bool p_water);
	void _draw();
	void _remove(uint32_t p_index);
	void _restore(GroundSurface3D *p_surface);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Ref<ArrayMesh> make_shape(int p_seed);

	void set_surface(const NodePath &p_path);
	NodePath get_surface() const { return surface_path; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_break_share(float p_share) { break_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_break_share() const { return break_share; }
	void set_radius(float p_radius) { radius = MAX(p_radius, 1.0f); }
	float get_radius() const { return radius; }
	void set_max_clods(int p_max);
	int get_max_clods() const { return max_clods; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }

	void update_now();
	void clear();
	int get_clod_count() const { return int(clods.size()); }
	int get_spawned_total() const { return spawned_total; }
	int get_landed_total() const { return landed_total; }
	int get_parked_count() const { return store.get_parked_count(); }
	double get_update_msec_mean() const { return update_runs ? update_msec_total / update_runs : 0.0; }
	double get_update_msec_worst() const { return update_msec_worst; }
	int get_update_runs() const { return update_runs; }

	GroundClods3D();
	~GroundClods3D();
};
