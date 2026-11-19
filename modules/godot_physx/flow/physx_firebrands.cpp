/**************************************************************************/
/*  physx_firebrands.cpp                                                  */
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

#include "physx_firebrands.h"

#include "physx_building_fire_3d.h"
#include "physx_burnable_3d.h"
#include "physx_fire_spread_3d.h"
#include "physx_flow_3d.h"

#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/physics_3d/physics_server_3d.h"

#include "modules/world/world_contracts.h"

float PhysXFirebrands::_rand() {
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return (seed & 0xffffff) / float(0x1000000);
}

// Schroeder (1969), "Ignition probability", as BehavePlus and Rothermel (1983) use it: the heat to bring fine dead fuel
// to ignition, Q_ig (cal/g), from its moisture M and temperature T (C), and P_ig from how far Q_ig is under 400.
float PhysXFirebrands::ignition_probability(float p_moisture, float p_air_temperature) {
	const float m = CLAMP(p_moisture, 0.0f, 1.0f);
	const float t = p_air_temperature;
	const float q_ig = 144.512f - 0.266f * t - 0.00058f * t * t - t * m + 18.54f * (1.0f - Math::exp(-15.1f * m)) + 640.0f * m;
	const float x = MAX((400.0f - q_ig) / 10.0f, 0.0f);
	return CLAMP(0.000048f * Math::pow(x, 4.3f) / 50.0f, 0.0f, 1.0f);
}

// The rise speed of a line fire's plume (Byram 1959; Lee and Emmons 1961: a line plume's centreline speed is about
// 1.7 (g I / (rho c_p T))^(1/3) and holds with height while the source looks like a line).
float PhysXFirebrands::plume_speed(float p_intensity, float p_air_temperature) {
	const float t = MAX(p_air_temperature + 273.15f, 200.0f);
	return 1.7f * Math::pow(MAX(9.8f * p_intensity * 1000.0f / (1.2f * 1005.0f * t), 0.0f), 1.0f / 3.0f);
}

void PhysXFirebrands::set_plumes(const LocalVector<Plume> &p_plumes, float p_length) {
	plumes = p_plumes;
	plume_length = MAX(p_length, 1.0f);
}

// Updraft at a point from the columns: each leans downwind (its axis moves on by U/w for each metre it rises) and
// widens as it entrains air (top-hat half width about 0.2 z, Morton, Taylor and Turner 1956). Above about the front's
// length the source looks like a point and the column slows, as z^(-1/3) in still air and z^(-1/2) bent over by the
// wind (Briggs 1975).
float PhysXFirebrands::_plume_lift(const Vector3 &p_pos, const Vector3 &p_wind) const {
	float lift = 0.0f;
	const Vector3 uh(p_wind.x, 0.0f, p_wind.z);
	const float u = uh.length();
	for (const Plume &pl : plumes) {
		const float z = p_pos.y - pl.base.y;
		if (z < 0.0f || pl.w0 <= 0.0f) {
			continue;
		}
		const float e = 1.0f / 3.0f + (1.0f / 6.0f) * CLAMP(u / pl.w0, 0.0f, 1.0f);
		const float w = pl.w0 * Math::pow(MIN(plume_length / MAX(z, 0.01f), 1.0f), e);
		const Vector3 axis = pl.base + uh * (z / MAX(w, 0.1f));
		const float b = pl.width + 0.2f * z; // the flaming depth near the ground
		const float dx = p_pos.x - axis.x, dz = p_pos.z - axis.z;
		lift = MAX(lift, w * Math::exp(-(dx * dx + dz * dz) / (b * b)));
	}
	return lift;
}

void PhysXFirebrands::release(float p_count, const LocalVector<Release> &p_points, PhysXFlow3D *p_flow) {
	if (p_points.is_empty()) {
		return;
	}
	carry += p_count;
	float total = 0.0f;
	for (const Release &r : p_points) {
		total += r.weight;
	}
	if (total <= 0.0f) {
		return;
	}
	// Near what can fly at once each new one stands for more (a crown fire throws thousands), back to one as they land.
	const float fill = float(brands.size()) / MAX_BRANDS;
	if (fill > 0.8f) {
		stand_for = MIN(stand_for * 1.03f, 1000.0f);
	} else if (fill < 0.5f) {
		stand_for = MAX(stand_for * 0.97f, 1.0f);
	}
	const float per = Math::floor(stand_for);
	while (carry >= per && (int)brands.size() < MAX_BRANDS) {
		carry -= per;
		float pick = _rand() * total;
		uint32_t k = 0;
		while (k + 1 < p_points.size() && pick > p_points[k].weight) {
			pick -= p_points[k].weight;
			k++;
		}
		Brand b;
		if (p_points[k].twig > 0.0f) {
			// A burning twig breaking off in the crown: it falls from rest through the needles at its own terminal
			// velocity, too heavy for the column to hold (cylinders of 1 to 3 cm: 6 to 12 m/s, Tohidi et al. 2015),
			// and glows on the ground for a minute or two.
			const float d = p_points[k].twig * (0.6f + 0.8f * _rand());
			b.p = p_points[k].position + Vector3((_rand() - 0.5f) * 0.6f, (_rand() - 0.5f) * 0.6f, (_rand() - 0.5f) * 0.6f);
			b.v = Vector3((_rand() - 0.5f) * 0.6f, -0.5f * _rand(), (_rand() - 0.5f) * 0.6f);
			b.vt = MIN(3.0f + 350.0f * d, 12.0f);
			b.life = MIN(30.0f + 3000.0f * d * (0.7f + 0.6f * _rand()), 120.0f);
			b.n = per;
			brands.push_back(b);
			continue;
		}
		b.p = p_points[k].position + Vector3((_rand() - 0.5f) * 0.3f, _rand() * 0.3f, (_rand() - 0.5f) * 0.3f);
		// Thrown up into the plume; the gas carries it from there.
		b.v = Vector3((_rand() - 0.5f) * 1.0f, 1.5f + _rand() * 2.5f, (_rand() - 0.5f) * 1.0f);
		if (p_flow) {
			b.v += p_flow->sample_velocity(b.p);
		}
		// Bark and twig flakes of 2 to 10 mm: falling at 2 to 6 m/s (Tohidi et al. 2015), glowing for about 10 to 50 s
		// (Tarifa et al. 1965: burnout time grows with size).
		const float d = 0.002f + 0.008f * _rand() * _rand();
		b.vt = 2.0f + 400.0f * d + _rand();
		b.life = 6.0f + 4500.0f * d * (0.7f + 0.6f * _rand());
		b.n = per;
		brands.push_back(b);
	}
	carry = MIN(carry, MAX(4.0f, 4.0f * per));
}

int PhysXFirebrands::step(Node3D *p_owner, PhysXFlow3D *p_flow, const Vector3 &p_wind, float p_delta, float p_glow_energy) {
	int started = 0;
	if (brands.is_empty()) {
		if (mmi) {
			mmi->set_visible(false);
		}
		return 0;
	}
	if (p_flow) {
		p_flow->request_velocity();
	}
	PhysicsDirectSpaceState3D *space = nullptr;
	if (p_owner && p_owner->is_inside_tree() && p_owner->get_world_3d().is_valid()) {
		space = p_owner->get_world_3d()->get_direct_space_state();
	}
	const Vector3 g(0.0f, -9.8f, 0.0f);
	const float z0 = 0.03f; // roughness length of short vegetation
	const float ref = Math::log(1.0f / z0); // the given wind is at about a metre (midflame)
	for (uint32_t i = 0; i < brands.size();) {
		Brand &b = brands[i];
		PhysXFireSpread3D *under = PhysXFireSpread3D::find_at(b.p);
		const float ground = under ? under->get_ground_height_at(b.p) : -1.0e9f;
		const float z = under ? MAX(b.p.y - ground, 0.1f) : 10.0f;
		// The air: the wind at the brand's height plus the plume's own motion.
		Vector3 air;
		if (const WorldWeather *wm = WorldContracts::weather(); wm->has_provider()) {
			air = wm->wind_at(b.p, z);
		} else
		{
			air = p_wind * (Math::log(MAX(z, z0 * 1.5f) / z0) / ref);
		}
		// Ash and embers near the ground drift in with the fires' indraft (WorldFire.wind_at), fading over the first
		// few metres where the plume below takes them.
		if (const WorldFire *fire = WorldContracts::fire(); fire->has_provider()) {
			air += fire->wind_at(b.p) * (1.0f - Math::smoothstep(2.0f, 6.0f, z));
		}
		// The plume: Flow's gas in the flames, the convection column above them (Flow's own grid holds only the
		// flame zone, and the fire's Flow already carries the wind, so only its updraft is taken).
		float lift = _plume_lift(b.p, air);
		if (p_flow) {
			const Vector3 fv = p_flow->sample_velocity(b.p);
			lift = MAX(lift, fv.y);
		}
		air.y += lift;
		// Quadratic drag that settles at the terminal velocity, which falls as the brand burns away (mass loss).
		const float left = MAX(1.0f - b.age / b.life, 0.0f);
		const float vt = MAX(b.vt * Math::sqrt(MAX(left, 0.15f)), 0.5f);
		const Vector3 rel = air - b.v;
		const Vector3 acc = g + rel * (rel.length() * 9.8f / (vt * vt));
		const Vector3 from = b.p;
		b.v += acc * p_delta;
		b.p += b.v * p_delta;
		b.age += p_delta;

		bool gone = b.age >= b.life || b.p.y < from.y - 500.0f;
		bool hit = false;
		Vector3 at;
		if (!gone && space && (b.v.y < 0.0f || b.p.y - from.y < 0.5f * (b.p - from).length())) { // falling, or flying flat into a wall
			PhysicsDirectSpaceState3D::RayParameters rp;
			rp.from = from;
			rp.to = b.p;
			PhysicsDirectSpaceState3D::RayResult rr;
			if (space->intersect_ray(rp, rr)) {
				hit = true;
				at = rr.position;
				// Onto something that burns: the brand rests there and heats it (a woodpile, a fence, a deck).
				if (PhysXBurnable3D *pb = PhysXBurnable3D::find_by_collider(rr.collider)) {
					pb->add_brand(rr.position, rr.normal, b.n * Math::sqrt(MAX(1.0f - b.age / b.life, 0.0f)));
					landed++;
					brands[i] = brands[brands.size() - 1];
					brands.resize(brands.size() - 1);
					continue;
				}
				// Onto a building: its walls and roof heat, or the brand drops through a window onto what is inside.
				if (PhysXBuildingFire3D *pbf = PhysXBuildingFire3D::find_by_collider(rr.collider)) {
					pbf->add_brand(rr.position, rr.normal, b.n * Math::sqrt(MAX(1.0f - b.age / b.life, 0.0f)));
					landed++;
					brands[i] = brands[brands.size() - 1];
					brands.resize(brands.size() - 1);
					continue;
				}
			}
		}
		if (!gone && !hit && under && b.p.y <= ground) {
			hit = true;
			at = Vector3(b.p.x, ground, b.p.z);
		}
		if (hit) {
			gone = true;
			landed++;
			// A spot fire, if it lands on unburnt fine fuel dry enough to catch (Manzello et al. 2006, 2008: glowing
			// brands light dry grass and litter, damp beds not at all).
			PhysXFireSpread3D *target = PhysXFireSpread3D::find_at(at);
			if (target && target->get_char_at(at) < 0.05f && target->get_intensity_at(at) <= 0.0f) {
				const float m = target->get_moisture_at(at);
				if (m < target->get_moisture_of_extinction()) {
					const float p1 = ignition_probability(m, target->_air_temperature()) * Math::sqrt(left) * 0.7f;
					const float p = 1.0f - Math::pow(1.0f - p1, b.n); // any of the brands it stands for
					if (_rand() < p) {
						target->ignite(at, 0.2f);
						started++;
						spot_fires++;
					}
				}
			}
		}
		if (gone) {
			brands[i] = brands[brands.size() - 1];
			brands.resize(brands.size() - 1);
		} else {
			i++;
		}
	}
	_draw(p_owner, p_glow_energy);
	return started;
}

// Short glowing streaks along each brand's flight (a camera's exposure smears a fast ember into a line), from about
// 900 C yellow-orange dimming to dull red as it burns out.
void PhysXFirebrands::_draw(Node3D *p_owner, float p_glow_energy) {
	if (!p_owner) {
		return;
	}
	if (!mmi) {
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_colors(true);
		Ref<BoxMesh> box;
		box.instantiate();
		box->set_size(Vector3(1.0f, 1.0f, 1.0f));
		// A brand is millimetres across, far under a pixel beyond a few metres: each streak is widened to about a
		// pixel and dimmed by as much, so it keeps the light it would cover and neither vanishes nor aliases.
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(R"(shader_type spatial;
render_mode unshaded, fog_disabled, shadows_disabled;
varying float fb_cover;
void vertex() {
	vec3 centre = MODEL_MATRIX[3].xyz;
	float depth = max(-(VIEW_MATRIX * vec4(centre, 1.0)).z, 0.01);
	float px = 2.0 * depth / max(abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y, 1e-6);
	float w = max(length(MODEL_MATRIX[0].xyz), 1e-6);
	float l = max(length(MODEL_MATRIX[2].xyz), 1e-6);
	float kw = max(1.2 * px / w, 1.0);
	float kl = max(1.2 * px / l, 1.0);
	VERTEX *= vec3(kw, kw, kl);
	fb_cover = 1.0 / (kw * kl);
}
void fragment() {
	ALBEDO = COLOR.rgb * fb_cover;
}
)");
		Ref<ShaderMaterial> mat;
		mat.instantiate();
		mat->set_shader(sh);
		box->surface_set_material(0, mat);
		mm->set_mesh(box);
		mmi = memnew(MultiMeshInstance3D);
		mmi->set_name("Firebrands");
		mmi->set_multimesh(mm);
		mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		p_owner->add_child(mmi, false, Node::INTERNAL_MODE_FRONT);
		mmi->set_as_top_level(true);
		mmi->set_global_transform(Transform3D());
	}
	mmi->set_visible(true);
	mm->set_instance_count(brands.size());
	for (uint32_t i = 0; i < brands.size(); i++) {
		const Brand &b = brands[i];
		const float left = MAX(1.0f - b.age / b.life, 0.0f);
		const float glow = left * left;
		const float speed = b.v.length();
		const float len = CLAMP(speed / 60.0f, 0.012f, 0.12f);
		const float w = 0.004f + 0.004f * left;
		Basis basis;
		if (speed > 1.0e-3f) {
			const Vector3 fwd = b.v / speed;
			const Vector3 up = Math::abs(fwd.y) < 0.99f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
			const Vector3 side = up.cross(fwd).normalized();
			basis = Basis(side * w, fwd.cross(side) * w, fwd * len);
		} else {
			basis = Basis().scaled(Vector3(w, w, w));
		}
		mm->set_instance_transform(i, Transform3D(basis, b.p));
		const float e = p_glow_energy * (0.25f + glow);
		mm->set_instance_color(i, Color(e, e * (0.12f + 0.28f * glow), e * 0.05f * glow, 1.0f));
	}
}

void PhysXFirebrands::clear() {
	brands.clear();
	carry = 0.0f;
	stand_for = 1.0f;
	if (mmi) {
		mmi->set_visible(false);
	}
}
