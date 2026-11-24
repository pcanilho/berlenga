/**************************************************************************/
/*  road_relief_3d.h                                                      */
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

#include "core/io/image.h"
#include "scene/3d/mesh_instance_3d.h"

class StaticBody3D;

// A used asphalt street's relief as real geometry (docs/ideas/street-water.md): crown and gutters, wheel-path ruts
// growing with traffic, ISO 8608 roughness, settlement sags, birdbath depressions, utility patches and potholes, all
// functions of world position so no two streets or two ends of one repeat and adjacent pieces join seamlessly. The
// road lies in the node's frame: X along (0..length), Z across (-width/2..width/2), Y up. It builds the displaced
// mesh (raster, path tracer and water see the same bed), an optional static collider of it, and answers
// get_height_at() from the same function.
class RoadRelief3D : public MeshInstance3D {
	GDCLASS(RoadRelief3D, MeshInstance3D);

public:
	enum RoughnessClass {
		ROUGHNESS_A, // very good, Gd(n0) = 16e-6 m3
		ROUGHNESS_B, // good, 64e-6
		ROUGHNESS_C, // average, 256e-6
		ROUGHNESS_NONE, // none (a height_map that has its own)
	};

private:
	float length = 20.0f; // m along X
	float width = 7.0f; // m kerb to kerb
	int lanes = 2;
	bool one_way = false; // a single cross slope falling to +Z instead of a crown
	float cross_slope = 0.02f; // rise over run, crown to kerb (AASHTO 1.5 to 2 %)
	float gutter_width = 0.3f; // m
	float gutter_drop = 0.025f; // m below the cross slope at the kerb
	float traffic = 0.5f; // 0 a quiet lane .. 1 a bus route: rut depth's share of rut_depth
	float rut_depth = 0.018f; // m, at full traffic
	float track = 1.8f; // m between a lane's wheel paths
	RoughnessClass roughness_class = ROUGHNESS_B;
	float sag_depth = 0.008f; // m, settlement sags' amplitude
	float depression_density = 1.5f; // birdbaths per 100 m2
	float depression_depth = 0.015f; // m, deepest
	float patch_density = 0.15f; // per 100 m2
	float pothole_density = 0.02f; // per 100 m2
	float pothole_depth = 0.045f; // m
	int seed = 0;
	float cell = 0.08f; // m, the mesh's grid
	bool collision = true;
	uint32_t collision_layer = 1;
	// Storm drains: an inlet (a grate's recess in the gutter pan) every inlet_spacing metres along each gutter, at
	// places hashed from the world, or where inlets says (x along the road, y the side: -1 the -Z kerb, +1 the +Z).
	float inlet_spacing = 30.0f; // m, 0 none
	// The space the relief is a function of: the world, or this node's (a city strip that slides past).
	NodePath relief_frame;
	Transform3D built_xform;
	Transform3D _relief_xform() const;
	PackedVector2Array inlets;
	// Heights (m) a caller adds over the whole road, x along 0..length over its width, y across from the -Z kerb to the
	// +Z one over its height: a street's own relief from elsewhere (its own potholes, manholes, gutter grades).
	Ref<Image> height_map;
	LocalVector<float> hm;
	int hm_w = 0;
	int hm_h = 0;
	float _height_map_at(float p_x, float p_z) const;
	LocalVector<Vector2> inlet_at; // resolved: x along, y side
	void _resolve_inlets();
	float _inlet_dip(float p_x, float p_z) const;

	bool dirty = true;
	StaticBody3D *body = nullptr;

	// The built mesh's heights (node frame), for get_surface_height_at().
	LocalVector<float> grid;
	int grid_nx = 0;
	int grid_nz = 0;
	float grid_dx = 1.0f;
	float grid_dz = 1.0f;
	float _grid_height(float p_x, float p_z) const;
	// Where water stands when the relief is full (Priority-Flood, Barnes, Lehman and Mulla 2014, outlets the road's
	// ends and its inlets): per grid point the depth over the bed; and which grid points are inlets.
	LocalVector<float> pond;
	LocalVector<uint8_t> inlet_mask;
	void _flood(const LocalVector<float> &p_h, int p_nx, int p_nz, float p_dx, float p_dz);
	bool _grid_point(const Vector3 &p_world, float &r_u, float &r_v) const;

	// Spectral roughness: cosines in world x, z.
	struct Wave {
		float kx, kz; // radians per metre
		float amp; // m
		float phase;
	};
	LocalVector<Wave> waves;
	void _build_waves();
	void _queue_build();
	void _build();

	float _sag(float p_wx, float p_wz) const;
	float _features(float p_wx, float p_wz, const Basis &p_to_road) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The relief at a point of the road's frame (x along, z across, metres), and the height of the surface there in
	// the node's frame. Outside the road: the kerb-side value, clamped.
	float local_height(float p_x, float p_z) const;
	// The road's surface height at a world point (y of the surface above the point's x, z), or -INF off the road.
	float get_height_at(const Vector3 &p_world) const;
	// The same, read from the built mesh (its triangles, as drawn and as the collider has them): far cheaper, for
	// anything sampling many points (StreetWater3D's bed). -INF off the road or before the first build.
	float get_surface_height_at(const Vector3 &p_world) const;
	// The depth of water the relief holds at a world point when full (its hollows filled to their spill points, the
	// gutters draining to the inlets and the road's ends), 0 off the road or before the first build.
	float get_pond_depth_at(const Vector3 &p_world) const;
	// Whether a world point lies over an inlet's grate (water reaching it leaves the street).
	bool is_inlet_at(const Vector3 &p_world) const;
	bool contains(const Vector3 &p_world) const;
	void rebuild();

	void set_length(float p_v);
	float get_length() const { return length; }
	void set_width(float p_v);
	float get_width() const { return width; }
	void set_lanes(int p_v);
	int get_lanes() const { return lanes; }
	void set_one_way(bool p_v);
	bool is_one_way() const { return one_way; }
	void set_cross_slope(float p_v);
	float get_cross_slope() const { return cross_slope; }
	void set_gutter_width(float p_v);
	float get_gutter_width() const { return gutter_width; }
	void set_gutter_drop(float p_v);
	float get_gutter_drop() const { return gutter_drop; }
	void set_traffic(float p_v);
	float get_traffic() const { return traffic; }
	void set_rut_depth(float p_v);
	float get_rut_depth() const { return rut_depth; }
	void set_track(float p_v);
	float get_track() const { return track; }
	void set_roughness_class(RoughnessClass p_v);
	RoughnessClass get_roughness_class() const { return roughness_class; }
	void set_sag_depth(float p_v);
	float get_sag_depth() const { return sag_depth; }
	void set_depression_density(float p_v);
	float get_depression_density() const { return depression_density; }
	void set_depression_depth(float p_v);
	float get_depression_depth() const { return depression_depth; }
	void set_patch_density(float p_v);
	float get_patch_density() const { return patch_density; }
	void set_pothole_density(float p_v);
	float get_pothole_density() const { return pothole_density; }
	void set_pothole_depth(float p_v);
	float get_pothole_depth() const { return pothole_depth; }
	void set_seed(int p_v);
	int get_seed() const { return seed; }
	void set_cell(float p_v);
	float get_cell() const { return cell; }
	void set_collision(bool p_v);
	bool has_collision() const { return collision; }
	void set_collision_layer(uint32_t p_v);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_inlet_spacing(float p_v);
	float get_inlet_spacing() const { return inlet_spacing; }
	void set_relief_frame(const NodePath &p_v);
	NodePath get_relief_frame() const { return relief_frame; }
	void set_inlets(const PackedVector2Array &p_v);
	PackedVector2Array get_inlets() const { return inlets; }
	void set_height_map(const Ref<Image> &p_v);
	Ref<Image> get_height_map() const { return height_map; }
};

VARIANT_ENUM_CAST(RoadRelief3D::RoughnessClass);
