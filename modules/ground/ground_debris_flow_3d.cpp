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
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/rigid_body_3d.h"
#include "scene/main/scene_tree.h"
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
	b->hmax.resize(n);
	b->grounded.resize(n);
	for (int r = 0; r < b->ns; r++) {
		for (int c = 0; c < b->nw; c++) {
			const int i = r * b->nw + c;
			const float off = (float(c) - 0.5f * float(b->nw - 1)) * b->cell;
			const Vector3 p = b->center[r] + b->normal[r] * off;
			b->bed[i] = _ground_height(Vector3(p.x, p.y + 50.0f, p.z));
			b->scoured[i] = 0.0f;
			b->h[i] = 0.0f;
			b->hmax[i] = 0.0f;
			b->grounded[i] = 0;
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

static int _cell_in(const GroundDebrisFlow3D::Body &p_b, const Vector3 &p_position) {
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
		return -1;
	}
	const Vector3 rel = p_position - p_b.center[best];
	const float off = rel.x * p_b.normal[best].x + rel.z * p_b.normal[best].z;
	const int c = int(Math::round(off / p_b.cell + 0.5f * float(p_b.nw - 1)));
	if (c < 0 || c >= p_b.nw) {
		return -1;
	}
	const Vector3 along = Vector3(p_b.normal[best].z, 0.0f, -p_b.normal[best].x);
	if (Math::abs(rel.x * along.x + rel.z * along.z) > p_b.cell) {
		return -1;
	}
	return best * p_b.nw + c;
}

// The depth-averaged velocity in cell p_i: the mean of its faces' fluxes over its depth, along the path and across.
static Vector3 _velocity_of(const GroundDebrisFlow3D::Body &p_b, int p_i) {
	const int nw = p_b.nw;
	const int r = p_i / nw;
	const int c = p_i % nw;
	const float h = p_b.h[p_i];
	if (h < 0.01f) {
		return Vector3();
	}
	const float qs = 0.5f * (p_b.qs[r * nw + c] + p_b.qs[(r + 1) * nw + c]);
	const float qn = 0.5f * (p_b.qn[r * (nw + 1) + c] + p_b.qn[r * (nw + 1) + c + 1]);
	// Positive qs runs towards the next row, positive qn towards +normal.
	Vector3 down = r + 1 < p_b.ns ? p_b.center[r + 1] - p_b.center[r] : p_b.center[r] - p_b.center[MAX(r - 1, 0)];
	down.y = 0.0f;
	down = down.length_squared() > 1e-8f ? down.normalized() : Vector3();
	return (down * qs + p_b.normal[r] * qn) / MAX(h, 0.05f);
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

// Wet post-fire slurry: dark with ash, a water film's gloss.
static const Color SLURRY_ALBEDO(0.2f, 0.16f, 0.125f);

// The sheet's tint at p over slurry p_depth deep. No flat sheet: ash streaks at about 4 m and 1.3 m, and thin margins
// paler where the water film has drained. The deposit the ground takes over carries the same tint.
static Color _ash_tint(const Vector3 &p, float p_depth) {
	const float n = 0.65f * _noise(p.x / 4.0f, p.z / 4.0f) + 0.35f * _noise(p.x / 1.3f + 17.0f, p.z / 1.3f + 5.0f);
	const float thin = 1.0f - CLAMP(p_depth / 0.3f, 0.0f, 1.0f);
	const float k = 0.75f + 0.5f * n;
	return Color(k * (1.0f + 0.7f * thin), k * (1.0f + 0.6f * thin), k * (1.0f + 0.45f * thin));
}

// Levees (Johnson et al. 2012, "Grain-size segregation and levee formation in geophysical mass flows"; Iverson 1997):
// the coarse grains rise to the surge's top and are shouldered to its margins, where they stop first and stand as
// ridges while the finer core drains on down the channel. Each cell on the edge of the ground the flow covered (it
// ran there at its peak, beside a cell it never reached) keeps a ridge about half that peak depth high, varied along
// its length (no two levees alike); the volume comes off the rest of the deposit evenly, so none is made.
void GroundDebrisFlow3D::_levees(Body &p_body) {
	Body &b = p_body;
	const int nw = b.nw;
	const int ns = b.ns;
	LocalVector<float> add;
	add.resize(b.h.size());
	for (float &a : add) {
		a = 0.0f;
	}
	float v_add = 0.0f;
	for (int r = 0; r < ns; r++) {
		for (int c = 1; c + 1 < nw; c++) {
			const int i = r * nw + c;
			add[i] = 0.0f;
			if (b.hmax[i] < 0.1f) {
				continue;
			}
			const bool edge = b.hmax[i - 1] < 0.01f || b.hmax[i + 1] < 0.01f || (r > 0 && b.hmax[i - nw] < 0.01f) || (r + 1 < ns && b.hmax[i + nw] < 0.01f);
			if (!edge) {
				continue;
			}
			// Only along a margin the flow held (most of the cell's neighbours ran too), against the local peak
			// averaged over them: a splash into one cell builds no ridge.
			float peak = 0.0f;
			int ran = 0;
			int cnt = 0;
			for (int dr = -1; dr <= 1; dr++) {
				for (int dc = -1; dc <= 1; dc++) {
					const int rr = r + dr;
					if (rr < 0 || rr >= ns) {
						continue;
					}
					const float hm = b.hmax[rr * nw + c + dc];
					peak += hm;
					cnt++;
					ran += hm >= 0.1f ? 1 : 0;
				}
			}
			if (ran < 4) {
				continue;
			}
			peak /= MAX(cnt, 1);
			const Vector3 p = _cell_position(b, r, c);
			const float vary = 0.7f + 0.6f * (0.65f * _noise(p.x / 6.0f, p.z / 6.0f) + 0.35f * _noise(p.x / 2.0f + 9.0f, p.z / 2.0f + 3.0f));
			const float ridge = MIN(levee_height * peak * vary, 1.0f);
			if (ridge > b.h[i]) {
				add[i] = ridge - b.h[i];
				v_add += add[i];
			}
		}
	}
	float v_rest = 0.0f;
	for (uint32_t i = 0; i < b.h.size(); i++) {
		if (add[i] == 0.0f && b.h[i] > 0.02f) {
			v_rest += b.h[i];
		}
	}
	if (v_add <= 0.0f || v_rest <= 0.0f) {
		return;
	}
	// At most half the deposit goes into its levees.
	const float scale = MIN(1.0f, 0.5f * v_rest / v_add);
	const float take = 1.0f - scale * v_add / v_rest;
	for (uint32_t i = 0; i < b.h.size(); i++) {
		if (add[i] > 0.0f) {
			b.h[i] += scale * add[i];
		} else if (b.h[i] > 0.02f) {
			b.h[i] *= take;
		}
	}
}

// A body's local bounds from its collision shapes (their debug meshes' boxes), kept per body.
AABB GroundDebrisFlow3D::_body_box(RigidBody3D *p_body) {
	const ObjectID id = p_body->get_instance_id();
	if (const AABB *a = body_boxes.getptr(id)) {
		return *a;
	}
	AABB box;
	bool any = false;
	for (int k = 0; k < p_body->get_child_count(); k++) {
		CollisionShape3D *cs = Object::cast_to<CollisionShape3D>(p_body->get_child(k));
		if (!cs || cs->get_shape().is_null()) {
			continue;
		}
		Ref<ArrayMesh> m = cs->get_shape()->get_debug_mesh();
		if (m.is_null()) {
			continue;
		}
		const AABB a = cs->get_transform().xform(m->get_aabb());
		box = any ? box.merge(a) : a;
		any = true;
	}
	if (!any) {
		box = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));
	}
	body_boxes.insert(id, box);
	return box;
}

// The flow shoves what stands in it: a drag of rho Cd A u^2 / 2 on each rigid body in the deform groups (vehicles,
// props), over its width and the part of its height the flow covers (Cd 1.2, a bluff body; Zanchetta et al. 2004 and
// Kang and Kim 2016 use the same hydrodynamic term for debris-flow impact on obstacles).
void GroundDebrisFlow3D::_push_bodies(Body &p_body, float p_dt) {
	if (p_dt <= 0.0f || !push_bodies) {
		return;
	}
	GroundSurface3D *gs = GroundSurface3D::get_active();
	PackedStringArray groups = gs ? gs->get_deform_groups() : PackedStringArray();
	if (groups.is_empty()) {
		groups.push_back("ground_bodies");
	}
	for (const String &g : groups) {
		for (Node *nd : get_tree()->get_nodes_in_group(StringName(g))) {
			RigidBody3D *rb = Object::cast_to<RigidBody3D>(nd);
			if (!rb || rb->is_freeze_enabled()) {
				continue;
			}
			const Vector3 at = rb->get_global_position();
			const int i = _cell_in(p_body, at);
			if (i < 0 || p_body.h[i] < 0.05f) {
				continue;
			}
			const Vector3 u = _velocity_of(p_body, i);
			const Vector3 rel = u - Vector3(rb->get_linear_velocity().x, 0.0f, rb->get_linear_velocity().z);
			const float speed = rel.length();
			if (speed < 0.05f) {
				continue;
			}
			// The body's frontal area in the flow: its box's width times the flow depth over its base.
			const AABB box = rb->get_global_transform().xform(_body_box(rb));
			const float base = box.position.y;
			const float wet = CLAMP(p_body.bed[i] + p_body.h[i] - base, 0.0f, box.size.y);
			if (wet <= 0.0f) {
				continue;
			}
			const float width = 0.5f * (box.size.x + box.size.z);
			// Drag only brings the body up to the flow's speed: over a long step it would overshoot.
			const float j = MIN(0.5f * density * 1.2f * width * wet * speed * speed * p_dt, rb->get_mass() * speed);
			rb->apply_central_impulse(rel / speed * j);
		}
	}
}

// The deposit and the position at fractional cell (p_r, p_c), bilinear between the four cells around it.
float GroundDebrisFlow3D::_grid_sample(const Body &p_body, float p_r, float p_c, Vector3 &r_position) const {
	const Body &b = p_body;
	const float rr = CLAMP(p_r, 0.0f, float(b.ns - 1));
	const float cc = CLAMP(p_c, 0.0f, float(b.nw - 1));
	const int r0 = MIN(int(rr), b.ns - 2);
	const int c0 = MIN(int(cc), b.nw - 2);
	const float fr = rr - float(r0);
	const float fc = cc - float(c0);
	auto at = [&](int r, int c) { return b.h[r * b.nw + c]; };
	const float h = Math::lerp(Math::lerp(at(r0, c0), at(r0, c0 + 1), fc), Math::lerp(at(r0 + 1, c0), at(r0 + 1, c0 + 1), fc), fr);
	const Vector3 a = _cell_position(b, r0, c0).lerp(_cell_position(b, r0, c0 + 1), fc);
	const Vector3 d = _cell_position(b, r0 + 1, c0).lerp(_cell_position(b, r0 + 1, c0 + 1), fc);
	r_position = a.lerp(d, fr);
	return h;
}

void GroundDebrisFlow3D::_deposit(Body &p_body) {
	Body &b = p_body;
	if (b.deposited) {
		return;
	}
	b.deposited = true;
	_levees(b);
	GroundSurface3D *gs = GroundSurface3D::get_active();
	// Near the camera the print field takes the deposit as ground (ruts and prints under it fill, the surface rises),
	// so the cells there are drawn by the ground, not the flow's sheet.
	// Inside the field's faded edge only, so the ground and the sheet meet level.
	const Rect2 field_rect = gs ? gs->get_deform_rect().grow(-4.0f) : Rect2();
	Ref<WorldWater> water;
	if (WorldContracts *wc = WorldContracts::get_singleton()) {
		water = wc->get_water();
	}
	TerrainDeformer3D *deformer = terrain_deformer.is_empty() ? nullptr : Object::cast_to<TerrainDeformer3D>(get_node_or_null(terrain_deformer));
	const int nw = b.nw;
	HashMap<Vector2i, Vector2> fan; // 1 m cell: depth sum, samples
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
					// Into a river or lake: its solids as sediment, in kg.
					water->add_sediment_mass(p, h * b.cell * b.cell * sediment_share * 2650.0f, 0.0f);
				} else if (gs) {
					// The deposit goes to the ground's 1 m cells as the mean of 3 x 3 bilinear samples per flow cell,
					// so the fan's surface carries neither this lattice nor that one.
					for (int kr = 0; kr < 3; kr++) {
						for (int kc = 0; kc < 3; kc++) {
							Vector3 q;
							const float hq = _grid_sample(b, float(r) + (float(kr) - 1.0f) / 3.0f, float(c) + (float(kc) - 1.0f) / 3.0f, q);
							const Vector2i k(int(Math::floor(q.x)), int(Math::floor(q.z)));
							Vector2 *acc = fan.getptr(k);
							if (acc) {
								*acc += Vector2(hq, 1.0f);
							} else {
								fan.insert(k, Vector2(hq, 1.0f));
							}
						}
					}
					if (field_rect.has_area() && field_rect.has_point(Vector2(p.x, p.z))) {
						// The deposit becomes the field's ground (GroundSurface3D::refresh_deposits) and the ground draws
						// it there instead of the sheet.
						constexpr int K = 3;
						const float rad = b.cell / float(K) / Math::sqrt((float)Math::PI);
						for (int kr = 0; kr < K; kr++) {
							for (int kc = 0; kc < K; kc++) {
								const float fr = float(r) + (float(kr) + 0.5f) / float(K) - 0.5f;
								const float fc = float(c) + (float(kc) + 0.5f) / float(K) - 0.5f;
								Vector3 q;
								const float hq = _grid_sample(b, fr, fc, q);
								if (hq > 0.005f) {
									// Whatever relief the ground had there (prints, ruts, their rims) is buried: hollows
									// filled, rims taken down, so the deposit's top is smooth.
									const float relief = gs->get_rim_at(q) - gs->get_dent_at(q);
									if (relief < -0.0005f) {
										gs->deposit(q, rad, -relief);
									} else if (relief > 0.0005f && relief < hq) {
										gs->dig(q, rad, relief);
									}
								}
							}
						}
						b.grounded[r * nw + c] = 1;
					}
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
		for (const KeyValue<Vector2i, Vector2> &kv : fan) {
			const float d = kv.value.x / MAX(kv.value.y, 1.0f);
			if (d > 0.01f) {
				// The ground shows it in the sheet's own colour (linear albedo times the vertex tint).
				const Vector3 q(float(kv.key.x) + 0.5f, 0.0f, float(kv.key.y) + 0.5f);
				const Color lin = SLURRY_ALBEDO.srgb_to_linear() * _ash_tint(q, d);
				gs->add_mud_deposit(q, 0.0f, d, false, Color(lin.r, lin.g, lin.b, 1.0f));
			}
		}
		gs->refresh_deposits();
	}
	// The flow's own mesh stays: it is the deposit until the ground shows it.
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
		flow_material->set_albedo(SLURRY_ALBEDO);
		flow_material->set_roughness(0.55f);
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
		float h; // drawn depth
		float t; // the slurry's own depth there, for the tint (a sheet tapering into ground that took it is not thin)
	};
	auto emit = [&](const V &v) {
		const Vector3 &p = v.ground;
		st->set_color(_ash_tint(p, v.t));
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
			// Cells the print field took over as ground: the ground draws them, raised by the deposit; the sheet
			// carries on 3 cm under that surface, hidden, so where the field's ground fades out (its outer metres,
			// GroundSurface3D::_apply_deposits()) the sheet is already there at full height, with no step.
			if (b.grounded[i]) {
				hd[i] = MAX(hd[i] - 0.03f, 0.0f);
			}
		}
	}
	auto corner = [&](int r, int c) {
		const int i = r * nw + c;
		return V{ _cell_position(b, r, c), hd[i], hd[i] + (b.grounded[i] ? 0.03f : 0.0f) };
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
				out[n++] = V{ p.ground.lerp(q.ground, t), wet, Math::lerp(p.t, q.t, t) };
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
			const bool edge = MIN(MIN(hd[i], hd[i + 1]), MIN(hd[i + nw], hd[i + nw + 1])) < wet;
			if (!edge) {
				triangle(v00, v10, v01);
				triangle(v01, v10, v11);
				continue;
			}
			// A margin cell: split 4 x 4 with the depth bilinear across it, so the 1 cm contour curves between the
			// lattice's points instead of running in straight 1.5 m facets (a saw-tooth seen from afar).
			const int S = 4;
			auto sub = [&](int a, int bb) {
				const float u = float(a) / S;
				const float w = float(bb) / S;
				V o;
				o.ground = v00.ground.lerp(v10.ground, u).lerp(v01.ground.lerp(v11.ground, u), w);
				o.h = Math::lerp(Math::lerp(v00.h, v10.h, u), Math::lerp(v01.h, v11.h, u), w);
				o.t = Math::lerp(Math::lerp(v00.t, v10.t, u), Math::lerp(v01.t, v11.t, u), w);
				return o;
			};
			for (int a = 0; a < S; a++) {
				for (int bb = 0; bb < S; bb++) {
					const V s00 = sub(a, bb);
					const V s10 = sub(a + 1, bb);
					const V s01 = sub(a, bb + 1);
					const V s11 = sub(a + 1, bb + 1);
					triangle(s00, s10, s01);
					triangle(s01, s10, s11);
				}
			}
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
			for (uint32_t i = 0; i < b->h.size(); i++) {
				b->hmax[i] = MAX(b->hmax[i], b->h[i]);
			}
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
	// The shove on bodies standing in the flow, once a call (a force holds for one physics step).
	if (is_inside_tree()) {
		for (Body *b : bodies) {
			if (!b->done) {
				_push_bodies(*b, p_seconds * time_scale);
			}
		}
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

Vector3 GroundDebrisFlow3D::get_flow_velocity_at(const Vector3 &p_position) const {
	Vector3 v;
	for (const Body *b : bodies) {
		if (b->done) {
			continue;
		}
		const int i = _cell_in(*b, p_position);
		if (i >= 0 && b->h[i] > 0.01f) {
			const Vector3 u = _velocity_of(*b, i);
			if (u.length_squared() > v.length_squared()) {
				v = u;
			}
		}
	}
	return v;
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
			// Found by the ground's loose stones, which the flow carries (mud round 18.1).
			add_to_group(SNAME("ground_debris_flows"));
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
	ClassDB::bind_method(D_METHOD("get_flow_velocity_at", "position"), &GroundDebrisFlow3D::get_flow_velocity_at);

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
	ClassDB::bind_method(D_METHOD("set_levee_height", "share"), &GroundDebrisFlow3D::set_levee_height);
	ClassDB::bind_method(D_METHOD("get_levee_height"), &GroundDebrisFlow3D::get_levee_height);
	ClassDB::bind_method(D_METHOD("set_push_bodies", "push"), &GroundDebrisFlow3D::set_push_bodies);
	ClassDB::bind_method(D_METHOD("is_push_bodies"), &GroundDebrisFlow3D::is_push_bodies);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "levee_height", PROPERTY_HINT_RANGE, "0,1.5,0.05"), "set_levee_height", "get_levee_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "push_bodies"), "set_push_bodies", "is_push_bodies");

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
