/**************************************************************************/
/*  vegetation_trails.cpp                                                 */
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

#include "vegetation_field_3d.h"

#include "core/math/random_pcg.h"
#include "core/templates/sort_array.h"
#include "modules/terrain_deform/terrain_data_map_3d.h"

// Game trails (docs/ideas/vegetation.md, "Game trails"). Deer and other large herbivores wear narrow, lasting paths
// between the places they use: cover, feeding lawns, water, and fords and saddles between them. Trails form because
// each animal prefers ground already trodden (the active walker model: Helbing, Keltsch and Molnar 1997, Nature 388;
// Helbing, Schweitzer et al. 1997, Phys. Rev. E 56), so routes between many pairs of places merge into a branching network instead
// of a straight line each. Here: a least-cost route over a grid for every pair of goals within reach, nearest pairs
// first, each cell's cost lowered by the passes already routed over it (attraction), the cost raised by slope (deer
// contour round steep ground and take the shallowest line up; cost after Tobler-type walking functions), and by water
// (fords at the shallowest; deep water not crossed). A seeded roughness keeps the lines from running ruler-straight
// over even ground. The network comes out as worn paths (VegetationPaths: points, width, passes a year), a piece per
// stretch of equal traffic, so the fields' trampling rules (PlantSpecies.trample_resistance) wear it.
namespace {

struct TrailGrid {
	int w = 0;
	int h = 0;
	Vector2 origin;
	float cell = 2.0f;
	LocalVector<float> cost; // Base cost a cell (>= 1); 0 = impassable.
	LocalVector<float> flow; // Passes a year routed over it so far.

	int at(int x, int z) const { return z * w + x; }
	Vector2i cell_of(const Vector2 &p) const {
		return Vector2i(CLAMP(int((p.x - origin.x) / cell), 0, w - 1), CLAMP(int((p.y - origin.y) / cell), 0, h - 1));
	}
	Vector2 centre(int x, int z) const { return origin + Vector2((x + 0.5f) * cell, (z + 0.5f) * cell); }
};

struct OpenItem {
	float f;
	int idx;
};
struct OpenLess {
	bool operator()(const OpenItem &a, const OpenItem &b) const { return a.f > b.f; } // Min-heap.
};

float hash01(uint32_t x, uint32_t y, uint32_t s) {
	uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ s * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return float(h & 0xffffff) / float(0xffffff);
}

// Smooth value noise, 0..1.
float value_noise(float x, float y, uint32_t s) {
	const int xi = int(Math::floor(x));
	const int yi = int(Math::floor(y));
	const float fx = x - xi;
	const float fy = y - yi;
	const float u = fx * fx * (3.0f - 2.0f * fx);
	const float v = fy * fy * (3.0f - 2.0f * fy);
	const float a = hash01(xi, yi, s);
	const float b = hash01(xi + 1, yi, s);
	const float c = hash01(xi, yi + 1, s);
	const float d = hash01(xi + 1, yi + 1, s);
	return Math::lerp(Math::lerp(a, b, u), Math::lerp(c, d, u), v);
}

// A* from a to b; r_route the cells from a to b. Each step costs its length times the mean of the two cells' costs,
// eased where traffic already runs (attraction: a well-used trail costs down to 1 - p_attract of fresh ground).
bool route(const TrailGrid &g, const Vector2i &a, const Vector2i &b, float p_attract, float p_flow_ref, LocalVector<int> &r_route) {
	const int n = g.w * g.h;
	LocalVector<float> dist;
	dist.resize(n);
	LocalVector<int> from;
	from.resize(n);
	LocalVector<uint8_t> closed;
	closed.resize(n);
	for (int i = 0; i < n; i++) {
		dist[i] = 1e30f;
		from[i] = -1;
		closed[i] = 0;
	}
	auto eff = [&](int i) {
		const float worn = g.flow[i] / (g.flow[i] + p_flow_ref);
		return g.cost[i] * (1.0f - p_attract * worn);
	};
	LocalVector<OpenItem> open;
	const int ia = g.at(a.x, a.y);
	const int ib = g.at(b.x, b.y);
	if (g.cost[ia] <= 0.0f || g.cost[ib] <= 0.0f) {
		return false;
	}
	dist[ia] = 0.0f;
	// The heuristic: straight distance times the cheapest a step can cost (well-worn trail on level ground).
	const float hmin = 1.0f - p_attract;
	auto heur = [&](int x, int z) { return Vector2(x - b.x, z - b.y).length() * g.cell * hmin; };
	open.push_back({ heur(a.x, a.y), ia });
	SortArray<OpenItem, OpenLess> heap;
	static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	while (!open.is_empty()) {
		heap.pop_heap(0, open.size(), open.ptr());
		const OpenItem it = open[open.size() - 1];
		open.resize(open.size() - 1);
		if (closed[it.idx]) {
			continue;
		}
		closed[it.idx] = 1;
		if (it.idx == ib) {
			break;
		}
		const int x = it.idx % g.w;
		const int z = it.idx / g.w;
		const float ci = eff(it.idx);
		for (int k = 0; k < 8; k++) {
			const int nx = x + DX[k];
			const int nz = z + DZ[k];
			if (nx < 0 || nz < 0 || nx >= g.w || nz >= g.h) {
				continue;
			}
			const int j = g.at(nx, nz);
			if (closed[j] || g.cost[j] <= 0.0f) {
				continue;
			}
			const float step = (k < 4 ? 1.0f : Math::SQRT2) * g.cell * 0.5f * (ci + eff(j));
			const float nd = dist[it.idx] + step;
			if (nd < dist[j]) {
				dist[j] = nd;
				from[j] = it.idx;
				open.push_back({ nd + heur(nx, nz), j });
				heap.push_heap(0, open.size() - 1, 0, open[open.size() - 1], open.ptr());
			}
		}
	}
	if (from[ib] < 0 && ia != ib) {
		return false;
	}
	r_route.clear();
	for (int i = ib; i >= 0; i = from[i]) {
		r_route.push_back(i);
		if (i == ia) {
			break;
		}
	}
	r_route.reverse();
	return true;
}

} // namespace

Array VegetationField3D::make_game_trails(Node *p_data_map, const Rect2 &p_area, const PackedVector3Array &p_goals, const Dictionary &p_options) {
	Array out;
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(p_data_map);
	ERR_FAIL_NULL_V_MSG(map, out, "make_game_trails needs a TerrainDataMap3D.");
	ERR_FAIL_COND_V(p_area.size.x <= 0.0f || p_area.size.y <= 0.0f, out);
	const float cell = MAX(float(p_options.get("cell", 2.0f)), 0.5f);
	const float reach = float(p_options.get("reach", 250.0f)); // Goals further apart are not linked directly.
	const float passes = float(p_options.get("passes", 1500.0f)); // A year, along one link (a small herd's daily round).
	const float width = float(p_options.get("width", 0.35f)); // The tread of a single-file trail.
	const float attract = CLAMP(float(p_options.get("attraction", 0.6f)), 0.0f, 0.9f);
	const float max_slope = Math::deg_to_rad(float(p_options.get("max_slope_deg", 40.0f)));
	const float ford_depth = float(p_options.get("ford_depth", 0.4f));
	const float rough = float(p_options.get("roughness", 0.35f));
	const uint32_t seed = uint32_t(int(p_options.get("seed", 1)));

	TrailGrid g;
	g.cell = cell;
	g.origin = p_area.position;
	g.w = MAX(int(Math::ceil(p_area.size.x / cell)), 2);
	g.h = MAX(int(Math::ceil(p_area.size.y / cell)), 2);
	ERR_FAIL_COND_V_MSG(int64_t(g.w) * g.h > 4000000, out, "make_game_trails: area over 4 M cells; raise options.cell.");
	g.cost.resize(g.w * g.h);
	g.flow.resize(g.w * g.h);
	for (int z = 0; z < g.h; z++) {
		for (int x = 0; x < g.w; x++) {
			const Vector2 c = g.centre(x, z);
			const Vector3 p(c.x, 0.0f, c.y);
			const int i = g.at(x, z);
			g.flow[i] = 0.0f;
			const float slope = map->get_slope_at(p); // Radians.
			const float depth = map->get_water_depth_at(p);
			if (slope > max_slope || depth > ford_depth) {
				g.cost[i] = 0.0f;
				continue;
			}
			// Steepness: cheap up to about 10 degrees, then quickly dearer (Tobler 1993's hiking function falls off
			// past a 5% grade; ungulate trails keep under 20-25 degrees and contour round steeper ground).
			const float t = Math::tan(slope);
			float c0 = 1.0f + 6.0f * t * t + 30.0f * MAX(t - 0.45f, 0.0f) * MAX(t - 0.45f, 0.0f);
			if (depth > 0.0f) {
				c0 *= 1.0f + 8.0f * depth / MAX(ford_depth, 0.01f); // Wade only at the shallows.
			}
			// Roughness: logs, tussocks, soft ground the map does not hold, as smooth noise at two scales.
			const float nz = 0.65f * value_noise(c.x / 9.0f, c.y / 9.0f, seed) + 0.35f * value_noise(c.x / 3.5f, c.y / 3.5f, seed + 7u);
			c0 *= 1.0f + rough * nz;
			g.cost[i] = c0;
		}
	}
	// The links: every pair within reach, nearest first (short links set the network the long ones then follow).
	struct Link {
		int a, b;
		float d;
	};
	LocalVector<Link> links;
	for (int i = 0; i < p_goals.size(); i++) {
		for (int j = i + 1; j < p_goals.size(); j++) {
			const float d = Vector2(p_goals[i].x - p_goals[j].x, p_goals[i].z - p_goals[j].z).length();
			if (d <= reach) {
				links.push_back({ i, j, d });
			}
		}
	}
	struct LinkLess {
		bool operator()(const Link &a, const Link &b) const { return a.d < b.d; }
	};
	links.sort_custom<LinkLess>();
	LocalVector<LocalVector<int>> routes;
	LocalVector<int> r;
	for (const Link &l : links) {
		const Vector2i a = g.cell_of(Vector2(p_goals[l.a].x, p_goals[l.a].z));
		const Vector2i b = g.cell_of(Vector2(p_goals[l.b].x, p_goals[l.b].z));
		if (!route(g, a, b, attract, passes, r) || r.size() < 2) {
			continue;
		}
		for (int i : r) {
			g.flow[i] += passes;
		}
		routes.push_back(LocalVector<int>());
		routes[routes.size() - 1] = r;
	}
	// Out as paths: each route cut where its traffic changes (where other routes join or leave), so a stretch shared
	// by several links carries their summed passes (VegetationPaths takes the busiest path at a point).
	// A stretch shared by several routes goes out once.
	LocalVector<uint8_t> emitted;
	emitted.resize(g.w * g.h);
	for (int i = 0; i < g.w * g.h; i++) {
		emitted[i] = 0;
	}
	for (const LocalVector<int> &rt : routes) {
		PackedVector3Array pts;
		LocalVector<int> cells;
		float run_flow = g.flow[rt[0]];
		auto flush = [&](float p_flow) {
			bool fresh = false;
			for (int c : cells) {
				fresh = fresh || !emitted[c];
				emitted[c] = 1;
			}
			if (pts.size() >= 2 && fresh) {
				// Chaikin corner cutting, twice: the grid's 45-degree steps into a walked curve, the ends kept.
				for (int it = 0; it < 2; it++) {
					PackedVector3Array s;
					s.push_back(pts[0]);
					for (int k = 0; k + 1 < pts.size(); k++) {
						s.push_back(pts[k].lerp(pts[k + 1], 0.25f));
						s.push_back(pts[k].lerp(pts[k + 1], 0.75f));
					}
					s.push_back(pts[pts.size() - 1]);
					pts = s;
				}
				Dictionary d;
				d["points"] = pts;
				// Busier trails widen a little as animals pass each other (about 0.1 m for each tenfold).
				d["width"] = width * (1.0f + 0.3f * Math::log(MAX(p_flow / MAX(passes, 1.0f), 1.0f)) / Math::log(10.0f));
				d["passes"] = p_flow;
				d["kind"] = "game_trail";
				out.push_back(d);
			}
		};
		for (int k = 0; k < (int)rt.size(); k++) {
			const int i = rt[k];
			const Vector2 c = g.centre(i % g.w, i / g.w);
			const Vector3 p(c.x, map->get_height_at(Vector3(c.x, 0.0f, c.y)), c.y);
			if (g.flow[i] != run_flow && pts.size() > 0) {
				pts.push_back(p); // The joint belongs to both pieces.
				flush(run_flow);
				pts = PackedVector3Array();
				cells.clear();
				run_flow = g.flow[i];
			}
			pts.push_back(p);
			cells.push_back(i);
		}
		flush(run_flow);
	}
	return out;
}
