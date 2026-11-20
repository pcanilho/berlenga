/**************************************************************************/
/*  physx_water_mist_3d.h                                                 */
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
#include "scene/resources/mesh.h"

class MeshInstance3D;
class ShaderMaterial;

// The sea inside sea caves (docs/ideas/water.md, "Sea-cave water"): CaveNetwork3D carries the sea into each sea cave
// as a long wave along the passage (its surge, the water contract's answer there); this draws that water with the
// river's surface (clear water, sky and sun reflections, white water, the path tracer's refracting medium). A ribbon
// per sea cave along the passage's line: each station's level, half width and current from
// CaveNetwork3D.get_sea_cave_water(), rebuilt every frame. No water state of its own.
class PhysXSeaCaveWater3D : public Node3D {
	GDCLASS(PhysXSeaCaveWater3D, Node3D);

	NodePath cave_network;
	int lanes = 9; // vertices across the passage
	float wall_margin = 0.25f; // m past the passage's half width, into the walls, so no gap shows at the rock
	float white_water_speed = 1.5f; // m/s: the current where white water starts (it is full at twice that)

	struct Ribbon {
		int cave = -1;
		MeshInstance3D *instance = nullptr;
		Ref<ArrayMesh> mesh;
	};
	LocalVector<Ribbon> ribbons;
	Ref<ShaderMaterial> material;
	double water_time = 0.0;

	void _update();
	void _clear();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_cave_network(const NodePath &p_path);
	NodePath get_cave_network() const { return cave_network; }
	void set_lanes(int p_lanes);
	int get_lanes() const { return lanes; }
	void set_wall_margin(float p_margin) { wall_margin = MAX(p_margin, 0.0f); }
	float get_wall_margin() const { return wall_margin; }
	void set_white_water_speed(float p_speed) { white_water_speed = MAX(p_speed, 0.01f); }
	float get_white_water_speed() const { return white_water_speed; }

	int get_ribbon_count() const { return int(ribbons.size()); }
	Ref<ShaderMaterial> get_water_material() const { return material; }
	// Rebuilds the surfaces from the network now (it does so every frame; for tests and stills).
	void update_now() { _update(); }
};
