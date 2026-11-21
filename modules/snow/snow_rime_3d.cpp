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
#include "scene/resources/mesh.h"

namespace {
constexpr float WATER_DENSITY = 1000.0f; // kg/m3
constexpr float AIR_DENSITY = 1.3f; // kg/m3 near -5 C
constexpr float AIR_VISCOSITY = 1.72e-5f; // Pa s, dynamic
constexpr float RIME_MIN_DENSITY = 100.0f; // kg/m3: soft rime
constexpr float RIME_MAX_DENSITY = 890.0f; // kg/m3: hard rime toward glaze
constexpr float MELT_RATE = 0.004f; // m/h of rime lost per C above 0 (sun and warm air together)
constexpr float SHOWN = 0.0008f; // m: thinner rime draws nothing
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
		// snow), a little light through its thin feathers.
		default_material.instantiate();
		default_material->set_albedo(Color(0.88f, 0.9f, 0.93f));
		default_material->set_roughness(0.8f);
		default_material->set_specular(0.335f);
		default_material->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
		default_material->set_backlight(Color(0.35f, 0.38f, 0.42f));
	}
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
					dt = accretion_rate(speed, p_fog_water, p_celsius, droplet_diameter, d) * facing * p_hours;
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
	const float keep = expected > float(max_feathers) ? float(max_feathers) / expected : 1.0f;
	const float widen = 1.0f / Math::sqrt(keep);
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
				const float half = CLAMP(0.22f * length, 0.0006f, 0.008f) * widen;
				const Vector3 tip = root + dir * length;
				Vector3 s0 = dir.cross(n);
				if (s0.length_squared() < 1e-6f) {
					s0 = side_a;
				}
				s0.normalize();
				const Vector3 s1 = dir.cross(s0).normalized();
				// A flat vane (a rime feather grows as a plate across the wind's sideways direction), with a ridge
				// so it is not a card from the side.
				const Vector3 l = root + s0 * half;
				const Vector3 r = root - s0 * half;
				const Vector3 ridge = root + s1 * (0.35f * half);
				const Vector3 back = root - s1 * (0.35f * half);
				// Wound to face out of the vane (clockwise seen from outside), so both renderers see its outer faces.
				const Vector3 mid = root + dir * (0.25f * length);
				auto face = [&](const Vector3 &p_a, const Vector3 &p_b) {
					const Vector3 n = (tip - p_a).cross(p_b - p_a);
					if (n.dot((p_a + p_b + tip) / 3.0f - mid) >= 0.0f) {
						tri(p_a, p_b, tip);
					} else {
						tri(p_b, p_a, tip);
					}
				};
				face(l, ridge);
				face(ridge, r);
				face(r, back);
				face(back, l);
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
