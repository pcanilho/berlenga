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
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "modules/terrain_deform/terrain_erosion_filter.h"
#include "modules/terrain_deform/terrain_generator_3d.h"

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
	caves.push_back(c);
	return int(caves.size()) - 1;
}

void CaveNetwork3D::clear_caves() {
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

void CaveNetwork3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("add_cave", "entrance", "out"), &CaveNetwork3D::add_cave);
	ClassDB::bind_method(D_METHOD("clear_caves"), &CaveNetwork3D::clear_caves);
	ClassDB::bind_method(D_METHOD("get_cave_count"), &CaveNetwork3D::get_cave_count);
	ClassDB::bind_method(D_METHOD("get_segments", "cave"), &CaveNetwork3D::get_segments);
	ClassDB::bind_method(D_METHOD("get_chambers", "cave"), &CaveNetwork3D::get_chambers);
	ClassDB::bind_method(D_METHOD("get_key_points", "cave"), &CaveNetwork3D::get_key_points);
	ClassDB::bind_method(D_METHOD("find_entrances", "rect", "max"), &CaveNetwork3D::find_entrances, DEFVAL(8));
	ClassDB::bind_method(D_METHOD("get_last_build_msec"), &CaveNetwork3D::get_last_build_msec);
	ClassDB::bind_method(D_METHOD("get_last_sample_count"), &CaveNetwork3D::get_last_sample_count);

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

	BIND_ENUM_CONSTANT(PASSAGE_ENTRANCE);
	BIND_ENUM_CONSTANT(PASSAGE_TUBE);
	BIND_ENUM_CONSTANT(PASSAGE_BED);
	BIND_ENUM_CONSTANT(PASSAGE_KEYHOLE);
	BIND_ENUM_CONSTANT(PASSAGE_CANYON);
	BIND_ENUM_CONSTANT(PASSAGE_SHAFT);
	BIND_ENUM_CONSTANT(PASSAGE_MAX);
}
