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

	RenderingDevice *rd = nullptr;
	bool local = false;
	SafeFlag built;

	RID shader, pipeline;
	bool owns_shader = false;
	RID buf_params, buf_rest, buf_strands, buf_pos, buf_prev, tex_out;
	RID uniform_set;

	int strand_count = 0;
	int point_count = 0;
	int tex_width = 0;
	int tex_height = 0;
	bool needs_reset = true;

	void rt_compile(Ref<HairStrandSolverGPU> p_self);
	void rt_build(Ref<HairStrandSolverGPU> p_self, PackedFloat32Array p_rest, PackedInt32Array p_strands);
	void rt_step(Ref<HairStrandSolverGPU> p_self, PackedByteArray p_params, int p_substeps);
	void rt_reset(Ref<HairStrandSolverGPU> p_self);
	void rt_free_buffers();
	void rt_free(Ref<HairStrandSolverGPU> p_self);

	static void free_shared();
};

class HairStrandSolver {
	Ref<HairStrandSolverGPU> gpu;
	void _dispatch(const Callable &p_call) const;
	Transform3D last_xform;
	bool has_last = false;
	double time = 0.0;

public:
	struct Settings {
		float damping = 0.5f; // fraction of velocity lost per second
		float root_follow = 0.3f; // pull toward the groomed pose at the root, per 1/60 s
		float tip_follow = 0.0f; // the same at the tip
		float shape_stiffness = 0.5f; // pull toward each segment's groomed bend, per 1/60 s
		float length_damping = 0.9f; // DFTL: share of each length correction taken back from the parent
		Vector3 gravity = Vector3(0, -9.8f, 0);
		Vector3 wind; // m/s^2, added to gravity
		float gust = 0.3f; // 0..1, how much the wind rises and falls per strand
		float margin = 0.002f; // kept between strands and the collision spheres (m)
		Vector<Vector4> spheres; // world centre, radius
		Transform3D output_xform; // world -> the render copy's space
	};

	bool is_available() const { return gpu.is_valid() && gpu->rd != nullptr; }

	// p_rest: 4 floats per point (groom-space position, length of the segment from the previous
	// point). p_strands: 2 ints per strand (first point, point count).
	void build(const PackedFloat32Array &p_rest, const PackedInt32Array &p_strands);
	// p_xform: groom space -> world this frame.
	void step(const Transform3D &p_xform, double p_delta, int p_substeps, const Settings &p_settings);
	void reset();

	RID get_position_texture_rd_rid() const;
	int get_texture_width() const;

	HairStrandSolver();
	~HairStrandSolver();
};
