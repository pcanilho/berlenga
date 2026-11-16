/**************************************************************************/
/*  cave_network_3d.cpp                                                     */
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


#include "cave_network_3d.h"

#include "rock_field_3d.h"

#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/sort_array.h"
#include "modules/terrain_deform/terrain_erosion_filter.h"
#include "modules/terrain_deform/terrain_generator_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#include "scene/resources/multimesh.h"

TerrainGenerator3D *CaveNetwork3D::_get_generator() const {
	if (generator_path.is_empty() || !is_inside_tree()) {
		return nullptr;
	}
	return Object::cast_to<TerrainGenerator3D>(get_node_or_null(generator_path));
}

namespace {

struct CaveGraph {
	LocalVector<Vector3> p;
	LocalVector<LocalVector<int>> nbr;
	LocalVector<LocalVector<float>> cost;
};

// Least-cost paths from a set of sources (Dijkstra; the graph is small enough that A* buys little, and one search
// from a key point serves every pair it is in).
void cave_dijkstra(const CaveGraph &p_g, const LocalVector<int> &p_sources, LocalVector<float> &r_dist, LocalVector<int> &r_parent) {
	const int n = int(p_g.p.size());
	r_dist.resize(n);
	r_parent.resize(n);
	for (int i = 0; i < n; i++) {
		r_dist[i] = 1e30f;
		r_parent[i] = -1;
	}
	struct Item {
		float d;
		int i;
	};
	struct ItemLess {
		_FORCE_INLINE_ bool operator()(const Item &p_a, const Item &p_b) const { return p_a.d > p_b.d; } // a min-heap
	};
	LocalVector<Item> heap;
	for (int s : p_sources) {
		r_dist[s] = 0.0f;
		heap.push_back({ 0.0f, s });
	}
	SortArray<Item, ItemLess> sorter;
	sorter.make_heap(0, heap.size(), heap.ptr());
	while (!heap.is_empty()) {
		sorter.pop_heap(0, heap.size(), heap.ptr());
		const Item it = heap[heap.size() - 1];
		heap.resize(heap.size() - 1);
		if (it.d > r_dist[it.i]) {
			continue;
		}
		const LocalVector<int> &nb = p_g.nbr[it.i];
		const LocalVector<float> &cs = p_g.cost[it.i];
		for (uint32_t k = 0; k < nb.size(); k++) {
			const float d = it.d + cs[k];
			if (d < r_dist[nb[k]]) {
				r_dist[nb[k]] = d;
				r_parent[nb[k]] = it.i;
				heap.push_back({ d, nb[k] });
				sorter.push_heap(0, heap.size() - 1, 0, heap[heap.size() - 1], heap.ptr());
			}
		}
	}
}

} // namespace

bool CaveNetwork3D::_grow(Cave &r_cave, uint32_t p_seed) const {
	const TerrainGenerator3D *gen = _get_generator();
	if (!gen) {
		return false;
	}
	RandomPCG rng(p_seed);
	const Vector3 E = r_cave.entrance;
	Vector3 o = Vector3(r_cave.out.x, 0, r_cave.out.z);
	if (o.length_squared() < 1e-6f) {
		o = Vector3(0, 0, 1);
	}
	o.normalize();
	const Vector3 f = -o; // into the hill
	const Vector3 sd(-f.z, 0, f.x);
	const real_t L = cave_length;
	auto H = [&](real_t p_x, real_t p_z) -> real_t { return gen->get_height(Vector3(p_x, 0, p_z)); };
	auto dist_xz = [&](const Vector3 &p_p) -> real_t { return Vector2(p_p.x - E.x, p_p.z - E.z).length(); };
	// Rock over a passage: the least cover near the entrance, more further in (the plateau stands higher there).
	auto cover_at = [&](const Vector3 &p_p) -> real_t { return cover + MIN(real_t(0.08) * dist_xz(p_p), real_t(15.0)); };
	auto in_rock = [&](const Vector3 &p_p) -> bool { return p_p.y < H(p_p.x, p_p.z) - cover_at(p_p) && p_p.y > E.y - 60.0; };

	// Inception horizons: the bedding planes, dipping as the generator's strata do (else a gentle random dip), one
	// through the entrance's floor, every bedding_spacing above and below it, each wandering a little.
	real_t dip = Math::deg_to_rad(real_t(1.0) + real_t(3.0) * rng.randf()), dip_dir = rng.randf() * Math::TAU;
	if (gen->is_strata_enabled()) {
		dip = Math::deg_to_rad(gen->get_strata_dip());
		dip_dir = Math::deg_to_rad(gen->get_strata_dip_direction());
	}
	const Vector3 dd(Math::sin(dip_dir), 0, -Math::cos(dip_dir)); // clockwise from north (-z)
	const real_t tan_dip = Math::tan(dip);
	const uint32_t hseed = p_seed * 2654435761u + 7u;
	auto bed_q = [&](const Vector3 &p_p) -> real_t {
		return p_p.y - E.y + tan_dip * dd.dot(p_p - E) + 1.5 * (TerrainErosionFilter::fbm(hseed, p_p.x, p_p.z, 120.0, 2) - 0.5);
	};
	auto bed_dist = [&](const Vector3 &p_p) -> real_t {
		const real_t q = bed_q(p_p);
		return Math::abs(q - Math::round(q / bedding_spacing) * bedding_spacing);
	};
	// The water table: the spring at the entrance, rising gently into the hill.
	auto water = [&](const Vector3 &p_p) -> real_t { return E.y + 1.0 + 0.012 * dist_xz(p_p); };
	// Two joint sets, near orthogonal, vertical.
	const real_t ja = rng.randf() * Math::PI;
	const Vector3 n1(Math::cos(ja), 0, Math::sin(ja));
	const real_t jb = ja + Math::PI * 0.5 + (rng.randf() - 0.5f) * 0.35f;
	const Vector3 n2(Math::cos(jb), 0, Math::sin(jb));

	CaveGraph g;
	// Key points: the start of the passage a few metres in, and sinks under closed hollows on the ground above
	// (dolines drain into the cave), deep enough to be in rock.
	const Vector3 k0 = E + f * 6.0 + Vector3(0, 1.2, 0);
	LocalVector<int> keys;
	g.p.push_back(k0);
	keys.push_back(0);
	struct Sink {
		Vector3 p;
		real_t score;
	};
	LocalVector<Sink> sinks;
	for (int i = 0; i < 120; i++) {
		const Vector3 c = E + f * (30.0 + (L - 30.0) * rng.randf()) + sd * ((rng.randf() - 0.5f) * L);
		const real_t hc = H(c.x, c.z);
		if (hc < E.y + 15.0) {
			continue;
		}
		real_t ring = 0.0;
		for (int k = 0; k < 6; k++) {
			const real_t a = real_t(k) * Math::TAU / 6.0;
			ring += H(c.x + Math::cos(a) * 20.0, c.z + Math::sin(a) * 20.0);
		}
		const real_t hollow = ring / 6.0 - hc; // positive in a closed hollow
		sinks.push_back({ Vector3(c.x, hc - cover_at(c) - real_t(3.0), c.z), real_t(hollow + 2.0 * real_t(rng.randf())) });
	}
	struct SinkMore {
		_FORCE_INLINE_ bool operator()(const Sink &p_a, const Sink &p_b) const { return p_a.score > p_b.score; }
	};
	sinks.sort_custom<SinkMore>();
	const int want = 2 + int(rng.randf() * 2.99f);
	for (const Sink &s : sinks) {
		if (int(keys.size()) > want) {
			break;
		}
		bool far = in_rock(s.p);
		for (int k : keys) {
			far = far && g.p[k].distance_to(s.p) > 0.25 * L;
		}
		if (far) {
			keys.push_back(int(g.p.size()));
			g.p.push_back(s.p);
		}
	}
	if (keys.size() < 2) {
		return false;
	}

	// Samples in the rock: a jittered grid, denser on the bedding planes and the water table (where conduits form).
	real_t ylo = E.y - 30.0, yhi = E.y;
	for (int k : keys) {
		yhi = MAX(yhi, g.p[k].y + 15.0);
	}
	const real_t s = sample_spacing;
	auto jit = [&]() -> real_t { return (real_t(rng.randf()) - 0.5) * 0.8; };
	for (real_t u = -2.0; u <= L; u += s) {
		for (real_t v = -0.6 * L; v <= 0.6 * L; v += s) {
			const Vector3 c = E + f * u + sd * v;
			const real_t top = MIN(yhi, H(c.x, c.z));
			for (real_t y = ylo; y <= top; y += s) {
				const Vector3 p = c + f * (jit() * s) + sd * (jit() * s) + Vector3(0, y + jit() * s, 0);
				if (in_rock(p)) {
					g.p.push_back(p);
				}
			}
			// The horizons and the water table through this column: a point on each, anywhere in the column (on a
			// horizon bed_q is a whole number of spacings).
			const Vector3 q = Vector3(c.x, E.y, c.z) + f * (jit() * s) + sd * (jit() * s);
			const real_t off = bed_q(q);
			for (real_t level = Math::floor((ylo - E.y) / bedding_spacing) * bedding_spacing; level <= yhi - E.y; level += bedding_spacing) {
				const Vector3 p(q.x, E.y + level - off, q.z);
				if (p.y >= ylo && p.y <= yhi && in_rock(p)) {
					g.p.push_back(p);
				}
			}
			const Vector3 w(q.x, water(q), q.z);
			if (in_rock(w)) {
				g.p.push_back(w);
			}
		}
	}
	const int n = int(g.p.size());
	r_cave.keys.clear();
	for (int k : keys) {
		r_cave.keys.push_back(g.p[k]);
	}

	// The nearest-neighbour graph (Paris et al. use 40; 20 keeps paths as straight here at our spacing), each edge's
	// cost the line integral of the medium's cost: cheap along a bedding plane (smoothstep of the distance to it
	// over a quarter of the spacing) and along a joint plane, a little dearer straight up, and out of the rock never.
	const real_t cell = s * 1.6;
	HashMap<Vector3i, LocalVector<int>> grid;
	auto cell_of = [&](const Vector3 &p_p) { return Vector3i(int(Math::floor(p_p.x / cell)), int(Math::floor(p_p.y / cell)), int(Math::floor(p_p.z / cell))); };
	for (int i = 0; i < n; i++) {
		grid[cell_of(g.p[i])].push_back(i);
	}
	const int knn = 20;
	g.nbr.resize(n);
	g.cost.resize(n);
	const real_t rL = MAX(real_t(0.25) * bedding_spacing, real_t(1.0));
	auto medium = [&](const Vector3 &p_p) -> real_t {
		const real_t x = MIN(bed_dist(p_p) / rL, real_t(1.0));
		return x * x * (3.0 - 2.0 * x);
	};
	struct Near {
		float d;
		int i;
		bool operator<(const Near &p_o) const { return d < p_o.d; }
	};
	LocalVector<Near> cand;
	for (int i = 0; i < n; i++) {
		cand.clear();
		const Vector3i c = cell_of(g.p[i]);
		for (int z = -1; z <= 1; z++) {
			for (int y = -1; y <= 1; y++) {
				for (int x = -1; x <= 1; x++) {
					if (const LocalVector<int> *b = grid.getptr(c + Vector3i(x, y, z))) {
						for (int j : *b) {
							if (j != i) {
								cand.push_back({ float(g.p[i].distance_to(g.p[j])), j });
							}
						}
					}
				}
			}
		}
		cand.sort();
		for (uint32_t k = 0; k < cand.size() && int(k) < knn; k++) {
			const int j = cand[k].i;
			if (j < i && g.nbr[j].find(i) >= 0) {
				continue; // already linked from the other end
			}
			const Vector3 a = g.p[i], b = g.p[j];
			const Vector3 mid = (a + b) * 0.5;
			if (!in_rock(mid) && i != 0 && j != 0) {
				continue;
			}
			const Vector3 d = (b - a) / MAX(real_t(cand[k].d), real_t(1e-4));
			const real_t joint = MIN(n1.dot(d) * n1.dot(d), n2.dot(d) * n2.dot(d)); // 0 along a joint plane
			const real_t bed = (medium(a) + 4.0 * medium(mid) + medium(b)) / 6.0;
			// Water moves along the water table (the epiphreatic trunk) and drops down shafts from the sinks to it, so
			// level and vertical travel are cheap and inclined travel dear (sin 2 theta: most at 45 degrees).
			const real_t wt = MIN(Math::abs(mid.y - water(mid)) / 8.0, real_t(1.0));
			const real_t level = Math::sqrt(MAX(real_t(1.0) - d.y * d.y, real_t(0.0)));
			const real_t incline = 2.0 * Math::abs(d.y) * level;
			const float c_ab = float(cand[k].d * (0.2 + 1.0 * bed + 0.6 * joint + 0.6 * wt * wt * (3.0 - 2.0 * wt) * level + 0.9 * incline));
			g.nbr[i].push_back(j);
			g.cost[i].push_back(c_ab);
			g.nbr[j].push_back(i);
			g.cost[j].push_back(c_ab);
		}
	}

	// Least-cost paths from every key point, then the gamma-skeleton: a pair is linked unless some third key point k
	// lies inside its gamma lune, d(a,k)^gamma + d(k,b)^gamma < d(a,b)^gamma.
	const int K = int(keys.size());
	LocalVector<LocalVector<float>> kd;
	LocalVector<LocalVector<int>> kp;
	kd.resize(K);
	kp.resize(K);
	for (int a = 0; a < K; a++) {
		LocalVector<int> src;
		src.push_back(keys[a]);
		cave_dijkstra(g, src, kd[a], kp[a]);
	}
	HashSet<uint64_t> used_edges;
	HashMap<int, int> degree;
	auto add_path = [&](const LocalVector<int> &p_parent, int p_from) {
		int i = p_from;
		while (i >= 0 && p_parent[i] >= 0) {
			const int j = p_parent[i];
			const uint64_t key = i < j ? (uint64_t(i) << 32 | uint64_t(j)) : (uint64_t(j) << 32 | uint64_t(i));
			if (!used_edges.has(key)) {
				used_edges.insert(key);
				degree[i] = (degree.has(i) ? degree[i] : 0) + 1;
				degree[j] = (degree.has(j) ? degree[j] : 0) + 1;
			}
			i = j;
		}
	};
	for (int a = 0; a < K; a++) {
		for (int b = a + 1; b < K; b++) {
			const real_t dab = kd[a][keys[b]];
			if (dab > 1e29f) {
				continue;
			}
			bool keep = true;
			for (int k = 0; k < K && keep; k++) {
				if (k == a || k == b) {
					continue;
				}
				keep = Math::pow(real_t(kd[a][keys[k]]), gamma) + Math::pow(real_t(kd[b][keys[k]]), gamma) >= Math::pow(dab, gamma);
			}
			if (keep) {
				add_path(kp[a], keys[b]);
			}
		}
	}
	if (used_edges.is_empty()) {
		return false;
	}
	// Side passages (the paper's amplification): random points in the rock joined to the network by their least-cost
	// path to it, dead ends that branch off.
	for (int m = 0; m < side_passages; m++) {
		const int t = 1 + int(rng.rand() % uint32_t(MAX(n - 1, 1)));
		if (degree.has(t)) {
			continue;
		}
		LocalVector<int> src;
		for (const KeyValue<int, int> &kv : degree) {
			src.push_back(kv.key);
		}
		LocalVector<float> dist;
		LocalVector<int> parent;
		cave_dijkstra(g, src, dist, parent);
		if (dist[t] < 1e29f && dist[t] < 0.6f * float(L)) {
			add_path(parent, t);
		}
	}

	// Chains between junctions and ends, walked into polylines.
	HashMap<int, LocalVector<int>> adj;
	for (const uint64_t e : used_edges) {
		const int a = int(e >> 32), b = int(e & 0xffffffffu);
		adj[a].push_back(b);
		adj[b].push_back(a);
	}
	HashSet<uint64_t> walked;
	LocalVector<LocalVector<Vector3>> chains;
	LocalVector<int> chain_start;
	for (const KeyValue<int, LocalVector<int>> &kv : adj) {
		if (kv.value.size() == 2 && kv.key != keys[0]) {
			continue;
		}
		for (int nb : kv.value) {
			int prev = kv.key, cur = nb;
			const uint64_t k0e = prev < cur ? (uint64_t(prev) << 32 | uint64_t(cur)) : (uint64_t(cur) << 32 | uint64_t(prev));
			if (walked.has(k0e)) {
				continue;
			}
			LocalVector<Vector3> ch;
			ch.push_back(g.p[prev]);
			while (true) {
				const uint64_t ke = prev < cur ? (uint64_t(prev) << 32 | uint64_t(cur)) : (uint64_t(cur) << 32 | uint64_t(prev));
				walked.insert(ke);
				ch.push_back(g.p[cur]);
				const LocalVector<int> &na = adj[cur];
				if (na.size() != 2 || cur == keys[0]) {
					break;
				}
				const int next = na[0] == prev ? na[1] : na[0];
				prev = cur;
				cur = next;
			}
			chains.resize(chains.size() + 1);
			chains[chains.size() - 1] = ch;
			chain_start.push_back(kv.key);
		}
	}

	// Tortuosity: each chain resampled every 3 m and its points wandered sideways (midpoint displacement of up to
	// 4 m at full tortuosity, Paris et al.), smoothly along it.
	const real_t wander = 0.05 + 3.95 * tortuosity;
	const uint32_t wseed = p_seed * 747796405u + 11u;
	r_cave.segments.clear();
	r_cave.chambers.clear();
	// The entrance passage: from the mouth's floor out at the foot in to the first key point.
	{
		Segment es;
		es.a = E + Vector3(0, 1.2, 0) - f * 1.0;
		es.b = k0;
		es.ra = 2.2f + 0.8f * rng.randf();
		es.rb = es.ra * 0.9f;
		es.type = PASSAGE_ENTRANCE;
		r_cave.segments.push_back(es);
	}
	for (uint32_t ci = 0; ci < chains.size(); ci++) {
		const LocalVector<Vector3> &ch = chains[ci];
		LocalVector<Vector3> pts;
		for (uint32_t k = 0; k + 1 < ch.size(); k++) {
			const real_t len = ch[k].distance_to(ch[k + 1]);
			const int steps = MAX(1, int(Math::ceil(len / 3.0)));
			for (int st = 0; st < steps; st++) {
				pts.push_back(ch[k].lerp(ch[k + 1], real_t(st) / real_t(steps)));
			}
		}
		pts.push_back(ch[ch.size() - 1]);
		for (uint32_t k = 1; k + 1 < pts.size(); k++) {
			const Vector3 t = (pts[k + 1] - pts[k - 1]).normalized();
			Vector3 side = t.cross(Vector3(0, 1, 0));
			if (side.length_squared() < 1e-4f) {
				side = Vector3(1, 0, 0);
			}
			side.normalize();
			const real_t ends = MIN(real_t(k), real_t(pts.size() - 1 - k)) / 3.0; // ends stay on their nodes
			const real_t w = wander * MIN(ends, real_t(1.0));
			pts[k] += side * (w * real_t(TerrainErosionFilter::fbm(wseed + ci, pts[k].x, pts[k].z, 25.0, 3) - 0.5) * 2.0);
			pts[k].y += 0.3 * w * real_t(TerrainErosionFilter::fbm(wseed + ci + 99u, pts[k].z, pts[k].x, 18.0, 2) - 0.5);
		}
		// One width per branch with a little variation along it (no size order in real caves, Paris et al.).
		const real_t r0 = (chain_start[ci] == keys[0]) ? 1.8 + 0.8 * rng.randf() : 0.7 + 1.6 * rng.randf();
		for (uint32_t k = 0; k + 1 < pts.size(); k++) {
			Segment sg;
			sg.a = pts[k];
			sg.b = pts[k + 1];
			sg.ra = float(r0 * (0.85 + 0.3 * TerrainErosionFilter::fbm(wseed + 5u, sg.a.x, sg.a.z, 15.0, 2)));
			sg.rb = float(r0 * (0.85 + 0.3 * TerrainErosionFilter::fbm(wseed + 5u, sg.b.x, sg.b.z, 15.0, 2)));
			const Vector3 m = (sg.a + sg.b) * 0.5, d = (sg.b - sg.a).normalized();
			const real_t above = m.y - water(m);
			if (Math::abs(d.y) > 0.7f) {
				sg.type = PASSAGE_SHAFT;
			} else if (above < 0.5) {
				sg.type = bed_dist(m) < 1.5 ? PASSAGE_BED : PASSAGE_TUBE;
			} else if (above < 6.0) {
				sg.type = PASSAGE_KEYHOLE;
			} else {
				sg.type = PASSAGE_CANYON;
			}
			r_cave.segments.push_back(sg);
		}
	}
	// Breakdown chambers at some junctions, kept under their cover.
	for (const KeyValue<int, LocalVector<int>> &kv : adj) {
		if (kv.value.size() < 3 || rng.randf() > chamber_chance) {
			continue;
		}
		Chamber c;
		c.center = g.p[kv.key];
		const real_t room = H(c.center.x, c.center.z) - cover_at(c.center) - c.center.y;
		const real_t rx = 4.0 + 8.0 * rng.randf();
		c.radii = Vector3(rx, CLAMP(rx * (0.35 + 0.25 * rng.randf()), real_t(2.0), MAX(room, real_t(2.0))), rx * (0.6 + 0.4 * rng.randf()));
		c.yaw = rng.randf() * Math::TAU;
		r_cave.chambers.push_back(c);
	}
	r_cave.bounds = AABB(E, Vector3());
	for (const Segment &sg : r_cave.segments) {
		r_cave.bounds.expand_to(sg.a);
		r_cave.bounds.expand_to(sg.b);
	}
	for (const Chamber &c : r_cave.chambers) {
		r_cave.bounds.expand_to(c.center + c.radii);
		r_cave.bounds.expand_to(c.center - c.radii);
	}
	const_cast<CaveNetwork3D *>(this)->last_samples = n;
	return !r_cave.segments.is_empty();
}

int CaveNetwork3D::add_cave(const Vector3 &p_entrance, const Vector3 &p_out) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	Cave c;
	c.entrance = p_entrance;
	c.out = p_out;
	const uint32_t s = uint32_t(seed) * 2246822519u ^ uint32_t(int(Math::floor(p_entrance.x)) * 73856093) ^ uint32_t(int(Math::floor(p_entrance.z)) * 19349663);
	const bool ok = _grow(c, s);
	last_build_usec = OS::get_singleton()->get_ticks_usec() - t0;
	ERR_FAIL_COND_V_MSG(!ok, -1, "CaveNetwork3D: no network could be grown here (no generator, or too little rock behind the entrance).");
	_scatter_blocks(c, s);
	_stream_finish(true); // the batch in flight reads the caves
	streamed = false;
	caves.push_back(c);
	return int(caves.size()) - 1;
}

void CaveNetwork3D::clear_caves() {
	_stream_finish(false);
	streamed = false;
	caves.clear();
}

Array CaveNetwork3D::get_segments(int p_cave) const {
	Array out;
	const Cave *c = get_cave(p_cave);
	ERR_FAIL_NULL_V(c, out);
	for (const Segment &s : c->segments) {
		Dictionary d;
		d["a"] = s.a;
		d["b"] = s.b;
		d["radius_a"] = s.ra;
		d["radius_b"] = s.rb;
		d["type"] = int(s.type);
		out.push_back(d);
	}
	return out;
}

Array CaveNetwork3D::get_chambers(int p_cave) const {
	Array out;
	const Cave *c = get_cave(p_cave);
	ERR_FAIL_NULL_V(c, out);
	for (const Chamber &ch : c->chambers) {
		Dictionary d;
		d["center"] = ch.center;
		d["radii"] = ch.radii;
		d["yaw"] = ch.yaw;
		out.push_back(d);
	}
	return out;
}

Array CaveNetwork3D::get_key_points(int p_cave) const {
	Array out;
	const Cave *c = get_cave(p_cave);
	ERR_FAIL_NULL_V(c, out);
	for (const Vector3 &k : c->keys) {
		out.push_back(k);
	}
	return out;
}

// Entrances: cliff feet, as RockField3D finds its cave mouths (a wall of 50 to 76 degrees rising at least 8 m within
// 12 m of a level foot, no water there), limestone only when a RockField3D says what the rock is; at least 150 m apart.
Array CaveNetwork3D::find_entrances(const Rect2 &p_rect, int p_max) const {
	Array out;
	const TerrainGenerator3D *gen = _get_generator();
	ERR_FAIL_NULL_V(gen, out);
	const RockField3D *rocks = rock_field_path.is_empty() ? nullptr : Object::cast_to<RockField3D>(get_node_or_null(rock_field_path));
	struct Found {
		Vector3 p, out;
		real_t score;
	};
	LocalVector<Found> found;
	RandomPCG rng(uint32_t(seed) * 3266489917u + 1u);
	auto H = [&](real_t p_x, real_t p_z) -> real_t { return gen->get_height(Vector3(p_x, 0, p_z)); };
	for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += 8.0) {
		for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += 8.0) {
			const real_t e = 4.0;
			Vector3 n(H(x - e, z) - H(x + e, z), 2.0 * e, H(x, z - e) - H(x, z + e));
			if (!n.is_finite()) {
				continue; // Outside the generated heights.
			}
			n.normalize();
			if (n.y >= 0.64f || n.y <= 0.23f) {
				continue;
			}
			const Vector2 down = Vector2(n.x, n.z).normalized();
			const Vector3 foot(x + down.x * 10.0, 0, z + down.y * 10.0);
			const real_t hf = H(foot.x, foot.z);
			if (H(foot.x - down.x * 12.0, foot.z - down.y * 12.0) <= hf + 8.0 || gen->get_water_depth(Vector3(foot.x, hf, foot.z)) > 0.0) {
				continue;
			}
			bool level = true;
			for (int k = 1; k <= 4 && level; k++) {
				level = Math::abs(H(foot.x + down.x * 2.0 * k, foot.z + down.y * 2.0 * k) - hf) < 1.5;
			}
			if (!level) {
				continue;
			}
			if (rocks && rocks->get_rock_type_at(Vector3(foot.x, hf, foot.z)) != RockGenerator::ROCK_LIMESTONE) {
				continue;
			}
			// The mouth where the wall rises 1 m above the foot.
			Vector3 m(foot.x, hf, foot.z);
			for (real_t t = 0.0; t <= 14.0; t += 0.5) {
				const Vector3 q(foot.x - down.x * t, 0, foot.z - down.y * t);
				if (H(q.x, q.z) > hf + 1.0) {
					m = Vector3(q.x + down.x * 1.0, hf, q.z + down.y * 1.0);
					break;
				}
			}
			found.push_back({ m, Vector3(down.x, 0, down.y), real_t(1.0 - n.y) + real_t(0.3 * rng.randf()) });
		}
	}
	struct More {
		_FORCE_INLINE_ bool operator()(const Found &p_a, const Found &p_b) const { return p_a.score > p_b.score; }
	};
	found.sort_custom<More>();
	LocalVector<Vector3> taken;
	for (const Found &fd : found) {
		if (out.size() >= p_max) {
			break;
		}
		bool far = true;
		for (const Vector3 &t : taken) {
			far = far && Vector2(t.x - fd.p.x, t.z - fd.p.z).length() > 150.0f;
		}
		if (!far) {
			continue;
		}
		taken.push_back(fd.p);
		Dictionary d;
		d["position"] = fd.p;
		d["out"] = fd.out;
		out.push_back(d);
	}
	return out;
}


// Fallen blocks on the floors: breakdown in chambers (slabs off the roof along its beds, Ford and Williams, "Karst
// Hydrogeology and Geomorphology", ch. 7) and rounded boulders in sea caves (tumbled by the waves). Each is a box,
// slab-shaped in chambers, tilted, half sunk into the floor.
void CaveNetwork3D::_scatter_blocks(Cave &r_cave, uint32_t p_seed) const {
	RandomPCG rng(p_seed * 2654435761u + 41u);
	auto add = [&](const Vector3 &p_at, const Vector3 &p_half, real_t p_round) {
		Basis b = Basis(Vector3(0, 1, 0), rng.randf() * Math::TAU);
		b = Basis(Vector3(rng.randf() - 0.5, 0, rng.randf() - 0.5).normalized(), (rng.randf() - 0.5) * 0.7) * b;
		Block bk;
		bk.xform = Transform3D(b.scaled_local(p_half), p_at);
		bk.inv = bk.xform.affine_inverse();
		bk.round = p_round;
		r_cave.blocks.push_back(bk);
	};
	for (const Chamber &ch : r_cave.chambers) {
		const real_t floor_y = ch.center.y - 0.55 * ch.radii.y;
		const int n = 4 + int(rng.randf() * 7.0);
		for (int i = 0; i < n; i++) {
			const real_t a = rng.randf() * Math::TAU, rr = Math::sqrt(rng.randf()) * 0.7;
			const Vector3 at = ch.center + Basis(Vector3(0, 1, 0), ch.yaw).xform(Vector3(Math::cos(a) * ch.radii.x * rr, 0, Math::sin(a) * ch.radii.z * rr));
			const real_t sz = 0.4 + 1.3 * Math::pow(double(rng.randf()), 1.8);
			add(Vector3(at.x, floor_y + sz * 0.15, at.z), Vector3(sz, sz * (0.3 + 0.25 * rng.randf()), sz * (0.6 + 0.4 * rng.randf())), 0.06 + 0.05 * sz);
		}
	}
	if (r_cave.sea) {
		for (const Segment &sg : r_cave.segments) {
			if (sg.type != PASSAGE_SEA || rng.randf() > 0.7f) {
				continue;
			}
			const int n = 1 + int(rng.randf() * 3.0);
			for (int i = 0; i < n; i++) {
				const Vector3 m = sg.a.lerp(sg.b, rng.randf());
				const Vector3 t = (sg.b - sg.a).normalized(), side = t.cross(Vector3(0, 1, 0)).normalized();
				const real_t floor_y = m.y - 1.25 * sg.ra;
				const real_t sz = 0.25 + 0.7 * Math::pow(double(rng.randf()), 1.5);
				add(Vector3(m.x, floor_y + sz * 0.3, m.z) + side * ((rng.randf() - 0.5) * sg.ra), Vector3(sz, sz * (0.6 + 0.3 * rng.randf()), sz * (0.7 + 0.3 * rng.randf())), sz * 0.45);
			}
		}
	}
}

// Sea caves (docs/ideas/rocks.md, stage 3; Moore 1954, Trenhaile, "Coastal Dynamics and Landforms"): waves quarry a
// joint or fault at the foot of a cliff over water deep enough to keep their energy (the coast layer's 0.8 to 12 m).
// The cave is short (under about 100 m), straight along its joint, widest and tallest at the mouth, floored below the
// low water so the sea runs in, often ending in a chamber where the joint crosses another; where the roof of that
// chamber is thin, a shaft breaks through to the cliff top (a blowhole).
Array CaveNetwork3D::find_sea_entrances(const Rect2 &p_rect, int p_max) const {
	Array out;
	const TerrainGenerator3D *gen = _get_generator();
	ERR_FAIL_NULL_V(gen, out);
	if (!gen->has_sea_result()) {
		return out;
	}
	const real_t sea = gen->get_sea_level();

	struct Found {
		Vector3 p, out;
		real_t score;
	};
	LocalVector<Found> found;
	RandomPCG rng(uint32_t(seed) * 2654435761u + 77u);
	auto H = [&](real_t p_x, real_t p_z) -> real_t { return gen->get_height(Vector3(p_x, 0, p_z)); };
	for (real_t z = p_rect.position.y; z < p_rect.get_end().y; z += 8.0) {
		for (real_t x = p_rect.position.x; x < p_rect.get_end().x; x += 8.0) {
			const real_t h = H(x, z);
			if (!Math::is_finite(h) || h < sea + 8.0) {
				continue; // a cliff at least 8 m over the sea
			}

			const real_t e = 4.0;
			Vector3 n(H(x - e, z) - H(x + e, z), 2.0 * e, H(x, z - e) - H(x, z + e));
			if (!n.is_finite()) {
				continue;
			}
			n.normalize();
			if (n.y >= 0.8f) {
				continue;
			}
			const Vector2 dn = Vector2(n.x, n.z).normalized();
			// Seaward from the cliff: the first point under the sea, then the depth there.
			Vector2 m(x, z);
			bool wet = false;
			for (real_t t = 0.0; t < 60.0; t += 1.0) {
				const Vector2 q = Vector2(x, z) + dn * t;
				if (H(q.x, q.y) < sea - 0.5) {
					m = q;
					wet = true;
					break;
				}
			}
			if (!wet) {
				continue;
			}

			const Vector2 off = m + dn * 4.0;
			const real_t depth = sea - H(off.x, off.y);
			if (depth < 0.3 || depth > 25.0) {
				continue;
			}

			// The mouth: back from the waterline to where the cliff face rises 2 m above the sea.
			Vector2 mouth = m;
			for (real_t t = 0.0; t < 20.0; t += 0.5) {
				const Vector2 q = m - dn * t;
				if (H(q.x, q.y) > sea + 2.0) {
					mouth = q + dn * 1.0;
					break;
				}
			}
			// The cliff behind must stand: 10 m in, the ground at least 8 m over the sea.
			const Vector2 back = mouth - dn * 10.0;
			if (H(back.x, back.y) < sea + 8.0) {
				continue;
			}
			found.push_back({ Vector3(mouth.x, sea, mouth.y), Vector3(dn.x, 0, dn.y), real_t(H(back.x, back.y) - sea) + real_t(4.0 * rng.randf()) });
		}
	}
	struct More {
		_FORCE_INLINE_ bool operator()(const Found &p_a, const Found &p_b) const { return p_a.score > p_b.score; }
	};
	found.sort_custom<More>();
	LocalVector<Vector3> taken;
	for (const Found &fd : found) {
		if (out.size() >= p_max) {
			break;
		}
		bool far = true;
		for (const Vector3 &t : taken) {
			far = far && Vector2(t.x - fd.p.x, t.z - fd.p.z).length() > 80.0f;
		}
		if (!far) {
			continue;
		}
		taken.push_back(fd.p);
		Dictionary d;
		d["position"] = fd.p;
		d["out"] = fd.out;
		out.push_back(d);
	}
	return out;
}

int CaveNetwork3D::add_sea_cave(const Vector3 &p_entrance, const Vector3 &p_out) {
	TerrainGenerator3D *gen = _get_generator();
	ERR_FAIL_NULL_V(gen, -1);
	const real_t sea = gen->has_sea_result() ? gen->get_sea_level() : p_entrance.y;
	RandomPCG rng(uint32_t(seed) * 2654435761u ^ uint32_t(int(Math::floor(p_entrance.x)) * 73856093) ^ uint32_t(int(Math::floor(p_entrance.z)) * 19349663) ^ 0x5eau);
	Cave c;
	c.sea = true;
	c.entrance = p_entrance;
	c.out = Vector3(p_out.x, 0, p_out.z).normalized();
	const Vector3 f = -c.out;
	// The joint: the facing turned by up to 15 degrees, held for the whole cave (sea caves are straight).
	const Vector3 dir = Basis(Vector3(0, 1, 0), real_t((rng.randf() - 0.5) * Math::deg_to_rad(30.0))).xform(f);
	const Vector3 side = dir.cross(Vector3(0, 1, 0)).normalized();
	const real_t length = 35.0 + 60.0 * rng.randf();
	const real_t r_mouth = 2.6 + 1.6 * rng.randf();
	const uint32_t ws = rng.rand();
	Vector3 prev = p_entrance + c.out * 2.0;
	real_t prev_r = r_mouth * 1.15;
	for (real_t t = 3.0; t <= length; t += 3.0) {
		const real_t u = t / length;
		// The floor (passage line minus 1.25 r) at 1.5 m below the sea at the mouth, rising to the high water at the back;
		// a slight wander along the joint.
		const real_t r = Math::lerp(r_mouth, real_t(1.3), Math::pow(u, real_t(0.7))) * real_t(0.8 + 0.45 * TerrainErosionFilter::fbm(ws + 3u, t, 7.0, 14.0, 2));
		const real_t floor_y = sea + Math::lerp(real_t(-1.5), real_t(0.6), u * u);
		Vector3 p = p_entrance + dir * t + side * real_t((TerrainErosionFilter::fbm(ws, t, 0.0, 30.0, 2) - 0.5) * 3.0);
		p.y = floor_y + 1.25 * r;
		// Rock over the roof and to both sides: stop where it gets too thin (the cave would break out of the cliff).
		const real_t roof = p.y + 0.6 * r;
		// (Not in the first metres: the mouth is in the cliff face, under its patch.)
		const bool roof_thin = t > 6.0 && gen->get_height(p) - roof < 3.0;
		const bool side_thin = t > 10.0 && (gen->get_height(p + side * (0.75 * r + 3.0)) < roof + 2.0 || gen->get_height(p - side * (0.75 * r + 3.0)) < roof + 2.0);
		if (roof_thin || side_thin) {
			break;
		}
		Segment sg;
		sg.a = prev;
		sg.b = p;
		sg.ra = float(prev_r);
		sg.rb = float(r);
		sg.type = PASSAGE_SEA;
		c.segments.push_back(sg);
		prev = p;
		prev_r = r;
	}
	ERR_FAIL_COND_V_MSG(c.segments.size() < 3, -1, "CaveNetwork3D: too little cliff behind this sea cave mouth.");
	// A chamber at the back where the joint meets another, half the time.
	const Vector3 end = c.segments[c.segments.size() - 1].b;
	if (rng.randf() < 0.5) {
		Chamber ch;
		const real_t rx = 3.5 + 3.5 * rng.randf();
		ch.radii = Vector3(rx, MIN(rx * 0.8, MAX(gen->get_height(end) - end.y - 4.0, real_t(2.5))), rx * 0.8);
		ch.center = end + dir * (rx * 0.5);
		ch.center.y = sea + 0.4 + 0.55 * ch.radii.y; // the flat floor (0.55 of its height down) at high water
		ch.yaw = rng.randf() * Math::TAU;
		c.chambers.push_back(ch);
		// The blowhole: where the cover over the chamber is under 25 m, a shaft up to the cliff top.
		const Vector3 top = ch.center + Vector3(0, ch.radii.y * 0.8, 0);
		const real_t ground = gen->get_height(top);
		if (ground - top.y < 25.0) {
			Segment sh;
			sh.a = top;
			sh.b = Vector3(top.x, ground + 2.0, top.z);
			sh.ra = 0.9f + 0.5f * rng.randf();
			sh.rb = sh.ra * 1.3f; // wider where it opens
			sh.type = PASSAGE_SHAFT;
			c.segments.push_back(sh);
			c.blowholes.push_back(Vector3(top.x, ground, top.z));
		}
	}
	_scatter_blocks(c, rng.rand());
	c.keys.push_back(p_entrance);
	c.bounds = AABB(p_entrance, Vector3());
	for (const Segment &sg : c.segments) {
		c.bounds.expand_to(sg.a);
		c.bounds.expand_to(sg.b);
	}
	for (const Chamber &ch : c.chambers) {
		c.bounds.expand_to(ch.center + ch.radii);
		c.bounds.expand_to(ch.center - ch.radii);
	}
	_stream_finish(true);
	streamed = false;
	caves.push_back(c);
	return int(caves.size()) - 1;
}

bool CaveNetwork3D::is_sea_cave(int p_cave) const {
	ERR_FAIL_INDEX_V(p_cave, int(caves.size()), false);
	return caves[p_cave].sea;
}

PackedVector3Array CaveNetwork3D::get_blowholes(int p_cave) const {
	PackedVector3Array out;
	ERR_FAIL_INDEX_V(p_cave, int(caves.size()), out);
	for (const Vector3 &b : caves[p_cave].blowholes) {
		out.push_back(b);
	}
	return out;
}

/* Meshes */

namespace {

// Value noise (smooth, cheap) for the walls' roughness.
inline float cave_hash(int p_x, int p_y, int p_z, uint32_t p_seed) {
	uint32_t h = uint32_t(p_x) * 374761393u + uint32_t(p_y) * 668265263u + uint32_t(p_z) * 2246822519u + p_seed * 3266489917u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h ^ (h >> 16)) * (1.0f / 4294967296.0f);
}

float cave_noise(const Vector3 &p_p, uint32_t p_seed) {
	const float fx = Math::floor(p_p.x), fy = Math::floor(p_p.y), fz = Math::floor(p_p.z);
	const int ix = int(fx), iy = int(fy), iz = int(fz);
	float tx = p_p.x - fx, ty = p_p.y - fy, tz = p_p.z - fz;
	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);
	tz = tz * tz * (3.0f - 2.0f * tz);
	float c[2][2];
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			c[z][y] = Math::lerp(cave_hash(ix, iy + y, iz + z, p_seed), cave_hash(ix + 1, iy + y, iz + z, p_seed), tx);
		}
	}
	return Math::lerp(Math::lerp(c[0][0], c[0][1], ty), Math::lerp(c[1][0], c[1][1], ty), tz);
}

// Worley (cellular) noise: the distance to the nearest and second nearest feature points, one per unit cell.
// Dissolution scallops are such cells: dish-shaped hollows with sharp ridges between them (Curl 1974).
void cave_worley(const Vector3 &p_p, uint32_t p_seed, float &r_f1, float &r_f2) {
	const int ix = int(Math::floor(p_p.x)), iy = int(Math::floor(p_p.y)), iz = int(Math::floor(p_p.z));
	r_f1 = 9.0f;
	r_f2 = 9.0f;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				const Vector3 f(ix + x + cave_hash(ix + x, iy + y, iz + z, p_seed), iy + y + cave_hash(ix + x, iy + y, iz + z, p_seed + 1u), iz + z + cave_hash(ix + x, iy + y, iz + z, p_seed + 2u));
				const float d = (f - p_p).length_squared();
				if (d < r_f1) {
					r_f2 = r_f1;
					r_f1 = d;
				} else if (d < r_f2) {
					r_f2 = d;
				}
			}
		}
	}
	r_f1 = Math::sqrt(r_f1);
	r_f2 = Math::sqrt(r_f2);
}

float cave_fbm(const Vector3 &p_p, uint32_t p_seed, int p_octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	Vector3 q = p_p;
	for (int o = 0; o < p_octaves; o++) {
		sum += amp * cave_noise(q, p_seed + uint32_t(o) * 101u);
		norm += amp;
		amp *= 0.5f;
		q = q * 2.03f + Vector3(17.1f, 3.7f, 9.3f);
	}
	return sum / norm;
}

inline real_t cave_smin(real_t a, real_t b, real_t k) {
	const real_t h = MAX(k - Math::abs(a - b), real_t(0.0)) / k;
	return MIN(a, b) - h * h * k * 0.25;
}

// 2D distance (approximate, negative inside) to an ellipse with semi-axes (p_rx, p_ry) centred at (0, p_cy).
inline real_t cave_ellipse(real_t u, real_t v, real_t p_rx, real_t p_ry, real_t p_cy) {
	const real_t x = u / p_rx, y = (v - p_cy) / p_ry;
	return (Math::sqrt(x * x + y * y) - 1.0) * MIN(p_rx, p_ry);
}

// 2D box from v0 to v1, half width hw, corners rounded by p_round.
inline real_t cave_box(real_t u, real_t v, real_t hw, real_t v0, real_t v1, real_t p_round) {
	const real_t cy = 0.5 * (v0 + v1), hh = 0.5 * (v1 - v0);
	const real_t dx = Math::abs(u) - (hw - p_round), dy = Math::abs(v - cy) - (hh - p_round);
	return Vector2(MAX(dx, real_t(0.0)), MAX(dy, real_t(0.0))).length() + MIN(MAX(dx, dy), real_t(0.0)) - p_round;
}

// A passage's cross-section by type (u across, v up from the passage line, r its half width): distance to the open
// section, negative inside, and the height of its sediment floor (a fill that levels the bottom; none in shafts).
real_t cave_profile(CaveNetwork3D::PassageType p_type, real_t u, real_t v, real_t r, real_t &r_floor) {
	switch (p_type) {
		case CaveNetwork3D::PASSAGE_ENTRANCE:
			r_floor = -0.45 * r;
			return cave_ellipse(u, v, 1.1 * r, 0.8 * r, 0.2 * r);
		case CaveNetwork3D::PASSAGE_BED:
			r_floor = -0.25 * r;
			return cave_ellipse(u, v, 1.8 * r, 0.45 * r, 0.0);
		case CaveNetwork3D::PASSAGE_KEYHOLE:
			r_floor = -1.1 * r;
			return MIN(cave_ellipse(u, v, 0.8 * r, 0.6 * r, 0.4 * r), cave_box(u, v, 0.3 * r, -1.3 * r, 0.4 * r, 0.12 * r));
		case CaveNetwork3D::PASSAGE_CANYON:
			r_floor = -1.1 * r;
			return MIN(cave_box(u, v, 0.4 * r, -1.25 * r, 1.2 * r, 0.15 * r), cave_ellipse(u, v, 0.4 * r, 0.5 * r, 1.2 * r));
		case CaveNetwork3D::PASSAGE_SHAFT:
			r_floor = -1e9;
			return Math::sqrt(u * u + v * v) - r;
		case CaveNetwork3D::PASSAGE_SEA: {
			// Cut along a joint and the beds it crosses (sea caves are joint-guided, Moore 1954): straight sides
			// stepped in and out at each bed (1.1 m), a gabled roof where blocks fell out along two joint sets, the
			// floor bare rock and boulders, no fill. The sides lean a little with the joint's dip.
			r_floor = -1.25 * r;
			const real_t bed = Math::fposmod(real_t(v + 0.37f * r), real_t(1.1)) / 1.1;
			const real_t step = 0.18 * r * (Math::smoothstep(real_t(0.0), real_t(0.12), bed) - Math::smoothstep(real_t(0.55), real_t(0.67), bed));
			const real_t uu = u - 0.12 * v; // the joint's dip
			const real_t hw = 0.62 * r + step;
			const real_t sides = Math::abs(uu) - hw;
			const real_t bottom = -1.3 * r - v;
			const real_t gable = (v - (1.55 * r - 0.85 * Math::abs(uu))) * 0.76;
			return MAX(MAX(sides, bottom), gable);
		}
		case CaveNetwork3D::PASSAGE_TUBE:
		default:
			r_floor = -0.55 * r;
			return cave_ellipse(u, v, r, 0.75 * r, 0.0);
	}
}

} // namespace

// Distance to the open cave (negative inside): every passage as its section swept along it, the ends run on by half
// a width so bends close, joined with a smooth minimum; chambers as ellipsoids with a flat breakdown floor; the
// walls roughened by noise at 2.2 m and 0.7 m (dissolution hollows and spalls).
real_t CaveNetwork3D::_air(const FieldSource &p_src, const Vector3 &p_p) const {
	real_t d = 1e9;
	for (const Segment *sg : p_src.segments) {
		Vector3 t = sg->b - sg->a;
		const real_t len = t.length();
		if (len < 1e-4f) {
			continue;
		}
		t /= len;
		const real_t r_end = MAX(sg->ra, sg->rb);
		const real_t l = (p_p - sg->a).dot(t);
		const real_t ext = 0.5 * r_end;
		const real_t s = l < -ext ? -ext - l : (l > len + ext ? l - len - ext : 0.0);
		const real_t lc = CLAMP(l, real_t(0.0), len);
		const Vector3 q = p_p - (sg->a + t * lc);
		Vector3 u_axis, v_axis;
		if (sg->type == PASSAGE_SHAFT || Math::abs(t.y) > 0.9f) {
			u_axis = Vector3(1, 0, 0);
			v_axis = Vector3(0, 0, 1);
			u_axis = (u_axis - t * u_axis.dot(t)).normalized();
			v_axis = t.cross(u_axis);
		} else {
			u_axis = t.cross(Vector3(0, 1, 0)).normalized();
			v_axis = u_axis.cross(t);
		}
		const real_t r = Math::lerp(real_t(sg->ra), real_t(sg->rb), lc / len);
		real_t floor_v;
		real_t dc = cave_profile(sg->type, q.dot(u_axis), q.dot(v_axis), r, floor_v);
		// The fill is never quite level: ripples, puddle hollows and mud banks a few decimetres high.
		floor_v += 0.35 * (cave_fbm(p_p / 1.6f, uint32_t(seed) + 101u, 2) - 0.5f) + 0.12 * (cave_fbm(p_p / 0.5f, uint32_t(seed) + 103u, 2) - 0.5f);
		dc = MAX(dc, floor_v - q.dot(v_axis));
		const real_t ds = s > 0.0 ? (dc > 0.0 ? Math::sqrt(s * s + dc * dc) : s) : dc;
		d = cave_smin(d, ds, 0.8);
	}
	for (const Chamber *c : p_src.chambers) {
		const Vector3 q = Basis(Vector3(0, 1, 0), -c->yaw).xform(p_p - c->center);
		const Vector3 n = q / c->radii;
		real_t dc = (n.length() - 1.0) * MIN(c->radii.x, MIN(c->radii.y, c->radii.z));
		dc = MAX(dc, -0.55 * c->radii.y - q.y);
		d = cave_smin(d, dc, 2.0);
	}
	if (roughness > 0.0 && d < 2.5) {
		const uint32_t ns = uint32_t(seed) * 2654435761u + 3u;
		d += roughness * (0.35 * (cave_fbm(p_p / 2.2f, ns, 3) - 0.5f) + 0.12 * (cave_fbm(p_p / 0.7f, ns + 7u, 2) - 0.5f));
		if (scallops > 0.0) {
			// Scallops: hollows 1.2 m across (the big ones of slow phreatic flow; small fast-flow scallops are below
			// the mesh cell), deepest in the middle of each cell, ridges where two cells meet.
			float f1, f2;
			cave_worley(p_p / 1.2f, ns + 11u, f1, f2);
			const float ridge = CLAMP((f2 - f1) / 0.6f, 0.0f, 1.0f);
			d -= scallops * 0.14 * ridge * (2.0f - ridge);
		}
	}
	return d;
}

// The rock: positive in it, below ground and outside the cave, min(f_H, f_K) with the heightfield as a distance,
// f_H = (h - y) / sqrt(1 + slope^2) as Paris et al. take it.
real_t CaveNetwork3D::_rock(const FieldSource &p_src, const TerrainGenerator3D *p_gen, const Vector3 &p_p, bool *r_ground) const {
	const real_t cave = _air(p_src, p_p); // positive outside the cave, in the rock
	if (r_ground) {
		*r_ground = false;
	}
	// The ground everywhere (so the field is one function; the mesher keeps the ground's own quads only inside an
	// entrance patch). Far from the surface its scale does not matter.
	const real_t h = p_gen->get_height(p_p);
	real_t g = h - p_p.y;
	if (Math::abs(g) < 12.0) {
		const real_t eps = 0.5;
		const real_t hx = p_gen->get_height(p_p + Vector3(eps, 0, 0)) - p_gen->get_height(p_p - Vector3(eps, 0, 0));
		const real_t hz = p_gen->get_height(p_p + Vector3(0, 0, eps)) - p_gen->get_height(p_p - Vector3(0, 0, eps));
		g /= Math::sqrt(1.0 + (hx * hx + hz * hz) / (4.0 * eps * eps));
	}
	real_t f = cave;
	if (g < cave) {
		if (r_ground) {
			*r_ground = true;
		}
		f = g;
	}
	// Fallen blocks: rounded boxes added to the rock, set into the floor with a small smooth blend.
	for (const Block *bk : p_src.blocks) {
		const Vector3 q = bk->inv.xform(p_p);
		const Vector3 hs = bk->xform.basis.get_scale();
		const Vector3 d = q.abs() * hs - hs + Vector3(bk->round, bk->round, bk->round); // in metres (inv maps to unit box)
		real_t sd = Vector3(MAX(d.x, real_t(0.0)), MAX(d.y, real_t(0.0)), MAX(d.z, real_t(0.0))).length() + MIN(MAX(d.x, MAX(d.y, d.z)), real_t(0.0)) - bk->round;
		if (sd < 0.5) {
			// Broken faces: spalls and chipped edges.
			sd += 0.32 * (cave_fbm(p_p / 0.8f, uint32_t(seed) + 211u, 2) - 0.5f) + 0.1 * (cave_fbm(p_p / 0.35f, uint32_t(seed) + 213u, 2) - 0.5f);
		}
		if (-sd > f - 0.3) {
			const real_t k = 0.12;
			const real_t hh = CLAMP(0.5 + 0.5 * (-sd - f) / k, 0.0, 1.0);
			f = Math::lerp(f, -sd, hh) + k * hh * (1.0 - hh);
		}
	}
	return f;
}

real_t CaveNetwork3D::get_rock_distance(const Vector3 &p_position) const {
	FieldSource src;
	_gather(AABB(p_position - Vector3(1, 1, 1), Vector3(2, 2, 2)), src);
	const TerrainGenerator3D *gen = _get_generator();
	return gen ? _rock(src, gen, p_position) : _air(src, p_position);
}

void CaveNetwork3D::_gather(const AABB &p_box, FieldSource &r_src) const {
	for (const Cave &c : caves) {
		if (!c.bounds.grow(16.0).intersects(p_box)) {
			continue;
		}
		for (const Segment &sg : c.segments) {
			const real_t r = MAX(sg.ra, sg.rb) * 2.2 + 2.0;
			AABB b(sg.a, Vector3());
			b.expand_to(sg.b);
			if (b.grow(r).intersects(p_box)) {
				r_src.segments.push_back(&sg);
			}
		}
		for (const Block &bk : c.blocks) {
			if (AABB(bk.xform.origin, Vector3()).grow(bk.xform.basis.get_scale().length() + 1.0).intersects(p_box)) {
				r_src.blocks.push_back(&bk);
			}
		}
		for (const Chamber &ch : c.chambers) {
			if (AABB(ch.center - ch.radii, ch.radii * 2.0).grow(3.0).intersects(p_box)) {
				r_src.chambers.push_back(&ch);
			}
		}
		const real_t pr = patch_radius * 1.4143 + 4.0;
		if (patch_radius > 0.0 && AABB(c.entrance - Vector3(pr, 60.0, pr), Vector3(2.0 * pr, 1000.0, 2.0 * pr)).intersects(p_box)) {
			r_src.patches.push_back(c.entrance);
			r_src.patch_out.push_back(c.out);
		}
	}
}

// Surface nets (Gibson 1998, as in Lysenko's and Bartholomew's naive surface nets): the field at the corners of a
// grid of mesh_cell, one vertex per cell the surface crosses (the mean of its edge crossings), one quad per grid edge
// it crosses joining the four cells round that edge. Each chunk owns the edges that start inside it and samples one
// cell past its sides, so neighbours share their border vertices exactly. Normals from the field's gradient; quads
// facing the open side. The ground's own surface (a patch's) is a second surface, drawn with the ground material.
void CaveNetwork3D::_mesh_chunk(const Vector3i &p_key, Chunk &r_chunk) const {
	const TerrainGenerator3D *gen = mesh_gen;
	const int N = MAX(1, int(Math::round(chunk_size / mesh_cell)));
	const real_t c = chunk_size / real_t(N); // whole cells per chunk, so neighbours share their border samples
	const Vector3 O = Vector3(p_key) * chunk_size;
	FieldSource src;
	_gather(AABB(O - Vector3(2 * c, 2 * c, 2 * c), Vector3(chunk_size + 4 * c, chunk_size + 4 * c, chunk_size + 4 * c)), src);
	r_chunk.key = p_key;
	r_chunk.triangles = 0;
	if (src.segments.is_empty() && src.chambers.is_empty() && src.patches.is_empty()) {
		return;
	}
	// Drip points round the chunk (their flowstone reaches over its sides), found from the field alone so every chunk
	// finds the same ones.
	LocalVector<Drip> drips;
	if (speleothems > 0.0 && gen) {
		_drips(src, gen, AABB(O, Vector3(chunk_size, chunk_size, chunk_size)).grow(2.0), drips);
	}
	const int S = N + 3; // samples from -1 to N + 1
	auto sidx = [S](int x, int y, int z) { return ((z + 1) * S + (y + 1)) * S + (x + 1); };
	LocalVector<float> f;
	LocalVector<uint8_t> ground;
	f.resize(S * S * S);
	ground.resize(S * S * S);
	bool any_pos = false, any_neg = false;
	for (int z = -1; z <= N + 1; z++) {
		for (int y = -1; y <= N + 1; y++) {
			for (int x = -1; x <= N + 1; x++) {
				bool gr = false;
				float v = float(gen ? _rock(src, gen, O + Vector3(x, y, z) * c, &gr) : _air(src, O + Vector3(x, y, z) * c));
				if (!drips.is_empty()) {
					v = float(_flowstone(drips, O + Vector3(x, y, z) * c, v));
				}
				f[sidx(x, y, z)] = v;
				ground[sidx(x, y, z)] = gr ? 1 : 0;
				any_pos = any_pos || v > 0.0f;
				any_neg = any_neg || v <= 0.0f;
			}
		}
	}
	if (!any_pos || !any_neg) {
		return;
	}
	// Cells from -1 to N, one vertex where the sign changes.
	const int C = N + 2;
	auto cidx = [C](int x, int y, int z) { return ((z + 1) * C + (y + 1)) * C + (x + 1); };
	LocalVector<int> cell_vertex;
	cell_vertex.resize(C * C * C);
	LocalVector<Vector3> verts;
	LocalVector<uint8_t> vert_ground;
	static const int corner[8][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 } };
	static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
	for (int z = -1; z <= N; z++) {
		for (int y = -1; y <= N; y++) {
			for (int x = -1; x <= N; x++) {
				float v[8];
				int pos = 0, gr = 0;
				for (int k = 0; k < 8; k++) {
					const int si = sidx(x + corner[k][0], y + corner[k][1], z + corner[k][2]);
					v[k] = f[si];
					pos += v[k] > 0.0f ? 1 : 0;
					gr += ground[si];
				}
				if (pos == 0 || pos == 8) {
					cell_vertex[cidx(x, y, z)] = -1;
					continue;
				}
				Vector3 sum;
				int n = 0;
				for (int e = 0; e < 12; e++) {
					const float a = v[edges[e][0]], b = v[edges[e][1]];
					if ((a > 0.0f) == (b > 0.0f)) {
						continue;
					}
					const float t = a / (a - b);
					const Vector3 pa(corner[edges[e][0]][0], corner[edges[e][0]][1], corner[edges[e][0]][2]);
					const Vector3 pb(corner[edges[e][1]][0], corner[edges[e][1]][1], corner[edges[e][1]][2]);
					sum += pa.lerp(pb, t);
					n++;
				}
				cell_vertex[cidx(x, y, z)] = int(verts.size());
				verts.push_back(O + (Vector3(x, y, z) + sum / real_t(n)) * c);
				vert_ground.push_back(gr >= 4 ? 1 : 0);
			}
		}
	}
	// Quads on the edges this chunk owns (start sample in 0..N-1 on all three axes).
	LocalVector<int> tris[2]; // 0 walls, 1 ground
	for (int z = 0; z < N; z++) {
		for (int y = 0; y < N; y++) {
			for (int x = 0; x < N; x++) {
				const float a = f[sidx(x, y, z)];
				for (int axis = 0; axis < 3; axis++) {
					const int bx = x + (axis == 0), by = y + (axis == 1), bz = z + (axis == 2);
					const float b = f[sidx(bx, by, bz)];
					if ((a > 0.0f) == (b > 0.0f)) {
						continue;
					}
					// The four cells round the edge.
					int q[4];
					if (axis == 0) {
						q[0] = cidx(x, y - 1, z - 1);
						q[1] = cidx(x, y, z - 1);
						q[2] = cidx(x, y, z);
						q[3] = cidx(x, y - 1, z);
					} else if (axis == 1) {
						q[0] = cidx(x - 1, y, z - 1);
						q[1] = cidx(x - 1, y, z);
						q[2] = cidx(x, y, z);
						q[3] = cidx(x, y, z - 1);
					} else {
						q[0] = cidx(x - 1, y - 1, z);
						q[1] = cidx(x, y - 1, z);
						q[2] = cidx(x, y, z);
						q[3] = cidx(x - 1, y, z);
					}
					int vi[4];
					bool ok = true;
					for (int k = 0; k < 4; k++) {
						vi[k] = cell_vertex[q[k]];
						ok = ok && vi[k] >= 0;
					}
					if (!ok) {
						continue;
					}
					// Ground quads outside the patch radius are the landscape's to draw.
					int gq = 0;
					for (int k = 0; k < 4; k++) {
						gq += vert_ground[vi[k]];
					}
					const bool is_ground = gq >= 3;
					if (is_ground) {
						// The patch is a square along the entrance's facing, the landscape's hole the same square 1.5 m
						// smaller, so the two overlap all round.
						bool inside = false;
						for (uint32_t pi = 0; pi < src.patches.size() && !inside; pi++) {
							const Vector3 d = verts[vi[0]] - src.patches[pi];
							const Vector3 o = Vector3(src.patch_out[pi].x, 0, src.patch_out[pi].z).normalized();
							inside = Math::abs(d.x * o.x + d.z * o.z) < patch_radius && Math::abs(-d.x * o.z + d.z * o.x) < patch_radius;
						}
						if (!inside) {
							continue;
						}
					}
					// Front faces are clockwise in Godot: (v1 - v0) x (v2 - v0) then points away from the viewer, into
					// the rock, the way the field rises (from b to a when a is the rock side).
					Vector3 into = Vector3(axis == 0, axis == 1, axis == 2) * (a > 0.0f ? -1.0f : 1.0f);
					// The quad's orientation from its whole outline (Newell), so a folded quad cannot flip it; then the
					// diagonal whose two triangles both face the open side, the shorter if both do (a fold split the
					// wrong way leaves one triangle facing the rock, culled: a sliver of sky through the wall).
					const Vector3 q0 = verts[vi[0]];
					const Vector3 nq = (verts[vi[1]] - q0).cross(verts[vi[2]] - q0) + (verts[vi[2]] - q0).cross(verts[vi[3]] - q0);
					if (nq.dot(into) < 0.0f) {
						SWAP(vi[1], vi[3]);
					}
					const Vector3 &p0 = verts[vi[0]], &p1 = verts[vi[1]], &p2 = verts[vi[2]], &p3 = verts[vi[3]];
					auto faces = [&into](const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) { return (p_b - p_a).cross(p_c - p_a).dot(into) > 0.0f; };
					const bool d02 = faces(p0, p1, p2) && faces(p0, p2, p3);
					const bool d13 = faces(p1, p2, p3) && faces(p1, p3, p0);
					const bool use13 = d02 != d13 ? d13 : p1.distance_squared_to(p3) < p0.distance_squared_to(p2);
					LocalVector<int> &out = tris[is_ground ? 1 : 0];
					const int o = use13 ? 1 : 0;
					out.push_back(vi[o]);
					out.push_back(vi[o + 1]);
					out.push_back(vi[(o + 2) % 4]);
					out.push_back(vi[o]);
					out.push_back(vi[(o + 2) % 4]);
					out.push_back(vi[(o + 3) % 4]);
				}
			}
		}
	}
	if (tris[0].is_empty() && tris[1].is_empty()) {
		return;
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for (int sfc = 0; sfc < 2; sfc++) {
		if (tris[sfc].is_empty()) {
			continue;
		}
		// Compact this surface's vertices; normals from the field's gradient (toward the open side).
		HashMap<int, int> remap;
		PackedVector3Array pv, pn;
		PackedColorArray pc;
		PackedInt32Array pi;
		for (int vi : tris[sfc]) {
			int *m = remap.getptr(vi);
			if (!m) {
				const Vector3 p = verts[vi];
				const real_t e = 0.5 * c;
				auto F = [&](const Vector3 &q) {
					const real_t r = gen ? _rock(src, gen, q) : _air(src, q);
					return drips.is_empty() ? r : _flowstone(drips, q, r);
				};
				Vector3 g(F(p + Vector3(e, 0, 0)) - F(p - Vector3(e, 0, 0)), F(p + Vector3(0, e, 0)) - F(p - Vector3(0, e, 0)), F(p + Vector3(0, 0, e)) - F(p - Vector3(0, 0, e)));
				Vector3 n = -g;
				n = n.length_squared() > 1e-12f ? n.normalized() : Vector3(0, 1, 0);
				remap.insert(vi, pv.size());
				pv.push_back(p);
				pn.push_back(n);
				// Red: how much flowstone coats it (the wall shader turns it to wet calcite).
				real_t fw = 0.0;
				if (!drips.is_empty()) {
					_flowstone(drips, p, 0.0, &fw);
				}
				pc.push_back(Color(fw, 0, 0));
				pi.push_back(pv.size() - 1);
			} else {
				pi.push_back(*m);
			}
		}
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = pv;
		arrays[Mesh::ARRAY_NORMAL] = pn;
		arrays[Mesh::ARRAY_COLOR] = pc;
		arrays[Mesh::ARRAY_INDEX] = pi;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_name(mesh->get_surface_count() - 1, sfc == 0 ? "walls" : "ground");
		r_chunk.triangles += pi.size() / 3;
	}
	r_chunk.mesh = mesh;
	// The formations of the drip points this chunk owns (by their roof, or their foot for standing ones alone).
	const AABB own(O, Vector3(chunk_size, chunk_size, chunk_size));
	auto owns = [&](const Vector3 &q) { return q.x >= O.x && q.y >= O.y && q.z >= O.z && q.x < own.get_end().x && q.y < own.get_end().y && q.z < own.get_end().z; };
	for (const Drip &d : drips) {
		const int vk = d.variant;
		const Basis spin(Vector3(0, 1, 0), d.yaw);
		if (d.kind == SPEL_COLUMN) {
			if (owns(d.foot)) {
				r_chunk.spel[SPEL_COLUMN * SPEL_VARIANTS + vk].push_back(Transform3D(spin.scaled(Vector3(d.rad, d.gap + 0.3, d.rad)), d.foot - Vector3(0, 0.15, 0)));
			}
			continue;
		}
		if (owns(d.roof)) {
			if (d.kind == SPEL_CURTAIN) {
				// x along the run, y down, z the sheet's thickness (baked in the mesh).
				const Vector3 x = d.along * d.rad, z = d.along.cross(Vector3(0, 1, 0)).normalized();
				r_chunk.spel[SPEL_CURTAIN * SPEL_VARIANTS + vk].push_back(Transform3D(Basis(x, Vector3(0, d.len + 0.15, 0), z), d.roof + Vector3(0, 0.15, 0)));
			} else {
				r_chunk.spel[SPEL_STALACTITE * SPEL_VARIANTS + vk].push_back(Transform3D(spin.scaled(Vector3(d.rad, d.len + 0.12, d.rad)), d.roof + Vector3(0, 0.12, 0)));
			}
		}
		if (d.mite > 0.0 && owns(d.foot)) {
			r_chunk.spel[SPEL_STALAGMITE * SPEL_VARIANTS + (vk + 1) % SPEL_VARIANTS].push_back(Transform3D(Basis(Vector3(0, 1, 0), d.yaw + 1.7).scaled(Vector3(d.mrad, d.mite + 0.12, d.mrad)), d.foot - Vector3(0, 0.12, 0)));
		}
	}
}

// Speleothems (docs/ideas/rocks.md, stage 3; the placement rule of Franke and Müller 2022): calcite is laid down
// where water seeping through the roof drips into air. Candidate drip points sit on a jittered 0.6 m grid, more of
// them along joint lines (seepage follows the cracks, so formations stand in rows and curtains), and more where the
// cover is thin. Each column of the grid is walked down on a fixed global step to find its roofs, so every chunk
// finds the same points. A roof needs a free fall under it and darkness (none in the first metres from an entrance,
// nor in sea caves). Under a level roof hangs a stalactite, usually with a stalagmite where the drop lands, or a
// column where the two would meet; under an inclined roof the water trickles along it and builds a curtain.
void CaveNetwork3D::_drips(const FieldSource &p_src, const TerrainGenerator3D *p_gen, const AABB &p_box, LocalVector<Drip> &r_drips) const {
	auto F = [&](const Vector3 &q) { return _rock(p_src, p_gen, q); };
	const uint32_t ns = uint32_t(seed) * 2246822519u + 29u;
	const real_t cell = 0.6, step = 0.2;
	const real_t ja = cave_hash(1, 2, 3, ns) * Math::PI;
	const Vector2 jn(Math::cos(ja), Math::sin(ja)); // across the joint lines
	const int x0 = int(Math::floor(p_box.position.x / cell)), x1 = int(Math::floor(p_box.get_end().x / cell));
	const int z0 = int(Math::floor(p_box.position.z / cell)), z1 = int(Math::floor(p_box.get_end().z / cell));
	const int y0 = int(Math::floor(p_box.position.y / step)), y1 = int(Math::ceil(p_box.get_end().y / step));
	for (int iz = z0; iz <= z1; iz++) {
		for (int ix = x0; ix <= x1; ix++) {
			const real_t px = (ix + cave_hash(ix, 0, iz, ns + 4u)) * cell, pz = (iz + cave_hash(ix, 0, iz, ns + 5u)) * cell;
			// Joint lines 3.5 m apart, broken into lengths by noise: most drips fall on them.
			const real_t across = (px * jn.x + pz * jn.y) / 3.5;
			const real_t line = Math::pow(Math::abs(Math::cos(across * Math::PI)), 10.0) * (cave_fbm(Vector3(px, 0, pz) / 6.0f, ns + 6u, 2) > 0.45f ? 1.0 : 0.0);
			// Chambers are the most decorated (big roofs, slow seepage over a wide area): twice the density.
			bool in_chamber = false;
			for (const Chamber *ch : p_src.chambers) {
				const Vector3 q = Basis(Vector3(0, 1, 0), -ch->yaw).xform(Vector3(px, ch->center.y, pz) - ch->center);
				in_chamber = in_chamber || Vector2(q.x / ch->radii.x, q.z / ch->radii.z).length_squared() < 1.0f;
			}
			const real_t dens = MIN(0.45 * speleothems * (0.2 + 1.8 * line) * (in_chamber ? 2.0 : 1.0), 0.9);
			const float pick = cave_hash(ix, 1, iz, ns);
			if (pick > dens) {
				continue;
			}
			if (px < p_box.position.x || px >= p_box.get_end().x || pz < p_box.position.z || pz >= p_box.get_end().z) {
				continue;
			}
			bool rock_above = F(Vector3(px, (y1 + 1) * step, pz)) > 0.0;
			for (int iy = y1; iy >= y0; iy--) {
				const Vector3 q(px, iy * step, pz);
				const bool rock = F(q) > 0.0;
				if (rock_above && !rock) {
					// A roof between iy and iy + 1: refine it.
					real_t lo = iy * step, hi = (iy + 1) * step;
					for (int k = 0; k < 6; k++) {
						const real_t mid = 0.5 * (lo + hi);
						(F(Vector3(px, mid, pz)) > 0.0 ? hi : lo) = mid;
					}
					const Vector3 roof(px, 0.5 * (lo + hi), pz);
					const real_t e = 0.15;
					Vector3 n(F(roof - Vector3(e, 0, 0)) - F(roof + Vector3(e, 0, 0)), F(roof - Vector3(0, e, 0)) - F(roof + Vector3(0, e, 0)), F(roof - Vector3(0, 0, e)) - F(roof + Vector3(0, 0, e)));
					n = n.length_squared() > 1e-12f ? n.normalized() : Vector3(0, -1, 0); // toward the air
					const real_t cover = p_gen->get_height(roof) - roof.y;
					const real_t seep = CLAMP(1.15 - cover / 45.0, 0.2, 1.0);
					bool dark = true;
					for (const Cave &cv : caves) {
						dark = dark && cv.entrance.distance_to(roof) > 14.0 && !(cv.sea && cv.bounds.grow(2.0).has_point(roof));
					}
					const int iyk = int(Math::floor(roof.y * 2.0));
					if (dark && n.y < -0.45f && cave_hash(ix, iyk, iz, ns + 9u) < seep) {
						real_t gap = 0.0;
						for (real_t dy = 0.1; dy < 16.0; dy += 0.1) {
							if (F(roof - Vector3(0, dy, 0)) > 0.0) {
								gap = dy;
								break;
							}
						}
						if (gap >= 0.6) {
							const float r1 = cave_hash(ix, iyk, iz, ns + 1u), r2 = cave_hash(ix, iyk, iz, ns + 2u), r3 = cave_hash(ix, iyk, iz, ns + 3u);
							Drip d;
							d.roof = roof;
							d.foot = roof - Vector3(0, gap, 0);
							d.gap = gap;
							d.yaw = r3 * Math::TAU;
							d.variant = int(cave_hash(ix, iyk, iz, ns + 7u) * float(int(SPEL_VARIANTS))) % SPEL_VARIANTS;
							// Lengths follow a power law (many short, few long), as surveys of stalactite populations find.
							d.len = MIN(0.15 + 1.8 * Math::pow(r1, 2.0f), 0.55 * gap);
							d.mite = r2 < 0.7f ? MIN(0.12 + 1.4 * Math::pow(r2 / 0.7f, 1.6f), 0.45 * gap) : 0.0;
							if (n.y > -0.85f) {
								// An inclined roof: a curtain along its fall line, 0.6 to 2.5 m long.
								d.kind = SPEL_CURTAIN;
								d.along = Vector3(n.x, 0, n.z).normalized();
								d.rad = 0.6 + 1.9 * r3;
								d.len = MIN(0.2 + 0.9 * Math::pow(r1, 1.5f), 0.5 * gap);
								d.mite = 0.0;
							} else if (d.mite > 0.0 && d.len + d.mite > 0.8 * gap && gap < 6.0) {
								d.kind = SPEL_COLUMN;
								d.rad = 0.14 + 0.2 * r2 + 0.04 * gap;
							} else {
								d.kind = SPEL_STALACTITE;
								d.rad = 0.04 + 0.13 * d.len; // a base about a sixth of the length, wider with age
								d.mrad = 0.08 + 0.17 * d.mite; // stalagmites are blunter and wider (the drop splashes)
							}
							r_drips.push_back(d);
						}
					}
				}
				rock_above = rock;
			}
		}
	}
}

// Flowstone: the calcite the drips leave round each formation's foot and at its roof, a low mound under standing
// forms (sheets where mounds meet) and a boss round hanging ones, added to the rock with a smooth maximum so the
// forms rise out of the floor and roof rather than standing on them. r_weight: how close p is to flowstone (0..1).
real_t CaveNetwork3D::_flowstone(const LocalVector<Drip> &p_drips, const Vector3 &p_p, real_t p_rock, real_t *r_weight) {
	real_t f = p_rock;
	real_t w = 0.0;
	for (const Drip &d : p_drips) {
		const bool stands = d.kind == SPEL_COLUMN || d.mite > 0.0;
		if (stands) {
			const real_t rad = d.kind == SPEL_COLUMN ? d.rad : d.mrad;
			const real_t R = 2.6 * rad + 0.35, h = 0.12 + 0.6 * rad;
			const Vector3 q = p_p - d.foot;
			if (q.length_squared() < (R + 0.6) * (R + 0.6)) {
				const Vector3 e(q.x / R, q.y / h, q.z / R);
				const real_t sd = (e.length() - 1.0) * MIN(R, h);
				const real_t k = 0.25;
				const real_t hh = CLAMP(0.5 + 0.5 * (-sd - f) / k, 0.0, 1.0);
				f = Math::lerp(f, -sd, hh) + k * hh * (1.0 - hh); // smooth max(f, -sd)
				w = MAX(w, CLAMP(1.0 - (e.length() - 1.0) / 0.6, 0.0, 1.0));
			}
		}
		if (d.kind != SPEL_COLUMN) {
			const real_t rad = d.kind == SPEL_CURTAIN ? 0.25 : d.rad;
			const real_t R = 2.2 * rad + 0.25, h = 0.1 + 0.5 * rad;
			const Vector3 q = p_p - d.roof;
			if (q.length_squared() < (R + 0.5) * (R + 0.5)) {
				const Vector3 e(q.x / R, q.y / h, q.z / R);
				const real_t sd = (e.length() - 1.0) * MIN(R, h);
				const real_t k = 0.2;
				const real_t hh = CLAMP(0.5 + 0.5 * (-sd - f) / k, 0.0, 1.0);
				f = Math::lerp(f, -sd, hh) + k * hh * (1.0 - hh);
				w = MAX(w, CLAMP(1.0 - (e.length() - 1.0) / 0.5, 0.0, 1.0));
			}
		} else {
			// A column's roof end flares into the roof too.
			const real_t R = 2.0 * d.rad + 0.3, h = 0.1 + 0.5 * d.rad;
			const Vector3 q = p_p - d.roof;
			if (q.length_squared() < (R + 0.5) * (R + 0.5)) {
				const Vector3 e(q.x / R, q.y / h, q.z / R);
				const real_t sd = (e.length() - 1.0) * MIN(R, h);
				const real_t k = 0.2;
				const real_t hh = CLAMP(0.5 + 0.5 * (-sd - f) / k, 0.0, 1.0);
				f = Math::lerp(f, -sd, hh) + k * hh * (1.0 - hh);
				w = MAX(w, CLAMP(1.0 - (e.length() - 1.0) / 0.5, 0.0, 1.0));
			}
		}
	}
	if (r_weight) {
		*r_weight = w;
	}
	return f;
}

struct CaveMeshJob {
	const CaveNetwork3D *net;
	LocalVector<Vector3i> keys;
	LocalVector<CaveNetwork3D::Chunk> made;
	LocalVector<CaveNetwork3D::Chunk *> out;
	HashSet<Vector3i> pinned;
};

void CaveNetwork3D::_mesh_job(void *p_data, uint32_t p_index) {
	CaveMeshJob *job = static_cast<CaveMeshJob *>(p_data);
	job->net->_mesh_chunk(job->keys[p_index], *job->out[p_index]);
}

Ref<Material> CaveNetwork3D::_wall_material() {
	if (wall_material.is_valid()) {
		return wall_material;
	}
	if (default_wall.is_valid()) {
		return default_wall;
	}
	// Limestone walls in world space (no UVs on a surface-nets mesh): pale grey mottled at metres, darker and wet
	// toward the floor, the sediment floor brown and smoother; no light of its own.
	Ref<Shader> sh;
	sh.instantiate();
	sh->set_code(R"(shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
uniform vec3 rock_color = vec3(0.30, 0.29, 0.26);
uniform vec3 sediment_color = vec3(0.13, 0.10, 0.07);
varying vec3 w_pos;
varying vec3 w_nrm;
float h3(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(h3(i), h3(i + vec3(1, 0, 0)), f.x), mix(h3(i + vec3(0, 1, 0)), h3(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(h3(i + vec3(0, 0, 1)), h3(i + vec3(1, 0, 1)), f.x), mix(h3(i + vec3(0, 1, 1)), h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { return 0.5 * vn(p) + 0.25 * vn(p * 2.03 + 7.0) + 0.125 * vn(p * 4.1 + 3.0) + 0.0625 * vn(p * 8.3 + 1.0); }
void vertex() {
	w_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	w_nrm = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}
// Joint cracks: two vertical joint sets (planes 1.7 and 2.3 m apart, crossing at about 70 degrees) and the bedding
// planes, each a thin dark line where the wall cuts it, wobbled and broken into lengths by noise (joints are planar
// sets, not cells).
float joints(vec3 p) {
	float wob = 0.15 * (fbm(p * 0.9) - 0.5);
	float a = abs(fract(dot(p.xz, vec2(0.94, 0.34)) / 1.7 + wob) - 0.5) * 2.0;
	float b = abs(fract(dot(p.xz, vec2(-0.17, 0.98)) / 2.3 + wob * 1.3) - 0.5) * 2.0;
	float c = abs(fract(p.y / 1.1 + 0.6 * wob) - 0.5) * 2.0;
	float ma = step(0.45, fbm(p * 0.35 + 3.0)), mb = step(0.5, fbm(p * 0.3 + 9.0)), mc = step(0.4, fbm(p * 0.25 + 15.0));
	return max(max(ma * smoothstep(0.965, 0.995, a), mb * smoothstep(0.97, 0.995, b)), mc * smoothstep(0.96, 0.99, c));
}
// The surface relief the 0.3 m mesh cannot carry: spalls and dissolution pits (fbm at 0.3 m and 8 cm), bedding
// steps, and the joints as grooves.
float relief(vec3 p) {
	float h = 0.6 * fbm(p * 3.3) + 0.25 * fbm(p * 12.0 + 5.0);
	float bed = fract(p.y / 1.1 + 0.4 * fbm(p * 0.6));
	h += 0.3 * smoothstep(0.0, 0.08, bed) * (1.0 - bed);
	h -= 0.5 * joints(p);
	return h;
}
void fragment() {
	float m = fbm(w_pos * 0.45);
	float fine = fbm(w_pos * 3.1);
	vec3 col = rock_color * (0.7 + 0.6 * m) * (0.85 + 0.3 * fine);
	// Beds: darker bands every half metre or so.
	col *= 0.85 + 0.15 * smoothstep(0.3, 0.7, sin(w_pos.y * 11.0 + 3.0 * m) * 0.5 + 0.5);
	float crack = joints(w_pos);
	col *= 1.0 - 0.35 * crack;
	float floor_w = smoothstep(0.55, 0.85, w_nrm.y);
	col = mix(col, sediment_color * (0.8 + 0.4 * fine), floor_w);
	// Flowstone streaks: water runs down the walls in sheets, leaving smooth, paler, glossy calcite in vertical
	// streaks (noise stretched along the fall line).
	float wall = 1.0 - abs(w_nrm.y);
	float streak = smoothstep(0.62, 0.8, fbm(vec3(w_pos.x * 1.7, w_pos.y * 0.18, w_pos.z * 1.7) + 23.0)) * wall;
	col = mix(col, vec3(0.48, 0.42, 0.34) * (0.85 + 0.3 * fine), streak * 0.7);
	// Flowstone mounds and bosses round the formations (the mesher's red): smooth wet calcite.
	float flow = clamp(COLOR.r, 0.0, 1.0);
	col = mix(col, vec3(0.46, 0.38, 0.28) * (0.8 + 0.35 * fine), flow * 0.8);
	streak = max(streak, flow);
	float wet = clamp(floor_w + streak + 0.4 * smoothstep(0.55, 0.8, fbm(w_pos * 0.8 + 11.0)), 0.0, 1.0);
	ALBEDO = col * mix(1.0, 0.72, wet);
	ROUGHNESS = mix(0.9, 0.25, wet);
	// The relief as a bump on the world normal (its gradient off the surface), smoothed where flowstone coats it.
	float e = 0.02;
	float h0 = relief(w_pos);
	vec3 g = vec3(relief(w_pos + vec3(e, 0, 0)) - h0, relief(w_pos + vec3(0, e, 0)) - h0, relief(w_pos + vec3(0, 0, e)) - h0) / e;
	vec3 n = normalize(w_nrm);
	g -= n * dot(g, n);
	float amount = 0.035 * (1.0 - 0.7 * streak) * (1.0 - 0.5 * floor_w);
	vec3 nb = normalize(n - amount * g);
	NORMAL = normalize((VIEW_MATRIX * vec4(nb, 0.0)).xyz);
	AO = 1.0 - 0.4 * crack;
}
)");
	default_wall.instantiate();
	default_wall->set_shader(sh);
	// The landscape's own rock colour where it has one (TerrainMaterial3D tm_rock_color), so the walls carry on the
	// cliff's rock into the cave.
	if (Ref<ShaderMaterial> gm = ground_material; gm.is_valid()) {
		const Variant rc = gm->get_shader_parameter("tm_rock_color");
		if (rc.get_type() == Variant::COLOR) {
			const Color lin = Color(rc).srgb_to_linear();
			default_wall->set_shader_parameter("rock_color", Vector3(lin.r, lin.g, lin.b));
		}
	}
	return default_wall;
}

// Chunks any passage, chamber or patch reaches inside the area; the patch chunks (whole, whatever the area: the
// landscape is open over all of the patch) also go in r_pinned; p_all_patches takes every cave's patch, near or not
// (streaming: the landscape's hole is open from any distance).
void CaveNetwork3D::_wanted(const AABB &p_area, HashSet<Vector3i> &r_want, HashSet<Vector3i> &r_pinned, bool p_all_patches) const {
	const TerrainGenerator3D *gen = _get_generator();
	auto add_box = [&](const AABB &p_b, bool p_whole) {
		const AABB b = p_whole ? p_b : p_b.intersection(p_area);
		if (!b.has_volume()) {
			return;
		}
		const Vector3i lo = Vector3i((b.position / chunk_size).floor()), hi = Vector3i((b.get_end() / chunk_size).floor());
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					r_want.insert(Vector3i(x, y, z));
					if (p_whole) {
						r_pinned.insert(Vector3i(x, y, z));
					}
				}
			}
		}
	};
	for (const Cave &cv : caves) {
		if (!cv.bounds.grow(chunk_size).intersects(p_area)) {
			continue;
		}
		for (const Segment &sg : cv.segments) {
			AABB b(sg.a, Vector3());
			b.expand_to(sg.b);
			add_box(b.grow(MAX(sg.ra, sg.rb) * 2.0 + 1.5), false);
		}
		for (const Chamber &ch : cv.chambers) {
			add_box(AABB(ch.center - ch.radii, ch.radii * 2.0).grow(1.5), false);
		}
	}
	if (patch_radius <= 0.0) {
		return;
	}
	for (const Cave &cv : caves) {
		// The patch: from the foot to the top of the wall within the radius.
		const real_t pr = patch_radius * 1.4143;
		if (!p_all_patches && !p_area.intersects(AABB(cv.entrance - Vector3(pr, 4.0, pr), Vector3(2.0 * pr, 8.0, 2.0 * pr)))) {
			continue;
		}
		real_t top = cv.entrance.y + 4.0;
		if (gen) {
			for (real_t dz = -pr; dz <= pr; dz += 2.0) {
				for (real_t dx = -pr; dx <= pr; dx += 2.0) {
					top = MAX(top, gen->get_height(cv.entrance + Vector3(dx, 0, dz)) + 1.0);
				}
			}
		}
		add_box(AABB(cv.entrance - Vector3(pr, 4.0, pr), Vector3(2.0 * pr, top - cv.entrance.y + 4.0, 2.0 * pr)), true);
	}
}

// The unit speleothem shapes. Stalactites take the shape Short, Baygents and Goldstein found for them ("Stalactite
// growth as a free-boundary problem", Physics of Fluids 2005): a long carrot taper from a flared base to a thin
// soda-straw drip tip, the same for all sizes. Stalagmites are candles with a rounded, splash-flattened top and a
// spread foot; columns are both, fused. Curtains are thin folded sheets hanging along an inclined roof, their lower
// edge scalloped into drip tips. Each kind has four variants: lumpy, bent, some fused twins, never a clean lathe.
// UV.y runs 0 at the base (roof or floor) to 1 at the tip, for the shader's wet tips and dark, muddy feet.
Ref<ArrayMesh> CaveNetwork3D::_spel_mesh(int p_kind, int p_variant) {
	const int key = p_kind * SPEL_VARIANTS + p_variant;
	if (spel_mesh[key].is_valid()) {
		return spel_mesh[key];
	}
	RandomPCG rng(uint32_t(p_kind * 977 + p_variant * 131 + 7));
	PackedVector3Array pv, pn;
	PackedVector2Array puv;
	PackedInt32Array pi;
	const uint32_t ns = uint32_t(key) * 7919u + 3u;
	// One turned tube along a bent axis, lumpy (3D noise on the radius, not rings); dir -1 hangs, +1 stands.
	auto tube = [&](real_t p_dir, real_t p_scale, const Vector3 &p_offset, real_t p_bend, real_t p_lean, auto &&p_radius) {
		const int R = 18, H = 44;
		const int base = pv.size();
		const real_t lean_a = rng.randf() * Math::TAU;
		const Vector2 lean(Math::cos(lean_a), Math::sin(lean_a));
		for (int j = 0; j <= H; j++) {
			const real_t t = real_t(j) / H;
			const Vector3 axis = p_offset + Vector3(lean.x * (p_lean * t + p_bend * t * t), p_dir * t * p_scale, lean.y * (p_lean * t + p_bend * t * t));
			for (int i = 0; i < R; i++) {
				const real_t a = Math::TAU * i / R;
				const Vector3 dir(Math::cos(a), 0, Math::sin(a));
				const real_t lump = 1.0 + 0.32 * (cave_fbm(dir * 1.4f + Vector3(0, t * 4.5f, 0), ns, 3) - 0.5f) + 0.12 * (cave_fbm(dir * 4.0f + Vector3(0, t * 14.0f, 0), ns + 1u, 2) - 0.5f);
				const real_t r = p_radius(t) * lump * p_scale;
				pv.push_back(axis + dir * r);
				puv.push_back(Vector2(real_t(i) / R, t));
			}
		}
		for (int j = 0; j <= H; j++) {
			for (int i = 0; i < R; i++) {
				const Vector3 du = pv[base + j * R + (i + 1) % R] - pv[base + j * R + (i + R - 1) % R];
				const Vector3 dv = pv[base + MIN(j + 1, H) * R + i] - pv[base + MAX(j - 1, 0) * R + i];
				Vector3 n = du.cross(dv);
				const Vector3 c = p_offset + Vector3(0, p_dir * real_t(j) / H * p_scale, 0);
				const Vector3 out = pv[base + j * R + i] - c;
				if (n.dot(Vector3(out.x, 0, out.z)) < 0.0) {
					n = -n;
				}
				pn.push_back(n.length_squared() > 1e-12f ? n.normalized() : Vector3(0, p_dir, 0));
			}
		}
		for (int j = 0; j < H; j++) {
			for (int i = 0; i < R; i++) {
				const int a = base + j * R + i, b = base + j * R + (i + 1) % R, c = base + (j + 1) * R + i, d = base + (j + 1) * R + (i + 1) % R;
				if (p_dir > 0.0) {
					pi.append_array({ a, b, c, b, d, c });
				} else {
					pi.append_array({ a, c, b, b, c, d });
				}
			}
		}
	};
	const real_t bend = (rng.randf() - 0.5) * 0.25, lean = (rng.randf() - 0.5) * 0.12;
	if (p_kind == SPEL_STALACTITE) {
		// The carrot: r = (1 - t)^0.9 flared at the base, down to a soda straw of a tenth of the base radius in the
		// last eighth, closed in a round drip tip.
		auto carrot = [](real_t t) -> real_t {
			const real_t body = Math::pow(MAX(1.0 - t, 0.0), 0.9) * (1.0 + 0.6 * Math::pow(MAX(1.0 - t, 0.0), 10.0));
			const real_t straw = 0.1 * Math::sqrt(MAX(1.0 - Math::pow((t - 0.86) / 0.14, 8.0), 0.0));
			return t < 0.86 ? MAX(body, real_t(0.1)) : straw;
		};
		tube(-1.0, 1.0, Vector3(), bend, lean, carrot);
		if (p_variant >= 2) {
			// Fused twins (and a third in the last variant): younger stalactites grown from the same crack.
			tube(-1.0, 0.55 + 0.2 * rng.randf(), Vector3(0.55, 0.0, 0.25), -bend, -lean, carrot);
			if (p_variant == 3) {
				tube(-1.0, 0.35 + 0.15 * rng.randf(), Vector3(-0.35, 0.0, 0.5), bend * 0.5, lean, carrot);
			}
		}
	} else if (p_kind == SPEL_STALAGMITE) {
		auto candle = [](real_t t) -> real_t {
			const real_t foot = 1.0 + 0.9 * Math::pow(MAX(1.0 - t, 0.0), 7.0); // spread on the floor
			const real_t top = Math::sqrt(MAX(1.0 - Math::pow(double(t), 6.0), 0.0)); // rounded, splash-flattened
			return 0.62 * foot * top;
		};
		tube(1.0, 1.0, Vector3(), bend * 0.5, lean, candle);
		if (p_variant == 3) {
			tube(1.0, 0.5, Vector3(0.6, 0.0, 0.2), 0.0, -lean, candle);
		}
	} else if (p_kind == SPEL_COLUMN) {
		auto column = [](real_t t) -> real_t {
			const double m = Math::abs(2.0 * t - 1.0);
			return 0.5 + 0.35 * Math::pow(m, 3.0) + 0.5 * Math::pow(m, 14.0);
		};
		tube(1.0, 1.0, Vector3(), 0.0, 0.0, column);
	} else {
		// A curtain: a sheet 1 long (x, -0.5..0.5) hanging 1 down, folded in z, 3 to 5 cm thick, its lower edge
		// scalloped into drip tips; built as two faces and a rim.
		const int U = 64, V = 20;
		const real_t thick = 0.02 + 0.01 * rng.randf();
		const real_t fold_f = 3.0 + 3.0 * rng.randf(), fold_a = 0.05 + 0.05 * rng.randf();
		auto depth = [&](real_t u) -> real_t {
			const real_t env = Math::sqrt(MAX(1.0 - Math::pow(2.0 * Math::abs(u), 4.0), 0.0));
			const real_t tips = 0.75 + 0.25 * Math::pow(Math::abs(Math::sin(u * Math::PI * (9.0 + p_variant * 2.0))), 0.5);
			return env * tips * (0.85 + 0.3 * (cave_fbm(Vector3(u * 6.0f, 0, 0), ns + 3u, 2) - 0.5f));
		};
		for (int side = 0; side < 2; side++) {
			const real_t sz = side == 0 ? 1.0 : -1.0;
			const int base = pv.size();
			for (int j = 0; j <= V; j++) {
				const real_t t = real_t(j) / V;
				for (int i = 0; i <= U; i++) {
					const real_t u = real_t(i) / U - 0.5;
					const real_t y = -t * depth(u);
					const real_t fold = fold_a * Math::sin(u * Math::TAU * fold_f + 1.3 * p_variant) * (0.4 + 0.6 * t);
					pv.push_back(Vector3(u, y, fold + sz * thick * (1.0 - 0.6 * t)));
					puv.push_back(Vector2(u + 0.5, t));
					// Normal: the fold's slope, facing this side.
					const real_t dz = fold_a * Math::TAU * fold_f * Math::cos(u * Math::TAU * fold_f + 1.3 * p_variant) * (0.4 + 0.6 * t);
					pn.push_back(Vector3(-dz * sz, 0.0, sz).normalized());
				}
			}
			for (int j = 0; j < V; j++) {
				for (int i = 0; i < U; i++) {
					const int a = base + j * (U + 1) + i, b = a + 1, c = a + (U + 1), d = c + 1;
					if (side == 0) {
						pi.append_array({ a, c, b, b, c, d });
					} else {
						pi.append_array({ a, b, c, b, d, c });
					}
				}
			}
		}
		// The rim along the lower edge joins the two faces.
		const int f0 = V * (U + 1), f1 = (V + 1) * (U + 1) + V * (U + 1);
		for (int i = 0; i < U; i++) {
			const int a = f0 + i, b = f0 + i + 1, c = f1 + i, d = f1 + i + 1;
			pi.append_array({ a, b, c, b, d, c });
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = pv;
	arrays[Mesh::ARRAY_NORMAL] = pn;
	arrays[Mesh::ARRAY_TEX_UV] = puv;
	arrays[Mesh::ARRAY_INDEX] = pi;
	spel_mesh[key].instantiate();
	spel_mesh[key]->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (spel_material.is_null()) {
		// Wet calcite: cream to amber, translucent-looking where thin, glossy where water runs (the tips and the
		// curtain edges), dark and muddy at the feet of standing forms, banded faintly with growth layers.
		Ref<Shader> sh;
		sh.instantiate();
		sh->set_code(R"(shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
uniform vec3 calcite = vec3(0.58, 0.49, 0.37);
uniform vec3 amber = vec3(0.45, 0.30, 0.17);
uniform vec3 mud = vec3(0.16, 0.12, 0.09);
varying vec3 w_pos;
varying float up;
float h3(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vn(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(h3(i), h3(i + vec3(1, 0, 0)), f.x), mix(h3(i + vec3(0, 1, 0)), h3(i + vec3(1, 1, 0)), f.x), f.y),
			mix(mix(h3(i + vec3(0, 0, 1)), h3(i + vec3(1, 0, 1)), f.x), mix(h3(i + vec3(0, 1, 1)), h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { return 0.5 * vn(p) + 0.25 * vn(p * 2.03 + 7.0) + 0.125 * vn(p * 4.1 + 3.0); }
void vertex() {
	w_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	up = (MODEL_MATRIX * vec4(0.0, 1.0, 0.0, 0.0)).y; // < 0 never; the kind comes from the base height below
}
void fragment() {
	float t = UV.y;
	float m = fbm(w_pos * 6.0);
	vec3 col = mix(calcite, amber, 0.25 + 0.5 * fbm(w_pos * 1.3 + 4.0));
	col *= 0.8 + 0.35 * m;
	// Growth layers, faint and irregular.
	col *= 0.92 + 0.08 * sin(t * 60.0 + 8.0 * m);
	// Wet: the tip (where the drop hangs) and the feet; the feet also take mud.
	float tip = smoothstep(0.7, 1.0, t);
	float foot = 1.0 - smoothstep(0.0, 0.18, t);
	col = mix(col, mud * (0.8 + 0.4 * m), foot * 0.75);
	float wet = max(tip, foot);
	ALBEDO = col * mix(1.0, 0.75, wet);
	ROUGHNESS = mix(0.5, 0.12, wet);
	SPECULAR = 0.6;
}
)");
		spel_material.instantiate();
		spel_material->set_shader(sh);
	}
	spel_mesh[key]->surface_set_material(0, spel_material);
	return spel_mesh[key];
}

void CaveNetwork3D::_add_chunk(Chunk &p_chunk, const Ref<Material> &p_wall) {
	if (p_chunk.mesh.is_null()) {
		return;
	}
	MeshInstance3D *mi = memnew(MeshInstance3D);
	mi->set_name(vformat("Chunk %d %d %d", p_chunk.key.x, p_chunk.key.y, p_chunk.key.z));
	mi->set_mesh(p_chunk.mesh);
	for (int sfc = 0; sfc < p_chunk.mesh->get_surface_count(); sfc++) {
		const bool is_ground = p_chunk.mesh->surface_get_name(sfc) == "ground";
		mi->set_surface_override_material(sfc, is_ground && ground_material.is_valid() ? ground_material : p_wall);
	}
	add_child(mi);
	mi->set_global_transform(Transform3D());
	if (collision) {
		StaticBody3D *body = memnew(StaticBody3D);
		CollisionShape3D *cs = memnew(CollisionShape3D);
		Ref<ConcavePolygonShape3D> shape;
		shape.instantiate();
		const Vector<Face3> faces = p_chunk.mesh->get_faces();
		PackedVector3Array fv;
		fv.resize(faces.size() * 3);
		for (int fi = 0; fi < faces.size(); fi++) {
			for (int k = 0; k < 3; k++) {
				fv.write[fi * 3 + k] = faces[fi].vertex[k];
			}
		}
		shape->set_faces(fv);
		shape->set_backface_collision_enabled(true);
		cs->set_shape(shape);
		body->add_child(cs);
		mi->add_child(body);
	}
	static const char *spel_names[SPEL_KINDS] = { "Stalactites", "Stalagmites", "Columns", "Curtains" };
	for (int k = 0; k < SPEL_KINDS * SPEL_VARIANTS; k++) {
		if (p_chunk.spel[k].is_empty()) {
			continue;
		}
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_mesh(_spel_mesh(k / SPEL_VARIANTS, k % SPEL_VARIANTS));
		mm->set_instance_count(int(p_chunk.spel[k].size()));
		for (int i = 0; i < p_chunk.spel[k].size(); i++) {
			mm->set_instance_transform(i, p_chunk.spel[k][i]);
		}
		MultiMeshInstance3D *mmi = memnew(MultiMeshInstance3D);
		mmi->set_name(vformat("%s %d", spel_names[k / SPEL_VARIANTS], k % SPEL_VARIANTS));
		mmi->set_multimesh(mm);
		mi->add_child(mmi);
	}
	p_chunk.node = mi;
}

// The landscape opens over each patch (less a 1.5 m ring the patch overlaps), once per cave.
void CaveNetwork3D::_open_holes() {
	TerrainGenerator3D *gen = _get_generator();
	if (!gen || patch_radius <= 1.5) {
		return;
	}
	for (Cave &cv : caves) {
		if (cv.holed) {
			continue;
		}
		const real_t hr = patch_radius - 1.5;
		const Vector3 o = Vector3(cv.out.x, 0, cv.out.z).normalized();
		const Basis b(Vector3(-o.z, 0, o.x) * hr, Vector3(0, 500.0, 0), o * hr);
		gen->add_hole(Transform3D(b, cv.entrance + Vector3(0, 490.0, 0)));
		for (const Vector3 &bh : cv.blowholes) {
			// The blowhole's mouth: a round-ish box a little inside the shaft's top, so the shaft's own wall meets the
			// ground's edge.
			const real_t br = 0.9;
			gen->add_hole(Transform3D(Basis().scaled(Vector3(br, 6.0, br)), bh));
		}
		cv.holed = true;
	}
}

int CaveNetwork3D::build_meshes(const AABB &p_area) {
	_stream_finish(true);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	HashSet<Vector3i> want, pinned;
	_wanted(p_area, want, pinned);
	mesh_gen = _get_generator();
	CaveMeshJob job;
	job.net = this;
	for (const Vector3i &k : want) {
		if (!chunks.has(k)) {
			job.keys.push_back(k);
		}
	}
	job.made.resize(job.keys.size());
	for (uint32_t i = 0; i < job.made.size(); i++) {
		job.out.push_back(&job.made[i]);
	}
	if (!job.keys.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&_mesh_job, &job, int(job.keys.size()), -1, true, "CaveNetwork3D meshes");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
	const Ref<Material> wm = _wall_material();
	int count = 0;
	for (Chunk &ch : job.made) {
		ch.pinned = pinned.has(ch.key);
		_add_chunk(ch, wm);
		// Empty chunks are kept too (no node), so they are not meshed again.
		chunks.insert(ch.key, ch);
		count += ch.node ? 1 : 0;
	}
	_open_holes();
	last_mesh_usec = OS::get_singleton()->get_ticks_usec() - t0;
	return count;
}

void CaveNetwork3D::set_follow_target(const NodePath &p_path) {
	follow_target = p_path;
	streamed = false;
	if (is_inside_tree()) {
		set_process_internal(!follow_target.is_empty());
	}
}

// One step of streaming: integrate a finished batch, else start the next one when the target has moved 4 m (or
// nothing streamed yet). Freed: chunks past stream_radius plus one chunk, unless pinned.
void CaveNetwork3D::_stream_update() {
	if (stream_job) {
		if (!WorkerThreadPool::get_singleton()->is_group_task_completed(stream_group)) {
			return;
		}
		_stream_finish(true);
	}
	const Node3D *target = Object::cast_to<Node3D>(get_node_or_null(follow_target));
	if (!target || caves.is_empty()) {
		return;
	}
	const Vector3 p = target->get_global_position();
	if (streamed && p.distance_to(stream_from) < 4.0) {
		return;
	}
	stream_from = p;
	streamed = true;
	const real_t r = stream_radius;
	const real_t half = chunk_size * 0.5;
	auto centre = [&](const Vector3i &k) { return (Vector3(k) + Vector3(0.5, 0.5, 0.5)) * chunk_size; };
	// Free the far ones.
	LocalVector<Vector3i> drop;
	for (const KeyValue<Vector3i, Chunk> &kv : chunks) {
		if (!kv.value.pinned && centre(kv.key).distance_to(p) > r + chunk_size * 1.8) {
			drop.push_back(kv.key);
		}
	}
	for (const Vector3i &k : drop) {
		if (chunks[k].node) {
			chunks[k].node->queue_free();
		}
		chunks.erase(k);
	}
	// Mesh the near ones (nearest first, so the batch fills in from the target outwards).
	HashSet<Vector3i> want, pinned;
	_wanted(AABB(p - Vector3(r, r, r), Vector3(2.0 * r, 2.0 * r, 2.0 * r)), want, pinned, true);
	LocalVector<Vector3i> keys;
	for (const Vector3i &k : want) {
		if (!chunks.has(k) && (pinned.has(k) || centre(k).distance_to(p) <= r + half)) {
			keys.push_back(k);
		}
	}
	_open_holes();
	if (keys.is_empty()) {
		return;
	}
	struct Near {
		Vector3 p;
		real_t cs = 16.0;
		bool operator()(const Vector3i &a, const Vector3i &b) const {
			return ((Vector3(a) + Vector3(0.5, 0.5, 0.5)) * cs).distance_squared_to(p) < ((Vector3(b) + Vector3(0.5, 0.5, 0.5)) * cs).distance_squared_to(p);
		}
	};
	SortArray<Vector3i, Near> sorter;
	sorter.compare.p = p;
	sorter.compare.cs = chunk_size;
	sorter.sort(keys.ptr(), keys.size());
	mesh_gen = _get_generator();
	stream_job = memnew(CaveMeshJob);
	stream_job->net = this;
	stream_job->keys = keys;
	stream_job->pinned = pinned;
	stream_job->made.resize(keys.size());
	for (uint32_t i = 0; i < keys.size(); i++) {
		stream_job->out.push_back(&stream_job->made[i]);
	}
	stream_group = WorkerThreadPool::get_singleton()->add_native_group_task(&_mesh_job, stream_job, int(keys.size()), -1, false, "CaveNetwork3D streaming");
}

// Waits for the batch in flight; p_keep adds its chunks, else drops them (the caves changed under it).
void CaveNetwork3D::_stream_finish(bool p_keep) {
	if (!stream_job) {
		return;
	}
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(stream_group);
	if (p_keep && is_inside_tree()) {
		const Ref<Material> wm = _wall_material();
		for (Chunk &ch : stream_job->made) {
			if (chunks.has(ch.key)) {
				continue;
			}
			ch.pinned = stream_job->pinned.has(ch.key);
			_add_chunk(ch, wm);
			chunks.insert(ch.key, ch);
		}
	}
	memdelete(stream_job);
	stream_job = nullptr;
	stream_group = -1;
}

void CaveNetwork3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(!follow_target.is_empty());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_stream_update();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_stream_finish(false);
		} break;
		case NOTIFICATION_PREDELETE: {
			_stream_finish(false);
		} break;
	}
}

void CaveNetwork3D::clear_meshes() {
	_stream_finish(false);
	streamed = false;
	for (KeyValue<Vector3i, Chunk> &kv : chunks) {
		if (kv.value.node) {
			kv.value.node->queue_free();
		}
	}
	chunks.clear();
}

int CaveNetwork3D::get_speleothem_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, Chunk> &kv : chunks) {
		for (int k = 0; k < SPEL_KINDS * SPEL_VARIANTS; k++) {
			n += kv.value.node ? int(kv.value.spel[k].size()) : 0;
		}
	}
	return n;
}

int CaveNetwork3D::get_mesh_chunk_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, Chunk> &kv : chunks) {
		n += kv.value.node ? 1 : 0;
	}
	return n;
}

int CaveNetwork3D::get_mesh_triangle_count() const {
	int n = 0;
	for (const KeyValue<Vector3i, Chunk> &kv : chunks) {
		n += kv.value.triangles;
	}
	return n;
}

void CaveNetwork3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_cave", "entrance", "out"), &CaveNetwork3D::add_cave);
	ClassDB::bind_method(D_METHOD("clear_caves"), &CaveNetwork3D::clear_caves);
	ClassDB::bind_method(D_METHOD("add_sea_cave", "entrance", "out"), &CaveNetwork3D::add_sea_cave);
	ClassDB::bind_method(D_METHOD("find_sea_entrances", "rect", "max"), &CaveNetwork3D::find_sea_entrances, DEFVAL(4));
	ClassDB::bind_method(D_METHOD("is_sea_cave", "cave"), &CaveNetwork3D::is_sea_cave);
	ClassDB::bind_method(D_METHOD("get_blowholes", "cave"), &CaveNetwork3D::get_blowholes);
	ClassDB::bind_method(D_METHOD("get_cave_count"), &CaveNetwork3D::get_cave_count);
	ClassDB::bind_method(D_METHOD("get_segments", "cave"), &CaveNetwork3D::get_segments);
	ClassDB::bind_method(D_METHOD("get_chambers", "cave"), &CaveNetwork3D::get_chambers);
	ClassDB::bind_method(D_METHOD("get_key_points", "cave"), &CaveNetwork3D::get_key_points);
	ClassDB::bind_method(D_METHOD("find_entrances", "rect", "max"), &CaveNetwork3D::find_entrances, DEFVAL(8));
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &CaveNetwork3D::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_sample_count"), &CaveNetwork3D::get_last_sample_count);

	ClassDB::bind_method(D_METHOD("build_meshes", "area"), &CaveNetwork3D::build_meshes);
	ClassDB::bind_method(D_METHOD("clear_meshes"), &CaveNetwork3D::clear_meshes);
	ClassDB::bind_method(D_METHOD("get_mesh_chunk_count"), &CaveNetwork3D::get_mesh_chunk_count);
	ClassDB::bind_method(D_METHOD("is_streaming"), &CaveNetwork3D::is_streaming);
	ClassDB::bind_method(D_METHOD("get_speleothem_count"), &CaveNetwork3D::get_speleothem_count);
	ClassDB::bind_method(D_METHOD("set_scallops", "amount"), &CaveNetwork3D::set_scallops);
	ClassDB::bind_method(D_METHOD("get_scallops"), &CaveNetwork3D::get_scallops);
	ClassDB::bind_method(D_METHOD("set_speleothems", "amount"), &CaveNetwork3D::set_speleothems);
	ClassDB::bind_method(D_METHOD("get_speleothems"), &CaveNetwork3D::get_speleothems);
	ClassDB::bind_method(D_METHOD("set_follow_target", "path"), &CaveNetwork3D::set_follow_target);
	ClassDB::bind_method(D_METHOD("get_follow_target"), &CaveNetwork3D::get_follow_target);
	ClassDB::bind_method(D_METHOD("set_stream_radius", "metres"), &CaveNetwork3D::set_stream_radius);
	ClassDB::bind_method(D_METHOD("get_stream_radius"), &CaveNetwork3D::get_stream_radius);
	ClassDB::bind_method(D_METHOD("get_mesh_triangle_count"), &CaveNetwork3D::get_mesh_triangle_count);
	ClassDB::bind_method(D_METHOD("get_last_mesh_msec"), &CaveNetwork3D::get_last_mesh_msec);
	ClassDB::bind_method(D_METHOD("get_rock_distance", "position"), &CaveNetwork3D::get_rock_distance);
	ClassDB::bind_method(D_METHOD("set_mesh_cell", "metres"), &CaveNetwork3D::set_mesh_cell);
	ClassDB::bind_method(D_METHOD("get_mesh_cell"), &CaveNetwork3D::get_mesh_cell);
	ClassDB::bind_method(D_METHOD("set_patch_radius", "metres"), &CaveNetwork3D::set_patch_radius);
	ClassDB::bind_method(D_METHOD("get_patch_radius"), &CaveNetwork3D::get_patch_radius);
	ClassDB::bind_method(D_METHOD("set_roughness", "value"), &CaveNetwork3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &CaveNetwork3D::get_roughness);
	ClassDB::bind_method(D_METHOD("set_collision", "enable"), &CaveNetwork3D::set_collision);
	ClassDB::bind_method(D_METHOD("has_collision"), &CaveNetwork3D::has_collision);
	ClassDB::bind_method(D_METHOD("set_wall_material", "material"), &CaveNetwork3D::set_wall_material);
	ClassDB::bind_method(D_METHOD("get_wall_material"), &CaveNetwork3D::get_wall_material);
	ClassDB::bind_method(D_METHOD("set_ground_material", "material"), &CaveNetwork3D::set_ground_material);
	ClassDB::bind_method(D_METHOD("get_ground_material"), &CaveNetwork3D::get_ground_material);

	ClassDB::bind_method(D_METHOD("set_generator", "path"), &CaveNetwork3D::set_generator);
	ClassDB::bind_method(D_METHOD("get_generator"), &CaveNetwork3D::get_generator);
	ClassDB::bind_method(D_METHOD("set_rock_field", "path"), &CaveNetwork3D::set_rock_field);
	ClassDB::bind_method(D_METHOD("get_rock_field"), &CaveNetwork3D::get_rock_field);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &CaveNetwork3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &CaveNetwork3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_cave_length", "metres"), &CaveNetwork3D::set_cave_length);
	ClassDB::bind_method(D_METHOD("get_cave_length"), &CaveNetwork3D::get_cave_length);
	ClassDB::bind_method(D_METHOD("set_sample_spacing", "metres"), &CaveNetwork3D::set_sample_spacing);
	ClassDB::bind_method(D_METHOD("get_sample_spacing"), &CaveNetwork3D::get_sample_spacing);
	ClassDB::bind_method(D_METHOD("set_bedding_spacing", "metres"), &CaveNetwork3D::set_bedding_spacing);
	ClassDB::bind_method(D_METHOD("get_bedding_spacing"), &CaveNetwork3D::get_bedding_spacing);
	ClassDB::bind_method(D_METHOD("set_cover", "metres"), &CaveNetwork3D::set_cover);
	ClassDB::bind_method(D_METHOD("get_cover"), &CaveNetwork3D::get_cover);
	ClassDB::bind_method(D_METHOD("set_gamma", "gamma"), &CaveNetwork3D::set_gamma);
	ClassDB::bind_method(D_METHOD("get_gamma"), &CaveNetwork3D::get_gamma);
	ClassDB::bind_method(D_METHOD("set_tortuosity", "value"), &CaveNetwork3D::set_tortuosity);
	ClassDB::bind_method(D_METHOD("get_tortuosity"), &CaveNetwork3D::get_tortuosity);
	ClassDB::bind_method(D_METHOD("set_side_passages", "count"), &CaveNetwork3D::set_side_passages);
	ClassDB::bind_method(D_METHOD("get_side_passages"), &CaveNetwork3D::get_side_passages);
	ClassDB::bind_method(D_METHOD("set_chamber_chance", "chance"), &CaveNetwork3D::set_chamber_chance);
	ClassDB::bind_method(D_METHOD("get_chamber_chance"), &CaveNetwork3D::get_chamber_chance);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "generator", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainGenerator3D"), "set_generator", "get_generator");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "rock_field", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "RockField3D"), "set_rock_field", "get_rock_field");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_GROUP("Network", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cave_length", PROPERTY_HINT_RANGE, "20,2000,1,suffix:m"), "set_cave_length", "get_cave_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sample_spacing", PROPERTY_HINT_RANGE, "2,50,0.5,suffix:m"), "set_sample_spacing", "get_sample_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bedding_spacing", PROPERTY_HINT_RANGE, "2,200,0.5,suffix:m"), "set_bedding_spacing", "get_bedding_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cover", PROPERTY_HINT_RANGE, "1,100,0.5,suffix:m"), "set_cover", "get_cover");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gamma", PROPERTY_HINT_RANGE, "0.5,10,0.05"), "set_gamma", "get_gamma");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tortuosity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_tortuosity", "get_tortuosity");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "side_passages", PROPERTY_HINT_RANGE, "0,64,1"), "set_side_passages", "get_side_passages");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chamber_chance", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_chamber_chance", "get_chamber_chance");

	ADD_GROUP("Meshes", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mesh_cell", PROPERTY_HINT_RANGE, "0.05,4,0.01,suffix:m"), "set_mesh_cell", "get_mesh_cell");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_radius", PROPERTY_HINT_RANGE, "0,100,0.5,suffix:m"), "set_patch_radius", "get_patch_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scallops", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_scallops", "get_scallops");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speleothems", PROPERTY_HINT_RANGE, "0,4,0.01"), "set_speleothems", "get_speleothems");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision"), "set_collision", "has_collision");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "wall_material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_wall_material", "get_wall_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ground_material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_ground_material", "get_ground_material");
	ADD_GROUP("Streaming", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "follow_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_follow_target", "get_follow_target");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stream_radius", PROPERTY_HINT_RANGE, "8,512,1,suffix:m"), "set_stream_radius", "get_stream_radius");

	BIND_ENUM_CONSTANT(PASSAGE_ENTRANCE);
	BIND_ENUM_CONSTANT(PASSAGE_TUBE);
	BIND_ENUM_CONSTANT(PASSAGE_BED);
	BIND_ENUM_CONSTANT(PASSAGE_KEYHOLE);
	BIND_ENUM_CONSTANT(PASSAGE_CANYON);
	BIND_ENUM_CONSTANT(PASSAGE_SHAFT);
	BIND_ENUM_CONSTANT(PASSAGE_SEA);
	BIND_ENUM_CONSTANT(PASSAGE_MAX);
}
