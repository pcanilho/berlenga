/**************************************************************************/
/*  sod_lip_fall_3d.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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


#include "sod_lip_fall_3d.h"

#include "plant_generator.h"
#include "plant_species.h"
#include "vegetation_field_3d.h"

#include "core/config/engine.h"
#include "core/math/geometry_2d.h"
#include "core/math/random_pcg.h"
#include "core/object/worker_thread_pool.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/mesh.h"

namespace {
uint64_t lip_key(const Vector3 &p) {
	return hash_murmur3_one_64(uint64_t(int64_t(Math::round(p.x * 20.0f))), hash_murmur3_one_64(uint64_t(int64_t(Math::round(p.z * 20.0f)))));
}
// A deterministic draw per lip and check, so a run repeats.
float lip_draw(uint64_t p_key, int p_tick) {
	return float(hash_murmur3_one_64(uint64_t(p_tick), p_key) & 0xFFFFFF) / float(0x1000000);
}
} // namespace

// The lip cut into pieces for Blast (round 9): cracks across its length, where the root mat tears (a cantilever's
// tension cracks run along the bank, so the fallen slab breaks across it), each a little askew. The lip's own
// surfaces split along them: the soil (centre) surface clipped exactly, blades and roots by where each triangle lies.
// Each piece's shell is its cross-section's convex hull, 4 mm inside the lip's surfaces, run from crack to crack:
// it gives the piece its collision and draws the torn soil on the breaks.
namespace {
struct SodV {
	Vector3 p, n;
	float t[4] = { 1, 0, 0, 1 };
	Color c = Color(1, 1, 1);
	Vector2 uv, uv2;
};

SodV sod_mix(const SodV &p_a, const SodV &p_b, float p_t) {
	SodV v;
	v.p = p_a.p.lerp(p_b.p, p_t);
	v.n = p_a.n.lerp(p_b.n, p_t).normalized();
	for (int k = 0; k < 4; k++) {
		v.t[k] = Math::lerp(p_a.t[k], p_b.t[k], p_t);
	}
	v.c = p_a.c.lerp(p_b.c, p_t);
	v.uv = p_a.uv.lerp(p_b.uv, p_t);
	v.uv2 = p_a.uv2.lerp(p_b.uv2, p_t);
	return v;
}

struct SodSoup {
	LocalVector<SodV> v;
	bool tangents = false, colors = false, uvs = false, uv2s = false;
	Array arrays() const {
		PackedVector3Array vo, no;
		PackedFloat32Array to;
		PackedColorArray co;
		PackedVector2Array uo, u2o;
		for (const SodV &x : v) {
			vo.push_back(x.p);
			no.push_back(x.n);
			for (int k = 0; k < 4; k++) {
				to.push_back(x.t[k]);
			}
			co.push_back(x.c);
			uo.push_back(x.uv);
			u2o.push_back(x.uv2);
		}
		Array a;
		a.resize(Mesh::ARRAY_MAX);
		a[Mesh::ARRAY_VERTEX] = vo;
		a[Mesh::ARRAY_NORMAL] = no;
		if (tangents) {
			a[Mesh::ARRAY_TANGENT] = to;
		}
		if (colors) {
			a[Mesh::ARRAY_COLOR] = co;
		}
		if (uvs) {
			a[Mesh::ARRAY_TEX_UV] = uo;
		}
		if (uv2s) {
			a[Mesh::ARRAY_TEX_UV2] = u2o;
		}
		return a;
	}
};

// A triangle wound to face p_out (the destructible's convention: (c - a) x (b - a) outward).
void sod_tri(SodSoup &r_s, const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, const Vector3 &p_out, const Color &p_col) {
	Vector3 a = p_a, b = p_b, c = p_c;
	Vector3 n = (c - a).cross(b - a);
	if (n.dot(p_out) < 0.0f) {
		SWAP(b, c);
		n = -n;
	}
	if (n.length_squared() < 1e-14f) {
		return;
	}
	n.normalize();
	const Vector3 vs[3] = { a, b, c };
	for (int k = 0; k < 3; k++) {
		SodV v;
		v.p = vs[k];
		v.n = n;
		v.c = p_col;
		// Soil UVs: planar on the face's dominant axis.
		v.uv = Math::abs(n.z) > 0.7f ? Vector2(vs[k].x, vs[k].y) * 2.0f : Vector2(vs[k].z, vs[k].y) * 2.0f;
		r_s.v.push_back(v);
	}
}
} // namespace

void SodLipFall3D::_piece_job(PieceJob *p_job) {
	const Array &surfaces = p_job->surfaces;
	const int cs = p_job->centre_surface;
	if (cs < 0 || cs >= surfaces.size()) {
		return;
	}
	auto read = [](const Array &p_a, LocalVector<SodV> &r_tris, SodSoup &r_fmt) {
		const PackedVector3Array vin = p_a[Mesh::ARRAY_VERTEX];
		const PackedVector3Array nin = p_a[Mesh::ARRAY_NORMAL];
		const PackedFloat32Array tin = p_a[Mesh::ARRAY_TANGENT];
		const PackedColorArray cin = p_a[Mesh::ARRAY_COLOR];
		const PackedVector2Array uin = p_a[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array u2in = p_a[Mesh::ARRAY_TEX_UV2];
		r_fmt.tangents = !tin.is_empty();
		r_fmt.colors = !cin.is_empty();
		r_fmt.uvs = !uin.is_empty();
		r_fmt.uv2s = !u2in.is_empty();
		PackedInt32Array idx = p_a[Mesh::ARRAY_INDEX];
		if (idx.is_empty()) {
			idx.resize(vin.size());
			for (int i = 0; i < vin.size(); i++) {
				idx.set(i, i);
			}
		}
		for (int i = 0; i + 2 < idx.size(); i += 3) {
			for (int k = 0; k < 3; k++) {
				const int j = idx[i + k];
				SodV v;
				v.p = vin[j];
				v.n = j < nin.size() ? nin[j] : Vector3(0, 1, 0);
				if (tin.size() >= (j + 1) * 4) {
					for (int q = 0; q < 4; q++) {
						v.t[q] = tin[j * 4 + q];
					}
				}
				v.c = j < cin.size() ? cin[j] : Color(1, 1, 1);
				v.uv = j < uin.size() ? uin[j] : Vector2();
				v.uv2 = j < u2in.size() ? u2in[j] : Vector2();
				r_tris.push_back(v);
			}
		}
	};
	// The lip's length (mesh Z) from its soil surface.
	LocalVector<SodV> soil;
	SodSoup soil_fmt;
	read(surfaces[cs], soil, soil_fmt);
	if (soil.size() < 3) {
		return;
	}
	float zmin = 1e9f, zmax = -1e9f;
	for (const SodV &v : soil) {
		zmin = MIN(zmin, v.p.z);
		zmax = MAX(zmax, v.p.z);
	}
	const float len = zmax - zmin;
	RandomPCG rng(p_job->seed);
	const int n = CLAMP(int(Math::round(len / 0.3f)), 2, 5);
	// Crack i: z = c[i] + k[i] x, between pieces i and i + 1. Tilts up to 8 degrees keep neighbours apart over the
	// lip's depth (estimate; the cracks' run is not sourced).
	LocalVector<float> cc, kk;
	for (int i = 1; i < n; i++) {
		cc.push_back(zmin + len * (float(i) + (rng.randf() - 0.5f) * 0.5f) / float(n));
		kk.push_back(Math::tan(Math::deg_to_rad((rng.randf() - 0.5f) * 16.0f)));
	}
	auto side = [&](int p_crack, const Vector3 &p) { return p.z - (cc[p_crack] + kk[p_crack] * p.x); };
	auto piece_of = [&](const Vector3 &p) {
		int k = 0;
		while (k < n - 1 && side(k, p) > 0.0f) {
			k++;
		}
		return k;
	};
	const int surf_count = surfaces.size();
	// Per piece, per surface: its triangles.
	LocalVector<LocalVector<SodSoup>> soups;
	soups.resize(n);
	for (int p = 0; p < n; p++) {
		soups[p].resize(surf_count);
	}
	for (int s = 0; s < surf_count; s++) {
		LocalVector<SodV> tris;
		SodSoup fmt;
		if (s == cs) {
			tris = soil;
			fmt = soil_fmt;
		} else {
			read(surfaces[s], tris, fmt);
		}
		for (int p = 0; p < n; p++) {
			soups[p][s].tangents = fmt.tangents;
			soups[p][s].colors = fmt.colors;
			soups[p][s].uvs = fmt.uvs;
			soups[p][s].uv2s = fmt.uv2s;
		}
		for (uint32_t t = 0; t + 2 < tris.size(); t += 3) {
			if (s != cs) {
				// Blades and roots go whole with the piece they grow from.
				const int p = piece_of((tris[t].p + tris[t + 1].p + tris[t + 2].p) / 3.0f);
				for (int k = 0; k < 3; k++) {
					soups[p][s].v.push_back(tris[t + k]);
				}
				continue;
			}
			// The soil: clipped to each piece's slab (Sutherland-Hodgman against its two cracks).
			for (int p = 0; p < n; p++) {
				LocalVector<SodV> poly;
				for (int k = 0; k < 3; k++) {
					poly.push_back(tris[t + k]);
				}
				for (int e = 0; e < 2 && poly.size() >= 3; e++) {
					const int crack = e == 0 ? p - 1 : p;
					if (crack < 0 || crack >= n - 1) {
						continue;
					}
					// Keep side(crack) >= 0 past the lower crack, <= 0 before the upper.
					const float sign = e == 0 ? 1.0f : -1.0f;
					LocalVector<SodV> out;
					for (uint32_t k = 0; k < poly.size(); k++) {
						const SodV &a = poly[k];
						const SodV &b = poly[(k + 1) % poly.size()];
						const float da = sign * side(crack, a.p);
						const float db = sign * side(crack, b.p);
						if (da >= 0.0f) {
							out.push_back(a);
						}
						if ((da >= 0.0f) != (db >= 0.0f)) {
							out.push_back(sod_mix(a, b, da / (da - db)));
						}
					}
					poly = out;
				}
				for (uint32_t k = 1; k + 1 < poly.size(); k++) {
					soups[p][s].v.push_back(poly[0]);
					soups[p][s].v.push_back(poly[k]);
					soups[p][s].v.push_back(poly[k + 1]);
				}
			}
		}
	}
	// Shells and bonds.
	const Color earth(0.85f, 0.82f, 0.78f);
	Array pieces, bonds;
	Array root;
	SodSoup root_shell;
	LocalVector<Vector<Point2>> hulls;
	hulls.resize(n);
	for (int p = 0; p < n; p++) {
		const SodSoup &so = soups[p][cs];
		Vector<Point2> pts;
		float pz0 = 1e9f, pz1 = -1e9f;
		for (const SodV &v : so.v) {
			pts.push_back(Point2(v.p.x, v.p.y));
			pz0 = MIN(pz0, v.p.z);
			pz1 = MAX(pz1, v.p.z);
		}
		Vector<Point2> hull = pts.size() >= 3 ? Geometry2D::convex_hull(pts) : Vector<Point2>();
		if (hull.size() > 1 && hull[0].is_equal_approx(hull[hull.size() - 1])) {
			hull.resize(hull.size() - 1);
		}
		if (hull.size() < 3) {
			return; // A degenerate piece: no asset, the block lands whole.
		}
		Point2 mid;
		for (const Point2 &h : hull) {
			mid += h;
		}
		mid /= float(hull.size());
		for (int i = 0; i < hull.size(); i++) {
			const Point2 d = hull[i] - mid;
			hull.write[i] = mid + d * MAX(1.0f - 0.004f / MAX(d.length(), 0.01f), 0.5f);
		}
		hulls[p] = hull;
		auto z_lo = [&](float x) { return p == 0 ? pz0 + 0.004f : cc[p - 1] + kk[p - 1] * x; };
		auto z_hi = [&](float x) { return p == n - 1 ? pz1 - 0.004f : cc[p] + kk[p] * x; };
		SodSoup shell;
		shell.colors = true;
		shell.uvs = true;
		const int hn = hull.size();
		const Vector3 centre3(mid.x, mid.y, 0.5f * (z_lo(mid.x) + z_hi(mid.x)));
		for (int i = 0; i < hn; i++) {
			const Point2 a = hull[i], b = hull[(i + 1) % hn];
			const Vector3 a0(a.x, a.y, z_lo(a.x)), a1(a.x, a.y, z_hi(a.x)), b0(b.x, b.y, z_lo(b.x)), b1(b.x, b.y, z_hi(b.x));
			const Point2 e = b - a;
			const Vector3 out = Vector3(e.y, -e.x, 0.0f).dot(Vector3(a.x - mid.x, a.y - mid.y, 0.0f)) >= 0.0f ? Vector3(e.y, -e.x, 0.0f) : Vector3(-e.y, e.x, 0.0f);
			sod_tri(shell, a0, b0, b1, out, earth);
			sod_tri(shell, a0, b1, a1, out, earth);
			if (i > 0 && i + 1 < hn) {
				const Point2 h0 = hull[0];
				const Vector3 lo_n = Vector3(p == 0 ? 0.0f : kk[p - 1], 0.0f, -1.0f);
				const Vector3 hi_n = Vector3(p == n - 1 ? 0.0f : -kk[p], 0.0f, 1.0f);
				sod_tri(shell, Vector3(h0.x, h0.y, z_lo(h0.x)), a0, b0, lo_n, earth);
				sod_tri(shell, Vector3(h0.x, h0.y, z_hi(h0.x)), a1, b1, hi_n, earth);
			}
		}
		(void)centre3;
		Array piece;
		Dictionary sh;
		sh["arrays"] = shell.arrays();
		sh["slot"] = cs;
		piece.push_back(sh);
		for (int s = 0; s < surf_count; s++) {
			if (soups[p][s].v.is_empty()) {
				continue;
			}
			Dictionary d;
			d["arrays"] = soups[p][s].arrays();
			d["slot"] = s;
			piece.push_back(d);
			root.push_back(d);
		}
		pieces.push_back(piece);
		for (const SodV &v : shell.v) {
			root_shell.v.push_back(v);
		}
	}
	root_shell.colors = true;
	root_shell.uvs = true;
	float volume = 0.0f;
	for (uint32_t t = 0; t + 2 < root_shell.v.size(); t += 3) {
		volume += root_shell.v[t].p.dot(root_shell.v[t + 1].p.cross(root_shell.v[t + 2].p)) / 6.0f;
	}
	p_job->volume = Math::abs(volume);
	Dictionary rs;
	rs["arrays"] = root_shell.arrays();
	rs["slot"] = cs;
	root.insert(0, rs);
	for (int p = 0; p + 1 < n; p++) {
		// At the crack: the piece's cross-section there.
		const Vector<Point2> &h = hulls[p];
		float area = 0.0f;
		Point2 mid;
		for (int i = 0; i < h.size(); i++) {
			const Point2 a = h[i], b = h[(i + 1) % h.size()];
			area += a.x * b.y - b.x * a.y;
			mid += a;
		}
		mid /= float(h.size());
		Dictionary bd;
		bd["chunks"] = Vector2i(p, p + 1);
		bd["centroid"] = Vector3(mid.x, mid.y, cc[p] + kk[p] * mid.x);
		bd["normal"] = Vector3(-kk[p], 0.0f, 1.0f).normalized();
		bd["area"] = MAX(Math::abs(area) * 0.5f, 1e-4f);
		bonds.push_back(bd);
	}
	Object *authoring = ClassDB::instantiate("PhysXBlastAuthoring");
	if (!authoring) {
		return;
	}
	Ref<RefCounted> holder(Object::cast_to<RefCounted>(authoring));
	const Variant res = authoring->call("build_prefractured", root, pieces, bonds, p_job->materials);
	p_job->asset = res;
	if (p_job->asset.is_valid()) {
		p_job->asset->set_meta("sod_volume", p_job->volume); // m3 unscaled: the soil, for the pieces' mass.
	}
	if (p_job->asset.is_null()) {
		WARN_PRINT(vformat("SodLipFall3D: Blast pieces failed: %s", String(authoring->call("get_last_error"))));
	} else if (p_job->asset->has_method("prepare_render")) {
		p_job->asset->call("prepare_render");
	}
}

void SodLipFall3D::_request_pieces(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null() || !ClassDB::class_exists("PhysXBlastAuthoring")) {
		return;
	}
	const ObjectID id = p_mesh->get_instance_id();
	if (piece_assets.has(id) || piece_jobs.has(id)) {
		return;
	}
	PieceJob *job = memnew(PieceJob);
	const Ref<ArrayMesh> am = p_mesh;
	job->centre_surface = -1;
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		job->surfaces.push_back(p_mesh->surface_get_arrays(s));
		job->materials.push_back(p_mesh->surface_get_material(s));
		if (am.is_valid() && am->surface_get_name(s) == "centre") {
			job->centre_surface = s;
		}
	}
	job->seed = uint64_t(id) * 2654435761ull + 17;
	job->task = WorkerThreadPool::get_singleton()->add_template_task(this, &SodLipFall3D::_piece_job_member, job, false, String("SodLipFall3D pieces"));
	piece_jobs[id] = job;
}

Ref<Resource> SodLipFall3D::_pieces_for(const Ref<Mesh> &p_mesh) {
	if (p_mesh.is_null()) {
		return Ref<Resource>();
	}
	const ObjectID id = p_mesh->get_instance_id();
	if (const Ref<Resource> *a = piece_assets.getptr(id)) {
		return *a;
	}
	PieceJob **job = piece_jobs.getptr(id);
	if (!job) {
		return Ref<Resource>();
	}
	// Landed before its pieces were ready (a short drop): wait for them, a few ms.
	WorkerThreadPool::get_singleton()->wait_for_task_completion((*job)->task);
	const Ref<Resource> asset = (*job)->asset;
	piece_assets[id] = asset;
	memdelete(*job);
	piece_jobs.erase(id);
	return asset;
}

SodLipFall3D::~SodLipFall3D() {
	for (KeyValue<ObjectID, PieceJob *> &kv : piece_jobs) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
		memdelete(kv.value);
	}
	piece_jobs.clear();
}

// A hard landing breaks the block: it becomes a dynamic Blast destructible of its pieces in the same pose, going on
// as the block was, and the hit breaks the cracks near where it struck, the harder the farther.
void SodLipFall3D::_land(Fallen &r_f, float p_speed_change) {
	r_f.landed = true;
	if (p_speed_change < break_speed || !ClassDB::class_exists("PhysXDestructible3D")) {
		return;
	}
	const Ref<Resource> asset = _pieces_for(r_f.mesh);
	if (asset.is_null()) {
		return;
	}
	Object *obj = ClassDB::instantiate("PhysXDestructible3D");
	Node3D *node = Object::cast_to<Node3D>(obj);
	if (!node) {
		if (obj) {
			memdelete(obj);
		}
		return;
	}
	RigidBody3D *body = r_f.body;
	const Transform3D g = body->get_global_transform();
	node->set_name(vformat("SodPieces%d", breaks));
	node->set("blast_asset", asset);
	node->set("health", 1.0f);
	node->set("auto_mass", false);
	const float soil = float(asset->get_meta("sod_volume", 0.0f)) * r_f.scale.x * r_f.scale.y * r_f.scale.z;
	node->set("mass", soil > 0.0f ? density * soil : body->get_mass());
	node->set("dynamic", true);
	node->set("anchor", 2); // ANCHOR_NONE
	node->set("impact_strength", 1.0e12); // Only the landing below breaks it.
	node->set("shatter_speed", 0.3f);
	node->set("freeze_when_asleep", false); // A river may move them yet.
	node->set("initial_linear_velocity", body->get_linear_velocity());
	node->set("initial_angular_velocity", body->get_angular_velocity());
	node->set_as_top_level(true);
	// The body's origin sits at the mesh's centre point (its children carry the instance's scale).
	node->set_transform(g * Transform3D(Basis().scaled(r_f.scale), -r_f.centre * r_f.scale));
	node->set_meta("sod_cause", body->get_meta("sod_cause", String()));
	add_child(node);
	// Struck along its underside: the cracks within a span of the slab's middle break, more of them the harder (radii
	// in metres; damage over the bond health, which a landing at break_speed just reaches).
	const float hit = p_speed_change / break_speed;
	const Vector3 low = g.origin - g.basis.get_column(1).normalized() * (0.5f * r_f.height);
	node->call("apply_radial_damage", low, 2.0f * hit, 0.3f, 0.3f + 0.4f * MIN(hit, 2.0f));
	r_f.pieces = node->get_instance_id();
	r_f.body = nullptr;
	body->queue_free();
	breaks++;
}

// Blocks and pieces in water: buoyancy and the flow's drag (0.5 rho Cd A |u - v| (u - v), A the side of a cube of
// the same volume), as impulses each step; the bed's friction (the engine's contacts) holds the heavy ones.
void SodLipFall3D::_carry(double p_delta) {
	const WorldWater *water = WorldContracts::water();
	if (!water) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const float rho_w = 1000.0f;
	const float g = 9.81f;
	auto push = [&](RID p_body, const Vector3 &p_centre, const Vector3 &p_velocity, float p_volume, float p_height) {
		if (!p_body.is_valid() || p_volume <= 0.0f) {
			return;
		}
		const float wy = water->height_at(p_centre);
		if (!Math::is_finite(wy)) {
			return;
		}
		const float sub = CLAMP((wy - (p_centre.y - 0.5f * p_height)) / MAX(p_height, 0.01f), 0.0f, 1.0f);
		if (sub <= 0.0f) {
			return;
		}
		const Vector3 rel = water->flow_at(p_centre) - p_velocity;
		const float area = Math::pow(p_volume, 2.0f / 3.0f);
		const Vector3 force = Vector3(0.0f, rho_w * g * p_volume * sub, 0.0f) + rel * (0.5f * rho_w * drag_coefficient * area * sub * rel.length());
		ps->body_apply_central_impulse(p_body, force * float(p_delta));
		if (rel.length_squared() > 0.0025f) {
			ps->body_set_state(p_body, PhysicsServer3D::BODY_STATE_SLEEPING, false);
		}
	};
	for (Fallen &f : fallen) {
		if (f.body) {
			push(f.body->get_rid(), f.body->get_global_position(), f.body->get_linear_velocity(), f.volume, f.height);
			continue;
		}
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(f.pieces));
		if (!node) {
			continue;
		}
		const int count = node->call("get_piece_count");
		for (int i = 0; i < count; i++) {
			const RID rid = node->call("get_piece_body", i);
			const float vol = node->call("get_piece_volume", i);
			push(rid, node->call("get_piece_center", i), node->call("get_piece_linear_velocity", i), vol, Math::pow(MAX(vol, 1e-6f), 1.0f / 3.0f));
		}
	}
}

void SodLipFall3D::_process_fallen(double p_delta) {
	for (uint32_t i = 0; i < fallen.size(); i++) {
		Fallen &f = fallen[i];
		f.age += p_delta;
		if (f.body && !f.landed) {
			if (!f.body->is_inside_tree()) {
				continue;
			}
			// A landing: the block stopped hard in a step (the first contact, not the tipping off the bank).
			const Vector3 v = f.body->get_linear_velocity();
			const float change = (f.last_velocity - v).length();
			if (f.age > 0.1 && f.last_velocity.length() > 0.5f && change > 1.0f) {
				_land(f, f.last_velocity.length());
			} else if (f.age > 10.0) {
				f.landed = true;
			}
			f.last_velocity = v;
		}
	}
	_carry(p_delta);
	// Carried off out of range, or gone: dropped.
	const Vector3 eye = _camera_position();
	for (int i = int(fallen.size()) - 1; i >= 0; i--) {
		Fallen &f = fallen[i];
		Node3D *pieces = Object::cast_to<Node3D>(ObjectDB::get_instance(f.pieces));
		if (!f.body && !pieces) {
			fallen.remove_at(i);
			continue;
		}
		if (pieces && int(pieces->call("get_piece_count")) > 0) {
			bool near = false;
			const int count = pieces->call("get_piece_count");
			for (int k = 0; k < count && !near; k++) {
				near = Vector3(pieces->call("get_piece_center", k)).distance_to(eye) < range * 1.5f;
			}
			if (!near) {
				pieces->queue_free();
				fallen.remove_at(i);
			}
		}
	}
}

Vector3 SodLipFall3D::_camera_position() const {
	if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		return get_viewport()->get_camera_3d()->get_global_position();
	}
	return is_inside_tree() ? get_global_position() : Vector3();
}

void SodLipFall3D::_break(Node *p_field, int p_species, const Transform3D &p_xf, const Ref<Mesh> &p_mesh, const String &p_cause) {
	VegetationField3D *vf = Object::cast_to<VegetationField3D>(p_field);
	if (!vf) {
		return;
	}
	gone.insert(lip_key(p_xf.origin), true);
	vf->add_plant_removal(p_xf.origin, 0.6f, p_species);
	falls++;
	if (p_mesh.is_null()) {
		return;
	}
	// The block: the sod slab (its soil's bounds, the mesh's top 20 cm across its width; roots and blades ride along).
	AABB bb = p_mesh->get_aabb();
	const ObjectID mid = p_mesh->get_instance_id();
	if (const AABB *sb = soil_boxes.getptr(mid)) {
		bb = *sb;
	} else {
		const Ref<ArrayMesh> am = p_mesh;
		for (int si = 0; am.is_valid() && si < am->get_surface_count(); si++) {
			if (am->surface_get_name(si) == "centre") {
				const Array a = am->surface_get_arrays(si);
				const PackedVector3Array v = a[Mesh::ARRAY_VERTEX];
				if (!v.is_empty()) {
					bb = AABB(v[0], Vector3());
					for (const Vector3 &q : v) {
						bb.expand_to(q);
					}
				}
			}
		}
		soil_boxes[mid] = bb;
	}
	const float top = bb.position.y + bb.size.y;
	const Vector3 scale = p_xf.basis.get_scale();
	const Vector3 half(MAX(bb.size.x * 0.5f, 0.05f), 0.1f, MAX(bb.size.z * 0.5f, 0.1f));
	const Vector3 centre(bb.position.x + half.x, MIN(top, 0.02f) - half.y, bb.position.z + half.z);
	RigidBody3D *body = memnew(RigidBody3D);
	body->set_name(vformat("SodBlock%d", falls));
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_mesh(p_mesh);
	mi->set_position(-centre);
	body->add_child(mi);
	CollisionShape3D *cs = memnew(CollisionShape3D);
	Ref<BoxShape3D> box;
	box.instantiate();
	box->set_size(half * 2.0f);
	cs->set_shape(box);
	body->add_child(cs);
	body->set_mass(MAX(density * 8.0f * half.x * half.y * half.z * scale.x * scale.y * scale.z, 1.0f));
	add_child(body);
	// The body carries the instance's scale on its children (a rigid body's own basis stays orthonormal).
	mi->set_scale(scale);
	mi->set_position(-centre * scale);
	cs->set_scale(scale);
	const Basis rot = p_xf.basis.orthonormalized();
	body->set_global_transform(Transform3D(rot, p_xf.xform(centre)));
	// A cantilever fails at its root and tips out over the drop: turning about the edge (mesh Z), its outer side down,
	// with a little push out.
	const Vector3 out = rot.get_column(0).normalized();
	const Vector3 along = rot.get_column(2).normalized();
	body->set_angular_velocity(along * 1.2f);
	body->set_linear_velocity(out * 0.4f);
	body->set_meta("sod_cause", p_cause);
	Fallen f;
	f.body = body;
	f.mesh = p_mesh;
	f.scale = scale;
	f.centre = centre;
	f.volume = 8.0f * half.x * half.y * half.z * scale.x * scale.y * scale.z;
	f.height = 2.0f * half.y * scale.y;
	f.last_velocity = body->get_linear_velocity();
	fallen.push_back(f);
	_request_pieces(p_mesh); // Ready by the time it lands.
	while (int(fallen.size()) > max_blocks) {
		// The oldest settle where they lie: frozen, still drawn.
		if (fallen[0].body) {
			fallen[0].body->set_freeze_enabled(true);
		} else if (Node *pieces = Object::cast_to<Node>(ObjectDB::get_instance(fallen[0].pieces))) {
			pieces->set("freeze_when_asleep", true);
		}
		fallen.remove_at(0);
	}
}

void SodLipFall3D::check(double p_delta) {
	if (!enabled || !is_inside_tree()) {
		return;
	}
	const Vector3 eye = _camera_position();
	const WorldWeather *wm = WorldContracts::weather();
	const WorldWater *water = WorldContracts::water();
	const int tick = int(Engine::get_singleton()->get_physics_frames());
	// Loads: the bodies of the load group.
	LocalVector<Vector3> loads;
	if (load_group != StringName()) {
		const Vector<Node *> nodes = get_tree()->get_nodes_in_group(load_group);
		for (Node *n : nodes) {
			const Node3D *n3 = Object::cast_to<Node3D>(n);
			if (n3 && n3->is_inside_tree() && n3->get_global_position().distance_to(eye) < range + 2.0f) {
				loads.push_back(n3->get_global_position());
			}
		}
	}
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
	for (int f = 0; f < found.size(); f++) {
		VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
		if (!vf || vf->get_biome().is_null()) {
			continue;
		}
		const TypedArray<PlantSpecies> species = vf->get_biome()->get_species();
		for (int s = 0; s < species.size(); s++) {
			const Ref<PlantSpecies> sp = species[s];
			if (sp.is_null() || int(sp->get_meta("plant_preset", -1)) != PlantGenerator::PRESET_TURF_LIP) {
				continue;
			}
			const Array lips = vf->get_instances_near(s, eye, range);
			for (int i = 0; i < lips.size(); i++) {
				const Dictionary d = lips[i];
				const Transform3D xf = d["transform"];
				const Ref<Mesh> mesh = d["mesh"];
				const uint64_t key = lip_key(xf.origin);
				if (gone.has(key)) {
					continue;
				}
				const Transform3D inv = xf.affine_inverse();
				const AABB bb = mesh.is_valid() ? mesh->get_aabb() : AABB(Vector3(-0.1, -0.2, -0.5), Vector3(0.4, 0.4, 1.0));
				// A body standing on the overhang (over the drop, within the lip's length, up to 2 m over it).
				String cause;
				for (const Vector3 &l : loads) {
					const Vector3 q = inv.xform(l);
					if (q.x > -0.02f && q.x < bb.position.x + bb.size.x + 0.05f && q.z > bb.position.z && q.z < bb.position.z + bb.size.z && q.y > -0.3f && q.y < 2.0f) {
						cause = "load";
						break;
					}
				}
				if (cause.is_empty()) {
					// The water at its foot (0.6 m out over the drop) cutting under it, or the sod soaked in heavy rain:
					// a hazard rate, a deterministic draw per lip and look.
					float hazard = 0.0f;
					const Vector3 out = xf.basis.get_column(0).normalized();
					const Vector3 foot = xf.origin + out * 0.6f;
					if (water) {
						const float wy = water->height_at(foot);
						if (Math::is_finite(wy) && wy > xf.origin.y - 0.7f && wy < xf.origin.y + 0.05f) {
							hazard += 1.0f / undercut_time;
						}
					}
					if (wm && wm->surface_wetness_at(xf.origin) > 0.8f && wm->rainfall_at(xf.origin) > 5.0f) {
						hazard += 1.0f / soaked_time;
					}
					if (hazard > 0.0f && lip_draw(key, tick) < 1.0f - Math::exp(-hazard * float(p_delta))) {
						cause = hazard >= 1.0f / undercut_time ? "undercut" : "soaked";
					}
				}
				if (!cause.is_empty()) {
					_break(vf, s, xf, mesh, cause);
				}
			}
		}
	}
}

bool SodLipFall3D::break_at(const Vector3 &p_position) {
	if (!is_inside_tree()) {
		return false;
	}
	const TypedArray<Node> found = get_tree()->get_root()->find_children("*", "VegetationField3D", true, false);
	float best = 1.0f;
	VegetationField3D *bf = nullptr;
	int bs = -1;
	Transform3D bx;
	Ref<Mesh> bm;
	for (int f = 0; f < found.size(); f++) {
		VegetationField3D *vf = Object::cast_to<VegetationField3D>(found[f].operator Object *());
		if (!vf || vf->get_biome().is_null()) {
			continue;
		}
		const TypedArray<PlantSpecies> species = vf->get_biome()->get_species();
		for (int s = 0; s < species.size(); s++) {
			const Ref<PlantSpecies> sp = species[s];
			if (sp.is_null() || int(sp->get_meta("plant_preset", -1)) != PlantGenerator::PRESET_TURF_LIP) {
				continue;
			}
			const Array lips = vf->get_instances_near(s, p_position, 1.0f);
			for (int i = 0; i < lips.size(); i++) {
				const Dictionary d = lips[i];
				const Transform3D xf = d["transform"];
				const float dd = Vector2(xf.origin.x - p_position.x, xf.origin.z - p_position.z).length();
				if (dd < best && !gone.has(lip_key(xf.origin))) {
					best = dd;
					bf = vf;
					bs = s;
					bx = xf;
					bm = d["mesh"];
				}
			}
		}
	}
	if (!bf) {
		return false;
	}
	_break(bf, bs, bx, bm, "asked");
	return true;
}

Array SodLipFall3D::get_blocks() const {
	Array out;
	for (const Fallen &f : fallen) {
		if (f.body) {
			out.push_back(f.body);
		} else if (Object *pieces = ObjectDB::get_instance(f.pieces)) {
			out.push_back(pieces);
		}
	}
	return out;
}

void SodLipFall3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			for (KeyValue<ObjectID, PieceJob *> &kv : piece_jobs) {
				WorkerThreadPool::get_singleton()->wait_for_task_completion(kv.value->task);
				if (kv.value->asset.is_valid()) {
					piece_assets[kv.key] = kv.value->asset;
				}
				memdelete(kv.value);
			}
			piece_jobs.clear();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_process_fallen(get_physics_process_delta_time());
			check_left -= get_physics_process_delta_time();
			if (check_left <= 0.0) {
				check_left = 0.5;
				check(0.5);
			}
		} break;
	}
}

void SodLipFall3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("check", "delta"), &SodLipFall3D::check);
	ClassDB::bind_method(D_METHOD("break_at", "position"), &SodLipFall3D::break_at);
	ClassDB::bind_method(D_METHOD("get_fall_count"), &SodLipFall3D::get_fall_count);
	ClassDB::bind_method(D_METHOD("get_blocks"), &SodLipFall3D::get_blocks);
	ClassDB::bind_method(D_METHOD("get_break_count"), &SodLipFall3D::get_break_count);
	ClassDB::bind_method(D_METHOD("set_break_speed", "speed"), &SodLipFall3D::set_break_speed);
	ClassDB::bind_method(D_METHOD("get_break_speed"), &SodLipFall3D::get_break_speed);
	ClassDB::bind_method(D_METHOD("set_drag_coefficient", "coefficient"), &SodLipFall3D::set_drag_coefficient);
	ClassDB::bind_method(D_METHOD("get_drag_coefficient"), &SodLipFall3D::get_drag_coefficient);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SodLipFall3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &SodLipFall3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &SodLipFall3D::set_range);
	ClassDB::bind_method(D_METHOD("get_range"), &SodLipFall3D::get_range);
	ClassDB::bind_method(D_METHOD("set_load_group", "group"), &SodLipFall3D::set_load_group);
	ClassDB::bind_method(D_METHOD("get_load_group"), &SodLipFall3D::get_load_group);
	ClassDB::bind_method(D_METHOD("set_undercut_time", "seconds"), &SodLipFall3D::set_undercut_time);
	ClassDB::bind_method(D_METHOD("get_undercut_time"), &SodLipFall3D::get_undercut_time);
	ClassDB::bind_method(D_METHOD("set_soaked_time", "seconds"), &SodLipFall3D::set_soaked_time);
	ClassDB::bind_method(D_METHOD("get_soaked_time"), &SodLipFall3D::get_soaked_time);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &SodLipFall3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &SodLipFall3D::get_density);
	ClassDB::bind_method(D_METHOD("set_max_blocks", "count"), &SodLipFall3D::set_max_blocks);
	ClassDB::bind_method(D_METHOD("get_max_blocks"), &SodLipFall3D::get_max_blocks);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "1,500,1,suffix:m"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "load_group"), "set_load_group", "get_load_group");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "undercut_time", PROPERTY_HINT_RANGE, "1,100000,1,suffix:s"), "set_undercut_time", "get_undercut_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soaked_time", PROPERTY_HINT_RANGE, "1,100000,1,suffix:s"), "set_soaked_time", "get_soaked_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "100,3000,10,suffix:kg/m³"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_blocks", PROPERTY_HINT_RANGE, "1,256,1"), "set_max_blocks", "get_max_blocks");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "break_speed", PROPERTY_HINT_RANGE, "0.1,20,0.1,suffix:m/s"), "set_break_speed", "get_break_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drag_coefficient", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_drag_coefficient", "get_drag_coefficient");
}
