/**************************************************************************/
/*  snow_clumps_3d.h                                                      */
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
#include "modules/ground/ground_lump_draw.h"
#include "modules/ground/ground_lump_scan.h"
#include "modules/ground/ground_lump_store.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class MultiMeshInstance3D;
class SnowSurface3D;
class SnowCover3D;

// Snow clumps (docs/ideas/ground-detail.md, "Snow clumps"): the lumps a boot, a wheel or a sled breaks out of
// cohesive snow and leaves lying beside its print, as real geometry near the camera. Where the print field's rim
// grows (snow pushed up and out of a print, GroundDeformField::press), a share of the pushed volume breaks into
// clumps when the pack is cohesive: settled or moist snow sinters into lumps that hold together, cold powder
// sprays and leaves none (Sumner, O'Brien and Hodgins 1999; granules of snow avalanche debris, Bartelt and McArdell
// 2009: log-normal sizes, larger in warm wet snow). Each clump is one of a few shared shapes, rests on the snow
// surface, shrinks in a thaw and is buried by new snow; the path tracer instances them
// (RenderingServer.multimesh_set_rt_instanced).
class SnowClumps3D : public Node3D {
	GDCLASS(SnowClumps3D, Node3D);

public:
	static constexpr int SHAPES = 8;

private:
	NodePath surface_path;
	bool enabled = true;
	float break_share = 0.25f; // of the snow pushed into a rim, the share that breaks into clumps in cohesive snow
	float radius = 10.0f; // m round the camera that is watched for new rims
	int max_clumps = 30000;
	float update_rate = 6.0f; // Hz
	int seed = 0;

	using Clump = GroundLump;
	LocalVector<Clump> clumps;
	uint32_t next_slot = 0; // ring buffer past max_clumps

	// The print field's rim round the camera, as the last look saw it.
	GroundLumpScan scan;
	LocalVector<GroundLumpScan::Grown> grown;
	double since_scan = 0.0;
	uint64_t seen_cover_update = 0;
	RandomPCG rng;

	Ref<ArrayMesh> shapes[SHAPES];
	GroundLumpDraw lump_draw;
	GroundLumpStore store; // clumps kept in memory while the camera is away (as the print field keeps its tiles)
	LocalVector<GroundLump> back;
	Ref<Material> material;
	bool draw_dirty = false;
	int spawned_total = 0;
	double update_msec_total = 0.0;
	double update_msec_worst = 0.0;
	int update_runs = 0;

	SnowSurface3D *_surface() const;
	SnowCover3D *_cover(SnowSurface3D *p_surface) const;
	void _ensure_draw();
	void _free_draw();
	void _scan(SnowSurface3D *p_surface, SnowCover3D *p_cover);
	bool _spawn(SnowSurface3D *p_surface, SnowCover3D *p_cover, const Vector3 &p_at, const Vector2 &p_out, float p_size, float p_ceiling = 1e30f);
	void _crush(SnowSurface3D *p_surface);
	void _restore(SnowSurface3D *p_surface);
	void _age(SnowSurface3D *p_surface, SnowCover3D *p_cover);
	void _draw();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static Ref<ArrayMesh> make_shape(int p_seed);
	static float cohesion(float p_density, float p_wetness, float p_temperature);

	void set_surface(const NodePath &p_path);
	NodePath get_surface() const { return surface_path; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_break_share(float p_share) { break_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_break_share() const { return break_share; }
	void set_radius(float p_radius) { radius = MAX(p_radius, 1.0f); }
	float get_radius() const { return radius; }
	void set_max_clumps(int p_max);
	int get_max_clumps() const { return max_clumps; }
	void set_seed(int p_seed) { seed = p_seed; }
	int get_seed() const { return seed; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }

	// Scans the print field now and draws (tests and tools).
	void update_now();
	// Snow falling off something (a roof slab, a shaken vehicle, docs/ideas/snow.md follow-up 19): p_volume m3 breaks
	// into clumps lying on the snow within p_radius of p_at, larger in wet snow; returns how many.
	int drop(const Vector3 &p_at, float p_radius, float p_volume, float p_wetness, float p_size_scale = 1.0f, float p_ceiling = 1e30f);
	void clear();
	int get_clump_count() const { return int(clumps.size()); }
	int get_spawned_total() const { return spawned_total; }
	int get_parked_count() const { return store.get_parked_count(); }
	// Main-thread cost of the updates so far (scan, crush, age, draw).
	double get_update_msec_mean() const { return update_runs ? update_msec_total / update_runs : 0.0; }
	double get_update_msec_worst() const { return update_msec_worst; }
	int get_update_runs() const { return update_runs; }

	SnowClumps3D();
	~SnowClumps3D();
};
