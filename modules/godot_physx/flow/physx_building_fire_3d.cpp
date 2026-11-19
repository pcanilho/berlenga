/**************************************************************************/
/*  physx_building_fire_3d.cpp                                            */
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

#include "physx_building_fire_3d.h"

#ifdef GODOT_PHYSX_BLAST
#include "../blast/physx_destructible_3d.h"
#endif
#include "../nodes/physx_particle_fluid_3d.h"
#include "physx_burnable_3d.h"
#include "physx_fire_spread_3d.h"
#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"

LocalVector<PhysXBuildingFire3D *> PhysXBuildingFire3D::buildings;

static constexpr float SIGMA = 5.67e-8f;
static constexpr float WOOD_KRHOC = 1.2e5f; // k rho c of wood, (W/m2K)^2 s (as PhysXBurnable3D)
static constexpr float LINING_KRHOC = 8.4e4f; // k rho c of a timber lining (0.12 W/mK, 500 kg/m3, 1400 J/kgK)
static constexpr float FUEL_HEAT = 15.0f; // MJ/kg, effective heat of combustion of wood and contents
static constexpr float CHI_R = 0.3f; // radiant share of a flame's heat release
static constexpr float CHAR_RATE = 0.8e-3f / 60.0f; // m/s, notional charring of softwood (EN 1995-1-2, beta_n)
static constexpr float CLAD_SPREAD = 0.03f; // m/s, flame spread over a wooden wall (upward-dominated)
static constexpr float CLAD_HRR = 100.0f; // kW/m2 of burning cladding

PhysXBuildingFire3D::PhysXBuildingFire3D() {
	rng = uint32_t(uint64_t(this) >> 4) | 1u;
}

PhysXBuildingFire3D::~PhysXBuildingFire3D() {
	buildings.erase(this);
}

float PhysXBuildingFire3D::_rand() {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return float(rng & 0xffffffu) / float(0xffffff);
}

Node3D *PhysXBuildingFire3D::_target() const {
	return target.is_empty() ? const_cast<PhysXBuildingFire3D *>(this) : Object::cast_to<Node3D>(get_node_or_null(target));
}

void PhysXBuildingFire3D::_resolve() {
	if (!fire && !fire_path.is_empty()) {
		fire = Object::cast_to<PhysXFireSpread3D>(get_node_or_null(fire_path));
	}
	if (!flow) {
		if (!flow_path.is_empty()) {
			flow = Object::cast_to<PhysXFlow3D>(get_node_or_null(flow_path));
		} else if (fire) {
			flow = fire->get_flow();
		}
	}
#ifdef GODOT_PHYSX_BLAST
	if (destructible_id.is_null()) {
		if (PhysXDestructible3D *d = Object::cast_to<PhysXDestructible3D>(_target())) {
			destructible_id = d->get_instance_id();
		}
	}
#endif
}

Vector3 PhysXBuildingFire3D::_face_normal(int p_face) {
	static const Vector3 n[FACE_COUNT] = { Vector3(-1, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, -1), Vector3(0, 0, 1), Vector3(0, 1, 0) };
	return n[CLAMP(p_face, 0, FACE_COUNT - 1)];
}

// Face coordinates: on a wall, x runs along it from the room's lower corner and y up from the floor; on the top,
// (x, z) from the lower corner.
Vector3 PhysXBuildingFire3D::_face_point(const Room &p_room, int p_face, const Vector2 &p_uv) const {
	const Vector3 a = p_room.box.position;
	const Vector3 b = p_room.box.position + p_room.box.size;
	switch (p_face) {
		case FACE_NEG_X:
			return Vector3(a.x, a.y + p_uv.y, a.z + p_uv.x);
		case FACE_POS_X:
			return Vector3(b.x, a.y + p_uv.y, a.z + p_uv.x);
		case FACE_NEG_Z:
			return Vector3(a.x + p_uv.x, a.y + p_uv.y, a.z);
		case FACE_POS_Z:
			return Vector3(a.x + p_uv.x, a.y + p_uv.y, b.z);
		default:
			return Vector3(a.x + p_uv.x, b.y, a.z + p_uv.y);
	}
}

Vector2 PhysXBuildingFire3D::_face_uv(const Room &p_room, int p_face, const Vector3 &p_local) const {
	const Vector3 d = p_local - p_room.box.position;
	switch (p_face) {
		case FACE_NEG_X:
		case FACE_POS_X:
			return Vector2(d.z, d.y);
		case FACE_NEG_Z:
		case FACE_POS_Z:
			return Vector2(d.x, d.y);
		default:
			return Vector2(d.x, d.z);
	}
}

Vector2 PhysXBuildingFire3D::_face_size(const Room &p_room, int p_face) const {
	const Vector3 s = p_room.box.size;
	switch (p_face) {
		case FACE_NEG_X:
		case FACE_POS_X:
			return Vector2(s.z, s.y);
		case FACE_NEG_Z:
		case FACE_POS_Z:
			return Vector2(s.x, s.y);
		default:
			return Vector2(s.x, s.z);
	}
}

// The rooms from their dictionaries; an opening whose face lies on another room's face (within 30 cm) leads into
// that room, and a face covered by another room is not outside.
void PhysXBuildingFire3D::_build_rooms() {
	rooms_dirty = false;
	for (Room &r : rooms) {
		_disable(r.inner_emitter);
		_disable(r.roof_emitter);
		for (ObjectID id : r.clad_emitter) {
			_disable(id);
		}
		for (Opening &o : r.openings) {
			_disable(o.emitter);
		}
		if (Node *l = Object::cast_to<Node>(ObjectDB::get_instance(r.light))) {
			l->queue_free();
		}
	}
	rooms.clear();
	for (int i = 0; i < room_data.size(); i++) {
		const Dictionary d = room_data[i];
		Room r;
		r.box = d.get("box", AABB(Vector3(-2, 0, -2), Vector3(4, 2.5, 4)));
		r.box = r.box.abs();
		r.fuel_load = MAX(float(d.get("fuel_load", 780.0f)), 0.0f);
		const Array ops = d.get("openings", Array());
		for (int k = 0; k < ops.size(); k++) {
			const Dictionary od = ops[k];
			Opening o;
			o.face = CLAMP(int(od.get("face", 1)), 0, FACE_COUNT - 1);
			o.rect = od.get("rect", Rect2(0.5, 0.0, 0.9, 2.0));
			o.glazed = od.get("glazed", false);
			r.openings.push_back(o);
		}
		r.fuel_total = r.fuel_load * r.box.size.x * r.box.size.z;
		r.fuel_left = r.fuel_total;
		rooms.push_back(r);
	}
	for (uint32_t i = 0; i < rooms.size(); i++) {
		Room &r = rooms[i];
		for (int f = 0; f < FACE_COUNT; f++) {
			const Vector3 n = _face_normal(f);
			const Vector2 fs = _face_size(r, f);
			const Vector3 c = _face_point(r, f, fs * 0.5f) + n * 0.15f;
			for (uint32_t j = 0; j < rooms.size(); j++) {
				if (j != i && rooms[j].box.grow(0.15f).has_point(c)) {
					r.exterior[f] = false;
				}
			}
		}
		for (uint32_t k = 0; k < r.openings.size(); k++) {
			Opening &o = r.openings[k];
			o.break_at = glass_break_temperature + 80.0f * _rand(); // panes differ (Babrauskas 2004)
			const Vector3 n = _face_normal(o.face);
			const Vector3 c = _face_point(r, o.face, o.rect.get_center()) + n * 0.15f;
			for (uint32_t j = 0; j < rooms.size(); j++) {
				if (j != i && rooms[j].box.grow(0.3f).has_point(c)) {
					o.to_room = j;
					o.glazed = false;
				}
			}
		}
	}
	// A door between two rooms listed on one side only: the other side gets it too.
	for (uint32_t i = 0; i < rooms.size(); i++) {
		const uint32_t count = rooms[i].openings.size();
		for (uint32_t k = 0; k < count; k++) {
			const Opening o = rooms[i].openings[k];
			if (o.to_room < 0 || o.face == FACE_TOP) {
				continue;
			}
			Room &n = rooms[o.to_room];
			bool has = false;
			for (const Opening &no : n.openings) {
				has = has || no.to_room == int(i);
			}
			if (has) {
				continue;
			}
			static const int opposite[FACE_COUNT] = { FACE_POS_X, FACE_NEG_X, FACE_POS_Z, FACE_NEG_Z, FACE_TOP };
			const Room &r = rooms[i];
			const Vector3 a = _face_point(r, o.face, o.rect.position);
			Opening m = o;
			m.face = opposite[o.face];
			m.to_room = i;
			m.emitter = ObjectID();
			const Vector2 uv = _face_uv(n, m.face, a);
			m.rect.position = uv;
			n.openings.push_back(m);
		}
	}
	look_done = false;
}

float PhysXBuildingFire3D::_vent(const Room &p_room) const {
	float v = 0.01f * Math::sqrt(2.0f); // leaks round doors and windows
	for (const Opening &o : p_room.openings) {
		if (!o.glazed || o.broken) {
			const float h = o.face == FACE_TOP ? MIN(p_room.box.size.y, 2.0f) : o.rect.size.y;
			v += o.rect.get_area() * Math::sqrt(MAX(h, 0.05f));
		}
	}
	if (p_room.roof_open) {
		v += p_room.box.size.x * p_room.box.size.z * Math::sqrt(p_room.box.size.y);
	}
	if (p_room.floor_open) {
		v += 0.5f * p_room.box.size.x * p_room.box.size.z * Math::sqrt(p_room.box.size.y);
	}
	return v;
}

float PhysXBuildingFire3D::_lining_area(const Room &p_room) const {
	const Vector3 s = p_room.box.size;
	float a = 2.0f * (s.x * s.y + s.y * s.z + s.x * s.z);
	for (const Opening &o : p_room.openings) {
		a -= o.rect.get_area();
	}
	return MAX(a, 1.0f);
}

// Eurocode's external flame (EN 1991-1-2 Annex B, no forced draught): it reaches
// L = H (2.37 (Q / (A rho_g sqrt(g H)))^(2/3) - 1) above the opening's top, with the room's heat release Q (MW),
// the openings' area A and height H, and the gas at about 0.47 kg/m3.
float PhysXBuildingFire3D::_external_flame(const Room &p_room, const Opening &p_opening) const {
	if (p_opening.face == FACE_TOP || p_opening.to_room >= 0 || (p_opening.glazed && !p_opening.broken)) {
		return 0.0f;
	}
	if (p_room.state != ROOM_FLASHED && p_room.state != ROOM_DECAY) {
		return 0.0f;
	}
	float area = 0.0f;
	for (const Opening &o : p_room.openings) {
		if (o.face != FACE_TOP && o.to_room < 0 && (!o.glazed || o.broken)) {
			area += o.rect.get_area();
		}
	}
	const float h = MAX(p_opening.rect.size.y, 0.3f);
	const float q = (p_room.q + p_room.q_ext) / 1000.0f;
	const float x = q / (MAX(area, 0.1f) * 0.47f * Math::sqrt(9.81f * h));
	return MAX(h * (2.37f * Math::pow(x, 2.0f / 3.0f) - 1.0f), 0.0f);
}

void PhysXBuildingFire3D::_ignite_room(int p_room, const Vector3 &p_local) {
	Room &r = rooms[p_room];
	if (r.state != ROOM_COLD || r.fuel_left <= 0.0f) {
		return;
	}
	r.state = ROOM_GROWING;
	r.t = 0.0f;
	const Vector3 lo = r.box.position + Vector3(0.3f, 0.0f, 0.3f);
	const Vector3 hi = r.box.position + r.box.size - Vector3(0.3f, 0.0f, 0.3f);
	r.origin = Vector3(CLAMP(p_local.x, lo.x, MAX(hi.x, lo.x)), r.box.position.y, CLAMP(p_local.z, lo.z, MAX(hi.z, lo.z)));
}

void PhysXBuildingFire3D::ignite(const Vector3 &p_position) {
	if (rooms_dirty) {
		_build_rooms();
	}
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	int best = -1;
	float bd = 1e30f;
	for (uint32_t i = 0; i < rooms.size(); i++) {
		const Vector3 c = rooms[i].box.get_center();
		const float d = rooms[i].box.has_point(l) ? -1.0f : c.distance_to(l);
		if (d < bd) {
			bd = d;
			best = i;
		}
	}
	if (best >= 0) {
		_ignite_room(best, l);
	}
}

void PhysXBuildingFire3D::extinguish() {
	for (Room &r : rooms) {
		if (r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY) {
			r.state = ROOM_OUT;
		}
		r.q = 0.0f;
		r.q_ext = 0.0f;
		r.ember = 0.0f;
		for (int f = 0; f < FACE_COUNT; f++) {
			r.clad_r[f] = 0.0f;
		}
	}
}

// Heat on a room's outside face (kW/m2): a ground fire's flames at its foot (up to about 60 kW/m2 from a grass
// fire), Flow's hot gas against it, radiation from burning burnables and buildings (its own other rooms too: flames
// from a window lap the wall above), and glowing firebrand piles (as PhysXBurnable3D).
float PhysXBuildingFire3D::_exposure(int p_room, int p_face, Vector3 &r_hot) const {
	const Room &r = rooms[p_room];
	const Transform3D xf = get_global_transform();
	const Vector3 n = _face_normal(p_face);
	const Vector2 fs = _face_size(r, p_face);
	const Vector3 c = xf.xform(_face_point(r, p_face, fs * 0.5f) + n * 0.2f);
	float q = 0.0f;
	r_hot = c;
	if (fire && p_face != FACE_TOP) {
		for (int k = 0; k < 3; k++) {
			const Vector3 foot = xf.xform(_face_point(r, p_face, Vector2(fs.x * (0.2f + 0.3f * k), 0.0f)) + n * 0.5f);
			const float flaming = fire->get_intensity_at(foot);
			if (flaming > 0.0f && foot.y - fire->get_ground_height_at(foot) < MAX(fire->get_flame_length_at(foot), 0.3f)) {
				const float qf = MIN(60.0f * Math::sqrt(flaming / 600.0f), 60.0f);
				if (qf > q) {
					q = qf;
					r_hot = foot;
				}
			}
		}
	}
	if (flow) {
		const float t = CLAMP(flow->sample_temperature(c), 0.0f, 1.5f);
		q = MAX(q, 80.0f * t * t);
	}
	q += PhysXBurnable3D::radiant_flux_at(c) + radiant_flux_at(c, this, p_room);
	if (r.brand_glow[p_face] > 0.0f) {
		const float fan = 0.4f + 0.6f * MIN(Vector2(wind.x, wind.z).length() / 6.0f, 1.0f);
		const float qb = 50.0f * MIN(r.brand_glow[p_face] / 20.0f, 1.0f) * fan;
		if (qb > q) {
			r_hot = xf.xform(_face_point(r, p_face, r.brand_at[p_face]));
		}
		q = MAX(q, qb);
	}
	return q;
}

// Water from the liquid fluids (a hose, buckets, MPM water): the particles inside a room's box are water in the room,
// those within half a metre outside a face are water on its cladding.
void PhysXBuildingFire3D::_sample_fluids(const Transform3D &p_inv) {
	for (Room &r : rooms) {
		r.fluid_kg = 0.0f;
		for (int f = 0; f < FACE_COUNT; f++) {
			r.face_fluid_kg[f] = 0.0f;
		}
	}
	const LocalVector<PhysXParticleFluid3D *> &fluids = PhysXParticleFluid3D::get_water_fluids();
	if (fluids.is_empty() || rooms.is_empty()) {
		return;
	}
	AABB all = rooms[0].box;
	for (const Room &r : rooms) {
		all.merge_with(r.box);
	}
	all = all.grow(0.6f);
	LocalVector<Vector3> pts;
	for (const PhysXParticleFluid3D *fl : fluids) {
		if (!fl->is_inside_tree()) {
			continue;
		}
		pts.clear();
		fl->gather_world_positions(pts);
		const float mass = fl->get_particle_water_mass();
		for (const Vector3 &pw : pts) {
			const Vector3 p = p_inv.xform(pw);
			if (!all.has_point(p)) {
				continue;
			}
			for (Room &r : rooms) {
				if (r.box.has_point(p)) {
					r.fluid_kg += mass;
					break;
				}
				if (!r.box.grow(0.5f).has_point(p)) {
					continue;
				}
				// The face it is outside of.
				const Vector3 lo = r.box.position - p;
				const Vector3 hi = p - (r.box.position + r.box.size);
				const float d[FACE_COUNT] = { lo.x, hi.x, lo.z, hi.z, hi.y };
				int best = -1;
				for (int f = 0; f < FACE_COUNT; f++) {
					if (d[f] > 0.0f && r.exterior[f] && (best < 0 || d[f] > d[best])) {
						best = f;
					}
				}
				if (best >= 0) {
					r.face_fluid_kg[best] += mass;
				}
				break;
			}
		}
	}
}

void PhysXBuildingFire3D::apply_water(const Vector3 &p_position, float p_kg) {
	if (p_kg <= 0.0f) {
		return;
	}
	if (rooms_dirty) {
		_build_rooms();
	}
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	for (Room &r : rooms) {
		if (r.box.grow(0.05f).has_point(l)) {
			r.water_kg += p_kg;
			return;
		}
	}
	int best = -1;
	int best_face = 0;
	float bd = 1.0f;
	for (uint32_t i = 0; i < rooms.size(); i++) {
		const Room &r = rooms[i];
		for (int f = 0; f < FACE_COUNT; f++) {
			if (!r.exterior[f]) {
				continue;
			}
			const Vector2 fs = _face_size(r, f);
			const Vector2 uv = _face_uv(r, f, l);
			const float d = _face_point(r, f, Vector2(CLAMP(uv.x, 0.0f, fs.x), CLAMP(uv.y, 0.0f, fs.y))).distance_to(l);
			if (d < bd) {
				bd = d;
				best = i;
				best_face = f;
			}
		}
	}
	if (best < 0) {
		return;
	}
	// Through an open opening the water goes in.
	Room &r = rooms[best];
	const Vector2 uv = _face_uv(r, best_face, l);
	for (const Opening &o : r.openings) {
		if (o.face == best_face && o.to_room < 0 && (!o.glazed || o.broken) && o.rect.grow(0.1f).has_point(uv)) {
			r.water_kg += p_kg;
			return;
		}
	}
	r.face_water[best_face] += p_kg;
}

float PhysXBuildingFire3D::get_room_water_cooling(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	return rooms[p_room].cooling;
}

// Every 0.25 s: the cladding's heat dose; at a thick solid's ignition dose (pi/4) k rho c (T_ig - T0)^2 (as
// PhysXBurnable3D) the face catches where it is hottest. Wet wood (rain over 0.5 mm/h) takes no dose.
void PhysXBuildingFire3D::_sample(float p_delta) {
	float rain = 0.0f;
	t_air = 20.0f;
	wind = Vector3();
	const Vector3 centre = get_global_transform().origin;
	if (const WorldWeather *wm = WorldContracts::weather(); wm->has_provider()) {
		rain = MAX(wm->precipitation_at(centre), 0.0f);
		t_air = wm->air_temperature_at(centre);
		wind = wm->wind_at(centre);
	}
	if (wind == Vector3() && fire) {
		wind = fire->get_wind();
	}
	const float ig = 0.25f * Math::PI * WOOD_KRHOC * 330.0f * 330.0f;
	const Transform3D inv = get_global_transform().affine_inverse();
	_sample_fluids(inv);
	for (uint32_t i = 0; i < rooms.size(); i++) {
		Room &r = rooms[i];
		for (int f = 0; f < FACE_COUNT; f++) {
			r.brand_glow[f] *= Math::exp(-p_delta / 20.0f);
			if (!r.exterior[f] || r.clad_r[f] > 0.0f || r.state == ROOM_OUT || r.collapsed) {
				continue;
			}
			Vector3 hot;
			const float q = _exposure(i, f, hot);
			const float qw = q * 1000.0f;
			if (q > 5.0f && rain < 0.5f && r.face_water[f] + r.face_fluid_kg[f] < 0.1f) {
				r.clad_dose[f] += qw * qw * p_delta;
			} else {
				r.clad_dose[f] *= Math::exp(-p_delta / 30.0f);
			}
			if (r.clad_dose[f] >= ig) {
				const Vector2 fs = _face_size(r, f);
				const Vector2 uv = _face_uv(r, f, inv.xform(hot));
				r.clad_origin[f] = Vector2(CLAMP(uv.x, 0.0f, fs.x), CLAMP(uv.y, 0.0f, fs.y));
				r.clad_r[f] = 0.05f;
			}
		}
	}
}

// A room: t-squared growth (Q = 1 MW (t / growth_time)^2) up to its ventilation limit 1500 A sqrt(H) kW (Kawagoe)
// or its fuel's 250 kW/m2 of floor; the hot layer from MQH, dT = 6.85 (Q^2 / (A sqrt(H) h_k A_T))^(1/3) with the
// lining's h_k = sqrt(k rho c / t); glass falls out at about 450 C; flashover at Thomas's
// Q_FO = 7.8 A_T + 378 A sqrt(H) kW. Flashed, the fuel pyrolyses at its fuel-controlled rate and what the air cannot
// burn inside burns outside; it decays linearly once 70% has burnt (EN 1991-1-2 Annex E). Its timber chars at
// 0.8 mm a minute in a fully developed fire, slower in a cooler one.
void PhysXBuildingFire3D::_step_room(int p_room, float p_delta) {
	Room &r = rooms[p_room];
	// Cladding: the patch spreads, chars, lets the fire in where it reaches an opening or burns through.
	for (int f = 0; f < FACE_COUNT; f++) {
		r.face_water[f] = MAX(r.face_water[f] - r.face_water[f] * p_delta / 600.0f, 0.0f); // dries over ten minutes
		if (r.clad_r[f] <= 0.0f) {
			continue;
		}
		// Half a millimetre of water over the face puts its flames out (as PhysXBurnable3D).
		const Vector2 fsz = _face_size(r, f);
		if (r.face_water[f] + r.face_fluid_kg[f] > 0.5f * fsz.x * fsz.y) {
			r.steam_j += MIN(CLAD_HRR * Math::PI * r.clad_r[f] * r.clad_r[f] * 0.5f, 2000.0f) * 1000.0f * 5.0f;
			r.clad_r[f] = 0.0f;
			r.clad_dose[f] = 0.0f;
			continue;
		}
		if (r.state == ROOM_OUT && r.fuel_left <= 0.0f) {
			r.clad_r[f] = 0.0f;
			continue;
		}
		const Vector2 fs = _face_size(r, f);
		r.clad_r[f] = MIN(r.clad_r[f] + CLAD_SPREAD * p_delta, fs.length());
		r.clad_char[f] += CHAR_RATE * p_delta;
		for (Opening &o : r.openings) {
			if (o.face == f && o.to_room < 0) {
				const Vector2 cl = Vector2(CLAMP(r.clad_origin[f].x, o.rect.position.x, o.rect.get_end().x), CLAMP(r.clad_origin[f].y, o.rect.position.y, o.rect.get_end().y));
				if (cl.distance_to(r.clad_origin[f]) < r.clad_r[f]) {
					o.broken = true;
					_ignite_room(p_room, _face_point(r, f, o.rect.get_center()));
				}
			}
		}
		if (r.clad_char[f] >= (f == FACE_TOP ? roof_thickness : wall_thickness)) {
			if (f == FACE_TOP) {
				r.roof_open = true;
			}
			_ignite_room(p_room, _face_point(r, f, r.clad_origin[f]));
		}
	}
	// Water in the room (docs/ideas/physx.md, "Water puts building fires out"): what lies in it boils off taking
	// 2.6 MJ/kg, over about eight seconds where it meets the fire.
	const float water_kw = (r.water_kg + r.fluid_kg) * PhysXSteam::BOIL_HEAT / 8.0f / 1000.0f;
	r.water_kg = MAX(r.water_kg - r.water_kg * p_delta / 8.0f, 0.0f);
	r.cooling = (r.state == ROOM_COLD || r.state == ROOM_OUT) ? 0.0f : water_kw;
	if (r.state == ROOM_OUT && water_kw > 0.0f) {
		r.ember = MAX(r.ember - p_delta / 30.0f, 0.0f); // the char drowned
	}
	if (r.state == ROOM_COLD || r.state == ROOM_OUT) {
		r.q = 0.0f;
		r.q_ext = 0.0f;
		r.temperature = Math::lerp(r.temperature, t_air, 1.0f - Math::exp(-p_delta / 600.0f));
		if (r.state == ROOM_OUT) {
			r.ember *= Math::exp(-p_delta / 1800.0f); // the char glows out over the next hours
		}
		return;
	}
	r.t += p_delta;
	// Absorbing about a third of what the fire could release puts the flames down (Grimwood; Sardqvist's critical
	// flow of about 0.1 to 0.3 kg/s per MW): the share it absorbs damps the fuel's burning at once, and a room held
	// under that for twenty seconds goes out. Flashed rooms fall back to a growing fire; once the water stops it can
	// grow again from what is left.
	float damp = 0.0f;
	if (water_kw > 0.0f && (r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY)) {
		const float need = 0.3f * MAX(r.potential, 10.0f);
		const float share = CLAMP(water_kw / need, 0.0f, 1.0f);
		r.knock = share >= 1.0f ? r.knock + p_delta / 20.0f : r.knock - p_delta / 120.0f;
		damp = MAX(0.7f * share, r.knock);
		r.steam_j += MIN(water_kw, r.q + r.q_ext + 50.0f) * 1000.0f * p_delta;
	} else {
		r.knock -= p_delta / 120.0f;
	}
	r.knock = CLAMP(r.knock, 0.0f, 1.0f);
	if (r.knock >= 1.0f) {
		r.state = ROOM_OUT;
		r.ember = 0.2f; // wet char
		r.q = 0.0f;
		r.q_ext = 0.0f;
		r.knock = 0.0f;
		return;
	}
	const float floor_area = r.box.size.x * r.box.size.z;
	const float vent = _vent(r);
	const float q_vent = 1500.0f * vent;
	const float q_fuel = heat_release_rate * floor_area;
	const float a_t = _lining_area(r);
	float pyro = 0.0f; // kW of fuel given off
	if (r.state == ROOM_GROWING) {
		const float tg = r.t / growth_time;
		r.potential = MIN(1000.0f * tg * tg + 10.0f, q_fuel);
		pyro = r.potential * (1.0f - damp);
		if (r.knock > 0.0f) { // the water is winning: the fire shrinks, and regrows from there
			r.t = MIN(r.t, growth_time * Math::sqrt(MAX(pyro - 10.0f, 0.0f) / 1000.0f)); // it regrows from here
		}
		r.q = MIN(pyro, q_vent);
		r.q_ext = 0.0f;
		const float hk = Math::sqrt(LINING_KRHOC / MAX(r.t, 10.0f)) / 1000.0f;
		const float qc = MAX(r.q - water_kw, 0.0f); // the gas the water has not cooled
		const float dt = 6.85f * Math::pow(qc * qc / (vent * hk * a_t), 1.0f / 3.0f);
		r.temperature = damp > 0.0f ? Math::lerp(r.temperature, MIN(t_air + dt, 1100.0f), 1.0f - Math::exp(-p_delta / 15.0f)) : MIN(t_air + dt, 1100.0f);
		const float q_fo = 7.8f * a_t + 378.0f * vent;
		// Flashover; a room open enough never meets Thomas's condition but is fully involved once its fuel burns at
		// its own rate.
		if (damp <= 0.0f && (r.q >= q_fo || r.q >= 0.95f * q_fuel)) {
			r.state = ROOM_FLASHED;
			r.flash_time = r.t;
		} else if (r.fuel_left <= 0.0f) {
			r.state = ROOM_OUT;
			r.ember = 1.0f;
		} else if (pyro > q_vent * 1.5f && r.t > 1800.0f) {
			// Starved of air in a closed room: it dies down to embers (Drysdale).
			r.state = ROOM_OUT;
			r.ember = 0.3f;
		}
	}
	if (r.state == ROOM_FLASHED || r.state == ROOM_DECAY) {
		// Decay: linear to nothing over the time that burns the fuel left (EN 1991-1-2 Annex E).
		if (r.state == ROOM_FLASHED && r.fuel_left < 0.3f * r.fuel_total) {
			r.state = ROOM_DECAY;
			r.q_decay = MAX(q_fuel, 1.0f);
			r.decay_t0 = r.t;
			r.decay_time = 2.0f * r.fuel_left * 1000.0f / r.q_decay;
		}
		pyro = r.state == ROOM_DECAY ? r.q_decay * MAX(1.0f - (r.t - r.decay_t0) / MAX(r.decay_time, 1.0f), 0.0f) : q_fuel;
		r.potential = MAX(pyro, r.state == ROOM_FLASHED ? q_fuel : 0.0f);
		pyro *= 1.0f - damp;
		r.q = MIN(pyro, q_vent);
		r.q_ext = MAX(pyro - q_vent, 0.0f);
		// Hot layer: MQH with the lining heated through, held between 800 C and 1100 C while fully developed (and
		// not being cooled).
		const float hk = Math::sqrt(LINING_KRHOC / MAX(r.t, 10.0f)) / 1000.0f;
		const float qc = MAX(r.q - water_kw, 0.0f);
		const float dt = 6.85f * Math::pow(qc * qc / (vent * hk * a_t), 1.0f / 3.0f);
		const float target = (r.state == ROOM_FLASHED && damp <= 0.0f) ? CLAMP(t_air + dt, 800.0f, 1100.0f) : MIN(t_air + dt, 1100.0f);
		r.temperature = Math::lerp(r.temperature, target, 1.0f - Math::exp(-p_delta / (damp > 0.0f ? 15.0f : 60.0f)));
		// Knocked down below flashover: back to a growing fire of what still burns.
		if (r.state == ROOM_FLASHED && damp > 0.5f) {
			r.state = ROOM_GROWING;
			r.t = growth_time * Math::sqrt(MAX(pyro, 10.0f) / 1000.0f);
			r.potential = MAX(pyro, 10.0f);
			r.q_ext = 0.0f;
		} else if (r.fuel_left <= 0.0f || (pyro < 20.0f && damp <= 0.0f)) {
			r.state = ROOM_OUT;
			r.ember = 1.0f;
			r.q = 0.0f;
			r.q_ext = 0.0f;
		}
	}
	// Before flashover what the air cannot burn smoulders on (little is lost); after, it burns outside.
	r.fuel_left = MAX(r.fuel_left - (r.state == ROOM_GROWING ? r.q : pyro) / 1000.0f * p_delta, 0.0f);
	for (Opening &o : r.openings) {
		if (o.glazed && !o.broken && r.temperature >= o.break_at) {
			o.broken = true;
		}
	}
	if (r.temperature > 300.0f) {
		r.char_depth += CHAR_RATE * CLAMP((r.temperature - 300.0f) / 600.0f, 0.0f, 1.5f) * p_delta;
	}
	if (!r.roof_open && r.exterior[FACE_TOP] && r.char_depth >= roof_thickness) {
		r.roof_open = true;
	}
	if (!r.collapsed && 2.0f * r.char_depth >= collapse_share * member_size) {
		r.collapsed = true;
		r.roof_open = r.roof_open || r.exterior[FACE_TOP];
	}
}

// Smoke ahead of the fire: a two-zone layer per room (Quintiere; as CFAST). A burning room's plume entrains air
// into the layer under its ceiling (Heskestad: m = 0.071 Q_c^(1/3) z^(5/3) + 0.0018 Q_c kg/s, with Q_c = 0.7 Q and z
// the clear height), carrying soot at smoke_yield of the fuel burnt. The layer leaves through every opening whose top
// is below it as a buoyant flow, m = (2/3) Cd W rho_s sqrt(2 g (1 - rho_s / rho_a)) dh^(3/2) with dh the depth of
// layer in the opening, and through the leaks at the ceiling; through a door it fills the next room, which smokes in
// turn. Visibility in the layer is 3 / K for soot's mass extinction of 8.7 m2/g (Mulholland).
void PhysXBuildingFire3D::_smoke(float p_delta) {
	const float rho_a = 353.0f / (t_air + 273.15f);
	for (Room &r : rooms) {
		const float area = r.box.size.x * r.box.size.z;
		const float h = r.box.size.y;
		const bool burning = r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY;
		const float t_layer = burning ? MAX(r.temperature, r.layer_temp) : r.layer_temp;
		const float rho_s = 353.0f / (t_layer + 273.15f);
		float depth = r.layer_mass / MAX(rho_s * area, 1e-3f);
		if (burning && r.q > 0.0f) {
			const float qc = 0.7f * r.q;
			const float z = MAX(h - depth, 0.1f);
			const float m = 0.071f * Math::pow(qc, 1.0f / 3.0f) * Math::pow(z, 5.0f / 3.0f) + 0.0018f * qc;
			r.layer_mass += m * p_delta;
			r.layer_temp = t_layer;
			r.soot += smoke_yield * (r.q + r.q_ext) / (FUEL_HEAT * 1000.0f) * p_delta;
		} else {
			// A layer with no fire under it cools toward the air (and the walls) over a few minutes.
			r.layer_temp = Math::lerp(r.layer_temp, t_air, 1.0f - Math::exp(-p_delta / 300.0f));
		}
		depth = r.layer_mass / MAX(rho_s * area, 1e-3f);
		const float buoy = Math::sqrt(2.0f * 9.81f * MAX(1.0f - rho_s / rho_a, 0.02f));
		const float soot_share = r.soot / MAX(r.layer_mass, 1e-3f);
		float out = 0.0f;
		r.smoke_out = 0.0f;
		auto vent = [&](float p_width, float p_dh, int p_to) {
			if (p_dh <= 0.0f) {
				return;
			}
			const float m = MIN((2.0f / 3.0f) * 0.7f * p_width * rho_s * buoy * Math::pow(p_dh, 1.5f) * p_delta, MAX(r.layer_mass - out, 0.0f));
			out += m;
			if (p_to >= 0) {
				Room &n = rooms[p_to];
				const float mix = m / MAX(n.layer_mass + m, 1e-6f);
				n.layer_temp = Math::lerp(n.layer_temp, t_layer, mix);
				n.layer_mass += m;
				n.soot += m * soot_share;
			} else {
				r.smoke_out += m / MAX(p_delta, 1e-3f);
			}
		};
		const float z_i = h - depth; // the layer's underside above the floor
		for (const Opening &o : r.openings) {
			if (o.face == FACE_TOP) {
				if (!o.glazed || o.broken) {
					vent(Math::sqrt(o.rect.get_area()), MIN(depth, 1.0f), o.to_room);
				}
				continue;
			}
			const float top = o.rect.get_end().y;
			const float bottom = o.rect.position.y;
			if (z_i >= top) {
				continue;
			}
			if (o.glazed && !o.broken) {
				continue;
			}
			vent(o.rect.size.x, top - MAX(z_i, bottom), o.to_room);
		}
		if (r.roof_open) {
			vent(Math::sqrt(area), MIN(depth, 1.0f), -1);
		}
		// Leaks at the ceiling (eaves, gaps round doors): about 0.01 m2.
		vent(0.1f, MIN(depth, 0.1f) * 0.1f, -1);
		if (out > 0.0f) {
			r.soot = MAX(r.soot - out * soot_share, 0.0f);
			r.layer_mass = MAX(r.layer_mass - out, 0.0f);
		}
		// A full room pushes the rest out (down to the floor).
		const float full = rho_s * area * h;
		if (r.layer_mass > full) {
			const float spill = r.layer_mass - full;
			r.soot = MAX(r.soot - spill * soot_share, 0.0f);
			r.layer_mass = full;
			r.smoke_out += spill / MAX(p_delta, 1e-3f);
		}
		// Soot settles out of a still layer over hours.
		r.soot *= Math::exp(-p_delta / 7200.0f);
	}
}

float PhysXBuildingFire3D::get_room_layer_height(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	const Room &r = rooms[p_room];
	const float rho_s = 353.0f / (r.layer_temp + 273.15f);
	return MAX(r.box.size.y - r.layer_mass / MAX(rho_s * r.box.size.x * r.box.size.z, 1e-3f), 0.0f);
}

float PhysXBuildingFire3D::_extinction(const Room &p_room) const {
	const float rho_s = 353.0f / (p_room.layer_temp + 273.15f);
	const float vol = p_room.layer_mass / MAX(rho_s, 1e-3f);
	return vol > 1e-3f ? 8700.0f * p_room.soot / vol : 0.0f;
}

float PhysXBuildingFire3D::get_room_visibility(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 1000.0f);
	const float k = _extinction(rooms[p_room]);
	return k > 0.003f ? 3.0f / k : 1000.0f;
}

float PhysXBuildingFire3D::get_visibility_at(const Vector3 &p_position) const {
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	for (uint32_t i = 0; i < rooms.size(); i++) {
		const Room &r = rooms[i];
		if (r.box.has_point(l)) {
			return l.y - r.box.position.y >= get_room_layer_height(i) ? get_room_visibility(i) : 1000.0f;
		}
	}
	return 1000.0f;
}

// Fire between rooms: through an open door or hatch the hot layer of a room over 500 C radiates into the next
// (half the view of a blackbody at its temperature) and its flames reach through once it flashes; a wall, floor or
// ceiling that burns through lets it in; a room collapsing drops burning timber into the one below; flames out of a
// window lap the window above and break it (leapfrogging). Their ground below catches under a window's flames.
void PhysXBuildingFire3D::_spread(float p_delta) {
	const float ig = 0.25f * Math::PI * WOOD_KRHOC * 330.0f * 330.0f;
	for (uint32_t i = 0; i < rooms.size(); i++) {
		Room &r = rooms[i];
		const bool hot = (r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY) && r.temperature > 500.0f;
		if (!hot) {
			continue;
		}
		for (const Opening &o : r.openings) {
			if (o.to_room < 0 || rooms[o.to_room].state != ROOM_COLD) {
				continue;
			}
			Room &n = rooms[o.to_room];
			const float tk = r.temperature + 273.15f;
			const float q = 0.5f * SIGMA * tk * tk * tk * tk;
			n.neighbour_dose += q * q * p_delta;
			if (n.neighbour_dose >= ig || r.state == ROOM_FLASHED) {
				const Vector3 at = _face_point(r, o.face, o.rect.get_center()) + _face_normal(o.face) * 0.3f;
				_ignite_room(o.to_room, at);
			}
		}
		for (uint32_t j = 0; j < rooms.size(); j++) {
			Room &n = rooms[j];
			if (j == i || (n.state != ROOM_COLD && n.state != ROOM_GROWING)) {
				continue;
			}
			const AABB &a = r.box;
			const AABB &b = n.box;
			const bool touch = a.grow(0.3f).intersects(b) && !a.grow(-0.05f).intersects(b);
			if (!touch) {
				continue;
			}
			const bool above = b.position.y >= a.position.y + a.size.y - 0.3f;
			const bool below = b.position.y + b.size.y <= a.position.y + 0.3f;
			const float through = above ? floor_thickness : (below ? 1e9f : wall_thickness);
			if (r.char_depth >= through || r.collapsed) {
				// Through a burnt floor or a fallen ceiling the room above opens onto the fire.
				n.floor_open = n.floor_open || above;
				const Vector3 at = above ? Vector3(b.get_center().x, b.position.y, b.get_center().z) : (a.get_center() + b.get_center()) * 0.5f;
				_ignite_room(j, at);
			}
		}
		// Window flames on the facade above.
		for (Opening &o : r.openings) {
			const float l = _external_flame(r, o);
			if (l <= 0.0f) {
				continue;
			}
			const Vector3 top = _face_point(r, o.face, Vector2(o.rect.get_center().x, o.rect.get_end().y));
			for (uint32_t j = 0; j < rooms.size(); j++) {
				if (j == i) {
					continue;
				}
				Room &n = rooms[j];
				for (Opening &no : n.openings) {
					if (no.face != o.face || no.to_room >= 0 || (no.broken && n.state != ROOM_COLD)) {
						continue;
					}
					const Vector3 nb = _face_point(n, no.face, Vector2(no.rect.get_center().x, no.rect.position.y));
					const Vector3 d = nb - top;
					const Vector3 nn = _face_normal(o.face);
					if (Math::abs(d.dot(nn)) > 0.4f || d.y < -0.1f || d.y > l) {
						continue;
					}
					const float lateral = (d - nn * d.dot(nn) - Vector3(0, d.y, 0)).length();
					if (lateral > 0.5f * (o.rect.size.x + no.rect.size.x)) {
						continue;
					}
					no.flame_contact += p_delta;
					if (no.flame_contact > 60.0f) {
						no.broken = true;
						_ignite_room(j, nb);
					}
				}
			}
			// The ground under the flames (grass and litter against the wall): their radiation, a point source at
			// the window, lights it once the dose reaches its ignition dose (as the cladding's).
			if (fire) {
				const Vector3 g = get_global_transform().xform(_face_point(r, o.face, Vector2(o.rect.get_center().x, 0.0f)) + _face_normal(o.face) * 0.8f);
				const float dist = o.rect.position.y + 0.8f;
				const float qg = 1000.0f * CHI_R * (r.q_ext + 0.2f * r.q) / (4.0f * Math::PI * dist * dist); // W/m2
				if (qg > 10000.0f) {
					o.ground_dose += qg * qg * p_delta;
					if (o.ground_dose >= ig) {
						o.ground_dose = 0.0f;
						fire->ignite(g, 0.6f);
					}
				}
			}
		}
	}
}

// A PhysXDestructible3D target (Blast) gives way where a room's framing fails: its bonds there are damaged as the
// char eats the members (as PhysXBurnable3D), all at once when it collapses.
void PhysXBuildingFire3D::_weaken(int p_room) {
#ifdef GODOT_PHYSX_BLAST
	PhysXDestructible3D *d = Object::cast_to<PhysXDestructible3D>(ObjectDB::get_instance(destructible_id));
	Room &r = rooms[p_room];
	if (!d) {
		return;
	}
	const float lost = r.collapsed ? 1.0f : CLAMP(2.0f * r.char_depth / (collapse_share * member_size), 0.0f, 1.0f) * 0.5f;
	if (lost - r.weakened < 0.05f) {
		return;
	}
	const float dmg = d->get_health() * (lost - r.weakened) * 2.0f;
	r.weakened = lost;
	const Vector3 at = get_global_transform().xform(r.box.get_center() + Vector3(0, r.box.size.y * 0.4f, 0));
	const float rad = r.box.size.length() * 0.5f;
	const float shatter = d->get_shatter_speed();
	d->set_shatter_speed(0.0f);
	d->apply_radial_damage(at, dmg, rad * 0.6f, rad);
	d->set_shatter_speed(shatter);
#endif
}

PhysXFlowEmitter3D *PhysXBuildingFire3D::_emitter(ObjectID &r_id, const String &p_name) {
	PhysXFlowEmitter3D *e = Object::cast_to<PhysXFlowEmitter3D>(ObjectDB::get_instance(r_id));
	if (!e && flow) {
		e = memnew(PhysXFlowEmitter3D);
		e->set_name(String(get_name()) + p_name);
		flow->add_child(e);
		e->set_flow_path(e->get_path_to(flow));
		e->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
		r_id = e->get_instance_id();
	}
	return e;
}

void PhysXBuildingFire3D::_disable(ObjectID p_id) {
	if (PhysXFlowEmitter3D *e = Object::cast_to<PhysXFlowEmitter3D>(ObjectDB::get_instance(p_id))) {
		e->set_enabled(false);
	}
}

// Flames and smoke into Flow: inside each burning room (seen through its windows), smoke pouring from the top of
// its openings before flashover (above the neutral plane), flames out of them after (rising to Eurocode's length),
// through an open roof, and on burning cladding. One light per room at its fire, flickering.
void PhysXBuildingFire3D::_update_visuals(float p_delta) {
	const Transform3D xf = get_global_transform();
	flicker += p_delta;
	for (uint32_t i = 0; i < rooms.size(); i++) {
		Room &r = rooms[i];
		const bool burning = r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY;
		const float q_all = r.q + r.q_ext;
		const String base = "Room" + itos(i);
		if (!flow) {
			continue;
		}
		// Inside: a fire of about Q / (250 kW/m2) of floor, filling the room once flashed.
		if (burning) {
			PhysXFlowEmitter3D *e = _emitter(r.inner_emitter, base + "Fire");
			const float half = 0.5f * MIN(r.box.size.x, r.box.size.z);
			const float rad = r.state == ROOM_GROWING ? CLAMP(Math::sqrt(r.q / (250.0f * Math::PI)), 0.15f, half) : half;
			const Vector3 at = r.state == ROOM_GROWING ? r.origin + Vector3(0, rad * 0.6f, 0) : r.box.get_center() - Vector3(0, r.box.size.y * 0.2f, 0);
			e->set_enabled(true);
			e->set_radius(rad);
			e->set_global_position(xf.xform(at));
			e->set_temperature(1.0f);
			e->set_fuel(CLAMP(r.q / 2000.0f, 0.15f, 0.5f));
			e->set_smoke(0.3f);
			e->set_burn(0.0f);
			e->set_couple_rate(2.0f);
			e->set_velocity(Vector3(0, CLAMP(r.box.size.y * 0.6f, 0.4f, 2.0f), 0));
		} else {
			_disable(r.inner_emitter);
		}
		for (uint32_t k = 0; k < r.openings.size(); k++) {
			Opening &o = r.openings[k];
			const bool open = !o.glazed || o.broken;
			const float layer = get_room_layer_height(i);
			const bool smoking = open && o.to_room < 0 && r.smoke_out > 0.002f && (o.face == FACE_TOP || layer < o.rect.get_end().y);
			if ((!burning && !smoking) || !open || o.to_room >= 0) {
				_disable(o.emitter);
				continue;
			}
			const Vector3 n = _face_normal(o.face);
			PhysXFlowEmitter3D *e = _emitter(o.emitter, base + "Opening" + itos(k));
			e->set_enabled(true);
			e->set_burn(0.0f);
			e->set_couple_rate(2.0f);
			if (o.face == FACE_TOP) {
				e->set_radius(0.5f * MIN(o.rect.size.x, o.rect.size.y));
				e->set_global_position(xf.xform(_face_point(r, o.face, o.rect.get_center()) + n * 0.2f));
			} else {
				e->set_radius(CLAMP(0.35f * MIN(o.rect.size.x, o.rect.size.y), 0.15f, 0.7f));
				// The hot layer leaves through the opening's upper part.
				const Vector2 uv(o.rect.get_center().x, o.rect.position.y + o.rect.size.y * 0.8f);
				e->set_global_position(xf.xform(_face_point(r, o.face, uv) + n * 0.25f));
			}
			const float l = _external_flame(r, o);
			if (l > 0.0f || (o.face == FACE_TOP && r.state != ROOM_GROWING)) {
				// Tongues about the window's width, reaching Eurocode's length above it (not fireballs).
				e->set_temperature(1.0f);
				e->set_fuel(CLAMP(0.2f + 0.15f * l, 0.2f, 0.55f));
				e->set_smoke(0.4f);
				e->set_velocity(xf.basis.xform(n * 1.2f + Vector3(0, CLAMP(l * 0.8f, 0.6f, 3.0f), 0)));
			} else {
				// Smoke from the layer before flashover (and from smoke-filled rooms the fire has not reached): as thick
				// as its soot, as much as flows out, as warm as the layer.
				const float k = _extinction(r);
				const float h = CLAMP((r.layer_temp - 20.0f) / 500.0f, 0.0f, 1.0f);
				const float amount = CLAMP(k / 2.0f, 0.05f, 1.0f) * CLAMP(r.smoke_out / 0.5f, 0.1f, 1.0f);
				e->set_temperature(0.05f + 0.2f * h);
				e->set_fuel(0.0f);
				e->set_smoke(amount);
				e->set_velocity(xf.basis.xform(n * (0.4f + 0.6f * h) + Vector3(0, 0.3f + 0.4f * h, 0)));
			}
		}
		if (burning && r.roof_open) {
			PhysXFlowEmitter3D *e = _emitter(r.roof_emitter, base + "Roof");
			e->set_enabled(true);
			e->set_radius(0.4f * MIN(r.box.size.x, r.box.size.z));
			e->set_global_position(xf.xform(r.box.get_center() + Vector3(0, r.box.size.y * 0.5f, 0)));
			e->set_temperature(1.0f);
			e->set_fuel(CLAMP(q_all / 2000.0f, 0.3f, 1.0f));
			e->set_smoke(0.5f);
			e->set_burn(0.0f);
			e->set_couple_rate(2.0f);
			e->set_velocity(Vector3(0, 3.0f, 0));
		} else {
			_disable(r.roof_emitter);
		}
		for (int f = 0; f < FACE_COUNT; f++) {
			if (r.clad_r[f] <= 0.0f || (r.state == ROOM_OUT && r.ember <= 0.0f)) {
				_disable(r.clad_emitter[f]);
				continue;
			}
			const Vector2 fs = _face_size(r, f);
			const float rad = CLAMP(r.clad_r[f] * 0.5f, 0.15f, 0.5f * MAX(fs.x, fs.y));
			Vector2 uv = r.clad_origin[f] + (f == FACE_TOP ? Vector2() : Vector2(0, r.clad_r[f] * 0.5f));
			uv = Vector2(CLAMP(uv.x, 0.0f, fs.x), CLAMP(uv.y, 0.0f, fs.y));
			PhysXFlowEmitter3D *e = _emitter(r.clad_emitter[f], base + "Cladding" + itos(f));
			e->set_enabled(true);
			e->set_radius(rad);
			e->set_global_position(xf.xform(_face_point(r, f, uv) + _face_normal(f) * rad * 0.6f));
			e->set_temperature(1.0f);
			e->set_fuel(0.6f);
			e->set_smoke(0.2f);
			e->set_burn(0.0f);
			e->set_couple_rate(2.0f);
			e->set_velocity(Vector3(0, 1.0f, 0));
		}
		// The light of the fire: a point at the flames, as bright as the flames' radiant output.
		OmniLight3D *light = Object::cast_to<OmniLight3D>(ObjectDB::get_instance(r.light));
		const float glow = burning ? Math::sqrt(MAX(r.q, 1.0f) / 1000.0f) : 0.15f * r.ember;
		if (glow > 0.01f && light_energy > 0.0f) {
			if (!light) {
				light = memnew(OmniLight3D);
				light->set_name(base + "Light");
				light->set_color(Color(1.0f, 0.55f, 0.25f));
				light->set_shadow(true);
				add_child(light, false, INTERNAL_MODE_FRONT);
				r.light = light->get_instance_id();
			}
			light->set_visible(true);
			const float n = 0.85f + 0.15f * Math::sin(flicker * 11.0f + i * 1.7f) * Math::sin(flicker * 4.3f + i);
			light->set_param(Light3D::PARAM_ENERGY, light_energy * glow * (burning ? n : 1.0f));
			light->set_param(Light3D::PARAM_RANGE, r.box.size.length() * (burning ? 1.5f : 0.8f));
			const Vector3 at = r.state == ROOM_GROWING ? r.origin + Vector3(0, 0.4f, 0) : r.box.get_center();
			light->set_position(at);
			light->set_color(burning ? Color(1.0f, 0.55f, 0.25f) : Color(1.0f, 0.25f, 0.06f));
		} else if (light) {
			light->set_visible(false);
		}
	}
}

// Steam: the heat the water took leaves as steam (PhysXSteam), out of the room's openings and off its cladding.
void PhysXBuildingFire3D::_update_steam(float p_delta) {
	const Transform3D xf = get_global_transform();
	for (Room &r : rooms) {
		if (r.steam_j < 2.0e5f) {
			continue;
		}
		Vector3 at = r.box.get_center() + Vector3(0, r.box.size.y * 0.3f, 0);
		for (const Opening &o : r.openings) {
			if (o.to_room < 0 && (!o.glazed || o.broken)) {
				at = _face_point(r, o.face, Vector2(o.rect.get_center().x, o.rect.get_end().y - 0.1f)) + _face_normal(o.face) * 0.3f;
				break;
			}
		}
		steam.add_heat(xf.xform(at), r.steam_j, CLAMP(0.25f * r.box.size.length(), 0.4f, 1.5f), 3.0f);
		r.steam_j = 0.0f;
	}
	if (steam.is_steaming() || steam.get_total_kg() > 0.0f) {
		steam.step(this, wind, p_delta);
	}
}

// Firebrands from the flames out of its windows and its open roofs, lofted by its plume (Byram's column over a fire
// as wide as the building), falling where PhysXFirebrands takes them: spot fires on the ground, piles on burnables
// and buildings. Structure fires throw many: about 40 a second per megawatt (Suzuki and Manzello 2017).
void PhysXBuildingFire3D::_update_brands(float p_delta) {
	if (firebrands) {
		const Transform3D xf = get_global_transform();
		LocalVector<PhysXFirebrands::Release> pts;
		float q = 0.0f;
		AABB all;
		bool first = true;
		for (const Room &r : rooms) {
			if (first) {
				all = r.box;
				first = false;
			} else {
				all.merge_with(r.box);
			}
			if (r.state != ROOM_FLASHED && r.state != ROOM_DECAY) {
				continue;
			}
			q += r.q + r.q_ext;
			if (r.roof_open) {
				pts.push_back({ xf.xform(r.box.get_center() + Vector3(0, r.box.size.y * 0.5f, 0)), 2.0f });
			}
			for (const Opening &o : r.openings) {
				if (_external_flame(r, o) > 0.0f) {
					pts.push_back({ xf.xform(_face_point(r, o.face, Vector2(o.rect.get_center().x, o.rect.get_end().y)) + _face_normal(o.face) * 0.3f), 1.0f });
				}
			}
		}
		if (q > 0.0f && !pts.is_empty() && flow) {
			const float width = MAX(MAX(all.size.x, all.size.z), 1.0f);
			LocalVector<PhysXFirebrands::Plume> plume;
			PhysXFirebrands::Plume pl;
			pl.base = xf.xform(all.get_center() + Vector3(0, all.size.y * 0.5f, 0));
			pl.w0 = PhysXFirebrands::plume_speed(q / width, t_air);
			pl.width = width;
			plume.push_back(pl);
			brands.set_plumes(plume, width);
			brands.release(firebrand_rate * q / 1000.0f * p_delta, pts, flow);
		}
	}
	if (brands.get_count() > 0) {
		brands.step(this, flow, wind, p_delta, 6.0f);
	}
}

// The meshes char with the burn material PhysXBurnable3D uses: per mesh, the room it sits in most, its char
// spreading from the fire's origin over the room as the room burns, glowing while it is fully involved.
void PhysXBuildingFire3D::_update_look() {
	Node3D *t = _target();
	if (!t) {
		return;
	}
	const Transform3D xf = get_global_transform();
	LocalVector<Node *> stack;
	stack.push_back(t);
	while (!stack.is_empty()) {
		Node *nd = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int c = 0; c < nd->get_child_count(); c++) {
			stack.push_back(nd->get_child(c));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(nd);
		if (!mi || mi->get_mesh().is_null()) {
			continue;
		}
		if (!look_done) {
			for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++) {
				Ref<Material> base = mi->get_surface_override_material(s);
				if (base.is_null()) {
					base = mi->get_mesh()->surface_get_material(s);
				}
				const Ref<Material> m = PhysXBurnable3D::_burn_material(base);
				if (m.is_valid() && m != base) {
					mi->set_surface_override_material(s, m);
				}
			}
		}
		const Transform3D to_local = xf.affine_inverse() * mi->get_global_transform();
		const Vector3 c = to_local.xform(mi->get_aabb().get_center());
		int best = -1;
		float best_burn = -1.0f;
		for (uint32_t i = 0; i < rooms.size(); i++) {
			const Room &r = rooms[i];
			if (!r.box.grow(0.5f).has_point(c)) {
				continue;
			}
			bool lit = r.state != ROOM_COLD;
			for (int f = 0; f < FACE_COUNT; f++) {
				lit = lit || r.clad_r[f] > 0.0f;
			}
			const float burn = lit ? 1.0f + r.char_depth : 0.0f;
			if (burn > best_burn) {
				best_burn = burn;
				best = i;
			}
		}
		Vector4 o(0, 0, 0, 0);
		Vector4 st(0, 0, 0.1f, 0);
		if (best >= 0 && best_burn > 0.0f) {
			const Room &r = rooms[best];
			const Transform3D to_mesh = mi->get_global_transform().affine_inverse() * xf;
			const float sc = to_mesh.basis.get_scale().x;
			// Inside, the linings char once it is fully developed, over about ten minutes from the fire's origin
			// (soot before that does not show from outside); outside, the cladding's burning patch.
			Vector3 org = r.state != ROOM_COLD ? r.origin : r.box.get_center();
			float reach = r.flash_time >= 0.0f ? r.box.size.length() * CLAMP((r.t - r.flash_time) / 600.0f + 0.2f, 0.0f, 1.2f) : 0.0f;
			for (int f = 0; f < FACE_COUNT; f++) {
				if (r.clad_r[f] > reach) {
					reach = r.clad_r[f];
					org = _face_point(r, f, r.clad_origin[f]);
				}
			}
			const bool flaming = r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY;
			const Vector3 om = to_mesh.xform(org);
			o = Vector4(om.x, om.y, om.z, reach * sc);
			st = Vector4(flaming ? 6.0f : 0.0f, r.ember * 3.0f, CLAMP(r.box.size.length() * 0.03f, 0.05f, 0.3f) * sc, 1.0f);
		}
		mi->set_instance_shader_parameter("burn_origin", o);
		mi->set_instance_shader_parameter("burn_state", st);
	}
	look_done = true;
}

float PhysXBuildingFire3D::radiant_flux_at(const Vector3 &p_position, const PhysXBuildingFire3D *p_skip, int p_skip_room) {
	float q = 0.0f;
	for (const PhysXBuildingFire3D *b : buildings) {
		if (!b->is_inside_tree()) {
			continue;
		}
		const Transform3D xf = b->get_global_transform();
		for (uint32_t i = 0; i < b->rooms.size(); i++) {
			const Room &r = b->rooms[i];
			if ((b == p_skip && int(i) == p_skip_room) || (r.state != ROOM_GROWING && r.state != ROOM_FLASHED && r.state != ROOM_DECAY)) {
				continue;
			}
			// Openings: blackbody panels at the layer's temperature, A cos / (pi d^2).
			const float tk = r.temperature + 273.15f;
			const float e = SIGMA * tk * tk * tk * tk / 1000.0f;
			for (const Opening &o : r.openings) {
				if ((o.glazed && !o.broken) || o.to_room >= 0) {
					continue;
				}
				const Vector3 n = xf.basis.xform(_face_normal(o.face)).normalized();
				const Vector3 c = xf.xform(b->_face_point(r, o.face, o.rect.get_center()));
				const Vector3 d = p_position - c;
				const float dl = MAX(d.length(), 0.3f);
				const float cs = d.dot(n) / dl;
				if (cs > 0.0f) {
					q += MIN(e * o.rect.get_area() * cs / (Math::PI * dl * dl), e);
				}
			}
			// Flames outside and through the roof: a point source each.
			const float qf = r.q_ext + (r.roof_open ? r.q * 0.5f : 0.0f);
			if (qf > 0.0f) {
				const Vector3 c = xf.xform(r.box.get_center() + Vector3(0, r.box.size.y * 0.5f, 0));
				const float d = MAX(c.distance_to(p_position) - 0.25f * r.box.size.length(), 0.5f);
				q += CHI_R * qf / (4.0f * Math::PI * d * d);
			}
		}
	}
	return q;
}

PhysXBuildingFire3D *PhysXBuildingFire3D::find_by_collider(Object *p_collider) {
	Node *c = Object::cast_to<Node>(p_collider);
	if (!c) {
		return nullptr;
	}
	for (PhysXBuildingFire3D *b : buildings) {
		Node *t = b->_target();
		if (t && (t == c || t->is_ancestor_of(c))) {
			return b;
		}
	}
	return nullptr;
}

// A brand in an open opening lands on the room's contents: a glowing brand on dry furnishings lights them about one
// time in five (Manzello et al. 2006, on beds and upholstery). On a face it piles at the face's foot (walls) or where
// it lands (the roof), heating the cladding.
void PhysXBuildingFire3D::add_brand(const Vector3 &p_position, const Vector3 &p_normal, float p_glow) {
	if (p_glow <= 0.0f) {
		return;
	}
	if (rooms_dirty) {
		_build_rooms();
	}
	const Transform3D inv = get_global_transform().affine_inverse();
	const Vector3 l = inv.xform(p_position);
	const Vector3 nl = inv.basis.xform(p_normal).normalized();
	int best = -1;
	int best_face = FACE_TOP;
	float bd = 1e30f;
	for (uint32_t i = 0; i < rooms.size(); i++) {
		const Room &r = rooms[i];
		for (int f = 0; f < FACE_COUNT; f++) {
			if (!r.exterior[f]) {
				continue;
			}
			const Vector2 fs = _face_size(r, f);
			const Vector2 uv = _face_uv(r, f, l);
			const Vector3 p = _face_point(r, f, Vector2(CLAMP(uv.x, 0.0f, fs.x), CLAMP(uv.y, 0.0f, fs.y)));
			const float d = p.distance_to(l) - 0.2f * _face_normal(f).dot(nl);
			if (d < bd) {
				bd = d;
				best = i;
				best_face = f;
			}
		}
	}
	if (best < 0 || bd > 1.0f) {
		return;
	}
	Room &r = rooms[best];
	const Vector2 uv = _face_uv(r, best_face, l);
	for (const Opening &o : r.openings) {
		if (o.face == best_face && o.to_room < 0 && (!o.glazed || o.broken) && o.rect.grow(0.05f).has_point(uv)) {
			if (_rand() < 0.2f * p_glow) {
				_ignite_room(best, l - _face_normal(best_face) * 0.5f);
			}
			return;
		}
	}
	const Vector2 fs = _face_size(r, best_face);
	const Vector2 at = best_face == FACE_TOP ? Vector2(CLAMP(uv.x, 0.0f, fs.x), CLAMP(uv.y, 0.0f, fs.y)) : Vector2(CLAMP(uv.x, 0.0f, fs.x), 0.05f);
	if (r.brand_glow[best_face] <= 0.05f || r.brand_at[best_face].distance_to(at) > 0.5f) {
		if (p_glow >= r.brand_glow[best_face]) {
			r.brand_at[best_face] = at;
		}
	}
	r.brand_glow[best_face] += p_glow;
}

float PhysXBuildingFire3D::contract_fire_heat_at(const Vector3 &p_position) const {
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	float q = 0.0f;
	for (const Room &r : rooms) {
		if (l.x >= r.box.position.x && l.x <= r.box.position.x + r.box.size.x && l.z >= r.box.position.z && l.z <= r.box.position.z + r.box.size.z && l.y >= r.box.position.y - 0.5f && l.y <= r.box.position.y + r.box.size.y) {
			q = MAX(q, (r.q + r.q_ext) * 1000.0f / MAX(r.box.size.x * r.box.size.z, 1.0f));
		}
	}
	return q;
}

float PhysXBuildingFire3D::contract_fire_burnt_at(const Vector3 &p_position) const {
	const Vector3 l = get_global_transform().affine_inverse().xform(p_position);
	float b = 0.0f;
	for (const Room &r : rooms) {
		if (r.state != ROOM_COLD && l.x >= r.box.position.x && l.x <= r.box.position.x + r.box.size.x && l.z >= r.box.position.z && l.z <= r.box.position.z + r.box.size.z) {
			b = MAX(b, 1.0f - r.fuel_left / MAX(r.fuel_total, 1.0f));
		}
	}
	return b;
}

// Its convection column: 0.7 of its heat (the rest radiates) over its centre's top.
void PhysXBuildingFire3D::contract_fire_plumes(PackedVector4Array &r_plumes) const {
	const float q = get_heat_release();
	if (q <= 0.0f || rooms.is_empty()) {
		return;
	}
	AABB all = rooms[0].box;
	for (const Room &r : rooms) {
		all.merge_with(r.box);
	}
	const Vector3 c = get_global_transform().xform(all.get_center() + Vector3(0, all.size.y * 0.5f, 0));
	r_plumes.push_back(Vector4(c.x, c.y, c.z, 0.7f * q * 1000.0f));
}

void PhysXBuildingFire3D::contract_fire_smoke(PackedVector4Array &r_sources) const {
	const float s = get_smoke_rate();
	if (s <= 0.0f || rooms.is_empty()) {
		return;
	}
	AABB all = rooms[0].box;
	for (const Room &r : rooms) {
		all.merge_with(r.box);
	}
	const Vector3 c = get_global_transform().xform(all.get_center() + Vector3(0, all.size.y * 0.5f, 0));
	r_sources.push_back(Vector4(c.x, c.y, c.z, s));
}

float PhysXBuildingFire3D::get_heat_release() const {
	float q = 0.0f;
	for (const Room &r : rooms) {
		q += r.q + r.q_ext;
		for (int f = 0; f < FACE_COUNT; f++) {
			if (r.clad_r[f] > 0.0f) {
				q += CLAD_HRR * MIN(Math::PI * r.clad_r[f] * r.clad_r[f] * 0.5f, _face_size(r, f).x * _face_size(r, f).y);
			}
		}
	}
	return q;
}

// Particulate from the wood and contents it burns (a smoke yield of 0.03 kg/kg: wood about 0.015, furnishings more;
// Tewarson, SFPE Handbook), per real second.
float PhysXBuildingFire3D::get_smoke_rate() const {
	return smoke_yield * get_heat_release() / 1000.0f / FUEL_HEAT * time_scale;
}

bool PhysXBuildingFire3D::is_burning() const {
	for (const Room &r : rooms) {
		if (r.state == ROOM_GROWING || r.state == ROOM_FLASHED || r.state == ROOM_DECAY) {
			return true;
		}
		for (int f = 0; f < FACE_COUNT; f++) {
			if (r.clad_r[f] > 0.0f) {
				return true;
			}
		}
	}
	return false;
}

int PhysXBuildingFire3D::get_broken_windows() const {
	int n = 0;
	for (const Room &r : rooms) {
		for (const Opening &o : r.openings) {
			n += (o.glazed && o.broken) ? 1 : 0;
		}
	}
	return n;
}

PhysXBuildingFire3D::RoomState PhysXBuildingFire3D::get_room_state(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), ROOM_COLD);
	return rooms[p_room].state;
}

float PhysXBuildingFire3D::get_room_heat_release(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	return rooms[p_room].q + rooms[p_room].q_ext;
}

float PhysXBuildingFire3D::get_room_temperature(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	return rooms[p_room].temperature;
}

float PhysXBuildingFire3D::get_room_flashover_time(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), -1.0f);
	return rooms[p_room].flash_time;
}

float PhysXBuildingFire3D::get_room_char_depth(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	return rooms[p_room].char_depth;
}

float PhysXBuildingFire3D::get_room_fuel_left(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), 0.0f);
	return rooms[p_room].fuel_left / MAX(rooms[p_room].fuel_total, 1e-6f);
}

bool PhysXBuildingFire3D::is_room_collapsed(int p_room) const {
	ERR_FAIL_INDEX_V(p_room, (int)rooms.size(), false);
	return rooms[p_room].collapsed;
}

void PhysXBuildingFire3D::set_rooms(const Array &p_rooms) {
	room_data = p_rooms;
	rooms_dirty = true;
}

void PhysXBuildingFire3D::set_target(const NodePath &p_path) {
	target = p_path;
	destructible_id = ObjectID();
	look_done = false;
}

void PhysXBuildingFire3D::set_fire(const NodePath &p_path) {
	fire_path = p_path;
	fire = nullptr;
}

void PhysXBuildingFire3D::set_flow(const NodePath &p_path) {
	flow_path = p_path;
	flow = nullptr;
}

void PhysXBuildingFire3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!buildings.has(this)) {
				buildings.push_back(this);
			}
			WorldContracts::register_fire(this);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			buildings.erase(this);
			WorldContracts::unregister_fire(this);
			brands.clear();
			steam.clear();
			for (Room &r : rooms) {
				for (ObjectID *id : { &r.inner_emitter, &r.roof_emitter }) {
					if (Node *e = Object::cast_to<Node>(ObjectDB::get_instance(*id))) {
						e->queue_free();
					}
					*id = ObjectID();
				}
				for (ObjectID &id : r.clad_emitter) {
					if (Node *e = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
						e->queue_free();
					}
					id = ObjectID();
				}
				for (Opening &o : r.openings) {
					if (Node *e = Object::cast_to<Node>(ObjectDB::get_instance(o.emitter))) {
						e->queue_free();
					}
					o.emitter = ObjectID();
				}
			}
		} break;
		case NOTIFICATION_READY: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				set_physics_process_internal(true);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (rooms_dirty) {
				_build_rooms();
			}
			_resolve();
			const float real_dt = float(get_physics_process_delta_time());
			float left = real_dt * time_scale;
			sample_timer -= real_dt;
			if (sample_timer <= 0.0f) {
				sample_timer = 0.25f;
				_sample(0.25f * time_scale);
			}
			// Steps of at most a second of fire time.
			while (left > 0.0f) {
				const float dt = MIN(left, 1.0f);
				left -= dt;
				for (uint32_t i = 0; i < rooms.size(); i++) {
					_step_room(i, dt);
				}
				_smoke(dt);
				_spread(dt);
			}
			for (uint32_t i = 0; i < rooms.size(); i++) {
				_weaken(i);
			}
			_update_visuals(real_dt);
			_update_steam(real_dt);
			_update_brands(real_dt);
			_update_look();
		} break;
	}
}

void PhysXBuildingFire3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_rooms", "rooms"), &PhysXBuildingFire3D::set_rooms);
	ClassDB::bind_method(D_METHOD("get_rooms"), &PhysXBuildingFire3D::get_rooms);
	ClassDB::bind_method(D_METHOD("set_target", "path"), &PhysXBuildingFire3D::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &PhysXBuildingFire3D::get_target);
	ClassDB::bind_method(D_METHOD("set_fire", "path"), &PhysXBuildingFire3D::set_fire);
	ClassDB::bind_method(D_METHOD("get_fire"), &PhysXBuildingFire3D::get_fire);
	ClassDB::bind_method(D_METHOD("set_flow", "path"), &PhysXBuildingFire3D::set_flow);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXBuildingFire3D::get_flow);
	ClassDB::bind_method(D_METHOD("set_growth_time", "seconds"), &PhysXBuildingFire3D::set_growth_time);
	ClassDB::bind_method(D_METHOD("get_growth_time"), &PhysXBuildingFire3D::get_growth_time);
	ClassDB::bind_method(D_METHOD("set_heat_release_rate", "kw_per_m2"), &PhysXBuildingFire3D::set_heat_release_rate);
	ClassDB::bind_method(D_METHOD("get_heat_release_rate"), &PhysXBuildingFire3D::get_heat_release_rate);
	ClassDB::bind_method(D_METHOD("set_wall_thickness", "metres"), &PhysXBuildingFire3D::set_wall_thickness);
	ClassDB::bind_method(D_METHOD("get_wall_thickness"), &PhysXBuildingFire3D::get_wall_thickness);
	ClassDB::bind_method(D_METHOD("set_floor_thickness", "metres"), &PhysXBuildingFire3D::set_floor_thickness);
	ClassDB::bind_method(D_METHOD("get_floor_thickness"), &PhysXBuildingFire3D::get_floor_thickness);
	ClassDB::bind_method(D_METHOD("set_roof_thickness", "metres"), &PhysXBuildingFire3D::set_roof_thickness);
	ClassDB::bind_method(D_METHOD("get_roof_thickness"), &PhysXBuildingFire3D::get_roof_thickness);
	ClassDB::bind_method(D_METHOD("set_member_size", "metres"), &PhysXBuildingFire3D::set_member_size);
	ClassDB::bind_method(D_METHOD("get_member_size"), &PhysXBuildingFire3D::get_member_size);
	ClassDB::bind_method(D_METHOD("set_collapse_share", "share"), &PhysXBuildingFire3D::set_collapse_share);
	ClassDB::bind_method(D_METHOD("get_collapse_share"), &PhysXBuildingFire3D::get_collapse_share);
	ClassDB::bind_method(D_METHOD("set_glass_break_temperature", "celsius"), &PhysXBuildingFire3D::set_glass_break_temperature);
	ClassDB::bind_method(D_METHOD("get_glass_break_temperature"), &PhysXBuildingFire3D::get_glass_break_temperature);
	ClassDB::bind_method(D_METHOD("set_smoke_yield", "kg_per_kg"), &PhysXBuildingFire3D::set_smoke_yield);
	ClassDB::bind_method(D_METHOD("get_smoke_yield"), &PhysXBuildingFire3D::get_smoke_yield);
	ClassDB::bind_method(D_METHOD("set_light_energy", "energy"), &PhysXBuildingFire3D::set_light_energy);
	ClassDB::bind_method(D_METHOD("get_light_energy"), &PhysXBuildingFire3D::get_light_energy);
	ClassDB::bind_method(D_METHOD("set_firebrands", "enabled"), &PhysXBuildingFire3D::set_firebrands);
	ClassDB::bind_method(D_METHOD("has_firebrands"), &PhysXBuildingFire3D::has_firebrands);
	ClassDB::bind_method(D_METHOD("set_firebrand_rate", "per_second_per_mw"), &PhysXBuildingFire3D::set_firebrand_rate);
	ClassDB::bind_method(D_METHOD("get_firebrand_rate"), &PhysXBuildingFire3D::get_firebrand_rate);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &PhysXBuildingFire3D::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &PhysXBuildingFire3D::get_time_scale);

	ClassDB::bind_method(D_METHOD("ignite", "position"), &PhysXBuildingFire3D::ignite);
	ClassDB::bind_method(D_METHOD("apply_water", "position", "kg"), &PhysXBuildingFire3D::apply_water);
	ClassDB::bind_method(D_METHOD("get_room_water_cooling", "room"), &PhysXBuildingFire3D::get_room_water_cooling);
	ClassDB::bind_method(D_METHOD("get_room_layer_height", "room"), &PhysXBuildingFire3D::get_room_layer_height);
	ClassDB::bind_method(D_METHOD("get_room_visibility", "room"), &PhysXBuildingFire3D::get_room_visibility);
	ClassDB::bind_method(D_METHOD("get_visibility_at", "position"), &PhysXBuildingFire3D::get_visibility_at);
	ClassDB::bind_method(D_METHOD("extinguish"), &PhysXBuildingFire3D::extinguish);
	ClassDB::bind_method(D_METHOD("add_brand", "position", "normal", "glow"), &PhysXBuildingFire3D::add_brand);
	ClassDB::bind_method(D_METHOD("get_room_count"), &PhysXBuildingFire3D::get_room_count);
	ClassDB::bind_method(D_METHOD("get_room_state", "room"), &PhysXBuildingFire3D::get_room_state);
	ClassDB::bind_method(D_METHOD("get_room_heat_release", "room"), &PhysXBuildingFire3D::get_room_heat_release);
	ClassDB::bind_method(D_METHOD("get_room_temperature", "room"), &PhysXBuildingFire3D::get_room_temperature);
	ClassDB::bind_method(D_METHOD("get_room_flashover_time", "room"), &PhysXBuildingFire3D::get_room_flashover_time);
	ClassDB::bind_method(D_METHOD("get_room_char_depth", "room"), &PhysXBuildingFire3D::get_room_char_depth);
	ClassDB::bind_method(D_METHOD("get_room_fuel_left", "room"), &PhysXBuildingFire3D::get_room_fuel_left);
	ClassDB::bind_method(D_METHOD("is_room_collapsed", "room"), &PhysXBuildingFire3D::is_room_collapsed);
	ClassDB::bind_method(D_METHOD("get_broken_windows"), &PhysXBuildingFire3D::get_broken_windows);
	ClassDB::bind_method(D_METHOD("get_heat_release"), &PhysXBuildingFire3D::get_heat_release);
	ClassDB::bind_method(D_METHOD("get_smoke_rate"), &PhysXBuildingFire3D::get_smoke_rate);
	ClassDB::bind_method(D_METHOD("get_firebrand_count"), &PhysXBuildingFire3D::get_firebrand_count);
	ClassDB::bind_method(D_METHOD("get_spot_fire_count"), &PhysXBuildingFire3D::get_spot_fire_count);
	ClassDB::bind_method(D_METHOD("is_burning"), &PhysXBuildingFire3D::is_burning);
	ClassDB::bind_static_method("PhysXBuildingFire3D", D_METHOD("radiant_flux_at", "position"), &PhysXBuildingFire3D::_radiant_flux_bind);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "rooms"), "set_rooms", "get_rooms");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "fire", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFireSpread3D"), "set_fire", "get_fire");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "flow", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_flow", "get_flow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "growth_time", PROPERTY_HINT_RANGE, "10,2000,1,suffix:s"), "set_growth_time", "get_growth_time");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heat_release_rate", PROPERTY_HINT_RANGE, "10,1000,1,suffix:kW/m²"), "set_heat_release_rate", "get_heat_release_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wall_thickness", PROPERTY_HINT_RANGE, "0.005,0.5,0.005,suffix:m"), "set_wall_thickness", "get_wall_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_thickness", PROPERTY_HINT_RANGE, "0.005,0.5,0.005,suffix:m"), "set_floor_thickness", "get_floor_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roof_thickness", PROPERTY_HINT_RANGE, "0.005,0.5,0.005,suffix:m"), "set_roof_thickness", "get_roof_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "member_size", PROPERTY_HINT_RANGE, "0.01,0.5,0.005,suffix:m"), "set_member_size", "get_member_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collapse_share", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_collapse_share", "get_collapse_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glass_break_temperature", PROPERTY_HINT_RANGE, "50,1000,1,suffix:°C"), "set_glass_break_temperature", "get_glass_break_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoke_yield", PROPERTY_HINT_RANGE, "0,0.2,0.001"), "set_smoke_yield", "get_smoke_yield");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "light_energy", PROPERTY_HINT_RANGE, "0,32,0.1"), "set_light_energy", "get_light_energy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "firebrands"), "set_firebrands", "has_firebrands");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "firebrand_rate", PROPERTY_HINT_RANGE, "0,500,0.1"), "set_firebrand_rate", "get_firebrand_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0,3600,0.1"), "set_time_scale", "get_time_scale");

	BIND_ENUM_CONSTANT(ROOM_COLD);
	BIND_ENUM_CONSTANT(ROOM_GROWING);
	BIND_ENUM_CONSTANT(ROOM_FLASHED);
	BIND_ENUM_CONSTANT(ROOM_DECAY);
	BIND_ENUM_CONSTANT(ROOM_OUT);
	BIND_ENUM_CONSTANT(FACE_NEG_X);
	BIND_ENUM_CONSTANT(FACE_POS_X);
	BIND_ENUM_CONSTANT(FACE_NEG_Z);
	BIND_ENUM_CONSTANT(FACE_POS_Z);
	BIND_ENUM_CONSTANT(FACE_TOP);
}
