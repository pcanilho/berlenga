/**************************************************************************/
/*  physx_cloth_burn.cpp                                                  */
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

#include "physx_cloth_burn.h"

#include "../cloth/xpbd_cloth_solver.h"
#include "physx_burnable_3d.h"
#include "physx_fire_spread_3d.h"
#include "physx_flow_3d.h"
#include "physx_flow_emitter_3d.h"

#include "core/math/math_funcs.h"
#include "scene/main/node.h"
#include "servers/rendering/rendering_server.h"

// Thermally thin ignition (Quintiere, Fundamentals of Fire Phenomena, ch. 7): a fabric lights once it has taken in
// its areal heat capacity times the rise to ignition, about 0.26 kJ/m2K (a 0.5 mm cotton weave) times 380 K; below a
// critical flux of about 10 kW/m2 it loses heat as fast as it gains it and never lights.
static constexpr float IGNITION_DOSE = 0.26f * 380.0f; // kJ/m2
static constexpr float CRITICAL_FLUX = 10.0f; // kW/m2
static constexpr float UPWARD_SPREAD = 8.0f; // upward flame spread over downward and lateral on a hanging fabric
static constexpr int MAX_EMITTERS = 4;

void PhysXClothBurn::reset() {
	heat.clear();
	ignited.clear();
	progress.clear();
	jitter.clear();
	gone.clear();
	neighbours.clear();
	built_vertices = 0;
	built_indices = 0;
	time = 0.0f;
	any_lit = false;
	any_heat = false;
	clear_emitters();
}

void PhysXClothBurn::clear_emitters() {
	for (const ObjectID &id : emitters) {
		if (Node *n = Object::cast_to<Node>(ObjectDB::get_instance(id))) {
			n->queue_free();
		}
	}
	emitters.clear();
}

// Per-particle state follows the solver's particles (tearing appends copies); the neighbours follow its triangles.
void PhysXClothBurn::_rebuild(const XPBDClothSolver &p_solver) {
	const uint32_t n = p_solver.vertex_count();
	const uint32_t old = heat.size();
	heat.resize(n);
	ignited.resize(n);
	progress.resize(n);
	jitter.resize(n);
	gone.resize(n);
	for (uint32_t i = old; i < n; i++) {
		heat[i] = 0.0f;
		ignited[i] = -1.0f;
		progress[i] = 0.0f;
		uint32_t h = i * 2654435761u;
		h ^= h >> 15;
		jitter[i] = float(h & 0xffffu) / 65535.0f;
		gone[i] = 0;
	}
	neighbours.resize(n);
	for (LocalVector<int> &l : neighbours) {
		l.clear();
	}
	const LocalVector<int> &idx = p_solver.get_indices();
	auto link = [&](int a, int b) {
		if (!neighbours[a].has(b)) {
			neighbours[a].push_back(b);
		}
	};
	for (uint32_t t = 0; t + 2 < idx.size(); t += 3) {
		const int a = idx[t], b = idx[t + 1], c = idx[t + 2];
		link(a, b);
		link(b, a);
		link(b, c);
		link(c, b);
		link(c, a);
		link(a, c);
	}
	built_vertices = n;
	built_indices = idx.size();
}

void PhysXClothBurn::_sample(const XPBDClothSolver &p_solver, PhysXFireSpread3D *p_fire, PhysXFlow3D *p_flow, float p_delta) {
	const LocalVector<Vector3> &pos = p_solver.get_positions();
	for (uint32_t i = 0; i < pos.size(); i++) {
		if (ignited[i] >= 0.0f || gone[i]) {
			continue;
		}
		const Vector3 &p = pos[i];
		float q = 0.0f;
		if (p_fire) {
			// Flame contact: within a ground fire's flame over burning ground, up to about 60 kW/m2.
			const float flaming = p_fire->get_intensity_at(p);
			if (flaming > 0.0f && p.y - p_fire->get_ground_height_at(p) < MAX(p_fire->get_flame_length_at(p), 0.2f)) {
				q = MIN(60.0f * Math::sqrt(flaming / 600.0f), 60.0f);
			}
		}
		if (p_flow) {
			// Hot gas: Flow's temperature (1 at a flame's core) as up to about 80 kW/m2.
			const float t = CLAMP(p_flow->sample_temperature(p), 0.0f, 1.5f);
			q = MAX(q, 80.0f * t * t);
		}
		q += PhysXBurnable3D::radiant_flux_at(p);
		if (q > CRITICAL_FLUX) {
			heat[i] += (q - CRITICAL_FLUX) * p_delta;
			any_heat = true;
		} else {
			heat[i] *= Math::exp(-p_delta / 10.0f); // cools between exposures
		}
		if (heat[i] >= IGNITION_DOSE) {
			ignited[i] = time;
			any_lit = true;
		}
	}
}

void PhysXClothBurn::ignite(const XPBDClothSolver &p_solver, const Vector3 &p_world) {
	if (built_vertices != (uint32_t)p_solver.vertex_count() || built_indices != p_solver.get_indices().size()) {
		_rebuild(p_solver);
	}
	const LocalVector<Vector3> &pos = p_solver.get_positions();
	int best = -1;
	float bd = 1e30f;
	for (uint32_t i = 0; i < pos.size(); i++) {
		const float d = pos[i].distance_squared_to(p_world);
		if (!gone[i] && d < bd) {
			bd = d;
			best = i;
		}
	}
	if (best >= 0 && ignited[best] < 0.0f) {
		ignited[best] = time;
		heat[best] = IGNITION_DOSE;
		any_lit = true;
	}
}

bool PhysXClothBurn::step(Node *p_owner, XPBDClothSolver &p_solver, PhysXFireSpread3D *p_fire, PhysXFlow3D *p_flow, float p_delta) {
	if (p_solver.vertex_count() == 0) {
		return false;
	}
	if (built_vertices != (uint32_t)p_solver.vertex_count() || built_indices != p_solver.get_indices().size()) {
		_rebuild(p_solver);
	}
	time += p_delta;
	sample_timer -= p_delta;
	if (sample_timer <= 0.0f && (p_fire || p_flow || any_lit)) {
		sample_timer = 0.1f;
		_sample(p_solver, p_fire, p_flow, 0.1f);
	}
	if (!any_lit) {
		return false;
	}
	// Flame spreads from each burning particle to its neighbours over the edge between them, up the fabric fastest.
	const LocalVector<Vector3> &pos = p_solver.get_positions();
	bool burnt = false;
	for (uint32_t i = 0; i < pos.size(); i++) {
		if (ignited[i] < 0.0f || gone[i]) {
			continue;
		}
		const float age = time - ignited[i];
		const float through = settings.burn_through_time * (0.7f + 0.6f * jitter[i]);
		if (age >= through) {
			gone[i] = 1;
			burnt = true;
			continue;
		}
		for (int j : neighbours[i]) {
			if (ignited[j] >= 0.0f || gone[j]) {
				continue;
			}
			const Vector3 e = pos[j] - pos[i];
			const float len = e.length();
			if (len < 1.0e-5f) {
				progress[j] = 1.0f;
			} else {
				const float up = MAX(e.y / len, 0.0f);
				const float v = settings.flame_spread * (1.0f + (UPWARD_SPREAD - 1.0f) * up * up) * (0.6f + 0.8f * jitter[j]);
				progress[j] += p_delta * v / len;
			}
			if (progress[j] >= 1.0f) {
				ignited[j] = time;
			}
		}
	}
	int removed = 0;
	if (burnt) {
		removed = p_solver.burn_away(gone);
		if (removed > 0) {
			_rebuild(p_solver);
		}
	}
	_update_emitters(p_owner, p_solver, p_flow);
	return removed > 0;
}

// A few Flow emitters over the burning edge: the burning particles split into up to MAX_EMITTERS groups along their
// spread (farthest-point picks), each an emitter at its group's middle sized by the group.
void PhysXClothBurn::_update_emitters(Node *p_owner, const XPBDClothSolver &p_solver, PhysXFlow3D *p_flow) {
	if (!p_flow) {
		return;
	}
	const LocalVector<Vector3> &pos = p_solver.get_positions();
	LocalVector<int> burning;
	for (uint32_t i = 0; i < pos.size(); i++) {
		if (ignited[i] >= 0.0f && !gone[i] && time - ignited[i] < settings.burn_through_time) {
			burning.push_back(i);
		}
	}
	LocalVector<Vector3> seeds;
	if (!burning.is_empty()) {
		seeds.push_back(pos[burning[0]]);
		while ((int)seeds.size() < MAX_EMITTERS) {
			float far = 0.0f;
			int pick = -1;
			for (int b : burning) {
				float d = 1e30f;
				for (const Vector3 &s : seeds) {
					d = MIN(d, s.distance_squared_to(pos[b]));
				}
				if (d > far) {
					far = d;
					pick = b;
				}
			}
			if (pick < 0 || far < 0.04f) {
				break; // the rest are within 20 cm of a seed
			}
			seeds.push_back(pos[pick]);
		}
	}
	LocalVector<Vector3> centre;
	LocalVector<float> count;
	LocalVector<float> reach;
	centre.resize(seeds.size());
	count.resize(seeds.size());
	reach.resize(seeds.size());
	for (uint32_t k = 0; k < seeds.size(); k++) {
		centre[k] = Vector3();
		count[k] = 0.0f;
		reach[k] = 0.0f;
	}
	for (int b : burning) {
		int best = 0;
		float bd = 1e30f;
		for (uint32_t k = 0; k < seeds.size(); k++) {
			const float d = seeds[k].distance_squared_to(pos[b]);
			if (d < bd) {
				bd = d;
				best = k;
			}
		}
		centre[best] += pos[b];
		count[best] += 1.0f;
		reach[best] = MAX(reach[best], Math::sqrt(bd));
	}
	while (emitters.size() < seeds.size()) {
		PhysXFlowEmitter3D *e = memnew(PhysXFlowEmitter3D);
		e->set_name(String(p_owner->get_name()) + "Flame" + itos(emitters.size()));
		e->set_shape(PhysXFlowEmitter3D::SHAPE_SPHERE);
		p_flow->add_child(e);
		e->set_flow_path(e->get_path_to(p_flow));
		emitters.push_back(e->get_instance_id());
	}
	for (uint32_t k = 0; k < emitters.size(); k++) {
		PhysXFlowEmitter3D *e = Object::cast_to<PhysXFlowEmitter3D>(ObjectDB::get_instance(emitters[k]));
		if (!e) {
			continue;
		}
		if (k >= seeds.size() || count[k] <= 0.0f) {
			e->set_enabled(false);
			continue;
		}
		e->set_enabled(true);
		e->set_global_position(centre[k] / count[k]);
		e->set_radius(CLAMP(reach[k] * 0.3f + 0.04f, 0.06f, 0.15f)); // thin fabric: a band of low flame, not a bonfire
		e->set_temperature(1.0f);
		e->set_fuel(CLAMP(count[k] / 160.0f, 0.1f, 0.35f));
		e->set_burn(0.0f);
		e->set_smoke(0.0f);
		e->set_velocity(Vector3(0, 0.8f, 0));
		e->set_couple_rate(2.0f);
	}
}

void PhysXClothBurn::fill_colors(PackedColorArray &r_colors, int p_count) const {
	r_colors.resize(p_count);
	Color *w = r_colors.ptrw();
	for (int i = 0; i < p_count; i++) {
		if (i >= (int)heat.size()) {
			w[i] = Color(0, 0, 0, 1);
			continue;
		}
		const float scorch = CLAMP(heat[i] / IGNITION_DOSE, 0.0f, 1.0f);
		float ch = 0.0f;
		float glow = 0.0f;
		if (ignited[i] >= 0.0f) {
			const float age = (time - ignited[i]) / MAX(settings.burn_through_time, 0.1f);
			ch = CLAMP(age * 2.5f, 0.0f, 1.0f);
			// Glows at the burning edge: brightest just before the fabric burns through.
			glow = CLAMP(age * 1.5f, 0.0f, 1.0f) * (0.6f + 0.4f * jitter[i]);
		}
		// Alpha: 1 while whole, falling to 0 as the part burns through, so the shader eats the fabric along a soft
		// curve between particles instead of the mesh's triangle staircase.
		float whole = 1.0f;
		if (ignited[i] >= 0.0f) {
			const float age = (time - ignited[i]) / MAX(settings.burn_through_time * (0.7f + 0.6f * jitter[i]), 0.1f);
			whole = 1.0f - CLAMP(age, 0.0f, 1.0f); // over the whole burn, so the cut between particles is a smooth curve
		}
		w[i] = Color(ch, glow, scorch, whole);
	}
}

int PhysXClothBurn::get_burning_count() const {
	int n = 0;
	for (uint32_t i = 0; i < ignited.size(); i++) {
		n += (ignited[i] >= 0.0f && !gone[i]) ? 1 : 0;
	}
	return n;
}

int PhysXClothBurn::get_burnt_count() const {
	int n = 0;
	for (uint8_t g : gone) {
		n += g ? 1 : 0;
	}
	return n;
}

// The cloth's material with the burn read from vertex colours: scorched brown ahead of the flame, black char behind,
// the burning edge glowing (a dull red to orange blackbody).
RID PhysXClothBurn::get_material(const Ref<Material> &p_base) {
	if (burn_material.is_valid() && source_material == p_base) {
		return burn_material->get_rid();
	}
	source_material = p_base;
	Ref<BaseMaterial3D> base = p_base;
	if (base.is_null()) {
		Ref<StandardMaterial3D> def;
		def.instantiate();
		def->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		base = def;
	}
	BaseMaterial3D::flush_changes();
	String code = RenderingServer::get_singleton()->shader_get_code(base->get_shader_rid());
	const String albedo_line = "	ALBEDO = albedo.rgb * albedo_tex.rgb;\n";
	const int al = code.find(albedo_line);
	const int vx = code.find("void vertex() {");
	const int fr = code.find("void fragment() {");
	if (code.is_empty() || al < 0 || vx < 0 || fr < 0) {
		burn_material = p_base;
		return p_base.is_valid() ? p_base->get_rid() : RID();
	}
	code = code.insert(al + albedo_line.length(), R"(
	{
		// PhysXClothBurn: scorch, char and the burning edge's glow (vertex colour r char, g glow, b scorch).
		vec3 scorched = mix(ALBEDO, vec3(0.3, 0.16, 0.07) * (0.4 + dot(ALBEDO, vec3(0.3, 0.59, 0.11))), smoothstep(0.3, 1.0, cloth_burn.b));
		ALBEDO = mix(scorched, vec3(0.03, 0.027, 0.024), cloth_burn.r);
		cloth_burn_emit = mix(vec3(0.55, 0.06, 0.01), vec3(1.0, 0.3, 0.05), cloth_burn.g) * cloth_burn.g * cloth_glow_energy;
		// The burning edge: a noisy threshold on how far each part has burnt through (dissolve), glowing just
		// inside the cut.
		vec2 cb_p = UV * 96.0;
		vec2 cb_i = floor(cb_p);
		vec2 cb_f = fract(cb_p);
		cb_f = cb_f * cb_f * (3.0 - 2.0 * cb_f);
		float cb_h00 = fract(sin(dot(cb_i, vec2(127.1, 311.7))) * 43758.5453);
		float cb_h10 = fract(sin(dot(cb_i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
		float cb_h01 = fract(sin(dot(cb_i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
		float cb_h11 = fract(sin(dot(cb_i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
		float cb_n = mix(mix(cb_h00, cb_h10, cb_f.x), mix(cb_h01, cb_h11, cb_f.x), cb_f.y);
		float cb_cut = (1.0 - cloth_burn.a) + (cb_n - 0.5) * 0.2;
		if (cb_cut > 0.72) {
			discard;
		}
		// The burning edge: a band of glowing ember just inside the cut, black char behind it.
		float cb_rim = smoothstep(0.52, 0.66, cb_cut) * (1.0 - smoothstep(0.69, 0.72, cb_cut) * 0.3);
		ALBEDO = mix(ALBEDO, vec3(0.02), smoothstep(0.4, 0.6, cb_cut));
		cloth_burn_emit += vec3(1.0, 0.3, 0.05) * cb_rim * cloth_glow_energy * 1.5;
	}
)");
	// The glow added at the end of fragment(), after any emission the material sets.
	int depth = 0;
	int end = -1;
	const int fr2 = code.find("void fragment() {");
	for (int i = code.find("{", fr2); i < code.length(); i++) {
		if (code[i] == '{') {
			depth++;
		} else if (code[i] == '}' && --depth == 0) {
			end = i;
			break;
		}
	}
	if (end > 0) {
		code = code.insert(end, "	EMISSION += cloth_burn_emit;\n");
	}
	code = code.insert(code.find("{", fr2) + 1, "\n	vec3 cloth_burn_emit = vec3(0.0);");
	const int vx2 = code.find("void vertex() {");
	code = code.insert(vx2 + String("void vertex() {").length(), "\n	cloth_burn = COLOR;");
	code = code.insert(code.find("void vertex() {"), "varying vec4 cloth_burn;\nuniform float cloth_glow_energy = 4.0;\n\n");
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
	m->set_shader_parameter("cloth_glow_energy", settings.glow_energy);
	burn_material = m;
	return m->get_rid();
}
