/**************************************************************************/
/*  ground_dust_3d.h                                                      */
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

#include "ground_surface_3d.h"

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"

class FogVolume;
class ShaderMaterial;
class Shader;

// Dust from dry sand (docs/ideas/sand.md, "Drifting on sand"): the silt and fine sand a tyre lifts as it rolls
// and slides over dry, bare sand, carried off by the wind and settling back.
//
// Emission comes from the vehicles' wheels through GroundSurface3D::add_dust (linear in speed, as unpaved-road
// dust is, Gillies et al. 2005; sliding counts twice; none from damp sand). Transport is the standard Eulerian
// dispersion model on a small grid that follows a target: advection by the wind (log profile from the 10 m
// wind, sand roughness 1 mm) plus the plume's own jet, which the air slows within half a second; eddy
// diffusion; gravitational settling at the grains' Stokes speed, w = (rho_p - rho_a) g d^2 / (18 mu), 0.13 m/s
// at 40 um; whatever settles through the ground is deposited. It is drawn as a FogVolume whose density
// texture is the dust's extinction, the concentration times the mass extinction of grains this size,
// 3 Q / (2 rho_p d) with Q = 2: raster volumetric fog and the path tracer's camera segments both see it (the
// Environment needs volumetric fog on). The grid's half-metre cells hold the plume's mass; its billows are
// sub-grid detail added where it is drawn, as cloud renderers do: the density eroded by a tiling Worley fBm
// that drifts with the wind and rises (Schneider 2015, "The real-time volumetric cloudscapes of Horizon Zero
// Dawn": base density remapped by the noise, which leaves rounded puffs at thin edges and a solid core), the
// role wavelet turbulence (Kim et al. 2008) plays for offline smoke.
class GroundDust3D : public Node3D {
	GDCLASS(GroundDust3D, Node3D);

	NodePath ground_surface;
	NodePath follow_target;
	bool enabled = true;
	Vector3 extent = Vector3(32.0f, 10.0f, 32.0f); // m
	float cell_size = 0.5f; // m
	float grain_diameter = 15.0f; // um, the silt that makes the visible plume
	float diffusivity = 0.15f; // m2/s, eddy diffusion near the ground
	float jet_time = 0.5f; // s, how fast the air slows a plume's jet
	Color dust_color = Color(0.82, 0.72, 0.56);
	float visibility = 1.0f; // scales the drawn extinction (1: physical)
	float billow = 0.9f; // how deeply the noise erodes the plume's edges into puffs
	float billow_size = 2.0f; // m, the size of the largest puffs

	int nx = 0, ny = 0, nz = 0;
	Vector3i origin; // cell index of the grid's corner
	LocalVector<float> conc; // kg/m3
	LocalVector<float> tmp;
	LocalVector<Vector3> jet; // m/s, the plumes' own air motion
	LocalVector<Vector3> jet_tmp;
	LocalVector<float> floor_y; // ground height per column
	LocalVector<GroundSurface3D::Dust> sources;
	double airborne = 0.0; // kg
	double deposited = 0.0; // kg settled to the ground since the last reset (dust carried out of the grid is not counted)
	double emitted = 0.0;
	Vector3 last_follow;
	Vector3 follow_velocity;
	bool has_last = false;

	FogVolume *volume = nullptr;
	Ref<ShaderMaterial> fog;
	Ref<Shader> fog_shader;
	Vector3 noise_offset;
	Ref<ImageTexture3D> density;
	Vector<Ref<Image>> slices;
	bool texture_dirty = false;

	GroundSurface3D *_surface() const;
	Vector3 _follow_position() const;
	void _allocate();
	void _recenter(const Vector3 &p_center, GroundSurface3D *p_surface);
	void _floors(GroundSurface3D *p_surface);
	void _step(float p_dt);
	void _draw();
	float _sample(const LocalVector<float> &p_field, Vector3 p_pos) const;
	Vector3 _sample_jet(Vector3 p_pos) const;
	_FORCE_INLINE_ int _idx(int x, int y, int z) const { return (z * ny + y) * nx + x; }

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_ground_surface(const NodePath &p_path) { ground_surface = p_path; }
	NodePath get_ground_surface() const { return ground_surface; }
	void set_follow_target(const NodePath &p_path) { follow_target = p_path; }
	NodePath get_follow_target() const { return follow_target; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_extent(const Vector3 &p_extent);
	Vector3 get_extent() const { return extent; }
	void set_cell_size(float p_size);
	float get_cell_size() const { return cell_size; }
	void set_grain_diameter(float p_um) { grain_diameter = CLAMP(p_um, 1.0f, 200.0f); }
	float get_grain_diameter() const { return grain_diameter; }
	void set_diffusivity(float p_v) { diffusivity = MAX(p_v, 0.0f); }
	float get_diffusivity() const { return diffusivity; }
	void set_jet_time(float p_v) { jet_time = MAX(p_v, 0.01f); }
	float get_jet_time() const { return jet_time; }
	void set_dust_color(const Color &p_color);
	Color get_dust_color() const { return dust_color; }
	void set_visibility(float p_v) { visibility = MAX(p_v, 0.0f); }
	float get_visibility() const { return visibility; }
	void set_billow(float p_v);
	float get_billow() const { return billow; }
	void set_billow_size(float p_v);
	float get_billow_size() const { return billow_size; }

	float get_settling_speed() const; // m/s
	float get_mass_extinction() const; // m2/kg
	float get_airborne_mass() const { return (float)airborne; }
	float get_deposited_mass() const { return (float)deposited; }
	float get_emitted_mass() const { return (float)emitted; }
	float get_concentration_at(Vector3 p_position) const; // kg/m3
	void add_dust(Vector3 p_position, Vector3 p_velocity, float p_mass); // kg, at once
	void clear();

	GroundDust3D();
};
