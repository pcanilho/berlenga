/**************************************************************************/
/*  physx_waterfalls_3d.cpp                                               */
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

#include "physx_waterfalls_3d.h"

#include "physx_river_3d.h"
#include "physx_water_mist_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"
#include "scene/3d/gpu_particles_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "scene/resources/particle_process_material.h"

namespace {
constexpr float GRAVITY = 9.81f;

// Centripetal Catmull-Rom (alpha 0.5) through 2D points, 8 pieces per span, so a face sampled every few metres
// bends smoothly instead of in polyline corners.
PackedVector2Array smooth_profile(const PackedVector2Array &p_pts) {
	const int n = p_pts.size();
	if (n < 3) {
		return p_pts;
	}
	PackedVector2Array out;
	auto knot = [](float t, const Vector2 &a, const Vector2 &b) {
		return t + Math::sqrt(MAX(a.distance_to(b), 1e-4f));
	};
	for (int i = 0; i + 1 < n; i++) {
		const Vector2 p0 = p_pts[MAX(i - 1, 0)];
		const Vector2 p1 = p_pts[i];
		const Vector2 p2 = p_pts[i + 1];
		const Vector2 p3 = p_pts[MIN(i + 2, n - 1)];
		const float t0 = 0.0f;
		const float t1 = knot(t0, p0, p1);
		const float t2 = knot(t1, p1, p2);
		const float t3 = knot(t2, p2, p3);
		for (int k = 0; k < 8; k++) {
			const float t = Math::lerp(t1, t2, k / 8.0f);
			const Vector2 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
			const Vector2 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
			const Vector2 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
			const Vector2 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
			const Vector2 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
			out.push_back(b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1)));
		}
	}
	out.push_back(p_pts[n - 1]);
	return out;
}

// The point at arc length fraction p_f of a 2D polyline.
Vector2 at_fraction(const PackedVector2Array &p_pts, const PackedFloat32Array &p_arc, float p_f) {
	const int n = p_pts.size();
	const float target = p_f * p_arc[n - 1];
	int lo = 0;
	int hi = n - 1;
	while (hi - lo > 1) {
		const int mid = (lo + hi) / 2;
		if (p_arc[mid] <= target) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float span = p_arc[hi] - p_arc[lo];
	return p_pts[lo].lerp(p_pts[hi], span > 1e-6f ? (target - p_arc[lo]) / span : 0.0f);
}

GPUParticles3D *make_spray(const Transform3D &p_xf, float p_speed, float p_width, float p_power) {
	GPUParticles3D *spray = memnew(GPUParticles3D);
	spray->set_name("FallSpray");
	spray->set_transform(p_xf);
	spray->set_amount(CLAMP((int)(p_power / 200.0f), 32, 4096));
	const float rise = 0.35f * p_speed;
	spray->set_lifetime(MAX(0.6f, 2.0f * rise / GRAVITY));
	spray->set_pre_process_time(2.0);
	spray->set_visibility_aabb(AABB(Vector3(-p_width - 4.0f, -1.0f, -6.0f), Vector3(2.0f * p_width + 8.0f, rise * rise / GRAVITY + 3.0f, 12.0f)));
	Ref<ParticleProcessMaterial> pm;
	pm.instantiate();
	pm->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX);
	pm->set_emission_box_extents(Vector3(0.5f * p_width, 0.1f, 0.3f));
	pm->set_direction(Vector3(0, 1, -0.35f));
	pm->set_spread(40.0f);
	pm->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.4f * rise);
	pm->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, rise);
	pm->set_gravity(Vector3(0, -GRAVITY, 0));
	pm->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.5f);
	pm->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 1.5f);
	pm->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.04f);
	pm->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 0.12f);
	spray->set_process_material(pm);
	Ref<SphereMesh> drop;
	drop.instantiate();
	drop->set_radius(0.5f);
	drop->set_height(1.0f);
	drop->set_radial_segments(6);
	drop->set_rings(3);
	Ref<StandardMaterial3D> dm;
	dm.instantiate();
	dm->set_albedo(Color(0.95f, 0.97f, 1.0f));
	dm->set_roughness(0.25f);
	drop->set_material(dm);
	spray->set_draw_pass_mesh(0, drop);
	return spray;
}
} // namespace

// Self-aeration on a steep chute. The turbulent boundary layer grows from the lip and reaches the surface at the
// inception point, L_i / k_s = 13.6 (sin a)^0.0796 F*^0.713, F* = q / sqrt(g sin a k_s^3) (Wood, Ackers and
// Loveless 1983; Chanson 1993). Below it the flow draws in air towards C_e = 0.9 sin a (Chanson 1993, smooth
// chutes up to about 75 degrees; capped at 0.9 for steeper faces).
float PhysXWaterfalls3D::inception_length(float p_q, float p_sin_slope, float p_ks) {
	const float s = CLAMP(p_sin_slope, 0.05f, 1.0f);
	const float ks = MAX(p_ks, 1e-3f);
	const float f_star = MAX(p_q, 1e-5f) / Math::sqrt(GRAVITY * s * ks * ks * ks);
	return 13.6f * ks * Math::pow(s, 0.0796f) * Math::pow(f_star, 0.713f);
}

float PhysXWaterfalls3D::equilibrium_air(float p_sin_slope) {
	return MIN(0.9f * CLAMP(p_sin_slope, 0.0f, 1.0f), 0.9f);
}

void PhysXWaterfalls3D::_build_face(const Dictionary &p_fall, Object *p_source) {
	Face face;
	face.id = p_fall.get("id", String());
	face.type = p_fall.get("type", String());
	face.top = p_fall.get("top", Vector3());
	face.foot = p_fall.get("bottom", Vector3());
	face.height = p_fall.get("height", 0.0f);
	face.discharge = p_fall.get("discharge", 0.0f);
	face.width = p_fall.get("lip_width", p_fall.get("width", 2.0f));
	face.impact_speed = p_fall.get("impact_speed", 0.0f);
	Vector3 dir = p_fall.get("direction", Vector3(0, 0, 1));
	dir.y = 0.0f;
	dir = dir.length_squared() > 1e-8f ? dir.normalized() : Vector3(0, 0, 1);
	face.direction = dir;
	const Vector3 across(-dir.z, 0.0f, dir.x);
	const PackedVector3Array lip = p_fall.get("lip_points", PackedVector3Array());
	const PackedFloat32Array share = p_fall.get("lip_share", PackedFloat32Array());
	const Array profiles = p_fall.get("face_profiles", Array());
	if (lip.size() < 2 || share.size() != lip.size() || profiles.size() != 3 || face.discharge <= 0.0f) {
		return;
	}
	const float lw = MAX(face.width, 1.0f);
	const float spacing = lw / (lip.size() - 1);
	// The wet part of the lip: the brink points no higher than the water over the lowest one (a gorge's lip line
	// runs up its walls). Their shares come from the record, or from the water depth over each.
	int lowest = 0;
	for (int c = 1; c < lip.size(); c++) {
		if (lip[c].y < lip[lowest].y) {
			lowest = c;
		}
	}
	const float yc = Math::pow(face.discharge / lw, 2.0f / 3.0f) / Math::pow(GRAVITY, 1.0f / 3.0f);
	const float surface = lip[lowest].y + MAX(1.5f * yc, 0.25f);
	int c0 = lowest;
	int c1 = lowest;
	while (c0 > 0 && lip[c0 - 1].y <= surface) {
		c0--;
	}
	while (c1 < lip.size() - 1 && lip[c1 + 1].y <= surface) {
		c1++;
	}
	// At least two columns: a dry neighbour carries a thin edge.
	if (c1 == c0) {
		if (c1 < lip.size() - 1) {
			c1++;
		} else {
			c0--;
		}
	}
	const int cols = c1 - c0 + 1;
	PackedFloat32Array wet_share;
	float total = 0.0f;
	for (int c = c0; c <= c1; c++) {
		const float sh = MAX(share[c], 0.0f) * Math::pow(MAX(surface - lip[c].y, 0.01f), 5.0f / 3.0f);
		wet_share.push_back(sh);
		total += sh;
	}
	int main_col = 0; // the column carrying the most water speaks for the fall in get_contact_falls()
	for (int c = 0; c < cols; c++) {
		wet_share.set(c, total > 0.0f ? wet_share[c] / total : 1.0f / cols);
		if (wet_share[c] > wet_share[main_col]) {
			main_col = c;
		}
	}
	face.width = spacing * (cols - 1);
	// The line down the face under each column, as (run from the lip, height below it): the ground itself where
	// the source can be asked for heights (get_height), else between the record's three profiles (at -0.35, 0 and
	// +0.35 of the lip width). Cut a little past the foot and smoothed with a centripetal Catmull-Rom spline.
	const bool ask = p_source != nullptr && p_source->has_method("get_height");
	float reach = 0.0f;
	for (int l = 0; l < 3; l++) {
		const PackedVector3Array prof = profiles[l];
		if (prof.size() < 2) {
			return;
		}
		reach = MAX(reach, prof[prof.size() - 1].x);
	}
	LocalVector<PackedVector2Array> lines;
	LocalVector<PackedFloat32Array> arcs;
	float longest = 0.0f;
	float sin_face = 1.0f;
	for (int c = c0; c <= c1; c++) {
		PackedVector2Array pts;
		if (ask) {
			const int n = CLAMP((int)Math::ceil(reach / 0.5f), 2, 2048);
			for (int i = 0; i <= n; i++) {
				const float run = reach * i / n;
				const Vector3 q = Vector3(lip[c].x, 0.0f, lip[c].z) + dir * run;
				const float y = float(p_source->call("get_height", q)) - face.top.y;
				pts.push_back(Vector2(run, y));
				if (y <= -face.height - 1.0f) {
					break;
				}
			}
		} else {
			const float o = (float(c) / (lip.size() - 1) - 0.5f) * lw;
			const float f = CLAMP(o / (0.35f * lw), -1.0f, 1.0f);
			const PackedVector3Array pa = profiles[f < 0.0f ? 0 : 1];
			const PackedVector3Array pb = profiles[f < 0.0f ? 1 : 2];
			const float w = f < 0.0f ? f + 1.0f : f;
			for (int i = 0; i < MIN(pa.size(), pb.size()); i++) {
				const Vector2 pt = Vector2(pa[i].x, pa[i].y).lerp(Vector2(pb[i].x, pb[i].y), w);
				pts.push_back(pt);
				if (pt.y <= -face.height - 1.0f) {
					break;
				}
			}
		}
		const PackedVector2Array line = pts.size() > 2 && !ask ? smooth_profile(pts) : pts;
		PackedFloat32Array arc;
		arc.push_back(0.0f);
		for (int i = 1; i < line.size(); i++) {
			arc.push_back(arc[i - 1] + line[i].distance_to(line[i - 1]));
		}
		if (line.size() < 2 || -line[line.size() - 1].y < 0.5f * face.height) {
			return; // A line that stops short of the foot: no face to drape over.
		}
		longest = MAX(longest, arc[arc.size() - 1]);
		if (c - c0 == main_col) {
			sin_face = CLAMP(face.height / MAX(arc[arc.size() - 1], 1e-3f), 0.05f, 1.0f);
		}
		lines.push_back(line);
		arcs.push_back(arc);
	}
	const int rows = CLAMP((int)Math::ceil(longest / MAX(row_spacing, 0.25f)) + 1, 2, 1024);
	const Vector3 up(0, 1, 0);
	for (int c = 0; c < cols; c++) {
		Column col;
		col.q = face.discharge * wet_share[c] / spacing;
		const PackedVector2Array &line = lines[c];
		const PackedFloat32Array &arc = arcs[c];
		const float len = arc[arc.size() - 1];
		const Vector3 lip_c = lip[c0 + c];
		const float v0 = Math::pow(GRAVITY * MAX(col.q, 1e-5f), 1.0f / 3.0f);
		const float li = inception_length(col.q, sin_face, rock_roughness);
		float air = 0.0f;
		float travel = 0.0f;
		float prev_v = v0;
		Vector2 prev_p;
		for (int r = 0; r < rows; r++) {
			const float fr = float(r) / (rows - 1);
			const Vector2 p = at_fraction(line, arc, fr);
			// The bed's direction over about a metre either side (the line is sampled finer than that).
			const float span = MAX(1.0f / MAX(len, 1e-3f), 0.5f / rows);
			const Vector2 pn = at_fraction(line, arc, MIN(fr + span, 1.0f));
			const Vector2 pp = at_fraction(line, arc, MAX(fr - span, 0.0f));
			Vector2 t = pn - pp;
			t = t.length_squared() > 1e-10f ? t.normalized() : Vector2(0, -1);
			const float sin_a = CLAMP(-t.y, 0.02f, 1.0f);
			const float ds = r > 0 ? p.distance_to(prev_p) : 0.0f;
			// Speed: the sheet's momentum along the bed, gravity down the slope against Darcy-Weisbach friction on
			// the depth q / v (uniform equilibrium flow on chutes, Chanson 1993): dv/ds = (g sin a - f v^3 / 8q) / v.
			// It tends to the normal speed (8 g q sin a / f)^(1/3) within a few metres and slows on the flatter foot.
			float v = prev_v;
			if (r > 0) {
				const int sub = 8;
				const float h = ds / sub;
				for (int k = 0; k < sub; k++) {
					const float acc = GRAVITY * sin_a - friction_factor * v * v * v / (8.0f * MAX(col.q, 1e-5f));
					v = Math::sqrt(MAX(v * v + 2.0f * acc * h, 0.09f));
				}
			}
			if (r > 0) {
				travel += ds / MAX(0.5f * (v + prev_v), 0.1f);
			}
			// Air: none above the inception point, then towards C_e for the local slope over about 5 m (it
			// detrains on the gentler steps).
			const float s_from_lip = fr * len;
			const float target = s_from_lip > li ? equilibrium_air(sin_a) : 0.0f;
			air += (target - air) * (1.0f - Math::exp(-ds / 5.0f));
			// Bulked thickness over the rock: the clear-water depth swelled by the air it carries.
			const float depth = MAX(col.q / MAX(v, 0.1f), 0.003f) / (1.0f - MIN(air, 0.9f));
			const Vector2 n2 = Vector2(-t.y, t.x).y < 0.0f ? Vector2(t.y, -t.x) : Vector2(-t.y, t.x);
			const Vector3 base = Vector3(lip_c.x, face.top.y, lip_c.z) + dir * p.x + up * p.y;
			const Vector3 nrm = (dir * n2.x + up * n2.y).normalized();
			// Clear of the ground mesh, whose triangles cut the chords of a steep face.
			col.points.push_back(base + nrm * (depth + 0.05f + 0.15f * sin_a));
			col.speed.push_back(v);
			col.air.push_back(air);
			col.travel.push_back(travel);
			prev_v = v;
			prev_p = p;
		}
		if (c == main_col) {
			face.inception = li;
			face.air_at_foot = air;
			float peak = 0.0f;
			for (int r = 0; r < col.air.size(); r++) {
				peak = MAX(peak, col.air[r]);
			}
			face.air_peak = peak;
		}
		face.columns.push_back(col);
	}
	// Ledges: where the water strikes each one, at the speed it has there (the middle column's nearest row).
	const Array ledges = p_fall.get("ledges", Array());
	const Column &mid = face.columns[main_col];
	for (int i = 0; i < ledges.size(); i++) {
		const Dictionary l = ledges[i];
		const Vector3 at = l.get("position", Vector3());
		float best = 1e30f;
		float v = 0.0f;
		for (int r = 0; r < mid.points.size(); r++) {
			const float d = mid.points[r].distance_squared_to(at);
			if (d < best) {
				best = d;
				v = mid.speed[r];
			}
		}
		face.ledges.push_back(at);
		face.ledge_speed.push_back(v);
	}
	// The record's impact speed is a free jet's: water on the rock arrives at its own (friction-limited) speed.
	if (mid.speed.size() > 0) {
		face.impact_speed = mid.speed[mid.speed.size() - 1];
	}
	faces.push_back(face);
}

void PhysXWaterfalls3D::_build_mesh() {
	if (mesh_instance == nullptr) {
		mesh_instance = memnew(MeshInstance3D);
		mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(mesh_instance, false, INTERNAL_MODE_BACK);
	}
	if (material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(PhysXRiver3D::get_fall_shader_code());
		material.instantiate();
		material->set_shader(shader);
	}
	mesh_instance->set_material_override(material);
	const Transform3D inv = (is_inside_tree() ? get_global_transform() : get_transform()).affine_inverse();
	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedVector2Array uv2s;
	PackedColorArray colors;
	PackedInt32Array indices;
	for (const Face &face : faces) {
		const int cols = face.columns.size();
		if (cols < 2) {
			continue;
		}
		const int rows = face.columns[0].points.size();
		float q_max = 1e-6f;
		for (const Column &col : face.columns) {
			q_max = MAX(q_max, col.q);
		}
		const int base = verts.size();
		const float spacing = MAX(face.width, 1.0f) / (cols - 1);
		for (int c = 0; c < cols; c++) {
			const Column &col = face.columns[c];
			const float end = (c == 0 || c == cols - 1) ? 0.0f : 1.0f;
			const float edge = end * CLAMP(4.0f * col.q / q_max, 0.0f, 1.0f);
			for (int r = 0; r < rows; r++) {
				verts.push_back(inv.xform(col.points[r]));
				const Vector3 a = col.points[MIN(r + 1, rows - 1)] - col.points[MAX(r - 1, 0)];
				const Vector3 across = face.columns[MIN(c + 1, cols - 1)].points[r] - face.columns[MAX(c - 1, 0)].points[r];
				Vector3 n = a.cross(across);
				// Out of the rock: away from the face, towards the open air in front of it.
				if (n.dot(face.direction) + n.y * 0.25f < 0.0f) {
					n = -n;
				}
				normals.push_back(inv.basis.xform(n.normalized()).normalized());
				uvs.push_back(Vector2(col.travel[r], c * spacing));
				uv2s.push_back(Vector2(col.speed[r], 0.0f));
				// Aeration as a share of the most a sheet can hold (0.9): the streaks still show through white water.
				colors.push_back(Color(edge, CLAMP(col.air[r] / 0.9f, 0.0f, 1.0f), 0.0f, 1.0f));
			}
		}
		for (int c = 0; c + 1 < cols; c++) {
			for (int r = 0; r + 1 < rows; r++) {
				const int k00 = base + c * rows + r;
				const int k01 = k00 + rows; // next column
				const int k10 = k00 + 1; // next row
				const int k11 = k01 + 1;
				// Front faces wind clockwise as seen from outside: the geometric normal (right-handed) points into
				// the rock.
				const Vector3 g = (verts[k10] - verts[k00]).cross(verts[k01] - verts[k00]);
				const bool flip = g.dot(normals[k00 - 0]) > 0.0f;
				if (!flip) {
					indices.push_back(k00);
					indices.push_back(k10);
					indices.push_back(k01);
					indices.push_back(k01);
					indices.push_back(k10);
					indices.push_back(k11);
				} else {
					indices.push_back(k00);
					indices.push_back(k01);
					indices.push_back(k10);
					indices.push_back(k01);
					indices.push_back(k11);
					indices.push_back(k10);
				}
			}
		}
	}
	if (indices.is_empty()) {
		mesh_instance->set_mesh(Ref<Mesh>());
		return;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
	arrays[Mesh::ARRAY_COLOR] = colors;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh_instance->set_mesh(mesh);
}

void PhysXWaterfalls3D::_clear_effects() {
	for (Node3D *n : effects) {
		if (n != nullptr) {
			remove_child(n);
			memdelete(n);
		}
	}
	effects.clear();
}

void PhysXWaterfalls3D::_build_effects() {
	_clear_effects();
	if (Engine::get_singleton()->is_editor_hint() || (!spray && !mist)) {
		return;
	}
	const Transform3D inv = (is_inside_tree() ? get_global_transform() : get_transform()).affine_inverse();
	for (const Face &face : faces) {
		const Vector3 across(-face.direction.z, 0.0f, face.direction.x);
		// Local frame of a fall: x across, y up, -z downstream.
		Basis b(across, Vector3(0, 1, 0), -face.direction);
		b.orthonormalize();
		const Basis lb = inv.basis * b;
		if (spray) {
			// At each ledge the sheet strikes the step: a small plunge with the kinetic power arriving there.
			for (int i = 0; i < face.ledges.size(); i++) {
				const float v = face.ledge_speed[i];
				const float power = 0.5f * 1000.0f * face.discharge * v * v; // W
				Transform3D xf(lb, inv.xform(face.ledges[i]));
				GPUParticles3D *s = make_spray(xf, v, face.width, power);
				add_child(s, false, INTERNAL_MODE_BACK);
				effects.push_back(s);
			}
			const float v = face.impact_speed;
			const float power = 0.5f * 1000.0f * face.discharge * v * v;
			GPUParticles3D *s = make_spray(Transform3D(lb, inv.xform(face.foot)), v, face.width, power);
			add_child(s, false, INTERNAL_MODE_BACK);
			effects.push_back(s);
		}
		// A trickle down the rock (spring melt, a side stream) wets it but throws no measurable mist.
		if (mist && face.discharge >= mist_min_discharge) {
			// Fine drops leave the aerated sheet all the way down: a line source along the face, densest where it
			// lands. The share of the water atomised grows with the square of the speed (as PhysXRiver3D's falls).
			const Vector3 run_v = face.foot - face.top;
			const float run = Vector2(run_v.x, run_v.z).length();
			const float reach = CLAMP(6.0f + 0.3f * face.height, 8.0f, 60.0f);
			const Vector3 size(face.width + 2.0f * reach, face.height + 0.5f * reach + 6.0f, run + 2.0f * reach);
			const Vector3 centre = (face.top + face.foot) * 0.5f + Vector3(0, 0.25f * reach, 0);
			PhysXWaterMist3D *m = memnew(PhysXWaterMist3D);
			m->set_name("FallMist");
			m->set_transform(Transform3D(lb, inv.xform(centre)));
			m->set_size(size);
			m->set_cell_size(MAX(0.5f, MAX(size.x, MAX(size.y, size.z)) / 64.0f));
			// The source: the face, from the lip to the foot (local: -z downstream, y up), a little out from it.
			const Vector3 face_mid_local = b.xform_inv((face.top + face.foot) * 0.5f - centre);
			m->set_source_position(face_mid_local + Vector3(0, 0, -0.5f));
			m->set_source_extents(Vector3(0.5f * face.width + 0.5f, 0.5f * face.height + 0.5f, 0.5f * run + 1.0f));
			m->set_jet_direction(Vector3(0.0f, 0.0f, -1.0f));
			m->set_jet_speed(MIN(0.3f * face.impact_speed, 12.0f));
			const float v = MAX(face.impact_speed, 1.0f) / 10.0f;
			m->set_source_rate(mist_amount * 1e-5f * v * v * 1000.0f * face.discharge * 1000.0f); // g/s
			add_child(m, false, INTERNAL_MODE_BACK);
			effects.push_back(m);
		}
	}
}

void PhysXWaterfalls3D::rebuild() {
	faces.clear();
	Node *src = has_node(source) ? get_node(source) : nullptr;
	if (contact_falls && src != nullptr && src->has_method("get_waterfalls")) {
		const Array falls = src->call("get_waterfalls");
		for (int i = 0; i < falls.size(); i++) {
			const Dictionary f = falls[i];
			const String type = f.get("type", String());
			// Free falls (plunge) are the falling sheet's: PhysXRiver3D's curtain and, for tall ones, the broken
			// column (step 6b).
			if (type.is_empty() || type == "plunge" || float(f.get("height", 0.0f)) < min_height) {
				continue;
			}
			_build_face(f, src);
		}
	}
	_build_mesh();
	_build_effects();
	_exclude_from_rivers();
}

// The rivers draw their own falls from their beds: hand them the lips drawn here, so a contact fall is not also a
// free jet leaving the face.
void PhysXWaterfalls3D::_exclude_from_rivers() {
	if (!is_inside_tree()) {
		return;
	}
	PackedVector3Array lips;
	for (const Face &face : faces) {
		lips.push_back(face.top);
		lips.push_back(face.foot);
	}
	LocalVector<Node *> stack;
	stack.push_back(get_tree()->get_root());
	while (!stack.is_empty()) {
		Node *n = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		if (PhysXRiver3D *river = Object::cast_to<PhysXRiver3D>(n)) {
			if (river->get_fall_exclusions() != lips) {
				river->set_fall_exclusions(lips);
			}
		}
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
	}
}

Array PhysXWaterfalls3D::get_contact_falls() const {
	Array out;
	for (const Face &face : faces) {
		Dictionary d;
		d["id"] = face.id;
		d["type"] = face.type;
		d["height"] = face.height;
		d["discharge"] = face.discharge;
		d["inception_length"] = face.inception;
		d["air_at_foot"] = face.air_at_foot;
		d["air_peak"] = face.air_peak;
		d["impact_speed"] = face.impact_speed;
		d["ledges"] = face.ledges.size();
		d["rows"] = face.columns.is_empty() ? 0 : face.columns[0].points.size();
		out.push_back(d);
	}
	return out;
}

void PhysXWaterfalls3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			water_time += get_process_delta_time();
			if (material.is_valid() && !faces.is_empty()) {
				material->set_shader_parameter(SNAME("water_time"), (float)Math::fmod(water_time, 3600.0));
			}
		} break;
	}
}

void PhysXWaterfalls3D::set_source(const NodePath &p_path) {
	source = p_path;
}

void PhysXWaterfalls3D::set_contact_falls(bool p_enabled) {
	contact_falls = p_enabled;
}

void PhysXWaterfalls3D::set_spray(bool p_enabled) {
	spray = p_enabled;
}

void PhysXWaterfalls3D::set_mist(bool p_enabled) {
	mist = p_enabled;
}

void PhysXWaterfalls3D::set_mist_amount(float p_amount) {
	mist_amount = MAX(p_amount, 0.0f);
}

void PhysXWaterfalls3D::set_mist_min_discharge(float p_q) {
	mist_min_discharge = MAX(p_q, 0.0f);
	if (is_inside_tree()) {
		_build_effects();
	}
}

void PhysXWaterfalls3D::set_min_height(float p_height) {
	min_height = MAX(p_height, 0.0f);
}

void PhysXWaterfalls3D::set_rock_roughness(float p_ks) {
	rock_roughness = MAX(p_ks, 0.001f);
}

void PhysXWaterfalls3D::set_friction_factor(float p_f) {
	friction_factor = MAX(p_f, 0.005f);
}

void PhysXWaterfalls3D::set_row_spacing(float p_spacing) {
	row_spacing = MAX(p_spacing, 0.25f);
}

void PhysXWaterfalls3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("rebuild"), &PhysXWaterfalls3D::rebuild);
	ClassDB::bind_method(D_METHOD("get_contact_falls"), &PhysXWaterfalls3D::get_contact_falls);
	ClassDB::bind_method(D_METHOD("set_source", "path"), &PhysXWaterfalls3D::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &PhysXWaterfalls3D::get_source);
	ClassDB::bind_method(D_METHOD("set_contact_falls", "enabled"), &PhysXWaterfalls3D::set_contact_falls);
	ClassDB::bind_method(D_METHOD("get_contact_falls_enabled"), &PhysXWaterfalls3D::get_contact_falls_enabled);
	ClassDB::bind_method(D_METHOD("set_spray", "enabled"), &PhysXWaterfalls3D::set_spray);
	ClassDB::bind_method(D_METHOD("get_spray"), &PhysXWaterfalls3D::get_spray);
	ClassDB::bind_method(D_METHOD("set_mist", "enabled"), &PhysXWaterfalls3D::set_mist);
	ClassDB::bind_method(D_METHOD("get_mist"), &PhysXWaterfalls3D::get_mist);
	ClassDB::bind_method(D_METHOD("set_mist_amount", "amount"), &PhysXWaterfalls3D::set_mist_amount);
	ClassDB::bind_method(D_METHOD("get_mist_amount"), &PhysXWaterfalls3D::get_mist_amount);
	ClassDB::bind_method(D_METHOD("set_mist_min_discharge", "discharge"), &PhysXWaterfalls3D::set_mist_min_discharge);
	ClassDB::bind_method(D_METHOD("get_mist_min_discharge"), &PhysXWaterfalls3D::get_mist_min_discharge);
	ClassDB::bind_method(D_METHOD("set_min_height", "height"), &PhysXWaterfalls3D::set_min_height);
	ClassDB::bind_method(D_METHOD("get_min_height"), &PhysXWaterfalls3D::get_min_height);
	ClassDB::bind_method(D_METHOD("set_rock_roughness", "ks"), &PhysXWaterfalls3D::set_rock_roughness);
	ClassDB::bind_method(D_METHOD("get_rock_roughness"), &PhysXWaterfalls3D::get_rock_roughness);
	ClassDB::bind_method(D_METHOD("set_friction_factor", "f"), &PhysXWaterfalls3D::set_friction_factor);
	ClassDB::bind_method(D_METHOD("get_friction_factor"), &PhysXWaterfalls3D::get_friction_factor);
	ClassDB::bind_method(D_METHOD("set_row_spacing", "spacing"), &PhysXWaterfalls3D::set_row_spacing);
	ClassDB::bind_method(D_METHOD("get_row_spacing"), &PhysXWaterfalls3D::get_row_spacing);
	ClassDB::bind_static_method("PhysXWaterfalls3D", D_METHOD("inception_length", "q", "sin_slope", "ks"), &PhysXWaterfalls3D::inception_length);
	ClassDB::bind_static_method("PhysXWaterfalls3D", D_METHOD("equilibrium_air", "sin_slope"), &PhysXWaterfalls3D::equilibrium_air);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "source"), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "contact_falls"), "set_contact_falls", "get_contact_falls_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "spray"), "set_spray", "get_spray");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mist"), "set_mist", "get_mist");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_amount", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_mist_amount", "get_mist_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mist_min_discharge", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m\u00b3/s"), "set_mist_min_discharge", "get_mist_min_discharge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_height", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_min_height", "get_min_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rock_roughness", PROPERTY_HINT_RANGE, "0.001,1,0.001,suffix:m"), "set_rock_roughness", "get_rock_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction_factor", PROPERTY_HINT_RANGE, "0.005,1,0.001"), "set_friction_factor", "get_friction_factor");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "row_spacing", PROPERTY_HINT_RANGE, "0.25,10,0.05,suffix:m"), "set_row_spacing", "get_row_spacing");
}

PhysXWaterfalls3D::PhysXWaterfalls3D() {}

PhysXWaterfalls3D::~PhysXWaterfalls3D() {}
