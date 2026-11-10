/**************************************************************************/
/*  snow_surface_3d.h                                                         */
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


#pragma once

#include "core/templates/hash_map.h"
#include "modules/ground/ground_deform_field.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/3d/height_map_shape_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class CollisionShape3D;
class MeshInstance3D;
class SnowCover3D;
class StaticBody3D;

// Deep snow as geometry (docs/ideas/snow.md, stage 2). A camera-following grid, dense near the
// middle and coarser outward, is lifted in its vertex shader to the ground plus the SnowCover3D
// pack depth, so the snow has real volume in raster and under path tracing (displaced surfaces
// are traced from their capture, docs/ideas/displaced-hit-varyings.md). Thin snow (under
// min_depth) stays a material effect and the surface tucks under the ground there.
//
// Deformation, after Batman: Arkham Origins (GDC 2014), Rise of the Tomb Raider (SIGGRAPH 2015)
// and God of War Ragnarok (GDC 2023): a camera-following field of dent and rim heights. Unlike
// those, the field is mass conserving: a pressed column keeps (1 - rim_share) of what it loses
// as compaction, up to compacted_density, and pushes rim_share sideways into a rim around the
// contact. Bodies sink by Bekker's pressure-sinkage relation z = (p / (kc / b + kphi))^(1/n)
// (Bekker 1969, snow values from Wong, Theory of Ground Vehicles), and a heightfield collision
// patch follows the deformed surface so they rest in their own tracks. Trails refill as new
// snow settles into hollows and wind drifts over them, and shrink as the pack around them
// settles toward the compacted density. Tiles leaving the field are kept in a bounded cache and
// come back when the camera returns.
class SnowSurface3D : public Node3D {
	GDCLASS(SnowSurface3D, Node3D);

	static constexpr int TILE = GroundDeformField::TILE; // deformation texels per cached tile side

	NodePath snow_cover;
	NodePath follow_target;
	float extent = 96.0f;
	float detail_spacing = 0.125f;
	float max_spacing = 2.0f;
	float ground_spacing = 0.25f;
	float min_depth = 0.03f;
	float edge_fade = 8.0f;
	float micro_relief = 1.0f;
	Color tint = Color(1, 1, 1);

	bool deform_enabled = true;
	float deform_extent = 32.0f;
	int deform_resolution = 512;
	PackedStringArray deform_groups;
	float compacted_density = 450.0f; // kg/m3 under a boot or a wheel
	float rim_share = 0.3f;
	float contact_tolerance = 0.03f;
	float bekker_n = 1.6f;
	float bekker_kc = 4.37f; // kN/m^(n+1)
	float bekker_kphi = 196.72f; // kN/m^(n+2)
	float character_mass = 75.0f;
	float character_contact_area = 0.035f; // m2, one boot
	float refill_share = 0.5f;
	float wind_fill_rate = 0.5f; // per weather hour at full drift
	int max_cached_tiles = 1024;

	bool collision_enabled = true;
	float collision_extent = 16.0f;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	float collision_rate = 15.0f;

	// Ground, sampled from the cover's height source on a lattice.
	int ground_n = 0;
	Vector2i ground_origin; // lattice index of the first sample
	bool ground_valid = false;
	LocalVector<float> ground;
	Ref<Image> ground_image;
	Ref<ImageTexture> ground_texture;
	float ground_min = 0.0f;
	float ground_max = 0.0f;

	// Deformation field (shared with GroundSurface3D, modules/ground).
	GroundDeformField field;
	struct SnowColumns : public GroundDeformField::Columns {
		const SnowSurface3D *surface = nullptr;
		const SnowCover3D *cover = nullptr;
		GroundDeformField::Column column(float p_x, float p_z) const override;
		float sinkage(const GroundDeformField::Body &p_body) const override;
	};
	bool collision_dirty = true;
	Ref<Image> deform_image;
	Ref<ImageTexture> deform_texture;
	uint64_t seen_cover_update = 0;
	double collision_accum = 0.0;

	MeshInstance3D *mesh_instance = nullptr;
	Ref<ArrayMesh> mesh;
	Ref<ShaderMaterial> material;
	Ref<Shader> shader;
	StaticBody3D *body = nullptr;
	CollisionShape3D *collision_node = nullptr;
	Ref<HeightMapShape3D> collision_shape;
	Vector2 mesh_center;
	bool mesh_dirty = true;
	double step_msec = 0.0;

	SnowCover3D *_cover() const;
	Vector3 _follow_position() const;
	float _texel() const { return deform_extent / deform_resolution; }
	void _build_mesh();
	void _ensure_children();
	void _follow(Vector3 p_center);
	void _shift_ground(Vector2i p_origin);
	void _shift_deform(Vector2i p_origin);
	float _ground_at(float p_x, float p_z) const;
	float _dent_max(float p_depth, float p_density) const;
	float _press(Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round);
	void _press_movers(double p_delta);
	void _relax();
	void _update_collision();
	void _upload_deform();
	void _upload_ground();
	void _update_material();
	float _surface_at(float p_x, float p_z, const SnowCover3D *p_cover) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_snow_cover(const NodePath &p_path);
	NodePath get_snow_cover() const { return snow_cover; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_extent(float p_extent);
	float get_extent() const { return extent; }
	void set_detail_spacing(float p_spacing);
	float get_detail_spacing() const { return detail_spacing; }
	void set_max_spacing(float p_spacing);
	float get_max_spacing() const { return max_spacing; }
	void set_ground_spacing(float p_spacing);
	float get_ground_spacing() const { return ground_spacing; }
	void set_min_depth(float p_depth);
	float get_min_depth() const { return min_depth; }
	void set_edge_fade(float p_m);
	float get_edge_fade() const { return edge_fade; }
	void set_micro_relief(float p_scale);
	float get_micro_relief() const { return micro_relief; }
	void set_tint(const Color &p_tint);
	Color get_tint() const { return tint; }

	void set_deform_enabled(bool p_enabled);
	bool is_deform_enabled() const { return deform_enabled; }
	void set_deform_extent(float p_extent);
	float get_deform_extent() const { return deform_extent; }
	void set_deform_resolution(int p_resolution);
	int get_deform_resolution() const { return deform_resolution; }
	void set_deform_groups(const PackedStringArray &p_groups) { deform_groups = p_groups; }
	PackedStringArray get_deform_groups() const { return deform_groups; }
	void set_compacted_density(float p_density) { compacted_density = CLAMP(p_density, 100.0f, 917.0f); }
	float get_compacted_density() const { return compacted_density; }
	void set_rim_share(float p_share) { rim_share = CLAMP(p_share, 0.0f, 0.9f); }
	float get_rim_share() const { return rim_share; }
	void set_contact_tolerance(float p_m) { contact_tolerance = MAX(p_m, 0.0f); }
	float get_contact_tolerance() const { return contact_tolerance; }
	void set_bekker_n(float p_n) { bekker_n = CLAMP(p_n, 0.3f, 3.0f); }
	float get_bekker_n() const { return bekker_n; }
	void set_bekker_kc(float p_k) { bekker_kc = MAX(p_k, 0.0f); }
	float get_bekker_kc() const { return bekker_kc; }
	void set_bekker_kphi(float p_k) { bekker_kphi = MAX(p_k, 0.001f); }
	float get_bekker_kphi() const { return bekker_kphi; }
	void set_character_mass(float p_kg) { character_mass = MAX(p_kg, 0.0f); }
	float get_character_mass() const { return character_mass; }
	void set_character_contact_area(float p_m2) { character_contact_area = MAX(p_m2, 0.001f); }
	float get_character_contact_area() const { return character_contact_area; }
	void set_refill_share(float p_share) { refill_share = CLAMP(p_share, 0.0f, 1.0f); }
	float get_refill_share() const { return refill_share; }
	void set_wind_fill_rate(float p_rate) { wind_fill_rate = MAX(p_rate, 0.0f); }
	float get_wind_fill_rate() const { return wind_fill_rate; }
	void set_max_cached_tiles(int p_tiles) { max_cached_tiles = MAX(p_tiles, 0); }
	int get_max_cached_tiles() const { return max_cached_tiles; }

	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_extent(float p_extent);
	float get_collision_extent() const { return collision_extent; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_rate(float p_rate) { collision_rate = MAX(p_rate, 0.5f); }
	float get_collision_rate() const { return collision_rate; }

	// Presses the snow down to p_bottom (world height of the body's underside) within p_radius of
	// p_position (a bowl for round bodies, flat for boxes), plus p_sink of Bekker sinkage below the
	// undisturbed surface. Returns the snow volume moved, m3.
	float press(Vector3 p_position, float p_radius, float p_bottom, float p_sink = 0.0f, bool p_round = true);
	// The same with a flat rectangular print p_size wide (x) and long (z), turned by p_yaw about up:
	// a boot, a sled runner, a crate.
	float press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink = 0.0f);
	static float bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi); // m
	void clear_deformation();
	void refresh_ground();

	float get_surface_height_at(Vector3 p_position) const;
	float get_dent_at(Vector3 p_position) const;
	float get_rim_at(Vector3 p_position) const;
	Vector2 get_moved_volume() const; // m3: (dent, rim) over the live field
	int get_cached_tile_count() const { return field.cached_tile_count(); }
	Rect2 get_deform_rect() const;
	double get_step_time_msec() const { return step_msec; }
	Ref<ShaderMaterial> get_surface_material() const { return material; }

	SnowSurface3D();
	~SnowSurface3D();
};
