/**************************************************************************/
/*  terrain_data_map_3d.h                                                 */
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

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

// Terrain analysis maps for placement (docs/ideas/terrain-data-maps.md): slope and aspect
// (Horn 1981), curvature (Zevenbergen and Thorne 1987), flow accumulation (multiple flow
// direction, Freeman 1991 / Quinn 1991 / Holmgren 1994, over a Priority-Flood filled surface,
// Barnes 2014), topographic wetness index (Beven and Kirkby 1979), water depth and signed
// distance to water (Felzenszwalb and Huttenlocher 2012). Baked on the CPU over a square
// centred on the node.
class TerrainDataMap3D : public Node3D {
	GDCLASS(TerrainDataMap3D, Node3D);

public:
	enum Channel {
		CHANNEL_HEIGHT,
		CHANNEL_SLOPE,
		CHANNEL_ASPECT,
		CHANNEL_CURVATURE,
		CHANNEL_FLOW,
		CHANNEL_WETNESS,
		CHANNEL_WATER_DEPTH,
		CHANNEL_WATER_DISTANCE,
		CHANNEL_SOIL_DEPTH, // Metres (geology_source only).
		CHANNEL_ROCK, // Bare rock exposure, 0 to 1 (geology_source only).
		CHANNEL_TALUS, // Scree cover, 0 to 1 (geology_source only).
		CHANNEL_MAX,
	};

private:
	float size = 256.0f;
	int resolution = 256;
	NodePath height_source;
	uint32_t ground_collision_mask = 1;
	TypedArray<NodePath> water_sources;
	NodePath geology_source;
	bool baked_geology = false;
	float flow_exponent = 1.1f;
	bool bake_on_ready = true;
	bool publish_shader_globals = true;

	int baked_resolution = 0;
	Vector2 baked_origin; // World XZ of the map's corner.
	float baked_cell = 1.0f;
	LocalVector<float> channels[CHANNEL_MAX];
	Ref<Image> data_image;
	Ref<Image> water_image;
	Ref<ImageTexture> data_texture;
	Ref<ImageTexture> water_texture;
	double bake_msec = 0.0;
	bool bake_queued = false;

	bool _sample_heights(LocalVector<float> &r_h, LocalVector<uint8_t> &r_valid);
	void _sample_water(const LocalVector<float> &p_h, LocalVector<float> &r_surface);
	void _publish_globals();
	float _sample(Channel p_channel, const Vector3 &p_global_position) const;
	void _queued_bake();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(float p_size);
	float get_size() const { return size; }
	void set_resolution(int p_resolution);
	int get_resolution() const { return resolution; }
	void set_height_source(const NodePath &p_path);
	NodePath get_height_source() const { return height_source; }
	void set_ground_collision_mask(uint32_t p_mask);
	uint32_t get_ground_collision_mask() const { return ground_collision_mask; }
	void set_geology_source(const NodePath &p_path) { geology_source = p_path; }
	NodePath get_geology_source() const { return geology_source; }
	bool has_geology() const { return baked_geology; }
	void set_water_sources(const TypedArray<NodePath> &p_sources);
	TypedArray<NodePath> get_water_sources() const { return water_sources; }
	void set_flow_exponent(float p_exponent);
	float get_flow_exponent() const { return flow_exponent; }
	void set_bake_on_ready(bool p_enable);
	bool is_bake_on_ready() const { return bake_on_ready; }
	void set_publish_shader_globals(bool p_enable);
	bool is_publishing_shader_globals() const { return publish_shader_globals; }

	Error bake();
	void queue_bake();
	bool is_baked() const { return baked_resolution > 0; }
	double get_bake_time_msec() const { return bake_msec; }
	Rect2 get_map_rect() const;

	float get_height_at(const Vector3 &p_global_position) const;
	float get_slope_at(const Vector3 &p_global_position) const;
	float get_aspect_at(const Vector3 &p_global_position) const;
	float get_curvature_at(const Vector3 &p_global_position) const;
	float get_flow_at(const Vector3 &p_global_position) const;
	float get_wetness_at(const Vector3 &p_global_position) const;
	float get_water_depth_at(const Vector3 &p_global_position) const;
	float get_water_distance_at(const Vector3 &p_global_position) const;
	float get_soil_depth_at(const Vector3 &p_global_position) const;
	float get_rock_at(const Vector3 &p_global_position) const;
	float get_talus_at(const Vector3 &p_global_position) const;

	Ref<Texture2D> get_data_texture() const { return data_texture; }
	Ref<Texture2D> get_water_texture() const { return water_texture; }
	Ref<Image> get_channel_image(Channel p_channel) const;

	static String get_shader_code();
};

VARIANT_ENUM_CAST(TerrainDataMap3D::Channel);
