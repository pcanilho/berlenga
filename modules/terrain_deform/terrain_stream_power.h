/**************************************************************************/
/*  terrain_stream_power.h                                                */
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

#pragma once

// Uplift plus fluvial erosion by the stream power law, the large-scale landscape model of
//   Cordonnier, Braun, Cani, Benes, Galin, Peytavie, Guerin, "Large Scale Terrain Generation from Tectonic Uplift
//   and Fluvial Erosion", Eurographics 2016,
// solved with the implicit O(n) scheme of
//   Braun, Willett, "A very efficient O(n), implicit and parallel method to solve the stream power equation",
//   Geomorphology 2013:
//   dh/dt = U - K A^m S   (n = 1), steepest-descent (D8) receivers, a stack ordered from the outlets upstream,
//   drainage area accumulated down the stack, then h_i = (h_i + dt U_i + F h_r) / (1 + F), F = K dt A^m / d_ir,
//   which is unconditionally stable for any time step.
// Additions:
//   - Depressions: Priority-Flood with epsilon (Barnes, Lehman, Mulla 2014) fills closed basins only when a step
//     leaves any, so every cell drains to an outlet; the fill is kept as deposited sediment (lakes filled by
//     sediment, the flat valley floors of an alluvial plain).
//   - Critical slope (threshold hillslopes, landsliding): h_i <= h_r + tan(theta) d_ir along the receiver tree, as
//     the rock-strength limit of Cordonnier 2016 and the debris slope of Schott et al. 2023.
//   - Hillslope diffusion dh/dt = k_d Laplacian(h) (Schott et al. 2023's "hillslope" term), explicit with stable
//     sub-steps, for rounded soil-mantled hilltops.
// Plain C++ on caller-owned buffers, single-threaded and in a fixed order, so a seed gives the same terrain on
// every machine.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class TerrainStreamPower {
public:
	struct Params {
		int nx = 0;
		int ny = 0;
		double dx = 16.0; // metres between grid nodes
		double dt = 20000.0; // years per step
		double erodibility = 2.0e-5; // K, 1/yr for m = 0.5
		double area_exponent = 0.5; // m (n = 1)
		double diffusion = 0.0; // k_d, m^2/yr
		double talus = 0.85; // tan(critical slope angle); <= 0 disables
		double base_level = 0.0; // outlet height
		int outlet_edges = 15; // bit 0 -x, 1 +x, 2 -z, 3 +z
		double fill_epsilon = 1.0e-3; // metres per cell drop forced across filled flats
		// Drainage area by multiple flow directions, each lower neighbour receiving slope^p of the flow
		// (Freeman 1991; Holmgren 1994; the "p_flow" of Schott et al. 2023). Removes the straight 45 degree valleys
		// single-direction (D8) routing leaves on a grid. 0 = D8.
		double flow_exponent = 1.1;
		// Sediment deposition, G of Yuan, Braun, Guerit et al. 2019 (JGR Earth Surface): dh/dt = U - K A^m S +
		// (G / A) Qs, Qs the sediment flux from upstream. 0 = detachment-limited (pure incision); around 1 rivers
		// drop much of their load where they slow, filling valleys into flat alluvial floors and floodplains.
		double deposition = 0.0;
		int deposition_iterations = 12; // Gauss-Seidel passes at most (Yuan et al. report a few)
		double deposition_tolerance = 5.0e-2; // metres: stop once no cell moves more in a pass
		double deposition_relaxation = 0.5;
	};

	// Buffers: all nx * ny, row-major (z rows of x). The caller allocates them; outlet cells are fixed.
	struct Buffers {
		double *height = nullptr; // in/out, metres
		const double *uplift = nullptr; // m/yr
		const uint8_t *outlet = nullptr; // optional extra outlet mask (sea), 1 = outlet; may be null
		double *area = nullptr; // out, drainage area m^2
		double *sediment = nullptr; // in/out, accumulated basin fill, metres
		int32_t *receiver = nullptr;
		double *receiver_dist = nullptr;
		int32_t *stack = nullptr;
		int32_t *donor_count = nullptr;
		int32_t *donor_offset = nullptr; // nx * ny + 1
		int32_t *donors = nullptr;
		int32_t *work = nullptr; // DFS / heap scratch, nx * ny
		double *scratch = nullptr; // nx * ny
		double *temp = nullptr; // nx * ny
		int32_t *order = nullptr; // nx * ny, cells from highest to lowest
		uint64_t *sort_keys = nullptr; // 2 * nx * ny
		uint32_t *radix_count = nullptr; // 65536
		uint8_t *flags = nullptr; // nx * ny
		// Optional rock strength per cell (layered rock): K multiplier and tan(critical slope). Null: uniform.
		const double *erodibility_scale = nullptr;
		const double *talus_cell = nullptr;
		// Optional: how far below the surface the hard rock ends (0 where the surface is soft). A hard cell whose
		// receiver lies below that depth stands on soft rock exposed in the face under it: undercut, its block of hard
		// rock collapses and it erodes on as the soft rock does, so the face retreats as a cliff (the caprock
		// waterfall, Gilbert 1907; Haviv et al. 2010).
		const double *hard_depth = nullptr;
		// Optional diffusion factor per cell (0..1): bare hard rock does not creep, so its cliffs stay sharp.
		const double *diffusion_scale = nullptr;
	};

	Params p;
	Buffers b;
	int pits_filled = 0; // cells raised by the last fill
	int deposition_passes = 0; // Gauss-Seidel passes the last step took
	std::vector<double> dep_start; // heights after uplift, before erosion and deposition
	std::vector<double> dep_flux; // eroded volume from each cell and everything above it, m^3 per step
	std::vector<uint8_t> dep_undercut; // hard blocks broken off this step (decided on the first pass)
	std::vector<double> dep_area; // drainage area along the receiver tree the flux follows (not the MFD area)

	// Outlets cached per cell (the edge test and the mask were most of the profile); rebuilt when the grid or the
	// mask changes.
	std::vector<uint8_t> outlet_cache;
	int cache_nx = -1, cache_ny = -1, cache_edges = -1;
	const uint8_t *cache_mask = nullptr;

	void _update_outlets() {
		if (cache_nx == p.nx && cache_ny == p.ny && cache_edges == p.outlet_edges && cache_mask == b.outlet && int(outlet_cache.size()) == p.nx * p.ny) {
			return;
		}
		const int n = p.nx * p.ny;
		outlet_cache.assign(n, 0);
		for (int i = 0; i < n; i++) {
			outlet_cache[i] = _outlet_test(i) ? 1 : 0;
		}
		cache_nx = p.nx;
		cache_ny = p.ny;
		cache_edges = p.outlet_edges;
		cache_mask = b.outlet;
	}

	bool is_outlet(int i) const { return outlet_cache[i] != 0; }
	void invalidate_outlets() { cache_nx = -1; }

	bool _outlet_test(int i) const {
		const int x = i % p.nx;
		const int z = i / p.nx;
		if (b.outlet && b.outlet[i]) {
			return true;
		}
		return ((p.outlet_edges & 1) && x == 0) || ((p.outlet_edges & 2) && x == p.nx - 1) ||
				((p.outlet_edges & 4) && z == 0) || ((p.outlet_edges & 8) && z == p.ny - 1);
	}

	// One time step; returns the largest height change in metres.
	double step() {
		_update_outlets();
		const int n = p.nx * p.ny;
		double *h = b.height;
		memcpy(b.scratch, h, sizeof(double) * n);

		// Uplift.
		for (int i = 0; i < n; i++) {
			if (is_outlet(i)) {
				h[i] = p.base_level;
			} else {
				h[i] += p.dt * b.uplift[i];
			}
		}

		_compute_receivers();
		if (_count_pits() > 0) {
			_priority_flood_fill();
			_compute_receivers();
		} else {
			pits_filled = 0;
		}
		_build_stack();

		_accumulate_area();

		if (p.deposition > 0.0) {
			_solve_with_deposition();
		} else {
			_solve(nullptr);
		}

		if (p.diffusion > 0.0) {
			_diffuse();
		}

		double max_dh = 0.0;
		for (int i = 0; i < n; i++) {
			max_dh = std::fmax(max_dh, std::fabs(h[i] - b.scratch[i]));
		}
		return max_dh;
	}

	// Implicit stream power and critical slope, from the outlets up: every receiver is final before its donors.
	// p_up: with deposition, the sediment volume arriving from upstream of each cell (Yuan et al. 2019, eq. 13
	// solved for h_i with the cell's own share implicit), and b.temp the heights after uplift.
	void _solve(const double *p_up) {
		const int n = p.nx * p.ny;
		double *h = b.height;
		const bool half_m = p.area_exponent == 0.5;
		const double cell_area = p.dx * p.dx;
		for (int k = 0; k < n; k++) {
			const int i = b.stack[k];
			const int r = b.receiver[i];
			if (r == i) {
				continue;
			}
			const double d = b.receiver_dist[i];
			const double am = half_m ? std::sqrt(b.area[i]) : std::pow(b.area[i], p.area_exponent);
			double kk = b.erodibility_scale ? p.erodibility * b.erodibility_scale[i] : p.erodibility;
			double top = p_up ? dep_start[i] : h[i];
			// With deposition the undercut is decided once per step, on the first (incision-only) pass: the later
			// passes move the foot by small amounts, and a block that breaks off and back in turn never settles.
			bool undercut = b.hard_depth && b.hard_depth[i] > 0.0;
			if (undercut) {
				if (!p_up) {
					undercut = h[r] < top - b.hard_depth[i] - p.talus * d;
					if (p.deposition > 0.0) {
						dep_undercut[i] = undercut;
					}
				} else {
					undercut = dep_undercut[i];
				}
			}
			if (undercut) {
				// Undercut: the soft rock under the block has worn back past it (its foot lower than the band's base by
				// a soft slope at its critical gradient), so the block of hard rock breaks off whole and the soft rock
				// under it erodes on. The face retreats a cell per such undercutting, at the pace the foot incises.
				top = top - b.hard_depth[i];
				kk = p.erodibility;
			}
			const double f = kk * p.dt * am / d;
			double hi;
			if (p_up) {
				const double g = p.deposition / dep_area[i];
				const double ga = g * cell_area;
				hi = ((1.0 + ga) * top + g * std::fmax(p_up[i], 0.0) + f * h[r]) / (1.0 + f + ga);
			} else {
				hi = (top + f * h[r]) / (1.0 + f);
			}
			if (p.talus > 0.0) {
				hi = std::fmin(hi, h[r] + (b.talus_cell ? b.talus_cell[i] : p.talus) * d);
			}
			h[i] = hi;
		}
	}

	// Yuan et al. 2019: the flux into a cell depends on the new heights upstream, which the stack solves after it,
	// so the solve is repeated with the flux from the last pass (Gauss-Seidel) until the heights settle. Alluvium
	// (the surface raised above the uplifted ground, worn first when the river cuts again) goes to b.sediment.
	void _solve_with_deposition() {
		const int n = p.nx * p.ny;
		double *h = b.height;
		const double cell_area = p.dx * p.dx;
		dep_start.assign(h, h + n);
		dep_flux.resize(n);
		// The flux runs down the single receiver tree, so it is spread over that tree's area: dividing it by the
		// multiple-flow area, which can be far smaller on a cell the tree funnels into, would pile it up there.
		dep_area.assign(n, cell_area);
		dep_undercut.assign(n, 0);
		for (int k = n - 1; k >= 0; k--) {
			const int i = b.stack[k];
			const int r = b.receiver[i];
			if (r != i) {
				dep_area[r] += dep_area[i];
			}
		}
		std::vector<double> &up = dep_flux;
		std::vector<double> prev(n);
		_solve(nullptr); // first guess: no deposition
		deposition_passes = 0;
		for (int it = 0; it < p.deposition_iterations; it++) {
			for (int i = 0; i < n; i++) {
				up[i] = 0.0;
			}
			// Upstream volume: down the stack from the top, each cell passes on what came in plus what it lost.
			for (int k = n - 1; k >= 0; k--) {
				const int i = b.stack[k];
				const int r = b.receiver[i];
				if (r != i) {
					up[r] += up[i] + (dep_start[i] - h[i]) * cell_area;
				}
			}
			memcpy(prev.data(), h, sizeof(double) * n);
			for (int i = 0; i < n; i++) {
				if (b.receiver[i] != i) {
					h[i] = dep_start[i];
				}
			}
			_solve(up.data());
			deposition_passes++;
			// Under-relaxed: the full update swings between two states when a step moves much sediment (the
			// flux and the heights it raises chase each other); half steps settle, halving the change per pass.
			const double w = it == 0 ? 1.0 : p.deposition_relaxation;
			double moved = 0.0;
			for (int i = 0; i < n; i++) {
				h[i] = prev[i] + w * (h[i] - prev[i]);
				moved = std::fmax(moved, std::fabs(h[i] - prev[i]));
			}
			if (moved < p.deposition_tolerance) {
				break;
			}
		}
		if (b.sediment) {
			for (int i = 0; i < n; i++) {
				b.sediment[i] = std::fmax(b.sediment[i] + h[i] - dep_start[i], 0.0);
			}
		}
	}

	// Routing only (receivers, stack, area) on the current heights, after filling any basin the last step left (so
	// the result drains everywhere); no erosion. For the final heights and maps.
	void route() {
		_update_outlets();
		_compute_receivers();
		if (_count_pits() > 0) {
			_priority_flood_fill();
			_compute_receivers();
		}
		_build_stack();
		_accumulate_area();
	}

private:
	// D8 neighbour order, fixed so ties resolve the same way everywhere.
	static constexpr int DX[8] = { 1, -1, 0, 0, 1, -1, 1, -1 };
	static constexpr int DZ[8] = { 0, 0, 1, -1, 1, 1, -1, -1 };

	void _compute_receivers() {
		const int nx = p.nx;
		const int ny = p.ny;
		const double diag = p.dx * std::sqrt(2.0);
		const double *h = b.height;
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const int i = z * nx + x;
				b.receiver[i] = i;
				b.receiver_dist[i] = p.dx;
				if (is_outlet(i)) {
					continue;
				}
				double best = 0.0;
				for (int k = 0; k < 8; k++) {
					const int xx = x + DX[k];
					const int zz = z + DZ[k];
					if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
						continue;
					}
					const int j = zz * nx + xx;
					const double d = k < 4 ? p.dx : diag;
					const double s = (h[i] - h[j]) / d;
					if (s > best) {
						best = s;
						b.receiver[i] = j;
						b.receiver_dist[i] = d;
					}
				}
			}
		}
	}

	int _count_pits() const {
		const int n = p.nx * p.ny;
		int pits = 0;
		for (int i = 0; i < n; i++) {
			if (b.receiver[i] == i && !is_outlet(i)) {
				pits++;
			}
		}
		return pits;
	}

	// Binary min-heap on (height, index) in b.work, keyed by b.scratch2 = filled heights in b.height.
	struct Heap {
		int32_t *data;
		int size = 0;
		const double *key;
		bool less(int a, int c) const { return key[a] < key[c] || (key[a] == key[c] && a < c); }
		void push(int v) {
			int i = size++;
			data[i] = v;
			while (i > 0) {
				const int parent = (i - 1) >> 1;
				if (!less(data[i], data[parent])) {
					break;
				}
				const int t = data[i];
				data[i] = data[parent];
				data[parent] = t;
				i = parent;
			}
		}
		int pop() {
			const int top = data[0];
			data[0] = data[--size];
			int i = 0;
			for (;;) {
				const int l = 2 * i + 1;
				const int r = l + 1;
				int m = i;
				if (l < size && less(data[l], data[m])) {
					m = l;
				}
				if (r < size && less(data[r], data[m])) {
					m = r;
				}
				if (m == i) {
					break;
				}
				const int t = data[i];
				data[i] = data[m];
				data[m] = t;
				i = m;
			}
			return top;
		}
	};

	// Priority-Flood + epsilon (Barnes et al. 2014, algorithm 3): flood from the outlets in height order; a cell
	// reached from a higher one is raised to just above it, so every cell has a strictly downhill path out.
	void _priority_flood_fill() {
		const int nx = p.nx;
		const int ny = p.ny;
		const int n = nx * ny;
		double *h = b.height;
		uint8_t *closed = b.flags;
		memset(closed, 0, n);
		Heap heap{ b.work, 0, h };
		for (int i = 0; i < n; i++) {
			if (is_outlet(i)) {
				closed[i] = 1;
				heap.push(i);
			}
		}
		int raised = 0;
		while (heap.size > 0) {
			const int c = heap.pop();
			const int x = c % nx;
			const int z = c / nx;
			for (int k = 0; k < 8; k++) {
				const int xx = x + DX[k];
				const int zz = z + DZ[k];
				if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
					continue;
				}
				const int j = zz * nx + xx;
				if (closed[j]) {
					continue;
				}
				closed[j] = 1;
				const double floor_h = h[c] + p.fill_epsilon;
				if (h[j] < floor_h) {
					if (b.sediment) {
						b.sediment[j] += floor_h - h[j];
					}
					h[j] = floor_h;
					raised++;
				}
				heap.push(j);
			}
		}
		pits_filled = raised;
	}

	// The stack of Braun and Willett: each outlet, then its donors depth first.
	void _build_stack() {
		const int n = p.nx * p.ny;
		memset(b.donor_count, 0, sizeof(int32_t) * n);
		for (int i = 0; i < n; i++) {
			if (b.receiver[i] != i) {
				b.donor_count[b.receiver[i]]++;
			}
		}
		b.donor_offset[0] = 0;
		for (int i = 0; i < n; i++) {
			b.donor_offset[i + 1] = b.donor_offset[i] + b.donor_count[i];
		}
		memset(b.donor_count, 0, sizeof(int32_t) * n);
		for (int i = 0; i < n; i++) {
			const int r = b.receiver[i];
			if (r != i) {
				b.donors[b.donor_offset[r] + b.donor_count[r]++] = i;
			}
		}
		int top = 0;
		int count = 0;
		for (int i = 0; i < n; i++) {
			if (b.receiver[i] != i) {
				continue;
			}
			b.work[top++] = i;
			while (top > 0) {
				const int c = b.work[--top];
				b.stack[count++] = c;
				for (int d = b.donor_offset[c + 1] - 1; d >= b.donor_offset[c]; d--) {
					b.work[top++] = b.donors[d];
				}
			}
		}
	}

	// LSD radix sort of the cells by height, highest first, four 16-bit passes over an order-preserving key;
	// stable, so equal heights stay in index order. O(n), the same on every machine.
	void _sort_by_height() {
		const int n = p.nx * p.ny;
		uint64_t *ka = b.sort_keys;
		uint64_t *kb = b.sort_keys + n;
		int32_t *ia = b.order;
		int32_t *ib = b.work;
		for (int i = 0; i < n; i++) {
			uint64_t u;
			memcpy(&u, &b.height[i], sizeof(u));
			// Doubles to unsigned order (flip all bits of negatives, the sign bit of positives), then invert
			// for descending.
			u = (u & 0x8000000000000000ull) ? ~u : (u | 0x8000000000000000ull);
			ka[i] = ~u;
			ia[i] = i;
		}
		uint32_t *count = b.radix_count;
		for (int pass = 0; pass < 4; pass++) {
			const int shift = pass * 16;
			memset(count, 0, sizeof(uint32_t) * 65536);
			for (int i = 0; i < n; i++) {
				count[(ka[i] >> shift) & 0xffff]++;
			}
			uint32_t sum = 0;
			for (int d = 0; d < 65536; d++) {
				const uint32_t c = count[d];
				count[d] = sum;
				sum += c;
			}
			for (int i = 0; i < n; i++) {
				const uint32_t dst = count[(ka[i] >> shift) & 0xffff]++;
				kb[dst] = ka[i];
				ib[dst] = ia[i];
			}
			uint64_t *tk = ka;
			ka = kb;
			kb = tk;
			int32_t *ti = ia;
			ia = ib;
			ib = ti;
		}
		// Four passes: the result is back in b.order.
	}

	void _accumulate_area() {
		const int n = p.nx * p.ny;
		const double cell_area = p.dx * p.dx;
		for (int i = 0; i < n; i++) {
			b.area[i] = cell_area;
		}
		if (p.flow_exponent <= 0.0) {
			// D8: down the receiver tree, from the top of the stack.
			for (int k = n - 1; k >= 0; k--) {
				const int i = b.stack[k];
				const int r = b.receiver[i];
				if (r != i) {
					b.area[r] += b.area[i];
				}
			}
			return;
		}
		// MFD: visit cells from the highest down (ties by index), passing each cell's area to
		// every lower neighbour in proportion to slope^p. After the fill every non-outlet cell has one.
		const int nx = p.nx;
		const int ny = p.ny;
		const double diag = p.dx * std::sqrt(2.0);
		const double *h = b.height;
		_sort_by_height();
		const bool unit_flow = p.flow_exponent == 1.0;
		double w[8];
		for (int o = 0; o < n; o++) {
			const int i = b.order[o];
			if (is_outlet(i)) {
				continue;
			}
			const int x = i % nx;
			const int z = i / nx;
			double sum = 0.0;
			for (int k = 0; k < 8; k++) {
				w[k] = 0.0;
				const int xx = x + DX[k];
				const int zz = z + DZ[k];
				if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
					continue;
				}
				const int j = zz * nx + xx;
				const double s = (h[i] - h[j]) / (k < 4 ? p.dx : diag);
				if (s > 0.0) {
					// Neighbour weight also scales with the contour length it faces (Freeman 1991: 1 vs 1/sqrt 2).
					w[k] = (unit_flow ? s : std::pow(s, p.flow_exponent)) * (k < 4 ? 1.0 : 0.70710678118654752);
					sum += w[k];
				}
			}
			if (sum <= 0.0) {
				continue;
			}
			for (int k = 0; k < 8; k++) {
				if (w[k] > 0.0) {
					b.area[(z + DZ[k]) * nx + x + DX[k]] += b.area[i] * w[k] / sum;
				}
			}
		}
	}

	void _diffuse() {
		const int nx = p.nx;
		const int ny = p.ny;
		const int n = nx * ny;
		double *h = b.height;
		// Explicit FTCS is stable for k_d dt / dx^2 <= 1/4; split the step to stay under 0.2.
		const double a_total = p.diffusion * p.dt / (p.dx * p.dx);
		const int sub = (int)std::fmin(64.0, std::ceil(a_total / 0.2));
		const double a = a_total / sub;
		for (int s = 0; s < sub; s++) {
			memcpy(b.temp, h, sizeof(double) * n);
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					if (is_outlet(i)) {
						continue;
					}
					const double c = b.temp[i];
					const double l = x > 0 ? b.temp[i - 1] : c;
					const double r = x < nx - 1 ? b.temp[i + 1] : c;
					const double u = z > 0 ? b.temp[i - nx] : c;
					const double d = z < ny - 1 ? b.temp[i + nx] : c;
					if (b.diffusion_scale) {
						// Conservative fluxes, each across a face at the smaller factor of its two cells.
						const double *k = b.diffusion_scale;
						const double ki = k[i];
						const double kl = x > 0 ? std::fmin(ki, k[i - 1]) : 0.0;
						const double kr = x < nx - 1 ? std::fmin(ki, k[i + 1]) : 0.0;
						const double ku = z > 0 ? std::fmin(ki, k[i - nx]) : 0.0;
						const double kd = z < ny - 1 ? std::fmin(ki, k[i + nx]) : 0.0;
						h[i] = c + a * (kl * (l - c) + kr * (r - c) + ku * (u - c) + kd * (d - c));
						continue;
					}
					h[i] = c + a * (l + r + u + d - 4.0 * c);
				}
			}
		}
	}
};
