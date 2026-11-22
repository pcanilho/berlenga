/**************************************************************************/
/*  weather_clouds.h                                                      */
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

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "scene/resources/texture.h"

// The cloud layer of the weather map: one density field, after Guerrilla's Nubis (Schneider 2015,
// 2017) with Frostbite's measured extinction (Hillaire 2016). The same function runs on the CPU
// (cloud shadow bake, get_cloud_density_at) and in WeatherSkyMaterial (WEATHER_CLOUDS_GLSL), so the
// clouds in the sky and their shadows on the ground agree. See docs/ideas/weather.md, step 3.
namespace WeatherClouds {

constexpr int MAX_STORMS = 8;
constexpr float SHELF_DROP = 450.0f; // m, a shelf cloud's greatest drop below its storm's base
constexpr float SCUD_DEPTH = 450.0f; // m, how far under the base cumulus fractus hangs in humid air
constexpr float BASE_SAG = 150.0f; // m, the most a cumulus' base sags under its updraft below the condensation level
constexpr int MAX_CLOUD_LIGHTS = 8;
constexpr int MAX_CLOUD_EFFECTORS = 8;
constexpr int SHAPE_SIZE = 128; // texels across the base shape noise (tiles)
constexpr int DETAIL_SIZE = 128; // texels across the detail noise (tiles; RGBA: wisps low, high, billows low, high)

// Everything the density needs, as WeatherMap3D last eased it.
struct State {
	float base_height = 1500.0f; // m above y = 0
	float thickness = 1500.0f; // m, of cumulus (type 0.7); stratus is a quarter, cumulonimbus five times
	float density = 1.0f; // scale on the measured extinction
	float cover = 0.4f;
	float type = 0.3f;
	float variation = 0.35f;
	float cover_scale = 6000.0f; // m, the cover noise (WeatherMap3D::cloud_scale)
	float shape_scale = 3000.0f; // m, one tile of the shape noise
	Vector2 drift; // m, how far the clouds have moved with the wind aloft
	// m, how far a cloud's tissue one thickness above the base has been carried downwind (and veered) of
	// the base by the wind shear while it rose: tops lean over (Nubis's cloud top offset), growing as height
	// squared (shear_offset()).
	Vector2 shear;
	float size_mix = 0.0f; // 0..1: large clouds among the small (shape_at)
	// Clouds that form and clear (step 3e): the cover noise's time in lifetimes (below 0: frozen), how far the
	// shape noise's second read has slid against the first, and how far the sky's detail noise has risen
	// (texture units, wrapped).
	float evolve = -1.0f;
	Vector2 shift;
	float rise_detail = 0.0f;
	int storm_count = 0;
	float storms[MAX_STORMS][4] = {}; // x, z, radius of the cloud shield, cover times growth
	// Each storm's own cloud base (m above y = 0; <= 0: base_height), as a fire cloud's at its plume's
	// condensation level. Columns round a storm blend to it as they join its cloud shield (base_at()).
	float storm_base[MAX_STORMS] = {};
	// Round 8: each storm's peak rain (mm/h, times its growth) and downburst outflow (m/s). The outflow's gust
	// front lifts a shelf cloud ahead of the storm, along storm_dir (the way it moves); the sky draws the rain as
	// shafts under the base.
	float storm_rain[MAX_STORMS] = {};
	float storm_outflow[MAX_STORMS] = {};
	float storm_snow[MAX_STORMS] = {}; // share of the storm's precipitation falling as snow at the ground (rain_fraction)
	Vector2 storm_dir = Vector2(1, 0); // unit, the way the storms move (the steering wind)
	Vector2 rain_slant; // horizontal m per m of fall: the low wind over the drops' fall speed (about 7 m/s)
	// The high cloud layer (cirrus, cirrostratus; docs/ideas/clouds.md): a thin sheet at high_height, its cover
	// eased by the weather map (get_high_cloud_cover_at), its optical depth at full cover high_tau (ice clouds
	// are 0.03 to 3), drifting with the faster wind aloft and streaked along high_dir. Storm cells spread an
	// anvil shield downwind of themselves (the storms above, shield_reach radii ahead).
	float high_cover = 0.0f;
	float high_height = 8000.0f;
	float high_tau = 1.0f;
	float high_scale = 24000.0f; // m, its patches
	Vector2 high_drift; // m
	Vector2 high_dir = Vector2(1, 0); // unit, the wind aloft
	// Cirrocumulus share (round 7): how unstable the shear at the high layer leaves it, 0 (smooth cirrus) to 1
	// (rippled cirrocumulus in patches); from the gradient Richardson number (WeatherMap3D).
	float high_cc = 0.0f;
	// The mid layer (altocumulus, altostratus; docs/ideas/clouds.md): a slab mid_thickness deep centred on
	// mid_height, its cover eased by the weather map (get_mid_cloud_cover_at), its vertical optical depth at full
	// cover mid_tau, drifting with the wind aloft at its height; the sky breaks it into cloudlets in rows across
	// mid_dir. Storms add a band round their anvils.
	float mid_cover = 0.0f;
	float mid_height = 4500.0f;
	float mid_thickness = 400.0f;
	float mid_tau = 4.0f;
	float mid_scale = 12000.0f; // m, its patches
	Vector2 mid_drift; // m
	Vector2 mid_dir = Vector2(1, 0); // unit, the wind at its height
	// Stratocumulus (round 6): closed cells about sc_cell metres across, stretched sc_roll times along sc_dir (the
	// boundary layer's wind) into rolls when it blows.
	Vector2 sc_dir = Vector2(1, 0);
	float sc_roll = 1.0f;
	float sc_cell = 1800.0f;
	float cell_size = 1200.0f; // m, the cumulus cells (cells_at), each holding at most one cloud
	// Density effectors (WeatherMap3D.add_cloud_effector): spheres (world x, y, z, radius m) whose strength adds
	// cloud (> 0: a plume feeding a cumulus) or clears it (< 0: a blast's hole), falling off linearly to the edge.
	int effector_count = 0;
	float effectors[MAX_CLOUD_EFFECTORS][5] = {}; // x, y, z, radius, strength
	// Point lights in the clouds (WeatherMap3D.add_cloud_light): world x, y, z, reach (m); linear r, g, b times
	// energy. Only the sky reads them.
	int light_count = 0;
	float lights[MAX_CLOUD_LIGHTS][7] = {}; // x, y, z, reach, r, g, b
	// Wave clouds (lenticulars; WeatherMountainWaves, docs/ideas/clouds.md): the mountain waves' vertical
	// displacement (m) at four world heights over a square fixed to the ground, and the slab (m) where the lift
	// can reach saturation (empty when wave_y1 <= wave_y0). The air's temperature (C) and relative humidity
	// at air_y0 (m) give the humidity aloft, with the mid and high layers' moist air.
	Vector<float> wave_eta; // wave_n * wave_n * 4
	int wave_n = 0;
	Vector2 wave_corner;
	float wave_size = 0.0f;
	float wave_levels[4] = { 2000.0f, 3500.0f, 5000.0f, 7000.0f };
	float wave_y0 = 0.0f; // the slab round the mid layer's moist air
	float wave_y1 = 0.0f;
	float wave_y2 = 0.0f; // the slab round the high layer's (lenses of ice: cirrocumulus lenticularis)
	float wave_y3 = 0.0f;
	float air_t0 = 15.0f;
	float air_rh0 = 0.6f;
	float scud = 0.0f; // share of the sky with fractus under its bases (WeatherMap3D: humid air, rain falling)
	// Lake-effect snow (WeatherLakeEffect): the fetch field (km of open water upwind, band axis 0..1) over a square
	// fixed to the ground, and lake_params: how unstable the air over the water is (0..1, from the water against the
	// air at 850 hPa), the share of wind-parallel rolls against a single band (by U / L), the rolls' spacing (m),
	// the convection's top over the base (m). lake_dir: the boundary layer's wind (unit).
	Vector<float> lake_field;
	int lake_n = 0;
	Vector2 lake_corner;
	float lake_size = 0.0f;
	Vector4 lake_params;
	Vector2 lake_dir = Vector2(1, 0);
	float air_y0 = 0.0f;
	float top_max = 0.0f; // highest top any column reaches now (m), set by update_top_max()
	float base_min = 1500.0f; // lowest base any column has now (m), set by update_top_max()

	void update_top_max();
};

// Cloud top over the base for a cloud type: stratus a quarter of the thickness, cumulus (0.7) all of
// it, cumulonimbus towers (type above 0.8) up to five times.
float top_height(const State &p_state, float p_type);
// The shape noise's offset at height p_y (m): the shear times the squared height over the thickness.
Vector2 shear_offset(const State &p_state, float p_y);
// The same over a column whose base is p_base (m).
Vector2 shear_offset(const State &p_state, float p_y, float p_base);
// The weather map's cover noise: four octaves of hashed value noise over (p - drift) / scale, 0..1. With
// p_evolve >= 0 (time in lifetimes) each octave moves on through time (clouds form and clear).
float cover_noise(Vector2 p_xz, Vector2 p_drift, float p_scale, float p_evolve = -1.0f);
// cover_noise's per-call constants (the drift, the scale and each octave's two time slices and blend), taken once for
// a pass over many points; cover_noise(frame, p) then gives exactly cover_noise(p, drift, scale, evolve).
struct CoverNoiseFrame {
	Vector2 drift;
	float scale = 1.0f;
	bool evolve = false;
	Vector2 offset_a[4];
	Vector2 offset_b[4];
	float w[4] = {};
	float len[4] = {};
};
CoverNoiseFrame cover_noise_frame(Vector2 p_drift, float p_scale, float p_evolve = -1.0f);
float cover_noise(const CoverNoiseFrame &p_frame, Vector2 p_xz);
// Cover and type at world x, z: the weather map's drifting noise plus storms.
Vector2 cover_type_at(const State &p_state, Vector2 p_xz);
// How high a storm's column reaches over p_xz, as a cloud type: under its anvil, the storm's own height.
float storm_column_type(const State &p_state, Vector2 p_xz);
// Lake-effect cloud at x, z (0..1): the fetch's growth times the air's instability, in a single band along the longest
// fetch or in rolls along the wind (WeatherLakeEffect; wc_lake_effect in the sky).
float lake_effect_at(const float *p_field, int p_n, Vector2 p_corner, float p_size, const Vector4 &p_params, Vector2 p_dir, Vector2 p_xz);
float lake_effect_at(const State &p_state, Vector2 p_xz);
// How much of the sky has cumulus fractus under its bases (0 to 1): humid air at the ground, or rain falling through it.
float scud_share(float p_rh, float p_rain);
// The cloud base (m) over world x, z: base_height, blended to each storm's own base over its cloud shield.
float base_at(const State &p_state, Vector2 p_xz);
// The high layer's cover (0..1) and vertical optical depth over world x, z (patches and anvil shields; the
// sky adds the fibres).
float high_cover_at(const State &p_state, Vector2 p_xz);
float high_tau_at(const State &p_state, Vector2 p_xz);
// The mid layer's cover (0..1) and vertical optical depth over world x, z.
float mid_cover_at(const State &p_state, Vector2 p_xz);
float mid_tau_at(const State &p_state, Vector2 p_xz);
// Extinction (1/m) at a world point, base shape only (the sky adds detail erosion at the edges).
// Where a world point reads the shape noise first (turned, stretched twice vertically), and the
// shape there: two reads of the tile at an irrational ratio of scales, evened out (0..1).
Vector3 shape_coord(const State &p_state, const Vector3 &p_pos);
float shape_at(const State &p_state, const Vector3 &p_pos);
// The density effectors at a point: the added share (> 0) or the cleared share (< 0), -1..1.
float effector_at(const State &p_state, const Vector3 &p_pos);
// Stratocumulus closed cells at x, z: 0 on the cells' borders to 1 in their middles (Worley F2 - F1 on a jittered
// grid, in the wind's frame); and how much of the type is stratocumulus (0..1).
float sc_cells_at(const State &p_state, Vector2 p_xz);
float sc_share(float p_type);
float shelf_drop(const State &p_state, int p_storm, Vector2 p_xz);
float layered_share(float p_type);
// Wave clouds: the dew point depression (K) of the air at a point (the model's profile, moistened by the mid and
// high layers' cover), the lift (m) that saturates it, and the wave cloud's extinction (1/m) there.
float wave_dew_depression_at(const State &p_state, const Vector3 &p_pos);
// The pile of plates' extra dew point depression (K) in the drier sheets of a stratified moist layer.
float wave_plate_depression(const State &p_state, const Vector3 &p_pos);
float wave_extinction_at(const State &p_state, const Vector3 &p_pos);
// The two slabs (m: x..y round the mid layer's height, z..w above) where wave cloud can form now, from the field's
// largest lift at each level (empty: y <= x).
Vector4 wave_slab(const State &p_state, const float *p_eta_max);
float extinction_at(const State &p_state, const Vector3 &p_pos);

// The tiling shape noises (Perlin-Worley base; detail: Curly-Alligator wisps and Worley billows, low and
// high frequency, interleaved RGBA), made once.
const Vector<float> &shape_noise();
const Vector<float> &detail_noise();
Ref<Texture3D> shape_texture();
Ref<Texture3D> detail_texture();
void free_textures();

// GLSL for the sky shader: the same cover, type and density as the functions above.
extern const char *GLSL;

} // namespace WeatherClouds
