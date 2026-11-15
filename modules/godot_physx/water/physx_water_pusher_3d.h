/**************************************************************************/
/*  physx_water_pusher_3d.h                                               */
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

#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"

// Bodies pushing water (docs/ideas/water.md, "Bodies pushing water"): a character's limbs or a body's shapes as
// spheres that push the water out of their way, as FluidNinja's bone and mesh interaction does. Each physics
// frame the spheres in or near the water go to every water body nearby: the live water areas take them as
// displaced volume (the water flows round and is lifted ahead of them), seas, lakes and rivers press their live
// layer under them, harder the faster they move, so a wading leg or a hand sweeping through leaves waves.
class PhysXWaterPusher3D : public Node3D {
	GDCLASS(PhysXWaterPusher3D, Node3D);

public:
	enum Source {
		SOURCE_AUTO, // the parent: bones of a Skeleton3D, else shapes of a CollisionObject3D
		SOURCE_SKELETON,
		SOURCE_SHAPES,
	};

private:
	Source source = SOURCE_AUTO;
	NodePath target; // the Skeleton3D or CollisionObject3D (empty: the parent)
	float bone_radius = 0.08f; // m, the limbs' thickness
	int max_spheres = 16;
	float strength = 1.0f;
	float rest_strength = 0.3f; // share of the push a still sphere gives the seas' live layer
	float reference_speed = 2.0f; // m/s at which the push is full
	bool enabled = true;

	struct Sphere {
		Vector3 position;
		float radius = 0.0f;
	};
	LocalVector<Sphere> spheres;
	LocalVector<Vector3> previous; // last frame's positions, for speed
	LocalVector<ObjectID> waters;
	double water_scan_timer = 0.0;
	int submitted = 0; // spheres submitted last frame (slots to clear)

	void _gather();
	void _gather_skeleton(Node *p_node);
	void _gather_shapes(Node *p_node);
	void _scan_waters();
	void _push(double p_dt);
	void _clear_all();
	int64_t _owner(int p_index) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_source(Source p_source) { source = p_source; }
	Source get_source() const { return source; }
	void set_target(const NodePath &p_path) { target = p_path; }
	NodePath get_target() const { return target; }
	void set_bone_radius(float p_radius) { bone_radius = MAX(p_radius, 0.005f); }
	float get_bone_radius() const { return bone_radius; }
	void set_max_spheres(int p_count) { max_spheres = CLAMP(p_count, 1, 48); }
	int get_max_spheres() const { return max_spheres; }
	void set_strength(float p_strength) { strength = MAX(p_strength, 0.0f); }
	float get_strength() const { return strength; }
	void set_rest_strength(float p_strength) { rest_strength = CLAMP(p_strength, 0.0f, 1.0f); }
	float get_rest_strength() const { return rest_strength; }
	void set_reference_speed(float p_speed) { reference_speed = MAX(p_speed, 0.01f); }
	float get_reference_speed() const { return reference_speed; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }

	// The spheres as last gathered (world centre, radius in w).
	PackedVector4Array get_spheres() const;
	int get_submitted_count() const { return submitted; }
};

VARIANT_ENUM_CAST(PhysXWaterPusher3D::Source);
