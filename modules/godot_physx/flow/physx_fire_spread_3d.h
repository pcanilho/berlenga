/**************************************************************************/
/*  physx_fire_spread_3d.h                                                  */
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
#include "physx_firebrands.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

class Decal;
class PhysXFlow3D;
class PhysXFlowEmitter3D;

// Fire spreading over the ground (docs/ideas/physx.md, "Spreading fire on surfaces"): the WRF-SFIRE surface fire
// model (Mandel, Beezley and Kochanski 2011) on a grid under this node. A level set (Osher and Sethian 1988) holds the
// fire front and moves at Rothermel's rate of spread (Rothermel 1972): faster with the wind along the front and uphill,
// zero where the fuel is too wet. Fuel left after a cell ignites decays as exp(-t / burn_time); the heat it gives off
// feeds a pool of emitters in a PhysXFlow3D, which draws the flames and smoke (raster and path traced) and lights the
// scene. What has burnt is charred, and the burning band glows, through a Decal and the global shader parameters
// fire_char_map / fire_char_rect for ground and vegetation shaders.
class PhysXFireSpread3D : public Node3D {
	GDCLASS(PhysXFireSpread3D, Node3D);

public:
	enum FuelType {
		FUEL_SHORT_GRASS, // Anderson fuel model 1
		FUEL_TALL_GRASS, // model 3
		FUEL_LITTER, // models 8-9, leaf and needle litter under trees
		FUEL_MAX,
	};

private:
	bool enabled = true;
	Vector2 size = Vector2(20, 20); // metres, along local X and Z, centred on the node
	float cell_size = 0.25f;
	FuelType fuel_type = FUEL_SHORT_GRASS;
	float fuel_moisture = 0.06f; // dead fuel moisture, fraction of dry mass
	float fuel_load_scale = 1.0f;
	Ref<Image> fuel_map; // optional, red: fuel load multiplier, green: extra moisture (fraction of dry weight, x 0.5)
	float fuel_patchiness = 0.5f; // patchy fuel and moisture, so the scar is ragged
	NodePath vegetation_field; // optional VegetationField3D: worn paths and leaf litter change the fuel
	LocalVector<float> rain_water; // kg of intercepted rain per kg of fuel, per cell
	LocalVector<uint8_t> quenched; // put out while burning (rain)
	float base_moisture = 0.06f; // fuel_moisture as the weather moves it
	bool use_weather_moisture = true;
	float moisture_timelag = 3600.0f; // s, the fine dead fuel's (1 h) time lag
	LocalVector<float> rate; // spread multiplier per cell (bed structure, patchiness)
	LocalVector<float> wet; // moisture over fuel_moisture, per cell (patches, fuel_map green)
	Vector3 wind; // world wind when no weather map gives one
	bool use_weather_wind = true;
	bool lightning_ignition = true; // storm lightning (WeatherLightning3D) can light the fuel it strikes
	uint64_t lightning_seen = 0;
	int lightning_ignitions = 0;
	void _ignite_from_lightning();
	float spread_scale = 1.0f; // speeds the front up or down (a game's pace)
	NodePath heat_source; // a PhysXFlow3D whose hot gas ignites the ground under it
	float ignition_temperature = 0.35f; // in the heat source's temperature units
	NodePath flow_path; // empty: one of its own
	int max_emitters = 32;
	float flame_strength = 1.0f;
	float char_darkness = 0.95f;
	float glow_energy = 4.0f;

	// The grid.
	int nx = 0, nz = 0;
	LocalVector<float> phi; // level set, below 0: ignited
	LocalVector<float> phi_next;
	LocalVector<float> t_ignite; // time of ignition, -1 before
	LocalVector<float> load; // kg/m2 of fine fuel
	LocalVector<float> height; // ground height (world y)
	LocalVector<float> fuel_left; // fraction 0..1
	LocalVector<float> heat; // released this step, W/m2
	bool heights_ready = false;
	bool grid_dirty = true;
	float time = 0.0f;
	float sample_timer = 0.0f;
	float map_timer = 0.0f;
	int band_x0 = 0, band_x1 = -1, band_z0 = 0, band_z1 = -1; // cells that may change
	float total_burnt = 0.0f; // m2

	PhysXFlow3D *flow = nullptr;
	bool own_flow = false;
	PhysXFlowEmitter3D *air = nullptr;
	LocalVector<PhysXFlowEmitter3D *> emitters;
	Decal *decal = nullptr;
	Ref<Image> char_image;
	Ref<ImageTexture> char_texture;
	Ref<Image> glow_image;
	Ref<ImageTexture> glow_texture;
	// Plants' view of the fire (fire_state_map): burnt, glow, peak flame length, peak crown scorch height.
	Ref<Image> state_image;
	Ref<ImageTexture> state_texture;
	// Large wood (logs and fallen branches from a VegetationField3D): kg/m2, diameter (m), fraction left, ignition time.
	LocalVector<float> heavy;
	LocalVector<float> heavy_d;
	LocalVector<float> heavy_left;
	LocalVector<float> heavy_t;
	bool heavy_any = false;
	// Spotting: brands thrown up by what burns this step, then flown and landed.
	bool spotting = true;
	float spotting_scale = 1.0f;
	PhysXFirebrands firebrands;
	LocalVector<PhysXFirebrands::Release> brand_points;
	float brand_count = 0.0f; // brands to throw this step
	static LocalVector<PhysXFireSpread3D *> fires;
	float _heavy_flaming_time(uint32_t p_i) const { return 18900.0f * heavy_d[p_i]; } // Anderson 1969, 384/sigma min
	bool _heavy_flaming(uint32_t p_i) const { return heavy_t[p_i] >= 0.0f && heavy_left[p_i] > HEAVY_SMOULDER && !quenched[p_i]; }
	static constexpr float HEAVY_SMOULDER = 0.4f; // below this share left, a log smoulders without flame
	LocalVector<float> peak_flame; // m, Byram's flame length of the front that crossed the cell
	LocalVector<float> peak_scorch; // m, Van Wagner's crown scorch height

	struct FuelModel {
		float load; // kg/m2
		float depth; // m
		float sigma; // surface-area-to-volume ratio, 1/ft (Rothermel's units)
		float beta_ratio; // packing ratio over its optimum
		float beta; // packing ratio
		float moisture_extinction;
		float r0; // no-wind, no-slope spread at zero moisture, m/s
		float burn_time; // s, fuel left decays as exp(-t / burn_time)
	};
	static const FuelModel &_model(FuelType p_type);

	void _allocate();
	void _read_heights();
	void _build_flow();
	void _build_decal();
	float _spread_rate(int p_x, int p_z, const Vector2 &p_normal, const Vector2 &p_wind) const;
	void _step(float p_delta);
	void _ignite_from_heat_source();
	void _update_emitters(const Vector3 &p_wind);
	void _update_maps();
	void _burn_heavy(float p_delta);
	Vector3 _wind() const;
	Vector3 _cell_world(int p_x, int p_z) const;
	bool _world_cell(const Vector3 &p_world, int &r_x, int &r_z) const;
	void _grow_band(int p_x0, int p_z0, int p_x1, int p_z1);
	void _redistance();

	friend class PhysXFirebrands;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void ignite(const Vector3 &p_position, float p_radius = 0.5f);
	void extinguish();
	float get_char_at(const Vector3 &p_position) const; // 0 unburnt .. 1 burnt out
	float get_intensity_at(const Vector3 &p_position) const; // fireline heat, kW/m2 of ground
	float get_flame_length_at(const Vector3 &p_position) const; // peak, m
	float get_scorch_height_at(const Vector3 &p_position) const; // peak crown scorch height, m
	float get_burnt_area() const { return total_burnt; }
	int get_burning_cells() const;

	void set_enabled(bool p_v) { enabled = p_v; }
	bool is_enabled() const { return enabled; }
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_cell_size(float p_v);
	float get_cell_size() const { return cell_size; }
	void set_fuel_type(FuelType p_type);
	FuelType get_fuel_type() const { return fuel_type; }
	void set_fuel_moisture(float p_v) {
		fuel_moisture = CLAMP(p_v, 0.0f, 2.0f);
		base_moisture = fuel_moisture;
	}
	float get_moisture_at(const Vector3 &p_position) const;
	// Fires in the scene tree, for things that burn without a fire set (cloth): the first whose area holds p_position.
	static PhysXFireSpread3D *find_at(const Vector3 &p_position);
	static bool any() { return !fires.is_empty(); }
	float get_heavy_fuel_at(const Vector3 &p_position) const;
	float get_moisture_of_extinction() const { return _model(fuel_type).moisture_extinction; }
	float get_ground_height_at(const Vector3 &p_position) const;
	void set_use_weather_moisture(bool p_v) { use_weather_moisture = p_v; }
	bool is_using_weather_moisture() const { return use_weather_moisture; }
	void set_moisture_timelag(float p_v) { moisture_timelag = MAX(p_v, 1.0f); }
	float get_moisture_timelag() const { return moisture_timelag; }
	float get_fuel_moisture() const { return fuel_moisture; }
	void set_fuel_load_scale(float p_v);
	float get_fuel_load_scale() const { return fuel_load_scale; }
	void set_fuel_map(const Ref<Image> &p_map);
	Ref<Image> get_fuel_map() const { return fuel_map; }
	void _update_moisture(float p_delta);
	float _moisture(uint32_t p_i) const { return base_moisture * wet[p_i] + rain_water[p_i]; }
	float _air_temperature() const;
	void set_fuel_patchiness(float p_v);
	float get_fuel_patchiness() const { return fuel_patchiness; }
	void set_vegetation_field(const NodePath &p_path);
	NodePath get_vegetation_field() const { return vegetation_field; }
	void set_wind(const Vector3 &p_wind) { wind = p_wind; }
	Vector3 get_wind() const { return wind; }
	void set_use_weather_wind(bool p_v) { use_weather_wind = p_v; }
	bool is_using_weather_wind() const { return use_weather_wind; }
	void set_lightning_ignition(bool p_v) { lightning_ignition = p_v; }
	bool is_lightning_ignition() const { return lightning_ignition; }
	int get_lightning_ignition_count() const { return lightning_ignitions; }
	// Chance that a ground strike with p_continuing seconds of continuing current lights fuel at p_moisture of
	// p_extinction (both fractions of dry weight).
	static float lightning_ignition_chance(float p_continuing, float p_moisture, float p_extinction);
	void set_spread_scale(float p_v) { spread_scale = MAX(p_v, 0.0f); }
	float get_spread_scale() const { return spread_scale; }
	void set_heat_source(const NodePath &p_path) { heat_source = p_path; }
	NodePath get_heat_source() const { return heat_source; }
	void set_ignition_temperature(float p_v) { ignition_temperature = MAX(p_v, 0.0f); }
	float get_ignition_temperature() const { return ignition_temperature; }
	void set_flow_path(const NodePath &p_path) { flow_path = p_path; }
	NodePath get_flow_path() const { return flow_path; }
	void set_max_emitters(int p_v);
	int get_max_emitters() const { return max_emitters; }
	void set_flame_strength(float p_v) { flame_strength = MAX(p_v, 0.0f); }
	float get_flame_strength() const { return flame_strength; }
	void set_char_darkness(float p_v) { char_darkness = CLAMP(p_v, 0.0f, 1.0f); }
	float get_char_darkness() const { return char_darkness; }
	void set_glow_energy(float p_v);
	float get_glow_energy() const { return glow_energy; }

	void set_spotting(bool p_v);
	bool is_spotting() const { return spotting; }
	void set_spotting_scale(float p_v) { spotting_scale = MAX(p_v, 0.0f); }
	float get_spotting_scale() const { return spotting_scale; }
	int get_firebrand_count() const { return firebrands.get_count(); }
	int get_spot_fire_count() const { return firebrands.get_spot_fires(); }

	PhysXFlow3D *get_flow() const { return flow; }
};

VARIANT_ENUM_CAST(PhysXFireSpread3D::FuelType);
