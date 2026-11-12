/**************************************************************************/
/*  ground_surface_3d.h                                                   */
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

#include "ground_deform_field.h"
#include "ground_height_query.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

class MeshInstance3D;
class WeatherMap3D;

// Footprints, ruts and dents in bare ground (docs/ideas/ground-deform.md): sand, soil and mud, and the
// leaf litter over them, pressed by bodies in deform_groups with Bekker sinkage (Wong's soil values),
// mass conserving, with walls slumping to the material's angle of repose (Sumner, O'Brien and Hodgins
// 1999). The ground's moisture from the weather map turns soil to mud and dry sand to damp sand; tree
// crowns drop litter; bare rock does not print. The field is drawn as a patch with the ground's own
// material, and the terrain, drawn with the same material (BaseMaterial3D ground_deform_enabled, or
// get_ground_deform_shader_code() in a shader), is lowered under it, so both renderers trace the prints.
class GroundSurface3D : public Node3D {
	GDCLASS(GroundSurface3D, Node3D);

public:
	enum GroundType {
		GROUND_SAND,
		GROUND_SOIL,
	};

	// What a lattice cell of ground is made of, from the rules (moisture, sand share, litter, rock).
	struct Cell {
		float n = 1.0f;
		float kc = 1.0f;
		float kphi = 1000.0f;
		float rim_share = 0.5f;
		float tan_repose = 0.7f;
		float rate = 1.0f; // slumping share per second
		float max_dent = 0.1f;
		float darken = 0.3f; // albedo lost where freshly turned
		float gloss = 1.0f; // roughness factor where freshly turned
		float litter = 0.0f; // m of litter on top
		float sand = 0.0f; // share of sand
		bool deformable = true;
	};
	static Cell material_cell(float p_sand, float p_moisture, float p_litter);

private:
	NodePath follow_target;
	NodePath height_source;
	NodePath snow_cover;
	uint32_t ground_collision_mask = 1;
	GroundType ground_type = GROUND_SOIL;
	float moisture = -1.0f; // -1: from the weather map
	float litter_depth = 0.04f;
	float litter_cover = 0.0f; // canopy closure to use with no weather map
	bool rock_from_geology = true;
	Ref<Image> type_map; // R: sand share, G: bare rock
	Rect2 type_map_rect;

	float deform_extent = 32.0f;
	int deform_resolution = 512;
	float ground_spacing = 0.25f;
	float detail_spacing = 0.0625f;
	float outer_spacing = 0.125f;
	PackedStringArray deform_groups;
	float contact_tolerance = 0.03f;
	float character_mass = 75.0f;
	float character_contact_area = 0.035f;
	int max_cached_tiles = 1024;

	Ref<Material> material;
	float skirt = 0.5f;
	float terrain_margin = 2.0f;
	float skirt_fade = 2.0f;

	float drying_time = 900.0f; // s
	float sand_drift_rate = 2.0f; // per hour at 10 m/s
	float rain_smoothing = 0.02f; // per hour per mm/h

	GroundDeformField field;
	struct Columns : public GroundDeformField::Columns {
		const GroundSurface3D *surface = nullptr;
		GroundDeformField::Column column(float p_x, float p_z) const override;
		float sinkage(const GroundDeformField::Body &p_body) const override;
	};

	// The ground lattice over the field, one sample every ground_spacing, and its cells.
	int ground_n = 0;
	Vector2i ground_origin;
	bool ground_valid = false;
	LocalVector<float> ground;
	LocalVector<Cell> cells;
	float ground_min = 0.0f;
	float ground_max = 0.0f;
	Ref<Image> height_image;
	Ref<ImageTexture> height_texture;
	Ref<Image> deform_image;
	Ref<ImageTexture> deform_texture;
	LocalVector<float> tint_albedo;
	LocalVector<float> tint_roughness;

	MeshInstance3D *mesh_instance = nullptr;
	Ref<ArrayMesh> mesh;
	Ref<Material> patch_material;
	Vector3 patch_uv_offset;
	Vector2 center;
	bool mesh_dirty = true;
	bool globals_published = false;
	double cells_accum = 0.0;
	double age_accum = 0.0;
	double step_msec = 0.0;

	static GroundSurface3D *active;

	Vector3 _follow_position() const;
	void _ensure_children();
	void _build_mesh();
	void _update_patch_material();
	void _follow(Vector3 p_center);
	void _sample_ground(Vector2i p_origin, int p_n);
	void _update_cells();
	void _age(double p_delta);
	void _upload();
	void _publish();
	void _set_global(const StringName &p_name, const Variant &p_value, int p_type);
	float _ground_at(float p_x, float p_z) const;
	const Cell &_cell_at(float p_x, float p_z) const;
	float _edge_distance(float p_x, float p_z) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_height_source(const NodePath &p_path);
	NodePath get_height_source() const { return height_source; }
	void set_snow_cover(const NodePath &p_path) { snow_cover = p_path; }
	NodePath get_snow_cover() const { return snow_cover; }
	void set_ground_collision_mask(uint32_t p_mask) { ground_collision_mask = p_mask; }
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_ground_type(GroundType p_type);
	GroundType get_ground_type() const { return ground_type; }
	void set_moisture(float p_moisture);
	float get_moisture() const { return moisture; }
	void set_litter_depth(float p_m);
	float get_litter_depth() const { return litter_depth; }
	void set_litter_cover(float p_cover);
	float get_litter_cover() const { return litter_cover; }
	void set_rock_from_geology(bool p_enable);
	bool is_rock_from_geology() const { return rock_from_geology; }
	void set_type_map(const Ref<Image> &p_map);
	Ref<Image> get_type_map() const { return type_map; }
	void set_type_map_rect(const Rect2 &p_rect);
	Rect2 get_type_map_rect() const { return type_map_rect; }

	void set_deform_extent(float p_extent);
	float get_deform_extent() const { return deform_extent; }
	void set_deform_resolution(int p_resolution);
	int get_deform_resolution() const { return deform_resolution; }
	void set_ground_spacing(float p_spacing);
	float get_ground_spacing() const { return ground_spacing; }
	void set_detail_spacing(float p_spacing);
	float get_detail_spacing() const { return detail_spacing; }
	void set_outer_spacing(float p_spacing);
	float get_outer_spacing() const { return outer_spacing; }
	void set_deform_groups(const PackedStringArray &p_groups) { deform_groups = p_groups; }
	PackedStringArray get_deform_groups() const { return deform_groups; }
	void set_contact_tolerance(float p_m) { contact_tolerance = MAX(p_m, 0.0f); }
	float get_contact_tolerance() const { return contact_tolerance; }
	void set_character_mass(float p_kg) { character_mass = MAX(p_kg, 0.0f); }
	float get_character_mass() const { return character_mass; }
	void set_character_contact_area(float p_m2) { character_contact_area = MAX(p_m2, 0.001f); }
	float get_character_contact_area() const { return character_contact_area; }
	void set_max_cached_tiles(int p_tiles) { max_cached_tiles = MAX(p_tiles, 0); }
	int get_max_cached_tiles() const { return max_cached_tiles; }

	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_skirt(float p_m);
	float get_skirt() const { return skirt; }
	void set_terrain_margin(float p_m);
	float get_terrain_margin() const { return terrain_margin; }
	void set_skirt_fade(float p_m);
	float get_skirt_fade() const { return skirt_fade; }
	void set_drying_time(float p_s) { drying_time = MAX(p_s, 1.0f); }
	float get_drying_time() const { return drying_time; }
	void set_sand_drift_rate(float p_rate) { sand_drift_rate = MAX(p_rate, 0.0f); }
	float get_sand_drift_rate() const { return sand_drift_rate; }
	void set_rain_smoothing(float p_rate) { rain_smoothing = MAX(p_rate, 0.0f); }
	float get_rain_smoothing() const { return rain_smoothing; }

	// Presses the ground like SnowSurface3D.press(): down to p_bottom under a sphere (p_round) or a flat
	// disc of p_radius, plus p_sink below the undisturbed surface. Returns the volume moved, m3.
	float press(Vector3 p_position, float p_radius, float p_bottom, float p_sink = 0.0f, bool p_round = true);
	float press_box(Vector3 p_position, Vector2 p_size, float p_yaw, float p_bottom, float p_sink = 0.0f);
	// Bekker sinkage of a body of p_mass kg on p_area m2 (width p_width) at a place, m.
	float get_sinkage_at(Vector3 p_position, float p_mass, float p_area, float p_width = 0.0f) const;
	// Runs slumping and ageing for p_seconds at once (tests, and settling a scene after loading).
	void settle(float p_seconds, float p_step = 1.0f / 60.0f);
	void clear_deformation();
	void refresh_ground();

	float get_surface_height_at(Vector3 p_position) const;
	float get_ground_height_at(Vector3 p_position) const;
	float get_dent_at(Vector3 p_position) const;
	float get_rim_at(Vector3 p_position) const;
	float get_fresh_at(Vector3 p_position) const;
	// The rules at a place: n, kc, kphi, rim share, repose angle (degrees), slump rate, max dent,
	// litter depth, sand share, deformable (1 or 0).
	Dictionary get_material_at(Vector3 p_position) const;
	Vector2 get_moved_volume() const;
	Rect2 get_deform_rect() const;
	int get_cached_tile_count() const { return field.cached_tile_count(); }
	double get_step_time_msec() const { return step_msec; }
	Ref<Material> get_patch_material() const { return patch_material; }
	PackedStringArray get_configuration_warnings() const override;

	GroundSurface3D();
	~GroundSurface3D();
};

VARIANT_ENUM_CAST(GroundSurface3D::GroundType);
