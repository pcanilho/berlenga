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
#include "scene/resources/image_texture.h"

class FogVolume;
class FogMaterial;

// Mist: the finest drops a waterfall, a rapid or a fountain throws, carried off on the air (Stokes-settling
// 20 um drops, a few cm/s, that evaporate in seconds to minutes with the humidity). A small grid of liquid
// water content (g/m^3) over a world-aligned box, stepped on the CPU (semi-Lagrangian advection, Stam 1999,
// plus turbulent diffusion, settling and evaporation), fed at a source and blown out by the source's own
// air (the spray wind a fall drags down and spreads along the pool) plus the WeatherMap3D's wind.
// Drawn in raster as a FogVolume with the grid as its density texture (needs the Environment's volumetric
// fog), and under path tracing as a dense medium traced with delta tracking (RTVolumes): extinction
// 3 LWC / (2 rho_w r) (Q_ext ~ 2), albedo ~1, a Henyey-Greenstein lobe of g 0.85 for drops this size.
class PhysXWaterMist3D : public Node3D {
	GDCLASS(PhysXWaterMist3D, Node3D);

	Vector3 size = Vector3(16.0f, 10.0f, 16.0f); // the box: centred on the node in x and z, from the node up; world axes
	float cell_size = 0.5f;
	Vector3 source_position; // local
	Vector3 source_extents = Vector3(3.0f, 0.5f, 1.0f); // local half extents
	float source_rate = 200.0f; // grams of fine drops a second
	float column_height = 0.0f; // m above the source's centre: a falling column there sheds mist too (0: none)
	float column_share = 0.4f; // of source_rate, fed evenly along that column
	float jet_speed = 3.0f; // m/s: the air leaving the source
	Vector3 jet_direction = Vector3(0.0f, 0.0f, -1.0f); // local, horizontal: the air spreads round it, never back against it
	float drop_radius = 20.0f; // micrometres
	float humidity = 0.85f; // relative, without a WeatherMap3D
	Vector3 wind; // m/s, without a WeatherMap3D
	bool follow_weather = true;
	float diffusion = 0.5f; // m^2/s
	float update_rate = 15.0f; // Hz
	bool emitting = true;

	Vector3i dims;
	Vector3 grid_min; // world
	float cell = 0.5f; // the cell size in use (cell_size, grown so no side passes MAX_CELLS)
	LocalVector<float> density;
	LocalVector<float> scratch;
	LocalVector<Vector3> jet; // the source's air per cell
	LocalVector<float> source; // g/m^3/s per cell
	bool grid_dirty = true;
	double accum = 0.0;
	float max_density = 0.0f;
	FogVolume *fog = nullptr;
	Ref<FogMaterial> fog_material;
	Ref<ImageTexture3D> texture;

	void _build_grid();
	void _step(float p_dt);
	void _upload();
	Vector3 _ambient_wind() const;
	float _wall_fade(int p_x, int p_y, int p_z) const;
	float _humidity() const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static constexpr int MAX_CELLS = 64; // per side

	// Per micrometre radius, from Stokes, Rogers and Yau's growth law and Mie extinction.
	static float settling_speed(float p_radius_um); // m/s
	static float evaporation_time(float p_radius_um, float p_humidity); // s
	static float extinction_per_density(float p_radius_um); // 1/m per g/m^3

	void set_size(const Vector3 &p_size);
	Vector3 get_size() const { return size; }
	void set_cell_size(float p_size);
	float get_cell_size() const { return cell_size; }
	void set_source_position(const Vector3 &p_position);
	Vector3 get_source_position() const { return source_position; }
	void set_source_extents(const Vector3 &p_extents);
	Vector3 get_source_extents() const { return source_extents; }
	void set_source_rate(float p_rate);
	float get_source_rate() const { return source_rate; }
	void set_column_height(float p_height);
	float get_column_height() const { return column_height; }
	void set_column_share(float p_share);
	float get_column_share() const { return column_share; }
	void set_jet_speed(float p_speed);
	float get_jet_speed() const { return jet_speed; }
	void set_jet_direction(const Vector3 &p_direction);
	Vector3 get_jet_direction() const { return jet_direction; }
	void set_drop_radius(float p_radius);
	float get_drop_radius() const { return drop_radius; }
	void set_humidity(float p_humidity);
	float get_humidity() const { return humidity; }
	void set_wind(const Vector3 &p_wind);
	Vector3 get_wind() const { return wind; }
	void set_follow_weather(bool p_enabled);
	bool get_follow_weather() const { return follow_weather; }
	void set_diffusion(float p_diffusion);
	float get_diffusion() const { return diffusion; }
	void set_update_rate(float p_rate);
	float get_update_rate() const { return update_rate; }
	void set_emitting(bool p_emitting);
	bool is_emitting() const { return emitting; }

	// Liquid water content (g/m^3) at a world position; 0 outside the box.
	float sample_density(const Vector3 &p_world) const;
	float get_max_density() const { return max_density; }
	// The grid as it is drawn (world-aligned box, its cells).
	Vector3 get_grid_min() const { return grid_min; }
	float get_grid_cell() const { return cell; }
	Vector3i get_grid_dims() const { return dims; }
	float get_cell_density(int p_x, int p_y, int p_z) const;
	Ref<ImageTexture3D> get_texture() const { return texture; }
	// Steps the grid now (a bench or a test runs it ahead of time).
	void advance(float p_seconds);

	PhysXWaterMist3D();
	~PhysXWaterMist3D();
};
