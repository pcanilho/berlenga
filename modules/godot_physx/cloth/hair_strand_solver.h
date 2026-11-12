/**************************************************************************/
/*  hair_strand_solver.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
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

#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/variant.h"

class RenderingDevice;

// Strand hair on the RenderingDevice (any GPU with compute): see hair_strands.glsl. Built from the
// groom's points (groom space) and strand ranges; stepped once per frame with the head's transform.
// Positions land in an RGBA32F texture the hair material reads directly.
class HairStrandSolverGPU : public RefCounted {
	GDSOFTCLASS(HairStrandSolverGPU, RefCounted);

public:
	static constexpr int MAX_SPHERES = 32;
	static constexpr int MAX_SDF_CELLS = 64 * 64 * 64;
	static constexpr int DOM_CELLS = 256; // deep opacity map, a side
	static constexpr int DOM_SLOTS = 4; // deep opacity maps (lights), at most

	RenderingDevice *rd = nullptr;
	bool local = false;
	SafeFlag built;

	RID shader, pipeline;
	bool owns_shader = false;
	RID buf_params, buf_rest, buf_strands, buf_pos, buf_prev, buf_follow, buf_order, tex_out;
	RID tex_out_prev, buf_last; // last frame's output (motion vectors) and where each point was
	RID buf_dom_depth, buf_dom_accum, tex_dom; // the deep opacity map
	bool history = false; // buf_last holds last frame's output
	RID buf_tris, buf_sdf; // the collision body's triangles and its distance field
	RID buf_vol, buf_vol_rest; // the hair volume grid (density, momentum) and the groom's own density
	RID buf_render_points; // per strand the points drawn: (first, count), then the indices
	RID buf_coll_cells, buf_coll_delta, buf_point_strand; // strand-strand collision: spatial hash, corrections, each point's strand
	RID tex_cloth; // a cloth's simulated positions (not owned), or tex_cloth_dummy
	RID tex_cloth_dummy, buf_cloth_tris; // the cloth's triangles (three particles each)
	int cloth_tri_count = 0;
	int coll_buckets = 0;
	int vol_cells = 0; // a side, 0: no volume grid
	int tris_capacity = 0; // triangles buf_tris holds
	RID uniform_set;

	int strand_count = 0;
	int point_count = 0;
	int tex_width = 0;
	int tex_height = 0;
	int slot_points = 0; // > 0: the output is in draw order, this many texels per strand
	int sort_size = 1;
	bool needs_reset = true;
	bool sorted = false; // the draw order has been sorted once since the build (cards have their slots)

	void rt_compile(Ref<HairStrandSolverGPU> p_self);
	void rt_build(Ref<HairStrandSolverGPU> p_self, PackedFloat32Array p_rest, PackedInt32Array p_strands, PackedFloat32Array p_follow, int p_slot_points, PackedInt32Array p_render_points, PackedFloat32Array p_vol_rest, int p_vol_cells);
	void rt_step(Ref<HairStrandSolverGPU> p_self, PackedByteArray p_params, int p_substeps, PackedFloat32Array p_tris);
	void rt_make_uniform_set();
	void rt_reset(Ref<HairStrandSolverGPU> p_self);
	void rt_hold(Ref<HairStrandSolverGPU> p_self);
	void rt_drop_history(Ref<HairStrandSolverGPU> p_self);
	void rt_set_cloth(Ref<HairStrandSolverGPU> p_self, RID p_texture, PackedInt32Array p_tris);
	void rt_free_buffers();
	void rt_free(Ref<HairStrandSolverGPU> p_self);

	static void free_shared();
};

class HairStrandSolver {
	Ref<HairStrandSolverGPU> gpu;
	void _dispatch(const Callable &p_call) const;
	Transform3D last_xform;
	bool has_last = false;
	int cloth_tris = 0;
	double time = 0.0;
	// The hair volume grid, groom space: its corner, cell and the groom's mean density.
	Vector3 vol_origin;
	float vol_cell = 0.0f;
	float vol_mean = 0.0f;
	int vol_cells = 0;

public:
	struct Settings {
		// AMD TressFX 4.1's parameters and defaults, in metres and seconds.
		float damping = 0.08f; // velocity decay toward the air's, exp(-damping * 60 dt)
		float global_stiffness = 0.1f; // pull toward the groomed pose, per 1/60 s
		float global_range = 0.3f; // share of the strand, from the root, that global shape acts on
		float local_stiffness = 0.9f; // keeps each segment's groomed angle to its parent (0..1)
		int local_iterations = 2;
		int length_iterations = 2;
		float vsp_coeff = 0.8f; // share of the head's motion that carries the strand along (VSP)
		float vsp_accel_threshold = 50.0f; // m/s^2: root acceleration above which VSP is total
		float max_speed = 12.0f; // m/s, speed clamp
		Vector3 gravity = Vector3(0, -9.8f, 0);
		Vector3 wind; // air velocity, m/s
		float gust = 0.3f; // 0..1, how much the wind rises and falls per strand
		float turbulence = 0.35f; // 0..1, eddies in the wind as a share of its speed
		float turbulence_size = 0.08f; // m, eddy size
		float lod_keep = 1.0f; // share of the strands the rasterizer draws (distance LOD), 0..1
		float margin = 0.002f; // kept between strands and the collision spheres (m)
		Vector<Vector4> spheres; // world centre, radius
		Transform3D output_xform; // world -> the render copy's space
		Transform3D view_xform; // world -> the camera, for the draw order
		PackedVector3Array body_triangles; // the collision body, world: three points per triangle
		float body_cell_size = 0.01f; // m, the body's distance field cell (grown to fit 64^3)
		bool volume_enabled = true; // hair-hair interaction through a density grid (Petrovic 2005)
		float volume_friction = 0.2f; // pull toward the neighbours' mean velocity, per 1/60 s
		float volume_stiffness = 1.0f; // push out of hair packed denser than the groom
		float collision_radius = 0.0f; // strand-strand collision between guides: half the distance kept (m); 0: off
		float collision_friction = 0.3f; // share of the sliding between touching guides taken off
		float wet = 0.0f; // 0 dry .. 1 soaked: heavier, softer, flatter (Ward et al. 2004)
		float wet_clumping = 0.0f; // how far wet followers gather onto their guides (0..1)
		// Hair cards: the strands from card_first on are the cards' (followers drawn by the card
		// mesh); 0: none. cards_only: only the guides and the card strands run (no other
		// followers, no draw-order sort after the first, only the cards' slots written).
		int card_first = 0;
		bool cards_only = false;
		// A cloth the hair rests on (set_cloth gives its texture and triangles): its texture's
		// space -> world, its world bounds, its texture's width and its thickness (m).
		Transform3D cloth_xform;
		AABB cloth_bounds;
		int cloth_texture_width = 1;
		float cloth_thickness = 0.0f;
		// Deep opacity maps (Yuksel and Keyser 2008), one per light, up to DOM_SLOTS; none: off.
		struct DeepShadowLight {
			Vector3 center; // world, the hair's bounding sphere
			float radius = 0.0f;
			Vector3 light; // world: toward the light (directional), or the light's position (local)
			bool local = false; // an omni or spot light
		};
		LocalVector<DeepShadowLight> dom_lights;
		float root_width = 0.0004f; // m, the strands' width, for their coverage
		float tip_width = 0.0001f;
	};

	bool is_available() const { return gpu.is_valid() && gpu->rd != nullptr; }

	// p_rest: 4 floats per point (groom-space position, length of the segment from the previous
	// point). p_strands: 2 ints per strand (first point, point count). p_follow: 8 floats per strand:
	// up to three guide strands (x, y, z; w -1 for a guide, which simulates) and their weights.
	// p_slot_points > 0: the output texture holds the strands sorted far to near from the camera
	// (for blending), p_slot_points texels each (padded past a strand's end, w = -1; otherwise w is
	// the share of the strand's points). 0: one texel per point, in groom order.
	// p_render_points (sorted output only): per strand (first, count) into the list that follows
	// them, the strand's points drawn (local indices, root to tip); empty: every point.
	void build(const PackedFloat32Array &p_rest, const PackedInt32Array &p_strands, const PackedFloat32Array &p_follow, int p_slot_points = 0, const PackedInt32Array &p_render_points = PackedInt32Array());
	// p_xform: groom space -> world this frame.
	void step(const Transform3D &p_xform, double p_delta, int p_substeps, const Settings &p_settings);
	void reset();
	// The hair stops moving (the simulation is paused): last frame's positions become this
	// frame's, so the motion vectors fall to zero.
	void hold();
	// The next frame writes no motion (its previous positions are its own): after the strands
	// were not drawn for a while (hair cards), so stale history gives no false motion.
	void drop_history();
	// A PhysXSkinnedCloth3D the hair collides with (its simulated triangles join the collision
	// body's distance field each frame); an invalid texture or no triangles: none.
	void set_cloth(RID p_texture, const PackedInt32Array &p_tris);
	int get_cloth_triangle_count() const { return cloth_tris; }

	RID get_position_texture_rd_rid() const;
	RID get_previous_position_texture_rd_rid() const;
	// The deep opacity maps, one row of DOM_CELLS per light (in Settings::dom_lights' order): per
	// row DOM_CELLS x DOM_CELLS cells (first hair's depth, coverage summed to the end of each of
	// three layers), then as many more (the first opaque depth).
	RID get_deep_shadow_texture_rd_rid() const;
	// A map's axes for a light direction (world, toward the light), as step() builds them.
	static void deep_shadow_axes(const Vector3 &p_light, Vector3 &r_u, Vector3 &r_v);
	// A map as step() lays it out for the shader: 4 vec4 (see hair_strands.glsl's dom_slot), or
	// false when the light cannot hold one (a local light inside the hair's bounding sphere).
	static bool deep_shadow_slot(const Settings::DeepShadowLight &p_light, float r_slot[16]);
	static Vector3 deep_shadow_layers() { return Vector3(0.01f, 0.04f, 0.15f); }
	int get_texture_width() const;

	HairStrandSolver();
	~HairStrandSolver();
};
