/**************************************************************************/
/*  physx_burnable_3d.cpp                                                 */
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

#include "physx_burnable_3d.h"

#include "physx_fire_spread_3d.h"
#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/material.h"
#include "scene/resources/multimesh.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

LocalVector<PhysXBurnable3D *> PhysXBurnable3D::burnables;
HashMap<RID, Ref<Material>> PhysXBurnable3D::material_cache;

void PhysXBurnable3D::free_materials() {
	material_cache.clear();
}

static constexpr float WOOD_KRHOC = 1.2e5f; // k rho c of wood, (W/m2K)^2 s
static constexpr float WOOD_HEAT = 15.0e6f; // J/kg, heat of combustion
static constexpr float CHI_R = 0.3f; // radiant share of a flame's heat release

PhysXBurnable3D::PhysXBurnable3D() {
	rng = uint32_t(uint64_t(this) >> 4) | 1u;
}

PhysXBurnable3D::~PhysXBurnable3D() {
	burnables.erase(this);
}

float PhysXBurnable3D::_rand() {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return float(rng & 0xffffffu) / float(0xffffff);
}

Node3D *PhysXBurnable3D::_target() const {
	return target.is_empty() ? const_cast<PhysXBurnable3D *>(this) : Object::cast_to<Node3D>(get_node_or_null(target));
}

Transform3D PhysXBurnable3D::_target_xform() const {
	const Node3D *t = _target();
	return t ? t->get_global_transform() : get_global_transform();
}

// The meshes under the target (its own MeshInstance3D and its children's), their bounds in target space, and their
// materials swapped for the burning look once.
void PhysXBurnable3D::_gather() {
	if (bounds_ready) {
		return;
	}
	Node3D *t = _target();
	if (!t) {
		return;
	}
	meshes.clear();
	bounds = AABB();
	bool first = true;
	const Transform3D inv = t->get_global_transform().affine_inverse();
	LocalVector<Node *> stack;
	stack.push_back(t);
	while (!stack.is_empty()) {
		Node *n = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		if (MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n)) {
			if (mi->get_mesh().is_valid()) {
				const Transform3D rel = inv * mi->get_global_transform();
				const AABB box = rel.xform(mi->get_mesh()->get_aabb());
				bounds = first ? box : bounds.merge(box);
				first = false;
				MeshRef r;
				r.id = mi->get_instance_id();
				r.to_mesh = rel.affine_inverse();
				meshes.push_back(r);
			}
		}
		for (int i = 0; i < n->get_child_count(); i++) {
			if (Object::cast_to<PhysXBurnable3D>(n->get_child(i))) {
				continue;
			}
			stack.push_back(n->get_child(i));
		}
	}
	if (first) {
		bounds = AABB(Vector3(-0.25f, -0.25f, -0.25f), Vector3(0.5f, 0.5f, 0.5f));
	}
	if (fuel_initial <= 0.0f) {
		fuel_initial = fuel_mass > 0.0f ? fuel_mass : bounds.get_volume() * 500.0f / 3.0f;
		fuel_left = fuel_initial;
	}
	bounds_ready = true;
}

void PhysXBurnable3D::_resolve() {
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
}

float PhysXBurnable3D::_surface_area() const {
	const Vector3 s = bounds.size;
	return MAX(2.0f * (s.x * s.y + s.y * s.z + s.x * s.z), 0.01f);
}

// Heat reaching the surface (kW/m2): the ground fire's flames at its base, Flow's hot gas around it, and the
// radiation of other burning burnables (a point source, chi_r HRR / 4 pi d^2). Ignition when the dose reaches a thick
// solid's (pi/4) k rho c (T_ig - T0)^2 (Quintiere; Babrauskas 2001 for wood), at the hottest place.
void PhysXBurnable3D::_sample_heat(float p_delta) {
	const Transform3D xf = _target_xform();
	const Vector3 c = xf.xform(bounds.get_center());
	const Vector3 base = xf.xform(bounds.get_center() - Vector3(0, bounds.size.y * 0.5f, 0));
	float q = 0.0f;
	Vector3 hot = base;
	if (fire) {
		// Flame contact: a grass fire's flames bathe what stands in them at up to about 60 kW/m2 at their peak
		// (some 600 kW/m2 released by the bed), less as the bed burns down.
		const float flaming = fire->get_intensity_at(base); // kW/m2 released by the ground there
		if (flaming > 0.0f) {
			const float reach = fire->get_flame_length_at(base);
			const float above = base.y - fire->get_ground_height_at(base);
			if (above < MAX(reach, 0.2f)) {
				q = MAX(q, MIN(60.0f * Math::sqrt(flaming / 600.0f), 60.0f));
			}
		}
	}
	if (flow) {
		// Hot gas: Flow's temperature (1 at the flame's core) as a convective and radiant flux up to about 80 kW/m2.
		const Vector3 pts[3] = { base, c, xf.xform(bounds.get_center() + Vector3(0, bounds.size.y * 0.5f, 0)) };
		for (const Vector3 &p : pts) {
			const float t = CLAMP(flow->sample_temperature(p), 0.0f, 1.5f);
			const float qg = 80.0f * t * t;
			if (qg > q) {
				q = qg;
				hot = p;
			}
		}
	}
	for (const PhysXBurnable3D *b : burnables) {
		if (b == this || b->state != STATE_FLAMING || !b->is_inside_tree()) {
			continue;
		}
		const Vector3 bc = b->_target_xform().xform(b->bounds.get_center());
		const float d = MAX(bc.distance_to(c) - 0.5f * (b->bounds.size.length() * 0.5f + bounds.size.length() * 0.5f), 0.3f);
		const float qr = CHI_R * b->hrr / 1000.0f / (4.0f * Math::PI * d * d);
		if (qr > q * 0.5f) {
			hot = c + (bc - c).normalized() * bounds.size.length() * 0.4f;
		}
		q += qr;
	}
	heat_in = q;
	if (state != STATE_UNBURNT) {
		return;
	}
	const float qw = q * 1000.0f;
	if (q > 5.0f && rain_water < 0.5f) {
		dose += qw * qw * p_delta;
	} else {
		dose *= Math::exp(-p_delta / 30.0f); // cools between exposures
	}
	const float dt = ignition_temperature - 20.0f;
	if (dose >= 0.25f * Math::PI * WOOD_KRHOC * dt * dt) {
		ignite(hot);
	}
}

void PhysXBurnable3D::ignite(const Vector3 &p_position) {
	_gather();
	if (state == STATE_FLAMING || fuel_left <= 0.0f) {
		return;
	}
	// The nearest point of the bounds: char starts there.
	const Vector3 local = _target_xform().affine_inverse().xform(p_position);
	const Vector3 lo = bounds.position, hi = bounds.position + bounds.size;
	origin_local = Vector3(CLAMP(local.x, lo.x, hi.x), CLAMP(local.y, lo.y, hi.y), CLAMP(local.z, lo.z, hi.z));
	burn_radius = MAX(burn_radius, 0.05f);
	state = STATE_FLAMING;
	rain_water = 0.0f;
}

void PhysXBurnable3D::extinguish() {
	if (state == STATE_FLAMING) {
		state = fuel_left > 0.0f ? STATE_SMOULDERING : STATE_BURNT_OUT;
		hrr = 0.0f;
	}
}

// Flaming: the burning patch grows from the ignition point at flame_spread; it gives off heat_release over its
// area (less as the fuel goes), burning fuel at the heat of combustion. Rain held on the wood damps and then puts
// it out. Out of fuel, it smoulders, then goes cold.
void PhysXBurnable3D::_burn(float p_delta) {
	float rain = 0.0f;
#ifdef MODULE_WEATHER_ENABLED
	if (const WeatherMap3D *wm = WeatherMap3D::get_active()) {
		rain = MAX(wm->get_rain_at(_target_xform().xform(bounds.get_center())), 0.0f);
	}
#endif
	// Water held on the surface (mm): a burning surface dries it fast, a cold one slowly.
	rain_water = MAX(rain_water + rain / 3600.0f * p_delta - p_delta * (state == STATE_FLAMING ? hrr / 2.0e8f : 1.0f / 1800.0f), 0.0f);
	if (state == STATE_FLAMING) {
		const float diag = bounds.size.length();
		burn_radius = MIN(burn_radius + flame_spread * p_delta, diag);
		const float area = MIN(Math::PI * burn_radius * burn_radius, _surface_area());
		const float left = fuel_initial > 0.0f ? fuel_left / fuel_initial : 0.0f;
		hrr = heat_release * 1000.0f * area * Math::sqrt(MAX(left, 0.0f)) * (1.0f - CLAMP(rain_water / 1.5f, 0.0f, 1.0f));
		fuel_left = MAX(fuel_left - hrr / WOOD_HEAT * p_delta, 0.0f);
		char_amount = CLAMP(area / _surface_area(), 0.0f, 1.0f);
		smoulder = MIN(smoulder + p_delta / 5.0f, 1.0f);
		if (fuel_left <= fuel_initial * 0.02f || rain_water >= 1.5f || (hrr < 1.0f && burn_radius > 0.1f)) {
			state = STATE_SMOULDERING; // burnt down, or put out by rain: embers in the char either way
			hrr = 0.0f;
		}
	} else if (state == STATE_SMOULDERING) {
		// Embers in the char die over about a minute, faster in rain.
		smoulder *= Math::exp(-p_delta / (rain > 0.0f ? 10.0f : 60.0f));
		if (smoulder < 0.01f) {
			smoulder = 0.0f;
			state = STATE_BURNT_OUT;
		}
	}
}

// One Flow sphere emitter of hot fuel gas over the burning patch, as the ground fire's tiles are fed.
void PhysXBurnable3D::_update_emitter() {
	if (!flow) {
		return;
	}
	if (!emitter) {
		emitter = memnew(PhysXFlowEmitter3D);
		emitter->set_name(String(get_name()) + "Flame");
		emitter->set_enabled(false);
		flow->add_child(emitter);
		emitter->set_flow_path(emitter->get_path_to(flow));
	}
	if (state != STATE_FLAMING || hrr <= 0.0f) {
		emitter->set_enabled(false);
		return;
	}
	const Transform3D xf = _target_xform();
	// Centred between the ignition point and the box's centre as the patch grows over it, on its upper part.
	const float grown = CLAMP(burn_radius / MAX(bounds.size.length() * 0.5f, 0.05f), 0.0f, 1.0f);
	Vector3 lc = origin_local.lerp(bounds.get_center(), grown);
	lc.y = MAX(lc.y, bounds.get_center().y);
	const float r = CLAMP(MIN(burn_radius, bounds.size.length() * 0.5f), 0.08f, 1.5f);
	const float flame = CLAMP(0.235f * Math::pow(hrr / 1000.0f, 0.4f) - 1.02f * 2.0f * r, 0.2f, 4.0f); // Heskestad
	emitter->set_enabled(true);
	emitter->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
	emitter->set_radius(r * 0.8f);
	emitter->set_global_position(xf.xform(lc));
	emitter->set_temperature(1.0f);
	emitter->set_fuel(flame_strength * CLAMP(hrr / 300000.0f, 0.15f, 1.0f));
	emitter->set_burn(0.0f);
	emitter->set_smoke(0.0f);
	emitter->set_velocity(Vector3(0, CLAMP(flame * 0.6f, 0.3f, 1.5f), 0));
	emitter->set_couple_rate(2.0f);
}

// Embers: released with the heat (ember_rate per MW), lofted by the plume and carried by Flow's gas with drag (a
// light firebrand's terminal speed about 3 m/s), glowing out over seconds. One landing on the ground fire's area
// lights it with a chance that falls to nothing at the fuel's moisture of extinction (Manzello et al. 2006).
void PhysXBurnable3D::_update_embers(float p_delta) {
	if (embers && state == STATE_FLAMING) {
		ember_carry += ember_rate * hrr / 1.0e6f * p_delta;
	}
	const Transform3D xf = _target_xform();
	while (ember_carry >= 1.0f && ember_list.size() < 512) {
		ember_carry -= 1.0f;
		Ember e;
		const Vector3 r = Vector3(_rand() - 0.5f, _rand() * 0.5f + 0.5f, _rand() - 0.5f);
		Vector3 lp = origin_local.lerp(bounds.get_center(), 0.5f) + bounds.size * r * Vector3(0.8f, 0.5f, 0.8f);
		e.p = xf.xform(lp);
		e.v = Vector3((_rand() - 0.5f) * 1.5f, 2.0f + _rand() * 3.0f, (_rand() - 0.5f) * 1.5f);
		e.life = 3.0f + _rand() * 9.0f;
		ember_list.push_back(e);
	}
	if (ember_list.is_empty()) {
		if (ember_mmi) {
			ember_mmi->set_visible(false);
		}
		return;
	}
	if (flow) {
		flow->request_velocity();
	}
	const float k = 3.0f; // 1/s, drag over mass
	const Vector3 g(0, -9.8f, 0);
	for (uint32_t i = 0; i < ember_list.size();) {
		Ember &e = ember_list[i];
		const Vector3 u = flow ? flow->sample_velocity(e.p) : Vector3();
		e.v += ((u - e.v) * k + g) * p_delta;
		e.p += e.v * p_delta;
		e.age += p_delta;
		float ground = xf.xform(bounds.position).y - 50.0f;
		if (fire) {
			ground = fire->get_ground_height_at(e.p);
		}
		bool gone = e.age >= e.life;
		if (!gone && e.p.y <= ground + 0.02f) {
			gone = true;
			const float glow = Math::exp(-2.0f * e.age / e.life);
			if (fire && glow > 0.2f) {
				const float m = fire->get_moisture_at(e.p);
				const float ext = fire->get_moisture_of_extinction();
				const float chance = 0.35f * glow * CLAMP(1.0f - m / MAX(ext * 0.9f, 1e-3f), 0.0f, 1.0f);
				if (_rand() < chance) {
					fire->ignite(Vector3(e.p.x, ground, e.p.z), 0.15f);
				}
			}
		}
		if (gone) {
			ember_list[i] = ember_list[ember_list.size() - 1];
			ember_list.resize(ember_list.size() - 1);
		} else {
			i++;
		}
	}
	// Drawn as small glowing points.
	if (!ember_mmi) {
		ember_mm.instantiate();
		ember_mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		ember_mm->set_use_colors(true);
		Ref<SphereMesh> sm;
		sm.instantiate();
		sm->set_radius(0.008f);
		sm->set_height(0.016f);
		sm->set_radial_segments(6);
		sm->set_rings(3);
		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
		mat->set_flag(BaseMaterial3D::FLAG_DISABLE_FOG, true);
		mat->set_albedo(Color(1, 1, 1));
		sm->surface_set_material(0, mat);
		ember_mm->set_mesh(sm);
		ember_mmi = memnew(MultiMeshInstance3D);
		ember_mmi->set_name("Embers");
		ember_mmi->set_multimesh(ember_mm);
		ember_mmi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		add_child(ember_mmi, false, INTERNAL_MODE_FRONT);
		ember_mmi->set_as_top_level(true);
		ember_mmi->set_global_transform(Transform3D());
	}
	ember_mmi->set_visible(true);
	ember_mm->set_instance_count(ember_list.size());
	for (uint32_t i = 0; i < ember_list.size(); i++) {
		const Ember &e = ember_list[i];
		const float g = Math::exp(-2.0f * e.age / e.life);
		ember_mm->set_instance_transform(i, Transform3D(Basis(), e.p));
		// A glowing firebrand, about 900 C dimming to dull red (linear, over 1 for the glow).
		ember_mm->set_instance_color(i, Color(glow_energy * g, glow_energy * 0.32f * g * g, glow_energy * 0.06f * g * g * g));
	}
}

// The meshes' look: char spreading from the ignition point with a ragged edge, the burning band glowing, and glowing
// cracks in the char while it smoulders, through instance uniforms the burn material reads.
void PhysXBurnable3D::_update_look() {
	if (!materials_done) {
		materials_done = true;
		for (const MeshRef &r : meshes) {
			MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(r.id));
			if (!mi || mi->get_mesh().is_null()) {
				continue;
			}
			for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++) {
				Ref<Material> base = mi->get_surface_override_material(s);
				if (base.is_null()) {
					base = mi->get_mesh()->surface_get_material(s);
				}
				const Ref<Material> m = _burn_material(base);
				if (m.is_valid() && m != base) {
					mi->set_surface_override_material(s, m);
				}
			}
		}
	}
	const float band = CLAMP(bounds.size.length() * 0.08f, 0.02f, 0.3f);
	for (const MeshRef &r : meshes) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(ObjectDB::get_instance(r.id));
		if (!mi) {
			continue;
		}
		const Vector3 o = r.to_mesh.xform(origin_local);
		const float sc = r.to_mesh.basis.get_scale().x;
		const float lit = state == STATE_UNBURNT ? 0.0f : 1.0f;
		mi->set_instance_shader_parameter("burn_origin", Vector4(o.x, o.y, o.z, burn_radius * sc * lit));
		mi->set_instance_shader_parameter("burn_state", Vector4(state == STATE_FLAMING ? glow_energy : 0.0f, smoulder * glow_energy * 0.5f, band * sc, lit));
	}
}

// The base material's shader with the burn lines (instance uniforms, so every crate shares one material). The
// path tracer reads instance uniforms too.
Ref<Material> PhysXBurnable3D::_burn_material(const Ref<Material> &p_base) {
	const Ref<BaseMaterial3D> base = p_base;
	if (base.is_null() || RenderingServer::get_singleton() == nullptr) {
		return p_base;
	}
	if (const Ref<Material> *found = material_cache.getptr(base->get_rid())) {
		return *found;
	}
	BaseMaterial3D::flush_changes();
	String code = RenderingServer::get_singleton()->shader_get_code(base->get_shader_rid());
	const int vx = code.find("void vertex() {");
	const String albedo_line = "	ALBEDO = albedo.rgb * albedo_tex.rgb;\n";
	const int al = code.find(albedo_line);
	if (code.is_empty() || vx < 0 || al < 0) {
		return p_base;
	}
	const String body = R"(
	{
		// PhysXBurnable3D: char from the ignition point, its edge ragged with a noise; the burning band glows,
		// and the char's cracks glow while it smoulders.
		if (burn_state.w > 0.0) {
			float d = length(burn_local - burn_origin.xyz);
			float n = burn_noise(burn_local * 9.0) * 0.6 + burn_noise(burn_local * 23.0) * 0.4;
			float edge = burn_origin.w - d + (n - 0.5) * burn_state.z * 2.0;
			float charred = smoothstep(-burn_state.z * 0.3, 0.0, edge);
			// Alligator char: blocks split by shrinkage cracks, the cell edges of a Voronoi pattern; cracks open
			// wider where the char is deeper (nearer the ignition point).
			float depth = clamp(edge / max(burn_state.z * 4.0, 1e-4), 0.0, 1.0);
			float cell = burn_cells(burn_local * 22.0);
			// Faded as the cells shrink below a few pixels, so they do not alias.
			float fw = fwidth(cell);
			float crack = (1.0 - smoothstep(0.012, 0.022 + 0.03 * depth + 0.5 * fw, cell)) / (1.0 + fw * 16.0);
			vec3 ch = mix(vec3(0.03, 0.026, 0.022), vec3(0.06, 0.05, 0.045), n) * (1.0 - 0.6 * crack);
			ALBEDO = mix(ALBEDO, ch, charred);
			// Glowing char in the cracks: a dull red to orange blackbody (about 600 to 900 C), flickering in patches.
			float heat = clamp(burn_noise(burn_local * 6.0 + vec3(0.0, TIME * 0.4, 0.0)) * 1.4 - 0.2, 0.0, 1.0);
			vec3 ember = mix(vec3(0.55, 0.06, 0.01), vec3(1.0, 0.3, 0.05), heat) * (0.3 + 0.7 * heat);
			burn_emit = vec3(0.5, 0.13, 0.02) * burn_state.x * (1.0 - smoothstep(0.0, burn_state.z, abs(edge))) * step(0.0, edge + burn_state.z * 0.3) + ember * burn_state.y * crack * charred;
		}
	}
)";
	code = code.insert(al + albedo_line.length(), body);
	// The glow added at the end of fragment(), after the material's own emission.
	const int fr = code.find("void fragment() {");
	int depth = 0;
	int end = -1;
	for (int i = code.find("{", fr); i < code.length(); i++) {
		if (code[i] == '{') {
			depth++;
		} else if (code[i] == '}') {
			depth--;
			if (depth == 0) {
				end = i;
				break;
			}
		}
	}
	if (end < 0) {
		return p_base;
	}
	code = code.insert(end, "	EMISSION += burn_emit;\n");
	code = code.insert(code.find("{", fr) + 1, "\n	vec3 burn_emit = vec3(0.0);");
	const int vx2 = code.find("void vertex() {");
	code = code.insert(vx2 + String("void vertex() {").length(), "\n	burn_local = VERTEX;");
	const String decls = R"(
instance uniform vec4 burn_origin;
instance uniform vec4 burn_state;
varying vec3 burn_local;
float burn_hash(vec3 p) {
	p = fract(p * 0.3183099 + 0.1);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float burn_noise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(burn_hash(i), burn_hash(i + vec3(1, 0, 0)), f.x), mix(burn_hash(i + vec3(0, 1, 0)), burn_hash(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(burn_hash(i + vec3(0, 0, 1)), burn_hash(i + vec3(1, 0, 1)), f.x), mix(burn_hash(i + vec3(0, 1, 1)), burn_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// Distance to the nearest Voronoi cell edge (F2 - F1).
float burn_cells(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	float d1 = 8.0;
	float d2 = 8.0;
	for (int k = 0; k < 27; k++) {
		vec3 o = vec3(float(k % 3), float((k / 3) % 3), float(k / 9)) - 1.0;
		vec3 c = i + o;
		vec3 r = o + vec3(burn_hash(c), burn_hash(c + 31.7), burn_hash(c + 57.3)) - f;
		float d = dot(r, r);
		if (d < d1) {
			d2 = d1;
			d1 = d;
		} else if (d < d2) {
			d2 = d;
		}
	}
	return sqrt(d2) - sqrt(d1);
}
)";
	code = code.insert(code.find("void vertex() {"), decls + "\n");
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(code);
	Ref<ShaderMaterial> m;
	m.instantiate();
	m->set_shader(sh);
	List<PropertyInfo> plist;
	sh->get_shader_uniform_list(&plist);
	const RID base_rid = base->get_rid();
	for (const PropertyInfo &pi : plist) {
		Variant v = RenderingServer::get_singleton()->material_get_param(base_rid, pi.name);
		if (v.get_type() == Variant::RID) {
			for (int t = 0; t < BaseMaterial3D::TEXTURE_MAX; t++) {
				const Ref<Texture2D> tex = base->get_texture(BaseMaterial3D::TextureParam(t));
				if (tex.is_valid() && tex->get_rid() == RID(v)) {
					v = tex;
					break;
				}
			}
		}
		if (v.get_type() != Variant::NIL) {
			m->set_shader_parameter(pi.name, v);
		}
	}
	material_cache.insert(base->get_rid(), m);
	return m;
}

void PhysXBurnable3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (!burnables.has(this)) {
				burnables.push_back(this);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			burnables.erase(this);
		} break;
		case NOTIFICATION_READY: {
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			const float dt = float(get_physics_process_delta_time());
			_gather();
			_resolve();
			sample_timer -= dt;
			if (sample_timer <= 0.0f) {
				sample_timer = 0.1f;
				_sample_heat(0.1f);
			}
			_burn(dt);
			_update_emitter();
			_update_embers(dt);
			_update_look();
		} break;
	}
}

void PhysXBurnable3D::set_target(const NodePath &p_path) {
	target = p_path;
	bounds_ready = false;
	materials_done = false;
}

void PhysXBurnable3D::set_fire(const NodePath &p_path) {
	fire_path = p_path;
	fire = nullptr;
}

void PhysXBurnable3D::set_flow(const NodePath &p_path) {
	flow_path = p_path;
	flow = nullptr;
}

void PhysXBurnable3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("ignite", "position"), &PhysXBurnable3D::ignite);
	ClassDB::bind_method(D_METHOD("extinguish"), &PhysXBurnable3D::extinguish);
	ClassDB::bind_method(D_METHOD("get_state"), &PhysXBurnable3D::get_state);
	ClassDB::bind_method(D_METHOD("is_burning"), &PhysXBurnable3D::is_burning);
	ClassDB::bind_method(D_METHOD("get_heat_release"), &PhysXBurnable3D::get_heat_release);
	ClassDB::bind_method(D_METHOD("get_heat_flux"), &PhysXBurnable3D::get_heat_flux);
	ClassDB::bind_method(D_METHOD("get_fuel_left"), &PhysXBurnable3D::get_fuel_left);
	ClassDB::bind_method(D_METHOD("get_char_amount"), &PhysXBurnable3D::get_char_amount);
	ClassDB::bind_method(D_METHOD("get_ember_count"), &PhysXBurnable3D::get_ember_count);

	ClassDB::bind_method(D_METHOD("set_target", "path"), &PhysXBurnable3D::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &PhysXBurnable3D::get_target);
	ClassDB::bind_method(D_METHOD("set_fire", "path"), &PhysXBurnable3D::set_fire);
	ClassDB::bind_method(D_METHOD("get_fire"), &PhysXBurnable3D::get_fire);
	ClassDB::bind_method(D_METHOD("set_flow", "path"), &PhysXBurnable3D::set_flow);
	ClassDB::bind_method(D_METHOD("get_flow"), &PhysXBurnable3D::get_flow);
	ClassDB::bind_method(D_METHOD("set_fuel_mass", "kg"), &PhysXBurnable3D::set_fuel_mass);
	ClassDB::bind_method(D_METHOD("get_fuel_mass"), &PhysXBurnable3D::get_fuel_mass);
	ClassDB::bind_method(D_METHOD("set_heat_release_rate", "kw_per_m2"), &PhysXBurnable3D::set_heat_release);
	ClassDB::bind_method(D_METHOD("get_heat_release_rate"), &PhysXBurnable3D::get_heat_release_rate);
	ClassDB::bind_method(D_METHOD("set_flame_spread", "speed"), &PhysXBurnable3D::set_flame_spread);
	ClassDB::bind_method(D_METHOD("get_flame_spread"), &PhysXBurnable3D::get_flame_spread);
	ClassDB::bind_method(D_METHOD("set_ignition_temperature", "celsius"), &PhysXBurnable3D::set_ignition_temperature);
	ClassDB::bind_method(D_METHOD("get_ignition_temperature"), &PhysXBurnable3D::get_ignition_temperature);
	ClassDB::bind_method(D_METHOD("set_flame_strength", "strength"), &PhysXBurnable3D::set_flame_strength);
	ClassDB::bind_method(D_METHOD("get_flame_strength"), &PhysXBurnable3D::get_flame_strength);
	ClassDB::bind_method(D_METHOD("set_embers", "enable"), &PhysXBurnable3D::set_embers);
	ClassDB::bind_method(D_METHOD("has_embers"), &PhysXBurnable3D::has_embers);
	ClassDB::bind_method(D_METHOD("set_ember_rate", "rate"), &PhysXBurnable3D::set_ember_rate);
	ClassDB::bind_method(D_METHOD("get_ember_rate"), &PhysXBurnable3D::get_ember_rate);
	ClassDB::bind_method(D_METHOD("set_glow_energy", "energy"), &PhysXBurnable3D::set_glow_energy);
	ClassDB::bind_method(D_METHOD("get_glow_energy"), &PhysXBurnable3D::get_glow_energy);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_target", "get_target");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "fire", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFireSpread3D"), "set_fire", "get_fire");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "flow", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "PhysXFlow3D"), "set_flow", "get_flow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fuel_mass", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater,suffix:kg"), "set_fuel_mass", "get_fuel_mass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heat_release_rate", PROPERTY_HINT_RANGE, "1,500,1,suffix:kW/m²"), "set_heat_release_rate", "get_heat_release_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flame_spread", PROPERTY_HINT_RANGE, "0,0.5,0.001,suffix:m/s"), "set_flame_spread", "get_flame_spread");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ignition_temperature", PROPERTY_HINT_RANGE, "100,700,1,suffix:°C"), "set_ignition_temperature", "get_ignition_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flame_strength", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_flame_strength", "get_flame_strength");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "embers"), "set_embers", "has_embers");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ember_rate", PROPERTY_HINT_RANGE, "0,500,0.1"), "set_ember_rate", "get_ember_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glow_energy", PROPERTY_HINT_RANGE, "0,32,0.1"), "set_glow_energy", "get_glow_energy");

	BIND_ENUM_CONSTANT(STATE_UNBURNT);
	BIND_ENUM_CONSTANT(STATE_FLAMING);
	BIND_ENUM_CONSTANT(STATE_SMOULDERING);
	BIND_ENUM_CONSTANT(STATE_BURNT_OUT);
}
