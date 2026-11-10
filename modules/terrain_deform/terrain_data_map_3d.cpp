/**************************************************************************/
/*  terrain_data_map_3d.cpp                                               */
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

#include "terrain_data_map_3d.h"

#include "terrain_deformer_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/templates/sort_array.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/rendering/rendering_server.h"

namespace {

// 8 neighbours: dx, dz, distance in cells, contour length in cells (Quinn et al. 1991).
const int NX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
const int NZ[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const float ND[8] = { 1.0f, Math::SQRT2, 1.0f, Math::SQRT2, 1.0f, Math::SQRT2, 1.0f, Math::SQRT2 };
const float NL[8] = { 0.5f, 0.354f, 0.5f, 0.354f, 0.5f, 0.354f, 0.5f, 0.354f };

const float FAR_DISTANCE = 1.0e6f;

// 1D squared Euclidean distance transform of a sampled function (Felzenszwalb and Huttenlocher 2012).
void edt_1d(const float *f, int n, float *d, int *v, float *z) {
	int k = 0;
	v[0] = 0;
	z[0] = -Math::INF;
	z[1] = Math::INF;
	for (int q = 1; q < n; q++) {
		float s;
		while (true) {
			const int p = v[k];
			s = ((f[q] + float(q) * q) - (f[p] + float(p) * p)) / (2.0f * (q - p));
			if (s <= z[k] && k > 0) {
				k--;
				continue;
			}
			break;
		}
		if (s <= z[k]) {
			// k == 0 and the new parabola is lower everywhere.
			v[0] = q;
			z[0] = -Math::INF;
			z[1] = Math::INF;
			continue;
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = Math::INF;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q) {
			k++;
		}
		const float dq = float(q - v[k]);
		d[q] = dq * dq + f[v[k]];
	}
}

// Squared distance (in cells) from every cell to the nearest cell where p_seed is set.
void edt_2d(const LocalVector<uint8_t> &p_seed, int n, LocalVector<float> &r_d2) {
	const float big = 1.0e12f;
	r_d2.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		r_d2[i] = p_seed[i] ? 0.0f : big;
	}
	LocalVector<float> f;
	LocalVector<float> d;
	LocalVector<int> v;
	LocalVector<float> z;
	f.resize(n);
	d.resize(n);
	v.resize(n);
	z.resize(n + 1);
	for (int x = 0; x < n; x++) {
		for (int y = 0; y < n; y++) {
			f[y] = r_d2[y * n + x];
		}
		edt_1d(f.ptr(), n, d.ptr(), v.ptr(), z.ptr());
		for (int y = 0; y < n; y++) {
			r_d2[y * n + x] = d[y];
		}
	}
	for (int y = 0; y < n; y++) {
		edt_1d(r_d2.ptr() + y * n, n, d.ptr(), v.ptr(), z.ptr());
		memcpy(r_d2.ptr() + y * n, d.ptr(), sizeof(float) * n);
	}
}

struct FloodCell {
	float h;
	int idx;
};

// Binary min-heap on height for Priority-Flood.
struct FloodHeap {
	LocalVector<FloodCell> a;
	void push(float p_h, int p_i) {
		a.push_back({ p_h, p_i });
		uint32_t c = a.size() - 1;
		while (c > 0) {
			const uint32_t p = (c - 1) / 2;
			if (a[p].h <= a[c].h) {
				break;
			}
			SWAP(a[p], a[c]);
			c = p;
		}
	}
	FloodCell pop() {
		const FloodCell top = a[0];
		a[0] = a[a.size() - 1];
		a.resize(a.size() - 1);
		uint32_t c = 0;
		const uint32_t n = a.size();
		while (true) {
			const uint32_t l = 2 * c + 1;
			const uint32_t r = l + 1;
			uint32_t m = c;
			if (l < n && a[l].h < a[m].h) {
				m = l;
			}
			if (r < n && a[r].h < a[m].h) {
				m = r;
			}
			if (m == c) {
				break;
			}
			SWAP(a[m], a[c]);
			c = m;
		}
		return top;
	}
};

} // namespace

/* Baking */

Rect2 TerrainDataMap3D::get_map_rect() const {
	if (baked_resolution > 0) {
		return Rect2(baked_origin, Vector2(baked_cell, baked_cell) * baked_resolution);
	}
	const Vector3 c = is_inside_tree() ? get_global_position() : get_position();
	return Rect2(Vector2(c.x, c.z) - Vector2(size, size) * 0.5f, Vector2(size, size));
}

bool TerrainDataMap3D::_sample_heights(LocalVector<float> &r_h, LocalVector<uint8_t> &r_valid) {
	const int n = resolution;
	const Rect2 rect = get_map_rect();
	const float cell = size / n;
	r_h.resize(n * n);
	r_valid.resize(n * n);

	Node *src = height_source.is_empty() ? nullptr : get_node_or_null(height_source);
	TerrainDeformer3D *deformer = Object::cast_to<TerrainDeformer3D>(src);
	Object *getter = nullptr;
	if (src && !deformer) {
		if (src->has_method("get_height")) {
			getter = src;
		} else if (src->has_method("get_data")) {
			// Terrain3D, by name: Terrain3D.get_data().get_height(position).
			Object *data = src->call("get_data");
			if (data && data->has_method("get_height")) {
				getter = data;
			}
		}
	}
	PhysicsDirectSpaceState3D *space = nullptr;
	if (is_inside_tree() && get_world_3d().is_valid()) {
		space = get_world_3d()->get_direct_space_state();
	}
	const float y0 = is_inside_tree() ? get_global_position().y : 0.0f;

	float lowest = Math::INF;
	bool any = false;
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const Vector3 p(rect.position.x + (x + 0.5f) * cell, y0, rect.position.y + (z + 0.5f) * cell);
			float h = Math::NaN;
			if (deformer) {
				h = deformer->get_height(p);
			} else if (getter) {
				h = getter->call("get_height", p);
			}
			if (!Math::is_finite(h) && space) {
				PhysicsDirectSpaceState3D::RayParameters rp;
				rp.from = Vector3(p.x, y0 + 10000.0f, p.z);
				rp.to = Vector3(p.x, y0 - 10000.0f, p.z);
				rp.collision_mask = ground_collision_mask;
				rp.hit_back_faces = false;
				PhysicsDirectSpaceState3D::RayResult rr;
				if (space->intersect_ray(rp, rr)) {
					h = rr.position.y;
				}
			}
			const int i = z * n + x;
			r_valid[i] = Math::is_finite(h) ? 1 : 0;
			r_h[i] = h;
			if (r_valid[i]) {
				any = true;
				lowest = MIN(lowest, h);
			}
		}
	}
	if (!any) {
		return false;
	}
	for (int i = 0; i < n * n; i++) {
		if (!r_valid[i]) {
			r_h[i] = lowest;
		}
	}
	return true;
}

void TerrainDataMap3D::_sample_water(const LocalVector<float> &p_h, LocalVector<float> &r_surface) {
	const int n = resolution;
	const Rect2 rect = get_map_rect();
	const float cell = size / n;
	r_surface.resize(n * n);
	for (int i = 0; i < n * n; i++) {
		r_surface[i] = -Math::INF;
	}
	for (int s = 0; s < water_sources.size(); s++) {
		Node *w = get_node_or_null(water_sources[s]);
		if (!w) {
			continue;
		}
		if (w->has_method("get_water_level")) {
			// Water surfaces (sea, lakes): a level plane at the node height plus water_level. Their
			// waves (sample_height) are left out: a bake should not depend on the wave phase. A
			// surface with an outline (PhysXWaterSurface3D.surface_mesh, a lake) is water only
			// inside it: the cells in the outline's bounds ask is_wet (or sample_height); without
			// one (the open sea) the plane covers the whole map.
			float level = w->call("get_water_level");
			Node3D *w3 = Object::cast_to<Node3D>(w);
			const Transform3D xf = (w3 && w3->is_inside_tree()) ? w3->get_global_transform() : Transform3D();
			level += xf.origin.y;
			bool has_outline_property = false;
			const Variant om = w->get("surface_mesh", &has_outline_property);
			const Ref<Mesh> outline = has_outline_property ? Ref<Mesh>(om) : Ref<Mesh>();
			if (outline.is_null()) {
				for (int i = 0; i < n * n; i++) {
					r_surface[i] = MAX(r_surface[i], level);
				}
				continue;
			}
			const AABB box = xf.xform(outline->get_aabb());
			const int x0 = MAX(int(Math::floor((box.position.x - rect.position.x) / cell)) - 1, 0);
			const int x1 = MIN(int(Math::floor((box.position.x + box.size.x - rect.position.x) / cell)) + 1, n - 1);
			const int z0 = MAX(int(Math::floor((box.position.z - rect.position.y) / cell)) - 1, 0);
			const int z1 = MIN(int(Math::floor((box.position.z + box.size.z - rect.position.y) / cell)) + 1, n - 1);
			const bool wet_query = w->has_method("is_wet");
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const int i = z * n + x;
					const Vector3 p(rect.position.x + (x + 0.5f) * cell, p_h[i], rect.position.y + (z + 0.5f) * cell);
					const bool wet = wet_query ? bool(w->call("is_wet", p)) : Math::is_finite(float(w->call("sample_height", p)));
					if (wet) {
						r_surface[i] = MAX(r_surface[i], level);
					}
				}
			}
		} else if (w->has_method("sample_height")) {
			// Rivers: surface height per point, -inf outside the channel. A river that lists its
			// cross-sections (PhysXRiver3D) is asked only about the cells near its channel: each
			// pair of sections marks the cells in its box, grown by the half width, bank and one
			// section spacing (sample_height finds the nearest point on the curve, so a call per
			// cell of a world-size map took minutes).
			LocalVector<uint8_t> near;
			bool all = true;
			if (w->has_method("get_section_count") && w->has_method("get_section")) {
				const int count = w->call("get_section_count");
				if (count >= 2) {
					all = false;
					near.resize(n * n);
					memset(near.ptr(), 0, n * n);
					const float margin = w->get("bank_margin");
					Dictionary prev = w->call("get_section", 0);
					for (int k = 1; k < count; k++) {
						const Dictionary cur = w->call("get_section", k);
						const Vector3 a = prev["centre"];
						const Vector3 b = cur["centre"];
						const float grow = 0.5f * MAX(float(prev["width"]), float(cur["width"])) + margin + a.distance_to(b) + cell;
						const int x0 = MAX(int(Math::floor((MIN(a.x, b.x) - grow - rect.position.x) / cell)), 0);
						const int x1 = MIN(int(Math::floor((MAX(a.x, b.x) + grow - rect.position.x) / cell)), n - 1);
						const int z0 = MAX(int(Math::floor((MIN(a.z, b.z) - grow - rect.position.y) / cell)), 0);
						const int z1 = MIN(int(Math::floor((MAX(a.z, b.z) + grow - rect.position.y) / cell)), n - 1);
						for (int z = z0; z <= z1; z++) {
							memset(near.ptr() + z * n + x0, 1, MAX(x1 - x0 + 1, 0));
						}
						prev = cur;
					}
				}
			}
			for (int z = 0; z < n; z++) {
				for (int x = 0; x < n; x++) {
					const int i = z * n + x;
					if (!all && !near[i]) {
						continue;
					}
					const Vector3 p(rect.position.x + (x + 0.5f) * cell, p_h[i], rect.position.y + (z + 0.5f) * cell);
					const float y = w->call("sample_height", p);
					if (Math::is_finite(y)) {
						r_surface[i] = MAX(r_surface[i], y);
					}
				}
			}
		}
	}
}

Error TerrainDataMap3D::bake() {
	ERR_FAIL_COND_V(resolution < 8, ERR_INVALID_PARAMETER);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const int n = resolution;
	const int nn = n * n;
	const float cell = size / n;

	// Bake against the current rect, not the previous bake's.
	baked_resolution = 0;
	const Rect2 rect = get_map_rect();

	LocalVector<float> h;
	LocalVector<uint8_t> valid;
	if (!_sample_heights(h, valid)) {
		ERR_FAIL_V_MSG(ERR_UNCONFIGURED, "TerrainDataMap3D: no heights. Set height_source or put colliders on ground_collision_mask under the map.");
	}
	LocalVector<float> surface;
	_sample_water(h, surface);

	for (int c = 0; c < CHANNEL_MAX; c++) {
		channels[c].resize(nn);
	}
	float *ch_h = channels[CHANNEL_HEIGHT].ptr();
	float *ch_slope = channels[CHANNEL_SLOPE].ptr();
	float *ch_aspect = channels[CHANNEL_ASPECT].ptr();
	float *ch_curv = channels[CHANNEL_CURVATURE].ptr();
	float *ch_flow = channels[CHANNEL_FLOW].ptr();
	float *ch_wet = channels[CHANNEL_WETNESS].ptr();
	float *ch_depth = channels[CHANNEL_WATER_DEPTH].ptr();
	float *ch_dist = channels[CHANNEL_WATER_DISTANCE].ptr();

	LocalVector<uint8_t> water;
	water.resize(nn);
	bool any_water = false;
	for (int i = 0; i < nn; i++) {
		ch_h[i] = h[i];
		const float d = surface[i] - h[i];
		ch_depth[i] = d > 0.0f ? d : 0.0f;
		water[i] = d > 0.0f ? 1 : 0;
		any_water = any_water || water[i];
	}

	auto H = [&](int x, int z) -> float {
		return h[CLAMP(z, 0, n - 1) * n + CLAMP(x, 0, n - 1)];
	};

	// Slope and aspect (Horn 1981) and curvature (Zevenbergen and Thorne 1987).
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const float a = H(x - 1, z - 1), b = H(x, z - 1), c = H(x + 1, z - 1);
			const float d = H(x - 1, z), e = H(x, z), f = H(x + 1, z);
			const float g = H(x - 1, z + 1), hh = H(x, z + 1), k = H(x + 1, z + 1);
			const float dzdx = ((c + 2.0f * f + k) - (a + 2.0f * d + g)) / (8.0f * cell);
			const float dzdz = ((g + 2.0f * hh + k) - (a + 2.0f * b + c)) / (8.0f * cell);
			const int i = z * n + x;
			ch_slope[i] = Math::atan(Math::sqrt(dzdx * dzdx + dzdz * dzdz));
			// Direction the ground faces (downhill), radians from +Z towards +X.
			float asp = Math::atan2(-dzdx, -dzdz);
			if (asp < 0.0f) {
				asp += Math::TAU;
			}
			ch_aspect[i] = (dzdx == 0.0f && dzdz == 0.0f) ? 0.0f : asp;
			// Zevenbergen-Thorne general curvature 2(D + E), positive in hollows.
			const float D = ((d + f) * 0.5f - e) / (cell * cell);
			const float E = ((b + hh) * 0.5f - e) / (cell * cell);
			ch_curv[i] = 2.0f * (D + E);
		}
	}

	// Priority-Flood with epsilon (Barnes, Lehman and Mulla 2014): fill pits so every land cell
	// drains to the map edge or to water. Water cells are outlets.
	LocalVector<float> filled;
	filled.resize(nn);
	LocalVector<uint8_t> closed;
	closed.resize(nn);
	memset(closed.ptr(), 0, nn);
	FloodHeap heap;
	heap.a.reserve(nn);
	auto push = [&](float p_h, int p_i) {
		heap.push(p_h, p_i);
	};
	for (int i = 0; i < nn; i++) {
		const int x = i % n;
		const int z = i / n;
		if (water[i] || x == 0 || z == 0 || x == n - 1 || z == n - 1) {
			filled[i] = h[i];
			closed[i] = 1;
			push(h[i], i);
		}
	}
	const float eps = 1.0e-4f * cell;
	while (!heap.a.is_empty()) {
		const FloodCell c = heap.pop();
		const int cx = c.idx % n;
		const int cz = c.idx / n;
		for (int k = 0; k < 8; k++) {
			const int x = cx + NX[k];
			const int z = cz + NZ[k];
			if (x < 0 || z < 0 || x >= n || z >= n) {
				continue;
			}
			const int j = z * n + x;
			if (closed[j]) {
				continue;
			}
			closed[j] = 1;
			filled[j] = MAX(h[j], filled[c.idx] + eps);
			push(filled[j], j);
		}
	}

	// Multiple flow direction accumulation, highest cell first. Each cell passes its upslope
	// area to every lower neighbour in proportion to (tan b)^p times contour length.
	LocalVector<int> order;
	order.resize(nn);
	for (int i = 0; i < nn; i++) {
		order[i] = i;
	}
	struct ByHeight {
		const float *f = nullptr;
		bool operator()(int a, int b) const { return f[a] > f[b]; }
	};
	SortArray<int, ByHeight> sorter;
	sorter.compare.f = filled.ptr();
	sorter.sort(order.ptr(), nn);
	const float cell_area = cell * cell;
	for (int i = 0; i < nn; i++) {
		ch_flow[i] = cell_area;
	}
	for (int o = 0; o < nn; o++) {
		const int i = order[o];
		if (water[i]) {
			continue; // Flow ends in water.
		}
		const int cx = i % n;
		const int cz = i / n;
		float w[8];
		float total = 0.0f;
		for (int k = 0; k < 8; k++) {
			w[k] = 0.0f;
			const int x = cx + NX[k];
			const int z = cz + NZ[k];
			if (x < 0 || z < 0 || x >= n || z >= n) {
				continue;
			}
			const float drop = filled[i] - filled[z * n + x];
			if (drop <= 0.0f) {
				continue;
			}
			w[k] = Math::pow(drop / (ND[k] * cell), flow_exponent) * NL[k];
			total += w[k];
		}
		if (total <= 0.0f) {
			continue; // Map edge or flat outlet.
		}
		const float a = ch_flow[i] / total;
		for (int k = 0; k < 8; k++) {
			if (w[k] > 0.0f) {
				ch_flow[(cz + NZ[k]) * n + cx + NX[k]] += a * w[k];
			}
		}
	}

	// Wetness (TWI = ln(a / tan b), a = upslope area per unit contour width) and log flow.
	for (int i = 0; i < nn; i++) {
		const float a = ch_flow[i] / cell;
		ch_wet[i] = Math::log(a / MAX(Math::tan(ch_slope[i]), 0.001f));
		ch_flow[i] = Math::log(ch_flow[i]) * 0.43429448f;
	}

	// Signed distance to the shoreline in metres, negative under water.
	if (any_water) {
		LocalVector<float> d_out;
		LocalVector<float> d_in;
		edt_2d(water, n, d_out);
		LocalVector<uint8_t> land;
		land.resize(nn);
		for (int i = 0; i < nn; i++) {
			land[i] = water[i] ? 0 : 1;
		}
		edt_2d(land, n, d_in);
		for (int i = 0; i < nn; i++) {
			ch_dist[i] = water[i] ? -(Math::sqrt(d_in[i]) - 0.5f) * cell : (Math::sqrt(d_out[i]) - 0.5f) * cell;
		}
	} else {
		for (int i = 0; i < nn; i++) {
			ch_dist[i] = FAR_DISTANCE;
		}
	}

	baked_resolution = n;
	baked_origin = rect.position;
	baked_cell = cell;

	// Two RGBA float images: height, slope, aspect, curvature / flow, wetness, depth, distance.
	Vector<uint8_t> d0;
	Vector<uint8_t> d1;
	d0.resize(nn * 16);
	d1.resize(nn * 16);
	float *p0 = reinterpret_cast<float *>(d0.ptrw());
	float *p1 = reinterpret_cast<float *>(d1.ptrw());
	for (int i = 0; i < nn; i++) {
		p0[i * 4 + 0] = ch_h[i];
		p0[i * 4 + 1] = ch_slope[i];
		p0[i * 4 + 2] = ch_aspect[i];
		p0[i * 4 + 3] = ch_curv[i];
		p1[i * 4 + 0] = ch_flow[i];
		p1[i * 4 + 1] = ch_wet[i];
		p1[i * 4 + 2] = ch_depth[i];
		p1[i * 4 + 3] = ch_dist[i];
	}
	data_image = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, d0);
	water_image = Image::create_from_data(n, n, false, Image::FORMAT_RGBAF, d1);
	if (data_texture.is_valid() && data_texture->get_width() == n) {
		data_texture->update(data_image);
		water_texture->update(water_image);
	} else {
		data_texture = ImageTexture::create_from_image(data_image);
		water_texture = ImageTexture::create_from_image(water_image);
	}
	bake_msec = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	_publish_globals();
	emit_signal(SNAME("baked"));
	return OK;
}

void TerrainDataMap3D::queue_bake() {
	if (bake_queued) {
		return;
	}
	bake_queued = true;
	callable_mp(this, &TerrainDataMap3D::_queued_bake).call_deferred();
}

void TerrainDataMap3D::_queued_bake() {
	bake_queued = false;
	if (is_inside_tree()) {
		bake();
	}
}

void TerrainDataMap3D::_publish_globals() {
	if (!publish_shader_globals || data_texture.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	static HashSet<StringName> added;
	auto set_global = [&](const StringName &p_name, const Variant &p_value, RSE::GlobalShaderParameterType p_type) {
		if (!added.has(p_name) && !ProjectSettings::get_singleton()->has_setting("shader_globals/" + String(p_name))) {
			added.insert(p_name);
			rs->global_shader_parameter_add(p_name, p_type, p_value);
			return;
		}
		rs->global_shader_parameter_set(p_name, p_value);
	};
	const Rect2 r = get_map_rect();
	set_global("terrain_data_map", data_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	set_global("terrain_water_map", water_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	set_global("terrain_data_rect", Vector4(r.position.x, r.position.y, r.size.x, r.size.y), RSE::GLOBAL_VAR_TYPE_VEC4);
}

/* Queries */

float TerrainDataMap3D::_sample(Channel p_channel, const Vector3 &p_global_position) const {
	ERR_FAIL_COND_V_MSG(baked_resolution == 0, Math::NaN, "TerrainDataMap3D: not baked yet.");
	const int n = baked_resolution;
	const float gx = (p_global_position.x - baked_origin.x) / baked_cell - 0.5f;
	const float gz = (p_global_position.z - baked_origin.y) / baked_cell - 0.5f;
	const int x0 = CLAMP(int(Math::floor(gx)), 0, n - 1);
	const int z0 = CLAMP(int(Math::floor(gz)), 0, n - 1);
	const int x1 = MIN(x0 + 1, n - 1);
	const int z1 = MIN(z0 + 1, n - 1);
	const float fx = CLAMP(gx - x0, 0.0f, 1.0f);
	const float fz = CLAMP(gz - z0, 0.0f, 1.0f);
	const float *c = channels[p_channel].ptr();
	if (p_channel == CHANNEL_ASPECT) {
		// Nearest cell: an angle does not interpolate across 0 / 2 pi.
		return c[(fz < 0.5f ? z0 : z1) * n + (fx < 0.5f ? x0 : x1)];
	}
	const float a = Math::lerp(c[z0 * n + x0], c[z0 * n + x1], fx);
	const float b = Math::lerp(c[z1 * n + x0], c[z1 * n + x1], fx);
	return Math::lerp(a, b, fz);
}

float TerrainDataMap3D::get_height_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_HEIGHT, p_pos);
}
float TerrainDataMap3D::get_slope_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_SLOPE, p_pos);
}
float TerrainDataMap3D::get_aspect_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_ASPECT, p_pos);
}
float TerrainDataMap3D::get_curvature_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_CURVATURE, p_pos);
}
float TerrainDataMap3D::get_flow_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_FLOW, p_pos);
}
float TerrainDataMap3D::get_wetness_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_WETNESS, p_pos);
}
float TerrainDataMap3D::get_water_depth_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_WATER_DEPTH, p_pos);
}
float TerrainDataMap3D::get_water_distance_at(const Vector3 &p_pos) const {
	return _sample(CHANNEL_WATER_DISTANCE, p_pos);
}

Ref<Image> TerrainDataMap3D::get_channel_image(Channel p_channel) const {
	ERR_FAIL_INDEX_V(p_channel, CHANNEL_MAX, Ref<Image>());
	ERR_FAIL_COND_V(baked_resolution == 0, Ref<Image>());
	const int nn = baked_resolution * baked_resolution;
	Vector<uint8_t> d;
	d.resize(nn * 4);
	memcpy(d.ptrw(), channels[p_channel].ptr(), nn * 4);
	return Image::create_from_data(baked_resolution, baked_resolution, false, Image::FORMAT_RF, d);
}

String TerrainDataMap3D::get_shader_code() {
	String c;
	c += "// World data from TerrainDataMap3D (TerrainDataMap3D.get_shader_code()).\n";
	c += "// terrain_data(p) = (height, slope rad, aspect rad, curvature 1/m)\n";
	c += "// terrain_water(p) = (log10 upslope area m2, wetness index, water depth m, signed distance to water m)\n";
	c += "global uniform sampler2D terrain_data_map;\n";
	c += "global uniform sampler2D terrain_water_map;\n";
	c += "global uniform vec4 terrain_data_rect;\n\n";
	c += "vec2 terrain_data_uv(vec3 world_pos) {\n";
	c += "	// Clamped to the outer texel centres, so the edges do not wrap around.\n";
	c += "	vec2 half_texel = 0.5 / vec2(textureSize(terrain_data_map, 0));\n";
	c += "	return clamp((world_pos.xz - terrain_data_rect.xy) / terrain_data_rect.zw, half_texel, 1.0 - half_texel);\n";
	c += "}\n\n";
	c += "vec4 terrain_data(vec3 world_pos) {\n";
	c += "	return textureLod(terrain_data_map, terrain_data_uv(world_pos), 0.0);\n";
	c += "}\n\n";
	c += "vec4 terrain_water(vec3 world_pos) {\n";
	c += "	return textureLod(terrain_water_map, terrain_data_uv(world_pos), 0.0);\n";
	c += "}\n";
	return c;
}

/* Properties */

void TerrainDataMap3D::set_size(float p_size) {
	size = MAX(p_size, 1.0f);
}

void TerrainDataMap3D::set_resolution(int p_resolution) {
	resolution = CLAMP(p_resolution, 8, 4096);
}

void TerrainDataMap3D::set_height_source(const NodePath &p_path) {
	height_source = p_path;
}

void TerrainDataMap3D::set_ground_collision_mask(uint32_t p_mask) {
	ground_collision_mask = p_mask;
}

void TerrainDataMap3D::set_water_sources(const TypedArray<NodePath> &p_sources) {
	water_sources = p_sources;
}

void TerrainDataMap3D::set_flow_exponent(float p_exponent) {
	flow_exponent = CLAMP(p_exponent, 0.5f, 10.0f);
}

void TerrainDataMap3D::set_bake_on_ready(bool p_enable) {
	bake_on_ready = p_enable;
}

void TerrainDataMap3D::set_publish_shader_globals(bool p_enable) {
	publish_shader_globals = p_enable;
	_publish_globals();
}

void TerrainDataMap3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			if (bake_on_ready && !Engine::get_singleton()->is_editor_hint()) {
				// Deferred, so terrain deformers and colliders are in place first.
				queue_bake();
			}
		} break;
	}
}

void TerrainDataMap3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &TerrainDataMap3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &TerrainDataMap3D::get_size);
	ClassDB::bind_method(D_METHOD("set_resolution", "resolution"), &TerrainDataMap3D::set_resolution);
	ClassDB::bind_method(D_METHOD("get_resolution"), &TerrainDataMap3D::get_resolution);
	ClassDB::bind_method(D_METHOD("set_height_source", "path"), &TerrainDataMap3D::set_height_source);
	ClassDB::bind_method(D_METHOD("get_height_source"), &TerrainDataMap3D::get_height_source);
	ClassDB::bind_method(D_METHOD("set_ground_collision_mask", "mask"), &TerrainDataMap3D::set_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("get_ground_collision_mask"), &TerrainDataMap3D::get_ground_collision_mask);
	ClassDB::bind_method(D_METHOD("set_water_sources", "sources"), &TerrainDataMap3D::set_water_sources);
	ClassDB::bind_method(D_METHOD("get_water_sources"), &TerrainDataMap3D::get_water_sources);
	ClassDB::bind_method(D_METHOD("set_flow_exponent", "exponent"), &TerrainDataMap3D::set_flow_exponent);
	ClassDB::bind_method(D_METHOD("get_flow_exponent"), &TerrainDataMap3D::get_flow_exponent);
	ClassDB::bind_method(D_METHOD("set_bake_on_ready", "enable"), &TerrainDataMap3D::set_bake_on_ready);
	ClassDB::bind_method(D_METHOD("is_bake_on_ready"), &TerrainDataMap3D::is_bake_on_ready);
	ClassDB::bind_method(D_METHOD("set_publish_shader_globals", "enable"), &TerrainDataMap3D::set_publish_shader_globals);
	ClassDB::bind_method(D_METHOD("is_publishing_shader_globals"), &TerrainDataMap3D::is_publishing_shader_globals);

	ClassDB::bind_method(D_METHOD("bake"), &TerrainDataMap3D::bake);
	ClassDB::bind_method(D_METHOD("queue_bake"), &TerrainDataMap3D::queue_bake);
	ClassDB::bind_method(D_METHOD("is_baked"), &TerrainDataMap3D::is_baked);
	ClassDB::bind_method(D_METHOD("get_bake_time_msec"), &TerrainDataMap3D::get_bake_time_msec);
	ClassDB::bind_method(D_METHOD("get_map_rect"), &TerrainDataMap3D::get_map_rect);
	ClassDB::bind_method(D_METHOD("get_height_at", "global_position"), &TerrainDataMap3D::get_height_at);
	ClassDB::bind_method(D_METHOD("get_slope_at", "global_position"), &TerrainDataMap3D::get_slope_at);
	ClassDB::bind_method(D_METHOD("get_aspect_at", "global_position"), &TerrainDataMap3D::get_aspect_at);
	ClassDB::bind_method(D_METHOD("get_curvature_at", "global_position"), &TerrainDataMap3D::get_curvature_at);
	ClassDB::bind_method(D_METHOD("get_flow_at", "global_position"), &TerrainDataMap3D::get_flow_at);
	ClassDB::bind_method(D_METHOD("get_wetness_at", "global_position"), &TerrainDataMap3D::get_wetness_at);
	ClassDB::bind_method(D_METHOD("get_water_depth_at", "global_position"), &TerrainDataMap3D::get_water_depth_at);
	ClassDB::bind_method(D_METHOD("get_water_distance_at", "global_position"), &TerrainDataMap3D::get_water_distance_at);
	ClassDB::bind_method(D_METHOD("get_data_texture"), &TerrainDataMap3D::get_data_texture);
	ClassDB::bind_method(D_METHOD("get_water_texture"), &TerrainDataMap3D::get_water_texture);
	ClassDB::bind_method(D_METHOD("get_channel_image", "channel"), &TerrainDataMap3D::get_channel_image);
	ClassDB::bind_static_method("TerrainDataMap3D", D_METHOD("get_shader_code"), &TerrainDataMap3D::get_shader_code);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "size", PROPERTY_HINT_RANGE, "1,16384,1,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_RANGE, "8,4096,1"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "height_source"), "set_height_source", "get_height_source");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_ground_collision_mask", "get_ground_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "water_sources", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_water_sources", "get_water_sources");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_exponent", PROPERTY_HINT_RANGE, "0.5,10,0.01"), "set_flow_exponent", "get_flow_exponent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_on_ready"), "set_bake_on_ready", "is_bake_on_ready");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_shader_globals"), "set_publish_shader_globals", "is_publishing_shader_globals");

	ADD_SIGNAL(MethodInfo("baked"));

	BIND_ENUM_CONSTANT(CHANNEL_HEIGHT);
	BIND_ENUM_CONSTANT(CHANNEL_SLOPE);
	BIND_ENUM_CONSTANT(CHANNEL_ASPECT);
	BIND_ENUM_CONSTANT(CHANNEL_CURVATURE);
	BIND_ENUM_CONSTANT(CHANNEL_FLOW);
	BIND_ENUM_CONSTANT(CHANNEL_WETNESS);
	BIND_ENUM_CONSTANT(CHANNEL_WATER_DEPTH);
	BIND_ENUM_CONSTANT(CHANNEL_WATER_DISTANCE);
	BIND_ENUM_CONSTANT(CHANNEL_MAX);
}
