/**************************************************************************/
/*  weather_tracks.cpp                                                    */
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

// Wet tracks (docs/ideas/world-links.md, "Wet tracks out of water and mud"). A body walking or driving out of water,
// wet mud or a puddle carries a film on its feet or tread; each print presses a share of it onto the ground, so the
// trail fades over some ten to twenty prints (the depleting-load trail of wet footprint decals in games, e.g. Red
// Dead Redemption 2's), and water running off its body drips round it, most in the first half minute. Prints and
// drips go into the contact wetness square ("wet where water has been"), which dries them by the weather's drying
// time (the air's vapour deficit, the wind, the sun: Penman), so they last minutes in a humid shade and fade fast in
// sun and wind. Bodies are the world_contact group, read through WorldContracts::describe_body.
// Out of mud the feet or tread also carry clay, a film of about a millimetre (a tread a little more), pressed onto
// firm ground print by print through WorldGround.track_mud (the ground's tracked mud), shedding about a seventh a
// footfall as GroundLegMud3D's boots do. Vehicles and characters that print their own mud (PhysXVehicle3D,
// GroundLegMud3D) carry only water here.

#include "weather_map_3d.h"

#include "core/object/class_db.h"
#include "modules/world/world_contracts.h"
#include "scene/main/scene_tree.h"

static constexpr float TRACK_PRINT_SHARE = 0.15f; // of the load pressed off by each footprint
static constexpr float TRACK_TREAD_SHARE = 0.03f; // by each tread stamp (every 15 cm or so)
static constexpr float TRACK_DRIP_TIME = 40.0f; // s, e-folding of the water running off a wet body
static constexpr float TRACK_LOAD_TIME = 180.0f; // s, the feet's film drying on them as they go
static constexpr float TRACK_DRIP_EVERY = 0.12f; // s between drops
static constexpr float TRACK_MUD_SOLE = 0.001f; // m of clay on a sole out of mud
static constexpr float TRACK_MUD_TREAD = 0.002f; // m in a tread
static constexpr float TRACK_MUD_MIN = 2e-5f; // m: thinner prints nothing

void WeatherMap3D::_tracks_step(float p_real_dt) {
	if (!wet_tracks || !contact_wetness_enabled || p_real_dt <= 0.0f || !is_inside_tree()) {
		return;
	}
	const Rect2 square = contact.rect();
	const WorldWater *ww = WorldContracts::water();
	WorldGround *wg = WorldContracts::ground();
	const bool water = ww->has_provider();
	for (KeyValue<ObjectID, TrackBody> &kv : track_bodies) {
		kv.value.seen = false;
	}
	auto rnd = [this]() {
		track_seed ^= track_seed << 13;
		track_seed ^= track_seed >> 17;
		track_seed ^= track_seed << 5;
		return (track_seed & 0xffffff) / float(0x1000000);
	};
	for (Node *n : get_tree()->get_nodes_in_group(SNAME(WorldContracts::CONTACT_GROUP))) {
		const Node3D *n3 = Object::cast_to<Node3D>(n);
		if (!n3 || !n3->is_visible_in_tree()) {
			continue;
		}
		const ObjectID id = n3->get_instance_id();
		const WorldContactBody b = WorldContracts::describe_body(n3);
		const Vector2 xz(b.center.x, b.center.z);
		TrackBody *t = track_bodies.getptr(id);
		if (!t) {
			t = &track_bodies.insert(id, TrackBody())->value;
			t->last_print = b.center;
		}
		if (t->seen) {
			continue;
		}
		t->seen = true;
		const float bottom = b.center.y - b.half_height;
		// In water: soaked through, the clay washed off, nothing printed (the water covers it).
		if (water && ww->depth_at(Vector3(b.center.x, bottom + 0.05f, b.center.z)) > 0.0f) {
			t->load = 1.0f;
			t->drip = 1.0f;
			t->mud = 0.0f;
			t->last_print = b.center;
			continue;
		}
		bool own_mud = n3->is_class("PhysXVehicle3D") || n3->is_class("PhysXVehicleWheel3D");
		for (int c = 0; c < n3->get_child_count() && !own_mud; c++) {
			own_mud = n3->get_child(c)->is_class("GroundLegMud3D");
		}
		// On the ground (or near it, without one).
		float ground = (wg->has_provider() || wg->get_script_instance()) ? wg->height_at(Vector3(b.center.x, b.center.y + 1.0f, b.center.z)) : -Math::INF;
		const bool grounded = !Math::is_finite(ground) || bottom - ground < 0.3f;
		if (grounded && Math::is_finite(ground)) {
			// Wet mud, standing water and puddles wet the feet as they tread them.
			const Vector3 at(b.center.x, ground, b.center.z);
			float wet = 0.0f;
			if (wg->mud_depth_at(at) > 0.01f) {
				wet = 0.8f;
				if (!own_mud) {
					t->mud = MAX(t->mud, b.wheel ? TRACK_MUD_TREAD : TRACK_MUD_SOLE);
				}
			} else if (wg->standing_water_at(at) > 0.003f || get_puddle_at(at) > 0.5f) {
				wet = 0.6f;
			}
			t->load = MAX(t->load, wet);
		}
		t->load *= Math::exp(-p_real_dt / TRACK_LOAD_TIME);
		t->drip *= Math::exp(-p_real_dt / TRACK_DRIP_TIME);
		if (!square.has_point(xz)) {
			t->last_print = b.center;
			continue;
		}
		// Prints, a stride apart (feet alternating either side of the way), or tread every 15 cm.
		const Vector2 step = xz - Vector2(t->last_print.x, t->last_print.z);
		const float stride = b.wheel ? 0.15f : CLAMP(2.2f * b.radius, 0.3f, 0.9f);
		if (step.length() > 10.0f) {
			t->last_print = b.center; // moved by a teleport
		} else if (grounded && (t->load > 0.03f || t->mud > TRACK_MUD_MIN) && step.length() >= stride) {
			const Vector2 dir = step.normalized();
			const float yaw = Math::atan2(-dir.y, dir.x);
			const float gy = Math::is_finite(ground) ? ground : bottom;
			// Firm ground under the print: mud only leaves the clay it carries where it is not mud itself.
			const bool firm = !Math::is_finite(ground) || wg->mud_depth_at(Vector3(b.center.x, ground, b.center.z)) <= 0.0f;
			if (b.wheel) {
				const float w = b.width > 0.0f ? b.width : 2.0f * b.radius;
				// A line of stamps along the way, as wide as the tread.
				const int k = MAX(int(Math::ceil(step.length() / 0.1f)), 1);
				for (int i = 1; i <= k; i++) {
					const Vector2 p = Vector2(t->last_print.x, t->last_print.z) + step * (float(i) / k);
					if (t->load > 0.03f) {
						contact.add_disc(Vector3(p.x, 0.0f, p.y), 0.5f * w, t->load);
					}
					if (firm && t->mud > TRACK_MUD_MIN) {
						wg->track_mud(Vector3(p.x, gy, p.y), 0.5f * w, t->mud, 1.0f, Vector4(1.0f, yaw, 0.0f, 0.0f));
						t->mud *= 1.0f - TRACK_TREAD_SHARE * (step.length() / k) / 0.15f;
					}
				}
				t->load *= Math::pow(1.0f - TRACK_TREAD_SHARE, step.length() / 0.15f);
			} else {
				const Vector2 side(-dir.y, dir.x);
				const Vector2 p = xz + side * (t->left ? 0.4f : -0.4f) * b.radius;
				const float foot = CLAMP(0.4f * b.radius, 0.06f, 0.25f);
				if (t->load > 0.03f) {
					contact.add_disc(Vector3(p.x, 0.0f, p.y), foot, t->load);
					// A foot is longer than wide: a second disc ahead of the heel.
					contact.add_disc(Vector3(p.x + dir.x * foot, 0.0f, p.y + dir.y * foot), foot * 0.9f, t->load * 0.9f);
				}
				if (firm && t->mud > TRACK_MUD_MIN) {
					// The sole's heel and forefoot (GroundLegMud3D's print), scaled with the body.
					const float sc = CLAMP(b.radius / 0.3f, 0.5f, 2.0f);
					wg->track_mud(Vector3(p.x + dir.x * 0.5f * foot, gy, p.y + dir.y * 0.5f * foot), 0.16f * sc, t->mud, 1.0f, Vector4(2.0f, yaw, 0.15f * sc, 0.055f * sc));
					t->mud *= 1.0f - TRACK_PRINT_SHARE;
				}
				t->left = !t->left;
				t->load *= 1.0f - TRACK_PRINT_SHARE;
			}
			t->last_print = b.center;
		}
		// Drips round the body.
		if (t->drip > 0.05f) {
			t->drip_timer += p_real_dt;
			while (t->drip_timer >= TRACK_DRIP_EVERY) {
				t->drip_timer -= TRACK_DRIP_EVERY;
				const float a = rnd() * float(Math::TAU), r = Math::sqrt(rnd()) * b.radius;
				contact.add_disc(Vector3(xz.x + Math::cos(a) * r, 0.0f, xz.y + Math::sin(a) * r), 0.07f, 0.7f * t->drip);
			}
		}
	}
	// Forget bodies that left.
	LocalVector<ObjectID> gone;
	for (const KeyValue<ObjectID, TrackBody> &kv : track_bodies) {
		if (!kv.value.seen) {
			gone.push_back(kv.key);
		}
	}
	for (const ObjectID &id : gone) {
		track_bodies.erase(id);
	}
}

void WeatherMap3D::set_wet_tracks(bool p_enabled) {
	wet_tracks = p_enabled;
	if (!p_enabled) {
		track_bodies.clear();
	}
}

Vector2 WeatherMap3D::get_body_water(Node *p_body) const {
	if (!p_body) {
		return Vector2();
	}
	const TrackBody *t = track_bodies.getptr(p_body->get_instance_id());
	return t ? Vector2(t->load, t->drip) : Vector2();
}

float WeatherMap3D::get_body_mud(Node *p_body) const {
	if (!p_body) {
		return 0.0f;
	}
	const TrackBody *t = track_bodies.getptr(p_body->get_instance_id());
	return t ? t->mud : 0.0f;
}

void WeatherMap3D::_bind_tracks_methods() {
	ClassDB::bind_method(D_METHOD("set_wet_tracks", "enabled"), &WeatherMap3D::set_wet_tracks);
	ClassDB::bind_method(D_METHOD("is_wet_tracks_enabled"), &WeatherMap3D::is_wet_tracks_enabled);
	ClassDB::bind_method(D_METHOD("get_body_water", "body"), &WeatherMap3D::get_body_water);
	ClassDB::bind_method(D_METHOD("get_body_mud", "body"), &WeatherMap3D::get_body_mud);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wet_tracks"), "set_wet_tracks", "is_wet_tracks_enabled");
}
