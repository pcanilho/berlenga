/**************************************************************************/
/*  snow_rime_3d.cpp                                                     */
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

#include "snow_rime_3d.h"
#include "snow_cover_3d.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/hashfuncs.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"

namespace {
constexpr float WATER_DENSITY = 1000.0f; // kg/m3
constexpr float AIR_DENSITY = 1.3f; // kg/m3 near -5 C
constexpr float AIR_VISCOSITY = 1.72e-5f; // Pa s, dynamic
constexpr float RIME_MIN_DENSITY = 100.0f; // kg/m3: soft rime
constexpr float RIME_MAX_DENSITY = 890.0f; // kg/m3: hard rime toward glaze
constexpr float MELT_RATE = 0.004f; // m/h of rime lost per C above 0 (sun and warm air together)
constexpr float SHOWN = 0.0008f; // m: thinner rime draws nothing
// A wide face still rimes: its grain, splinters and the first rime grains are small collectors of their own,
// and feathers grow from them (Makkonen 1984: rime on large structures grows from roughness elements). A
// collector this wide, at this share of its efficiency, sets the least a face catches.
constexpr float ROUGH_COLLECTOR = 0.01f; // m
constexpr float ROUGH_SHARE = 0.35f;
constexpr float CRUST_SHADE = 0.7f; // the crust's share of light under its feathers
constexpr float THIN_PART = 0.02f; // m: parts thinner than this keep all their feathers under the budget
} // namespace

float SnowRime3D::collision_efficiency(float p_wind, float p_droplet_um, float p_diameter) {
	const float d = MAX(p_droplet_um, 1.0f) * 1e-6f;
	const float v = MAX(p_wind, 0.0f);
	const float dc = MAX(p_diameter, 1e-4f);
	// The droplet's inertia parameter (Stokes number) on the cylinder, and Langmuir's parameter phi = Re^2 / K.
	const float k = WATER_DENSITY * d * d * v / (9.0f * AIR_VISCOSITY * dc);
	if (k <= 0.17f) {
		return 0.0f; // the droplets follow the air round it (Langmuir and Blodgett 1946's cut-off)
	}
	const float re = AIR_DENSITY * d * v / AIR_VISCOSITY;
	const float phi = re * re / k;
	const float a = 1.066f * Math::pow(k, -0.00616f) * Math::exp(-1.103f * Math::pow(k, -0.688f));
	const float b = 3.641f * Math::pow(k, -0.498f) * Math::exp(-1.497f * Math::pow(k, -0.694f));
	const float c = phi > 100.0f ? 0.00637f * Math::pow(phi - 100.0f, 0.381f) : 0.0f;
	return CLAMP(a - 0.028f - c * (b - 0.0454f), 0.0f, 1.0f);
}

float SnowRime3D::rime_density(float p_droplet_um, float p_impact_speed, float p_surface_celsius) {
	const float ts = MIN(p_surface_celsius, -0.5f);
	const float r = -p_droplet_um * MAX(p_impact_speed, 0.0f) / (2.0f * ts);
	return CLAMP(110.0f * Math::pow(MAX(r, 0.0f), 0.76f), RIME_MIN_DENSITY, RIME_MAX_DENSITY);
}

float SnowRime3D::accretion_rate(float p_wind, float p_fog_water, float p_celsius, float p_droplet_um, float p_diameter) {
	if (p_celsius >= 0.0f || p_fog_water <= 0.0f || p_wind <= 0.0f) {
		return 0.0f;
	}
	const float e = collision_efficiency(p_wind, p_droplet_um, p_diameter);
	// dM/dt = E w v per projected area (Makkonen 2000; sticking and accretion efficiencies 1 in dry growth). The
	// droplets strike at about the wind's speed times the efficiency's share of it.
	const float flux = e * p_fog_water * 1e-3f * p_wind; // kg/(m2 s)
	const float rho = rime_density(p_droplet_um, p_wind * MAX(e, 0.1f), p_celsius);
	return flux / rho * 3600.0f;
}

void SnowRime3D::_ensure_children() {
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT);
	}
	if (default_material.is_null()) {
		// Rime is ice full of air: white and matte, the air pockets scattering (albedo about 0.85, like old
		// snow), light through its thin feathers. Each feather is a vane card cut to a feather's outline with
		// its barbs (alpha tested, so both renderers cut the same edge), the way grass and fur cards draw fine
		// plates: geometry for the vane, the texture for the barbs too fine to model.
		default_material.instantiate();
		default_material->set_albedo(Color(0.88f, 0.9f, 0.93f));
		default_material->set_roughness(0.85f);
		default_material->set_specular(0.335f);
		default_material->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
		default_material->set_backlight(Color(0.55f, 0.58f, 0.62f));
		default_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
		default_material->set_alpha_scissor_threshold(0.35f);
		default_material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, _feather_texture());
		default_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	}
}

Ref<Texture2D> SnowRime3D::_feather_texture() {
	// The vane, u across (0.5 its rachis), v from root to tip: barbs leaving the rachis toward the tip at about
	// 40 degrees, an outline widest past the middle and pointed at the tip (rime feathers, Makkonen 2000's
	// photographs). White; alpha the coverage, a little soft so the mips keep the barbs' share.
	constexpr int W = 64, H = 128, SS = 4;
	Ref<Image> img = Image::create_empty(W, H, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			float cover = 0.0f;
			for (int sy = 0; sy < SS; sy++) {
				for (int sx = 0; sx < SS; sx++) {
					const float u = (x + (sx + 0.5f) / SS) / W;
					const float v = (y + (sy + 0.5f) / SS) / H;
					const float across = Math::abs(u - 0.5f) * 2.0f; // 0 at the rachis, 1 at the card's side
					// The outline: from a narrow root, widest at 0.6, closing to the tip.
					const float width = v < 0.6f ? 0.35f + 0.65f * Math::sin(float(Math::PI) * 0.5f * v / 0.6f) : Math::sqrt(MAX(1.0f - (v - 0.6f) / 0.4f, 0.0f));
					if (across > width) {
						continue;
					}
					if (across < 0.08f) {
						cover += 1.0f; // the rachis
						continue;
					}
					// Barbs: lines v + 0.84 across = const (about 40 degrees toward the tip), 7 along the vane (coarse
					// enough to stay open at a feather's few pixels), each tapering to its point at the outline.
					const float phase = (v - 0.84f * across * 0.5f) * 7.0f;
					const float f = phase - Math::floor(phase);
					if (f < 0.55f * (1.0f - 0.7f * across / MAX(width, 1e-3f))) {
						cover += 1.0f;
					}
				}
			}
			img->set_pixel(x, y, Color(1, 1, 1, cover / (SS * SS)));
		}
	}
	img->generate_mipmaps();
	return ImageTexture::create_from_image(img);
}

void SnowRime3D::_add_target(Node *p_node) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_node);
	if (mi && mi != mesh_instance && mi->get_mesh().is_valid()) {
		Ref<Mesh> mesh = mi->get_mesh();
		const Transform3D to_local = get_global_transform().affine_inverse() * mi->get_global_transform();
		Target t;
		t.node = mi->get_instance_id();
		const AABB box = mi->get_global_transform().xform(mesh->get_aabb());
		t.centre = box.get_center();
		// The collector: the part's smallest width (a post's, a rail's, a wire's).
		t.diameter = CLAMP(MIN(box.size.x, MIN(box.size.y, box.size.z)), 0.003f, 3.0f);
		HashMap<Vector3i, int> weld;
		LocalVector<Vector3> face_sum;
		for (int s = 0; s < mesh->get_surface_count(); s++) {
			if (mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			const Array arrays = mesh->surface_get_arrays(s);
			const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
			const PackedInt32Array idx = arrays[Mesh::ARRAY_INDEX];
			const int count = idx.is_empty() ? verts.size() : idx.size();
			for (int i = 0; i + 2 < count; i += 3) {
				int w[3];
				Vector3 p[3];
				for (int k = 0; k < 3; k++) {
					p[k] = to_local.xform(verts[idx.is_empty() ? i + k : idx[i + k]]);
					const Vector3i key(Math::round(p[k].x * 2000.0f), Math::round(p[k].y * 2000.0f), Math::round(p[k].z * 2000.0f));
					const HashMap<Vector3i, int>::Iterator it = weld.find(key);
					if (it) {
						w[k] = it->value;
					} else {
						w[k] = t.points.size();
						weld.insert(key, w[k]);
						t.points.push_back(p[k]);
						face_sum.push_back(Vector3());
						t.sharpness.push_back(1.0f); // the smallest cosine between its faces, turned below
					}
				}
				// Godot's front faces are clockwise: the outward normal is (c - a) x (b - a).
				const Vector3 n = (p[2] - p[0]).cross(p[1] - p[0]);
				if (n.length_squared() < 1e-14f) {
					continue;
				}
				for (int k = 0; k < 3; k++) {
					face_sum[w[k]] += n; // area weighted
					t.triangles.push_back(w[k]);
				}
			}
		}
		// The collector's width across the part's own long axis, not the world box's (a twig at 45 degrees has a
		// box as wide as it is long): principal axes of its points by power iteration on their covariance, the
		// smaller extent across the two minor ones.
		if (t.points.size() >= 4) {
			Vector3 mean;
			for (const Vector3 &q : t.points) {
				mean += q;
			}
			mean /= float(t.points.size());
			float cov[3][3] = {};
			for (const Vector3 &q : t.points) {
				const Vector3 d = q - mean;
				for (int r = 0; r < 3; r++) {
					for (int c = 0; c < 3; c++) {
						cov[r][c] += d[r] * d[c];
					}
				}
			}
			auto principal = [&](const Vector3 &p_not) {
				Vector3 v = Vector3(0.577f, 0.578f, 0.576f);
				v = (v - p_not * v.dot(p_not)).normalized();
				for (int it = 0; it < 48; it++) {
					Vector3 w(cov[0][0] * v.x + cov[0][1] * v.y + cov[0][2] * v.z, cov[1][0] * v.x + cov[1][1] * v.y + cov[1][2] * v.z, cov[2][0] * v.x + cov[2][1] * v.y + cov[2][2] * v.z);
					w -= p_not * w.dot(p_not);
					if (w.length_squared() < 1e-20f) {
						break;
					}
					v = w.normalized();
				}
				return v;
			};
			const Vector3 ax0 = principal(Vector3());
			const Vector3 ax1 = principal(ax0);
			const Vector3 ax2 = ax0.cross(ax1).normalized();
			float lo1 = 1e9f, hi1 = -1e9f, lo2 = 1e9f, hi2 = -1e9f;
			for (const Vector3 &q : t.points) {
				lo1 = MIN(lo1, q.dot(ax1));
				hi1 = MAX(hi1, q.dot(ax1));
				lo2 = MIN(lo2, q.dot(ax2));
				hi2 = MAX(hi2, q.dot(ax2));
			}
			t.diameter = CLAMP(MIN(MIN(hi1 - lo1, hi2 - lo2), t.diameter), 0.003f, 3.0f);
		}
		t.normals.resize(t.points.size());
		for (uint32_t i = 0; i < t.points.size(); i++) {
			t.normals[i] = face_sum[i].length_squared() > 0.0f ? face_sum[i].normalized() : Vector3(0, 1, 0);
		}
		// An edge or a corner: its faces turn away from the mean normal. Its collector is that much thinner.
		for (uint32_t i = 0; i < t.sharpness.size(); i++) {
			t.sharpness[i] = 0.0f;
		}
		for (uint32_t i = 0; i + 2 < t.triangles.size(); i += 3) {
			const Vector3 a = t.points[t.triangles[i]], b = t.points[t.triangles[i + 1]], c = t.points[t.triangles[i + 2]];
			const Vector3 fn = (c - a).cross(b - a).normalized();
			for (int k = 0; k < 3; k++) {
				const int v = t.triangles[i + k];
				t.sharpness[v] = MAX(t.sharpness[v], CLAMP(1.0f - fn.dot(t.normals[v]), 0.0f, 1.0f));
			}
		}
		t.thickness.resize(t.points.size());
		t.growth.resize(t.points.size());
		for (uint32_t i = 0; i < t.points.size(); i++) {
			t.thickness[i] = 0.0f;
			t.growth[i] = Vector3();
		}
		if (!t.triangles.is_empty()) {
			targets.push_back(t);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		if (c != mesh_instance) {
			_add_target(c);
		}
	}
}

void SnowRime3D::_collect_targets() {
	targets.clear();
	targets_dirty = false;
	if (!is_inside_tree()) {
		return;
	}
	for (int i = 0; i < target_paths.size(); i++) {
		Node *n = get_node_or_null(target_paths[i]);
		if (n) {
			_add_target(n);
		}
	}
	if (!target_group.is_empty()) {
		const Vector<Node *> nodes = get_tree()->get_nodes_in_group(StringName(target_group));
		for (Node *n : nodes) {
			_add_target(n);
		}
	}
}

void SnowRime3D::refresh_targets() {
	targets_dirty = true;
	_collect_targets();
	_build_mesh();
}

void SnowRime3D::grow(float p_hours, float p_celsius, const Vector3 &p_wind, float p_fog_water) {
	if (targets_dirty) {
		_collect_targets();
	}
	if (p_hours <= 0.0f || targets.is_empty()) {
		return;
	}
	const float speed = p_wind.length();
	const Vector3 into = speed > 1e-3f ? -get_global_transform().basis.xform_inv(p_wind).normalized() : Vector3();
	float most = 0.0f;
	float change = 0.0f;
	last_density = p_celsius < 0.0f ? rime_density(droplet_diameter, speed, p_celsius) : 0.0f;
	for (Target &t : targets) {
		for (uint32_t i = 0; i < t.points.size(); i++) {
			float dt = 0.0f;
			if (p_celsius < 0.0f) {
				const float facing = into.is_zero_approx() ? 0.0f : MAX(t.normals[i].dot(into), 0.0f);
				if (facing > 0.0f) {
					// Edges and corners are thinner collectors than the part's width: they catch more. The rime
					// itself widens the collector as it builds, so the growth slows (Makkonen's time-dependent
					// accretion).
					const float d = (t.diameter + 2.0f * t.thickness[i]) * (1.0f - 0.9f * t.sharpness[i]);
					const float rough = ROUGH_SHARE * accretion_rate(speed, p_fog_water, p_celsius, droplet_diameter, ROUGH_COLLECTOR + 2.0f * t.thickness[i]);
					dt = MAX(accretion_rate(speed, p_fog_water, p_celsius, droplet_diameter, d), rough) * facing * p_hours;
					most = MAX(most, dt / p_hours);
					t.growth[i] += into * dt;
				}
			} else {
				dt = -MIN(MELT_RATE * p_celsius * p_hours, t.thickness[i]);
			}
			const float before = t.thickness[i];
			t.thickness[i] = CLAMP(before + dt, 0.0f, max_thickness);
			change = MAX(change, Math::abs(t.thickness[i] - before));
		}
	}
	last_rate = most;
	drawn_change += change;
	if (drawn_change > 0.002f) {
		drawn_change = 0.0f;
		_build_mesh();
	}
}

void SnowRime3D::advance(float p_hours) {
	if (targets_dirty) {
		_collect_targets();
	}
	if (targets.is_empty() || !is_inside_tree()) {
		return;
	}
	const WorldWeather *weather = WorldContracts::weather()->has_provider() ? WorldContracts::weather() : nullptr;
	const SnowCover3D *cover = SnowCover3D::get_active();
	Vector3 at;
	for (const Target &t : targets) {
		at += t.centre / float(targets.size());
	}
	const float celsius = cover ? cover->get_air_temperature_at(at) : (weather ? weather->air_temperature_at(at) : 0.0f);
	const Vector3 wind = weather ? weather->wind_at(at) : Vector3();
	const float water = fog_water >= 0.0f ? fog_water : (weather ? weather->fog_water_at(at) : 0.0f);
	grow(p_hours, celsius, wind, water);
}

void SnowRime3D::clear() {
	for (Target &t : targets) {
		for (uint32_t i = 0; i < t.points.size(); i++) {
			t.thickness[i] = 0.0f;
			t.growth[i] = Vector3();
		}
	}
	_build_mesh();
}

void SnowRime3D::_build_mesh() {
	_ensure_children();
	if (!is_inside_tree()) {
		return;
	}
	// The feathers' budget: their count at the spacing over all rimed area, thinned evenly (and widened) past it.
	const float density = 1.0f / (feather_spacing * feather_spacing);
	float expected = 0.0f;
	// A thin part (a wire, a twig) has little surface but grows a full vane along it: its feathers closer.
	auto part_density = [&](const Target &p_t) {
		return density * CLAMP(0.03f / p_t.diameter, 1.0f, 10.0f);
	};
	for (const Target &t : targets) {
		for (uint32_t i = 0; i + 2 < t.triangles.size(); i += 3) {
			const int a = t.triangles[i], b = t.triangles[i + 1], c = t.triangles[i + 2];
			if (MAX(t.thickness[a], MAX(t.thickness[b], t.thickness[c])) > SHOWN) {
				expected += 0.5f * (t.points[c] - t.points[a]).cross(t.points[b] - t.points[a]).length() * part_density(t);
			}
		}
	}
	// Thin parts (twigs, wires) keep every feather: their vanes are what shows on them. The rest share what is
	// left.
	float thin_expected = 0.0f;
	for (const Target &t : targets) {
		if (t.diameter >= THIN_PART) {
			continue;
		}
		for (uint32_t i = 0; i + 2 < t.triangles.size(); i += 3) {
			const int a = t.triangles[i], b = t.triangles[i + 1], c = t.triangles[i + 2];
			if (MAX(t.thickness[a], MAX(t.thickness[b], t.thickness[c])) > SHOWN) {
				thin_expected += 0.5f * (t.points[c] - t.points[a]).cross(t.points[b] - t.points[a]).length() * part_density(t);
			}
		}
	}
	const float thin_keep = thin_expected > 0.5f * float(max_feathers) ? 0.5f * float(max_feathers) / thin_expected : 1.0f;
	const float left = MAX(float(max_feathers) - thin_expected * thin_keep, 0.0f);
	const float wide_expected = expected - thin_expected;
	const float wide_keep = wide_expected > left ? left / MAX(wide_expected, 1.0f) : 1.0f;
	PackedVector2Array uvs;
	PackedColorArray colors;
	PackedVector3Array verts;
	PackedVector3Array normals;
	auto tri = [&](const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
		Vector3 n = (p_c - p_a).cross(p_b - p_a);
		if (n.length_squared() < 1e-20f) {
			return;
		}
		n.normalize();
		verts.push_back(p_a);
		verts.push_back(p_b);
		verts.push_back(p_c);
		normals.push_back(n);
		normals.push_back(n);
		normals.push_back(n);
		for (int k = 0; k < 3; k++) {
			uvs.push_back(Vector2(0.5f, 0.5f)); // the vane's solid middle: the crust is opaque
			colors.push_back(Color(CRUST_SHADE, CRUST_SHADE, CRUST_SHADE)); // shaded by the feathers over it
		}
	};
	// A feather's triangle, with its own normals and vane UVs.
	auto vane_tri = [&](const Vector3 *p_p, const Vector3 *p_n, const Vector2 *p_uv, const float *p_shade) {
		for (int k = 0; k < 3; k++) {
			verts.push_back(p_p[k]);
			normals.push_back(p_n[k]);
			uvs.push_back(p_uv[k]);
			colors.push_back(Color(p_shade[k], p_shade[k], p_shade[k]));
		}
	};
	for (uint32_t ti = 0; ti < targets.size(); ti++) {
		const Target &t = targets[ti];
		auto crust_at = [&](int p_v) {
			// The crust under the feathers: a third of the deposit up to 3 mm (the rest is the feathers), at least
			// 1 mm off the surface.
			return t.points[p_v] + t.normals[p_v] * CLAMP(0.3f * t.thickness[p_v], 0.001f, 0.003f);
		};
		for (uint32_t i = 0; i + 2 < t.triangles.size(); i += 3) {
			const int v[3] = { t.triangles[i], t.triangles[i + 1], t.triangles[i + 2] };
			const float th[3] = { t.thickness[v[0]], t.thickness[v[1]], t.thickness[v[2]] };
			if (MAX(th[0], MAX(th[1], th[2])) <= SHOWN) {
				continue;
			}
			const Vector3 c0 = crust_at(v[0]), c1 = crust_at(v[1]), c2 = crust_at(v[2]);
			tri(c0, c1, c2);
			// Feathers: spikes rooted on the crust, pointing the way the rime grew (into the wind), each a
			// little off it and of its own length; the same ones every rebuild (seeded per triangle).
			RandomPCG rng(hash_murmur3_one_32(i, hash_murmur3_one_32(ti, uint32_t(seed))));
			const float area = 0.5f * (t.points[v[2]] - t.points[v[0]]).cross(t.points[v[1]] - t.points[v[0]]).length();
			const float keep = MAX(t.diameter < THIN_PART ? thin_keep : wide_keep, 0.02f);
			const float widen = 1.0f / Math::sqrt(keep);
			const float want = area * part_density(t) * keep;
			int count = int(want);
			if (rng.randf() < want - count) {
				count++;
			}
			for (int f = 0; f < count; f++) {
				float u = rng.randf(), w = rng.randf();
				if (u + w > 1.0f) {
					u = 1.0f - u;
					w = 1.0f - w;
				}
				const float b0 = 1.0f - u - w;
				const float thick = th[0] * b0 + th[1] * u + th[2] * w;
				const float jitter = rng.randf();
				if (thick <= SHOWN) {
					continue;
				}
				const Vector3 root = c0 * b0 + c1 * u + c2 * w;
				const Vector3 n = (t.normals[v[0]] * b0 + t.normals[v[1]] * u + t.normals[v[2]] * w).normalized();
				Vector3 g = t.growth[v[0]] * b0 + t.growth[v[1]] * u + t.growth[v[2]] * w;
				g = g.length_squared() > 1e-12f ? g.normalized() : n;
				// Feathers lean out of the surface a little whichever way the wind came.
				Vector3 dir = (g * 0.8f + n * 0.2f).normalized();
				const Vector3 side_a = dir.cross(Math::abs(dir.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0)).normalized();
				const Vector3 side_b = dir.cross(side_a);
				const float spin = rng.randf() * Math::TAU;
				dir = (dir + (side_a * Math::cos(spin) + side_b * Math::sin(spin)) * (0.25f * rng.randf())).normalized();
				const float length = MAX(thick - 0.003f, 0.5f * thick) * (0.6f + 0.8f * jitter);
				const float half = CLAMP(0.3f * length, 0.0008f, 0.01f) * widen;
				const Vector3 tip = root + dir * length;
				Vector3 s0 = dir.cross(n);
				if (s0.length_squared() < 1e-6f) {
					s0 = side_a;
				}
				s0.normalize();
				// A flat vane across the wind's sideways direction (a rime feather grows as a plate), turned a little
				// about its axis each, drawn from both sides. Its normals bend from the surface's at the root to the
				// growth direction at the tip, so the plates shade as soft tufts, not facets (as grass cards do).
				const float roll = (rng.randf() - 0.5f) * 0.9f;
				const Vector3 s1 = dir.cross(s0).normalized();
				s0 = (s0 * Math::cos(roll) + s1 * Math::sin(roll)).normalized();
				const Vector3 l = root + s0 * half;
				const Vector3 r = root - s0 * half;
				const Vector3 pts[4] = { root - dir * (0.05f * length), l + dir * (0.6f * length), tip, r + dir * (0.6f * length) };
				const Vector2 uv[4] = { Vector2(0.5f, 0.0f), Vector2(0.0f, 0.6f), Vector2(0.5f, 1.0f), Vector2(1.0f, 0.6f) };
				// Occlusion down the tuft (its neighbours shade a feather's root, its tip is open), and each feather a
				// little brighter or duller, as the grains in it are packed: depth instead of one flat white.
				const float own = 0.86f + 0.14f * rng.randf();
				const float shade[4] = { 0.62f * own, 0.86f * own, own, 0.86f * own };
				const Vector3 vn = s0.cross(dir).normalized(); // the vane's own normal (one side)
				// Clockwise from one side, then the other.
				const int sides[2][6] = { { 0, 1, 2, 0, 2, 3 }, { 0, 2, 1, 0, 3, 2 } };
				for (int sd = 0; sd < 2; sd++) {
					const Vector3 face_n = sd == 0 ? vn : -vn;
					const Vector3 nr = (n * 0.8f + face_n * 0.2f).normalized();
					const Vector3 nt = (dir * 0.5f + n * 0.2f + face_n * 0.3f).normalized();
					const Vector3 nm = (nr + nt).normalized();
					const Vector3 nn[4] = { nr, nm, nt, nm };
					for (int q = 0; q < 2; q++) {
						const int *o = &sides[sd][q * 3];
						const Vector3 tp[3] = { pts[o[0]], pts[o[1]], pts[o[2]] };
						const Vector3 tn[3] = { nn[o[0]], nn[o[1]], nn[o[2]] };
						const Vector2 tu[3] = { uv[o[0]], uv[o[1]], uv[o[2]] };
						const float ts[3] = { shade[o[0]], shade[o[1]], shade[o[2]] };
						vane_tri(tp, tn, tu, ts);
					}
				}
			}
		}
	}
	Ref<ArrayMesh> mesh;
	if (!verts.is_empty()) {
		mesh.instantiate();
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = verts;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TEX_UV] = uvs;
		arrays[Mesh::ARRAY_COLOR] = colors;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_material(0, material.is_valid() ? material : Ref<Material>(default_material));
	}
	mesh_instance->set_mesh(mesh);
}

int SnowRime3D::get_feather_count() const {
	if (!mesh_instance || mesh_instance->get_mesh().is_null()) {
		return 0;
	}
	int crust = 0;
	for (const Target &t : targets) {
		for (uint32_t i = 0; i + 2 < t.triangles.size(); i += 3) {
			if (MAX(t.thickness[t.triangles[i]], MAX(t.thickness[t.triangles[i + 1]], t.thickness[t.triangles[i + 2]])) > SHOWN) {
				crust++;
			}
		}
	}
	const int tris = mesh_instance->get_mesh()->surface_get_array_len(0) / 3;
	return MAX(tris - crust, 0) / 4;
}

float SnowRime3D::get_target_thickness(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)targets.size(), 0.0f);
	float m = 0.0f;
	for (float th : targets[p_index].thickness) {
		m = MAX(m, th);
	}
	return m;
}

Vector3 SnowRime3D::get_growth_direction(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)targets.size(), Vector3());
	const Target &t = targets[p_index];
	float m = 0.0f;
	Vector3 g;
	for (uint32_t i = 0; i < t.points.size(); i++) {
		if (t.thickness[i] > m) {
			m = t.thickness[i];
			g = t.growth[i];
		}
	}
	return g.length_squared() > 0.0f ? get_global_transform().basis.xform(g).normalized() : Vector3();
}

float SnowRime3D::get_thickness_at(const Vector3 &p_position) const {
	const Vector3 local = get_global_transform().affine_inverse().xform(p_position);
	float best = 0.25f; // 0.5 m, squared
	float th = 0.0f;
	for (const Target &t : targets) {
		for (uint32_t i = 0; i < t.points.size(); i++) {
			const float d = t.points[i].distance_squared_to(local);
			if (d < best) {
				best = d;
				th = t.thickness[i];
			}
		}
	}
	return th;
}

void SnowRime3D::set_target_paths(const TypedArray<NodePath> &p_paths) {
	target_paths = p_paths;
	targets_dirty = true;
}

void SnowRime3D::set_target_group(const String &p_group) {
	target_group = p_group;
	targets_dirty = true;
}

void SnowRime3D::set_feather_spacing(float p_m) {
	feather_spacing = MAX(p_m, 0.002f);
	if (is_inside_tree()) {
		_build_mesh();
	}
}

void SnowRime3D::set_max_feathers(int p_count) {
	max_feathers = MAX(p_count, 0);
	if (is_inside_tree()) {
		_build_mesh();
	}
}

void SnowRime3D::set_seed(int p_seed) {
	seed = p_seed;
	if (is_inside_tree()) {
		_build_mesh();
	}
}

void SnowRime3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	if (is_inside_tree()) {
		_build_mesh();
	}
}

void SnowRime3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_ensure_children();
			targets_dirty = true;
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// Grow with the snowpack's clock: each of its updates.
			const SnowCover3D *cover = SnowCover3D::get_active();
			if (cover && cover->get_update_count() != seen_update) {
				const bool first = seen_update == 0;
				seen_update = cover->get_update_count();
				if (!first) {
					advance(cover->get_last_step_hours());
				}
			}
		} break;
	}
}

void SnowRime3D::_bind_methods() {
	ClassDB::bind_static_method("SnowRime3D", D_METHOD("collision_efficiency", "wind", "droplet_um", "diameter"), &SnowRime3D::collision_efficiency);
	ClassDB::bind_static_method("SnowRime3D", D_METHOD("rime_density", "droplet_um", "impact_speed", "surface_celsius"), &SnowRime3D::rime_density);
	ClassDB::bind_static_method("SnowRime3D", D_METHOD("accretion_rate", "wind", "fog_water", "celsius", "droplet_um", "diameter"), &SnowRime3D::accretion_rate);
	ClassDB::bind_method(D_METHOD("advance", "hours"), &SnowRime3D::advance);
	ClassDB::bind_method(D_METHOD("grow", "hours", "celsius", "wind", "fog_water"), &SnowRime3D::grow);
	ClassDB::bind_method(D_METHOD("clear"), &SnowRime3D::clear);
	ClassDB::bind_method(D_METHOD("refresh_targets"), &SnowRime3D::refresh_targets);
	ClassDB::bind_method(D_METHOD("get_target_count"), &SnowRime3D::get_target_count);
	ClassDB::bind_method(D_METHOD("get_target_thickness", "index"), &SnowRime3D::get_target_thickness);
	ClassDB::bind_method(D_METHOD("get_target_diameter", "index"), &SnowRime3D::get_target_diameter);
	ClassDB::bind_method(D_METHOD("get_thickness_at", "position"), &SnowRime3D::get_thickness_at);
	ClassDB::bind_method(D_METHOD("get_growth_direction", "index"), &SnowRime3D::get_growth_direction);
	ClassDB::bind_method(D_METHOD("get_feather_count"), &SnowRime3D::get_feather_count);
	ClassDB::bind_method(D_METHOD("get_last_rate"), &SnowRime3D::get_last_rate);
	ClassDB::bind_method(D_METHOD("get_last_density"), &SnowRime3D::get_last_density);
	ClassDB::bind_method(D_METHOD("set_target_paths", "paths"), &SnowRime3D::set_target_paths);
	ClassDB::bind_method(D_METHOD("get_target_paths"), &SnowRime3D::get_target_paths);
	ClassDB::bind_method(D_METHOD("set_target_group", "group"), &SnowRime3D::set_target_group);
	ClassDB::bind_method(D_METHOD("get_target_group"), &SnowRime3D::get_target_group);
	ClassDB::bind_method(D_METHOD("set_droplet_diameter", "um"), &SnowRime3D::set_droplet_diameter);
	ClassDB::bind_method(D_METHOD("get_droplet_diameter"), &SnowRime3D::get_droplet_diameter);
	ClassDB::bind_method(D_METHOD("set_fog_water", "value"), &SnowRime3D::set_fog_water);
	ClassDB::bind_method(D_METHOD("get_fog_water"), &SnowRime3D::get_fog_water);
	ClassDB::bind_method(D_METHOD("set_feather_spacing", "spacing"), &SnowRime3D::set_feather_spacing);
	ClassDB::bind_method(D_METHOD("get_feather_spacing"), &SnowRime3D::get_feather_spacing);
	ClassDB::bind_method(D_METHOD("set_max_feathers", "count"), &SnowRime3D::set_max_feathers);
	ClassDB::bind_method(D_METHOD("get_max_feathers"), &SnowRime3D::get_max_feathers);
	ClassDB::bind_method(D_METHOD("set_max_thickness", "thickness"), &SnowRime3D::set_max_thickness);
	ClassDB::bind_method(D_METHOD("get_max_thickness"), &SnowRime3D::get_max_thickness);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SnowRime3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &SnowRime3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &SnowRime3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &SnowRime3D::get_material);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "target_paths", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_target_paths", "get_target_paths");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "target_group"), "set_target_group", "get_target_group");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "droplet_diameter", PROPERTY_HINT_RANGE, "1,100,0.5,suffix:um"), "set_droplet_diameter", "get_droplet_diameter");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fog_water", PROPERTY_HINT_RANGE, "-1,2,0.01,suffix:g/m3"), "set_fog_water", "get_fog_water");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "feather_spacing", PROPERTY_HINT_RANGE, "0.002,0.1,0.001,suffix:m"), "set_feather_spacing", "get_feather_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_feathers", PROPERTY_HINT_RANGE, "0,500000,1"), "set_max_feathers", "get_max_feathers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_thickness", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m"), "set_max_thickness", "get_max_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
}
