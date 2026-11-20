/**************************************************************************/
/*  physx_sea_cave_water_3d.cpp                                           */
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

#include "physx_sea_cave_water_3d.h"

#include "physx_river_3d.h"
#include "water_raster_reflections.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"

void PhysXSeaCaveWater3D::set_cave_network(const NodePath &p_path) {
	cave_network = p_path;
	_clear();
}

void PhysXSeaCaveWater3D::set_lanes(int p_lanes) {
	lanes = CLAMP(p_lanes, 2, 64);
}

void PhysXSeaCaveWater3D::_clear() {
	for (Ribbon &r : ribbons) {
		if (r.instance) {
			r.instance->queue_free();
		}
	}
	ribbons.clear();
}

namespace {

constexpr float SC_G = 9.81f;

inline float sc_hash(uint32_t p_x) {
	p_x = p_x * 747796405u + 2891336453u;
	p_x = ((p_x >> ((p_x >> 28u) + 4u)) ^ p_x) * 277803737u;
	return float((p_x >> 22u) ^ p_x) * (1.0f / 4294967296.0f);
}

// The group speed of the waves of wavenumber k over depth h (linear theory).
inline float sc_group_speed(float p_k, float p_h) {
	const float kh = MIN(p_k * MAX(p_h, 0.05f), 20.0f);
	const float c = Math::sqrt(SC_G / p_k * Math::tanh(kh));
	return 0.5f * c * (1.0f + 2.0f * kh / Math::sinh(2.0f * kh));
}

} // namespace

// The wave profile buffer (Jeschke et al. 2018, section 4.2): one periodic line of waves summed over the band the cave
// carries (0.6 to 4 m long), each at its own speed (deep water's dispersion over a 4 m depth) and a fixed random
// phase, the band's heights by a saturated (Phillips) spectrum; scaled to unit variance, so a direction's amplitude
// is its rms height.
void PhysXSeaCaveWater3D::_update_profile() {
	profile.resize(PROFILE_SIZE);
	const int m_lo = int(Math::ceil(PROFILE_PERIOD / 4.0f));
	const int m_hi = int(Math::floor(PROFILE_PERIOD / 0.6f));
	struct Comp {
		float k, w, a, phase;
	};
	LocalVector<Comp> comps;
	float var = 0.0f;
	for (int m = m_lo; m <= m_hi; m += 2) {
		Comp c;
		c.k = float(Math::TAU) * float(m) / PROFILE_PERIOD;
		c.w = Math::sqrt(SC_G * c.k * Math::tanh(c.k * 4.0f));
		c.a = Math::pow(c.k, -1.5f);
		c.phase = float(Math::TAU) * sc_hash(uint32_t(m) * 2654435761u);
		var += 0.5f * c.a * c.a;
		comps.push_back(c);
	}
	const float norm = 1.0f / Math::sqrt(MAX(var, 1e-12f));
	const float t = float(Math::fmod(water_time, 3600.0));
	for (int i = 0; i < PROFILE_SIZE; i++) {
		const float p = PROFILE_PERIOD * float(i) / float(PROFILE_SIZE);
		float v = 0.0f;
		for (const Comp &c : comps) {
			v += c.a * Math::cos(c.k * p - c.w * t + c.phase);
		}
		profile[i] = v * norm;
	}
}

float PhysXSeaCaveWater3D::get_wave_rms(int p_cave) const {
	for (const Ribbon &r : ribbons) {
		if (r.cave != p_cave || r.amp.is_empty()) {
			continue;
		}
		double sum = 0.0;
		const int cells = r.cells * WAVELET_ACROSS;
		for (int c = 0; c < cells; c++) {
			for (int d = 0; d < WAVELET_DIRS; d++) {
				const float a = r.amp[c * WAVELET_DIRS + d];
				sum += a * a;
			}
		}
		return float(Math::sqrt(sum / MAX(cells, 1)));
	}
	return 0.0f;
}

// One surface per sea cave. The layout is the river's (PhysXLiveWater3D's build_live_surface): UV the arc length from
// the arch and the distance across (m), UV2 the current (m/s, world x and z), COLOR.r = (depth + 1) / 4, COLOR.g white
// water, COLOR.b = 1. Dry stations (the back of a cave at low water) sit just under the floor's level so the waterline
// falls between them and the wet ones.
void PhysXSeaCaveWater3D::_update(double p_delta, bool p_mesh) {
	Node *net = cave_network.is_empty() ? nullptr : get_node_or_null(cave_network);
	if (net == nullptr || !net->has_method(SNAME("get_sea_cave_water"))) {
		_clear();
		return;
	}
	if (material.is_null()) {
		material.instantiate();
		material->set_shader(water_shared_shader(physx_river_water_shader_src()));
	}
	const int caves = int(net->call(SNAME("get_cave_count")));
	LocalVector<int> want;
	for (int c = 0; c < caves; c++) {
		if (bool(net->call(SNAME("is_sea_cave"), c))) {
			want.push_back(c);
		}
	}
	// Keep the surfaces in step with the sea caves.
	if (want.size() != ribbons.size()) {
		_clear();
		for (int c : want) {
			Ribbon r;
			r.cave = c;
			r.mesh.instantiate();
			r.instance = memnew(MeshInstance3D);
			r.instance->set_mesh(r.mesh);
			r.instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			add_child(r.instance, false, INTERNAL_MODE_BACK);
			ribbons.push_back(r);
		}
	}
	if (wavelets) {
		_update_profile();
	}
	const int D = WAVELET_DIRS;
	const int NA = WAVELET_ACROSS;
	float dir_cos[WAVELET_DIRS], dir_sin[WAVELET_DIRS], dir_offset[WAVELET_DIRS];
	for (int d = 0; d < D; d++) {
		const float th = float(Math::TAU) * float(d) / float(D);
		dir_cos[d] = Math::cos(th);
		dir_sin[d] = Math::sin(th);
		dir_offset[d] = PROFILE_PERIOD * sc_hash(uint32_t(d) * 40503u + 17u);
	}
	const Transform3D to_local = get_global_transform().affine_inverse();
	for (Ribbon &r : ribbons) {
		const Dictionary w = net->call(SNAME("get_sea_cave_water"), r.cave);
		PackedVector3Array pts = w.get("points", PackedVector3Array());
		PackedFloat32Array hw = w.get("half_widths", PackedFloat32Array());
		PackedFloat32Array fl = w.get("flows", PackedFloat32Array());
		PackedFloat32Array dp = w.get("depths", PackedFloat32Array());
		PackedFloat32Array al = w.get("along", PackedFloat32Array());
		const float covered = w.get("covered_from", 0.0f);
		if (p_mesh) {
			r.mesh->clear_surfaces();
		}
		if (pts.size() < 2 || hw.size() != pts.size() || fl.size() != pts.size() || dp.size() != pts.size()) {
			continue;
		}
		if (al.size() != pts.size()) {
			al.resize(pts.size());
			al.write[0] = 0.0f;
			for (int i = 1; i < pts.size(); i++) {
				al.write[i] = al[i - 1] + Vector2(pts[i].x - pts[i - 1].x, pts[i].z - pts[i - 1].z).length();
			}
		}
		// The surface starts where the arch closes over the water (the sea's own surface is cut from there), a wall
		// margin before it so the two overlap under the arch.
		const float start = MAX(covered - wall_margin, al[0]);
		const float end = al[al.size() - 1];
		const float length = end - start;
		if (length < 0.5f) {
			continue;
		}
		// The stations' values at an arc length (linear between them).
		struct At {
			Vector3 pos, along, across;
			float half = 0.0f, flow = 0.0f, depth = 0.0f, slope = 0.0f;
		};
		const int ns = al.size();
		auto sample = [&](float p_s) -> At {
			const float sa = CLAMP(p_s, al[0], al[ns - 1]);
			int i = 0;
			while (i + 2 < ns && al[i + 1] < sa) {
				i++;
			}
			const float t = CLAMP((sa - al[i]) / MAX(al[i + 1] - al[i], 1e-4f), 0.0f, 1.0f);
			At o;
			o.pos = pts[i].lerp(pts[i + 1], t);
			Vector3 dir(pts[i + 1].x - pts[i].x, 0.0f, pts[i + 1].z - pts[i].z);
			const float run = dir.length();
			o.along = run > 1e-4f ? dir / run : Vector3(1, 0, 0);
			o.across = Vector3(-o.along.z, 0.0f, o.along.x);
			o.half = Math::lerp(hw[i], hw[i + 1], t);
			o.flow = Math::lerp(fl[i], fl[i + 1], t);
			o.depth = Math::lerp(dp[i], dp[i + 1], t);
			o.slope = (pts[i + 1].y - pts[i].y) / MAX(run, 0.01f);
			return o;
		};

		// The short waves (Water Surface Wavelets): amplitude per cell (along from the arch, across the passage's
		// width as -1..1) and direction (0 = into the cave). Semi-Lagrangian steps at the band's group speed plus the
		// current; a wall or the cave's back mirrors the direction and keeps wall_reflection of the height; the mouth
		// lets in the sea's short waves heading in (a cos^2 spread round the passage).
		const int cells = MAX(2, int(Math::ceil(length / WAVELET_CELL)) + 1);
		const float cell = length / float(cells - 1);
		if (wavelets) {
			if (r.cells != cells) {
				r.cells = cells;
				r.amp.resize(cells * NA * D);
				r.amp_next.resize(cells * NA * D);
				for (float &a : r.amp) {
					a = 0.0f;
				}
			}
			float sea_rms = wave_inflow;
			if (sea_rms < 0.0f) {
				sea_rms = float(w.get("wave_rms", 0.0f)) * wave_inflow_share;
			}
			float inflow[WAVELET_DIRS];
			float spread_sum = 0.0f;
			for (int d = 0; d < D; d++) {
				const float c = MAX(dir_cos[d], 0.0f);
				inflow[d] = c * c;
				spread_sum += inflow[d] * inflow[d];
			}
			for (int d = 0; d < D; d++) {
				inflow[d] *= sea_rms / Math::sqrt(MAX(spread_sum, 1e-6f));
			}
			const float k_band = float(Math::TAU) / 1.5f;
			double remaining = MIN(p_delta, 0.5);
			LocalVector<At> rows;
			rows.resize(cells);
			for (int i = 0; i < cells; i++) {
				rows[i] = sample(start + cell * float(i));
			}
			auto amp_at = [&](float p_i, float p_j, int p_d) -> float {
				const float fi = CLAMP(p_i, 0.0f, float(cells - 1));
				const float fj = CLAMP(p_j, 0.0f, float(NA - 1));
				const int i0 = MIN(int(fi), cells - 2), j0 = MIN(int(fj), NA - 2);
				const float ti = fi - i0, tj = fj - j0;
				const float *a = r.amp.ptr();
				const float a00 = a[((i0 * NA) + j0) * D + p_d], a01 = a[((i0 * NA) + j0 + 1) * D + p_d];
				const float a10 = a[(((i0 + 1) * NA) + j0) * D + p_d], a11 = a[(((i0 + 1) * NA) + j0 + 1) * D + p_d];
				return Math::lerp(Math::lerp(a00, a01, tj), Math::lerp(a10, a11, tj), ti);
			};
			while (remaining > 1e-5) {
				const float dt = float(MIN(remaining, 1.0 / 30.0));
				remaining -= dt;
				const float keep = Math::exp(-dt / wave_decay_time);
				for (int i = 0; i < cells; i++) {
					const At &row = rows[i];
					const float cg = sc_group_speed(k_band, MAX(row.depth, 0.1f));
					const float hr = MAX(row.half, 0.2f);
					for (int j = 0; j < NA; j++) {
						const float n = -1.0f + 2.0f * (float(j) + 0.5f) / float(NA);
						for (int d = 0; d < D; d++) {
							float s2 = float(i) * cell - (cg * dir_cos[d] + row.flow) * dt;
							float n2 = n - cg * dir_sin[d] * dt / hr;
							int d2 = d;
							float gain = keep;
							if (s2 > length) {
								// Off the back of the cave: mirrored along the passage.
								s2 = 2.0f * length - s2;
								d2 = (D / 2 - d2 + D) % D;
								gain *= wall_reflection;
							}
							if (n2 > 1.0f || n2 < -1.0f) {
								// Off a wall: mirrored across.
								n2 = n2 > 1.0f ? 2.0f - n2 : -2.0f - n2;
								d2 = (D - d2) % D;
								gain *= wall_reflection;
							}
							float a;
							if (s2 < 0.0f) {
								a = inflow[d2];
							} else {
								a = amp_at(s2 / cell, (n2 + 1.0f) * 0.5f * float(NA) - 0.5f, d2);
							}
							// Dry water carries none.
							r.amp_next[((i * NA) + j) * D + d] = row.depth > 0.05f ? a * gain : 0.0f;
						}
					}
				}
				// A little spreading between neighbouring directions (the wavelets' angular diffusion).
				const float spread = 0.02f;
				for (int c = 0; c < cells * NA; c++) {
					float *a = r.amp_next.ptr() + c * D;
					float tmp[WAVELET_DIRS];
					for (int d = 0; d < D; d++) {
						tmp[d] = (1.0f - 2.0f * spread) * a[d] + spread * (a[(d + 1) % D] + a[(d + D - 1) % D]);
					}
					for (int d = 0; d < D; d++) {
						a[d] = tmp[d];
					}
				}
				SWAP(r.amp, r.amp_next);
			}
		}

		if (!p_mesh) {
			continue;
		}
		// The surface: rows every station_step along, lanes across to past the walls.
		const int rows_n = MAX(2, int(Math::ceil(length / station_step)) + 1);
		const float row_step = length / float(rows_n - 1);
		const int nl = lanes;
		LocalVector<At> row_at;
		row_at.resize(rows_n);
		LocalVector<float> height, across_m;
		height.resize(rows_n * nl);
		across_m.resize(rows_n);
		for (int a = 0; a < rows_n; a++) {
			const float s = row_step * float(a);
			const At o = sample(start + s);
			row_at[a] = o;
			const float half = o.half + wall_margin;
			across_m[a] = 2.0f * half / float(nl - 1);
			for (int l = 0; l < nl; l++) {
				const float y = -half + across_m[a] * float(l);
				float eta = 0.0f;
				if (wavelets && !r.amp.is_empty() && o.depth > 0.05f) {
					const float fi = s / cell;
					const float n = CLAMP(y / MAX(o.half, 0.2f), -1.0f, 1.0f);
					const float fj = (n + 1.0f) * 0.5f * float(NA) - 0.5f;
					const float fi_c = CLAMP(fi, 0.0f, float(cells - 1));
					const float fj_c = CLAMP(fj, 0.0f, float(NA - 1));
					const int i0 = MIN(int(fi_c), cells - 2), j0 = MIN(int(fj_c), NA - 2);
					const float ti = fi_c - i0, tj = fj_c - j0;
					const float *amp = r.amp.ptr();
					for (int d = 0; d < D; d++) {
						const float a00 = amp[((i0 * NA) + j0) * D + d], a01 = amp[((i0 * NA) + j0 + 1) * D + d];
						const float a10 = amp[(((i0 + 1) * NA) + j0) * D + d], a11 = amp[(((i0 + 1) * NA) + j0 + 1) * D + d];
						const float av = Math::lerp(Math::lerp(a00, a01, tj), Math::lerp(a10, a11, tj), ti);
						if (av <= 1e-5f) {
							continue;
						}
						float p = Math::fposmod(s * dir_cos[d] + y * dir_sin[d] + dir_offset[d], PROFILE_PERIOD) * (float(PROFILE_SIZE) / PROFILE_PERIOD);
						const int p0 = int(p) % PROFILE_SIZE;
						const float tp = p - Math::floor(p);
						eta += av * Math::lerp(profile[p0], profile[(p0 + 1) % PROFILE_SIZE], tp);
					}
					// Shallow water carries less (the waves break on the floor): full past 0.3 m.
					eta *= MIN(o.depth / 0.3f, 1.0f);
				}
				height[a * nl + l] = eta;
			}
		}

		PackedVector3Array verts, normals;
		PackedColorArray colors;
		PackedVector2Array uv, uv2;
		PackedInt32Array idx;
		verts.resize(rows_n * nl);
		normals.resize(rows_n * nl);
		colors.resize(rows_n * nl);
		uv.resize(rows_n * nl);
		uv2.resize(rows_n * nl);
		AABB box;
		for (int a = 0; a < rows_n; a++) {
			const At &o = row_at[a];
			const float half = o.half + wall_margin;
			const bool wet = o.depth > 0.01f;
			const float u = o.flow;
			const float white_flow = CLAMP((Math::abs(u) - white_water_speed) / white_water_speed, 0.0f, 1.0f);
			for (int l = 0; l < nl; l++) {
				const int k = a * nl + l;
				const float y = -half + across_m[a] * float(l);
				// The slope of the waves (central differences) on the surge's own slope along the passage.
				const float hs0 = height[MAX(a - 1, 0) * nl + l], hs1 = height[MIN(a + 1, rows_n - 1) * nl + l];
				const float dhs = (hs1 - hs0) / (row_step * float(MIN(a + 1, rows_n - 1) - MAX(a - 1, 0)));
				const float hy0 = height[a * nl + MAX(l - 1, 0)], hy1 = height[a * nl + MIN(l + 1, nl - 1)];
				const float dhy = (hy1 - hy0) / (across_m[a] * float(MIN(l + 1, nl - 1) - MAX(l - 1, 0)));
				const Vector3 nrm = (Vector3(0, 1, 0) - o.along * (o.slope + dhs) - o.across * dhy).normalized();
				// Steep wave faces (piling up at the walls and the back) break white.
				const float steep = Math::sqrt(dhs * dhs + dhy * dhy);
				const float white = MAX(white_flow, CLAMP((steep - 0.35f) / 0.3f, 0.0f, 1.0f));
				Vector3 p = o.pos + o.across * y + Vector3(0, height[k], 0);
				if (!wet) {
					p.y -= 0.03f;
				}
				verts.write[k] = to_local.xform(p);
				normals.write[k] = to_local.basis.xform(nrm).normalized();
				colors.write[k] = Color(CLAMP(((wet ? o.depth : -0.05f) + 1.0f) * 0.25f, 0.0f, 1.0f), white, 1.0f, 1.0f);
				uv2.write[k] = Vector2(o.along.x * u, o.along.z * u);
				// Arc length from the arch (the cross-fade with the sea runs from 0) and distance across.
				uv.write[k] = Vector2(start + row_step * float(a) - covered, y + half);
				if (k == 0) {
					box = AABB(verts[k], Vector3());
				} else {
					box.expand_to(verts[k]);
				}
			}
		}
		for (int a = 0; a < rows_n - 1; a++) {
			if (row_at[a].depth <= 0.01f && row_at[a + 1].depth <= 0.01f) {
				continue;
			}
			for (int l = 0; l < nl - 1; l++) {
				const int i0 = a * nl + l;
				const int i1 = i0 + 1;
				const int i2 = i0 + nl;
				const int i3 = i2 + 1;
				// Clockwise seen from above (Godot's front faces): the path tracer takes the top as entering the
				// water. The winding follows the surface's handedness (across is along turned left).
				idx.push_back(i0);
				idx.push_back(i2);
				idx.push_back(i1);
				idx.push_back(i1);
				idx.push_back(i2);
				idx.push_back(i3);
			}
		}
		if (idx.is_empty()) {
			continue;
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_COLOR] = colors;
		arrays[Mesh::ARRAY_TEX_UV] = uv;
		arrays[Mesh::ARRAY_TEX_UV2] = uv2;
		arrays[Mesh::ARRAY_INDEX] = idx;
		r.mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		r.mesh->surface_set_material(0, material);
		r.instance->set_custom_aabb(box.grow(1.0f));
	}
}

void PhysXSeaCaveWater3D::warm_up(double p_seconds) {
	const double step = 1.0 / 30.0;
	for (double t = 0.0; t < p_seconds; t += step) {
		water_time += step;
		_update(step, false);
	}
	_update(0.0, true);
}

void PhysXSeaCaveWater3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			const double dt = get_process_delta_time();
			water_time += dt;
			if (material.is_valid()) {
				material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
				material->set_shader_parameter(SNAME("fade_in_length"), fade_length);
			}
			_update(dt);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_clear();
		} break;
	}
}

void PhysXSeaCaveWater3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_cave_network", "path"), &PhysXSeaCaveWater3D::set_cave_network);
	ClassDB::bind_method(D_METHOD("get_cave_network"), &PhysXSeaCaveWater3D::get_cave_network);
	ClassDB::bind_method(D_METHOD("set_lanes", "lanes"), &PhysXSeaCaveWater3D::set_lanes);
	ClassDB::bind_method(D_METHOD("get_lanes"), &PhysXSeaCaveWater3D::get_lanes);
	ClassDB::bind_method(D_METHOD("set_station_step", "step"), &PhysXSeaCaveWater3D::set_station_step);
	ClassDB::bind_method(D_METHOD("get_station_step"), &PhysXSeaCaveWater3D::get_station_step);
	ClassDB::bind_method(D_METHOD("set_wall_margin", "margin"), &PhysXSeaCaveWater3D::set_wall_margin);
	ClassDB::bind_method(D_METHOD("get_wall_margin"), &PhysXSeaCaveWater3D::get_wall_margin);
	ClassDB::bind_method(D_METHOD("set_white_water_speed", "speed"), &PhysXSeaCaveWater3D::set_white_water_speed);
	ClassDB::bind_method(D_METHOD("get_white_water_speed"), &PhysXSeaCaveWater3D::get_white_water_speed);
	ClassDB::bind_method(D_METHOD("set_fade_length", "length"), &PhysXSeaCaveWater3D::set_fade_length);
	ClassDB::bind_method(D_METHOD("get_fade_length"), &PhysXSeaCaveWater3D::get_fade_length);
	ClassDB::bind_method(D_METHOD("set_wavelets", "enable"), &PhysXSeaCaveWater3D::set_wavelets);
	ClassDB::bind_method(D_METHOD("get_wavelets"), &PhysXSeaCaveWater3D::get_wavelets);
	ClassDB::bind_method(D_METHOD("set_wave_inflow", "rms"), &PhysXSeaCaveWater3D::set_wave_inflow);
	ClassDB::bind_method(D_METHOD("get_wave_inflow"), &PhysXSeaCaveWater3D::get_wave_inflow);
	ClassDB::bind_method(D_METHOD("set_wave_inflow_share", "share"), &PhysXSeaCaveWater3D::set_wave_inflow_share);
	ClassDB::bind_method(D_METHOD("get_wave_inflow_share"), &PhysXSeaCaveWater3D::get_wave_inflow_share);
	ClassDB::bind_method(D_METHOD("set_wave_decay_time", "seconds"), &PhysXSeaCaveWater3D::set_wave_decay_time);
	ClassDB::bind_method(D_METHOD("get_wave_decay_time"), &PhysXSeaCaveWater3D::get_wave_decay_time);
	ClassDB::bind_method(D_METHOD("set_wall_reflection", "reflection"), &PhysXSeaCaveWater3D::set_wall_reflection);
	ClassDB::bind_method(D_METHOD("get_wall_reflection"), &PhysXSeaCaveWater3D::get_wall_reflection);
	ClassDB::bind_method(D_METHOD("get_ribbon_count"), &PhysXSeaCaveWater3D::get_ribbon_count);
	ClassDB::bind_method(D_METHOD("get_water_material"), &PhysXSeaCaveWater3D::get_water_material);
	ClassDB::bind_method(D_METHOD("get_wave_rms", "cave"), &PhysXSeaCaveWater3D::get_wave_rms);
	ClassDB::bind_method(D_METHOD("update_now", "delta"), &PhysXSeaCaveWater3D::update_now, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("warm_up", "seconds"), &PhysXSeaCaveWater3D::warm_up);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "cave_network", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_cave_network", "get_cave_network");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lanes", PROPERTY_HINT_RANGE, "2,64,1"), "set_lanes", "get_lanes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "station_step", PROPERTY_HINT_RANGE, "0.05,5,0.01,suffix:m"), "set_station_step", "get_station_step");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_margin", PROPERTY_HINT_RANGE, "0,2,0.01,suffix:m"), "set_wall_margin", "get_wall_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "white_water_speed", PROPERTY_HINT_RANGE, "0.1,10,0.1,suffix:m/s"), "set_white_water_speed", "get_white_water_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fade_length", PROPERTY_HINT_RANGE, "0.01,20,0.01,suffix:m"), "set_fade_length", "get_fade_length");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wavelets"), "set_wavelets", "get_wavelets");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_inflow", PROPERTY_HINT_RANGE, "-1,3,0.01,suffix:m"), "set_wave_inflow", "get_wave_inflow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_inflow_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wave_inflow_share", "get_wave_inflow_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wave_decay_time", PROPERTY_HINT_RANGE, "0.1,120,0.1,suffix:s"), "set_wave_decay_time", "get_wave_decay_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_reflection", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wall_reflection", "get_wall_reflection");
}
