/**************************************************************************/
/*  ground_debris_flow_3d.cpp                                             */
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

#include "ground_debris_flow_3d.h"

#include "ground_surface_3d.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/surface_tool.h"

#include "modules/terrain_deform/terrain_brush_3d.h"
#include "modules/terrain_deform/terrain_deformer_3d.h"
#include "modules/world/world_contracts.h"

static constexpr float G = 9.81f;

GroundDebrisFlow3D::Body *GroundDebrisFlow3D::_find(int64_t p_id) const {
	for (Body *b : bodies) {
		if (b->id == p_id) {
			return b;
		}
		for (int64_t k : b->pooled) {
			if (k == p_id) {
				return b;
			}
		}
	}
	return nullptr;
}

float GroundDebrisFlow3D::_ground_height(const Vector3 &p_position) const {
	if (WorldContracts *wc = WorldContracts::get_singleton()) {
		Ref<WorldGround> g = wc->get_ground();
		if (g.is_valid()) {
			const float h = g->height_at(p_position);
			if (Math::is_finite(h)) {
				return h;
			}
		}
	}
	if (GroundSurface3D *gs = GroundSurface3D::get_active()) {
		return gs->get_surface_height_at(p_position);
	}
	return p_position.y;
}

Vector3 GroundDebrisFlow3D::_cell_position(const Body &p_body, int p_row, int p_col) const {
	const float off = (float(p_col) - 0.5f * float(p_body.nw - 1)) * p_body.cell;
	const Vector3 p = p_body.center[p_row] + p_body.normal[p_row] * off;
	return Vector3(p.x, p_body.bed[p_row * p_body.nw + p_col], p.z);
}

void GroundDebrisFlow3D::start(const Dictionary &p_event) {
	const int64_t id = p_event.get("id", int64_t(-1));
	if (id >= 0 && _find(id)) {
		return; // seen (a replayed log)
	}
	const float volume = MAX(float(p_event.get("volume", 0.0f)), 0.0f);
	PackedVector3Array path = p_event.get("path", PackedVector3Array());
	const Vector3 outlet = p_event.get("position", path.is_empty() ? Vector3() : path[0]);
	if (volume <= 0.0f || path.size() < 2) {
		return;
	}
	const PackedFloat32Array widths = p_event.get("widths", PackedFloat32Array());
	const PackedFloat32Array severity = p_event.get("severity", PackedFloat32Array());
	// Pooled: an event whose outlet lies on a running body's corridor feeds it there (the gullies of one burnt slope).
	for (Body *b : bodies) {
		if (b->done) {
			continue;
		}
		int best = -1;
		float best_d = pool_distance;
		for (int r = 0; r < b->ns; r++) {
			const float d = Vector2(outlet.x - b->center[r].x, outlet.z - b->center[r].z).length();
			if (d < best_d) {
				best_d = d;
				best = r;
			}
		}
		if (best >= 0) {
			Body::Feed f;
			f.row = best;
			f.volume = volume;
			f.start = b->time;
			b->feeds.push_back(f);
			b->pooled.push_back(id);
			b->still = 0.0f;
			return;
		}
	}
	Body *b = memnew(Body);
	b->id = id;
	b->cell = cell_size;
	// The path resampled at the cell size, carried on 40 m past its end along its last heading (the fan).
	LocalVector<float> cum;
	cum.push_back(0.0f);
	for (int k = 1; k < path.size(); k++) {
		cum.push_back(cum[k - 1] + Vector2(path[k].x - path[k - 1].x, path[k].z - path[k - 1].z).length());
	}
	const float total = cum[cum.size() - 1];
	Vector3 last_dir = path[path.size() - 1] - path[path.size() - 2];
	last_dir.y = 0.0f;
	last_dir = last_dir.length() > 1e-4f ? last_dir.normalized() : Vector3(0, 0, 1);
	const float run = total + 40.0f;
	int k = 0;
	for (float s = 0.0f; s <= run; s += b->cell) {
		Vector3 p;
		Vector3 dir;
		float pi = 0.0f;
		float w = 4.0f;
		float sev = 1.0f;
		if (s <= total) {
			while (k < (int)cum.size() - 2 && cum[k + 1] < s) {
				k++;
			}
			const float seg = MAX(cum[k + 1] - cum[k], 1e-4f);
			const float t = CLAMP((s - cum[k]) / seg, 0.0f, 1.0f);
			p = path[k].lerp(path[k + 1], t);
			dir = path[k + 1] - path[k];
			pi = float(k) + t;
			if (widths.size() > k + 1) {
				w = Math::lerp(widths[k], widths[k + 1], t);
			}
			if (severity.size() > k + 1) {
				sev = Math::lerp(severity[k], severity[k + 1], t);
			}
		} else {
			p = path[path.size() - 1] + last_dir * (s - total);
			dir = last_dir;
			pi = float(path.size() - 1) + (s - total) / MAX(b->cell, 0.1f);
			w = widths.is_empty() ? 4.0f : widths[widths.size() - 1];
			sev = 0.0f; // the fan is not scoured
		}
		dir.y = 0.0f;
		dir = dir.length() > 1e-4f ? dir.normalized() : last_dir;
		b->center.push_back(p);
		b->normal.push_back(Vector3(-dir.z, 0.0f, dir.x));
		b->path_index.push_back(pi);
		b->half_channel.push_back(0.5f * MAX(w, b->cell));
		b->severity.push_back(CLAMP(sev, 0.0f, 1.0f));
	}
	b->ns = (int)b->center.size();
	b->nw = MAX(3, int(corridor_width / b->cell) | 1);
	const int n = b->ns * b->nw;
	b->bed.resize(n);
	b->scoured.resize(n);
	b->loose.resize(n);
	b->h.resize(n);
	for (int r = 0; r < b->ns; r++) {
		for (int c = 0; c < b->nw; c++) {
			const int i = r * b->nw + c;
			const float off = (float(c) - 0.5f * float(b->nw - 1)) * b->cell;
			const Vector3 p = b->center[r] + b->normal[r] * off;
			b->bed[i] = _ground_height(Vector3(p.x, p.y + 50.0f, p.z));
			b->scoured[i] = 0.0f;
			b->h[i] = 0.0f;
			// The channel bed's loose fill, deepest where the fire left the most: the cap scour can reach.
			b->loose[i] = Math::abs(off) <= b->half_channel[r] ? max_scour * b->severity[r] : 0.0f;
		}
	}
	b->qs.resize((b->ns + 1) * b->nw);
	b->qn.resize(b->ns * (b->nw + 1));
	for (float &q : b->qs) {
		q = 0.0f;
	}
	for (float &q : b->qn) {
		q = 0.0f;
	}
	Body::Feed f;
	f.row = 0;
	f.volume = volume;
	b->feeds.push_back(f);
	bodies.push_back(b);
	_update_front(*b);
	emit_signal(SNAME("debris_flow_started"), b->id);
}

// The feed's hydrograph: a triangle peaking at Q_p = 0.1 V^(5/6) (Rickenmann 1999) a fifth of the way through,
// lasting 2 V / Q_p.
static float _feed_rate(float p_volume, float p_t) {
	const float qp = 0.1f * Math::pow(MAX(p_volume, 0.0f), 5.0f / 6.0f);
	if (qp <= 0.0f || p_t < 0.0f) {
		return 0.0f;
	}
	const float dur = 2.0f * p_volume / qp;
	const float tp = 0.2f * dur;
	if (p_t >= dur) {
		return 0.0f;
	}
	return p_t < tp ? qp * p_t / tp : qp * (dur - p_t) / (dur - tp);
}

void GroundDebrisFlow3D::_step(Body &p_body, float p_dt) {
	Body &b = p_body;
	const int ns = b.ns;
	const int nw = b.nw;
	const float dx = b.cell;
	const float area = dx * dx;
	auto eta = [&](int i) { return b.bed[i] + b.h[i]; };
	// Faces: momentum (local inertial, Bates et al. 2010) against the bed slope and the surface's, then the
	// Voellmy turbulence (semi-implicit), then Coulomb friction and the yield stress, which can only stop it.
	const float tau = voellmy_mu * G;
	const float yield = yield_stress / density;
	auto face = [&](float &q, int a, int c) {
		const float ea = eta(a);
		const float ec = eta(c);
		const float hf = MAX(ea, ec) - MAX(b.bed[a], b.bed[c]);
		if (hf < 1e-3f) {
			q = 0.0f;
			return;
		}
		q -= p_dt * G * hf * (ec - ea) / dx;
		q /= 1.0f + p_dt * G * Math::abs(q) / (voellmy_xi * hf * hf);
		const float stop = p_dt * (tau * hf + yield);
		q = Math::abs(q) <= stop ? 0.0f : q - SIGN(q) * stop;
		const float cap = 2.0f * hf * Math::sqrt(G * hf);
		q = CLAMP(q, -cap, cap);
	};
	for (int r = 1; r < ns; r++) {
		for (int c = 0; c < nw; c++) {
			face(b.qs[r * nw + c], (r - 1) * nw + c, r * nw + c);
		}
	}
	// Out of the corridor's end: whatever reaches it runs on (zero gradient).
	for (int c = 0; c < nw; c++) {
		b.qs[ns * nw + c] = MAX(b.qs[(ns - 1) * nw + c], 0.0f);
		b.qs[c] = 0.0f;
	}
	for (int r = 0; r < ns; r++) {
		b.qn[r * (nw + 1)] = 0.0f;
		b.qn[r * (nw + 1) + nw] = 0.0f;
		for (int c = 1; c < nw; c++) {
			face(b.qn[r * (nw + 1) + c], r * nw + c - 1, r * nw + c);
		}
	}
	// No cell gives more than it has: each face is the outflow of exactly one cell, so scaling a cell's outflows keeps
	// the volume.
	for (int r = 0; r < ns; r++) {
		for (int c = 0; c < nw; c++) {
			const int i = r * nw + c;
			float &up = b.qs[r * nw + c]; // out when negative
			float &down = b.qs[(r + 1) * nw + c]; // out when positive
			float &left = b.qn[r * (nw + 1) + c]; // out when negative
			float &right = b.qn[r * (nw + 1) + c + 1]; // out when positive
			const float out = (MAX(-up, 0.0f) + MAX(down, 0.0f) + MAX(-left, 0.0f) + MAX(right, 0.0f)) * dx * p_dt;
			const float have = b.h[i] * area;
			if (out > have && out > 0.0f) {
				const float f = have / out;
				if (up < 0.0f) {
					up *= f;
				}
				if (down > 0.0f) {
					down *= f;
				}
				if (left < 0.0f) {
					left *= f;
				}
				if (right > 0.0f) {
					right *= f;
				}
			}
		}
	}
	for (int r = 0; r < ns; r++) {
		for (int c = 0; c < nw; c++) {
			const int i = r * nw + c;
			const float div = (b.qs[(r + 1) * nw + c] - b.qs[r * nw + c] + b.qn[r * (nw + 1) + c + 1] - b.qn[r * (nw + 1) + c]) / dx;
			b.h[i] = MAX(b.h[i] - p_dt * div, 0.0f);
		}
	}
	for (int c = 0; c < nw; c++) {
		b.volume_out += b.qs[ns * nw + c] * dx * p_dt;
	}
	// The feeds, spread over the channel's cells in their row.
	for (const Body::Feed &f : b.feeds) {
		const float q = _feed_rate(f.volume, b.time - f.start);
		if (q <= 0.0f) {
			continue;
		}
		int cells = 0;
		for (int c = 0; c < nw; c++) {
			if (Math::abs((float(c) - 0.5f * float(nw - 1)) * dx) <= b.half_channel[f.row]) {
				cells++;
			}
		}
		cells = MAX(cells, 1);
		for (int c = 0; c < nw; c++) {
			if (cells == 1 ? c == nw / 2 : Math::abs((float(c) - 0.5f * float(nw - 1)) * dx) <= b.half_channel[f.row]) {
				b.h[f.row * nw + c] += q * p_dt / (area * cells);
			}
		}
		b.volume_in += q * p_dt;
	}
	// Entrainment (McDougall and Hungr 2005): the flow takes up the channel's loose bed at E h |u| per area.
	if (entrainment > 0.0f) {
		for (int r = 0; r < ns; r++) {
			if (b.severity[r] <= 0.0f) {
				continue;
			}
			for (int c = 0; c < nw; c++) {
				const int i = r * nw + c;
				if (b.h[i] < 0.05f || b.scoured[i] >= b.loose[i]) {
					continue;
				}
				const float us = 0.5f * (b.qs[r * nw + c] + b.qs[(r + 1) * nw + c]);
				const float un = 0.5f * (b.qn[r * (nw + 1) + c] + b.qn[r * (nw + 1) + c + 1]);
				const float flux = Vector2(us, un).length(); // h |u|
				const float de = MIN(entrainment * b.severity[r] * flux * p_dt, b.loose[i] - b.scoured[i]);
				if (de > 0.0f) {
					b.bed[i] -= de;
					b.scoured[i] += de;
					b.h[i] += de;
					b.volume_entrained += de * area;
				}
			}
		}
	}
	b.time += p_dt;
}

void GroundDebrisFlow3D::_update_front(Body &p_body) {
	Body &b = p_body;
	const int nw = b.nw;
	b.front_depth = 0.0f;
	b.front_width = 0.0f;
	b.front_speed = 0.0f;
	for (int r = b.ns - 1; r >= 0; r--) {
		int best = -1;
		int wet = 0;
		for (int c = 0; c < nw; c++) {
			const float h = b.h[r * nw + c];
			if (h > 0.05f) {
				wet++;
				if (best < 0 || h > b.h[r * nw + best]) {
					best = c;
				}
			}
		}
		if (best < 0) {
			continue;
		}
		const int i = r * nw + best;
		const Vector3 p = _cell_position(b, r, best);
		b.front = Vector3(p.x, p.y + b.h[i], p.z);
		b.front_index = b.path_index[r];
		b.front_depth = b.h[i];
		b.front_width = wet * b.cell;
		b.front_speed = 0.5f * Math::abs(b.qs[r * nw + best] + b.qs[(r + 1) * nw + best]) / MAX(b.h[i], 0.05f);
		return;
	}
	b.front = b.center.is_empty() ? Vector3() : Vector3(b.center[0].x, b.bed[nw / 2], b.center[0].z);
	b.front_index = 0.0f;
}

void GroundDebrisFlow3D::_deposit(Body &p_body) {
	Body &b = p_body;
	if (b.deposited) {
		return;
	}
	b.deposited = true;
	GroundSurface3D *gs = GroundSurface3D::get_active();
	Ref<WorldWater> water;
	if (WorldContracts *wc = WorldContracts::get_singleton()) {
		water = wc->get_water();
	}
	TerrainDeformer3D *deformer = terrain_deformer.is_empty() ? nullptr : Object::cast_to<TerrainDeformer3D>(get_node_or_null(terrain_deformer));
	const int nw = b.nw;
	for (int r = 0; r < b.ns; r++) {
		// Runs of deposit across each row: one raised strip each on the terrain.
		int run_start = -1;
		float run_sum = 0.0f;
		for (int c = 0; c <= nw; c++) {
			const float h = c < nw ? b.h[r * nw + c] : 0.0f;
			if (c < nw && h > 0.02f) {
				const Vector3 p = _cell_position(b, r, c);
				const float wd = water.is_valid() ? water->depth_at(p) : 0.0f;
				if (wd > 0.1f) {
					// Into a river or lake: its solids as sediment (the call takes kg/s: here, over one second).
					water->add_sediment(p, h * b.cell * b.cell * sediment_share * 2650.0f, 0.0f);
				} else if (gs) {
					gs->add_mud_deposit(p, 0.75f * b.cell, h, false);
				}
				if (run_start < 0) {
					run_start = c;
					run_sum = 0.0f;
				}
				run_sum += h;
				continue;
			}
			if (run_start >= 0 && deformer) {
				const int len = c - run_start;
				const float mid = 0.5f * float(run_start + c - 1);
				const Vector3 p = b.center[r] + b.normal[r] * ((mid - 0.5f * float(nw - 1)) * b.cell);
				TerrainBrush3D *brush = memnew(TerrainBrush3D);
				brush->set_shape(TerrainBrush3D::SHAPE_RECTANGLE);
				brush->set_mode(TerrainBrush3D::MODE_ADD);
				brush->set_size(Vector2(len * b.cell, b.cell * 1.1f));
				brush->set_depth(-run_sum / len);
				brush->set_falloff(0.5f * b.cell);
				deformer->add_child(brush);
				const Vector3 n = b.normal[r];
				brush->set_global_transform(Transform3D(Basis(Vector3(0, 1, 0), Math::atan2(n.z, -n.x)), Vector3(p.x, p.y, p.z)));
				deformer->add_brush(brush);
			}
			run_start = -1;
		}
	}
	if (deformer) {
		deformer->queue_apply();
	}
	if (gs) {
		gs->refresh_cells();
	}
	// The flow's own mesh stays: it is the deposit until the ground shows it.
}

// Smooth value noise in 0..1 on a unit lattice (hashed corners, smoothstep blend).
static float _noise(float p_x, float p_z) {
	auto hash = [](int x, int z) {
		uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return float((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
	};
	const int x0 = int(Math::floor(p_x));
	const int z0 = int(Math::floor(p_z));
	const float fx = Math::smoothstep(0.0f, 1.0f, p_x - float(x0));
	const float fz = Math::smoothstep(0.0f, 1.0f, p_z - float(z0));
	const float a = Math::lerp(hash(x0, z0), hash(x0 + 1, z0), fx);
	const float c = Math::lerp(hash(x0, z0 + 1), hash(x0 + 1, z0 + 1), fx);
	return Math::lerp(a, c, fz);
}

void GroundDebrisFlow3D::_update_mesh(Body &p_body) {
	Body &b = p_body;
	if (!show_flow) {
		if (b.mesh) {
			b.mesh->set_visible(false);
		}
		return;
	}
	if (!b.mesh) {
		b.mesh = memnew(MeshInstance3D);
		b.mesh->set_as_top_level(true);
		add_child(b.mesh, false, INTERNAL_MODE_BACK);
		b.mesh->set_global_transform(Transform3D());
	}
	if (flow_material.is_null()) {
		// Wet post-fire slurry: dark with ash, a water film's gloss.
		flow_material.instantiate();
		flow_material->set_albedo(Color(0.15f, 0.12f, 0.095f));
		flow_material->set_roughness(0.4f);
		flow_material->set_specular(0.5f);
		flow_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	}
	const int nw = b.nw;
	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);
	st->set_smooth_group(0);
	// The sheet's margin follows the 1 cm contour (marching triangles: each cell triangle is clipped where the depth
	// crosses it), so the edge runs smoothly across the lattice instead of in 1.5 m steps.
	const float wet = 0.01f;
	struct V {
		Vector3 ground;
		float h;
	};
	auto emit = [&](const V &v) {
		const Vector3 &p = v.ground;
		// No flat sheet: ash streaks at about 4 m and 1.3 m, and thin margins paler where the water film has drained.
		const float n = 0.65f * _noise(p.x / 4.0f, p.z / 4.0f) + 0.35f * _noise(p.x / 1.3f + 17.0f, p.z / 1.3f + 5.0f);
		const float thin = 1.0f - CLAMP(v.h / 0.3f, 0.0f, 1.0f);
		const float k = 0.75f + 0.5f * n;
		st->set_color(Color(k * (1.0f + 0.7f * thin), k * (1.0f + 0.6f * thin), k * (1.0f + 0.45f * thin)));
		st->add_vertex(Vector3(p.x, p.y + v.h, p.z));
	};
	// The drawn depth is lightly smoothed over the cell's neighbours, so the sheet has no row-by-row steps.
	LocalVector<float> hd;
	hd.resize(b.h.size());
	for (int r = 0; r < b.ns; r++) {
		for (int c = 0; c < nw; c++) {
			const int i = r * nw + c;
			// The corridor's outer columns draw dry, so a flow that reaches the corridor's side tapers to the ground.
			if (c == 0 || c == nw - 1) {
				hd[i] = 0.0f;
				continue;
			}
			float sum = 0.0f;
			int cnt = 0;
			for (int dr = -1; dr <= 1; dr++) {
				for (int dc = -1; dc <= 1; dc++) {
					const int rr = r + dr;
					if (rr < 0 || rr >= b.ns || (dr == 0 && dc == 0)) {
						continue;
					}
					sum += b.h[rr * nw + c + dc];
					cnt++;
				}
			}
			hd[i] = 0.5f * b.h[i] + 0.5f * sum / MAX(cnt, 1);
		}
	}
	auto corner = [&](int r, int c) {
		return V{ _cell_position(b, r, c), hd[r * nw + c] };
	};
	bool any = false;
	auto triangle = [&](const V &a, const V &bb, const V &c) {
		const V in[3] = { a, bb, c };
		V out[4];
		int n = 0;
		for (int k = 0; k < 3; k++) {
			const V &p = in[k];
			const V &q = in[(k + 1) % 3];
			const bool pin = p.h >= wet;
			const bool qin = q.h >= wet;
			if (pin) {
				out[n++] = p;
			}
			if (pin != qin) {
				const float t = (wet - p.h) / (q.h - p.h);
				out[n++] = V{ p.ground.lerp(q.ground, t), wet };
			}
		}
		for (int k = 1; k + 1 < n; k++) {
			emit(out[0]);
			emit(out[k]);
			emit(out[k + 1]);
			any = true;
		}
	};
	for (int r = 0; r + 1 < b.ns; r++) {
		for (int c = 0; c + 1 < nw; c++) {
			const int i = r * nw + c;
			if (hd[i] < wet && hd[i + 1] < wet && hd[i + nw] < wet && hd[i + nw + 1] < wet) {
				continue;
			}
			const V v00 = corner(r, c);
			const V v10 = corner(r + 1, c);
			const V v01 = corner(r, c + 1);
			const V v11 = corner(r + 1, c + 1);
			triangle(v00, v10, v01);
			triangle(v01, v10, v11);
		}
	}
	if (!any) {
		b.mesh->set_mesh(Ref<Mesh>());
		return;
	}
	st->generate_normals();
	Ref<ArrayMesh> m = st->commit();
	m->surface_set_material(0, flow_material);
	b.mesh->set_mesh(m);
	b.mesh->set_visible(true);
}

void GroundDebrisFlow3D::advance(float p_seconds, float p_step) {
	float t = 0.0f;
	while (t < p_seconds) {
		const float dt = MIN(p_step, p_seconds - t);
		for (Body *b : bodies) {
			if (b->done) {
				continue;
			}
			// Substeps within the wave speed's limit (CFL 0.5).
			float left = dt * time_scale;
			while (left > 0.0f) {
				float hmax = 0.01f;
				for (float h : b->h) {
					hmax = MAX(hmax, h);
				}
				const float sub = MIN(left, 0.5f * b->cell / (2.0f * Math::sqrt(G * hmax)));
				_step(*b, sub);
				left -= sub;
			}
			_update_front(*b);
			// Done once the feeds have ended and nothing has moved for five seconds.
			float qmax = 0.0f;
			for (float q : b->qs) {
				qmax = MAX(qmax, Math::abs(q));
			}
			for (float q : b->qn) {
				qmax = MAX(qmax, Math::abs(q));
			}
			bool feeding = false;
			for (const Body::Feed &f : b->feeds) {
				feeding = feeding || _feed_rate(f.volume, b->time - f.start) > 0.0f || b->time < f.start;
			}
			b->still = (!feeding && qmax < 0.002f) ? b->still + dt * time_scale : 0.0f;
			if (b->still > 5.0f) {
				b->done = true;
				_deposit(*b);
			}
			b->mesh_accum += dt;
		}
		t += dt;
	}
	// Mesh and one front signal per body per call, after the last substep, so listeners (PhysXDebrisSurge3D) read
	// a settled front; the last one comes with done true. Events pooled into a body are signalled under their own
	// ids too.
	for (Body *b : bodies) {
		if (b->mesh_accum <= 0.0f) {
			continue;
		}
		b->mesh_accum = 0.0f;
		if (is_inside_tree()) {
			_update_mesh(*b);
		}
		for (int k = -1; k < (int)b->pooled.size(); k++) {
			const int64_t id = k < 0 ? b->id : b->pooled[k];
			emit_signal(SNAME("debris_front_changed"), id);
			if (b->done) {
				emit_signal(SNAME("debris_flow_finished"), id);
			}
		}
	}
}

Dictionary GroundDebrisFlow3D::get_debris_front(int64_t p_event_id) const {
	Dictionary d;
	const Body *b = _find(p_event_id);
	if (!b) {
		d["done"] = true;
		return d;
	}
	if (b->front_depth > 0.0f) {
		d["position"] = b->front;
	}
	d["path_index"] = b->front_index;
	d["depth"] = b->front_depth;
	d["speed"] = b->front_speed;
	d["width"] = b->front_width;
	float v = 0.0f;
	for (float h : b->h) {
		v += h;
	}
	d["volume"] = v * b->cell * b->cell;
	d["done"] = b->done;
	return d;
}

int GroundDebrisFlow3D::get_active_bodies() const {
	int n = 0;
	for (const Body *b : bodies) {
		n += b->done ? 0 : 1;
	}
	return n;
}

float GroundDebrisFlow3D::get_volume(int64_t p_event_id) const {
	const Body *b = _find(p_event_id);
	if (!b) {
		return 0.0f;
	}
	float v = 0.0f;
	for (float h : b->h) {
		v += h;
	}
	return v * b->cell * b->cell;
}

Dictionary GroundDebrisFlow3D::get_budget(int64_t p_event_id) const {
	Dictionary d;
	const Body *b = _find(p_event_id);
	if (!b) {
		return d;
	}
	d["in"] = b->volume_in;
	d["entrained"] = b->volume_entrained;
	d["out"] = b->volume_out;
	d["now"] = get_volume(p_event_id);
	d["time"] = b->time;
	return d;
}

static float _depth_in(const GroundDebrisFlow3D::Body &p_b, const Vector3 &p_position) {
	// The nearest cell: the row whose centre is nearest along, then across.
	int best = -1;
	float best_d = 1e30f;
	for (int r = 0; r < p_b.ns; r++) {
		const float d = Vector2(p_position.x - p_b.center[r].x, p_position.z - p_b.center[r].z).length_squared();
		if (d < best_d) {
			best_d = d;
			best = r;
		}
	}
	if (best < 0) {
		return 0.0f;
	}
	const Vector3 rel = p_position - p_b.center[best];
	const float off = rel.x * p_b.normal[best].x + rel.z * p_b.normal[best].z;
	const int c = int(Math::round(off / p_b.cell + 0.5f * float(p_b.nw - 1)));
	if (c < 0 || c >= p_b.nw) {
		return 0.0f;
	}
	const Vector3 along = Vector3(p_b.normal[best].z, 0.0f, -p_b.normal[best].x);
	if (Math::abs(rel.x * along.x + rel.z * along.z) > p_b.cell) {
		return 0.0f;
	}
	return p_b.h[best * p_b.nw + c];
}

float GroundDebrisFlow3D::get_deposit_depth_at(const Vector3 &p_position) const {
	float d = 0.0f;
	for (const Body *b : bodies) {
		if (b->done) {
			d = MAX(d, _depth_in(*b, p_position));
		}
	}
	return d;
}

float GroundDebrisFlow3D::get_flow_depth_at(const Vector3 &p_position) const {
	float d = 0.0f;
	for (const Body *b : bodies) {
		if (!b->done) {
			d = MAX(d, _depth_in(*b, p_position));
		}
	}
	return d;
}

void GroundDebrisFlow3D::clear() {
	for (Body *b : bodies) {
		if (b->mesh) {
			b->mesh->queue_free();
		}
		memdelete(b);
	}
	bodies.clear();
}

void GroundDebrisFlow3D::_on_debris_flow(const Dictionary &p_event) {
	start(p_event);
}

void GroundDebrisFlow3D::_connect() {
	if (connected || !listen) {
		return;
	}
	WorldContracts *wc = WorldContracts::get_singleton();
	if (!wc) {
		return;
	}
	Ref<WorldGround> g = wc->get_ground();
	if (g.is_valid()) {
		const Callable cb = callable_mp(this, &GroundDebrisFlow3D::_on_debris_flow);
		if (!g->is_connected(SNAME("debris_flow"), cb)) {
			g->connect(SNAME("debris_flow"), cb);
		}
		connected = true;
	}
}

void GroundDebrisFlow3D::_disconnect() {
	if (connected) {
		if (WorldContracts *wc = WorldContracts::get_singleton()) {
			Ref<WorldGround> g = wc->get_ground();
			const Callable cb = callable_mp(this, &GroundDebrisFlow3D::_on_debris_flow);
			if (g.is_valid() && g->is_connected(SNAME("debris_flow"), cb)) {
				g->disconnect(SNAME("debris_flow"), cb);
			}
		}
		connected = false;
	}
}

void GroundDebrisFlow3D::set_listen(bool p_v) {
	listen = p_v;
	if (!listen) {
		_disconnect();
	}
}

void GroundDebrisFlow3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_ENTER_TREE: {
			if (!Engine::get_singleton()->is_editor_hint()) {
				_connect();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_disconnect();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (Engine::get_singleton()->is_editor_hint()) {
				return;
			}
			_connect();
			if (get_active_bodies() > 0) {
				advance((float)get_physics_process_delta_time(), (float)get_physics_process_delta_time());
			}
		} break;
	}
}

void GroundDebrisFlow3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "event"), &GroundDebrisFlow3D::start);
	ClassDB::bind_method(D_METHOD("clear"), &GroundDebrisFlow3D::clear);
	ClassDB::bind_method(D_METHOD("advance", "seconds", "step"), &GroundDebrisFlow3D::advance, DEFVAL(0.05));
	ClassDB::bind_method(D_METHOD("get_debris_front", "event_id"), &GroundDebrisFlow3D::get_debris_front);
	ClassDB::bind_method(D_METHOD("get_body_count"), &GroundDebrisFlow3D::get_body_count);
	ClassDB::bind_method(D_METHOD("get_active_bodies"), &GroundDebrisFlow3D::get_active_bodies);
	ClassDB::bind_method(D_METHOD("get_volume", "event_id"), &GroundDebrisFlow3D::get_volume);
	ClassDB::bind_method(D_METHOD("get_budget", "event_id"), &GroundDebrisFlow3D::get_budget);
	ClassDB::bind_method(D_METHOD("get_deposit_depth_at", "position"), &GroundDebrisFlow3D::get_deposit_depth_at);
	ClassDB::bind_method(D_METHOD("get_flow_depth_at", "position"), &GroundDebrisFlow3D::get_flow_depth_at);

	ClassDB::bind_method(D_METHOD("set_listen", "listen"), &GroundDebrisFlow3D::set_listen);
	ClassDB::bind_method(D_METHOD("is_listening"), &GroundDebrisFlow3D::is_listening);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &GroundDebrisFlow3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &GroundDebrisFlow3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_corridor_width", "width"), &GroundDebrisFlow3D::set_corridor_width);
	ClassDB::bind_method(D_METHOD("get_corridor_width"), &GroundDebrisFlow3D::get_corridor_width);
	ClassDB::bind_method(D_METHOD("set_voellmy_mu", "mu"), &GroundDebrisFlow3D::set_voellmy_mu);
	ClassDB::bind_method(D_METHOD("get_voellmy_mu"), &GroundDebrisFlow3D::get_voellmy_mu);
	ClassDB::bind_method(D_METHOD("set_voellmy_xi", "xi"), &GroundDebrisFlow3D::set_voellmy_xi);
	ClassDB::bind_method(D_METHOD("get_voellmy_xi"), &GroundDebrisFlow3D::get_voellmy_xi);
	ClassDB::bind_method(D_METHOD("set_yield_stress", "pa"), &GroundDebrisFlow3D::set_yield_stress);
	ClassDB::bind_method(D_METHOD("get_yield_stress"), &GroundDebrisFlow3D::get_yield_stress);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &GroundDebrisFlow3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &GroundDebrisFlow3D::get_density);
	ClassDB::bind_method(D_METHOD("set_entrainment", "rate"), &GroundDebrisFlow3D::set_entrainment);
	ClassDB::bind_method(D_METHOD("get_entrainment"), &GroundDebrisFlow3D::get_entrainment);
	ClassDB::bind_method(D_METHOD("set_max_scour", "depth"), &GroundDebrisFlow3D::set_max_scour);
	ClassDB::bind_method(D_METHOD("get_max_scour"), &GroundDebrisFlow3D::get_max_scour);
	ClassDB::bind_method(D_METHOD("set_pool_distance", "distance"), &GroundDebrisFlow3D::set_pool_distance);
	ClassDB::bind_method(D_METHOD("get_pool_distance"), &GroundDebrisFlow3D::get_pool_distance);
	ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &GroundDebrisFlow3D::set_time_scale);
	ClassDB::bind_method(D_METHOD("get_time_scale"), &GroundDebrisFlow3D::get_time_scale);
	ClassDB::bind_method(D_METHOD("set_terrain_deformer", "path"), &GroundDebrisFlow3D::set_terrain_deformer);
	ClassDB::bind_method(D_METHOD("get_terrain_deformer"), &GroundDebrisFlow3D::get_terrain_deformer);
	ClassDB::bind_method(D_METHOD("set_show_flow", "show"), &GroundDebrisFlow3D::set_show_flow);
	ClassDB::bind_method(D_METHOD("is_show_flow"), &GroundDebrisFlow3D::is_show_flow);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "listen"), "set_listen", "is_listening");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.5,5,0.1,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "corridor_width", PROPERTY_HINT_RANGE, "4,200,1,suffix:m"), "set_corridor_width", "get_corridor_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voellmy_mu", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_voellmy_mu", "get_voellmy_mu");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "voellmy_xi", PROPERTY_HINT_RANGE, "10,4000,10,suffix:m/s²"), "set_voellmy_xi", "get_voellmy_xi");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "yield_stress", PROPERTY_HINT_RANGE, "0,10000,10,suffix:Pa"), "set_yield_stress", "get_yield_stress");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "1000,2500,10,suffix:kg/m³"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "entrainment", PROPERTY_HINT_RANGE, "0,0.05,0.0001,suffix:1/m"), "set_entrainment", "get_entrainment");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_scour", PROPERTY_HINT_RANGE, "0,3,0.05,suffix:m"), "set_max_scour", "get_max_scour");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pool_distance", PROPERTY_HINT_RANGE, "0,200,1,suffix:m"), "set_pool_distance", "get_pool_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale", PROPERTY_HINT_RANGE, "0,100,0.1"), "set_time_scale", "get_time_scale");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_deformer", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDeformer3D"), "set_terrain_deformer", "get_terrain_deformer");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_flow"), "set_show_flow", "is_show_flow");

	ADD_SIGNAL(MethodInfo("debris_flow_started", PropertyInfo(Variant::INT, "event_id")));
	ADD_SIGNAL(MethodInfo("debris_front_changed", PropertyInfo(Variant::INT, "event_id")));
	ADD_SIGNAL(MethodInfo("debris_flow_finished", PropertyInfo(Variant::INT, "event_id")));
}

GroundDebrisFlow3D::GroundDebrisFlow3D() {
}

GroundDebrisFlow3D::~GroundDebrisFlow3D() {
	for (Body *b : bodies) {
		memdelete(b);
	}
	bodies.clear();
}
