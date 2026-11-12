/**************************************************************************/
/*  terrain_generator_3d.cpp                                              */
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

#include "terrain_generator_3d.h"

#include "terrain_coast.h"
#include "terrain_deformer_3d.h"
#include "terrain_erosion_filter.h"
#include "terrain_geology.h"
#include "terrain_glacial.h"
#include "terrain_hydraulic.h"
#include "terrain_runoff.h"
#include "terrain_sky.h"
#include "terrain_stream_power.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/path_3d.h"
#include "scene/3d/world_environment.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/curve.h"
#include "scene/resources/environment.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

#include <algorithm>

static inline int _gen_floor_div(int p_a, int p_b) {
	const int q = p_a / p_b;
	return (p_a % p_b != 0 && ((p_a < 0) != (p_b < 0))) ? q - 1 : q;
}

// Seeded hash in [0, 1): a cell's starting roughness, the only thing the seed changes. Small differences here pick
// different drainage networks, which is what makes two seeds different landscapes.
static inline double _gen_hash01(uint32_t p_seed, uint32_t p_i) {
	uint32_t x = p_i * 0x9E3779B1u ^ (p_seed + 0x7F4A7C15u) * 0x85EBCA6Bu;
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return double(x) / 4294967296.0;
}

// Smooth value noise in -1..1 (bilinear with a smoothstep fade on a hashed lattice), for slow wobbles.
static double _gen_value_noise(uint32_t p_seed, double p_x, double p_z, double p_wavelength) {
	const double x = p_x / p_wavelength, z = p_z / p_wavelength;
	const double fx = Math::floor(x), fz = Math::floor(z);
	const int32_t ix = int32_t(fx), iz = int32_t(fz);
	double tx = x - fx, tz = z - fz;
	tx = tx * tx * (3.0 - 2.0 * tx);
	tz = tz * tz * (3.0 - 2.0 * tz);
	auto at = [&](int32_t a, int32_t b) { return 2.0 * _gen_hash01(p_seed, uint32_t(a) * 73856093u ^ uint32_t(b) * 19349663u) - 1.0; };
	const double a = Math::lerp(at(ix, iz), at(ix + 1, iz), tx);
	const double b = Math::lerp(at(ix, iz + 1), at(ix + 1, iz + 1), tx);
	return Math::lerp(a, b, tz);
}

void TerrainGenerator3D::set_size(const Vector2 &p_size) {
	size = Vector2(MAX(p_size.x, real_t(0.0)), MAX(p_size.y, real_t(0.0)));
	update_configuration_warnings();
}

void TerrainGenerator3D::set_cell_size(real_t p_size) {
	cell_size = MAX(p_size, real_t(0.25));
	update_configuration_warnings();
}

Rect2 TerrainGenerator3D::get_world_rect() const {
	const Vector3 c = is_inside_tree() ? get_global_position() : get_position();
	return Rect2(c.x - size.x * 0.5, c.z - size.y * 0.5, size.x, size.y);
}

Node *TerrainGenerator3D::_get_terrain_node() const {
	if (!is_inside_tree()) {
		return nullptr;
	}
	Node *node = get_node_or_null(terrain_path);
	if (node) {
		return node;
	}
	TerrainDeformer3D *def = Object::cast_to<TerrainDeformer3D>(get_node_or_null(deformer_path));
	if (def) {
		return def->get_node_or_null(def->get_terrain());
	}
	return nullptr;
}

void TerrainGenerator3D::_build_uplift(LocalVector<double> &r_uplift) const {
	const Rect2 rect = get_world_rect();
	const int n = nx * ny;
	LocalVector<float> field;
	field.resize(n);
	for (int i = 0; i < n; i++) {
		field[i] = base_uplift;
	}

	// Painted map over the whole rectangle (red channel, 0..1).
	if (uplift_map.is_valid()) {
		Ref<Image> img = uplift_map->get_image();
		if (img.is_valid() && !img->is_empty()) {
			img = img->duplicate();
			if (img->is_compressed()) {
				img->decompress();
			}
			img->convert(Image::FORMAT_RF);
			const int iw = img->get_width();
			const int ih = img->get_height();
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					// Bilinear, texel centres at (k + 0.5) / size.
					const real_t u = CLAMP((real_t(x) / MAX(nx - 1, 1)) * iw - 0.5, real_t(0.0), real_t(iw - 1));
					const real_t v = CLAMP((real_t(z) / MAX(ny - 1, 1)) * ih - 0.5, real_t(0.0), real_t(ih - 1));
					const int u0 = int(u);
					const int v0 = int(v);
					const int u1 = MIN(u0 + 1, iw - 1);
					const int v1 = MIN(v0 + 1, ih - 1);
					const real_t fu = u - u0;
					const real_t fv = v - v0;
					const real_t a = Math::lerp(img->get_pixel(u0, v0).r, img->get_pixel(u1, v0).r, fu);
					const real_t b = Math::lerp(img->get_pixel(u0, v1).r, img->get_pixel(u1, v1).r, fu);
					field[z * nx + x] = MAX(field[z * nx + x], float(CLAMP(Math::lerp(a, b, fv), real_t(0.0), real_t(1.0))));
				}
			}
		}
	}

	// Ranges along Path3D curves: a raised-cosine band around the curve, its half width from the path's "width"
	// metadata or range_width, its strength from "uplift" metadata or 1.
	for (int r = 0; r < ranges.size(); r++) {
		Path3D *path = Object::cast_to<Path3D>(get_node_or_null(ranges[r]));
		if (!path || path->get_curve().is_null()) {
			continue;
		}
		const PackedVector3Array pts = path->get_curve()->get_baked_points();
		if (pts.size() < 2) {
			continue;
		}
		const Transform3D xf = path->get_global_transform();
		const real_t w = path->has_meta("width") ? real_t(path->get_meta("width")) : range_width;
		const real_t strength = path->has_meta("uplift") ? real_t(path->get_meta("uplift")) : real_t(1.0);
		if (w <= 0.0 || path->has_meta("basin")) {
			continue;
		}
		for (int s = 0; s + 1 < pts.size(); s++) {
			const Vector3 a3 = xf.xform(pts[s]);
			const Vector3 b3 = xf.xform(pts[s + 1]);
			const Vector2 a(a3.x, a3.z);
			const Vector2 b(b3.x, b3.z);
			const Vector2 ab = b - a;
			const real_t len2 = ab.length_squared();
			const int x0 = MAX(0, int(Math::floor((MIN(a.x, b.x) - w - rect.position.x) / cell_size)));
			const int x1 = MIN(nx - 1, int(Math::ceil((MAX(a.x, b.x) + w - rect.position.x) / cell_size)));
			const int z0 = MAX(0, int(Math::floor((MIN(a.y, b.y) - w - rect.position.y) / cell_size)));
			const int z1 = MIN(ny - 1, int(Math::ceil((MAX(a.y, b.y) + w - rect.position.y) / cell_size)));
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const Vector2 p(rect.position.x + x * cell_size, rect.position.y + z * cell_size);
					const real_t t = len2 > 0.0 ? CLAMP((p - a).dot(ab) / len2, real_t(0.0), real_t(1.0)) : real_t(0.0);
					const real_t d = p.distance_to(a + ab * t) / w;
					if (d < 1.0) {
						const float v = float(strength * (0.5 + 0.5 * Math::cos(Math::PI * d)));
						field[z * nx + x] = MAX(field[z * nx + x], v);
					}
				}
			}
		}
	}

	// Basins: paths with "basin" metadata scale the uplift down to their "uplift" share along the curve (raised
	// cosine over the width), after the map and ranges: a high bowl among the peaks, a cirque floor, a pass.
	for (int r = 0; r < ranges.size(); r++) {
		Path3D *path = Object::cast_to<Path3D>(get_node_or_null(ranges[r]));
		if (!path || path->get_curve().is_null() || !path->has_meta("basin")) {
			continue;
		}
		const PackedVector3Array pts = path->get_curve()->get_baked_points();
		const Transform3D xf = path->get_global_transform();
		const real_t w = path->has_meta("width") ? real_t(path->get_meta("width")) : range_width;
		const real_t share = path->has_meta("uplift") ? real_t(path->get_meta("uplift")) : real_t(0.0);
		if (w <= 0.0 || pts.is_empty()) {
			continue;
		}
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const Vector2 p(rect.position.x + x * cell_size, rect.position.y + z * cell_size);
				real_t dmin = 1e30;
				for (int k = 0; k < pts.size(); k++) {
					const Vector3 a3 = xf.xform(pts[k]);
					if (k + 1 < pts.size()) {
						const Vector3 b3 = xf.xform(pts[k + 1]);
						const Vector2 a(a3.x, a3.z), ab = Vector2(b3.x, b3.z) - a;
						const real_t len2 = ab.length_squared();
						const real_t t = len2 > 0.0 ? CLAMP((p - a).dot(ab) / len2, real_t(0.0), real_t(1.0)) : real_t(0.0);
						dmin = MIN(dmin, p.distance_to(a + ab * t));
					} else {
						dmin = MIN(dmin, p.distance_to(Vector2(a3.x, a3.z)));
					}
				}
				const real_t d = dmin / w;
				if (d < 1.0) {
					const real_t c = 0.5 + 0.5 * Math::cos(Math::PI * d);
					field[z * nx + x] *= float(Math::lerp(real_t(1.0), share, c));
				}
			}
		}
	}

	// Noise modulates what the map and ranges set: 1 where the noise is high, 1 - strength where it is low.
	if (uplift_noise.is_valid() && uplift_noise_strength > 0.0) {
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const real_t t = CLAMP(0.5 + 0.5 * uplift_noise->get_noise_2d(rect.position.x + x * cell_size, rect.position.y + z * cell_size), real_t(0.0), real_t(1.0));
				field[z * nx + x] *= float(1.0 - uplift_noise_strength * (1.0 - t));
			}
		}
	}

	r_uplift.resize(n);
	const double rate = double(uplift) * 1.0e-3; // mm/yr to m/yr
	for (int i = 0; i < n; i++) {
		r_uplift[i] = rate * double(field[i]);
	}
}

bool TerrainGenerator3D::generate() {
	// Readers on other threads (Landscape3D streaming heights) finish before any grid is replaced.
	emit_signal(SNAME("changing"));
	if (size.x <= 0.0 || size.y <= 0.0) {
		clear_result();
		return false;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	result_rect = get_world_rect();
	result_cell = cell_size;
	nx = int(Math::ceil(size.x / cell_size)) + 1;
	ny = int(Math::ceil(size.y / cell_size)) + 1;
	const int n = nx * ny;
	ERR_FAIL_COND_V_MSG(int64_t(nx) * ny > 64 * 1024 * 1024, false, "TerrainGenerator3D: grid too large; raise cell_size.");

	LocalVector<double> h, up, area, sed, rd, scratch, temp;
	LocalVector<int32_t> rec, stack, donor_count, donor_offset, donors, work, order;
	LocalVector<uint64_t> sort_keys;
	LocalVector<uint32_t> radix_count;
	LocalVector<uint8_t> flags;
	h.resize(n);
	area.resize(n);
	memset(area.ptr(), 0, sizeof(double) * n); // the layered rock reads the last step's area before the first
	sed.resize(n);
	rd.resize(n);
	scratch.resize(n);
	temp.resize(n);
	rec.resize(n);
	for (int i = 0; i < n; i++) {
		rec[i] = i;
	}
	stack.resize(n);
	donor_count.resize(n);
	donor_offset.resize(n + 1);
	donors.resize(n);
	work.resize(n);
	order.resize(n);
	sort_keys.resize(n * 2);
	radix_count.resize(65536);
	flags.resize(n);
	_build_uplift(up);
	for (int i = 0; i < n; i++) {
		h[i] = double(sea_level) + double(initial_roughness) * _gen_hash01(uint32_t(seed), uint32_t(i));
		sed[i] = 0.0;
	}

	TerrainStreamPower sp;
	sp.p.nx = nx;
	sp.p.ny = ny;
	sp.p.dx = cell_size;
	sp.p.dt = time_step;
	sp.p.erodibility = erodibility;
	sp.p.area_exponent = area_exponent;
	sp.p.diffusion = hillslope_diffusion;
	sp.p.deposition = deposition;
	result_deposition = deposition > 0.0;
	sp.p.talus = critical_slope > 0.0 ? Math::tan(Math::deg_to_rad(double(critical_slope))) : 0.0;
	sp.p.base_level = sea_level;
	sp.p.outlet_edges = outlet_edges;
	sp.p.flow_exponent = flow_exponent;
	sp.b.height = h.ptr();
	sp.b.uplift = up.ptr();
	sp.b.area = area.ptr();
	sp.b.sediment = sed.ptr();
	sp.b.receiver = rec.ptr();
	sp.b.receiver_dist = rd.ptr();
	sp.b.stack = stack.ptr();
	sp.b.donor_count = donor_count.ptr();
	sp.b.donor_offset = donor_offset.ptr();
	sp.b.donors = donors.ptr();
	sp.b.work = work.ptr();
	sp.b.scratch = scratch.ptr();
	sp.b.temp = temp.ptr();
	sp.b.flags = flags.ptr();
	sp.b.order = order.ptr();
	sp.b.sort_keys = sort_keys.ptr();
	sp.b.radix_count = radix_count.ptr();

	// The sea is base level: its cells are outlets, held at sea_level, so the land drains to the coast. With no sea
	// and no outlet edge, one corner cell is the outlet so water can still leave.
	LocalVector<uint8_t> sea_mask;
	_build_sea_mask(sea_mask);
	has_sea = false;
	for (int i = 0; i < n && !has_sea; i++) {
		has_sea = sea_mask[i] != 0;
	}
	LocalVector<uint8_t> outlet_mask;
	if (has_sea) {
		sp.b.outlet = sea_mask.ptr();
		for (int i = 0; i < n; i++) {
			if (sea_mask[i]) {
				h[i] = sea_level;
			}
		}
	} else if (outlet_edges == 0) {
		outlet_mask.resize(n);
		memset(outlet_mask.ptr(), 0, n);
		outlet_mask[0] = 1;
		sp.b.outlet = outlet_mask.ptr();
	}

	// Land beyond the map draining in at the inflow markers, on every grid the steps run on.
	const LocalVector<InflowPoint> inflow_points = _inflow_points();
	LocalVector<double> inflow_fine, sill_fine, level_fine, water_fine;
	if (!inflow_points.is_empty()) {
		_inflow_grid(inflow_points, nx, ny, cell_size, sp.b.outlet, inflow_fine, sill_fine, level_fine, &water_fine);
		sp.b.inflow_area = inflow_fine.ptr();
		sp.b.sill = sill_fine.is_empty() ? nullptr : sill_fine.ptr();
		sp.b.sill_level = level_fine.is_empty() ? nullptr : level_fine.ptr();
	}

	// Layered rock (Forte, Yanites and Whipple 2016; Haviv et al. 2010): bands of hard rock in a sequence that rises
	// with the uplift. A node's rock is the layer at its stratigraphic height: the ground height minus the uplift
	// since the start (the rock under it has risen that far), plus the dip along its direction and a slow wobble.
	// Hard rock erodes strata_hardness times slower and stands at strata_cliff_angle, so where a river cuts down
	// through a hard band it steps down over a cliff: a knickpoint, a waterfall where the band is thick enough.
	LocalVector<double> strata_k, strata_talus, strata_offset, strata_depth, strata_creep;
	double strata_time = 0.0;
	const double soft_talus = critical_slope > 0.0 ? Math::tan(Math::deg_to_rad(double(critical_slope))) : 0.0;
	const double hard_talus = Math::tan(Math::deg_to_rad(double(strata_cliff_angle)));
	auto strata_weight = [&](double q) -> double {
		const double period = double(strata_spacing);
		const double phase = Math::fposmod(q, period) / period;
		const double f = double(strata_hard_fraction);
		const double inside = phase < f ? MIN(phase, f - phase) : -MIN(phase - f, 1.0 - phase);
		const double edge = MIN(2.0, 0.1 * period) / period; // a soft contact a couple of metres thick
		const double t = CLAMP((inside + edge) / (2.0 * edge), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	};
	auto strata_prepare = [&](int p_nx, int p_ny, double p_dx) {
		if (!strata_enabled) {
			return;
		}
		const int nn = p_nx * p_ny;
		strata_k.resize(nn);
		strata_talus.resize(nn);
		strata_offset.resize(nn);
		strata_depth.resize(nn);
		strata_creep.resize(nn);
		const double dip = Math::tan(Math::deg_to_rad(double(strata_dip)));
		const double dir = Math::deg_to_rad(double(strata_dip_direction));
		const double ddx = Math::sin(dir), ddz = -Math::cos(dir);
		for (int z = 0; z < p_ny; z++) {
			for (int x = 0; x < p_nx; x++) {
				const double px = result_rect.position.x + x * p_dx;
				const double pz = result_rect.position.y + z * p_dx;
				strata_offset[z * p_nx + x] = dip * (px * ddx + pz * ddz) + double(strata_wobble) * TerrainErosionFilter::fbm(uint32_t(seed) * 2654435761u + 17u, px, pz, 700.0, 3);
			}
		}
		sp.b.erodibility_scale = strata_k.ptr();
		sp.b.talus_cell = strata_talus.ptr();
		sp.b.hard_depth = strata_depth.ptr();
		sp.b.diffusion_scale = strata_creep.ptr();
	};
	auto strata_update = [&](int p_n, const double *p_h, const double *p_up) {
		if (!strata_enabled) {
			return;
		}
		// Hard rock erodes slowly everywhere, but only channels stand as cliffs and stop creeping: the river's bed holds
		// the step, while hillslopes in hard rock still fail along joints at the soft rock's critical slope (cliffs on
		// every hard hillslope cell would leave pinnacles on the grid).
		const double ac = double(strata_channel_area);
		for (int i = 0; i < p_n; i++) {
			const double q = p_h[i] - p_up[i] * strata_time + strata_offset[i];
			const double a = sp.b.area[i];
			const double g = ac <= 0.0 ? 1.0 : CLAMP((a - 0.5 * ac) / ac, 0.0, 1.0);
			const double wr = strata_weight(q);
			const double w = wr * g * g * (3.0 - 2.0 * g);
			const double depth = Math::fposmod(q, double(strata_spacing));
			strata_depth[i] = w > 0.5 && depth < double(strata_hard_fraction) * double(strata_spacing) ? depth : 0.0;
			strata_k[i] = 1.0 + (1.0 / double(strata_hardness) - 1.0) * wr;
			strata_talus[i] = soft_talus + (hard_talus - soft_talus) * w;
			strata_creep[i] = 1.0 - w;
		}
		// The soft rock right under a hard lip is the lower part of its face, worn back by the plunge pool and kept
		// as steep as the hard rock above it stands (Scheingross and Lamb 2017).
		for (int i = 0; i < p_n; i++) {
			const int r = sp.b.receiver[i];
			if (r != i && r >= 0 && r < p_n && strata_depth[i] > 0.0) {
				strata_talus[r] = MAX(strata_talus[r], strata_talus[i]);
			}
		}
	};
	// Ice ages between the rivers' steps (Cordonnier et al. 2023 alternate glacial and fluvial periods): every
	// eighth step the glaciers of the ground as it stands erode for glacial_share of those steps' time.
	TerrainGlacial glac;
	glac.p.ela = glacial_ela;
	glac.p.erodibility *= double(glacial_erosion);
	glac.p.sliding = glacial_sliding;
	// Glaciers whose catchment lies beyond the map come in with their rivers: each inflow brings its off-map area
	// times the mean net balance there as ice.
	glac.sources.clear();
	if (glacial_inflow_balance > 0.0) {
		for (const InflowPoint &ip : inflow_points) {
			TerrainGlacial::Source src;
			src.u = (ip.position.x - result_rect.position.x) / MAX(result_rect.size.x, real_t(1.0));
			src.v = (ip.position.y - result_rect.position.y) / MAX(result_rect.size.y, real_t(1.0));
			src.q = ip.area * double(glacial_inflow_balance);
			glac.sources.push_back(src);
		}
	}
	// The ice ages between the river steps can run on a coarser ice grid (glacial_coarse_cell): the steady-ice solve
	// is 96% of the stage, and a grid twice as coarse has a quarter of the nodes and takes four times longer pseudo
	// steps. The last glaciation, the one whose basins and walls are left, always runs on glac's 64 m grid.
	TerrainGlacial glac_mid = glac;
	TerrainGlacial *mid = &glac;
	if (glacial_coarse_cell > 0.0) {
		glac_mid.p.cell = double(glacial_coarse_cell);
		mid = &glac_mid;
	}
	const bool glacial = glacial_enabled && glacial_erosion > 0.0;
	uint64_t glacial_usec = 0;
	int glacial_count = 0;
	// Rock quality under the ice: joint density varies over hundreds of metres, and glaciers quarry well-jointed rock
	// far faster (Duhnforth et al. 2010). exp(variation x two octaves of noise), mean about 1; soft patches dig
	// basins, sound rock stands as bars (Cook and Swift 2012). Built per grid size (coarse levels have their own).
	std::vector<double> rock_scale, rock_combined;
	int rock_scale_nx = 0;
	auto glacial_scale = [&](int p_nx, int p_ny, double p_dx) -> const double * {
		if (glacial_rock_variation <= 0.0) {
			return nullptr;
		}
		if (rock_scale_nx != p_nx || rock_scale.size() != size_t(p_nx) * p_ny) {
			rock_scale.resize(size_t(p_nx) * p_ny);
			rock_scale_nx = p_nx;
			const uint32_t rs = uint32_t(seed) * 0x9E3779B1u + 4242u;
			const double L = double(glacial_rock_scale);
			for (int z = 0; z < p_ny; z++) {
				for (int x = 0; x < p_nx; x++) {
					const double wx = result_rect.position.x + x * p_dx, wz = result_rect.position.y + z * p_dx;
					const double v = 0.75 * _gen_value_noise(rs, wx, wz, L) + 0.35 * _gen_value_noise(rs + 1u, wx, wz, 0.43 * L);
					rock_scale[size_t(z) * p_nx + x] = Math::exp(double(glacial_rock_variation) * v);
				}
			}
		}
		// Hard beds of the strata quarry slower too (riegels where a trough crosses a hard bed).
		if (strata_enabled && strata_k.size() == uint32_t(p_nx * p_ny)) {
			rock_combined.resize(rock_scale.size());
			for (size_t i = 0; i < rock_scale.size(); i++) {
				rock_combined[i] = rock_scale[i] * strata_k[i];
			}
			return rock_combined.data();
		}
		return rock_scale.data();
	};
	auto strata_step = [&](int p_n, const double *p_h, const double *p_up) -> double {
		strata_update(p_n, p_h, p_up);
		const double c = sp.step();
		strata_time += double(time_step);
		if (glacial && glacial_share > 0.0 && ++glacial_count % 8 == 0) {
			const uint64_t tg = OS::get_singleton()->get_ticks_usec();
			mid->erode(sp.b.height, sp.p.nx, sp.p.ny, sp.p.dx, 8.0 * double(time_step) * double(glacial_share), sp.b.sediment, sp.b.outlet, sp.b.sill, sp.b.sill_level, glacial_scale(sp.p.nx, sp.p.ny, sp.p.dx));
			glacial_usec += OS::get_singleton()->get_ticks_usec() - tg;
		}
		return c;
	};

	// Coarse to fine (the V-cycle of Tzathas et al. 2024, with our time-stepping solver at every level): the
	// steps run on grids halved 2x2 at a time, where the large-scale relief and drainage form for a sixteenth of
	// the cost per halving, then each finer level starts from the bilinear upsample of the one below (plus the
	// initial roughness, so flats break the same way) and runs refine_steps to grow its own small valleys,
	// hillslopes and talus. Uplift is restricted by full weighting, outlets and the sea by injection.
	int levels = 0;
	{
		int cx = nx, cz = ny;
		const int max_levels = coarse_levels < 0 ? 8 : coarse_levels;
		while (levels < max_levels && (cx - 1) % 2 == 0 && (cz - 1) % 2 == 0 && (cx - 1) / 2 + 1 >= (coarse_levels < 0 ? 129 : 9) && (cz - 1) / 2 + 1 >= (coarse_levels < 0 ? 129 : 9)) {
			cx = (cx - 1) / 2 + 1;
			cz = (cz - 1) / 2 + 1;
			levels++;
		}
	}
	double change = 0.0;
	if (levels == 0) {
		strata_prepare(nx, ny, cell_size);
		for (int s = 0; s < steps; s++) {
			change = strata_step(n, h.ptr(), up.ptr());
		}
	} else {
		struct Level {
			int nx = 0, ny = 0;
			LocalVector<double> up, h, sed, inflow, sill, level;
			LocalVector<uint8_t> outlet;
		};
		LocalVector<Level> lv;
		lv.resize(levels + 1);
		lv[0].nx = nx;
		lv[0].ny = ny;
		for (int l = 1; l <= levels; l++) {
			const Level &f = lv[l - 1];
			Level &c = lv[l];
			c.nx = (f.nx - 1) / 2 + 1;
			c.ny = (f.ny - 1) / 2 + 1;
			const double *fu = l == 1 ? up.ptr() : f.up.ptr();
			const uint8_t *fo = l == 1 ? sp.b.outlet : (f.outlet.is_empty() ? nullptr : f.outlet.ptr());
			c.up.resize(c.nx * c.ny);
			if (fo) {
				c.outlet.resize(c.nx * c.ny);
			}
			for (int z = 0; z < c.ny; z++) {
				for (int x = 0; x < c.nx; x++) {
					double sum = 0.0, wsum = 0.0;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dx = -1; dx <= 1; dx++) {
							const int fx = 2 * x + dx, fz = 2 * z + dz;
							if (fx < 0 || fz < 0 || fx >= f.nx || fz >= f.ny) {
								continue;
							}
							const double w = (dx == 0 ? 2.0 : 1.0) * (dz == 0 ? 2.0 : 1.0);
							sum += w * fu[fz * f.nx + fx];
							wsum += w;
						}
					}
					c.up[z * c.nx + x] = sum / wsum;
					if (fo) {
						c.outlet[z * c.nx + x] = fo[(2 * z) * f.nx + 2 * x];
					}
				}
			}
		}
		const uint8_t *fine_outlet = sp.b.outlet;
		auto run = [&](int l, double *p_h, double *p_sed, int p_steps) {
			Level &L = lv[l];
			sp.p.nx = L.nx;
			sp.p.ny = L.ny;
			sp.p.dx = double(cell_size) * double(1 << l);
			sp.b.height = p_h;
			sp.b.sediment = p_sed;
			sp.b.uplift = l == 0 ? up.ptr() : L.up.ptr();
			sp.b.outlet = l == 0 ? fine_outlet : (L.outlet.is_empty() ? nullptr : L.outlet.ptr());
			sp.invalidate_outlets();
			if (!inflow_points.is_empty()) {
				if (l == 0) {
					sp.b.inflow_area = inflow_fine.ptr();
					sp.b.sill = sill_fine.is_empty() ? nullptr : sill_fine.ptr();
					sp.b.sill_level = level_fine.is_empty() ? nullptr : level_fine.ptr();
				} else {
					_inflow_grid(inflow_points, L.nx, L.ny, sp.p.dx, sp.b.outlet, L.inflow, L.sill, L.level, nullptr);
					sp.b.inflow_area = L.inflow.ptr();
					sp.b.sill = L.sill.is_empty() ? nullptr : L.sill.ptr();
					sp.b.sill_level = L.level.is_empty() ? nullptr : L.level.ptr();
				}
			}
			strata_prepare(L.nx, L.ny, sp.p.dx);
			for (int s = 0; s < p_steps; s++) {
				change = strata_step(L.nx * L.ny, p_h, sp.b.uplift);
			}
		};
		// Coarsest level: the full run from the initial roughness.
		{
			Level &L = lv[levels];
			L.h.resize(L.nx * L.ny);
			L.sed.resize(L.nx * L.ny);
			for (int i = 0; i < L.nx * L.ny; i++) {
				const bool o = !L.outlet.is_empty() && L.outlet[i];
				L.h[i] = o ? double(sea_level) : double(sea_level) + double(initial_roughness) * _gen_hash01(uint32_t(seed), uint32_t(i));
				L.sed[i] = 0.0;
			}
			run(levels, L.h.ptr(), L.sed.ptr(), steps);
		}
		for (int l = levels - 1; l >= 0; l--) {
			Level &c = lv[l + 1];
			Level &f = lv[l];
			double *fh = h.ptr();
			double *fs = sed.ptr();
			if (l > 0) {
				f.h.resize(f.nx * f.ny);
				f.sed.resize(f.nx * f.ny);
				fh = f.h.ptr();
				fs = f.sed.ptr();
			}
			const uint8_t *fo = l == 0 ? fine_outlet : (f.outlet.is_empty() ? nullptr : f.outlet.ptr());
			for (int z = 0; z < f.ny; z++) {
				const int z0 = MIN(z / 2, c.ny - 1), z1 = MIN(z0 + 1, c.ny - 1);
				const double tz = (z & 1) ? 0.5 : 0.0;
				for (int x = 0; x < f.nx; x++) {
					const int x0 = MIN(x / 2, c.nx - 1), x1 = MIN(x0 + 1, c.nx - 1);
					const double tx = (x & 1) ? 0.5 : 0.0;
					auto bil = [&](const LocalVector<double> &a) {
						const double a0 = a[z0 * c.nx + x0] * (1.0 - tx) + a[z0 * c.nx + x1] * tx;
						const double a1 = a[z1 * c.nx + x0] * (1.0 - tx) + a[z1 * c.nx + x1] * tx;
						return a0 * (1.0 - tz) + a1 * tz;
					};
					const int i = z * f.nx + x;
					if (fo && fo[i]) {
						fh[i] = double(sea_level);
					} else {
						fh[i] = bil(c.h) + double(initial_roughness) * _gen_hash01(uint32_t(seed) + 7919u * uint32_t(l + 1), uint32_t(i));
					}
					fs[i] = bil(c.sed);
				}
			}
			run(l, fh, fs, refine_steps);
			c.h.clear();
			c.sed.clear();
		}
		sp.p.nx = nx;
		sp.p.ny = ny;
		sp.p.dx = cell_size;
		sp.b.height = h.ptr();
		sp.b.sediment = sed.ptr();
		sp.b.uplift = up.ptr();
		sp.b.outlet = fine_outlet;
		sp.invalidate_outlets();
		sp.b.inflow_area = inflow_fine.is_empty() ? nullptr : inflow_fine.ptr();
		sp.b.sill = sill_fine.is_empty() ? nullptr : sill_fine.ptr();
		sp.b.sill_level = level_fine.is_empty() ? nullptr : level_fine.ptr();
	}
	// The last glaciation, after the rivers: its basins are left as they are (lakes, _basin_lakes), so the routing
	// below runs on a filled copy.
	glacial_ice.clear();
	last_glacial_eroded = 0.0;
	last_glacial_ice_area = 0.0;
	LocalVector<double> unfilled;
	// Scree: cliffs shed rock for scree_age years (postglacial rockwall retreat, about 0.5 mm/yr: Sass 2005;
	// Hales and Roering 2005); it falls down the steepest way, gathers in the gullies and stops at the cliff's foot,
	// where it builds cones against the wall at the angle of repose (talus cones, Statham 1976; Luckman 2013).
	// Loose and coarse: no soil is added on it.
	last_scree_volume = 0.0;
	scree_cover.clear();
	till_cover.clear();
	auto scree_pass = [&]() {
		if (scree_age <= 0.0) {
			return;
		}
		const double c = double(cell_size);
		const double cliff = Math::tan(Math::deg_to_rad(50.0)); // a rock wall, not a sharp crest between two slopes
		const double stream_area = 5.0e4; // m2: a perennial stream's head in these mountains
		const double foot = Math::tan(Math::deg_to_rad(38.0));
		const double repose = Math::tan(Math::deg_to_rad(double(talus_angle)));
		const double retreat = 0.0005;
		LocalVector<double> load, dep;
		load.resize(n);
		dep.resize(n);
		// Basins (the lakes to come): priority-flood fill from the map edge and the sea (Barnes, Lehman and Mulla
		// 2014). Rock falling into standing water sinks to its bed; cones are built only above the water line.
		LocalVector<double> water;
		water.resize(n);
		{
			typedef std::pair<double, int32_t> Item;
			std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
			LocalVector<uint8_t> seen;
			seen.resize(n);
			memset(seen.ptr(), 0, n);
			for (int i = 0; i < n; i++) {
				const int x = i % nx, z = i / nx;
				if (x == 0 || z == 0 || x == nx - 1 || z == ny - 1 || (sp.b.outlet && sp.b.outlet[i])) {
					water[i] = h[i];
					seen[i] = 1;
					pq.push(Item(h[i], i));
				}
			}
			while (!pq.empty()) {
				const Item it = pq.top();
				pq.pop();
				const int x = it.second % nx, z = it.second / nx;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, zz = z + dz;
						if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						const int j = zz * nx + xx;
						if (seen[j]) {
							continue;
						}
						seen[j] = 1;
						water[j] = MAX(h[j], it.first);
						pq.push(Item(water[j], j));
					}
				}
			}
		}
		// A river's floor: cells within 48 m of a channel draining 1 km2 or more. The river works its floor and
		// carries off the toe of any talus that reaches it, so no talus is laid there (its pools and lakes stay).
		{
			const int rr = MAX(1, int(Math::ceil(48.0 / c)));
			LocalVector<uint8_t> near;
			near.resize(n);
			memset(near.ptr(), 0, n);
			for (int i = 0; i < n; i++) {
				if (area[i] < 1.0e6) {
					continue;
				}
				const int x = i % nx, z = i / nx;
				for (int zz = MAX(z - rr, 0); zz <= MIN(z + rr, ny - 1); zz++) {
					for (int xx = MAX(x - rr, 0); xx <= MIN(x + rr, nx - 1); xx++) {
						near[zz * nx + xx] = 1;
					}
				}
			}
			for (int i = 0; i < n; i++) {
				if (near[i]) {
					water[i] = MAX(water[i], h[i] + 1.0);
				}
			}
		}
		auto wet = [&](int i) { return water[i] > h[i] + 0.5; };
		auto steepest = [&](int i, double &r_g) {
			const int x = i % nx, z = i / nx;
			int best = -1;
			r_g = 0.0;
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int xx = x + dx, zz = z + dz;
					if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
						continue;
					}
					const int j = zz * nx + xx;
					const double g = (h[i] - h[j]) / ((dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * c);
					if (g > r_g) {
						r_g = g;
						best = j;
					}
				}
			}
			return best;
		};
		for (int i = 0; i < n; i++) {
			double g;
			steepest(i, g);
			load[i] = g > cliff ? retreat * scree_age * c * c * Math::sqrt(1.0 + g * g) : 0.0;
			dep[i] = 0.0;
		}
		LocalVector<int32_t> ord;
		ord.resize(n);
		for (int i = 0; i < n; i++) {
			ord[i] = i;
		}
		const double *hp = h.ptr();
		std::sort(ord.ptr(), ord.ptr() + n, [hp](int32_t a, int32_t b) { return hp[a] > hp[b]; });
		const uint8_t *fixed = sp.b.outlet;
		for (int k = 0; k < n; k++) {
			const int32_t i = ord[k];
			if (load[i] <= 0.0) {
				continue;
			}
			double g;
			const int j = steepest(i, g);
			if (j < 0 || (fixed && fixed[j])) {
				continue; // into the sea or off the map
			}
			if (g < foot) {
				dep[i] += load[i]; // the foot of the wall
				continue;
			}
			// Down every lower neighbour by slope^4 (multiple flow directions, close to the steepest path): a plain
			// face spreads its fall into an apron along its foot, gullies gather theirs into cones.
			const int x = i % nx, z = i / nx;
			int nbs[8];
			double wts[8];
			int cnt = 0;
			double wsum = 0.0;
			for (int dz = -1; dz <= 1; dz++) {
				for (int dx = -1; dx <= 1; dx++) {
					const int xx = x + dx, zz = z + dz;
					if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
						continue;
					}
					const int q = zz * nx + xx;
					const double gq = (h[i] - h[q]) / ((dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * c);
					if (gq > 0.0) {
						nbs[cnt] = q;
						wts[cnt] = gq * gq * gq * gq;
						wsum += wts[cnt];
						cnt++;
					}
				}
			}
			for (int q = 0; q < cnt; q++) {
				const int t = nbs[q];
				if ((fixed && fixed[t]) || area[t] > stream_area || wet(t)) {
					continue; // a stream takes what falls into it (talus lines valleys, it does not dam them); a lake swallows it
				}
				load[t] += load[i] * wts[q] / wsum;
			}
		}
		// A half cone against the wall, V = pi r^2 t / 6 with t = r tan(repose), merged by maximum. Its toe is
		// concave (the larger blocks run further, fall sorting): the last third of the radius eases out.
		LocalVector<double> top;
		top.resize(n);
		memcpy(top.ptr(), h.ptr(), sizeof(double) * n);
		for (int i = 0; i < n; i++) {
			if (dep[i] <= 0.0) {
				continue;
			}
			last_scree_volume += dep[i];
			const double t = Math::pow(6.0 * dep[i] * repose * repose / Math::PI, 1.0 / 3.0);
			const double r = t / repose;
			const int rc = int(Math::ceil(r / c)) + 1;
			const int x = i % nx, z = i / nx;
			for (int zz = MAX(z - rc, 0); zz <= MIN(z + rc, ny - 1); zz++) {
				for (int xx = MAX(x - rc, 0); xx <= MIN(x + rc, nx - 1); xx++) {
					const int j = zz * nx + xx;
					if ((fixed && fixed[j]) || area[j] > stream_area || wet(j)) {
						continue; // the stream keeps its bed (it carries off the toe), the lake its basin
					}
					const double d = Math::sqrt(double((xx - x) * (xx - x) + (zz - z) * (zz - z))) * c;
					const double u = d / r;
					double zc;
					if (u < 0.67) {
						zc = t - repose * d;
					} else {
						const double v = (u - 0.67) / 0.33; // 0..1 over the toe: slope eases to 0
						zc = t * 0.33 * (1.0 - v) * (1.0 - v);
						if (u >= 1.0) {
							zc = 0.0;
						}
					}
					top[j] = MAX(top[j], h[i] + zc);
				}
			}
		}
		// The cones from neighbouring foot cells merge by maximum and leave ribs between them; real aprons are
		// smooth sheets (the scree shifts, Statham 1976). Two 3x3 passes over the scree only, never below the rock.
		LocalVector<double> sm;
		sm.resize(n);
		for (int pass = 0; pass < 2; pass++) {
			memcpy(sm.ptr(), top.ptr(), sizeof(double) * n);
			for (int z = 1; z < ny - 1; z++) {
				for (int x = 1; x < nx - 1; x++) {
					const int i = z * nx + x;
					if (top[i] <= h[i] + 0.01) {
						continue;
					}
					double sum = 0.0;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dx = -1; dx <= 1; dx++) {
							sum += top[i + dz * nx + dx];
						}
					}
					sm[i] = MAX(h[i], sum / 9.0);
				}
			}
			memcpy(top.ptr(), sm.ptr(), sizeof(double) * n);
		}
		scree_cover.resize(n);
		for (int i = 0; i < n; i++) {
			scree_cover[i] = float(MAX(top[i] - h[i], 0.0)); // for the ground material: scree, not soil
			h[i] = top[i];
		}
	};
	// Meanders: on the flat alluvial floors the main rivers bend and migrate for meander_age years (Howard and Knutson
	// 1984: bank migration from the local curvature plus an upstream-weighted sum of it, Omega = -1, Gamma = 2.5;
	// Sylvester et al. 2019; Paris et al. 2023 for terrain), cut off their tightest loops (oxbows) and are carved
	// into the ground along their last course; the oxbows stay as shallow abandoned channels.
	last_meander_count = 0;
	last_oxbow_count = 0;
	LocalVector<uint8_t> meander_cell; // the cut channels: the fan backfill leaves them open
	auto meander_pass = [&]() {
		channels.clear();
		chan_buckets.clear();
		channel_of_cell.clear();
		if (meander_age <= 0.0) {
			return;
		}
		const double c = double(cell_size);
		// Drainage as the solver routes it (D8 on its priority-flood filled surface), so the reaches found here are
		// the courses the rivers take and the new channel replaces the old one exactly.
		LocalVector<int32_t> rcv, order;
		LocalVector<double> fill, acc;
		rcv.resize(n);
		order.resize(n);
		fill.resize(n);
		{
			LocalVector<double> keep;
			keep.resize(n);
			memcpy(keep.ptr(), h.ptr(), sizeof(double) * n);
			sp.route();
			memcpy(fill.ptr(), h.ptr(), sizeof(double) * n);
			memcpy(h.ptr(), keep.ptr(), sizeof(double) * n);
			for (int i = 0; i < n; i++) {
				rcv[i] = sp.b.receiver[i] == i ? -1 : sp.b.receiver[i];
				order[i] = sp.b.stack[i];
			}
		}
		// Drainage area on that tree, with the land beyond the map that drains in at the inflows.
		acc.resize(n);
		for (int i = 0; i < n; i++) {
			acc[i] = c * c + (water_fine.is_empty() ? 0.0 : water_fine[i]);
		}
		for (int k = int(order.size()) - 1; k >= 0; k--) {
			const int i = order[k];
			if (rcv[i] >= 0) {
				acc[rcv[i]] += acc[i];
			}
		}
		auto hb = [&](double px, double pz) { // bilinear ground at grid-metre coordinates
			const double fx = CLAMP(px / c, 0.0, double(nx - 1)), fz = CLAMP(pz / c, 0.0, double(ny - 1));
			const int x0 = MIN(int(fx), nx - 2), z0 = MIN(int(fz), ny - 2);
			const double tx = fx - x0, tz = fz - z0;
			return (h[z0 * nx + x0] * (1 - tx) + h[z0 * nx + x0 + 1] * tx) * (1 - tz) + (h[(z0 + 1) * nx + x0] * (1 - tx) + h[(z0 + 1) * nx + x0 + 1] * tx) * tz;
		};
		const double min_area = 2.0e6; // m2: rivers, not brooks
		LocalVector<uint8_t> used;
		used.resize(n);
		memset(used.ptr(), 0, n);
		LocalVector<int32_t> up_count;
		up_count.resize(n);
		memset(up_count.ptr(), 0, sizeof(int32_t) * n);
		for (int i = 0; i < n; i++) {
			if (rcv[i] >= 0 && acc[i] >= min_area && acc[rcv[i]] >= min_area) {
				up_count[rcv[i]]++;
			}
		}
		uint32_t rng = uint32_t(seed) * 0x9E3779B1u + 0x51ED27u;
		auto rnd = [&]() {
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return double(rng) / 4294967296.0;
		};
		struct Curve {
			LocalVector<Vector2> pts;
			double w = 10.0, z0 = 0.0, z1 = 0.0;
			LocalVector<LocalVector<Vector2>> oxbows;
			LocalVector<double> oxbow_t; // year each was cut off
			LocalVector<int32_t> cells;
		};
		LocalVector<Curve> curves;
		for (int head = 0; head < n; head++) {
			if (acc[head] < min_area || up_count[head] != 0 || used[head] || double(river_width_coefficient) * Math::sqrt(acc[head] * 6.0e-8) < 0.5 * c) {
				continue;
			}
			// Down the river; runs over the flat alluvial floor (gradient under Leopold and Wolman's (1957) meandering
			// threshold S = 0.012 Q^-0.44, loose fill over half a metre, not standing water) of at least 12 cells become
			// meander reaches. Gradient and fill are means over 9 cells along the river, so one D8 step does not cut a reach.
			LocalVector<int32_t> path;
			LocalVector<double> dist;
			for (int i = head; i >= 0 && !used[i]; i = rcv[i]) {
				used[i] = 1;
				if (sp.b.outlet && sp.b.outlet[i]) {
					break;
				}
				if (!path.is_empty()) {
					const int p0 = path[path.size() - 1];
					const int dx = i % nx - p0 % nx, dz = i / nx - p0 / nx;
					dist.push_back(dist[dist.size() - 1] + (dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * c);
				} else {
					dist.push_back(0.0);
				}
				path.push_back(i);
			}
			const int pn = int(path.size());
			LocalVector<uint8_t> flats;
			flats.resize(pn);
			for (int k = 0; k < pn; k++) {
				const int k0 = MAX(k - 4, 0), k1 = MIN(k + 4, pn - 1);
				double sm = 0.0;
				for (int t = k0; t <= k1; t++) {
					sm += sed[path[t]];
				}
				const int i = path[k];
				const double qm = MAX(acc[i] * 6.0e-8, 0.01);
				const double S = k1 > k0 ? (h[path[k0]] - h[path[k1]]) / (dist[k1] - dist[k0]) : 1.0;
				flats[k] = S < 0.012 * Math::pow(qm, -0.44) && sm / (k1 - k0 + 1) > 0.1 && fill[i] - h[i] < 0.5 && h[i] > double(sea_level) + 1.5; // above the tidal reach
			}
			// A short steeper or thinner stretch (up to four cells) inside a floodplain does not end the reach.
			for (int k = 0, last = -1; k < pn; k++) {
				if (flats[k]) {
					if (last >= 0 && k - last - 1 <= 4) {
						for (int t = last + 1; t < k; t++) {
							flats[t] = 1;
						}
					}
					last = k;
				}
			}
			LocalVector<int32_t> run;
			for (int k = 0; k <= pn; k++) {
				const bool flat = k < pn && flats[k];
				if (flat) {
					run.push_back(path[k]);
				} else {
					if (run.size() >= 12) {
						Curve cv;
						cv.cells = run;
						const double a = acc[run[run.size() - 1]];
						cv.w = MAX(double(river_width_coefficient) * Math::sqrt(a * 6.0e-8), 4.0);
						for (uint32_t r = 0; r < run.size(); r++) {
							cv.pts.push_back(Vector2((run[r] % nx) * c, (run[r] / nx) * c));
						}
						curves.push_back(cv);
					}
					run.clear();
				}
			}
		}
		for (Curve &cv : curves) {
			const double w = cv.w;
			const double ds = MAX(0.25 * w, 2.0);
			auto resample = [&](LocalVector<Vector2> &p) {
				LocalVector<Vector2> o;
				o.push_back(p[0]);
				double carry = 0.0;
				for (uint32_t k = 1; k < p.size(); k++) {
					const Vector2 a = p[k - 1], b = p[k];
					const double L = a.distance_to(b);
					double t = ds - carry;
					while (t <= L) {
						o.push_back(a + (b - a) * real_t(t / L));
						t += ds;
					}
					carry = L - (t - ds);
				}
				if (o[o.size() - 1].distance_to(p[p.size() - 1]) > 0.25 * ds) {
					o.push_back(p[p.size() - 1]);
				} else {
					o[o.size() - 1] = p[p.size() - 1];
				}
				p = o;
			};
			// Smooth the D8 path, then resample at half a width.
			for (int pass = 0; pass < 4; pass++) {
				LocalVector<Vector2> q;
				q = cv.pts;
				for (uint32_t k = 1; k + 1 < cv.pts.size(); k++) {
					q[k] = (cv.pts[k - 1] + cv.pts[k] * 2.0f + cv.pts[k + 1]) * 0.25f;
				}
				cv.pts = q;
			}
			resample(cv.pts);
			cv.z0 = hb(cv.pts[0].x, cv.pts[0].y);
			cv.z1 = hb(cv.pts[cv.pts.size() - 1].x, cv.pts[cv.pts.size() - 1].y);
			const double floor_top = MAX(cv.z0, cv.z1) + 3.0; // banks above this are the valley's walls (bedrock)
			// A wander to start the bends: smooth noise along the river at about the meander wavelength (11 widths,
			// Leopold and Wolman 1960), a width high. Any start grows into the meander wavelength; this one gets there
			// sooner than a valley floor's straight run would.
			{
				double sacc = 0.0;
				LocalVector<Vector2> q;
				q = cv.pts;
				for (uint32_t k = 2; k + 2 < cv.pts.size(); k++) {
					sacc += cv.pts[k].distance_to(cv.pts[k - 1]);
					const Vector2 tg = (cv.pts[k + 1] - cv.pts[k - 1]).normalized();
					const double off = w * TerrainErosionFilter::fbm(uint32_t(seed) ^ 0x68E31DA4u, sacc, double(curves.size()) * 977.0, 11.0 * w, 2);
					q[k] += Vector2(-tg.y, tg.x) * real_t(off * MIN(1.0, MIN(double(k), double(cv.pts.size() - 1 - k)) / 6.0));
				}
				cv.pts = q;
				resample(cv.pts);
			}
			// m/yr of bank migration per unit dimensionless curvature: banks move about 2% of the width a year at R1 0.5,
			// the median of measured rates (Hooke 2003; Constantine et al. 2014). meanderpy's 0.3 w/yr is for big rivers in
			// fast-forward.
			const double kl = 0.04 * w;
			const double alpha = 2.0 * 0.02 / (w / 33.0); // 2 Cf / depth, meanderpy's Cf 0.02 and width/depth 33 (Sylvester et al. 2019)
			double t = 0.0;
			int guard = 0;
			while (t < meander_age && guard++ < 200000 && cv.pts.size() > 6) {
				const int m = int(cv.pts.size());
				LocalVector<double> r0, r1;
				LocalVector<Vector2> nrm;
				r0.resize(m);
				r1.resize(m);
				nrm.resize(m);
				for (int k = 0; k < m; k++) {
					const Vector2 a = cv.pts[MAX(k - 1, 0)], b = cv.pts[k], e = cv.pts[MIN(k + 1, m - 1)];
					const Vector2 tg = (e - a);
					const double L = MAX(double(tg.length()), 1e-6);
					nrm[k] = Vector2(-tg.y, tg.x) / real_t(L);
					// Signed curvature from the turning angle over the two segments.
					const Vector2 d1 = b - a, d2 = e - b;
					const double cr = double(d1.x) * d2.y - double(d1.y) * d2.x;
					const double l1 = MAX(double(d1.length()), 1e-6), l2 = MAX(double(d2.length()), 1e-6);
					const double ang = Math::atan2(cr, double(d1.dot(d2)));
					r0[k] = (k == 0 || k == m - 1) ? 0.0 : ang / (0.5 * (l1 + l2)) * w;
					(void)l1;
				}
				// Curvature smoothed over a few points (two 1-2-1 passes), so the resampling's point-scale kinks do not
				// grow into loops of their own.
				for (int pass = 0; pass < 2; pass++) {
					LocalVector<double> sm;
					sm = r0;
					for (int k = 1; k + 1 < m; k++) {
						sm[k] = 0.25 * (r0[k - 1] + 2.0 * r0[k] + r0[k + 1]);
					}
					r0 = sm;
				}
				for (int k = 0; k < m; k++) {
					double sum = 0.0, wsum = 0.0, xi = 0.0;
					for (int q = k; q >= 0 && xi < 6.0 / alpha; q--) {
						const double g = Math::exp(-alpha * xi);
						sum += r0[q] * g;
						wsum += g;
						if (q > 0) {
							xi += cv.pts[q].distance_to(cv.pts[q - 1]);
						}
					}
					r1[k] = -1.0 * r0[k] + 2.5 * (wsum > 0.0 ? sum / wsum : 0.0);
				}
				// The reach's ends hold (they meet the river's fixed course above and below): migration fades to nothing
				// over ten widths at each end, so the belt does not drift off and leave a straight run to a pinned end.
				LocalVector<double> hold;
				hold.resize(m);
				{
					double run = 0.0, len = 0.0;
					for (int k = 1; k < m; k++) {
						len += cv.pts[k].distance_to(cv.pts[k - 1]);
					}
					for (int k = 0; k < m; k++) {
						if (k > 0) {
							run += cv.pts[k].distance_to(cv.pts[k - 1]);
						}
						const double f = CLAMP(MIN(run, len - run) / (10.0 * w), 0.0, 1.0);
						hold[k] = f * f * (3.0 - 2.0 * f);
					}
				}
				for (int k = 0; k < m; k++) {
					r1[k] *= hold[k];
				}
				double vmax = 1e-9;
				for (int k = 2; k < m - 2; k++) {
					vmax = MAX(vmax, Math::abs(kl * r1[k]));
				}
				const double dt = MIN(meander_age - t, 0.25 * ds / vmax);
				for (int k = 2; k < m - 2; k++) {
					// Toward the outer bank: a bend turning left (positive curvature) moves right, against the left normal.
					// Banks of uneven strength (clay plugs, old fills: Motta et al. 2012) keep bending straight reaches.
					const double erod = 1.0 + 0.6 * TerrainErosionFilter::fbm(uint32_t(seed) ^ 0x2545F491u, cv.pts[k].x, cv.pts[k].y, 4.0 * w, 2);
					// ... and push it a little to one side or the other where they differ across the river, which seeds new
					// bends on a straight run (the upstream end feeds none: bends travel down the reach).
					// That pattern changes from flood to flood, faded between epochs of 300 years.
					const double ep = t / 300.0;
					const uint32_t e0 = uint32_t(ep);
					const double ef = ep - double(e0);
					const double n0 = TerrainErosionFilter::fbm((uint32_t(seed) ^ 0x1B873593u) + e0 * 0x9E3779B1u, cv.pts[k].x, cv.pts[k].y, 8.0 * w, 2);
					const double n1 = TerrainErosionFilter::fbm((uint32_t(seed) ^ 0x1B873593u) + (e0 + 1) * 0x9E3779B1u, cv.pts[k].x, cv.pts[k].y, 8.0 * w, 2);
					const double side = 0.3 * hold[k] * (n0 + (n1 - n0) * ef);
					const Vector2 np = cv.pts[k] - nrm[k] * real_t(kl * (erod * r1[k] + side) * dt);
					if (np.x < c || np.y < c || np.x > (nx - 2) * c || np.y > (ny - 2) * c) {
						continue;
					}
					if (hb(np.x, np.y) > floor_top) {
						continue; // the bank is the valley wall: rock, it does not move
					}
					cv.pts[k] = np;
				}
				t += dt;
				resample(cv.pts);
				// Cutoffs: a loop whose neck closes to under a width is cut through; the loop is left as an oxbow.
				// Chute cutoffs too (Howard 1996): within three widths, a course over five times the straight way across
				// is cut through the point bar in a flood.
				const uint32_t gap = uint32_t(MAX(8.0, Math::ceil(4.0 * w / ds))); // a loop is at least four widths round
				for (uint32_t a = 0; a < cv.pts.size() && (guard % 8) == 0; a++) {
					for (uint32_t b = a + gap; b < cv.pts.size(); b++) {
						const double dab = cv.pts[a].distance_to(cv.pts[b]);
						if (dab < MAX(0.9 * w, ds) || (dab < 3.0 * w && double(b - a) * ds > 5.0 * dab)) {
							LocalVector<Vector2> ox;
							for (uint32_t q = a; q <= b; q++) {
								ox.push_back(cv.pts[q]);
							}
							cv.oxbows.push_back(std::move(ox));
							cv.oxbow_t.push_back(t);
							LocalVector<Vector2> keep;
							for (uint32_t q = 0; q <= a; q++) {
								keep.push_back(cv.pts[q]);
							}
							for (uint32_t q = b; q < cv.pts.size(); q++) {
								keep.push_back(cv.pts[q]);
							}
							cv.pts = keep;
							// The new straight cut is not straight for long: the flood leaves it rough.
							for (uint32_t q = a > 2 ? a - 2 : 2; q < MIN(a + 3, cv.pts.size() - 2); q++) {
								const Vector2 tq = (cv.pts[q + 1] - cv.pts[q - 1]).normalized();
								cv.pts[q] += Vector2(-tq.y, tq.x) * real_t((rnd() - 0.5) * 0.3 * w);
							}
							b = cv.pts.size();
						}
					}
				}
			}
			// No bend tighter than 1.5 widths in radius (rivers' bends rarely are: Leopold and Wolman 1960, Hickin
			// 1974): the discrete model can kink tighter, and the water's ribbon would fold over itself there.
			for (int pass = 0; pass < 30; pass++) {
				bool kinked = false;
				LocalVector<Vector2> q;
				q = cv.pts;
				for (uint32_t k = 1; k + 1 < cv.pts.size(); k++) {
					const Vector2 d1 = cv.pts[k] - cv.pts[k - 1], d2 = cv.pts[k + 1] - cv.pts[k];
					const double ang = Math::abs(Math::atan2(double(d1.x) * d2.y - double(d1.y) * d2.x, double(d1.dot(d2))));
					const double L = 0.5 * double(d1.length() + d2.length());
					if (ang > L / (1.5 * w)) {
						q[k] = (cv.pts[k - 1] + cv.pts[k + 1]) * 0.5f;
						kinked = true;
					}
				}
				cv.pts = q;
				if (!kinked) {
					break;
				}
				resample(cv.pts);
			}
			last_meander_count++;
		}
		// Carve: the floodplain is levelled back over the old straight course, then the channel is cut along the new
		// one (depth a twelfth of the width, at least a metre, parabolic across), its bed falling evenly from the
		// reach's top to its end; oxbows at half that depth.
		meander_cell.resize(n);
		memset(meander_cell.ptr(), 0, n);
		channel_of_cell.resize(n);
		for (int i = 0; i < n; i++) {
			channel_of_cell[i] = -1;
		}
		const Vector2 world0 = result_rect.position;
		for (Curve &cv : curves) {
			const double w = cv.w;
			const double depth = MAX(w / 12.0, 1.0);
			double total = 0.0;
			LocalVector<double> sacc;
			sacc.push_back(0.0);
			for (uint32_t k = 1; k < cv.pts.size(); k++) {
				total += cv.pts[k].distance_to(cv.pts[k - 1]);
				sacc.push_back(total);
			}
			// Fill the old course (and the cells beside it) to just over the floodplain's even fall, so the water takes
			// the new channel wherever the two cross.
			for (uint32_t k = 0; k < cv.cells.size(); k++) {
				const double plain = cv.z0 + (cv.z1 - cv.z0) * double(k) / double(MAX(int(cv.cells.size()) - 1, 1));
				const int cx = cv.cells[k] % nx, cz = cv.cells[k] / nx;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = cx + dx, zz = cz + dz;
						if (xx < 0 || zz < 0 || xx >= nx || zz >= ny || (sp.b.outlet && sp.b.outlet[zz * nx + xx])) {
							continue;
						}
						double &hh = h[zz * nx + xx];
						const double up = MAX(plain + 0.3 - hh, 0.0);
						hh += up;
						sed[zz * nx + xx] += up;
					}
				}
			}
			// The cut eases in over three widths at the reach's head. At its foot the bed may not rise (the water would
			// pond behind the step): the river's course below is cut down to it instead (the tail, once the ground is final).
			auto ramp = [&](double p_s) { return CLAMP(p_s / (3.0 * w), 0.0, 1.0); };
			// The same channel at full resolution, for the ground and the rivers' points.
			auto add_channel = [&](const LocalVector<Vector2> &pts, const LocalVector<double> *sp_acc, double dep, double p_bed) {
				Channel ch;
				ch.reach = sp_acc != nullptr;
				ch.half_width = real_t(0.5 * w);
				ch.depth = real_t(dep);
				for (uint32_t k = 0; k < pts.size(); k++) {
					ch.points.push_back(world0 + pts[k]);
					const double plain = sp_acc ? cv.z0 + (cv.z1 - cv.z0) * (*sp_acc)[k] / MAX(total, 1.0) : p_bed + dep;
					const double d = sp_acc ? dep * ramp((*sp_acc)[k]) : dep;
					ch.bed.push_back(real_t(plain - d));
					ch.dep.push_back(real_t(d));
				}
				channels.push_back(std::move(ch));
				return int(channels.size()) - 1;
			};
			auto carve = [&](const LocalVector<Vector2> &pts, const LocalVector<double> *sp_acc, double dep, int p_channel) {
				const double half = 0.5 * w + 0.5 * c;
				for (uint32_t k = 0; k + 1 < pts.size(); k++) {
					const Vector2 a = pts[k], b = pts[k + 1];
					const int x0 = MAX(int((MIN(a.x, b.x) - half) / c), 0), x1 = MIN(int((MAX(a.x, b.x) + half) / c) + 1, nx - 1);
					const int z0 = MAX(int((MIN(a.y, b.y) - half) / c), 0), z1 = MIN(int((MAX(a.y, b.y) + half) / c) + 1, ny - 1);
					const Vector2 ab = b - a;
					const double l2 = MAX(double(ab.length_squared()), 1e-9);
					for (int z = z0; z <= z1; z++) {
						for (int x = x0; x <= x1; x++) {
							const Vector2 p(x * c, z * c);
							const double tt = CLAMP(double((p - a).dot(ab)) / l2, 0.0, 1.0);
							const double d = p.distance_to(a + ab * real_t(tt));
							if (d > half) {
								continue;
							}
							const double sfrac = sp_acc ? ((*sp_acc)[k] + tt * Math::sqrt(l2)) / MAX(total, 1.0) : 0.5;
							const double plain = cv.z0 + (cv.z1 - cv.z0) * sfrac;
							const double u = d / half;
							const double bed = plain - (sp_acc ? dep * ramp(sfrac * total) : dep) * (1.0 - u * u);
							const int i = z * nx + x;
							if (!(sp.b.outlet && sp.b.outlet[i])) {
								h[i] = MIN(h[i], bed);
								meander_cell[i] = 1;
								if (p_channel >= 0 && d <= 0.5 * w + 0.25 * c) {
									channel_of_cell[i] = p_channel;
								}
							}
						}
					}
				}
			};
			// Oxbows silt up after the cut (floods fill them from the river end; most are dry hollows within a few
			// centuries, Constantine et al. 2010): half the channel's depth when cut, an e-fold every 400 years, gone
			// under 0.15 m.
			for (uint32_t q = 0; q < cv.oxbows.size(); q++) {
				const LocalVector<Vector2> &ox = cv.oxbows[q];
				const double dep = 0.5 * depth * Math::exp(-(meander_age - cv.oxbow_t[q]) / 400.0);
				if (dep < 0.15) {
					continue;
				}
				const double ends = 0.5 * (hb(ox[0].x, ox[0].y) + hb(ox[ox.size() - 1].x, ox[ox.size() - 1].y));
				add_channel(ox, nullptr, dep, ends - dep);
				carve(ox, nullptr, dep, -1);
				last_oxbow_count++;
			}
			{
				const int ci = add_channel(cv.pts, &sacc, depth, 0.0);
				channels[ci].foot_cell = cv.cells[cv.cells.size() - 1];
				carve(cv.pts, &sacc, depth, ci);
			}
		}
		_bucket_channels();
	};
	// Braided outwash: below the last glaciers the meltwater's gravel spreads over a wide flat floor and splits round
	// shifting bars (a sandur or valley train). Reaches above Leopold and Wolman's (1957) braiding threshold with ice in
	// their catchment get a braidplain about six widths across, graded flat across, and Murray and Paola's (1994)
	// cellular braided river runs over it for outwash_age years on a grid straightened along the reach: water shares
	// out to the three cells below by slope^0.5, carries gravel as (Q S)^2.5 and lets the banks fall in sideways.
	// The relief it leaves (channels and bars, scaled to the river's depth) is laid on the full-resolution ground; the
	// water takes the strongest thread.
	auto outwash_pass = [&]() {
		last_braid_count = 0;
		if (outwash_age <= 0.0 || glacial_ice.size() != uint32_t(n)) {
			return;
		}
		const double c = double(cell_size);
		const Vector2 world0 = result_rect.position;
		LocalVector<int32_t> rcv, order;
		LocalVector<double> fill, acc, ice;
		rcv.resize(n);
		order.resize(n);
		fill.resize(n);
		{
			LocalVector<double> keep;
			keep.resize(n);
			memcpy(keep.ptr(), h.ptr(), sizeof(double) * n);
			sp.route();
			memcpy(fill.ptr(), h.ptr(), sizeof(double) * n);
			memcpy(h.ptr(), keep.ptr(), sizeof(double) * n);
			for (int i = 0; i < n; i++) {
				rcv[i] = sp.b.receiver[i] == i ? -1 : sp.b.receiver[i];
				order[i] = sp.b.stack[i];
			}
		}
		acc.resize(n);
		ice.resize(n);
		for (int i = 0; i < n; i++) {
			acc[i] = c * c + (water_fine.is_empty() ? 0.0 : water_fine[i]);
			ice[i] = glacial_ice[i] > 2.0f ? acc[i] : 0.0; // a glacier coming in from beyond the map brings its meltwater too
		}
		for (int k = int(order.size()) - 1; k >= 0; k--) {
			const int i = order[k];
			if (rcv[i] >= 0) {
				acc[rcv[i]] += acc[i];
				// The braidplain dates from the ice's last stand, when the glacier fed it directly: lakes in the basins it
				// left behind came after.
				ice[rcv[i]] += ice[i];
			}
		}
		if (meander_cell.size() != uint32_t(n)) {
			meander_cell.resize(n);
			memset(meander_cell.ptr(), 0, n);
		}
		if (channel_of_cell.size() != uint32_t(n)) {
			channel_of_cell.resize(n);
			for (int i = 0; i < n; i++) {
				channel_of_cell[i] = -1;
			}
		}
		const double min_area = 2.0e6;
		LocalVector<uint8_t> used;
		used.resize(n);
		memset(used.ptr(), 0, n);
		LocalVector<int32_t> up_count;
		up_count.resize(n);
		memset(up_count.ptr(), 0, sizeof(int32_t) * n);
		for (int i = 0; i < n; i++) {
			if (rcv[i] >= 0 && acc[i] >= min_area && acc[rcv[i]] >= min_area) {
				up_count[rcv[i]]++;
			}
		}
		uint32_t rng = uint32_t(seed) * 0x2545F491u + 0x7F4A7C15u;
		auto rnd = [&]() {
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return double(rng) / 4294967296.0;
		};
		struct Reach {
			LocalVector<int32_t> cells;
			double w = 10.0;
		};
		LocalVector<Reach> reaches;
		for (int head = 0; head < n; head++) {
			if (acc[head] < min_area || up_count[head] != 0 || used[head]) {
				continue;
			}
			LocalVector<int32_t> path;
			LocalVector<double> dist;
			for (int i = head; i >= 0 && !used[i]; i = rcv[i]) {
				used[i] = 1;
				if (sp.b.outlet && sp.b.outlet[i]) {
					break;
				}
				if (!path.is_empty()) {
					const int p0 = path[path.size() - 1];
					const int dx = i % nx - p0 % nx, dz = i / nx - p0 / nx;
					dist.push_back(dist[dist.size() - 1] + (dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * c);
				} else {
					dist.push_back(0.0);
				}
				path.push_back(i);
			}
			const int pn = int(path.size());
			LocalVector<uint8_t> flags;
			flags.resize(pn);
			for (int k = 0; k < pn; k++) {
				const int k0 = MAX(k - 2, 0), k1 = MIN(k + 2, pn - 1);
				const int i = path[k];
				const double qm = MAX(acc[i] * 6.0e-8, 0.01);
				const double S = k1 > k0 ? (h[path[k0]] - h[path[k1]]) / (dist[k1] - dist[k0]) : 0.0;
				// Above the braiding line but not a torrent, fed by ice, out of the ice, dry, above the sea, not meandering.
				double sm = 0.0;
				for (int t = k0; t <= k1; t++) {
					sm += sed[path[t]];
				}
				// Gravel enough to braid: meltwater from ice upstream, or the thick fill of a fan (fans' streams braid,
				// Blair and McPherson 1994).
				const bool gravel = ice[i] >= 0.05 * acc[i] || sm / (k1 - k0 + 1) > 1.0;
				// Meltwater rivers braid at their summer floods, several times the mean flow: the threshold on that (bankfull)
				// discharge, taken as five times the mean for an ice-fed river.
				const double qb = ice[i] >= 0.05 * acc[i] ? 5.0 * qm : qm;
				flags[k] = acc[i] >= min_area && S >= 0.012 * Math::pow(qb, -0.44) && S < 0.05 && gravel && glacial_ice[i] < 1.0f && fill[i] - h[i] < 0.5 && h[i] > double(sea_level) + 1.5 && !meander_cell[i];
			}
			for (int k = 0, last = -1; k < pn; k++) {
				if (flags[k]) {
					if (last >= 0 && k - last - 1 <= 5) {
						for (int t = last + 1; t < k; t++) {
							flags[t] = 1;
						}
					}
					last = k;
				}
			}
			LocalVector<int32_t> run;
			for (int k = 0; k <= pn; k++) {
				if (k < pn && flags[k] && !meander_cell[path[k]]) {
					run.push_back(path[k]);
				} else {
					if (run.size() >= 6) {
						Reach r;
						r.cells = run;
						r.w = MAX(double(river_width_coefficient) * Math::sqrt(acc[run[run.size() - 1]] * 6.0e-8), 4.0);
						reaches.push_back(r);
					}
					run.clear();
				}
			}
		}
		for (Reach &rc : reaches) {
			const double w = rc.w;
			const double half = CLAMP(6.0 * w, 3.0 * c, 400.0);
			const double ds = CLAMP(0.25 * w, 2.0, 8.0);
			const double depth = CLAMP(0.06 * w, 0.5, 2.0);
			// Centre line: the D8 path smoothed, resampled at ds; the floor's level along it smoothed and never rising.
			LocalVector<Vector2> cl;
			for (int32_t i : rc.cells) {
				cl.push_back(Vector2((i % nx) * c, (i / nx) * c));
			}
			for (int pass = 0; pass < 6; pass++) {
				LocalVector<Vector2> q;
				q = cl;
				for (uint32_t k = 1; k + 1 < cl.size(); k++) {
					q[k] = (cl[k - 1] + cl[k] * 2.0f + cl[k + 1]) * 0.25f;
				}
				cl = q;
			}
			LocalVector<double> lvl;
			const int cn = int(rc.cells.size());
			lvl.resize(cn);
			for (int k = 0; k < cn; k++) {
				double sm = 0.0;
				int cnt = 0;
				for (int t = MAX(k - 3, 0); t <= MIN(k + 3, cn - 1); t++) {
					sm += h[rc.cells[t]];
					cnt++;
				}
				lvl[k] = sm / cnt;
				if (k > 0) {
					lvl[k] = MIN(lvl[k], lvl[k - 1] - 1e-4 * c);
				}
			}
			LocalVector<Vector2> line;
			LocalVector<double> lz, sacc;
			line.push_back(cl[0]);
			lz.push_back(lvl[0]);
			sacc.push_back(0.0);
			{
				double carry = 0.0;
				for (int k = 1; k < cn; k++) {
					const Vector2 a = cl[k - 1], b = cl[k];
					const double L = a.distance_to(b);
					double t = ds - carry;
					while (t <= L) {
						line.push_back(a + (b - a) * real_t(t / L));
						lz.push_back(Math::lerp(lvl[k - 1], lvl[k], t / MAX(L, 1e-9)));
						sacc.push_back(sacc[sacc.size() - 1] + ds);
						t += ds;
					}
					carry = L - (t - ds);
				}
			}
			const int R = int(line.size());
			if (R < 8) {
				continue;
			}
			LocalVector<Vector2> nrm;
			nrm.resize(R);
			for (int k = 0; k < R; k++) {
				const Vector2 d = (line[MIN(k + 1, R - 1)] - line[MAX(k - 1, 0)]).normalized();
				nrm[k] = Vector2(-d.y, d.x);
			}
			const double S = MAX((lz[0] - lz[R - 1]) / MAX(sacc[R - 1], 1.0), 1e-3);
			// Grade the floor on the coarse grid: flat across, at the centre line's level, lowering only; the floor is
			// bare gravel (no soil, read as loose stones by the ground) and the fan backfill leaves it open.
			for (int k = 0; k + 1 < R; k++) {
				const Vector2 a = line[k], b = line[k + 1];
				const double hh = half + 0.5 * c;
				const int x0 = MAX(int((MIN(a.x, b.x) - hh) / c), 0), x1 = MIN(int((MAX(a.x, b.x) + hh) / c) + 1, nx - 1);
				const int z0 = MAX(int((MIN(a.y, b.y) - hh) / c), 0), z1 = MIN(int((MAX(a.y, b.y) + hh) / c) + 1, ny - 1);
				const Vector2 ab = b - a;
				const double l2 = MAX(double(ab.length_squared()), 1e-9);
				for (int z = z0; z <= z1; z++) {
					for (int x = x0; x <= x1; x++) {
						const Vector2 p(x * c, z * c);
						const double tt = CLAMP(double((p - a).dot(ab)) / l2, 0.0, 1.0);
						const double d = p.distance_to(a + ab * real_t(tt));
						if (d > hh) {
							continue;
						}
						const int i = z * nx + x;
						if (sp.b.outlet && sp.b.outlet[i]) {
							continue;
						}
						const double level = Math::lerp(lz[k], lz[k + 1], tt);
						// The floor's edge eases up into the valley side over the last cell.
						const double edge = CLAMP((d - half) / (0.5 * c), 0.0, 1.0);
						const double target = level + edge * MAX(h[i] - level, 0.0);
						if (h[i] > target) {
							sed[i] = MAX(sed[i] - (h[i] - target), 0.0);
							h[i] = target;
						}
						meander_cell[i] = 1;
						if (d <= half) {
							if (scree_cover.size() != uint32_t(n)) {
								scree_cover.resize(n);
								memset(scree_cover.ptr(), 0, sizeof(float) * n);
							}
							scree_cover[i] = 1.0f;
						}
					}
				}
			}
			// Murray and Paola's cellular braided river on the straightened floor: rows down the reach, columns across.
			const int C = MAX(int(2.0 * half / ds) + 1, 5);
			const double dn = 2.0 * half / double(C - 1);
			LocalVector<double> z, Q, Qn, qsin, qsout;
			z.resize(R * C);
			Q.resize(R * C);
			Qn.resize(R * C);
			qsin.resize(R * C);
			qsout.resize(R * C);
			for (int r = 0; r < R; r++) {
				for (int cc = 0; cc < C; cc++) {
					z[r * C + cc] = -S * r * ds + (rnd() - 0.5) * 0.02 * depth;
				}
			}
			const double dg = Math::sqrt(ds * ds + dn * dn);
			const double kl = 0.3; // bank collapse into a channel, per unit lateral slope (MP's lateral transport)
			const int iters = CLAMP(int(outwash_age / 5.0), 60, 900);
			for (int it = 0; it < iters; it++) {
				memset(Q.ptr(), 0, sizeof(double) * R * C);
				memset(qsin.ptr(), 0, sizeof(double) * R * C);
				memset(qsout.ptr(), 0, sizeof(double) * R * C);
				// The water comes in over the middle half of the top row, a little unevenly.
				for (int cc = C / 4; cc <= 3 * C / 4; cc++) {
					Q[cc] = 0.5 + rnd();
				}
				for (int r = 0; r < R; r++) {
					for (int cc = 0; cc < C; cc++) {
						const int i = r * C + cc;
						const double q = Q[i];
						if (q <= 0.0) {
							continue;
						}
						double s[3], wgt[3];
						int nb[3];
						double sum = 0.0;
						bool any_down = false;
						for (int t = 0; t < 3; t++) {
							const int k = cc + t - 1;
							nb[t] = (k >= 0 && k < C) ? k : -1;
							if (nb[t] < 0) {
								s[t] = 0.0;
								continue;
							}
							const double below = r + 1 < R ? z[(r + 1) * C + k] : z[r * C + k] - S * ds;
							s[t] = (z[i] - below) / (t == 1 ? ds : dg);
							any_down = any_down || s[t] > 0.0;
						}
						for (int t = 0; t < 3; t++) {
							wgt[t] = nb[t] < 0 ? 0.0 : (any_down ? (s[t] > 0.0 ? Math::sqrt(s[t]) : 0.0) : 1.0 / Math::sqrt(MAX(-s[t], 1e-6)));
							sum += wgt[t];
						}
						if (sum <= 0.0) {
							continue;
						}
						for (int t = 0; t < 3; t++) {
							if (wgt[t] <= 0.0) {
								continue;
							}
							const double qk = q * wgt[t] / sum;
							const double qs = Math::pow(MAX(qk * s[t], 0.0), 2.5);
							qsout[i] += qs;
							if (r + 1 < R) {
								Q[(r + 1) * C + nb[t]] += qk;
								qsin[(r + 1) * C + nb[t]] += qs;
							}
						}
					}
				}
				// The top row is fed what it passes on (the river brings as much gravel as it can carry there).
				for (int cc = 0; cc < C; cc++) {
					qsin[cc] = qsout[cc];
				}
				double mx = 0.0;
				for (int i = 0; i < R * C; i++) {
					mx = MAX(mx, Math::abs(qsin[i] - qsout[i]));
				}
				if (mx <= 0.0) {
					break;
				}
				const double dt = 0.02 * S * ds / mx; // no cell moves more than a fiftieth of a row's drop a step
				for (int i = 0; i < R * C; i++) {
					z[i] += (qsin[i] - qsout[i]) * dt;
				}
				// Banks fall in sideways where the water runs below them.
				for (int r = 1; r < R; r++) {
					for (int cc = 0; cc + 1 < C; cc++) {
						const int a = r * C + cc, b = a + 1;
						const double f = kl * (z[a] - z[b]) / dn * (Q[a] + Q[b] > 0.0 ? 1.0 : 0.0) * 0.02 * S * ds;
						z[a] -= f;
						z[b] += f;
					}
				}
				memcpy(Qn.ptr(), Q.ptr(), sizeof(double) * R * C);
			}
			// The relief against the plane: each row's mean taken out, scaled so the deepest channels are the river's
			// depth, bars standing up to 0.4 of it; fading out over the floor's outer fifth.
			LocalVector<float> rel;
			rel.resize(R * C);
			LocalVector<double> devs;
			for (int r = 0; r < R; r++) {
				double m = 0.0;
				for (int cc = 0; cc < C; cc++) {
					m += z[r * C + cc];
				}
				m /= C;
				for (int cc = 0; cc < C; cc++) {
					rel[r * C + cc] = float(z[r * C + cc] - m);
					devs.push_back(z[r * C + cc] - m);
				}
			}
			devs.sort();
			const double low = -devs[uint32_t(0.03 * devs.size())];
			const double sc = low > 1e-9 ? depth / low : 0.0;
			for (int r = 0; r < R; r++) {
				const double ends = MIN(MIN(r * ds, (R - 1 - r) * ds) / (2.0 * w), 1.0); // eased in and out at the ends
				for (int cc = 0; cc < C; cc++) {
					const double u = Math::abs(-half + cc * dn) / half;
					const double fade = 1.0 - CLAMP((u - 0.8) / 0.2, 0.0, 1.0);
					rel[r * C + cc] = float(CLAMP(double(rel[r * C + cc]) * sc, -depth, 0.4 * depth) * fade * ends);
				}
			}
			// The mean-flow water surface: per row, the level that wets about 30% of the plain (Ashmore 1991: three to six
			// active threads across a braidplain), threads up to the river's depth under it, bars standing above it.
			LocalVector<float> stage_rel;
			stage_rel.resize(R);
			{
				LocalVector<float> rowv;
				rowv.resize(C);
				for (int r = 0; r < R; r++) {
					for (int cc = 0; cc < C; cc++) {
						rowv[cc] = rel[r * C + cc];
					}
					rowv.sort();
					stage_rel[r] = rowv[uint32_t(0.3 * (C - 1))];
				}
			}
			Channel bp;
			bp.stage_rel = stage_rel;
			bp.head_cell = rc.cells[MIN(2u, rc.cells.size() - 1)]; // a cell into the reach (the first may be a lake's)
			bp.half_width = real_t(0.5 * half); // the buckets reach twice this: the whole floor
			bp.braid_half = real_t(half);
			bp.braid_cols = C;
			bp.braid = rel;
			for (int k = 0; k < R; k++) {
				bp.points.push_back(world0 + line[k]);
				bp.bed.push_back(0.0f);
				bp.dep.push_back(0.0f);
			}
			channels.push_back(std::move(bp));
			// The water's thread: from the strongest inflow down the strongest flow of the last step.
			int col = C / 2;
			{
				double best = -1.0;
				for (int cc = 0; cc < C; cc++) {
					if (Qn[cc] > best) {
						best = Qn[cc];
						col = cc;
					}
				}
			}
			Channel th;
			th.reach = true;
			th.braid_thread = true;
			th.half_width = real_t(MAX(0.5 * w, 2.0));
			th.depth = real_t(0.5 * depth);
			for (int r = 0; r < R; r++) {
				if (r > 0) {
					int nb = col;
					double best = -1.0;
					for (int t = -1; t <= 1; t++) {
						const int k = col + t;
						if (k >= 0 && k < C && Qn[r * C + k] > best) {
							best = Qn[r * C + k];
							nb = k;
						}
					}
					col = nb;
				}
				const double off = -half + col * dn;
				th.points.push_back(world0 + line[r] + nrm[r] * real_t(off));
				th.bed.push_back(rel[r * C + col]); // relative to the floor until the ground is final
				th.dep.push_back(real_t(0.5 * depth));
			}
			for (int pass = 0; pass < 2; pass++) {
				LocalVector<Vector2> q;
				q = th.points;
				for (uint32_t k = 1; k + 1 < th.points.size(); k++) {
					q[k] = (th.points[k - 1] + th.points[k] * 2.0f + th.points[k + 1]) * 0.25f;
				}
				th.points = q;
			}
			th.points[0] = world0 + line[0];
			th.foot_cell = rc.cells[rc.cells.size() - 1];
			channels.push_back(std::move(th));
			const int ti = int(channels.size()) - 1;
			for (int32_t i : rc.cells) {
				channel_of_cell[i] = ti;
			}
			last_braid_count++;
		}
		_bucket_channels();
	};
	if (glacial && glacial_final > 0.0) {
		const uint64_t tg = OS::get_singleton()->get_ticks_usec();
		glac.erode(h.ptr(), nx, ny, cell_size, double(glacial_final), sed.ptr(), sp.b.outlet, sill_fine.is_empty() ? nullptr : sill_fine.ptr(), level_fine.is_empty() ? nullptr : level_fine.ptr(), glacial_scale(nx, ny, cell_size));
		glacial_usec += OS::get_singleton()->get_ticks_usec() - tg;
		// Walls the ice left steeper than rock stands for long (the trough's side, the step below a riegel) shed
		// rockfall until they are no steeper than strata_cliff_angle; the debris piles at their feet. One
		// sweep from the highest cell down settles every pair, since each cell's higher neighbours are final first.
		{
			const double S = Math::tan(Math::deg_to_rad(double(strata_cliff_angle)));
			LocalVector<int32_t> ord;
			ord.resize(n);
			for (int i = 0; i < n; i++) {
				ord[i] = i;
			}
			const double *hp = h.ptr();
			std::sort(ord.ptr(), ord.ptr() + n, [hp](int32_t a, int32_t b) { return hp[a] > hp[b]; });
			const uint8_t *fixed = sp.b.outlet;
			for (int k = 0; k < n; k++) {
				const int32_t i = ord[k];
				if (fixed && fixed[i]) {
					continue;
				}
				const int x = i % nx, z = i / nx;
				double need = h[i];
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, zz = z + dz;
						if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						const double d = (dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * double(cell_size);
						need = MAX(need, h[zz * nx + xx] - S * d);
					}
				}
				if (need > h[i]) {
					h[i] = need; // blocky rockfall debris, not alluvium: it keeps its detail
				}
			}
		}
		// Moraines: rock falling from the walls onto the ice rides it down to the margin and is dropped there; a
		// margin that stands still for moraine_age years builds a ridge (lateral along the sides, a terminal loop
		// at the snout) that can dam a lake (Benn and Evans 2010, ch. 11; Rowan, Egholm and Clark 2015).
		last_moraine_volume = 0.0;
		if (moraine_age > 0.0) {
			const double c = double(cell_size);
			const double wall = Math::tan(Math::deg_to_rad(40.0));
			const double repose = Math::tan(Math::deg_to_rad(34.0)); // fresh till's flanks
			const double retreat = 0.001; // m/yr of headwall retreat (Hallet, Hunter and Bogen 1996: 0.1 to several mm/yr)
			LocalVector<double> ice, s_top, debris, dep;
			LocalVector<uint8_t> carried;
			ice.resize(n);
			s_top.resize(n);
			debris.resize(n);
			dep.resize(n);
			carried.resize(n);
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					ice[i] = glac.ice_at(x, z, cell_size);
					s_top[i] = h[i] + ice[i];
					debris[i] = 0.0;
					dep[i] = 0.0;
					carried[i] = 0;
				}
			}
			// The ice grid is 64 m: smooth its thickness on the fine grid (two box passes of 3 cells each way) so the
			// margin follows a curve, not the ice grid's steps; the glacier's edge is where it is 10 m thick (thinner
			// ice is the smooth solve's skirt over the slopes, not a glacier).
			{
				LocalVector<double> tmpb;
				tmpb.resize(n);
				for (int pass = 0; pass < 2; pass++) {
					for (int z = 0; z < ny; z++) {
						for (int x = 0; x < nx; x++) {
							double sum = 0.0;
							int w = 0;
							for (int d = -3; d <= 3; d++) {
								const int xx = CLAMP(x + d, 0, nx - 1);
								sum += ice[z * nx + xx];
								w++;
							}
							tmpb[z * nx + x] = sum / w;
						}
					}
					for (int z = 0; z < ny; z++) {
						for (int x = 0; x < nx; x++) {
							double sum = 0.0;
							int w = 0;
							for (int d = -3; d <= 3; d++) {
								const int zz = CLAMP(z + d, 0, ny - 1);
								sum += tmpb[zz * nx + x];
								w++;
							}
							ice[z * nx + x] = sum / w;
						}
					}
				}
				for (int i = 0; i < n; i++) {
					ice[i] = MAX(0.0, ice[i] - 9.0); // the glacier's edge at 10 m
					s_top[i] = h[i] + ice[i];
				}
			}
			auto slope_at = [&](int x, int z) {
				const int x0 = MAX(x - 1, 0), x1 = MIN(x + 1, nx - 1), z0 = MAX(z - 1, 0), z1 = MIN(z + 1, ny - 1);
				const double gx = (h[z * nx + x1] - h[z * nx + x0]) / ((x1 - x0) * c);
				const double gz = (h[z1 * nx + x] - h[z0 * nx + x]) / ((z1 - z0) * c);
				return Math::sqrt(gx * gx + gz * gz);
			};
			// Supply: bare walls steeper than 40 degrees retreat over the stand; their rock goes down the wall.
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					if (ice[i] <= 1.0 && slope_at(x, z) > wall) {
						debris[i] = retreat * moraine_age * c * c * slope_at(x, z);
					}
				}
			}
			// Basal debris: what the ice quarried over the stand, its share of the last glaciation's wear. About half
			// leaves in the meltwater (outwash); the rest is carried to the margin as till (Hallet et al. 1996).
			{
				const std::vector<double> &worn = glac.get_total();
				if (worn.size() == size_t(n) && glacial_final > 0.0) {
					const double share = 0.5 * MIN(1.0, moraine_age / double(glacial_final));
					for (int i = 0; i < n; i++) {
						if (ice[i] > 1.0) {
							debris[i] += worn[i] * share * c * c;
						}
					}
				}
			}
			// Down the surface (bed + ice) from the top: on bare ground the rock only keeps going while the wall is
			// steep (otherwise it is talus, the rockfall above handles it); on the ice it rides to the first bare
			// cell, where it is dropped.
			LocalVector<int32_t> ord;
			ord.resize(n);
			for (int i = 0; i < n; i++) {
				ord[i] = i;
			}
			const double *sp_top = s_top.ptr();
			std::sort(ord.ptr(), ord.ptr() + n, [sp_top](int32_t a, int32_t b) { return sp_top[a] > sp_top[b]; });
			for (int k = 0; k < n; k++) {
				const int32_t i = ord[k];
				if (debris[i] <= 0.0) {
					continue;
				}
				const bool on_ice = ice[i] > 1.0;
				if (!on_ice && carried[i]) {
					dep[i] += debris[i]; // the margin
					continue;
				}
				const int x = i % nx, z = i / nx;
				if (!on_ice && slope_at(x, z) <= wall) {
					continue; // talus below a wall that never reached the ice
				}
				// Multiple flow directions (Freeman 1991; Quinn et al. 1991): the load splits over every lower
				// neighbour by slope^1.1, so it spreads across the ice as flow lines diverge toward the margins
				// instead of gathering on one line (single-direction routing dropped it all at a few points).
				int nbs[8];
				double wts[8];
				int cnt = 0;
				double wsum = 0.0;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, zz = z + dz;
						if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						const int j = zz * nx + xx;
						const double L = (dx != 0 && dz != 0 ? Math::SQRT2 : 1.0) * c;
						const double g = (s_top[i] - s_top[j]) / L;
						if (g > 0.0) {
							nbs[cnt] = j;
							wts[cnt] = Math::pow(g, 1.1) * (dx != 0 && dz != 0 ? 0.354 : 0.5); // contour length
							wsum += wts[cnt];
							cnt++;
						}
					}
				}
				if (cnt == 0) {
					continue; // a hollow in the ice surface: the load goes englacial and leaves in the meltwater
				}
				for (int q = 0; q < cnt; q++) {
					debris[nbs[q]] += debris[i] * wts[q] / wsum;
					carried[nbs[q]] |= uint8_t(on_ice || carried[i]);
				}
			}
			// Each margin cell's debris spread along the margin (the ice edge wanders a little over the stand), then
			// laid as a cone at the angle of repose, V = pi r^2 t / 3 with t = r tan(repose); cones merge by maximum.
			LocalVector<double> spread;
			spread.resize(n);
			for (int pass = 0; pass < 2; pass++) {
				for (int i = 0; i < n; i++) {
					spread[i] = dep[i];
				}
				for (int z = 1; z < ny - 1; z++) {
					for (int x = 1; x < nx - 1; x++) {
						const int i = z * nx + x;
						if (dep[i] <= 0.0) {
							continue;
						}
						double sum = 0.0, w = 0.0;
						for (int dz = -1; dz <= 1; dz++) {
							for (int dx = -1; dx <= 1; dx++) {
								const int j = i + dz * nx + dx;
								if (dep[j] > 0.0) {
									sum += dep[j];
									w += 1.0;
								}
							}
						}
						spread[i] = sum / w;
					}
				}
				for (int i = 0; i < n; i++) {
					dep[i] = spread[i];
				}
			}
			LocalVector<double> top;
			top.resize(n);
			memcpy(top.ptr(), h.ptr(), sizeof(double) * n);
			for (int i = 0; i < n; i++) {
				if (dep[i] <= 0.0) {
					continue;
				}
				last_moraine_volume += dep[i];
				const double t = Math::pow(3.0 * dep[i] * repose * repose / Math::PI, 1.0 / 3.0);
				const int rc = int(Math::ceil(t / repose / c));
				const int x = i % nx, z = i / nx;
				for (int zz = MAX(z - rc, 0); zz <= MIN(z + rc, ny - 1); zz++) {
					for (int xx = MAX(x - rc, 0); xx <= MIN(x + rc, nx - 1); xx++) {
						const int j = zz * nx + xx;
						if (sp.b.outlet && sp.b.outlet[j]) {
							continue;
						}
						const double d = Math::sqrt(double((xx - x) * (xx - x) + (zz - z) * (zz - z))) * c;
						top[j] = MAX(top[j], h[i] + t - repose * d);
					}
				}
			}
			till_cover.resize(n);
			for (int i = 0; i < n; i++) {
				till_cover[i] = float(MAX(top[i] - h[i], 0.0)); // for the ground material: till
				if (top[i] > h[i]) {
					sed[i] += top[i] - h[i]; // till: loose, soil on it
					h[i] = top[i];
				}
			}
		}
		last_glacial_eroded = real_t(glac.eroded_max);
		last_glacial_ice_area = real_t(glac.ice_area);
		glacial_ice.resize(n);
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				glacial_ice[z * nx + x] = float(glac.ice_at(x, z, cell_size));
			}
		}
		// Ice-worn rock: where the last glaciers flowed and how much rock they wore, for the detail shader
		// (terrain_ice_flow: rg the flow direction in xz, b the wear). Ice flows down its surface (bed + ice),
		// smoothed over the ice grid; wear is the last glaciation's erosion, full at 20 m.
		{
			const std::vector<double> &worn = glac.get_total();
			Vector<uint8_t> bytes;
			bytes.resize(n * 4);
			uint8_t *w = bytes.ptrw();
			bool any = false;
			const int k = MAX(1, int(Math::round(64.0 / double(cell_size))));
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int i = z * nx + x;
					auto surf = [&](int xx, int zz) {
						xx = CLAMP(xx, 0, nx - 1);
						zz = CLAMP(zz, 0, ny - 1);
						return h[zz * nx + xx] + double(glacial_ice[zz * nx + xx]);
					};
					double fx = -(surf(x + k, z) - surf(x - k, z));
					double fz = -(surf(x, z + k) - surf(x, z - k));
					const double fl = Math::sqrt(fx * fx + fz * fz);
					double wear = worn.size() == size_t(n) ? CLAMP(worn[i] / 20.0, 0.0, 1.0) : 0.0;
					if (fl < 1e-6) {
						wear = 0.0;
						fx = 1.0;
						fz = 0.0;
					} else {
						fx /= fl;
						fz /= fl;
					}
					any = any || wear > 0.0;
					w[i * 4 + 0] = uint8_t(CLAMP(Math::round((fx * 0.5 + 0.5) * 255.0), 0.0, 255.0));
					w[i * 4 + 1] = uint8_t(CLAMP(Math::round((fz * 0.5 + 0.5) * 255.0), 0.0, 255.0));
					w[i * 4 + 2] = uint8_t(Math::round(wear * 255.0));
					w[i * 4 + 3] = 255;
				}
			}
			if (any && ice_worn_detail) {
				Ref<Image> img = Image::create_from_data(nx, ny, false, Image::FORMAT_RGBA8, bytes);
				if (ice_flow_texture.is_valid() && ice_flow_texture->get_width() == nx && ice_flow_texture->get_height() == ny) {
					ice_flow_texture->update(img);
				} else {
					ice_flow_texture = ImageTexture::create_from_image(img);
				}
			} else {
				ice_flow_texture.unref();
			}
		}
		scree_pass();
			meander_pass();
			outwash_pass();
			unfilled.resize(n);
		memcpy(unfilled.ptr(), h.ptr(), sizeof(double) * n);
	} else {
		scree_pass();
		meander_pass();
		outwash_pass();
	}
	last_glacial_msec = real_t(double(glacial_usec) / 1000.0);
	sill_cells.clear();
	if (!sill_fine.is_empty()) {
		sill_cells.resize(n);
		for (int i = 0; i < n; i++) {
			sill_cells[i] = float(sill_fine[i]);
		}
	}
	inflow_cells.clear();
	if (!water_fine.is_empty()) {
		inflow_cells.resize(n);
		for (int i = 0; i < n; i++) {
			inflow_cells[i] = float(water_fine[i]);
		}
	}
	hardness.clear();
	if (strata_enabled) {
		if (levels > 0) {
			strata_prepare(nx, ny, cell_size);
		}
		// The rock at the surface everywhere (the bands as they crop out, on hillslopes too).
		hardness.resize(n);
		for (int i = 0; i < n; i++) {
			hardness[i] = float(strata_weight(h[i] - up[i] * strata_time + strata_offset[i]));
		}
	}
	// Abandoned slots on the fans: the single-direction rivers switch cells from step to step on a depositing
	// surface and each leaves a one-cell trench in the toe (a channel the river has since left). Real fans backfill
	// abandoned channels (avulsion, Bryant et al. 1995; Jerolmack and Paola 2007). A cell on alluvium lower than six
	// or more of its eight neighbours by half a metre, carrying less than a river's area, fills to the median of its
	// neighbours (an edge-keeping filter: steps and scarps stay). Two passes.
	last_slots_filled = 0;
	if (result_deposition && fan_backfill) {
		const double river_area = 1.0e5;
		for (int pass = 0; pass < 2; pass++) {
			for (int z = 1; z < ny - 1; z++) {
				for (int x = 1; x < nx - 1; x++) {
					const int i = z * nx + x;
					if (sed[i] < 0.3 || area[i] >= river_area || (!meander_cell.is_empty() && meander_cell[i])) {
						continue;
					}
					double nb[8];
					int k = 0, higher = 0;
					for (int dz = -1; dz <= 1; dz++) {
						for (int dx = -1; dx <= 1; dx++) {
							if (dx == 0 && dz == 0) {
								continue;
							}
							nb[k] = h[i + dz * nx + dx];
							higher += nb[k] > h[i] + 0.5 ? 1 : 0;
							k++;
						}
					}
					if (higher < 6) {
						continue;
					}
					std::sort(nb, nb + 8);
					const double med = 0.5 * (nb[3] + nb[4]);
					sed[i] += med - h[i];
					h[i] = med;
					last_slots_filled++;
				}
			}
		}
	}
	sp.route();
	if (!unfilled.is_empty()) {
		// The basins the ice wore stay: the receivers and areas come from the filled surface (water crosses the
		// lakes), the ground and its sediment go back to what the ice left. Other hollows stay filled.
		LocalVector<uint8_t> seen;
		seen.resize(n);
		memset(seen.ptr(), 0, n);
		LocalVector<int32_t> part;
		for (int s0 = 0; s0 < n; s0++) {
			if (seen[s0] || h[s0] - unfilled[s0] <= 0.0) {
				continue;
			}
			part.clear();
			part.push_back(s0);
			seen[s0] = 1;
			bool iced = false;
			for (uint32_t q = 0; q < part.size(); q++) {
				const int32_t c = part[q];
				iced = iced || glacial_ice[c] > 1.0f;
				const int cx = c % nx, cz = c / nx;
				for (int dz = -1; dz <= 1; dz++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = cx + dx, zz = cz + dz;
						if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
							continue;
						}
						const int j = zz * nx + xx;
						if (!seen[j] && h[j] - unfilled[j] > 0.0) {
							seen[j] = 1;
							part.push_back(j);
						}
					}
				}
			}
			if (!iced) {
				continue;
			}
			for (int32_t c : part) {
				sed[c] = MAX(sed[c] - (h[c] - unfilled[c]), 0.0);
				h[c] = unfilled[c];
			}
		}
	}
	sp.b.erodibility_scale = nullptr;
	sp.b.talus_cell = nullptr;
	sp.b.hard_depth = nullptr;
	sp.b.diffusion_scale = nullptr;
	last_steps = levels > 0 ? steps + levels * refine_steps : steps;
	last_deposition_passes = sp.deposition_pass_total;
	last_coarse_levels = levels;
	last_change = real_t(change);

	double hmax = sea_level;
	for (int i = 0; i < n; i++) {
		hmax = MAX(hmax, h[i]);
	}
	const double scale = (max_height > 0.0 && hmax > sea_level) ? (double(max_height) - sea_level) / (hmax - sea_level) : 1.0;

	result_min = float(sea_level + height_offset);
	result_max = float(sea_level + (hmax - sea_level) * scale + height_offset);
	heights.resize(n);
	drainage.resize(n);
	sediment.resize(n);
	uplift_field.resize(n);
	for (int i = 0; i < n; i++) {
		heights[i] = float(sea_level + (h[i] - sea_level) * scale + height_offset);
		drainage[i] = float(area[i]);
		sediment[i] = float(sed[i] * scale);
		uplift_field[i] = float(up[i] * 1.0e3);
	}
	// The meandered channels into the same heights: their floodplain level scaled like the ground, their depth kept.
	for (Channel &ch : channels) {
		if (ch.braid_thread || !ch.braid.is_empty()) {
			continue; // relative to the floor
		}
		for (uint32_t k = 0; k < ch.bed.size(); k++) {
			const double plain = double(ch.bed[k]) + double(ch.dep[k]);
			ch.bed[k] = real_t(sea_level + (plain - sea_level) * scale + height_offset - double(ch.dep[k]));
		}
	}
	// Loose fill cannot stand steeper than its critical slope: what the solver left on such ground (deposits on hard
	// sill cells that do not wear, then faces cut below them) is rock with scree, not soil. Faded from 85% to 110%
	// of the critical gradient, as the geology fades soil (otherwise a 74 degree bar face read as 390 m of soil and
	// grew grass and conifers).
	if (critical_slope > 0.0) {
		const double cs = Math::tan(Math::deg_to_rad(double(critical_slope)));
		LocalVector<float> keep;
		keep.resize(n);
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const int i = z * nx + x;
				const double gx = (heights[z * nx + MIN(x + 1, nx - 1)] - heights[z * nx + MAX(x - 1, 0)]) / (double(MIN(x + 1, nx - 1) - MAX(x - 1, 0)) * cell_size);
				const double gz = (heights[MIN(z + 1, ny - 1) * nx + x] - heights[MAX(z - 1, 0) * nx + x]) / (double(MIN(z + 1, ny - 1) - MAX(z - 1, 0)) * cell_size);
				const double t = CLAMP((Math::sqrt(gx * gx + gz * gz) - 0.85 * cs) / (0.25 * cs), 0.0, 1.0);
				keep[i] = float(1.0 - t * t * (3.0 - 2.0 * t));
			}
		}
		for (int i = 0; i < n; i++) {
			sediment[i] *= keep[i];
			sed[i] *= double(keep[i]);
		}
	}
	_alluvium_reach();

	// Seabed and beaches below and along the coast.
	LocalVector<float> pre_coast; // the meandered channels follow what the coast and the lakes do to the ground
	if (!channels.is_empty()) {
		pre_coast = heights;
	}
	const uint64_t tw = OS::get_singleton()->get_ticks_usec();
	if (has_sea) {
		_shape_coast(sea_mask);
	}
	sea_bottom = result_min;
	for (int i = 0; i < n; i++) {
		sea_bottom = MIN(sea_bottom, heights[i]);
	}

	// Geology (soil, rock, talus) from the final ground, and the receivers for the rivers.
	receivers.resize(n);
	memcpy(receivers.ptr(), rec.ptr(), sizeof(int32_t) * n);
	channel_area.resize(n);
	TerrainGeology::d8_area(n, receivers.ptr(), stack.ptr(), double(cell_size) * cell_size, channel_area.ptr());
	// Rain and snowmelt from the climate (before the lakes, whose balance takes the mean runoff).
	_bake_runoff(sea_mask);
	// Lakes behind natural dams (they change the ground, so before the geology).
	_make_lakes(sea_mask);
	if (!pre_coast.is_empty()) {
		// Oxbows move with the ground under them. A reach's floodplain runs evenly between the final ground at its two
		// ends (detail included), so the channel meets the river's course above and below it exactly: the water may
		// not climb out of it.
		chan_buckets.clear();
		for (Channel &ch : channels) {
			if (!ch.braid.is_empty()) {
				// The plain's water surface on the final floor, never rising downstream.
				// (the closest non-rising profile to the floor's, by pooling adjacent violators: a plain min dropped
				// the surface under a bump all the way down).
				const uint32_t np = ch.points.size();
				ch.stage.resize(np);
				LocalVector<double> val, wt;
				LocalVector<uint32_t> len;
				for (uint32_t k = 0; k < np; k++) {
					val.push_back(double(_ground(ch.points[k].x, ch.points[k].y)) + ch.stage_rel[k]);
					wt.push_back(1.0);
					len.push_back(1);
					while (val.size() > 1 && val[val.size() - 1] > val[val.size() - 2]) {
						const uint32_t a = val.size() - 2, b = val.size() - 1;
						val[a] = (val[a] * wt[a] + val[b] * wt[b]) / (wt[a] + wt[b]);
						wt[a] += wt[b];
						len[a] += len[b];
						val.resize(b);
						wt.resize(b);
						len.resize(b);
					}
				}
				for (uint32_t blk = 0, k = 0; blk < val.size(); blk++) {
					for (uint32_t t = 0; t < len[blk]; t++) {
						ch.stage[k++] = real_t(val[blk]);
					}
				}
				continue;
			}
			if (ch.braid_thread) {
				// The thread lies in the floor's relief: its bed the final ground there plus that relief, never rising.
				const uint32_t last = ch.points.size() - 1;
				const real_t step = 0.0005f * ch.points[0].distance_to(ch.points[MIN(last, 1u)]);
				for (uint32_t k = 0; k <= last; k++) {
					ch.bed[k] = _ground(ch.points[k].x, ch.points[k].y) + ch.bed[k] - ch.dep[k] * 0.6f;
					if (k > 0) {
						ch.bed[k] = MIN(ch.bed[k], ch.bed[k - 1] - step);
					}
				}
				continue;
			}
			if (!ch.reach) {
				for (uint32_t k = 0; k < ch.bed.size(); k++) {
					ch.bed[k] += _sample(heights, ch.points[k].x, ch.points[k].y) - _sample(pre_coast, ch.points[k].x, ch.points[k].y);
				}
				continue;
			}
			const uint32_t last = ch.points.size() - 1;
			const real_t g0 = _ground(ch.points[0].x, ch.points[0].y);
			real_t g1 = _ground(ch.points[last].x, ch.points[last].y);
			g1 = MIN(g1, g0);
			real_t len = 0.0, run = 0.0;
			for (uint32_t k = 1; k <= last; k++) {
				len += ch.points[k].distance_to(ch.points[k - 1]);
			}
			for (uint32_t k = 0; k <= last; k++) {
				if (k > 0) {
					run += ch.points[k].distance_to(ch.points[k - 1]);
				}
				ch.bed[k] = Math::lerp(g0, g1, run / MAX(len, real_t(1e-3))) - ch.dep[k];
			}
		}
		_bucket_channels();
	}
	uint64_t water_usec = OS::get_singleton()->get_ticks_usec() - tw;
	soil.resize(n);
	rock.resize(n);
	talus.resize(n);
	TerrainGeology::Params gp;
	gp.nx = nx;
	gp.ny = ny;
	gp.dx = cell_size;
	gp.erodibility = erodibility;
	gp.area_exponent = area_exponent;
	gp.diffusion = hillslope_diffusion;
	gp.critical_slope = sp.p.talus > 0.0 ? sp.p.talus : 10.0;
	gp.soil_production = soil_production * 1.0e-3;
	gp.soil_scale = soil_depth_scale;
	gp.soil_max = max_soil_depth;
	gp.repose = Math::tan(Math::deg_to_rad(double(talus_angle)));
	gp.talus_reach = talus_reach;
	double umax = 0.0;
	for (int i = 0; i < n; i++) {
		umax = MAX(umax, up[i]);
	}
	gp.steepness_ref = MAX(umax / MAX(double(erodibility), 1e-12), 1e-6);
	TerrainGeology::compute(gp, heights.ptr(), drainage.ptr(), sediment.ptr(), receivers.ptr(), stack.ptr(), soil.ptr(), rock.ptr(), talus.ptr());
	// The scree the walls shed (scree_age) is scree at the surface, a metre of it full: it reads as scree to the
	// material and to plant rules, however the solver's sediment has it.
	if (scree_cover.size() == uint32_t(n)) {
		for (int i = 0; i < n; i++) {
			const float t = CLAMP(scree_cover[i], 0.0f, 1.0f);
			talus[i] = MAX(talus[i], t * t * (3.0f - 2.0f * t));
		}
	}
	Ref<Image> geo = get_geology_image();
	if (geology_texture.is_valid() && geology_texture->get_width() == nx && geology_texture->get_height() == ny) {
		geology_texture->update(geo);
	} else {
		geology_texture = ImageTexture::create_from_image(geo);
	}
	const uint64_t tr = OS::get_singleton()->get_ticks_usec();
	_route_rivers();
	_cut_tails();
	_bake_lips();
	water_usec += OS::get_singleton()->get_ticks_usec() - tr;
	last_water_msec = double(water_usec) / 1000.0;
	_bake_detail_keep();
	_bake_spray();
	_publish_globals();
	_erode_tiles();
	// After the tiles: the sky is read from the final ground.
	_bake_sky();
	_publish_globals();
	last_generate_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("generated"));
	return true;
}

void TerrainGenerator3D::clear_result() {
	emit_signal(SNAME("changing"));
	nx = 0;
	ny = 0;
	heights.clear();
	sky_vis.clear();
	sky_horizon.clear();
	sky_sh.clear();
	sky_nx = sky_ny = 0;
	drainage.clear();
	sediment.clear();
	alluvium_reach.clear();
	inflow_cells.clear();
	sill_cells.clear();
	glacial_ice.clear();
	channels.clear();
	chan_buckets.clear();
	channel_of_cell.clear();
	lips.clear();
	lip_buckets.clear();
	uplift_field.clear();
	soil.clear();
	rock.clear();
	talus.clear();
	beach.clear();
	receivers.clear();
	channel_area.clear();
	water_body.clear();
	water_depth.clear();
	lake_rim.clear();
	lake_rim_weight.clear();
	river_receivers.clear();
	river_area.clear();
	lake_info.clear();
	runoff_fields.clear();
	runoff_n = 0;
	flow_index.clear();
	flows.clear();
	has_sea = false;
	tile_delta.clear();
	tile_flow.clear();
	tile_deposit.clear();
}

// Catmull-Rom bicubic between grid nodes (interpolating, so every node keeps its eroded height), clamped at the
// borders. NaN outside the rectangle.
float TerrainGenerator3D::_sample(const LocalVector<float> &p_grid, real_t p_x, real_t p_z) const {
	if (nx < 2 || ny < 2 || p_grid.size() != uint32_t(nx * ny)) {
		return Math::NaN;
	}
	const Rect2 &rect = result_rect;
	const real_t gx = (p_x - rect.position.x) / result_cell;
	const real_t gz = (p_z - rect.position.y) / result_cell;
	const real_t eps = 1.0e-4;
	if (gx < -eps || gz < -eps || gx > nx - 1 + eps || gz > ny - 1 + eps) {
		return Math::NaN;
	}
	const int ix = CLAMP(int(Math::floor(gx)), 0, nx - 2);
	const int iz = CLAMP(int(Math::floor(gz)), 0, ny - 2);
	const real_t fx = CLAMP(gx - ix, real_t(0.0), real_t(1.0));
	const real_t fz = CLAMP(gz - iz, real_t(0.0), real_t(1.0));
	auto at = [&](int x, int z) -> real_t {
		return p_grid[CLAMP(z, 0, ny - 1) * nx + CLAMP(x, 0, nx - 1)];
	};
	auto cr = [](real_t p0, real_t p1, real_t p2, real_t p3, real_t t) -> real_t {
		return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
	};
	real_t rows[4];
	for (int k = 0; k < 4; k++) {
		const int z = iz - 1 + k;
		rows[k] = cr(at(ix - 1, z), at(ix, z), at(ix + 1, z), at(ix + 2, z), fx);
	}
	return float(cr(rows[0], rows[1], rows[2], rows[3], fz));
}

// Alluvium depth (capped at the ALLUVIUM_FADE metres over which the detail fades) carried out past the deposit,
// losing all of it over 3/8 of the detail's longest wavelength: the toe of a fan, where the deposit thins to a few
// decimetres over a wide convex slope, would otherwise get the bedrock gullies at nearly full depth, all running
// down the same slope side by side. A two-pass
// chamfer distance (Borgefors 1986), so it costs two sweeps of the grid.
static constexpr float ALLUVIUM_FADE = 0.5f;

void TerrainGenerator3D::_alluvium_reach() {
	alluvium_reach.clear();
	if (!result_deposition || sediment.size() != uint32_t(nx * ny) || nx < 2 || ny < 2) {
		return;
	}
	const int n = nx * ny;
	alluvium_reach.resize(n);
	for (int i = 0; i < n; i++) {
		alluvium_reach[i] = MIN(MAX(sediment[i], 0.0f), ALLUVIUM_FADE);
	}
	// River channels across the floor keep little alluvium themselves (they are where it moves): a plateau of two
	// cells first, so a channel and its banks inside a deposit count as the deposit, then the fall-off.
	{
		const int rad = MAX(1, int(Math::round(32.0 / double(cell_size))));
		LocalVector<float> row_max;
		row_max.resize(n);
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				float v = 0.0f;
				for (int k = MAX(0, x - rad); k <= MIN(nx - 1, x + rad); k++) {
					v = MAX(v, alluvium_reach[z * nx + k]);
				}
				row_max[z * nx + x] = v;
			}
		}
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				float v = 0.0f;
				for (int k = MAX(0, z - rad); k <= MIN(ny - 1, z + rad); k++) {
					v = MAX(v, row_max[k * nx + x]);
				}
				alluvium_reach[z * nx + x] = v;
			}
		}
	}
	const float per_m = ALLUVIUM_FADE / float(MAX(double(detail_wavelength) * 0.375, double(cell_size)));
	const float a = per_m * float(cell_size);
	const float d = a * float(Math::SQRT2);
	float *r = alluvium_reach.ptr();
	for (int z = 0; z < ny; z++) {
		for (int x = 0; x < nx; x++) {
			float v = r[z * nx + x];
			if (x > 0) {
				v = MAX(v, r[z * nx + x - 1] - a);
			}
			if (z > 0) {
				v = MAX(v, r[(z - 1) * nx + x] - a);
				if (x > 0) {
					v = MAX(v, r[(z - 1) * nx + x - 1] - d);
				}
				if (x < nx - 1) {
					v = MAX(v, r[(z - 1) * nx + x + 1] - d);
				}
			}
			r[z * nx + x] = v;
		}
	}
	for (int z = ny - 1; z >= 0; z--) {
		for (int x = nx - 1; x >= 0; x--) {
			float v = r[z * nx + x];
			if (x < nx - 1) {
				v = MAX(v, r[z * nx + x + 1] - a);
			}
			if (z < ny - 1) {
				v = MAX(v, r[(z + 1) * nx + x] - a);
				if (x < nx - 1) {
					v = MAX(v, r[(z + 1) * nx + x + 1] - d);
				}
				if (x > 0) {
					v = MAX(v, r[(z + 1) * nx + x - 1] - d);
				}
			}
			r[z * nx + x] = v;
		}
	}
}

namespace {
constexpr real_t LIP_BUCKET = 128.0; // m
constexpr real_t LIP_FACE = 3.0; // m the face takes from the lip's height to the foot's (three 1 m grid steps, so a face oblique to the grid does not alias into ribs)
constexpr real_t LIP_SIDE = 12.0; // m beside the held width over which the step fades into the slope
constexpr real_t GORGE_WALL = 2.0; // m over which a gorge wall rises from its floor to the ground beside it
constexpr real_t POOL_BAR = 1.4; // the tail bar reaches this many pool radii from the centre
constexpr real_t NOTCH_DROP = 25.0; // m: taller falls keep their face and get a notch at the lip instead of a step
constexpr real_t LIP_END = 8.0; // m before the lip and past the foot over which it fades
} // namespace

float TerrainGenerator3D::_ground_detail(real_t p_x, real_t p_z) const {
	const float base = _sample(heights, p_x, p_z);
	if (!detail_enabled || detail_octaves <= 0 || detail_depth <= 0.0 || Math::is_nan(base)) {
		return base;
	}
	// Gradient of the simulated surface over a 3x3 stencil one grid cell wide, clamped inside the rectangle. The
	// four quarter gradients also give the coherence of the slope: their mean length over their mean (Kass and
	// Witkin 1987, Bigun and Granlund 1987). It is 1 on an even slope and falls towards 0 where opposite slopes meet
	// (crests, valley floors); the filter fades with it, so gullies do not notch a crest into a comb.
	const Rect2 &rect = result_rect;
	const real_t e = result_cell;
	const real_t xs[3] = { MAX(p_x - e, rect.position.x), p_x, MIN(p_x + e, rect.position.x + rect.size.x) };
	const real_t zs[3] = { MAX(p_z - e, rect.position.y), p_z, MIN(p_z + e, rect.position.y + rect.size.y) };
	double hs[3][3];
	for (int j = 0; j < 3; j++) {
		for (int i = 0; i < 3; i++) {
			hs[j][i] = (i == 1 && j == 1) ? double(base) : double(_sample(heights, xs[i], zs[j]));
		}
	}
	double gx = 0.0;
	double gz = 0.0;
	double glen = 0.0;
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			const double wx = MAX(double(xs[i + 1] - xs[i]), 1e-6);
			const double wz = MAX(double(zs[j + 1] - zs[j]), 1e-6);
			const double qx = (hs[j][i + 1] - hs[j][i] + hs[j + 1][i + 1] - hs[j + 1][i]) / (2.0 * wx);
			const double qz = (hs[j + 1][i] - hs[j][i] + hs[j + 1][i + 1] - hs[j][i + 1]) / (2.0 * wz);
			gx += 0.25 * qx;
			gz += 0.25 * qz;
			glen += 0.25 * Math::sqrt(qx * qx + qz * qz);
		}
	}
	// Remapped so an even slope (and a gently curved one) keeps the full effect.
	const double coherence = glen > 1e-9 ? (Math::sqrt(gx * gx + gz * gz) / glen - 0.3) / 0.6 : 0.0;
	TerrainErosionFilter::Params ep;
	ep.seed = uint32_t(seed) * 0x2545F491u + 1u;
	ep.wavelength = detail_wavelength;
	ep.octaves = detail_octaves;
	ep.depth = detail_depth;
	ep.detail = detail_reach;
	ep.gully_weight = detail_gully_weight;
	ep.assumed_slope = detail_assumed_slope;
	ep.valley_height = result_min;
	ep.peak_height = MAX(result_max, result_min + 1.0f);
	double off = TerrainErosionFilter::offset(ep, p_x, p_z, base, gx, gz, coherence);
	// Crests: where the gullies fade (low coherence) on convex ground, an irregular ridged noise takes over, so a
	// crest line keeps pinnacles and cols of varied size instead of turning straight (or a regular comb).
	if (detail_crest > 0.0) {
		const double convex = -(hs[1][0] + hs[1][2] + hs[0][1] + hs[2][1] - 4.0 * hs[1][1]) / (4.0 * e);
		const double c = CLAMP(coherence, 0.0, 1.0);
		const double fade = 1.0 - c * c * (3.0 - 2.0 * c);
		const double t = CLAMP(convex / 0.05, 0.0, 1.0);
		const double w = fade * t * t * (3.0 - 2.0 * t);
		if (w > 0.0) {
			const double amp = double(detail_crest) * 2.0 * detail_depth / MAX(double(detail_gully_weight), 1e-3) * detail_wavelength;
			off += w * amp * TerrainErosionFilter::crest_noise(ep.seed ^ 0x5bd1e995u, p_x, p_z, detail_wavelength * 2.0, MAX(detail_octaves, 1) + 2);
		}
	}
	// Not under water (the seabed and lake beds stay smooth), and never down to the outlets: land the filter cuts
	// stays above the sea (by 0.15 m), so the coast keeps its shape and no hollow floods.
	off *= _water_fade(p_x, p_z);
	if (result_deposition && alluvium_reach.size() == heights.size()) {
		// Alluvium the rivers laid down is smooth (an aggrading surface buries gullies): bedrock gullies and crests
		// fade out over its first half metre, so a fan front or a terrace riser stays a soft bank instead of growing
		// rocky knobs. The fade reaches past the thin
		// edge of a deposit (see _alluvium_reach), so a fan toe does not grow a comb of gullies either.
		// Loose fill does not stand steeper than its angle of repose (about 35 degrees): on steeper ground the
		// sample is rock however much sediment the cells around it hold, and keeps its detail.
		const double steep = CLAMP((Math::sqrt(gx * gx + gz * gz) - 0.7) / 0.3, 0.0, 1.0);
		const double sd = CLAMP(double(_sample(alluvium_reach, p_x, p_z)) / ALLUVIUM_FADE, 0.0, 1.0) * (1.0 - steep * steep * (3.0 - 2.0 * steep));
		off *= 1.0 - sd * sd * (3.0 - 2.0 * sd);
	}
	if (has_sea) {
		// Beaches and the coastal strip stay smooth: the detail grows in over the first metres above the sea.
		const double t = CLAMP((double(base) - double(result_min) - 0.5) / 5.5, 0.0, 1.0);
		off *= t * t * (3.0 - 2.0 * t);
	}
	// Ledges on cliffs (row 11 of the waterfall gap list): steep faces of bedded rock weather into steps, each harder
	// bed a ledge over a softer recess (differential erosion of interbedded strata, Forte et al. 2016; the layered
	// subsurface of Cordonnier et al. 2018, "Sculpting mountains"). Beds every cliff_ledge_spacing metres, their
	// heights wandering by most of a bed over a few hundred metres; each bed is a near-flat ledge for 60% of its
	// height and a riser for the rest, faded in from 40 to 55 degrees so soil slopes stay as they were.
	if (cliff_ledge_spacing > 0.0) {
		const double slope = Math::sqrt(gx * gx + gz * gz);
		const double tw = CLAMP((slope - 0.84) / (1.43 - 0.84), 0.0, 1.0);
		double w = tw * tw * (3.0 - 2.0 * tw) * double(cliff_ledge_strength) * _water_fade(p_x, p_z);
		if (w > 0.0) {
			const double L = double(cliff_ledge_spacing);
			const uint32_t ls = uint32_t(seed) * 0x9E3779B1u + 77u;
			const double wob = L * (0.8 * _gen_value_noise(ls, p_x, p_z, 240.0) + 0.25 * _gen_value_noise(ls + 1u, p_x, p_z, 60.0));
			const double h0 = double(base) + off;
			// Beds of uneven thickness: the bed count runs on a monotonic warp of height (two incommensurate waves,
			// slope stays positive), and each bed's ledge share is its own.
			const double hw = (h0 + wob) / L;
			const double q = hw + 0.22 * Math::sin(hw * 2.3999632) + 0.12 * Math::sin(hw * 0.9213 + 1.7);
			const double fl = Math::floor(q);
			const double f = q - fl;
			const uint32_t bh = (uint32_t(int64_t(fl)) * 0x9E3779B1u) ^ (ls * 0x85EBCA6Bu);
			// Each bed: a narrow ledge (20 to 35 percent of the bed, rising a quarter of its height, so it still sheds
			// water and scree) and the riser above it.
			const double l = 0.2 + 0.15 * double((bh >> 8) & 0xFFFF) / 65535.0;
			double t = f < l ? 0.25 * f / l : 0.25 + 0.75 * (f - l) / (1.0 - l);
			// Back from bed units to height: invert the warp locally (Newton step from q).
			const double qt = fl + t;
			const double dq = 1.0 + 0.22 * 2.3999632 * Math::cos(hw * 2.3999632) + 0.12 * 0.9213 * Math::cos(hw * 0.9213 + 1.7);
			const double ht = (hw + (qt - q) / dq) * L - wob;
			off += w * (ht - h0);
		}
	}
	// With a sea, a cut deeper than a quarter of the ground's height above that floor is compressed smoothly (exponentially, same
	// slope where it starts) to at most half of it, so gully floors near the coast keep sloping down to the sea
	// instead of flattening on it; high ground keeps its full detail.
	const double floor_h = double(result_min) + (has_sea ? 0.15 : 0.0);
	const double room = double(base) - floor_h;
	if (has_sea && off < -0.25 * room && room > 0.0) {
		const double q = 0.25 * room;
		off = -(q + q * (1.0 - Math::exp((off + q) / q)));
	}
	return float(MAX(double(base) + off, MIN(double(base), floor_h)));
}

float TerrainGenerator3D::_ground(real_t p_x, real_t p_z) const {
	float g = _ground_detail(p_x, p_z);
	if (Math::is_nan(g)) {
		return g;
	}
	if (tile_erosion_enabled && !tile_delta.is_empty()) {
		g += _tile_sample(tile_delta, p_x, p_z) * _water_fade(p_x, p_z);
		if (has_sea) {
			g = MAX(g, MIN(_sample(heights, p_x, p_z), result_min + 0.15f));
		}
	}
	if (!chan_buckets.is_empty()) {
		g = _apply_channels(p_x, p_z, g);
	}
	// Around a lake the detail and the tile erosion may not take ground that was above the water below it: the
	// shore and the dam hold the lake, and its surface ends where they rise.
	if (!lake_rim.is_empty()) {
		const int x = CLAMP(int(Math::round((p_x - result_rect.position.x) / result_cell)), 0, nx - 1);
		const int z = CLAMP(int(Math::round((p_z - result_rect.position.y) / result_cell)), 0, ny - 1);
		const float level = lake_rim[z * nx + x];
		if (!Math::is_nan(level)) {
			// Full within a cell of the water, fading out over the next two (a hard edge at the ring left terraces with
			// stepped walls along the cell lines).
			const float w = CLAMP(_sample(lake_rim_weight, p_x, p_z), 0.0f, 1.0f);
			const float hold = MIN(_sample(heights, p_x, p_z), level + 0.05f);
			if (g < hold) {
				g = Math::lerp(g, hold, w);
			}
		}
	}
	return lip_buckets.is_empty() ? g : _apply_lips(p_x, p_z, g);
}

void TerrainGenerator3D::_bucket_channels() {
	chan_buckets.clear();
	if (channels.is_empty()) {
	return;
	}
	const Vector2 world0 = result_rect.position;
	{
		chan_bx = int(Math::ceil(result_rect.size.x / CHANNEL_BUCKET)) + 1;
		chan_bz = int(Math::ceil(result_rect.size.y / CHANNEL_BUCKET)) + 1;
		chan_buckets.resize(chan_bx * chan_bz);
		for (uint32_t ci = 0; ci < channels.size(); ci++) {
			const Channel &ch = channels[ci];
			Rect2 box(ch.points[0], Vector2());
			for (const Vector2 &q : ch.points) {
				box.expand_to(q);
			}
			box = box.grow(2.0f * ch.half_width + 1.0f);
			const int x0 = CLAMP(int(Math::floor((box.position.x - world0.x) / CHANNEL_BUCKET)), 0, chan_bx - 1);
			const int x1 = CLAMP(int(Math::floor((box.get_end().x - world0.x) / CHANNEL_BUCKET)), 0, chan_bx - 1);
			const int z0 = CLAMP(int(Math::floor((box.position.y - world0.y) / CHANNEL_BUCKET)), 0, chan_bz - 1);
			const int z1 = CLAMP(int(Math::floor((box.get_end().y - world0.y) / CHANNEL_BUCKET)), 0, chan_bz - 1);
			for (int z = z0; z <= z1; z++) {
				for (int x = x0; x <= x1; x++) {
					const real_t bx0 = world0.x + x * CHANNEL_BUCKET, bz0 = world0.y + z * CHANNEL_BUCKET;
					const Rect2 cell(bx0, bz0, CHANNEL_BUCKET, CHANNEL_BUCKET);
					bool near = false;
					for (uint32_t k = 0; k < ch.points.size() && !near; k++) {
						near = cell.grow(2.0f * ch.half_width + 1.0f).has_point(ch.points[k]);
					}
					if (near) {
						chan_buckets[z * chan_bx + x].push_back(int(ci));
					}
				}
			}
		}
	}
}

// The meandered channels at full resolution: a parabolic bed within the half width, its banks easing back to the
// ground over a half width more. Only lowers the ground.
float TerrainGenerator3D::_apply_channels(real_t p_x, real_t p_z, float p_g) const {
	const int bx = int(Math::floor((p_x - result_rect.position.x) / CHANNEL_BUCKET));
	const int bz = int(Math::floor((p_z - result_rect.position.y) / CHANNEL_BUCKET));
	if (bx < 0 || bz < 0 || bx >= chan_bx || bz >= chan_bz) {
		return p_g;
	}
	float g = p_g;
	const Vector2 pp(p_x, p_z);
	for (int ci : chan_buckets[bz * chan_bx + bx]) {
		const Channel &ch = channels[ci];
		if (!ch.braid.is_empty()) {
			// A braidplain: the nearest row along it and the signed offset across, its relief bilinear.
			real_t bd = 1e30f, off = 0.0, row = 0.0;
			for (uint32_t k = 1; k < ch.points.size(); k++) {
				const Vector2 a = ch.points[k - 1], b = ch.points[k];
				const Vector2 ab = b - a;
				const real_t l2 = MAX(ab.length_squared(), real_t(1e-6));
				const real_t t = CLAMP((pp - a).dot(ab) / l2, real_t(0.0), real_t(1.0));
				const Vector2 q = pp - (a + ab * t);
				const real_t dd = q.length_squared();
				if (dd < bd) {
					bd = dd;
					row = real_t(k - 1) + t;
					off = (ab.x * q.y - ab.y * q.x) / Math::sqrt(l2);
				}
			}
			if (off <= -ch.braid_half || off >= ch.braid_half) {
				continue;
			}
			const int C = ch.braid_cols;
			const int R = int(ch.points.size());
			const real_t fc = (off + ch.braid_half) / (2.0f * ch.braid_half) * real_t(C - 1);
			const int c0 = CLAMP(int(fc), 0, C - 2), r0 = CLAMP(int(row), 0, R - 2);
			const real_t tc = CLAMP(fc - c0, real_t(0.0), real_t(1.0)), tr = CLAMP(row - r0, real_t(0.0), real_t(1.0));
			const float v = Math::lerp(Math::lerp(ch.braid[r0 * C + c0], ch.braid[r0 * C + c0 + 1], float(tc)), Math::lerp(ch.braid[(r0 + 1) * C + c0], ch.braid[(r0 + 1) * C + c0 + 1], float(tc)), float(tr));
			g += v;
			continue;
		}
		real_t best = 1e30f, bed = 0.0, dep = 0.0;
		for (uint32_t k = 1; k < ch.points.size(); k++) {
			const Vector2 a = ch.points[k - 1], b = ch.points[k];
			const Vector2 ab = b - a;
			const real_t l2 = MAX(ab.length_squared(), real_t(1e-6));
			const real_t t = CLAMP((pp - a).dot(ab) / l2, real_t(0.0), real_t(1.0));
			const real_t dd = pp.distance_to(a + ab * t);
			if (dd < best) {
				best = dd;
				bed = Math::lerp(ch.bed[k - 1], ch.bed[k], t);
				dep = Math::lerp(ch.dep[k - 1], ch.dep[k], t);
			}
		}
		const real_t hw = ch.half_width;
		if (best >= 2.0f * hw) {
			continue;
		}
		const real_t top = bed + dep;
		real_t target;
		if (best <= hw) {
			const real_t u = best / hw;
			target = bed + dep * u * u;
		} else {
			const real_t t = (best - hw) / hw;
			target = Math::lerp(top, real_t(g), t * t * (3.0f - 2.0f * t));
		}
		g = MIN(g, float(target));
	}
	return g;
}

// Bilinear on the tile erosion grid (its cells are fine: a few metres).
float TerrainGenerator3D::_tile_sample(const LocalVector<float> &p_grid, real_t p_x, real_t p_z) const {
	if (p_grid.size() != uint32_t(tile_nx * tile_nz)) {
		return 0.0f;
	}
	const real_t gx = CLAMP((p_x - result_rect.position.x) / tile_cell, real_t(0.0), real_t(tile_nx - 1));
	const real_t gz = CLAMP((p_z - result_rect.position.y) / tile_cell, real_t(0.0), real_t(tile_nz - 1));
	const int x0 = MIN(int(gx), tile_nx - 2);
	const int z0 = MIN(int(gz), tile_nz - 2);
	const real_t fx = gx - x0;
	const real_t fz = gz - z0;
	const float *g = p_grid.ptr();
	const real_t a = Math::lerp(real_t(g[z0 * tile_nx + x0]), real_t(g[z0 * tile_nx + x0 + 1]), fx);
	const real_t b = Math::lerp(real_t(g[(z0 + 1) * tile_nx + x0]), real_t(g[(z0 + 1) * tile_nx + x0 + 1]), fx);
	return float(Math::lerp(a, b, fz));
}

// One tile: its core plus an apron on every side, eroded on its own (seeded by its index) so the result does not
// depend on threads or on the other tiles.
void TerrainGenerator3D::_erode_tile(void *p_job, uint32_t p_tile) {
	TileJob *job = static_cast<TileJob *>(p_job);
	const TerrainGenerator3D *gen = job->gen;
	const int tx = int(p_tile) % job->tiles_x;
	const int tz = int(p_tile) / job->tiles_x;
	const int x0 = MAX(tx * job->core - job->apron, 0);
	const int z0 = MAX(tz * job->core - job->apron, 0);
	const int x1 = MIN((tx + 1) * job->core + job->apron, gen->tile_nx - 1);
	const int z1 = MIN((tz + 1) * job->core + job->apron, gen->tile_nz - 1);
	TerrainHydraulic::Params hp;
	hp.nx = x1 - x0 + 1;
	hp.ny = z1 - z0 + 1;
	hp.dx = gen->tile_cell;
	hp.seed = TerrainHydraulic::hash(uint32_t(gen->seed) * 0x9E3779B9u ^ (uint32_t(tx) * 0x85EBCA6Bu + uint32_t(tz) * 0xC2B2AE35u));
	hp.droplets_per_cell = gen->tile_erosion_droplets;
	hp.max_depth = gen->tile_erosion_max_depth;
	hp.rock_hardness = gen->tile_erosion_rock_hardness;
	hp.repose = Math::tan(Math::deg_to_rad(double(gen->talus_angle)));
	const int n = hp.nx * hp.ny;
	std::vector<float> in(n), cover(n), out(n), flow(n), dep(n);
	for (int z = 0; z < hp.ny; z++) {
		for (int x = 0; x < hp.nx; x++) {
			const real_t wx = gen->result_rect.position.x + (x0 + x) * gen->tile_cell;
			const real_t wz = gen->result_rect.position.y + (z0 + z) * gen->tile_cell;
			in[z * hp.nx + x] = gen->_ground_detail(wx, wz);
			cover[z * hp.nx + x] = MAX(gen->_sample(gen->soil, wx, wz), 0.0f) + MAX(gen->_sample(gen->talus, wx, wz), 0.0f);
		}
	}
	TerrainHydraulic::run(hp, in.data(), cover.data(), out.data(), flow.data(), dep.data());
	LocalVector<float> &r = (*job->results)[p_tile];
	r.resize(n * 3);
	for (int i = 0; i < n; i++) {
		r[i * 3 + 0] = out[i] - in[i];
		r[i * 3 + 1] = flow[i];
		r[i * 3 + 2] = dep[i];
	}
}

// Stage 4: overlapping tiles (core plus apron), each eroded on its own, blended with weights that are 1 over the
// core and fall smoothly to 0 halfway out into the apron, so neighbours cross-fade and droplets leaving a tile's
// edge (which cut it short) never show.
void TerrainGenerator3D::_erode_tiles() {
	tile_delta.clear();
	tile_flow.clear();
	tile_deposit.clear();
	if (!tile_erosion_enabled || nx < 2) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	tile_cell = tile_erosion_spacing;
	tile_nx = int(Math::floor((nx - 1) * result_cell / tile_cell)) + 1;
	tile_nz = int(Math::floor((ny - 1) * result_cell / tile_cell)) + 1;
	const int core = MAX(int(Math::round(tile_erosion_tile_size / tile_cell)), 8);
	const int apron = MAX(int(Math::round(tile_erosion_apron / tile_cell)), 2);
	const int tiles_x = (tile_nx - 1 + core - 1) / core;
	const int tiles_z = (tile_nz - 1 + core - 1) / core;
	LocalVector<LocalVector<float>> results;
	results.resize(tiles_x * tiles_z);
	TileJob job{ this, tiles_x, core, apron, &results };
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&TerrainGenerator3D::_erode_tile, &job, tiles_x * tiles_z, -1, true, SNAME("TerrainGenerator3D tiles"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	const int n = tile_nx * tile_nz;
	LocalVector<double> acc;
	LocalVector<double> wsum;
	acc.resize(n * 3);
	wsum.resize(n);
	memset(acc.ptr(), 0, sizeof(double) * n * 3);
	memset(wsum.ptr(), 0, sizeof(double) * n);
	const double fade = MAX(apron * 0.5, 1.0);
	auto ramp = [&](int c, int lo, int hi) -> double {
		// 1 inside [lo, hi], falling to 0 at fade cells outside it.
		const double d = c < lo ? lo - c : (c > hi ? c - hi : 0);
		const double t = CLAMP(1.0 - d / fade, 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	};
	for (int tz = 0; tz < tiles_z; tz++) {
		for (int tx = 0; tx < tiles_x; tx++) {
			const LocalVector<float> &r = results[tz * tiles_x + tx];
			const int x0 = MAX(tx * core - apron, 0);
			const int z0 = MAX(tz * core - apron, 0);
			const int x1 = MIN((tx + 1) * core + apron, tile_nx - 1);
			const int z1 = MIN((tz + 1) * core + apron, tile_nz - 1);
			const int w = x1 - x0 + 1;
			// Edges of the world are no tile border: full weight there.
			const int lo_x = tx == 0 ? 0 : tx * core;
			const int hi_x = tx == tiles_x - 1 ? tile_nx - 1 : (tx + 1) * core;
			const int lo_z = tz == 0 ? 0 : tz * core;
			const int hi_z = tz == tiles_z - 1 ? tile_nz - 1 : (tz + 1) * core;
			for (int z = z0; z <= z1; z++) {
				const double wz = ramp(z, lo_z, hi_z);
				if (wz <= 0.0) {
					continue;
				}
				for (int x = x0; x <= x1; x++) {
					const double wt = wz * ramp(x, lo_x, hi_x);
					if (wt <= 0.0) {
						continue;
					}
					const int li = ((z - z0) * w + (x - x0)) * 3;
					const int gi = z * tile_nx + x;
					acc[gi * 3 + 0] += wt * r[li + 0];
					acc[gi * 3 + 1] += wt * r[li + 1];
					acc[gi * 3 + 2] += wt * r[li + 2];
					wsum[gi] += wt;
				}
			}
		}
	}
	tile_delta.resize(n);
	tile_flow.resize(n);
	tile_deposit.resize(n);
	for (int i = 0; i < n; i++) {
		const double inv = wsum[i] > 0.0 ? 1.0 / wsum[i] : 0.0;
		tile_delta[i] = float(acc[i * 3 + 0] * inv);
		tile_flow[i] = float(acc[i * 3 + 1] * inv);
		tile_deposit[i] = float(acc[i * 3 + 2] * inv);
	}
	last_tile_erosion_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

real_t TerrainGenerator3D::get_tile_flow(const Vector3 &p_global_position) const {
	return _tile_sample(tile_flow, p_global_position.x, p_global_position.z);
}

real_t TerrainGenerator3D::get_tile_deposit(const Vector3 &p_global_position) const {
	return _tile_sample(tile_deposit, p_global_position.x, p_global_position.z);
}

void TerrainGenerator3D::_fill_row(void *p_job, uint32_t p_row) {
	const FillJob *job = static_cast<const FillJob *>(p_job);
	float *row = job->out + size_t(p_row) * job->width;
	const real_t z = job->z0 + p_row * job->spacing;
	for (int x = 0; x < job->width; x++) {
		row[x] = job->gen->_ground(job->x0 + x * job->spacing, z);
	}
}

// Heights of a regular grid of world points, rows in parallel. Each point is computed on its own, so the result
// does not depend on the thread count.
void TerrainGenerator3D::_fill(real_t p_x0, real_t p_z0, real_t p_spacing, int p_width, int p_rows, float *r_out) const {
	FillJob job{ this, p_x0, p_z0, p_spacing, p_width, r_out };
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&TerrainGenerator3D::_fill_row, &job, p_rows, -1, true, SNAME("TerrainGenerator3D"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
}

real_t TerrainGenerator3D::get_height(const Vector3 &p_global_position) const {
	return _ground(p_global_position.x, p_global_position.z);
}

real_t TerrainGenerator3D::get_base_height(const Vector3 &p_global_position) const {
	return _sample(heights, p_global_position.x, p_global_position.z);
}

real_t TerrainGenerator3D::get_drainage_area(const Vector3 &p_global_position) const {
	return _sample(drainage, p_global_position.x, p_global_position.z);
}

real_t TerrainGenerator3D::get_sediment_depth(const Vector3 &p_global_position) const {
	return _sample(sediment, p_global_position.x, p_global_position.z);
}

Ref<Image> TerrainGenerator3D::_grid_image(const LocalVector<float> &p_grid) const {
	if (nx <= 0 || p_grid.size() != uint32_t(nx * ny)) {
		return Ref<Image>();
	}
	Vector<uint8_t> bytes;
	bytes.resize(nx * ny * sizeof(float));
	memcpy(bytes.ptrw(), p_grid.ptr(), bytes.size());
	return Image::create_from_data(nx, ny, false, Image::FORMAT_RF, bytes);
}

Ref<Image> TerrainGenerator3D::get_height_image() const {
	return _grid_image(heights);
}

// The final ground (simulated plus detail) on a regular grid of world points, rows in parallel: p_size points from
// p_origin (x, z) every p_spacing metres. NaN outside the generated rectangle.
Ref<Image> TerrainGenerator3D::sample_heights(const Vector2 &p_origin, real_t p_spacing, const Vector2i &p_size) const {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0 || p_size.x > 16384 || p_size.y > 16384, Ref<Image>());
	ERR_FAIL_COND_V(p_spacing <= 0.0, Ref<Image>());
	Vector<uint8_t> bytes;
	bytes.resize(size_t(p_size.x) * p_size.y * sizeof(float));
	_fill(p_origin.x, p_origin.y, p_spacing, p_size.x, p_size.y, reinterpret_cast<float *>(bytes.ptrw()));
	return Image::create_from_data(p_size.x, p_size.y, false, Image::FORMAT_RF, bytes);
}

Ref<Image> TerrainGenerator3D::get_drainage_image() const {
	return _grid_image(drainage);
}

Ref<Image> TerrainGenerator3D::get_sediment_image() const {
	return _grid_image(sediment);
}

Ref<Image> TerrainGenerator3D::get_uplift_image() const {
	return _grid_image(uplift_field);
}

/* Geology and rivers */

real_t TerrainGenerator3D::get_soil_depth(const Vector3 &p_global_position) const {
	return MAX(_sample(soil, p_global_position.x, p_global_position.z), 0.0f);
}

real_t TerrainGenerator3D::get_rock_exposure(const Vector3 &p_global_position) const {
	return CLAMP(_sample(rock, p_global_position.x, p_global_position.z), 0.0f, 1.0f);
}

real_t TerrainGenerator3D::get_talus(const Vector3 &p_global_position) const {
	return CLAMP(_sample(talus, p_global_position.x, p_global_position.z), 0.0f, 1.0f);
}

real_t TerrainGenerator3D::get_rock_hardness(const Vector3 &p_global_position) const {
	if (hardness.size() != uint32_t(nx * ny) || nx <= 0) {
		return 0.0;
	}
	return CLAMP(_sample(hardness, p_global_position.x, p_global_position.z), 0.0f, 1.0f);
}

// Places along the rivers for lakes and falls: every river point carrying at least p_min_discharge (m3/s, in
// river_month), with how far the river drops over the next p_run metres downstream. Each is a Dictionary: position
// (on the ground), discharge, area (drainage, m2), drop (m over p_run), run (m actually measured, shorter at a mouth),
// river and index (as in get_rivers(p_min_area)). Ordered by river, then downstream.
Array TerrainGenerator3D::get_river_sites(real_t p_min_area, real_t p_min_discharge, real_t p_run) const {
	Array out;
	if (heights.size() != uint32_t(nx * ny)) {
		return out;
	}
	const Array rivers = _rivers(p_min_area, true);
	const real_t run = MAX(p_run, real_t(0.0));
	for (int r = 0; r < rivers.size(); r++) {
		const Dictionary d = rivers[r];
		const PackedInt32Array cells = d["cells"];
		const PackedVector3Array pts = d["points"];
		const PackedFloat32Array areas = d["areas"];
		// Distance along the river from its first point, to find the point p_run further down.
		LocalVector<real_t> along;
		along.resize(pts.size());
		real_t acc = 0.0;
		for (int k = 0; k < pts.size(); k++) {
			acc += k > 0 ? Vector2(pts[k].x - pts[k - 1].x, pts[k].z - pts[k - 1].z).length() : 0.0;
			along[k] = acc;
		}
		int e = 0;
		for (int k = 0; k < cells.size() && k < pts.size(); k++) {
			const float q = _flow(cells[k], river_month);
			if (q < p_min_discharge) {
				continue;
			}
			e = MAX(e, k);
			while (e + 1 < pts.size() && along[e] - along[k] < run) {
				e++;
			}
			const Vector3 top(pts[k].x, _ground(pts[k].x, pts[k].z), pts[k].z);
			const real_t below = _ground(pts[e].x, pts[e].z);
			Dictionary w;
			w["position"] = top;
			w["discharge"] = q;
			w["area"] = k < areas.size() ? real_t(areas[k]) : real_t(0.0);
			w["drop"] = top.y - below;
			w["run"] = along[e] - along[k];
			w["river"] = r;
			w["index"] = k;
			out.push_back(w);
		}
	}
	return out;
}

// Waterfalls along the rivers: runs of the channel steeper than 45 degrees in the eroded heights (soft rock stands at
// critical_slope at most, so only hard rock makes them) that drop at least p_min_height. Each is a Dictionary: top
// and bottom (the lip and the foot, on the ground), height, width and discharge (of the river at the lip), river
// (index in get_rivers(p_min_area)), index (of the lip in that river's points) and hardness (of the rock at the lip).
Array TerrainGenerator3D::get_waterfalls(real_t p_min_area, real_t p_min_height) const {
	Array out;
	if (heights.size() != uint32_t(nx * ny)) {
		return out;
	}
	const Array rivers = _rivers(p_min_area, true);
	for (int r = 0; r < rivers.size(); r++) {
		const Dictionary d = rivers[r];
		const PackedInt32Array cells = d["cells"];
		const PackedVector3Array pts = d["points"];
		const PackedFloat32Array widths = d["widths"];
		const PackedFloat32Array areas = d["areas"];
		auto steep = [&](int a) {
			const int u = cells[a], v = cells[a + 1];
			const real_t run = result_cell * Math::sqrt(real_t((u % nx - v % nx) * (u % nx - v % nx) + (u / nx - v / nx) * (u / nx - v / nx)));
			return heights[u] - heights[v] > run;
		};
		int k = 0;
		while (k + 1 < cells.size()) {
			if (!steep(k)) {
				k++;
				continue;
			}
			int e = k;
			while (e + 1 < cells.size() && steep(e)) {
				e++;
			}
			Vector3 top = Vector3(_cell_position(cells[k]).x, _ground(_cell_position(cells[k]).x, _cell_position(cells[k]).z), _cell_position(cells[k]).z);
			Vector3 bottom = Vector3(_cell_position(cells[e]).x, _ground(_cell_position(cells[e]).x, _cell_position(cells[e]).z), _cell_position(cells[e]).z);
			// A lip worn back up a gorge: the fall is at the gorge's head, from its brink to the slot floor below.
			for (const FallLip &l : lips) {
				if (l.origin.distance_to(Vector2(top.x, top.z)) < 0.5f * result_cell && l.top.distance_to(l.origin) > 0.5f) {
					const Vector2 brink = l.top + l.dir * (0.5f * l.run - 0.5f * LIP_FACE - 0.5f);
					const Vector2 foot = l.top + l.dir * (0.5f * l.run + 0.5f * LIP_FACE + 1.0f);
					top = Vector3(brink.x, _ground(brink.x, brink.y), brink.y);
					bottom = Vector3(foot.x, _ground(foot.x, foot.y), foot.y);
					break;
				}
			}
			const real_t drop = heights[cells[k]] - heights[cells[e]];
			if (drop >= p_min_height) {
				Dictionary w;
				w["top"] = top;
				w["bottom"] = bottom;
				w["height"] = top.y - bottom.y;
				w["width"] = widths[k];
				w["discharge"] = _flow(cells[k], river_month);
				w["river"] = r;
				w["index"] = k;
				w["hardness"] = get_rock_hardness(top);
				// Free fall: the jet leaving the brink at critical depth (v = (g q)^(1/3), q per metre of width) clears
				// the face over the drop when it travels further out than the face recedes.
				const real_t q = real_t(w["discharge"]) / MAX(widths[k], 0.5f);
				const real_t v = Math::pow(9.81f * MAX(q, real_t(0.0)), real_t(1.0 / 3.0));
				const real_t hgt = MAX(top.y - bottom.y, real_t(0.0));
				const real_t throw_m = v * Math::sqrt(2.0f * hgt / 9.81f);
				bool lipped = false;
				const Vector3 cell_top = _cell_position(cells[k]);
				for (const FallLip &l : lips) {
					lipped = lipped || (l.origin.distance_to(Vector2(cell_top.x, cell_top.z)) < 0.5f * result_cell);
				}
				const real_t face = lipped ? LIP_FACE : Vector2(top.x - bottom.x, top.z - bottom.z).length();
				w["face_run"] = face;
				w["jet_throw"] = throw_m;
				w["free_fall"] = throw_m > face;
				_fall_contract(w, pts, k, e, cells, widths[k], v);
				// A step drowned by a lake or the sea is no fall: its brink is under standing water.
				const Vector3 brink = w["top"];
				const real_t wl = get_water_level(brink);
				// Also no fall once the brink sits in the channel: a cell on a slot's edge measured the wall, not the bed.
				if ((Math::is_nan(wl) || wl <= brink.y + 0.5f) && real_t(w["height"]) >= p_min_height) {
					out.push_back(w);
				}
			}
			k = e;
		}
	}
	return out;
}

// The rest of a fall's record for the water code (docs/ideas/terrain-generation.md, "Fall data for the water"):
// the lip across its width with each part's share of the flow, the face down three lines, its tiers and ledges, the
// form, power, impact speed, the flow by month and a stable id.
void TerrainGenerator3D::_fall_contract(Dictionary &r_fall, const PackedVector3Array &p_pts, int p_k, int p_e, const PackedInt32Array &p_cells, real_t p_width, real_t p_v0) const {
	Vector3 top = r_fall["top"];
	const Vector3 bottom = r_fall["bottom"];
	real_t height = MAX(real_t(r_fall["height"]), real_t(0.0));
	const real_t discharge = r_fall["discharge"];
	Vector2 dir = Vector2(bottom.x - top.x, bottom.z - top.z);
	real_t span = dir.length();
	dir = span > 1e-4 ? dir / span : Vector2(0, 1);
	// The river's cell can sit on the edge of a slot narrower than the grid (a gorge, a cascade in a cleft): the brink
	// is the channel, the lowest ground across the flow within a channel width each side, so `top` moves there.
	const real_t lw0 = MAX(p_width, real_t(2.0));
	{
		const Vector2 ac(-dir.y, dir.x);
		Vector2 best(top.x, top.z);
		real_t best_g = _ground(top.x, top.z);
		for (int i = -16; i <= 16; i++) {
			const Vector2 q = Vector2(top.x, top.z) + ac * (real_t(i) / 16.0f * lw0);
			const real_t g = _ground(q.x, q.y);
			if (g < best_g - 0.01f) {
				best_g = g;
				best = q;
			}
		}
		if (best != Vector2(top.x, top.z)) {
			top = Vector3(best.x, best_g, best.y);
			height = MAX(top.y - bottom.y, real_t(0.0));
			r_fall["top"] = top;
			r_fall["height"] = height;
			dir = Vector2(bottom.x - top.x, bottom.z - top.z);
			span = dir.length();
			dir = span > 1e-4 ? dir / span : Vector2(0, 1);
		}
	}
	const Vector2 across(-dir.y, dir.x);
	r_fall["direction"] = Vector3(dir.x, 0, dir.y);
	// Stable id: the river and the distance down it to the lip (the same for the same seed and settings).
	real_t along = 0.0;
	for (int i = 1; i <= p_k && i < p_pts.size(); i++) {
		along += Vector2(p_pts[i].x - p_pts[i - 1].x, p_pts[i].z - p_pts[i - 1].z).length();
	}
	r_fall["id"] = vformat("r%d_%dm", int(r_fall["river"]), int(Math::round(along)));
	// The lip: 8 points across the channel at the brink, the ground under each, and each one's share of the flow by
	// Manning's law across the section (unit discharge goes as depth^(5/3) under a common water surface; the surface
	// is set by the river's width at the lip, so the deepest part of the brink carries most).
	real_t lw = lw0;
	// Water surface: the brink's lowest point plus the critical depth of the whole flow over the width.
	const real_t yc = Math::pow(MAX(discharge, real_t(0.0)) / lw, real_t(2.0 / 3.0)) / Math::pow(real_t(9.81), real_t(1.0 / 3.0));
	const real_t surface = top.y + MAX(yc, real_t(0.02));
	// The lip spans the wetted width only: from the channel out each way until the ground rises above the water
	// (at most half the river's width), so no lip point climbs the slot's walls.
	real_t ext[2] = { 0.0, 0.0 };
	for (int sgn = 0; sgn < 2; sgn++) {
		const real_t sd = sgn == 0 ? -1.0f : 1.0f;
		const real_t step = 0.5f * lw0 / 16.0f;
		for (int i = 1; i <= 16; i++) {
			const Vector2 q = Vector2(top.x, top.z) + across * (sd * step * i);
			if (_ground(q.x, q.y) > surface) {
				break;
			}
			ext[sgn] = step * i;
		}
	}
	const real_t wet_w = ext[0] + ext[1];
	const real_t centre = 0.5f * (ext[1] - ext[0]);
	// At least half the river's width: the grid's V-shaped bed is narrower than the channel it stands for.
	lw = MAX(wet_w, real_t(0.5) * lw0);
	PackedVector3Array lip;
	PackedFloat32Array share;
	for (int i = 0; i < 8; i++) {
		const real_t o = centre + (real_t(i) / 7.0 - 0.5) * lw;
		const Vector2 q = Vector2(top.x, top.z) + across * o;
		const real_t g = _ground(q.x, q.y);
		lip.push_back(Vector3(q.x, g, q.y));
	}
	real_t total = 0.0;
	for (int i = 0; i < 8; i++) {
		const real_t dpt = MAX(surface - lip[i].y, real_t(0.0));
		const real_t sh = Math::pow(dpt, real_t(5.0 / 3.0));
		share.push_back(sh);
		total += sh;
	}
	for (int i = 0; i < 8; i++) {
		share.set(i, total > 0.0 ? share[i] / total : real_t(1.0 / 8.0));
	}
	r_fall["lip_points"] = lip;
	r_fall["lip_share"] = share;
	r_fall["lip_width"] = lw;
	r_fall["overhang"] = 0.0; // a heightfield cannot undercut; the free-fall test above stands in for it
	// The hard bed the lip stands on: a strata band's thickness, or none on ordinary rock.
	r_fall["caprock"] = (strata_enabled && get_rock_hardness(top) >= 0.5f) ? real_t(strata_spacing * strata_hard_fraction) : real_t(0.0);
	// Face profiles down three lines (left, centre, right of the lip): 16 samples of (run from the lip, height
	// below it, overhang flag 0) from the brink to a little past the foot.
	const real_t reach = span + 8.0f;
	Array faces;
	for (int line = 0; line < 3; line++) {
		const real_t o = (real_t(line) - 1.0f) * 0.35f * lw;
		PackedVector3Array prof;
		for (int i = 0; i < 16; i++) {
			const real_t run = reach * real_t(i) / 15.0f;
			const Vector2 q = Vector2(top.x, top.z) + across * o + dir * run;
			prof.push_back(Vector3(run, _ground(q.x, q.y) - top.y, 0.0));
		}
		faces.push_back(prof);
	}
	r_fall["face_profiles"] = faces;
	// Tiers and ledges down the centre line at 1 m: a ledge is a stretch at most 0.35 steep (19 degrees) at least
	// 2 m long between steeper drops of 2 m or more; the drops between ledges are the tiers.
	Array tiers, ledges;
	{
		const int steps = MAX(2, int(Math::ceil(reach)));
		real_t prev = top.y;
		real_t tier_top = top.y;
		Vector3 tier_top_p = top;
		real_t flat = 0.0;
		Vector3 flat_start;
		for (int i = 1; i <= steps; i++) {
			const real_t run = reach * real_t(i) / steps;
			const Vector2 q = Vector2(top.x, top.z) + dir * run;
			const real_t g = _ground(q.x, q.y);
			const real_t dz = prev - g;
			const real_t dx = reach / steps;
			if (dz <= 0.35f * dx) {
				if (flat == 0.0) {
					flat_start = Vector3(q.x, g, q.y);
				}
				flat += dx;
			} else {
				if (flat >= 2.0f && tier_top - flat_start.y >= 2.0f && run < span) {
					Dictionary t;
					t["top"] = tier_top_p;
					t["bottom"] = flat_start;
					t["height"] = tier_top - flat_start.y;
					tiers.push_back(t);
					Dictionary l;
					l["position"] = flat_start;
					l["depth"] = flat;
					ledges.push_back(l);
					tier_top = flat_start.y;
					tier_top_p = flat_start;
				}
				flat = 0.0;
			}
			prev = g;
		}
		if (tier_top - bottom.y >= 0.5f) {
			Dictionary t;
			t["top"] = tier_top_p;
			t["bottom"] = bottom;
			t["height"] = tier_top - bottom.y;
			tiers.push_back(t);
		}
	}
	r_fall["tiers"] = tiers;
	r_fall["ledges"] = ledges;
	// Form (the World Waterfall Database's names): free of the face - plunge; several drops - tiered; down rock
	// steeper than 60 degrees - horsetail; gentler - cascade.
	const real_t face_slope = span > 0.0 ? height / span : 1e3f;
	String type;
	if (tiers.size() >= 2) {
		type = "tiered";
	} else if (bool(r_fall["free_fall"])) {
		type = "plunge";
	} else if (face_slope >= 1.73f) {
		type = "horsetail";
	} else {
		type = "cascade";
	}
	r_fall["type"] = type;
	r_fall["power"] = 1000.0f * 9.81f * discharge * height;
	// Impact speed. A falling sheet breaks up after L_b = 6 q^0.32 m (Horeni 1956, q in m2/s per metre of lip); past
	// that it falls as drops and blobs that approach their terminal speed (about 9 m/s for drops of 3 mm and more,
	// Gunn and Kinzer 1949) over a drag length v_t^2 / g. impact_speed_free is the drag-free sqrt(v0^2 + 2 g H).
	const real_t qw = MAX(discharge / lw, real_t(1e-4));
	const real_t lb = 6.0f * Math::pow(qw, real_t(0.32));
	const real_t vfree = Math::sqrt(p_v0 * p_v0 + 2.0f * 9.81f * height);
	const real_t vt = 9.0f;
	real_t vimp = vfree;
	if (height > lb) {
		const real_t vb = Math::sqrt(p_v0 * p_v0 + 2.0f * 9.81f * lb);
		vimp = vt + (MAX(vb, vt) - vt) * Math::exp(-(height - lb) / (vt * vt / 9.81f));
	}
	r_fall["breakup_length"] = lb;
	r_fall["impact_speed"] = MIN(vimp, vfree);
	r_fall["impact_speed_free"] = vfree;
	PackedFloat32Array months;
	for (int m = 0; m < 12; m++) {
		months.push_back(_flow(p_cells[p_k], m));
	}
	r_fall["discharge_months"] = months;
	// The plunge pool baked for this fall (none where it lands in a lake or the sea): centre on the floor, radius,
	// depth below the foot, and the level it holds water to (the lowest point of its rim).
	const Vector3 origin = _cell_position(p_cells[p_k]);
	for (const PlungePool &pl : pools) {
		if (pl.top.distance_to(Vector2(origin.x, origin.z)) < 0.5f * result_cell) {
			// The water spills over the lowest point of the rim's crest: the highest ground along each radius
			// from the edge of the hole out over the tail bar, lowest over all directions.
			real_t level = 1e30f;
			for (int i = 0; i < 32; i++) {
				const real_t an = Math::TAU * real_t(i) / 32.0f;
				real_t crest = -1e30f;
				for (int j = 0; j <= 8; j++) {
					const real_t rr = pl.radius * (0.9f + (POOL_BAR - 0.9f) * real_t(j) / 8.0f);
					const real_t gh = _ground(pl.center.x + Math::cos(an) * rr, pl.center.y + Math::sin(an) * rr);
					if (Math::is_finite(gh)) {
						crest = MAX(crest, gh);
					}
				}
				level = MIN(level, crest);
			}
			r_fall["pool_center"] = Vector3(pl.center.x, _ground(pl.center.x, pl.center.y), pl.center.y);
			r_fall["pool_radius"] = pl.radius;
			r_fall["pool_depth"] = MAX(level - _ground(pl.center.x, pl.center.y), real_t(0.0));
			r_fall["pool_level"] = level;
			break;
		}
	}
	// Where the fall lands: "pool", "lake" or "sea" (a fall into standing water gets no pool of its own).
	const int wb = water_body.size() == uint32_t(nx * ny) ? water_body[p_cells[p_e]] : 0;
	r_fall["lands_in"] = r_fall.has("pool_radius") ? String("pool") : (wb == 1 ? String("sea") : (wb >= 2 ? String("lake") : String("ground")));
}

/* Rain and snowmelt (terrain_runoff.h; docs/ideas/terrain-generation.md "Rain and snowmelt feed the rivers") */

namespace {
constexpr int RUNOFF_STRIDE = 28; // 12 runoff, 12 snow, precipitation, shortwave, winter shortwave, surface
constexpr int RUNOFF_MIN_CELLS = 8;
} // namespace

// The climate on a coarse grid of the result (about the recipe's sun_cell), the sea as water, then each cell's
// monthly water balance.
void TerrainGenerator3D::_bake_runoff(const LocalVector<uint8_t> &p_sea) {
	runoff_fields.clear();
	runoff_n = 0;
	runoff_mean = 0.0;
	runoff_msec = 0.0;
	flow_index.clear();
	flows.clear();
	if (climate_recipe.is_null() || nx < 2 || ny < 2) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const TerrainClimate::Params &cp = climate_recipe->get_params();
	const int f = MAX(1, int(Math::round(climate_recipe->get_sun_cell() / cell_size)));
	const int N = MAX((nx + f - 1) / f, (ny + f - 1) / f);
	const float cell = float(cell_size) * f;
	std::vector<float> surf(N * N);
	std::vector<uint8_t> sea(N * N);
	for (int cz = 0; cz < N; cz++) {
		for (int cx = 0; cx < N; cx++) {
			double sum = 0.0;
			int count = 0, wet = 0;
			for (int z = cz * f; z < (cz + 1) * f; z++) {
				for (int x = cx * f; x < (cx + 1) * f; x++) {
					const int i = MIN(z, ny - 1) * nx + MIN(x, nx - 1);
					const bool s = p_sea.size() == uint32_t(nx * ny) && p_sea[i];
					sum += s ? MAX(heights[i], result_min) : heights[i];
					wet += s ? 1 : 0;
					count++;
				}
			}
			surf[cz * N + cx] = float(sum / count);
			sea[cz * N + cx] = 2 * wet > count;
		}
	}
	std::vector<float> precip(N * N), sw(N * N), sww(N * N), sky(N * N), hz;
	TerrainClimate::precipitation_on(cp, surf.data(), N, cell, climate_recipe->get_precipitation_cell(), precip.data());
	TerrainClimate::horizons(cp, surf.data(), N, cell, hz);
	TerrainClimate::shortwave(cp, surf.data(), N, cell, hz, sw.data(), sww.data(), sky.data());
	runoff_fields.resize(N * N * RUNOFF_STRIDE);
	float *rf = runoff_fields.ptr();
	TerrainClimate::parallel_for(N, [&](int z) {
		for (int x = 0; x < N; x++) {
			const int c = z * N + x;
			float *o = rf + c * RUNOFF_STRIDE;
			TerrainRunoff::Cell b;
			if (sea[c]) {
				for (int m = 0; m < 12; m++) {
					b.runoff[m] = 0.0f;
					b.snow[m] = 0.0f;
				}
			} else {
				TerrainRunoff::balance(cp, surf[c], precip[c], sw[c], sww[c], -1.0f, b);
			}
			for (int m = 0; m < 12; m++) {
				o[m] = b.runoff[m];
				o[12 + m] = b.snow[m];
			}
			o[24] = precip[c];
			o[25] = sw[c];
			o[26] = sww[c];
			o[27] = surf[c];
		}
	});
	double sum = 0.0;
	int land = 0;
	for (int c = 0; c < N * N; c++) {
		if (!sea[c]) {
			for (int m = 0; m < 12; m++) {
				sum += rf[c * RUNOFF_STRIDE + m];
			}
			land++;
		}
	}
	runoff_mean = land > 0 ? sum / land : 0.0;
	runoff_n = N;
	runoff_f = f;
	runoff_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

// Bilinear between coarse cell centres (field 0..RUNOFF_STRIDE-1).
float TerrainGenerator3D::_runoff_sample(int p_field, real_t p_x, real_t p_z) const {
	if (runoff_n <= 0) {
		return 0.0f;
	}
	const int N = runoff_n;
	const real_t half = 0.5 * (runoff_f - 1);
	const real_t gx = CLAMP(((p_x - result_rect.position.x) / result_cell - half) / runoff_f, real_t(0.0), real_t(N - 1));
	const real_t gz = CLAMP(((p_z - result_rect.position.y) / result_cell - half) / runoff_f, real_t(0.0), real_t(N - 1));
	const int x0 = MIN(int(gx), N - 1), z0 = MIN(int(gz), N - 1);
	const int x1 = MIN(x0 + 1, N - 1), z1 = MIN(z0 + 1, N - 1);
	const float fx = float(gx - x0), fz = float(gz - z0);
	auto at = [&](int x, int z) { return runoff_fields[(z * N + x) * RUNOFF_STRIDE + p_field]; };
	return Math::lerp(Math::lerp(at(x0, z0), at(x1, z0), fx), Math::lerp(at(x0, z1), at(x1, z1), fx), fz);
}

// Each month's runoff accumulated down the rivers' routing (lake cells drain to their outlets and lose their open
// water evaporation; the sea takes everything). Channel cells keep their twelve monthly discharges.
void TerrainGenerator3D::_route_runoff(const LocalVector<int32_t> &p_receivers, const LocalVector<int32_t> &p_order) {
	flow_index.clear();
	flows.clear();
	if (runoff_n <= 0) {
		return;
	}
	const int n = nx * ny;
	const double ca = double(cell_size) * cell_size;
	flow_index.resize(n);
	int kept = 0;
	for (int i = 0; i < n; i++) {
		flow_index[i] = river_area[i] >= RUNOFF_MIN_CELLS * ca ? kept++ : -1;
	}
	flows.resize(kept * 12);
	// Lake cells: open water (rain and melt less the lake's evaporation), at the lake's level.
	const TerrainClimate::Params &cp = climate_recipe->get_params();
	LocalVector<int32_t> lake_cells;
	for (int i = 0; i < n; i++) {
		if (water_body.size() == uint32_t(n) && water_body[i] >= 2) {
			lake_cells.push_back(i);
		}
	}
	LocalVector<float> lake_runoff;
	lake_runoff.resize(lake_cells.size() * 12);
	TerrainClimate::parallel_for(int(lake_cells.size()), [&](int k) {
		const int i = lake_cells[k];
		const Vector3 p = _cell_position(i);
		TerrainRunoff::Cell b;
		TerrainRunoff::balance(cp, lake_info[water_body[i] - 2].level, _runoff_sample(24, p.x, p.z), _runoff_sample(25, p.x, p.z), _runoff_sample(26, p.x, p.z), float(lake_evaporation), b);
		for (int m = 0; m < 12; m++) {
			lake_runoff[k * 12 + m] = b.runoff[m];
		}
	});
	LocalVector<int32_t> lake_slot;
	lake_slot.resize(n);
	for (int i = 0; i < n; i++) {
		lake_slot[i] = -1;
	}
	for (uint32_t k = 0; k < lake_cells.size(); k++) {
		lake_slot[lake_cells[k]] = int32_t(k);
	}
	LocalVector<double> acc;
	acc.resize(n);
	for (int m = 0; m < 12; m++) {
		const double to_flow = 1.0e-3 * ca / (TerrainRunoff::DAYS[m] * 86400.0);
		TerrainClimate::parallel_for(ny, [&](int z) {
			for (int x = 0; x < nx; x++) {
				const int i = z * nx + x;
				const int32_t b = water_body.size() == uint32_t(n) ? water_body[i] : 0;
				double mm;
				if (b == 1) {
					mm = 0.0;
				} else if (b >= 2) {
					mm = lake_runoff[lake_slot[i] * 12 + m];
				} else {
					const Vector3 p = _cell_position(i);
					mm = _runoff_sample(m, p.x, p.z);
				}
				// Land beyond the map draining in here gets the same rain and melt as the node it enters at.
				acc[i] = mm * to_flow * (inflow_cells.is_empty() ? 1.0 : 1.0 + double(inflow_cells[i]) / ca);
			}
		});
		for (uint32_t q = 0; q < p_order.size(); q++) {
			const int32_t i = p_order[q];
			const int32_t r = p_receivers[i];
			if (r != i) {
				acc[r] += acc[i];
			}
		}
		for (int i = 0; i < n; i++) {
			if (flow_index[i] >= 0) {
				flows[flow_index[i] * 12 + m] = float(MAX(acc[i], 0.0));
			}
		}
	}
}

// Discharge (m^3/s) of a cell in a month (-1: the annual mean). Without a climate, river_runoff times the area.
float TerrainGenerator3D::_flow(int p_cell, int p_month) const {
	if (p_cell < 0 || p_cell >= int(river_area.size())) {
		return 0.0f;
	}
	if (flow_index.is_empty()) {
		return float(get_discharge(river_area[p_cell]));
	}
	const int k = flow_index[p_cell];
	if (k < 0) {
		// A small headwater: its own area at the local runoff.
		const Vector3 p = _cell_position(p_cell);
		if (p_month >= 0) {
			return float(MAX(_runoff_sample(p_month, p.x, p.z), 0.0f) * 1.0e-3 * river_area[p_cell] / (TerrainRunoff::DAYS[p_month] * 86400.0));
		}
		float year = 0.0f;
		for (int m = 0; m < 12; m++) {
			year += _runoff_sample(m, p.x, p.z);
		}
		return float(MAX(year, 0.0f) * 1.0e-3 * river_area[p_cell] / 31536000.0);
	}
	if (p_month >= 0) {
		return flows[k * 12 + p_month];
	}
	double sum = 0.0;
	for (int m = 0; m < 12; m++) {
		sum += double(flows[k * 12 + m]) * TerrainRunoff::DAYS[m];
	}
	return float(sum / 365.0);
}

real_t TerrainGenerator3D::get_runoff_at(const Vector3 &p_global_position, int p_month) const {
	if (runoff_n <= 0) {
		return p_month < 0 ? river_runoff : river_runoff * TerrainRunoff::DAYS[CLAMP(p_month, 0, 11)] / 365.0;
	}
	if (p_month >= 0) {
		return _runoff_sample(CLAMP(p_month, 0, 11), p_global_position.x, p_global_position.z);
	}
	real_t year = 0.0;
	for (int m = 0; m < 12; m++) {
		year += _runoff_sample(m, p_global_position.x, p_global_position.z);
	}
	return year;
}

real_t TerrainGenerator3D::get_snow_water_at(const Vector3 &p_global_position, int p_month) const {
	if (runoff_n <= 0) {
		return 0.0;
	}
	return MAX(_runoff_sample(12 + CLAMP(p_month, 0, 11), p_global_position.x, p_global_position.z), 0.0f);
}

real_t TerrainGenerator3D::get_discharge_at(const Vector3 &p_global_position, int p_month) const {
	if (nx < 2 || river_area.size() != uint32_t(nx * ny)) {
		return 0.0;
	}
	const int x = CLAMP(int(Math::round((p_global_position.x - result_rect.position.x) / result_cell)), 0, nx - 1);
	const int z = CLAMP(int(Math::round((p_global_position.z - result_rect.position.y) / result_cell)), 0, ny - 1);
	return _flow(z * nx + x, CLAMP(p_month, -1, 11));
}

Color TerrainGenerator3D::get_geology_at(const Vector3 &p_global_position) const {
	const real_t a = _sample(drainage, p_global_position.x, p_global_position.z);
	return Color(get_soil_depth(p_global_position), get_rock_exposure(p_global_position), get_talus(p_global_position), std::log10(double(MAX(a, real_t(1.0)))));
}

// RGBA float: soil depth (m), rock exposure, talus (or -1..0 moraine till), log10 drainage area (m^2).
Ref<Image> TerrainGenerator3D::get_geology_image() const {
	const int n = nx * ny;
	if (nx <= 0 || soil.size() != uint32_t(n)) {
		return Ref<Image>();
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * 4 * sizeof(float));
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	for (int i = 0; i < n; i++) {
		// Sand where the coast made a beach, as a negative soil depth (-1 on the beach face, fading to 0 behind it),
		// so materials put sand on beaches and not on every low coastal plain.
		w[i * 4 + 0] = (beach.size() == uint32_t(n) && beach[i] > 0.0f) ? -beach[i] : soil[i];
		w[i * 4 + 1] = rock[i];
		// Moraine till (moraine_age) as a negative talus, -1..0, where it is more than the scree: unsorted stony
		// debris under a thin soil, which the material shows as soil with stones through it.
		const float till = till_cover.size() == uint32_t(n) ? CLAMP(till_cover[i] / 2.0f, 0.0f, 1.0f) : 0.0f;
		w[i * 4 + 2] = till > talus[i] ? -till : talus[i];
		w[i * 4 + 3] = float(std::log10(MAX(double(drainage[i]), 1.0)));
	}
	return Image::create_from_data(nx, ny, false, Image::FORMAT_RGBAF, bytes);
}

void TerrainGenerator3D::_publish_globals() {
	if (!publish_shader_globals || geology_texture.is_null() || !is_inside_tree()) {
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
	// The texels are grid nodes: the rectangle runs from the first node centre to the last.
	const Rect2 &r = result_rect;
	set_global("terrain_geology_map", geology_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	set_global("terrain_geology_rect", Vector4(r.position.x, r.position.y, (nx - 1) * result_cell, (ny - 1) * result_cell), RSE::GLOBAL_VAR_TYPE_VEC4);
	if (detail_keep_texture.is_valid()) {
		set_global("terrain_detail_keep_map", detail_keep_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
	}
	// Ice-worn rock for the detail normal; a zero rect switches it off.
	if (ice_flow_texture.is_valid()) {
		set_global("terrain_ice_flow", ice_flow_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		set_global("terrain_ice_flow_rect", Vector4(r.position.x, r.position.y, (nx - 1) * result_cell, (ny - 1) * result_cell), RSE::GLOBAL_VAR_TYPE_VEC4);
	} else {
		set_global("terrain_ice_flow_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
	}
	if (spray_texture.is_valid()) {
		set_global("terrain_spray_potential", spray_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2D);
		set_global("terrain_spray_rect", Vector4(r.position.x, r.position.y, (spray_nx - 1) * spray_cell, (spray_ny - 1) * spray_cell), RSE::GLOBAL_VAR_TYPE_VEC4);
	}
	// Sky occlusion for the ground's ambient light; a zero rect switches it off.
	if (sky_texture.is_valid()) {
		set_global("terrain_sky_map", sky_texture->get_rid(), RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY);
		set_global("terrain_sky_rect", Vector4(r.position.x, r.position.y, (sky_nx - 1) * sky_cell, (sky_ny - 1) * sky_cell), RSE::GLOBAL_VAR_TYPE_VEC4);
		set_global("terrain_sky_full", Vector4(sky_full[0], sky_full[1], sky_full[2], sky_full[3]), RSE::GLOBAL_VAR_TYPE_VEC4);
		set_global("terrain_sky_full2", Vector4(sky_full[4], sky_full[5], sky_full[6], sky_full[7]), RSE::GLOBAL_VAR_TYPE_VEC4);
		set_global("terrain_sky_full3", sky_full[8], RSE::GLOBAL_VAR_TYPE_FLOAT);
	} else {
		set_global("terrain_sky_rect", Vector4(), RSE::GLOBAL_VAR_TYPE_VEC4);
	}
}

/* Boulders below cliffs (docs/ideas/terrain-generation.md, "Rocks below cliffs") */

namespace {
inline uint32_t boulder_hash(uint32_t p_a, uint32_t p_b, uint32_t p_c) {
	uint32_t h = p_a * 0x9E3779B1u ^ (p_b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (p_c + 0x165667B1u) * 0xC2B2AE35u;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}
inline real_t boulder_unit(uint32_t p_h) {
	return real_t(p_h >> 8) / real_t(16777216.0);
}
} // namespace

// Blocks that fell off the cliffs and where they came to rest: each rock cell steeper than 45 degrees sheds blocks in
// proportion to its exposed face; each block rolls down the steepest way until its travel angle (from where it left
// the face) drops below the shadow angle (27.5 degrees at the least, Evans and Hungr 1993), larger blocks running
// further (fall sorting, Kirkby and Statham 1975). Sizes follow a power law (Pareto, exponent 2.5) from 0.25 to 3 m
// in radius. Deterministic for the seed; returns (x, ground y, z, radius) for blocks resting inside p_rect.
PackedVector4Array TerrainGenerator3D::get_boulders(const Rect2 &p_rect, real_t p_density) const {
	PackedVector4Array out;
	const int n = nx * ny;
	if (n <= 0 || heights.size() != uint32_t(n) || p_density <= 0.0) {
		return out;
	}
	const real_t margin = 400.0;
	const Rect2 src = p_rect.grow(margin);
	const int x0 = CLAMP(int(Math::floor((src.position.x - result_rect.position.x) / result_cell)), 1, nx - 2);
	const int x1 = CLAMP(int(Math::ceil((src.position.x + src.size.x - result_rect.position.x) / result_cell)), 1, nx - 2);
	const int z0 = CLAMP(int(Math::floor((src.position.y - result_rect.position.y) / result_cell)), 1, ny - 2);
	const int z1 = CLAMP(int(Math::ceil((src.position.y + src.size.y - result_rect.position.y) / result_cell)), 1, ny - 2);
	const bool has_rock = rock.size() == uint32_t(n);
	const bool has_water = water_body.size() == uint32_t(n);
	for (int z = z0; z <= z1; z++) {
		for (int x = x0; x <= x1; x++) {
			const int i = z * nx + x;
			const real_t gx = (heights[i + 1] - heights[i - 1]) / (2.0f * result_cell);
			const real_t gz = (heights[i + nx] - heights[i - nx]) / (2.0f * result_cell);
			const real_t slope = Math::sqrt(gx * gx + gz * gz);
			const real_t rk = has_rock ? rock[i] : real_t(slope > 1.0f ? 1.0f : 0.0f);
			if (slope < 1.0f || rk < 0.3f || (has_water && water_body[i] != 0)) {
				continue;
			}
			// Face area in the cell times exposure; about one block per 150 m2 of bare face at density 1.
			const real_t face = result_cell * result_cell * Math::sqrt(1.0f + slope * slope) * rk;
			const real_t want = face / 150.0f * p_density;
			int count = int(want);
			if (boulder_unit(boulder_hash(uint32_t(seed), uint32_t(i), 9999u)) < want - count) {
				count++;
			}
			for (int b = 0; b < count; b++) {
				const uint32_t h0 = boulder_hash(uint32_t(seed), uint32_t(i), uint32_t(b) * 4u);
				const real_t u1 = boulder_unit(h0);
				const real_t u2 = boulder_unit(boulder_hash(uint32_t(seed), uint32_t(i), uint32_t(b) * 4u + 1u));
				const real_t u3 = boulder_unit(boulder_hash(uint32_t(seed), uint32_t(i), uint32_t(b) * 4u + 2u));
				const real_t u4 = boulder_unit(boulder_hash(uint32_t(seed), uint32_t(i), uint32_t(b) * 4u + 3u));
				// Pareto radius, alpha 2.5, 0.25 .. 3 m.
				const real_t radius = MIN(real_t(0.25) * Math::pow(1.0f - u3 * 0.999f, real_t(-1.0 / 2.5)), real_t(3.0));
				const real_t shadow = Math::deg_to_rad(real_t(27.5) + real_t(6.0) * u4 - real_t(2.0) * (radius - 0.25f));
				const real_t tan_shadow = Math::tan(MAX(shadow, Math::deg_to_rad(real_t(22.0))));
				const Vector3 c = _cell_position(i);
				Vector2 p(c.x + (u1 - 0.5f) * result_cell, c.z + (u2 - 0.5f) * result_cell);
				// The run-out follows the grid's surface (bilinear; the 1 m detail only steers a block locally), in 3 m
				// steps; the resting height comes from the full ground.
				auto surf = [&](real_t px, real_t pz) { return real_t(_sample(heights, px, pz)); };
				const real_t start = surf(p.x, p.y);
				real_t travelled = 0.0;
				real_t h = start;
				bool lost = false;
				for (int step = 0; step < 200; step++) {
					const real_t e = 4.0;
					const Vector2 grad((surf(p.x + e, p.y) - surf(p.x - e, p.y)) / (2.0f * e), (surf(p.x, p.y + e) - surf(p.x, p.y - e)) / (2.0f * e));
					const real_t gl = grad.length();
					if (gl < 1e-3f) {
						break;
					}
					// Blocks bounce and spin off the fall line: up to 35 degrees either side each step, so they fan out
					// over the talus instead of all funnelling into the gully below.
					const real_t jit = (boulder_unit(boulder_hash(h0, uint32_t(step), 77u)) - 0.5f) * Math::deg_to_rad(real_t(70.0));
					const Vector2 np = p - (grad / gl).rotated(jit) * 3.0f;
					const real_t nh = surf(np.x, np.y);
					if (!Math::is_finite(nh) || nh >= h) {
						break;
					}
					travelled += 3.0f;
					p = np;
					h = nh;
					// Rest once the line from the release point dips under the shadow angle and the ground has eased.
					if ((start - h) < tan_shadow * travelled && gl < 0.75f) {
						break;
					}
					const int ci = CLAMP(int(Math::round((p.y - result_rect.position.y) / result_cell)), 0, ny - 1) * nx + CLAMP(int(Math::round((p.x - result_rect.position.x) / result_cell)), 0, nx - 1);
					if (has_water && water_body[ci] != 0) {
						lost = true; // into a lake, the sea or a river's water: out of sight
						break;
					}
				}
				if (lost || travelled < 4.0f || !p_rect.has_point(p)) {
					continue;
				}
				out.push_back(Vector4(p.x, _ground(p.x, p.y), p.y, radius));
			}
		}
	}
	return out;
}

/* Spray potential (docs/ideas/terrain-generation.md, "Spray potential") */

// The static baseline of where the falls keep the ground wet: each fall sprays from the line its water takes (lip
// to impact) and most from where it lands. Intensity falls off exponentially with distance (as measured around
// plunging jets: dam spillway atomization) over a reach that grows with the fall's power, and fades with height
// above the source (spray settles, it does not climb). Live wetting from wind-blown mist multiplies with it at run
// time; this map only says how much spray a place can get.
void TerrainGenerator3D::_bake_spray() {
	spray.clear();
	spray_nx = spray_ny = 0;
	if (spray_cell <= 0.0 || nx < 2 || ny < 2) {
		spray_texture.unref();
		return;
	}
	spray_nx = MAX(2, int(Math::floor((nx - 1) * result_cell / spray_cell)) + 1);
	spray_ny = MAX(2, int(Math::floor((ny - 1) * result_cell / spray_cell)) + 1);
	spray.resize(spray_nx * spray_ny);
	for (float &v : spray) {
		v = 0.0f;
	}
	const Array falls = get_waterfalls(1.0e4, 4.0);
	for (int f = 0; f < falls.size(); f++) {
		const Dictionary w = falls[f];
		const Vector3 top = w["top"];
		const Vector3 impact = w.has("pool_center") ? Vector3(w["pool_center"]) : Vector3(w["bottom"]);
		const real_t q = MAX(real_t(w["discharge"]), real_t(0.0));
		const real_t h = MAX(real_t(w["height"]), real_t(0.0));
		// Reach: 3 (Q H)^0.4 m, at least 4 m (a 20 m fall of 30 l/s wets a few metres; 3.6 m3/s over 330 m about
		// 50 m; the Victoria Falls' 1000 m3/s over 108 m some 300 m, the rainforest's width).
		const real_t reach = MAX(real_t(3.0) * Math::pow(q * h, real_t(0.4)), real_t(4.0));
		const real_t peak = CLAMP(real_t(0.25) + real_t(0.25) * Math::log(1.0 + q * h) / Math::log(real_t(10.0)), real_t(0.25), real_t(1.0));
		const real_t ext = reach * 1.5f;
		const int x0 = CLAMP(int(Math::floor((MIN(top.x, impact.x) - ext - result_rect.position.x) / spray_cell)), 0, spray_nx - 1);
		const int x1 = CLAMP(int(Math::ceil((MAX(top.x, impact.x) + ext - result_rect.position.x) / spray_cell)), 0, spray_nx - 1);
		const int z0 = CLAMP(int(Math::floor((MIN(top.z, impact.z) - ext - result_rect.position.y) / spray_cell)), 0, spray_ny - 1);
		const int z1 = CLAMP(int(Math::ceil((MAX(top.z, impact.z) + ext - result_rect.position.y) / spray_cell)), 0, spray_ny - 1);
		const Vector3 seg = impact - top;
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const real_t px = result_rect.position.x + x * spray_cell;
				const real_t pz = result_rect.position.y + z * spray_cell;
				const Vector3 p(px, _ground(px, pz), pz);
				// Horizontal distance to the water's path in plan (lip to impact); the curtain wets everything below the
				// lip beside it, more the further down (the sheet breaks into spray as it falls), most at the impact.
				const Vector2 s2(seg.x, seg.z);
				const real_t t = CLAMP(Vector2(p.x - top.x, p.z - top.z).dot(s2) / MAX(s2.length_squared(), real_t(1e-6)), real_t(0.0), real_t(1.0));
				const Vector2 c(top.x + s2.x * t, top.z + s2.y * t);
				const real_t d = Vector2(p.x - c.x, p.z - c.y).length();
				if (d > ext) {
					continue;
				}
				const real_t above = MAX(p.y - MAX(top.y, impact.y), real_t(0.0));
				const real_t down = CLAMP((top.y - p.y) / MAX(h, real_t(1.0)), real_t(0.0), real_t(1.0));
				const real_t source = 0.35f + 0.65f * down * down;
				const real_t v = peak * source * Math::exp(-2.0f * d / reach) * Math::exp(-above / (0.25f * reach + 2.0f));
				float &o = spray[z * spray_nx + x];
				o = 1.0f - (1.0f - o) * (1.0f - float(MIN(v, real_t(1.0)))); // several falls add like probabilities
			}
		}
	}
	const Ref<Image> img = get_spray_image();
	if (spray_texture.is_valid() && spray_texture->get_width() == spray_nx && spray_texture->get_height() == spray_ny) {
		spray_texture->update(img);
	} else {
		spray_texture = ImageTexture::create_from_image(img);
	}
}

/* Sky occlusion (docs/ideas/terrain-material.md, "Sky occlusion") */

// The final ground on a grid sky_cell apart, its sky view factor and bent normal (TerrainSky), as an RGBA8 texture:
// r the sky view factor, gba the bent normal * 0.5 + 0.5.
void TerrainGenerator3D::_bake_sky() {
	sky_vis.clear();
	sky_horizon.clear();
	sky_sh.clear();
	sky_nx = sky_ny = 0;
	if (!sky_occlusion || nx < 2 || ny < 2) {
		sky_texture.unref();
		last_sky_msec = 0.0;
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	sky_nx = MAX(2, int(Math::floor((nx - 1) * result_cell / sky_cell)) + 1);
	sky_ny = MAX(2, int(Math::floor((ny - 1) * result_cell / sky_cell)) + 1);
	const int n = sky_nx * sky_ny;
	LocalVector<float> h;
	h.resize(n);
	_fill(result_rect.position.x, result_rect.position.y, sky_cell, sky_nx, sky_ny, h.ptr());
	for (int i = 0; i < n; i++) {
		if (Math::is_nan(h[i])) {
			h[i] = result_min;
		}
	}
	sky_vis.resize(n);
	sky_horizon.resize(size_t(n) * TerrainSky::DIRECTIONS);
	TerrainSky::compute(h.ptr(), sky_nx, sky_ny, float(sky_cell), float(sky_distance), sky_vis.ptr(), sky_horizon.ptr());
	// Until the sky's own light is read (update_sky_light), an even sky.
	TerrainSky::SkyBins even;
	TerrainSky::even_sky(even);
	_sky_light(&even);
	sky_light_last.clear();
	sky_light_wait = 0.0;
	last_sky_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

void TerrainGenerator3D::_upload_sky() {
	Vector<Ref<Image>> layers;
	for (int l = 0; l < 3; l++) {
		const Ref<Image> img = get_sky_image(l);
		if (img.is_null()) {
			return;
		}
		layers.push_back(img);
	}
	if (sky_texture.is_valid() && sky_texture->get_width() == sky_nx && sky_texture->get_height() == sky_ny) {
		for (int l = 0; l < 3; l++) {
			sky_texture->update_layer(layers[l], l);
		}
	} else {
		sky_texture.instantiate();
		sky_texture->create_from_images(layers);
	}
}

namespace {
struct SkyLightJob {
	const TerrainSky::SkyBins *bins;
	const uint8_t *horizon;
	float *sh;
	float inv_c0;
	int nx;
};
constexpr int SKY_SH = TerrainSky::SH;
} // namespace

void TerrainGenerator3D::_sky_light_row(void *p_job, uint32_t p_row) {
	const SkyLightJob *job = static_cast<const SkyLightJob *>(p_job);
	for (int x = 0; x < job->nx; x++) {
		const size_t i = size_t(p_row) * job->nx + x;
		float *sh = job->sh + i * SKY_SH;
		TerrainSky::seen_sky(*job->bins, job->horizon + i * TerrainSky::DIRECTIONS, sh);
		for (int k = 0; k < SKY_SH; k++) {
			sh[k] *= job->inv_c0;
		}
	}
}

// Every node's seen sky from these bins, and the texture.
void TerrainGenerator3D::_sky_light(const void *p_bins) {
	const TerrainSky::SkyBins &bins = *static_cast<const TerrainSky::SkyBins *>(p_bins);
	const int n = sky_nx * sky_ny;
	if (sky_nx < 2 || sky_horizon.size() != size_t(n) * TerrainSky::DIRECTIONS || !(bins.full[0] > 0.0f)) {
		return;
	}
	const float inv = 1.0f / bins.full[0];
	for (int k = 0; k < SKY_SH; k++) {
		sky_full[k] = bins.full[k] * inv;
	}
	sky_sh.resize(n * SKY_SH);
	SkyLightJob job{ &bins, sky_horizon.ptr(), sky_sh.ptr(), inv, sky_nx };
	WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(&TerrainGenerator3D::_sky_light_row, &job, sky_ny, -1, true, SNAME("TerrainSkyLight"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	_upload_sky();
	if (is_inside_tree()) {
		_publish_globals();
	}
}

// The ground's occlusion of the sky as it is now: from a radiance panorama of the sky (as
// RenderingServer.sky_bake_panorama returns it), the share of the light raster gathers around each node's bent
// normal that really reaches it past its horizons. A low sunset glow behind a ridge is then taken away, where an
// even sky's view factor would leave most of it.
bool TerrainGenerator3D::update_sky_light(const Ref<Image> &p_panorama) {
	ERR_FAIL_COND_V(p_panorama.is_null() || p_panorama->is_empty(), false);
	const int n = sky_nx * sky_ny;
	if (sky_nx < 2 || sky_horizon.size() != size_t(n) * TerrainSky::DIRECTIONS) {
		return false;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	Ref<Image> pano = p_panorama;
	if (pano->get_format() != Image::FORMAT_RGBF) {
		pano = p_panorama->duplicate();
		pano->convert(Image::FORMAT_RGBF);
	}
	const Vector<uint8_t> data = pano->get_data();
	TerrainSky::SkyBins bins;
	TerrainSky::gather_sky(reinterpret_cast<const float *>(data.ptr()), pano->get_width(), pano->get_height(), bins);
	_sky_light(&bins);
	last_sky_light_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	return true;
}

void TerrainGenerator3D::set_sky_light_source(const NodePath &p_path) {
	sky_light_source = p_path;
	sky_light_wait = 0.0;
	sky_light_last.clear();
	if (is_inside_tree()) {
		set_process_internal(!sky_light_source.is_empty());
	}
}

void TerrainGenerator3D::_sky_light_poll(double p_delta) {
	sky_light_wait -= p_delta;
	if (sky_light_wait > 0.0 || sky_nx < 2) {
		return;
	}
	Node *src = get_node_or_null(sky_light_source);
	if (!src) {
		return;
	}
	Ref<Environment> env;
	if (WorldEnvironment *we = Object::cast_to<WorldEnvironment>(src)) {
		env = we->get_environment();
	} else if (Camera3D *cam = Object::cast_to<Camera3D>(src)) {
		env = cam->get_environment();
	}
	if (env.is_null() || env->get_sky().is_null()) {
		return;
	}
	// A small panorama is enough for light this broad (the bins are 22.5 by 11.25 degrees).
	Ref<Image> pano = RenderingServer::get_singleton()->sky_bake_panorama(env->get_sky()->get_rid(), 1.0, false, Size2i(64, 32));
	if (pano.is_null() || pano->is_empty()) {
		return;
	}
	pano->convert(Image::FORMAT_RGBF);
	const Vector<uint8_t> data = pano->get_data();
	const float *f = reinterpret_cast<const float *>(data.ptr());
	const int count = data.size() / int(sizeof(float));
	double sum = 0.0;
	for (int i = 0; i < count; i++) {
		sum += f[i];
	}
	if (!(sum > 1e-6)) {
		return; // the sky is not drawn yet: try again next frame
	}
	sky_light_wait = sky_light_interval;
	// Skip the update while the sky stays within 2% of the last one used.
	if (sky_light_last.size() == count) {
		double diff = 0.0, total = 0.0;
		const float *l = sky_light_last.ptr();
		for (int i = 0; i < count; i++) {
			diff += Math::abs(f[i] - l[i]);
			total += Math::abs(l[i]);
		}
		if (diff <= 0.02 * MAX(total, 1e-9)) {
			return;
		}
	}
	sky_light_last.resize(count);
	memcpy(sky_light_last.ptrw(), f, count * sizeof(float));
	update_sky_light(pano);
}

void TerrainGenerator3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(!sky_light_source.is_empty());
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_sky_light_poll(get_process_delta_time());
		} break;
	}
}

void TerrainGenerator3D::set_sky_occlusion(bool p_enable) {
	if (sky_occlusion == p_enable) {
		return;
	}
	sky_occlusion = p_enable;
	if (nx >= 2) {
		_bake_sky();
		_publish_globals();
	}
}

real_t TerrainGenerator3D::get_sky_visibility(const Vector3 &p_global_position) const {
	if (sky_nx < 2 || sky_vis.size() != uint32_t(sky_nx * sky_ny)) {
		return 1.0;
	}
	const real_t gx = CLAMP((p_global_position.x - result_rect.position.x) / sky_cell, real_t(0.0), real_t(sky_nx - 1));
	const real_t gz = CLAMP((p_global_position.z - result_rect.position.y) / sky_cell, real_t(0.0), real_t(sky_ny - 1));
	const int x0 = MIN(int(gx), sky_nx - 2), z0 = MIN(int(gz), sky_ny - 2);
	const real_t fx = gx - x0, fz = gz - z0;
	const float *r0 = sky_vis.ptr() + z0 * sky_nx, *r1 = r0 + sky_nx;
	return Math::lerp(Math::lerp(real_t(r0[x0]), real_t(r0[x0 + 1]), fx), Math::lerp(real_t(r1[x0]), real_t(r1[x0 + 1]), fx), fz);
}

// One layer of the sky texture: RGBA half of harmonics 4 * p_layer onwards (the last layer holds one).
Ref<Image> TerrainGenerator3D::get_sky_image(int p_layer) const {
	const int n = sky_nx * sky_ny;
	ERR_FAIL_INDEX_V(p_layer, 3, Ref<Image>());
	if (sky_nx < 2 || sky_sh.size() != uint32_t(n * SKY_SH)) {
		return Ref<Image>();
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * 4 * sizeof(float));
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < 4; k++) {
			const int c = p_layer * 4 + k;
			w[i * 4 + k] = c < SKY_SH ? sky_sh[i * SKY_SH + c] : 0.0f;
		}
	}
	Ref<Image> img = Image::create_from_data(sky_nx, sky_ny, false, Image::FORMAT_RGBAF, bytes);
	img->convert(Image::FORMAT_RGBAH);
	return img;
}

Ref<Image> TerrainGenerator3D::get_spray_image() const {
	if (spray_nx < 2 || spray.size() != uint32_t(spray_nx * spray_ny)) {
		return Ref<Image>();
	}
	Vector<uint8_t> bytes;
	bytes.resize(spray.size());
	uint8_t *w = bytes.ptrw();
	for (uint32_t i = 0; i < spray.size(); i++) {
		w[i] = uint8_t(CLAMP(int(Math::round(spray[i] * 255.0f)), 0, 255));
	}
	return Image::create_from_data(spray_nx, spray_ny, false, Image::FORMAT_L8, bytes);
}

real_t TerrainGenerator3D::get_spray_potential(const Vector3 &p_global_position) const {
	if (spray_nx < 2 || spray.size() != uint32_t(spray_nx * spray_ny)) {
		return 0.0;
	}
	const Vector3 &local = p_global_position; // the generator's grid is in world x z, as get_rock_hardness
	const real_t fx = CLAMP((local.x - result_rect.position.x) / spray_cell, real_t(0.0), real_t(spray_nx - 1));
	const real_t fz = CLAMP((local.z - result_rect.position.y) / spray_cell, real_t(0.0), real_t(spray_ny - 1));
	const int x = MIN(int(fx), spray_nx - 2), z = MIN(int(fz), spray_ny - 2);
	const real_t tx = fx - x, tz = fz - z;
	const float *g = spray.ptr();
	const real_t a = Math::lerp(real_t(g[z * spray_nx + x]), real_t(g[z * spray_nx + x + 1]), tx);
	const real_t b = Math::lerp(real_t(g[(z + 1) * spray_nx + x]), real_t(g[(z + 1) * spray_nx + x + 1]), tx);
	return Math::lerp(a, b, tz);
}

// How much of the geometric erosion detail each grid node keeps (_ground_detail's fades for water, alluvium and the
// coastal strip), so the detail normal in materials fades in the same places (an unset global reads white: full
// detail).
void TerrainGenerator3D::_bake_detail_keep() {
	const int n = nx * ny;
	if (n <= 0 || heights.size() != uint32_t(n)) {
		detail_keep_texture.unref();
		return;
	}
	Vector<uint8_t> bytes;
	bytes.resize(n * sizeof(float));
	float *w = reinterpret_cast<float *>(bytes.ptrw());
	for (int i = 0; i < n; i++) {
		const Vector3 p = _cell_position(i);
		double keep = _water_fade(p.x, p.z);
		if (result_deposition && alluvium_reach.size() == uint32_t(n)) {
			const double sd = CLAMP(double(alluvium_reach[i]) / ALLUVIUM_FADE, 0.0, 1.0);
			keep *= 1.0 - sd * sd * (3.0 - 2.0 * sd);
		}
		if (has_sea) {
			const double t = CLAMP((double(heights[i]) - double(result_min) - 0.5) / 5.5, 0.0, 1.0);
			keep *= t * t * (3.0 - 2.0 * t);
		}
		w[i] = float(keep);
	}
	const Ref<Image> img = Image::create_from_data(nx, ny, false, Image::FORMAT_RF, bytes);
	if (detail_keep_texture.is_valid() && detail_keep_texture->get_width() == nx && detail_keep_texture->get_height() == ny) {
		detail_keep_texture->update(img);
	} else {
		detail_keep_texture = ImageTexture::create_from_image(img);
	}
}

String TerrainGenerator3D::get_geology_shader_code() {
	String c;
	c += "// Geology from TerrainGenerator3D (TerrainGenerator3D.get_geology_shader_code()).\n";
	c += "// terrain_geology(p) = (soil depth m, or -1..0 on beach sand; rock exposure 0..1, talus 0..1 or -1..0 moraine till, log10 drainage area m2)\n";
	c += "global uniform sampler2D terrain_geology_map;\n";
	c += "global uniform vec4 terrain_geology_rect;\n\n";
	c += "vec4 terrain_geology(vec3 world_pos) {\n";
	c += "	// Texel centres are the grid nodes at the rectangle's corners.\n";
	c += "	vec2 size = vec2(textureSize(terrain_geology_map, 0));\n";
	c += "	vec2 g = clamp((world_pos.xz - terrain_geology_rect.xy) / terrain_geology_rect.zw, 0.0, 1.0);\n";
	c += "	return textureLod(terrain_geology_map, (g * (size - 1.0) + 0.5) / size, 0.0);\n";
	c += "}\n";
	return c;
}

// The detail filter's slope law (TerrainErosionFilter::gradient) as shader code, for normals below the vertex spacing:
// octaves continuing below the ones in the geometry, with this node's seed, depth, gully weight and heights baked in.
String TerrainGenerator3D::get_detail_shader_code() const {
	const uint32_t s0 = uint32_t(seed) * 0x2545F491u + 1u;
	const double lac = 2.0;
	const double start = detail_wavelength / Math::pow(lac, double(MAX(detail_octaves, 0)));
	String c;
	c += "// Erosion detail below the vertex spacing (TerrainGenerator3D.get_detail_shader_code()): the slope of the\n";
	c += "// detail filter's next octaves, added to the surface normal. terrain_detail_normal(world_pos, world_normal,\n";
	c += "// octaves, footprint) returns the new world normal; footprint is the pixel size in metres (octaves shorter than\n";
	c += "// about four pixels fade out), 0 for none.\n";
	c += vformat("const uint TD_SEED = %du;\n", int64_t(s0));
	c += vformat("const int TD_FIRST_OCTAVE = %d;\n", MAX(detail_octaves, 0));
	c += vformat("const float TD_WAVELENGTH = %.6f;\n", start);
	c += vformat("const float TD_DEPTH = %.6f;\n", double(detail_depth));
	c += vformat("const float TD_GULLY = %.6f;\n", MAX(double(detail_gully_weight), 1e-3));
	c += vformat("const float TD_REACH = %.6f;\n", double(detail_reach));
	c += vformat("const float TD_ASSUMED = %.6f;\n", double(detail_assumed_slope));
	c += vformat("const float TD_VALLEY = %.3f;\n", double(result_min));
	c += vformat("const float TD_PEAK = %.3f;\n", double(MAX(result_max, result_min + 1.0f)));
	// The grid rectangle (first to last node) of the map of where the geometric detail fades (alluvium, water,
	// coast), published as terrain_detail_keep_map (1 = full detail).
	c += vformat("const vec4 TD_RECT = vec4(%.3f, %.3f, %.3f, %.3f);\n", double(result_rect.position.x), double(result_rect.position.y), double(MAX(nx - 1, 1) * result_cell), double(MAX(ny - 1, 1) * result_cell));
	c += "global uniform sampler2D terrain_detail_keep_map;\n";
	c += "global uniform sampler2D terrain_ice_flow : hint_default_black, filter_linear;\n";
	c += "global uniform vec4 terrain_ice_flow_rect;\n";
	c += R"GLSL(
uint td_hash(uint x, uint z, uint s) {
	uint h = (x * 0x8DA6B343u) ^ (z * 0xD8163841u) ^ (s * 0xCB1AB31Fu);
	h ^= h >> 16u;
	h *= 0x7FEB352Du;
	h ^= h >> 15u;
	h *= 0x846CA68Bu;
	h ^= h >> 16u;
	return h;
}

float td_ease_out(float t) {
	float v = 1.0 - clamp(t, 0.0, 1.0);
	return 1.0 - v * v;
}

vec2 terrain_detail_gradient(vec2 p, float h, vec2 g, int octaves, float footprint) {
	vec2 r = vec2(0.0);
	float slope = length(g);
	if (slope < 1e-6) {
		return r;
	}
	vec2 st = g / slope * TD_ASSUMED;
	float fade = clamp((h - TD_VALLEY) / (TD_PEAK - TD_VALLEY), 0.0, 1.0) * 2.0 - 1.0;
	float mask = td_ease_out(slope / 0.5);
	float lambda = TD_WAVELENGTH;
	vec2 fd = vec2(0.0);
	vec2 md = vec2(0.0);
	for (int o = 0; o < octaves; o++) {
		float sl = length(st);
		if (sl < 1e-6) {
			break;
		}
		vec2 t = vec2(-st.y, st.x) / sl;
		vec2 f = p / lambda;
		vec2 fi = floor(f);
		float c = 0.0;
		float s = 0.0;
		float ws = 0.0;
		vec2 dc = vec2(0.0);
		vec2 ds = vec2(0.0);
		vec2 dw = vec2(0.0);
		uint oseed = TD_SEED + uint(TD_FIRST_OCTAVE + o) * 0x9E3779B9u;
		for (int j = -2; j <= 2; j++) {
			for (int i = -2; i <= 2; i++) {
				vec2 cell = fi + vec2(float(i), float(j));
				uint hs = td_hash(uint(int(cell.x)), uint(int(cell.y)), oseed);
				vec2 piv = cell + vec2(float(hs & 0xffffu), float(hs >> 16u)) / 65536.0;
				vec2 d = f - piv;
				float w1 = 1.0 - dot(d, d) / 2.25;
				if (w1 <= 0.0) {
					continue;
				}
				float w = w1 * w1 * w1;
				vec2 gw = 3.0 * w1 * w1 * (-2.0 * d / 2.25);
				float ph = dot(d, t) * 6.2831853;
				float cp = cos(ph);
				float sp = sin(ph);
				c += w * cp;
				s += w * sp;
				ws += w;
				dc += gw * cp - w * sp * 6.2831853 * t;
				ds += gw * sp + w * cp * 6.2831853 * t;
				dw += gw;
			}
		}
		if (ws <= 0.0) {
			break;
		}
		c /= ws;
		s /= ws;
		dc = (dc / ws - c * dw / ws) / lambda;
		ds = (ds / ws - s * dw / ws) / lambda;
		float len = sqrt(c * c + s * s);
		if (len > 0.5) {
			vec2 dl = (c * dc + s * ds) / len;
			dc = (dc - c * dl / len) / len;
			ds = (ds - s * dl / len) / len;
			c /= len;
			s /= len;
		} else {
			dc *= 2.0;
			ds *= 2.0;
			c *= 2.0;
			s *= 2.0;
		}
		float raw = c * TD_GULLY;
		float faded = fade + (raw - fade) * mask;
		fd = fd * (1.0 - mask) + mask * TD_GULLY * dc + (raw - fade) * md;
		// Octaves fade out between eight and four pixels a wavelength (no shimmer).
		float aa = footprint > 0.0 ? clamp(2.0 - 8.0 * footprint / lambda, 0.0, 1.0) : 1.0;
		r += aa * fd * TD_DEPTH / TD_GULLY * lambda;
		fade = faded;
		float m0 = clamp(mask, 0.0, 1.0);
		float reach = 1.0 - pow(1.0 - m0, TD_REACH);
		float dreach = TD_REACH * pow(max(1.0 - m0, 0.0), TD_REACH - 1.0);
		float as = abs(s);
		float e = td_ease_out(as);
		float de = as < 1.0 ? 2.0 * (1.0 - as) * sign(s) : 0.0;
		md = dreach * md * e + reach * de * ds;
		mask = reach * e;
		st += -(s >= 0.0 ? 1.0 : -1.0) * TD_DEPTH * 6.2831853 * t;
		lambda /= 2.0;
	}
	return r;
}

vec3 terrain_detail_normal(vec3 world_pos, vec3 world_normal, int octaves, float footprint) {
	// The detail is a heightfield's (gullies on slopes, looked up by x and z): on faces past about 60 degrees a
	// lookup by x and z runs down the face as vertical streaks, so it fades out there (the material's own rock
	// texture, projected on the face, carries the detail).
	float face = smoothstep(0.35, 0.55, world_normal.y);
	if (face <= 0.0) {
		return world_normal;
	}
	vec2 g = -world_normal.xz / max(world_normal.y, 1e-3);
	vec2 size = vec2(textureSize(terrain_detail_keep_map, 0));
	vec2 u = clamp((world_pos.xz - TD_RECT.xy) / TD_RECT.zw, 0.0, 1.0);
	float keep = clamp(textureLod(terrain_detail_keep_map, (u * (size - 1.0) + 0.5) / size, 0.0).r, 0.0, 1.0);
	if (keep <= 0.0) {
		return world_normal;
	}
	vec2 d = face * keep * terrain_detail_gradient(world_pos.xz, world_pos.y, g, octaves, footprint);
	if (terrain_ice_flow_rect.z > 0.0) {
		// Ice-worn rock (roches moutonnees; Glasser and Bennett 2004, Hallet 1996): the ice abrades the faces
		// that meet it smooth and quarries the faces it leaves, so along the flow the up-flow slopes of the
		// detail soften and the down-flow slopes steepen; fine striations run with the flow.
		vec2 iu = clamp((world_pos.xz - terrain_ice_flow_rect.xy) / terrain_ice_flow_rect.zw, 0.0, 1.0);
		vec2 isz = vec2(textureSize(terrain_ice_flow, 0));
		vec4 ice = textureLod(terrain_ice_flow, (iu * (isz - 1.0) + 0.5) / isz, 0.0);
		float wear = ice.b;
		vec2 f = ice.rg * 2.0 - 1.0;
		float fl = length(f);
		if (wear > 0.0 && fl > 0.1) {
			f /= fl;
			float a = dot(d, f); // > 0: the detail rises along the flow, a face meeting the ice
			float a2 = a > 0.0 ? a * (1.0 - 0.7 * wear) : a * (1.0 + 0.8 * wear);
			d += f * (a2 - a);
			vec2 fp = vec2(-f.y, f.x);
			float sl = 0.35; // m between striations
			float aa = footprint > 0.0 ? clamp(2.0 - 8.0 * footprint / sl, 0.0, 1.0) : 1.0;
			float ph = dot(world_pos.xz, fp) * 6.2831853 / sl + 3.0 * sin(dot(world_pos.xz, f) * 0.7);
			d += fp * (0.06 * wear * aa * face * keep * cos(ph));
		}
	}
	return normalize(vec3(-(g.x + d.x), 1.0, -(g.y + d.y)));
}
)GLSL";
	return c;
}

// Below each meandered reach the river's routed course is cut down to the channel's bed and falls gently on (a
// millimetre per metre at least) to its mouth, so the water never climbs out of the channel or stops against a rise
// on the way (a beach berm at the mouth). The cut follows the routed cells smoothed as get_rivers() smooths them, so
// it lies under the water's own points.
void TerrainGenerator3D::_cut_tails() {
	const int n = nx * ny;
	if (channels.is_empty() || river_receivers.size() != uint32_t(n) || channel_of_cell.size() != uint32_t(n)) {
		return;
	}
	const uint32_t count = channels.size();
	bool any = false;
	for (uint32_t ci = 0; ci < count; ci++) {
		if (!channels[ci].reach || channels[ci].foot_cell < 0) {
			continue;
		}
		// Down the routed course while it stays on the channel, keeping two cells behind for the smoothing.
		LocalVector<int> chain;
		int q = channels[ci].foot_cell;
		for (int guard = 0; guard < n; guard++) {
			chain.push_back(q);
			const int r = river_receivers[q];
			if (r < 0 || r == q || channel_of_cell[r] != int32_t(ci)) {
				break;
			}
			q = r;
		}
		const int e = int(chain.size()) - 1;
		for (int guard = 0; guard < 400; guard++) {
			const int r = river_receivers[q];
			if (r < 0 || r == q) {
				break;
			}
			chain.push_back(r);
			if (water_body.size() == uint32_t(n) && water_body[r] != 0) {
				break; // the river ends in the sea or a lake
			}
			q = r;
		}
		if (int(chain.size()) - e < 2) {
			continue;
		}
		const int s0 = MAX(e - 2, 0);
		LocalVector<Vector2> tp;
		for (int k = s0; k < int(chain.size()); k++) {
			const Vector3 p = _cell_position(chain[k]);
			tp.push_back(Vector2(p.x, p.z));
		}
		for (int pass = 0; pass < 2; pass++) {
			LocalVector<Vector2> sm;
			sm = tp;
			for (uint32_t k = 1; k + 1 < tp.size(); k++) {
				sm[k] = (tp[k - 1] + tp[k] * 2.0f + tp[k + 1]) * 0.25f;
			}
			tp = sm;
		}
		const Channel &rc = channels[ci];
		Channel t;
		t.half_width = rc.half_width;
		t.points.push_back(rc.points[rc.points.size() - 1]);
		t.bed.push_back(rc.bed[rc.bed.size() - 1]);
		t.dep.push_back(rc.dep[rc.dep.size() - 1]);
		real_t bed = t.bed[0];
		for (uint32_t k = uint32_t(e - s0 + 1); k < tp.size(); k++) {
			const real_t g = _ground(tp[k].x, tp[k].y);
			bed = MIN(g, bed - 0.001f * tp[k].distance_to(t.points[t.points.size() - 1]));
			t.points.push_back(tp[k]);
			t.bed.push_back(bed);
			t.dep.push_back(MAX(g - bed, real_t(0.0)));
		}
		if (t.points.size() >= 2) {
			channels.push_back(std::move(t));
			any = true;
		}
	}
	if (any) {
		_bucket_channels();
	}
}

// The braidplains outwash_age made, for the water (a sheet over the plain at its stage: threads are wherever the bed
// dips below it, Peytavie et al. 2019) and for placement (no plants on the active gravel).
Array TerrainGenerator3D::get_braidplains() const {
	Array out;
	for (const Channel &ch : channels) {
		if (ch.braid.is_empty()) {
			continue;
		}
		PackedVector2Array line;
		PackedFloat32Array halves, stage;
		for (uint32_t k = 0; k < ch.points.size(); k++) {
			line.push_back(ch.points[k]);
			halves.push_back(ch.braid_half);
			stage.push_back(k < ch.stage.size() ? float(ch.stage[k]) : NAN);
		}
		Dictionary d;
		d["line"] = line; // centre line, world x z, downstream
		d["half_width"] = ch.braid_half;
		d["half_widths"] = halves; // per point
		d["stage"] = stage; // mean-flow water surface per point, world y, flat across, never rising downstream
		// m3/s at the head: the most any cell under the first tenth of the line carries (a lake cell there reads none).
		float q = ch.head_cell >= 0 ? _flow(ch.head_cell, river_month) : 0.0f;
		for (uint32_t k = 0; k <= ch.points.size() / 10; k++) {
			const int x = CLAMP(int(Math::round((ch.points[k].x - result_rect.position.x) / result_cell)), 0, nx - 1);
			const int z = CLAMP(int(Math::round((ch.points[k].y - result_rect.position.y) / result_cell)), 0, ny - 1);
			q = MAX(q, _flow(z * nx + x, river_month));
		}
		d["discharge"] = q;
		d["head_cell"] = ch.head_cell;
		// The river feeding it: the get_rivers() river whose points pass the head (within half_width) runs on through
		// the plain as its main thread and leaves it at the foot.
		d["head"] = line[0];
		d["foot"] = line[line.size() - 1];
		out.push_back(d);
	}
	return out;
}

Vector3 TerrainGenerator3D::_cell_position(int p_cell) const {
	const real_t x = result_rect.position.x + (p_cell % nx) * result_cell;
	const real_t z = result_rect.position.y + (p_cell / nx) * result_cell;
	return Vector3(x, 0.0, z);
}

// Sea cells: within coast_distance of an ocean edge, the coastline wandering by coast_roughness (fBm), or white in
// sea_map.
void TerrainGenerator3D::_build_sea_mask(LocalVector<uint8_t> &r_sea) const {
	const int n = nx * ny;
	r_sea.resize(n);
	memset(r_sea.ptr(), 0, n);
	const Rect2 &rect = result_rect;
	if (ocean_edges != 0) {
		const uint32_t s = uint32_t(seed) * 0x27D4EB2Fu + 0x165667B1u;
		const real_t w = (nx - 1) * cell_size;
		const real_t d = (ny - 1) * cell_size;
		for (int z = 0; z < ny; z++) {
			for (int x = 0; x < nx; x++) {
				const real_t px = x * cell_size;
				const real_t pz = z * cell_size;
				// Soft minimum of the distances to the ocean edges (log-sum-exp), so corners are rounded.
				const double k = MAX(double(coast_distance) * 0.5, double(cell_size));
				double sum = 0.0;
				if (ocean_edges & OUTLET_NEGATIVE_X) {
					sum += Math::exp(-px / k);
				}
				if (ocean_edges & OUTLET_POSITIVE_X) {
					sum += Math::exp(-(w - px) / k);
				}
				if (ocean_edges & OUTLET_NEGATIVE_Z) {
					sum += Math::exp(-pz / k);
				}
				if (ocean_edges & OUTLET_POSITIVE_Z) {
					sum += Math::exp(-(d - pz) / k);
				}
				const double edge = sum > 0.0 ? -k * Math::log(sum) : 1e30;
				const double wobble = TerrainErosionFilter::fbm(s, rect.position.x + px, rect.position.y + pz, coast_wavelength, 5);
				if (edge < coast_distance + coast_roughness * wobble) {
					r_sea[z * nx + x] = 1;
				}
			}
		}
	}
	if (sea_map.is_valid()) {
		Ref<Image> img = sea_map->get_image();
		if (img.is_valid() && !img->is_empty()) {
			img = img->duplicate();
			if (img->is_compressed()) {
				img->decompress();
			}
			img->convert(Image::FORMAT_RF);
			const int iw = img->get_width();
			const int ih = img->get_height();
			for (int z = 0; z < ny; z++) {
				for (int x = 0; x < nx; x++) {
					const int u = CLAMP(int(real_t(x) / MAX(nx - 1, 1) * iw), 0, iw - 1);
					const int v = CLAMP(int(real_t(z) / MAX(ny - 1, 1) * ih), 0, ih - 1);
					if (img->get_pixel(u, v).r > 0.5) {
						r_sea[z * nx + x] = 1;
					}
				}
			}
		}
	}
}

// Below the sea: the equilibrium shoreface profile of Bruun (1954) and Dean (1991), depth = A x^(2/3) at x metres
// offshore, then a shelf sloping on, with low fBm relief (bars and troughs) so it is not a perfect ramp. Above it:
// a beach of beach_slope over a width that varies along the coast, blended into the land behind; where the land is
// already lower than that (a flat valley mouth) it stays as it is.
void TerrainGenerator3D::_shape_coast(const LocalVector<uint8_t> &p_sea) {
	const int n = nx * ny;
	std::vector<uint8_t> land(n);
	for (int i = 0; i < n; i++) {
		land[i] = p_sea[i] ? 0 : 1;
	}
	std::vector<double> to_land, to_sea;
	TerrainCoast::distance(nx, ny, cell_size, land.data(), to_land);
	TerrainCoast::distance(nx, ny, cell_size, p_sea.ptr(), to_sea);
	const double water = result_min;
	const double half = 0.5 * cell_size;
	const double surf = 300.0;
	beach.resize(n);
	memset(beach.ptr(), 0, sizeof(float) * n);
	const uint32_t s = uint32_t(seed) * 0x9E3779B1u + 0x7F4A7C15u;
	for (int i = 0; i < n; i++) {
		const double px = result_rect.position.x + (i % nx) * cell_size;
		const double pz = result_rect.position.y + (i / nx) * cell_size;
		if (p_sea[i]) {
			const double x = to_land[i] < 1e29 ? MAX(to_land[i] - half, 0.25 * cell_size) : 1e9;
			double depth = seabed_profile * Math::pow(x, 2.0 / 3.0) + shelf_slope * MAX(x - surf, 0.0);
			depth *= 1.0 + 0.2 * TerrainErosionFilter::fbm(s, px, pz, 400.0, 3);
			heights[i] = float(water - CLAMP(depth, 0.1, double(max_sea_depth)));
		} else if (to_sea[i] < 1e29) {
			const double x = MAX(to_sea[i] - half, 0.0);
			// Land never lies at sea level: a coastal plain the run graded flat to the sea (its outlets) is lifted to a
			// beach rising at beach_slope over beach_width, then 2 m per km inland, so it stays dry above the swash.
			const double floor_h = water + 0.15 + beach_slope * MIN(x, double(beach_width)) + 0.002 * MAX(x - double(beach_width), 0.0);
			heights[i] = float(MAX(double(heights[i]), floor_h));
			if (beach_width <= 0.0) {
				continue;
			}
			// Beaches along some stretches of the coast and none along others (cliffs), from low noise: a beach
			// face of beach_slope up to w from the waterline, then a backshore blending into the land over 60 m.
			const double vary = CLAMP(0.5 + 0.5 * TerrainErosionFilter::fbm(s ^ 0x5bd1e995u, px, pz, MAX(double(coast_wavelength) * 0.5, 50.0), 3), 0.0, 1.0);
			const double t0 = CLAMP((vary - 0.3) / 0.4, 0.0, 1.0);
			const double w = 2.0 * beach_width * t0 * t0 * (3.0 - 2.0 * t0);
			const double back = 60.0;
			if (w > 0.0 && x < w + back) {
				const double t = CLAMP((x - w) / back, 0.0, 1.0);
				const double blend = t * t * (3.0 - 2.0 * t);
				const double beach_h = water + MAX(beach_slope * x, 0.15);
				heights[i] = float(MIN(double(heights[i]), Math::lerp(beach_h, double(heights[i]), blend)));
				beach[i] = float(1.0 - blend);
			}
		}
	}
}

// A marker's downstream dam face: "face_slope" metadata (rise over run), or "rock_lip" for a bedrock sill (a steep
// face of 3, about 72 degrees) that holds a lake right above a waterfall without burying the fall under a debris
// apron, as the default landslide-dam face (0.35) does on a steep reach. -1: the default.
double TerrainGenerator3D::_marker_face_slope(const Node3D *p_marker) {
	if (p_marker->has_meta("face_slope")) {
		return MAX(double(p_marker->get_meta("face_slope")), 0.05);
	}
	if (p_marker->has_meta("rock_lip") && bool(p_marker->get_meta("rock_lip"))) {
		return 3.0;
	}
	return -1.0;
}

// Lakes: the dams on lake markers first, then automatic ones on valley floors with at least lake_catchment above
// them, in a seeded order, lake_spacing apart, away from the sea.
/* Fall lips */

// A free-falling fall needs a face the jet clears: over hard rock the face stands vertical (the caprock lip). The
// grid holds a drop over a whole cell at best (16 m: about 58 degrees for 25 m), so each fall the rivers make over
// hard rock (at least 4 m, rock at the lip at least half hard) is kept here, and _ground holds the lip's height to the
// middle of the drop and the foot's after it, the step taking LIP_FACE metres.
void TerrainGenerator3D::_bake_lips() {
	lips.clear();
	pools.clear();
	gorges.clear();
	lip_buckets.clear();
	const bool hard_map = hardness.size() == uint32_t(nx * ny);
	const bool sills = sill_cells.size() == uint32_t(nx * ny);
	const bool want_lips = fall_lips && (hard_map || sills);
	if (!want_lips && !plunge_pools) {
		return;
	}
	const Array rivers = _rivers(1.0e5, true);
	for (int r = 0; r < rivers.size(); r++) {
		const Dictionary d = rivers[r];
		const PackedInt32Array cells = d["cells"];
		const PackedFloat32Array widths = d["widths"];
		auto steep = [&](int a) {
			const int u = cells[a], v = cells[a + 1];
			const real_t run = result_cell * Math::sqrt(real_t((u % nx - v % nx) * (u % nx - v % nx) + (u / nx - v / nx) * (u / nx - v / nx)));
			return heights[u] - heights[v] > run;
		};
		int k = 0;
		while (k + 1 < cells.size()) {
			if (!steep(k)) {
				k++;
				continue;
			}
			int e = k;
			while (e + 1 < cells.size() && steep(e)) {
				e++;
			}
			const int u = cells[k], v = cells[e];
			const real_t drop = heights[u] - heights[v];
			if (drop < 4.0f) {
				k = e;
				continue;
			}
			const Vector3 a = _cell_position(u), b = _cell_position(v);
			const Vector2 origin = Vector2(a.x, a.z);
			Vector2 top = origin;
			Vector2 dv = Vector2(b.x, b.z) - top;
			real_t run = dv.length();
			Vector2 dir = run > 0.0 ? dv / run : Vector2(0, 1);
			real_t h_top = heights[u], h_foot = heights[v];
			const real_t width = MAX(real_t(widths[k]), real_t(4.0));
			const bool massive = sills && sill_cells[u] > 0.5f;
			const bool lipped = want_lips && ((hard_map && hardness[u] >= 0.5f) || massive);
			// Gorge: the lip has worn back upstream for gorge_age years at gorge_rate sqrt(Q) metres a year (knickpoint
			// celerity, Crosby and Whipple 2006; the rate fitted to Niagara, Gilbert 1907), a twentieth of that in
			// massive rock (a riegel; Lamb and Dietrich 2009). It stops short of a lake (the fall would drain it) and
			// of the fall above, and leaves a slot as wide as the river with a floor falling gently to the old foot.
			if (lipped && gorge_age > 0.0) {
				const real_t q = MAX(real_t(_flow(u, river_month)), real_t(0.0));
				real_t want = real_t(gorge_age) * real_t(gorge_rate) * Math::sqrt(q) * (massive ? 0.05f : 1.0f);
				Gorge g;
				g.half_width = MAX(0.5f * width + 1.0f, real_t(3.0));
				g.points.push_back(Vector2(b.x, b.z));
				g.points.push_back(origin);
				real_t len = 0.0;
				int j = k;
				Vector2 lip_at = origin;
				while (j > 0 && want > 0.0) {
					const int c = cells[j - 1];
					if ((water_body.size() == uint32_t(nx * ny) && water_body[c] != 0) || (lake_rim.size() == uint32_t(nx * ny) && !Math::is_nan(lake_rim[c])) || (j - 1 > 0 && steep(j - 2))) {
						break;
					}
					if (c % nx < 3 || c / nx < 3 || c % nx >= nx - 3 || c / nx >= ny - 3) {
						break; // the river came in over the map's edge: its reach above is not here
					}
					const Vector3 cp = _cell_position(c);
					const Vector2 p2(cp.x, cp.z);
					const real_t step = p2.distance_to(lip_at);
					// Keep two cells of the reach above the new lip untouched.
					if (j - 1 < 2) {
						break;
					}
					if (step >= want) {
						lip_at = lip_at + (p2 - lip_at) * (want / MAX(step, real_t(1e-3)));
						h_top = Math::lerp(h_top, heights[c], want / MAX(step, real_t(1e-3)));
						len += want;
						want = 0.0;
						g.points.push_back(lip_at);
						break;
					}
					lip_at = p2;
					h_top = heights[c];
					len += step;
					want -= step;
					g.points.push_back(lip_at);
					j--;
				}
				if (len >= 2.0f) {
					g.points.reverse(); // new lip first, down to the old foot
					g.floor_top = h_foot + 0.01f * len;
					g.floor_bottom = h_foot;
					real_t total = 0.0;
					for (uint32_t i = 1; i < g.points.size(); i++) {
						total += g.points[i].distance_to(g.points[i - 1]);
					}
					g.length = total;
					g.origin = origin;
					// The new fall: from the worn-back lip into the head of the slot.
					dir = (g.points[1] - g.points[0]).normalized();
					top = g.points[0] - dir * (0.5f * LIP_FACE + 1.0f);
					run = LIP_FACE + 2.0f;
					h_foot = g.floor_top;
					gorges.push_back(g);
				}
			}
			if (lipped) {
				FallLip lip;
				lip.origin = origin;
				lip.top = top;
				lip.run = run;
				lip.dir = dir;
				lip.h_top = h_top;
				lip.h_bottom = h_foot;
				lip.half_width = width;
				// Tall falls (a hanging valley's lip, a bar face) do not get a built step: the face is already there
				// and the water runs over it. The river only notches the lip, the notch narrowing and shallowing down
				// the face (Yosemite, Staubbach, the falls off riegels: a cleft at the brink, the wall sheer below,
				// rockfall at the foot; Crosby and Whipple 2006, Lamb et al. 2007).
				lip.notch = h_top - h_foot > NOTCH_DROP;
				lips.push_back(lip);
			}
			// The pool where the jet lands: past the face (the lip's step sits mid-run), or at the foot of a fall that
			// runs down its rock. None where the fall ends in a lake or the sea.
			const bool wet = water_body.size() == uint32_t(nx * ny) && water_body[v] != 0;
			if (plunge_pools && !wet) {
				const real_t q = MAX(real_t(_flow(u, river_month)) / real_t(widths[k] > 0.5f ? widths[k] : 0.5f), real_t(1e-4));
				const real_t v0 = Math::pow(9.81f * q, real_t(1.0 / 3.0));
				const real_t throw_m = v0 * Math::sqrt(2.0f * MAX(h_top - h_foot, real_t(0.0)) / 9.81f);
				const bool stepped = lipped && !(h_top - h_foot > NOTCH_DROP);
				const real_t face_at = stepped ? 0.5f * run - 0.5f * LIP_FACE : 0.0f;
				const real_t face_end = stepped ? 0.5f * run + 0.5f * LIP_FACE : run;
				// Mason and Arumugam 1985: D + Y = 3.27 q^0.60 H^0.05 h^0.15 / (g^0.30 d^0.10); tailwater h at the
				// critical depth, cobbles d = 0.3 m. At least 1.5 m (a fall of 4 m or more always leaves a hollow).
				const real_t h = MAX(Math::pow(q * q / 9.81f, real_t(1.0 / 3.0)), real_t(0.2));
				const real_t dy = 3.27f * Math::pow(q, real_t(0.60)) * Math::pow(MAX(h_top - h_foot, real_t(1.0)), real_t(0.05)) * Math::pow(h, real_t(0.15)) / (Math::pow(real_t(9.81), real_t(0.30)) * Math::pow(real_t(0.3), real_t(0.10)));
				PlungePool pool;
				pool.top = origin;
				pool.depth = CLAMP(dy - h, real_t(1.5), real_t(15.0));
				// Pagliara et al. 2006: the scour hole is several times longer than deep; and wider than the jet.
				pool.radius = MAX(MAX(0.35f * width, 3.0f * pool.depth), real_t(4.0));
				const real_t s = MAX(face_at + throw_m, face_end + 0.5f * pool.radius);
				pool.center = top + dir * s;
				// The floor hangs from the ground where the pool lies (past the foot cell the bed may keep falling).
				pool.foot = MIN(h_foot, _sample(heights, pool.center.x, pool.center.y));
				pool.dir = dir;
				pools.push_back(pool);
			}
			k = e;
		}
	}
	if (lips.is_empty() && pools.is_empty() && gorges.is_empty()) {
		return;
	}
	lip_bx = int(Math::ceil(result_rect.size.x / LIP_BUCKET)) + 1;
	lip_bz = int(Math::ceil(result_rect.size.y / LIP_BUCKET)) + 1;
	lip_buckets.resize(lip_bx * lip_bz);
	auto add = [&](const Vector2 &p_c, real_t p_reach, int p_index) {
		const int x0 = CLAMP(int(Math::floor((p_c.x - p_reach - result_rect.position.x) / LIP_BUCKET)), 0, lip_bx - 1);
		const int x1 = CLAMP(int(Math::floor((p_c.x + p_reach - result_rect.position.x) / LIP_BUCKET)), 0, lip_bx - 1);
		const int z0 = CLAMP(int(Math::floor((p_c.y - p_reach - result_rect.position.y) / LIP_BUCKET)), 0, lip_bz - 1);
		const int z1 = CLAMP(int(Math::floor((p_c.y + p_reach - result_rect.position.y) / LIP_BUCKET)), 0, lip_bz - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				lip_buckets[z * lip_bx + x].push_back(p_index);
			}
		}
	};
	// Lips by index, pools after them (index lips.size() + j).
	for (uint32_t i = 0; i < lips.size(); i++) {
		const FallLip &l = lips[i];
		add(l.top, l.half_width + LIP_SIDE + l.run + LIP_END, int(i));
	}
	for (uint32_t j = 0; j < pools.size(); j++) {
		add(pools[j].center, pools[j].radius * POOL_BAR, int(lips.size() + j));
	}
	// Gorges after the pools, one entry per segment's bucket span.
	for (uint32_t j = 0; j < gorges.size(); j++) {
		const Gorge &g = gorges[j];
		for (uint32_t i = 1; i < g.points.size(); i++) {
			add((g.points[i] + g.points[i - 1]) * 0.5f, 0.5f * g.points[i].distance_to(g.points[i - 1]) + g.half_width + GORGE_WALL, int(lips.size() + pools.size() + j));
		}
	}
}

float TerrainGenerator3D::_apply_lips(real_t p_x, real_t p_z, float p_g) const {
	const int bx = int(Math::floor((p_x - result_rect.position.x) / LIP_BUCKET));
	const int bz = int(Math::floor((p_z - result_rect.position.y) / LIP_BUCKET));
	if (bx < 0 || bz < 0 || bx >= lip_bx || bz >= lip_bz) {
		return p_g;
	}
	float g = p_g;
	const LocalVector<int> &bucket = lip_buckets[bz * lip_bx + bx];
	int last_gorge = -1;
	for (int i : bucket) {
		const int gi = i - int(lips.size() + pools.size());
		if (gi < 0 || gi == last_gorge) {
			continue;
		}
		last_gorge = gi;
		// The slot: within the half width the floor (falling gently from the new lip to the old foot), walls standing
		// over GORGE_WALL metres beside it.
		const Gorge &gg = gorges[gi];
		const Vector2 pp(p_x, p_z);
		real_t best = 1e30f, at = 0.0, acc = 0.0;
		for (uint32_t k = 1; k < gg.points.size(); k++) {
			const Vector2 a = gg.points[k - 1], b = gg.points[k];
			const Vector2 ab = b - a;
			const real_t l2 = MAX(ab.length_squared(), real_t(1e-6));
			const real_t t = CLAMP((pp - a).dot(ab) / l2, real_t(0.0), real_t(1.0));
			const real_t dd = pp.distance_to(a + ab * t);
			if (dd < best) {
				best = dd;
				at = acc + t * Math::sqrt(l2);
			}
			acc += Math::sqrt(l2);
		}
		if (best >= gg.half_width + GORGE_WALL) {
			continue;
		}
		const real_t floor_h = Math::lerp(gg.floor_top, gg.floor_bottom, CLAMP(at / MAX(gg.length, real_t(1e-3)), real_t(0.0), real_t(1.0)));
		const real_t w = best <= gg.half_width ? 1.0 : 1.0 - (best - gg.half_width) / GORGE_WALL;
		if (g > floor_h) {
			g = float(Math::lerp(real_t(g), floor_h, real_t(w * w * (3.0 - 2.0 * w))));
		}
	}
	for (int i : bucket) {
		if (i >= int(lips.size())) {
			continue;
		}
		const FallLip &l = lips[i];
		const Vector2 q = Vector2(p_x, p_z) - l.top;
		const real_t s = q.dot(l.dir);
		const real_t t = Math::abs(q.cross(l.dir));
		if (s < -LIP_END || s > l.run + LIP_END || t > l.half_width + LIP_SIDE) {
			continue;
		}
		auto fade = [](real_t x) { return x * x * (real_t(3.0) - real_t(2.0) * x); };
		if (l.notch) {
			// The notch: up to 0.4 of the river's width deep at the brink (2 to 10 m), fading in over LIP_END
			// upstream and out down the face (gone by a third of the run), a rounded V that narrows as it goes
			// down, its walls wandering by a quarter of its width (jointed rock, not a saw cut).
			if (s < -LIP_END || s > l.run) {
				continue;
			}
			const real_t sa = s / MAX(l.run, real_t(1.0));
			const real_t depth_at = CLAMP(0.4f * l.half_width, real_t(2.0), real_t(10.0)) * (s < 0.0 ? fade(CLAMP(1.0 + s / LIP_END, 0.0, 1.0)) : fade(CLAMP(1.0 - 3.0 * sa, 0.0, 1.0)));
			if (depth_at <= 0.0) {
				continue;
			}
			const uint32_t ns = uint32_t(seed) * 0x9E3779B1u + uint32_t(i) * 7919u;
			const real_t hw = MAX(0.6f * l.half_width, real_t(3.0)) * (1.0f - 0.5f * CLAMP(sa * 3.0, 0.0, 1.0)) * (1.0f + 0.25f * TerrainErosionFilter::fbm(ns, s, 13.0, 6.0, 3));
			const real_t side = (q.cross(l.dir) > 0.0 ? 1.0 : -1.0);
			const real_t x = (t + 0.15f * hw * TerrainErosionFilter::fbm(ns + 1u, s, side * 29.0, 4.0, 2)) / MAX(hw, real_t(1.0));
			if (x >= 1.0) {
				continue;
			}
			const real_t prof = fade(1.0 - x) * (1.0 - 0.3 * x); // flat-ish bed, walls steepening outwards
			g = float(real_t(g) - depth_at * prof);
			continue;
		}
		const real_t wa = s < 0.0 ? fade(CLAMP(1.0 + s / LIP_END, 0.0, 1.0)) : (s > l.run ? fade(CLAMP(1.0 - (s - l.run) / LIP_END, 0.0, 1.0)) : 1.0);
		const real_t wl = t <= l.half_width ? 1.0 : fade(CLAMP(1.0 - (t - l.half_width) / LIP_SIDE, 0.0, 1.0));
		const real_t w = wa * wl;
		if (w <= 0.0) {
			continue;
		}
		// The step: the lip's height to the middle of the drop, the foot's after it. The face line wanders by up
		// to 2 m along its length (jointed rock breaks off in blocks, not along a ruler).
		const Vector2 along = Vector2(-l.dir.y, l.dir.x);
		const real_t wander = 2.0 * TerrainErosionFilter::fbm(uint32_t(seed) * 0x9E3779B1u + uint32_t(i), q.dot(along), real_t(i) * 97.0, 9.0, 3);
		const real_t u = CLAMP((s - 0.5 * l.run - wander) / LIP_FACE + 0.5, 0.0, 1.0);
		const real_t h = Math::lerp(l.h_top, l.h_bottom, fade(u));
		g = float(Math::lerp(real_t(g), h, w));
	}
	// Pools after the lips: a bowl (1 - (r/R)^2)^2 deep at the centre, flat at the rim, cut below the foot.
	for (int i : bucket) {
		if (i < int(lips.size()) || i >= int(lips.size() + pools.size())) {
			continue;
		}
		const PlungePool &pl = pools[i - lips.size()];
		const Vector2 d = Vector2(p_x, p_z) - pl.center;
		const real_t r2 = d.length_squared() / (pl.radius * pl.radius);
		if (r2 >= POOL_BAR * POOL_BAR) {
			continue;
		}
		// The tail bar: what the jet scours out piles up downstream of the hole (Pagliara et al. 2006), so the pool
		// holds water where the ground falls away below the foot. Up to the foot's height, peaking just past the
		// rim, on the downstream side only, and at most the depth plus a metre high.
		const real_t r = Math::sqrt(r2);
		const real_t ahead = d.dot(pl.dir) / MAX(d.length(), real_t(1e-3));
		if (ahead > 0.0 && r > 0.8) {
			const real_t x = (r - 0.8) / (POOL_BAR - 0.8);
			const real_t bump = Math::sin(Math::PI * x);
			const real_t raise = CLAMP(pl.foot - real_t(g), real_t(0.0), pl.depth + 1.0f);
			g += float(raise * bump * Math::sqrt(ahead));
		}
		if (r2 >= 1.0) {
			continue;
		}
		const real_t floor_h = pl.foot - pl.depth * (1.0 - r2) * (1.0 - r2);
		g = MIN(g, float(floor_h));
	}
	return g;
}

/* Inflows */

LocalVector<TerrainGenerator3D::InflowPoint> TerrainGenerator3D::_inflow_points() const {
	LocalVector<InflowPoint> pts;
	for (int m = 0; m < inflows.size(); m++) {
		const Node3D *marker = Object::cast_to<Node3D>(get_node_or_null(inflows[m]));
		if (!marker) {
			WARN_PRINT(vformat("TerrainGenerator3D: inflow marker %s is not a Node3D in the tree; ignored.", String(inflows[m])));
			continue;
		}
		InflowPoint ip;
		const Vector3 p = marker->get_global_position();
		ip.position = Vector2(p.x, p.z);
		// A marker with only a sill (no area metadata and a sill or level) brings no land in.
		const bool sill_only = !marker->has_meta("area") && (marker->has_meta("sill") || marker->has_meta("level"));
		ip.area = sill_only ? 0.0 : double(marker->has_meta("area") ? real_t(marker->get_meta("area")) : inflow_area) * 1.0e6;
		ip.sill = marker->has_meta("sill") ? double(real_t(marker->get_meta("sill"))) : 0.0;
		ip.erosion = marker->has_meta("erosion") ? CLAMP(double(real_t(marker->get_meta("erosion"))), 0.0, 1.0) : 1.0;
		if (marker->has_meta("level")) {
			ip.level = double(real_t(marker->get_meta("level"))) - double(height_offset);
			ip.sill = MAX(ip.sill, 1.0); // a level needs a sill to hold it (at least the entry node)
		}
		if (ip.area > 0.0 || ip.sill > 0.0) {
			pts.push_back(ip);
		}
	}
	return pts;
}

// The node each inflow enters at on a p_nx x p_ny grid of p_dx spacing: the one under the marker, or, when that is
// an outlet (an open map edge or the sea), the nearest node within 8 that is not, so the water runs over the land.
void TerrainGenerator3D::_inflow_grid(const LocalVector<InflowPoint> &p_points, int p_nx, int p_ny, double p_dx, const uint8_t *p_outlet, LocalVector<double> &r_area, LocalVector<double> &r_sill, LocalVector<double> &r_level, LocalVector<double> *r_water) const {
	r_area.resize(p_nx * p_ny);
	memset(r_area.ptr(), 0, sizeof(double) * p_nx * p_ny);
	if (r_water) {
		r_water->resize(p_nx * p_ny);
		memset(r_water->ptr(), 0, sizeof(double) * p_nx * p_ny);
	}
	r_sill.clear();
	r_level.clear();
	auto outlet = [&](int x, int z) {
		return (p_outlet && p_outlet[z * p_nx + x]) || ((outlet_edges & 1) && x == 0) || ((outlet_edges & 2) && x == p_nx - 1) || ((outlet_edges & 4) && z == 0) || ((outlet_edges & 8) && z == p_ny - 1);
	};
	for (const InflowPoint &ip : p_points) {
		const int cx = CLAMP(int(Math::round((ip.position.x - result_rect.position.x) / p_dx)), 0, p_nx - 1);
		const int cz = CLAMP(int(Math::round((ip.position.y - result_rect.position.y) / p_dx)), 0, p_ny - 1);
		int best = -1, best_d = 1 << 30;
		for (int z = MAX(cz - 8, 0); z <= MIN(cz + 8, p_ny - 1); z++) {
			for (int x = MAX(cx - 8, 0); x <= MIN(cx + 8, p_nx - 1); x++) {
				const int d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
				if (d < best_d && !outlet(x, z)) {
					best = z * p_nx + x;
					best_d = d;
				}
			}
		}
		if (best >= 0) {
			r_area[best] += ip.area * ip.erosion;
			if (r_water) {
				(*r_water)[best] += ip.area;
			}
			if (ip.sill > 0.0) {
				// A sill of resistant rock round the entry: full within the radius, at least the entry node itself.
				if (r_sill.is_empty()) {
					r_sill.resize(p_nx * p_ny);
					memset(r_sill.ptr(), 0, sizeof(double) * p_nx * p_ny);
				}
				const int bx = best % p_nx, bz = best / p_nx;
				const int rc = int(Math::ceil(ip.sill / p_dx));
				for (int z = MAX(bz - rc, 0); z <= MIN(bz + rc, p_ny - 1); z++) {
					for (int x = MAX(bx - rc, 0); x <= MIN(bx + rc, p_nx - 1); x++) {
						const double dd = Math::sqrt(double((x - bx) * (x - bx) + (z - bz) * (z - bz))) * p_dx;
						if (dd <= MAX(ip.sill, 0.5 * p_dx)) {
							r_sill[z * p_nx + x] = 1.0;
							if (!Math::is_nan(ip.level)) {
								if (r_level.is_empty()) {
									r_level.resize(p_nx * p_ny);
									for (uint32_t k = 0; k < r_level.size(); k++) {
										r_level[k] = NAN;
									}
								}
								r_level[z * p_nx + x] = ip.level;
							}
						}
					}
				}
			}
		}
	}
}

// Where land beyond the map drains in: position (on the ground) and area (m2), one per node.
Array TerrainGenerator3D::get_inflow_cells() const {
	Array out;
	for (uint32_t i = 0; i < inflow_cells.size(); i++) {
		if (inflow_cells[i] > 0.0f) {
			Dictionary d;
			Vector3 p = _cell_position(int(i));
			p.y = heights[i];
			d["position"] = p;
			d["area"] = inflow_cells[i];
			out.push_back(d);
		}
	}
	return out;
}

void TerrainGenerator3D::_make_lakes(const LocalVector<uint8_t> &p_sea) {
	const int n = nx * ny;
	lake_info.clear();
	water_body.resize(n);
	water_depth.clear();
	for (int i = 0; i < n; i++) {
		water_body[i] = p_sea[i] ? 1 : 0;
	}
	std::vector<uint8_t> taken(n);
	for (int i = 0; i < n; i++) {
		taken[i] = p_sea[i];
	}
	if (!glacial_ice.is_empty()) {
		_basin_lakes(taken);
	}
	if (lake_count <= 0 && lakes.is_empty()) {
		return;
	}
	TerrainCoast::LakeParams lp;
	lp.nx = nx;
	lp.ny = ny;
	lp.dx = cell_size;
	lp.min_area = lake_min_area;
	lp.runoff = (runoff_n > 0 ? runoff_mean : double(river_runoff)) * 1.0e-3;
	lp.evaporation = lake_evaporation * 1.0e-3;
	lp.max_cells = MAX(n / 4, 16);
	std::vector<float> before(n);
	LocalVector<Vector2i> sites;
	const double default_face = lp.face_slope;
	auto try_site = [&](int32_t p_site, real_t p_depth, const char **r_why = nullptr, double p_face = -1.0) -> bool {
		lp.dam_height = p_depth;
		lp.face_slope = p_face > 0.0 ? p_face : default_face;
		lp.dam_half_width = MAX(10.0 * p_depth, 3.0 * cell_size);
		memcpy(before.data(), heights.ptr(), sizeof(float) * n);
		TerrainCoast::Lake lake;
		if (!TerrainCoast::make_lake(lp, p_site, receivers.ptr(), channel_area.ptr(), taken.data(), heights.ptr(), lake, r_why)) {
			return false;
		}
		const int k = int(lake_info.size());
		LakeInfo info;
		info.level = lake.level;
		info.bed = lake.bed;
		info.outlet = lake.outlet;
		info.dam = lake.dam;
		info.catchment = lake.catchment;
		info.endorheic = lake.endorheic;
		info.cells = int(lake.cells.size());
		for (int32_t c : lake.outflow) {
			info.outflow.push_back(c);
		}
		info.lo = Vector2i(nx, ny);
		info.hi = Vector2i(-1, -1);
		for (int32_t c : lake.cells) {
			water_body[c] = 2 + k;
			taken[c] = 1;
			const Vector2i p(c % nx, c / nx);
			info.lo = info.lo.min(p);
			info.hi = info.hi.max(p);
		}
		for (int i = 0; i < n; i++) {
			if (heights[i] != before[i]) {
				taken[i] = 1; // the dam
			}
		}
		lake_info.push_back(info);
		sites.push_back(Vector2i(p_site % nx, p_site / nx));
		return true;
	};
	// Markers: the dam on the valley floor nearest each (a cell within three with at least a quarter of the most
	// drainage area there, so a marker on a slope finds the stream below it without sliding down the valley).
	for (int m = 0; m < lakes.size(); m++) {
		Node3D *marker = Object::cast_to<Node3D>(get_node_or_null(lakes[m]));
		if (!marker) {
			WARN_PRINT(vformat("TerrainGenerator3D: lake marker %s is not a Node3D in the tree; no lake made.", String(lakes[m])));
			continue;
		}
		const Vector3 p = marker->get_global_position();
		const int cx = int(Math::round((p.x - result_rect.position.x) / cell_size));
		const int cz = int(Math::round((p.z - result_rect.position.y) / cell_size));
		float most = 0.0f;
		for (int z = MAX(cz - 3, 0); z <= MIN(cz + 3, ny - 1); z++) {
			for (int x = MAX(cx - 3, 0); x <= MIN(cx + 3, nx - 1); x++) {
				most = MAX(most, channel_area[z * nx + x]);
			}
		}
		int32_t best = -1;
		int best_d = 1 << 30;
		for (int z = MAX(cz - 3, 0); z <= MIN(cz + 3, ny - 1); z++) {
			for (int x = MAX(cx - 3, 0); x <= MIN(cx + 3, nx - 1); x++) {
				const int i = z * nx + x;
				const int d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
				if (!taken[i] && channel_area[i] >= 0.25f * most && d < best_d) {
					best = i;
					best_d = d;
				}
			}
		}
		const real_t depth = marker->has_meta("depth") ? real_t(marker->get_meta("depth")) : lake_depth;
		const char *why = nullptr;
		if (cx < 0 || cz < 0 || cx >= nx || cz >= ny) {
			why = "it stands outside the generated area";
		} else if (best < 0) {
			why = "no free valley floor within three cells (it is in the sea, in another lake, or on a ridge)";
		} else if (!try_site(best, depth, &why, _marker_face_slope(marker)) && !why) {
			why = "no lake formed";
		}
		if (why) {
			WARN_PRINT(vformat("TerrainGenerator3D: lake marker %s at (%.0f, %.0f) made no lake: %s.", marker->get_name(), p.x, p.z, why));
		}
	}
	if (lake_count <= 0) {
		return;
	}
	std::vector<double> to_sea;
	if (has_sea) {
		TerrainCoast::distance(nx, ny, cell_size, p_sea.ptr(), to_sea);
	}
	const double keep_off = MAX(10.0 * lake_depth, 3.0 * cell_size) * 2.0;
	std::vector<std::pair<uint32_t, int32_t>> candidates;
	for (int i = 0; i < n; i++) {
		if (taken[i] || channel_area[i] < lake_catchment || receivers[i] == i) {
			continue;
		}
		if (heights[i] < result_min + 0.5 * lake_depth || (has_sea && to_sea[i] < keep_off)) {
			continue;
		}
		candidates.push_back({ TerrainHydraulic::hash(uint32_t(i) * 0x9E3779B9u ^ (uint32_t(seed) * 0x85EBCA6Bu + 0x1A2B3C4Du)), i });
	}
	std::sort(candidates.begin(), candidates.end());
	// Probe up to 512 of them (each dam tried and taken back) and build the largest lakes first.
	const real_t spacing = lake_spacing / cell_size;
	std::vector<std::pair<int, int32_t>> probes;
	for (uint32_t c = 0; c < candidates.size() && c < 512; c++) {
		const int32_t site = candidates[c].second;
		lp.dam_height = lake_depth;
		lp.dam_half_width = MAX(10.0 * lake_depth, 3.0 * cell_size);
		TerrainCoast::Lake lake;
		memcpy(before.data(), heights.ptr(), sizeof(float) * n);
		if (TerrainCoast::make_lake(lp, site, receivers.ptr(), channel_area.ptr(), taken.data(), heights.ptr(), lake)) {
			probes.push_back({ -int(lake.cells.size()), int32_t(c) });
			memcpy(heights.ptr(), before.data(), sizeof(float) * n);
		}
	}
	std::sort(probes.begin(), probes.end());
	int made = 0;
	for (const std::pair<int, int32_t> &pr : probes) {
		if (made >= lake_count) {
			break;
		}
		const int32_t site = candidates[pr.second].second;
		const Vector2i p(site % nx, site / nx);
		bool near = taken[site] != 0;
		for (uint32_t k = 0; k < sites.size() && !near; k++) {
			near = Vector2(p - sites[k]).length() < spacing;
		}
		if (!near && try_site(site, lake_depth)) {
			made++;
		}
	}
}

// Rivers' routing with the water in it: lake cells drain to their lake's outlet (or nowhere, for a lake with no
// outflow), a col outlet to the cell below it, the sea ends everything. The area is accumulated in topological
// order (Kahn). For tracing, lake cells end the rivers that reach them.
void TerrainGenerator3D::_route_rivers() {
	const int n = nx * ny;
	LocalVector<int32_t> rec2;
	rec2.resize(n);
	memcpy(rec2.ptr(), receivers.ptr(), sizeof(int32_t) * n);
	for (int i = 0; i < n; i++) {
		const int32_t b = water_body.size() == uint32_t(n) ? water_body[i] : 0;
		if (b == 1) {
			rec2[i] = i;
		} else if (b >= 2) {
			const LakeInfo &l = lake_info[b - 2];
			rec2[i] = l.outlet >= 0 ? l.outlet : i;
		}
	}
	// Over a col the water follows the flood's path out of the basin, not the old drainage (which led back in).
	for (uint32_t k = 0; k < lake_info.size(); k++) {
		const LocalVector<int32_t> &path = lake_info[k].outflow;
		for (uint32_t q = 0; q + 1 < path.size(); q++) {
			rec2[path[q]] = path[q + 1];
		}
	}
	river_area.resize(n);
	LocalVector<int32_t> indeg;
	indeg.resize(n);
	memset(indeg.ptr(), 0, sizeof(int32_t) * n);
	for (int i = 0; i < n; i++) {
		river_area[i] = float(double(cell_size) * cell_size + (inflow_cells.is_empty() ? 0.0 : double(inflow_cells[i])));
		if (rec2[i] != i) {
			indeg[rec2[i]]++;
		}
	}
	LocalVector<int32_t> queue;
	queue.reserve(n);
	for (int i = 0; i < n; i++) {
		if (indeg[i] == 0) {
			queue.push_back(i);
		}
	}
	for (uint32_t q = 0; q < queue.size(); q++) {
		const int32_t i = queue[q];
		const int32_t r = rec2[i];
		if (r != i) {
			river_area[r] += river_area[i];
			if (--indeg[r] == 0) {
				queue.push_back(r);
			}
		}
	}
	_route_runoff(rec2, queue);
	river_receivers = rec2;
	water_depth.resize(n);
	for (int i = 0; i < n; i++) {
		const int32_t b = water_body[i];
		if (b >= 1) {
			river_receivers[i] = i;
		}
		const double level = b == 1 ? double(result_min) : (b >= 2 ? double(lake_info[b - 2].level) : -1e30);
		water_depth[i] = float(MAX(level - double(heights[i]), 0.0));
	}
	// The ring of three cells around each lake (and the lake itself) carries its level, for _ground().
	lake_rim.clear();
	lake_rim_weight.clear();
	if (!lake_info.is_empty()) {
		lake_rim.resize(n);
		lake_rim_weight.resize(n);
		for (int i = 0; i < n; i++) {
			lake_rim[i] = Math::NaN;
			lake_rim_weight[i] = 0.0f;
		}
		for (int i = 0; i < n; i++) {
			const int32_t b = water_body[i];
			if (b < 2) {
				continue;
			}
			const float level = lake_info[b - 2].level;
			const int x = i % nx;
			const int z = i / nx;
			for (int zz = MAX(z - 3, 0); zz <= MIN(z + 3, ny - 1); zz++) {
				for (int xx = MAX(x - 3, 0); xx <= MIN(x + 3, nx - 1); xx++) {
					float &r = lake_rim[zz * nx + xx];
					r = Math::is_nan(r) ? level : MAX(r, level);
					const int d = MAX(Math::abs(zz - z), Math::abs(xx - x));
					lake_rim_weight[zz * nx + xx] = MAX(lake_rim_weight[zz * nx + xx], CLAMP((3.0f - d) / 2.0f, 0.0f, 1.0f));
				}
			}
		}
	}
}

// 1 on dry ground, falling to 0 under more than a few metres of water: the erosion detail and the tile erosion
// stay off the seabed and the lake beds.
float TerrainGenerator3D::_water_fade(real_t p_x, real_t p_z) const {
	if (water_depth.is_empty() || (!has_sea && lake_info.is_empty())) {
		return 1.0f;
	}
	const float d = _sample(water_depth, p_x, p_z);
	if (Math::is_nan(d)) {
		return 1.0f;
	}
	const float t = CLAMP((d - 0.5f) / 2.5f, 0.0f, 1.0f);
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

// A lake's outline as a flat mesh (node-local, around p_origin): the grid quads with a corner under it and no corner
// on lower ground outside it (so the surface stops at the outlet instead of hanging over the valley below).
Ref<ArrayMesh> TerrainGenerator3D::_lake_outline(int p_lake, const Vector3 &p_origin) const {
	const LakeInfo &l = lake_info[p_lake];
	const int32_t id = 2 + p_lake;
	PackedVector3Array verts;
	PackedVector3Array norms;
	for (int z = MAX(l.lo.y - 1, 0); z <= MIN(l.hi.y, ny - 2); z++) {
		for (int x = MAX(l.lo.x - 1, 0); x <= MIN(l.hi.x, nx - 2); x++) {
			const int c[4] = { z * nx + x, z * nx + x + 1, (z + 1) * nx + x, (z + 1) * nx + x + 1 };
			bool wet = false;
			bool ok = true;
			for (int k = 0; k < 4; k++) {
				if (water_body[c[k]] == id) {
					wet = true;
				} else if (heights[c[k]] < l.level) {
					ok = false;
				}
			}
			if (!wet || !ok) {
				continue;
			}
			const real_t x0 = result_rect.position.x + x * result_cell - p_origin.x;
			const real_t z0 = result_rect.position.y + z * result_cell - p_origin.z;
			const real_t x1 = x0 + result_cell;
			const real_t z1 = z0 + result_cell;
			// Clockwise seen from above (Godot's front faces).
			const Vector3 q[6] = { Vector3(x0, 0, z0), Vector3(x1, 0, z0), Vector3(x0, 0, z1), Vector3(x1, 0, z0), Vector3(x1, 0, z1), Vector3(x0, 0, z1) };
			for (int k = 0; k < 6; k++) {
				verts.push_back(q[k]);
				norms.push_back(Vector3(0, 1, 0));
			}
		}
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	if (verts.is_empty()) {
		return mesh;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = norms;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	return mesh;
}

// A point surely on a lake: its cell farthest from the shore (the pole of inaccessibility on the grid).
Vector3 TerrainGenerator3D::_lake_center(int p_lake) const {
	const LakeInfo &l = lake_info[p_lake];
	const int32_t id = 2 + p_lake;
	const int x0 = MAX(l.lo.x - 1, 0);
	const int z0 = MAX(l.lo.y - 1, 0);
	const int w = MIN(l.hi.x + 1, nx - 1) - x0 + 1;
	const int h = MIN(l.hi.y + 1, ny - 1) - z0 + 1;
	std::vector<uint8_t> dry(w * h);
	for (int z = 0; z < h; z++) {
		for (int x = 0; x < w; x++) {
			dry[z * w + x] = water_body[(z0 + z) * nx + x0 + x] != id;
		}
	}
	std::vector<double> dist;
	TerrainCoast::distance(w, h, result_cell, dry.data(), dist);
	int best = -1;
	for (int i = 0; i < w * h; i++) {
		if (!dry[i] && (best < 0 || dist[i] > dist[best])) {
			best = i;
		}
	}
	if (best < 0) {
		return Vector3(Math::NaN, l.level, Math::NaN);
	}
	return Vector3(result_rect.position.x + (x0 + best % w) * result_cell, l.level, result_rect.position.y + (z0 + best / w) * result_cell);
}

// Each lake: level, bed, depth, area (m^2), catchment (m^2), endorheic, center (on the water), rect (world XZ),
// dam (its spillway cell, on the ground), outlet (where the water leaves; absent with no outflow).
Array TerrainGenerator3D::get_lake_results() const {
	Array out;
	for (uint32_t k = 0; k < lake_info.size(); k++) {
		const LakeInfo &l = lake_info[k];
		Dictionary d;
		d["level"] = l.level;
		d["bed"] = l.bed;
		d["depth"] = l.level - l.bed;
		d["area"] = real_t(l.cells) * result_cell * result_cell;
		d["catchment"] = l.catchment;
		d["endorheic"] = l.endorheic;
		const Rect2 r(result_rect.position.x + l.lo.x * result_cell, result_rect.position.y + l.lo.y * result_cell, (l.hi.x - l.lo.x) * result_cell, (l.hi.y - l.lo.y) * result_cell);
		d["rect"] = r;
		d["center"] = _lake_center(int(k));
		Vector3 dam = _cell_position(l.dam);
		dam.y = heights[l.dam];
		d["dam"] = dam;
		if (l.outlet >= 0) {
			Vector3 o = _cell_position(l.outlet);
			o.y = heights[l.outlet];
			d["outlet"] = o;
		}
		out.push_back(d);
	}
	return out;
}

// Lakes in the rock basins the last glaciation left (cirques, overdeepened troughs): every closed basin of the ground
// at least lake_min_area in size and 1 m deep fills to its spill point (Priority-Flood, Barnes et al. 2014), and
// spills over its lowest rim cell, which the rivers below already leave by.
void TerrainGenerator3D::_basin_lakes(std::vector<uint8_t> &r_taken) {
	const int n = nx * ny;
	std::vector<float> fill(heights.ptr(), heights.ptr() + n);
	std::vector<uint8_t> closed(n, 0);
	typedef std::pair<float, int32_t> Item;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
	for (int i = 0; i < n; i++) {
		if (receivers[i] == i || r_taken[i]) {
			closed[i] = 1;
			heap.push(Item(fill[i], i));
		}
	}
	static const int DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static const int DZ[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
	while (!heap.empty()) {
		const Item it = heap.top();
		heap.pop();
		const int32_t c = it.second;
		const int x = c % nx, z = c / nx;
		for (int k = 0; k < 8; k++) {
			const int xx = x + DX[k], zz = z + DZ[k];
			if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
				continue;
			}
			const int j = zz * nx + xx;
			if (closed[j]) {
				continue;
			}
			closed[j] = 1;
			fill[j] = MAX(fill[j], fill[c]);
			heap.push(Item(fill[j], j));
		}
	}
	std::vector<int32_t> comp(n, -1);
	std::vector<int32_t> cells;
	for (int s0 = 0; s0 < n; s0++) {
		if (comp[s0] >= 0 || r_taken[s0] || fill[s0] - heights[s0] <= 0.01f) {
			continue;
		}
		cells.clear();
		cells.push_back(s0);
		comp[s0] = s0;
		float deepest = 0.0f;
		int32_t bottom = s0;
		bool iced = false;
		for (size_t q = 0; q < cells.size(); q++) {
			const int32_t c = cells[q];
			iced = iced || glacial_ice[c] > 1.0f;
			const float d = fill[c] - heights[c];
			if (d > deepest) {
				deepest = d;
				bottom = c;
			}
			const int x = c % nx, z = c / nx;
			for (int k = 0; k < 8; k++) {
				const int xx = x + DX[k], zz = z + DZ[k];
				if (xx < 0 || zz < 0 || xx >= nx || zz >= ny) {
					continue;
				}
				const int j = zz * nx + xx;
				if (comp[j] < 0 && !r_taken[j] && fill[j] - heights[j] > 0.01f) {
					comp[j] = s0;
					cells.push_back(j);
				}
			}
		}
		// Only basins the ice wore (the rivers' own hollows fill with sediment as they always have).
		if (!iced || deepest < 1.0f || double(cells.size()) * cell_size * cell_size < double(lake_min_area)) {
			continue;
		}
		// Out of the basin down the receivers (laid on the filled surface): the first cell outside is the sill.
		int32_t outlet = bottom;
		for (int guard = 0; guard < n && comp[outlet] == s0 && receivers[outlet] != outlet; guard++) {
			outlet = receivers[outlet];
		}
		if (comp[outlet] == s0) {
			continue;
		}
		const int k = int(lake_info.size());
		LakeInfo info;
		info.level = fill[bottom];
		info.bed = heights[bottom];
		info.outlet = outlet;
		info.dam = outlet;
		// The largest drainage through any of its cells (a river crossing the lake), the outlet's own at least.
		float through = channel_area[outlet];
		for (int32_t c : cells) {
			through = MAX(through, channel_area[c]);
		}
		info.catchment = through;
		info.cells = int(cells.size());
		info.lo = Vector2i(nx, ny);
		info.hi = Vector2i(-1, -1);
		for (int32_t c : cells) {
			water_body[c] = 2 + k;
			r_taken[c] = 1;
			const Vector2i p(c % nx, c / nx);
			info.lo = info.lo.min(p);
			info.hi = info.hi.max(p);
		}
		lake_info.push_back(info);
	}
}

real_t TerrainGenerator3D::get_glacier_depth(const Vector3 &p_global_position) const {
	if (nx <= 0 || glacial_ice.size() != uint32_t(nx * ny)) {
		return 0.0;
	}
	const int x = CLAMP(int(Math::round((p_global_position.x - result_rect.position.x) / result_cell)), 0, nx - 1);
	const int z = CLAMP(int(Math::round((p_global_position.z - result_rect.position.y) / result_cell)), 0, ny - 1);
	return glacial_ice[z * nx + x];
}

// Height of the water surface over a point (the sea's or a lake's), NaN on dry ground or outside.
real_t TerrainGenerator3D::get_water_level(const Vector3 &p_global_position) const {
	if (nx <= 0 || water_body.size() != uint32_t(nx * ny)) {
		return Math::NaN;
	}
	const int x = int(Math::round((p_global_position.x - result_rect.position.x) / result_cell));
	const int z = int(Math::round((p_global_position.z - result_rect.position.y) / result_cell));
	if (x < 0 || z < 0 || x >= nx || z >= ny) {
		return Math::NaN;
	}
	const int32_t b = water_body[z * nx + x];
	return b == 1 ? real_t(result_min) : (b >= 2 ? lake_info[b - 2].level : real_t(Math::NaN));
}

real_t TerrainGenerator3D::get_water_depth(const Vector3 &p_global_position) const {
	const real_t level = get_water_level(p_global_position);
	if (Math::is_nan(level)) {
		return 0.0;
	}
	return MAX(level - _ground(p_global_position.x, p_global_position.z), real_t(0.0));
}

Ref<Image> TerrainGenerator3D::get_water_image() const {
	return _grid_image(water_depth);
}

// The water: one PhysXWaterSurface3D (by name) for the sea, at sea level over the whole rectangle with its seabed
// read from the ground and shore waves on, and one per lake, at its level and cut to its outline. Without the PhysX
// module, plain MeshInstance3D sheets. Call before build_river_paths() so rivers ending in them get mouth_target.
Array TerrainGenerator3D::build_water(Node *p_parent) {
	Array nodes;
	ERR_FAIL_NULL_V(p_parent, nodes);
	ERR_FAIL_COND_V_MSG(!has_result(), nodes, "TerrainGenerator3D: call generate() first.");
	const bool physx = ClassDB::class_exists("PhysXWaterSurface3D");
	Node3D *parent3d = Object::cast_to<Node3D>(p_parent);
	const Transform3D to_local = parent3d && parent3d->is_inside_tree() ? parent3d->get_global_transform().affine_inverse() : Transform3D();
	auto make = [&](const String &p_name, const Vector3 &p_world, const Ref<Mesh> &p_sheet) -> Node3D * {
		Node3D *w = physx ? Object::cast_to<Node3D>(ClassDB::instantiate("PhysXWaterSurface3D")) : nullptr;
		if (!w) {
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_mesh(p_sheet);
			w = mi;
		}
		w->set_name(p_name);
		w->set_position(to_local.xform(p_world));
		return w;
	};
	auto attach = [&](Node3D *p_node) {
		p_parent->add_child(p_node, true);
		if (Engine::get_singleton()->is_editor_hint()) {
			p_node->set_owner(p_parent->get_owner() ? p_parent->get_owner() : p_parent);
		}
		nodes.push_back(p_node);
	};
	sea_node = ObjectID();
	lake_nodes.clear();
	if (has_sea) {
		Ref<PlaneMesh> plane;
		plane.instantiate();
		plane->set_size(result_rect.size);
		const Vector2 c = result_rect.get_center();
		Node3D *sea = make("Sea", Vector3(c.x, result_min, c.y), plane);
		if (physx) {
			const real_t extent = MAX(result_rect.size.x, result_rect.size.y);
			sea->set("domain_size", result_rect.size);
			sea->set("grid_resolution", CLAMP(int(extent / 16.0), 128, 512));
			sea->set("water_level", 0.0);
			// Past the simulated square the water is open sea of the constant depth: the seabed's mean depth along the
			// rectangle's edge, so there is no step in colour or waves at its border.
			double edge_depth = 0.0;
			int edge_count = 0;
			for (int i = 0; i < nx * ny; i++) {
				const int x = i % nx;
				const int z = i / nx;
				if ((x == 0 || z == 0 || x == nx - 1 || z == ny - 1) && water_body[i] == 1) {
					edge_depth += water_depth[i];
					edge_count++;
				}
			}
			sea->set("depth", MAX(edge_count > 0 ? edge_depth / edge_count : double(result_min - sea_bottom), 1.0));
			sea->set("seabed_from_floor", true);
			sea->set("shore_waves", true);
			sea->set("render_extent", MAX(extent * 8.0, real_t(20000.0)));
			// Past the square it is open sea, its waves in three cascades, its wind from the weather when there is one.
			sea->set("open_sea_beyond", true);
			sea->set("wave_cascades", 3);
			sea->set("follow_weather", true);
		}
		attach(sea);
		sea_node = sea->get_instance_id();
	}
	for (uint32_t k = 0; k < lake_info.size(); k++) {
		const LakeInfo &l = lake_info[k];
		const Vector2 c((l.lo.x + l.hi.x) * 0.5 * result_cell + result_rect.position.x, (l.lo.y + l.hi.y) * 0.5 * result_cell + result_rect.position.y);
		const Vector3 origin(c.x, l.level, c.y);
		Ref<ArrayMesh> outline = _lake_outline(int(k), origin);
		Node3D *lake = make(vformat("Lake%d", k), origin, outline);
		if (physx) {
			const real_t extent = MAX(l.hi.x - l.lo.x, l.hi.y - l.lo.y) * result_cell;
			lake->set("surface_mesh", outline);
			lake->set("grid_resolution", CLAMP(int(extent / 4.0), 64, 256));
			lake->set("water_level", 0.0);
			lake->set("depth", MAX(l.level - l.bed, real_t(1.0)));
			lake->set("seabed_from_floor", true);
			lake->set("render_extent", 0.0);
			lake->set("wind_speed", 4.0);
			lake->set("follow_weather", true);
		}
		lake->set_meta("level", l.level);
		attach(lake);
		lake_nodes.push_back(lake->get_instance_id());
	}
	return nodes;
}

// Mean discharge (m^3/s) of a drainage area (m^2) at river_runoff.
real_t TerrainGenerator3D::get_discharge(real_t p_area) const {
	return p_area * river_runoff * 1.0e-3 / 31557600.0;
}

// Rivers draining at least p_min_area m^2, trunk rivers first. Each is a Dictionary: points (world, head to mouth,
// on the ground), widths (m, hydraulic geometry w = a Q^0.5, Leopold and Maddock 1953), areas, discharge (m^3/s at
// the mouth), own_discharge (that minus its tributaries'), joins (index of the river it joins, -1 at an outlet).
Array TerrainGenerator3D::get_rivers(real_t p_min_area) const {
	return _rivers(p_min_area, false);
}

Array TerrainGenerator3D::_rivers(real_t p_min_area, bool p_cells) const {
	Array out;
	const int n = nx * ny;
	if (nx <= 0 || river_receivers.size() != uint32_t(n) || river_area.size() != uint32_t(n)) {
		return out;
	}
	std::vector<TerrainGeology::River> rivers;
	TerrainGeology::rivers(n, river_area.ptr(), river_receivers.ptr(), MAX(double(p_min_area), double(result_cell * result_cell)), rivers);
	// Water cells end rivers, so one with enough area of its own would start a river of one cell: drop those (they
	// have no tributaries) and renumber the joins.
	{
		std::vector<int> index(rivers.size(), -1);
		std::vector<TerrainGeology::River> kept;
		for (uint32_t r = 0; r < rivers.size(); r++) {
			if (rivers[r].cells.size() >= 2) {
				index[r] = int(kept.size());
				kept.push_back(std::move(rivers[r]));
			}
		}
		for (TerrainGeology::River &rv : kept) {
			rv.joins = rv.joins >= 0 ? index[rv.joins] : -1;
		}
		rivers = std::move(kept);
	}
	// A lake's outflow starts at its outlet. Where a stem runs on up past an outlet (a stream joining right there has
	// more area than any other donor once the lake cells end rivers), the part above becomes a tributary joining at
	// the outlet, and so do the tributaries that joined that part.
	if (!lake_info.is_empty()) {
		std::vector<uint8_t> is_outlet(n, 0);
		for (uint32_t k = 0; k < lake_info.size(); k++) {
			if (lake_info[k].outlet >= 0) {
				is_outlet[lake_info[k].outlet] = 1;
			}
		}
		for (uint32_t r = 0; r < rivers.size(); r++) {
			const std::vector<int32_t> cells = rivers[r].cells;
			const size_t last = rivers[r].joins >= 0 ? cells.size() - 1 : cells.size(); // a junction is not its own
			for (size_t k = 1; k < last; k++) {
				if (!is_outlet[cells[k]]) {
					continue;
				}
				TerrainGeology::River upper;
				upper.cells.assign(cells.begin(), cells.begin() + k + 1);
				upper.joins = int(r);
				rivers[r].cells.assign(cells.begin() + k, cells.end());
				const int ui = int(rivers.size());
				std::vector<uint8_t> in_upper(n, 0);
				for (size_t q = 0; q < k; q++) {
					in_upper[cells[q]] = 1;
				}
				for (TerrainGeology::River &t : rivers) {
					if (t.joins == int(r) && in_upper[t.cells.back()]) {
						t.joins = ui;
					}
				}
				rivers.push_back(std::move(upper));
				break;
			}
		}
	}
	LocalVector<double> own;
	own.resize(rivers.size());
	for (uint32_t r = 0; r < rivers.size(); r++) {
		const TerrainGeology::River &rv = rivers[r];
		// A tributary's mouth is the cell before the junction.
		const int mouth = rv.joins >= 0 && rv.cells.size() > 1 ? rv.cells[rv.cells.size() - 2] : rv.cells.back();
		own[r] = _flow(mouth, river_month);
	}
	for (uint32_t r = 0; r < rivers.size(); r++) {
		if (rivers[r].joins >= 0) {
			const TerrainGeology::River &rv = rivers[r];
			const int mouth = rv.cells.size() > 1 ? rv.cells[rv.cells.size() - 2] : rv.cells.back();
			own[rivers[r].joins] -= _flow(mouth, river_month);
		}
	}
	for (uint32_t r = 0; r < rivers.size(); r++) {
		const TerrainGeology::River &rv = rivers[r];
		const int m = int(rv.cells.size());
		// D8 steps smoothed into a curve (two passes of a 1-2-1 filter), the ends kept.
		std::vector<Vector3> pts(m);
		for (int k = 0; k < m; k++) {
			pts[k] = _cell_position(rv.cells[k]);
		}
		for (int pass = 0; pass < 2 && m > 2; pass++) {
			std::vector<Vector3> tmp = pts;
			for (int k = 1; k < m - 1; k++) {
				tmp[k] = (pts[k - 1] + pts[k] * 2.0 + pts[k + 1]) * 0.25;
			}
			pts = tmp;
		}
		// Along a meandered reach the water follows the channel's own curve (finer than the cells): the run of cells
		// the channel was cut through is replaced by its points, each taking the area of the run's cell at the same
		// fraction along. Callers that want the cells keep one point per cell.
		std::vector<Vector3> spts;
		std::vector<int32_t> scell;
		std::vector<uint8_t> son; // on a channel: the point's height is its water surface already
		spts.reserve(m);
		scell.reserve(m);
		for (int k = 0; k < m; k++) {
			const int ci = !p_cells && channel_of_cell.size() == uint32_t(n) ? channel_of_cell[rv.cells[k]] : -1;
			if (ci < 0 || k == 0) {
				spts.push_back(pts[k]);
				scell.push_back(rv.cells[k]);
				son.push_back(0);
				continue;
			}
			// The river may cross the loops on the grid: it takes the channel from the first cell on it to the last.
			int e = k;
			for (int q = k + 1; q < m; q++) {
				if (channel_of_cell[rv.cells[q]] == ci) {
					e = q;
				}
			}
			const Channel &ch = channels[ci];
			auto nearest = [&](const Vector3 &q) {
				int best = 0;
				real_t bd = 1e30f;
				for (uint32_t t = 0; t < ch.points.size(); t++) {
					const real_t dd = ch.points[t].distance_squared_to(Vector2(q.x, q.z));
					if (dd < bd) {
						bd = dd;
						best = int(t);
					}
				}
				return best;
			};
			// From the reach's head when the river comes in near it, to its foot when it leaves near that (the grid
			// course may cut a corner on to the channel a little way down).
			int a = nearest(pts[k]), b = nearest(pts[e]);
			const Vector2 head = ch.points[0], foot = ch.points[ch.points.size() - 1];
			if (a > 0 && spts.size() > 1) {
				// The grid course ran past the head: back up to its point nearest the head, and take the channel whole.
				int best = -1;
				real_t bd = 4.0f * result_cell;
				for (int q = int(spts.size()) - 1; q >= MAX(int(spts.size()) - 8, 1); q--) {
					const real_t dd = head.distance_to(Vector2(spts[q].x, spts[q].z));
					if (dd < bd) {
						bd = dd;
						best = q;
					}
				}
				if (best >= 0) {
					spts.resize(best + 1);
					scell.resize(best + 1);
					son.resize(best + 1);
					a = 0;
				}
			}
			if (e + 1 < m && foot.distance_to(Vector2(pts[e + 1].x, pts[e + 1].z)) < 4.0f * result_cell) {
				b = int(ch.points.size()) - 1;
			}
			if (b - a >= 2) {
				for (int t = a; t <= b; t++) {
					// The water stands at about 0.6 of the bankfull depth over the bed at ordinary flow.
					spts.push_back(Vector3(ch.points[t].x, ch.bed[t] + 0.6f * ch.dep[t], ch.points[t].y));
					son.push_back(1);
					scell.push_back(rv.cells[k + int(Math::round(double(e - k) * double(t - a) / double(b - a)))]);
				}
			} else {
				for (int t = k; t <= e; t++) {
					spts.push_back(pts[t]);
					scell.push_back(rv.cells[t]);
					son.push_back(0);
				}
			}
			k = e;
		}
		const int sm = int(spts.size());
		PackedVector3Array points;
		PackedFloat32Array widths;
		PackedFloat32Array areas;
		points.resize(sm);
		widths.resize(sm);
		areas.resize(sm);
		for (int k = 0; k < sm; k++) {
			const real_t a = river_area[scell[k]];
			Vector3 p = spts[k];
			if (!son[k]) {
				p.y = _ground(p.x, p.z);
			}
			// At a mouth in the sea or a lake, on the water rather than its bed. The last point sits on the water even
			// where the shore's erosion detail (full strength in the shallowest margin) lifts the ground above it.
			const int32_t b = water_body.size() == uint32_t(n) ? water_body[scell[k]] : 0;
			if (b >= 1) {
				const real_t level = b == 1 ? real_t(result_min) : lake_info[b - 2].level;
				p.y = k == sm - 1 ? level : MAX(p.y, level);
			}
			points.set(k, p);
			areas.set(k, a);
			widths.set(k, river_width_coefficient * Math::sqrt(_flow(scell[k], river_month)));
		}
		const int mouth = rv.joins >= 0 && m > 1 ? rv.cells[m - 2] : rv.cells[m - 1];
		Dictionary d;
		d["points"] = points;
		d["widths"] = widths;
		d["areas"] = areas;
		d["discharge"] = _flow(mouth, river_month);
		PackedFloat32Array monthly;
		monthly.resize(12);
		for (int m = 0; m < 12; m++) {
			monthly.set(m, _flow(mouth, m));
		}
		d["monthly_discharge"] = monthly;
		d["own_discharge"] = MAX(own[r], 0.0);
		d["joins"] = rv.joins;
		// Where it ends and starts: "sea", "lake" (index in "lake"), "river" (a tributary) or "edge"; "from_lake" is
		// the lake whose outflow it is, -1 for a river rising on land.
		const int32_t end_body = water_body.size() == uint32_t(n) ? water_body[rv.cells[m - 1]] : 0;
		d["ends_in"] = rv.joins >= 0 ? "river" : (end_body == 1 ? "sea" : (end_body >= 2 ? "lake" : "edge"));
		d["lake"] = end_body >= 2 && rv.joins < 0 ? end_body - 2 : -1;
		int from_lake = -1;
		for (uint32_t k = 0; k < lake_info.size(); k++) {
			if (lake_info[k].outlet == rv.cells[0]) {
				from_lake = int(k);
			}
		}
		d["from_lake"] = from_lake;
		if (p_cells) {
			PackedInt32Array cells;
			cells.resize(m);
			for (int k = 0; k < m; k++) {
				cells.set(k, rv.cells[k]);
			}
			d["cells"] = cells;
		}
		out.push_back(d);
	}
	return out;
}

// One path per river under p_parent: a PhysXRiver3D when that class exists (curve, widths, its own discharge, and
// mouth_target set to the river it joins), otherwise a Path3D with the widths and discharge as metadata.
Array TerrainGenerator3D::build_river_paths(Node *p_parent, real_t p_min_area) {
	Array nodes;
	ERR_FAIL_NULL_V(p_parent, nodes);
	const Array rivers = get_rivers(p_min_area);
	const bool physx = ClassDB::class_exists("PhysXRiver3D");
	Node3D *parent3d = Object::cast_to<Node3D>(p_parent);
	const Transform3D to_local = parent3d && parent3d->is_inside_tree() ? parent3d->get_global_transform().affine_inverse() : Transform3D();
	for (int r = 0; r < rivers.size(); r++) {
		const Dictionary d = rivers[r];
		const PackedVector3Array pts = d["points"];
		Path3D *path = physx ? Object::cast_to<Path3D>(ClassDB::instantiate("PhysXRiver3D")) : memnew(Path3D);
		ERR_CONTINUE(!path);
		path->set_name(vformat("River%d", r));
		Ref<Curve3D> curve;
		curve.instantiate();
		for (int k = 0; k < pts.size(); k++) {
			curve->add_point(to_local.xform(pts[k]));
		}
		path->set_curve(curve);
		if (physx) {
			path->set("widths", d["widths"]);
			path->set("discharge", d["own_discharge"]);
		} else {
			path->set_meta("widths", d["widths"]);
			path->set_meta("discharge", d["own_discharge"]);
		}
		path->set_meta("joins", d["joins"]);
		p_parent->add_child(path, true);
		if (Engine::get_singleton()->is_editor_hint()) {
			// Saved with the scene being edited.
			path->set_owner(p_parent->get_owner() ? p_parent->get_owner() : p_parent);
		}
		nodes.push_back(path);
	}
	if (physx) {
		for (int r = 0; r < nodes.size(); r++) {
			const Dictionary d = rivers[r];
			const int j = int(d["joins"]);
			Node *a = Object::cast_to<Node>(nodes[r]);
			Node *b = nullptr;
			if (j >= 0) {
				b = Object::cast_to<Node>(nodes[j]);
			} else if (String(d["ends_in"]) == "sea") {
				b = ObjectDB::get_instance<Node>(sea_node);
			} else if (String(d["ends_in"]) == "lake" && int(d["lake"]) < int(lake_nodes.size())) {
				b = ObjectDB::get_instance<Node>(lake_nodes[int(d["lake"])]);
			}
			if (b && a->is_inside_tree() && b->is_inside_tree()) {
				a->set("mouth_target", a->get_path_to(b));
			}
		}
	}
	return nodes;
}

int TerrainGenerator3D::write_to_terrain() {
	ERR_FAIL_COND_V_MSG(!has_result(), 0, "TerrainGenerator3D: call generate() first.");
	ERR_FAIL_COND_V(!is_inside_tree(), 0);
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	TerrainDeformer3D *def = Object::cast_to<TerrainDeformer3D>(get_node_or_null(deformer_path));
	Node *terrain = _get_terrain_node();
	MeshInstance3D *mesh = Object::cast_to<MeshInstance3D>(get_node_or_null(ground_mesh_path));
	if (!mesh && def) {
		mesh = Object::cast_to<MeshInstance3D>(def->get_node_or_null(def->get_ground_mesh()));
	}
	// Lift the deformer's brushes first so its base heights are the old ground, then re-apply them on the new one.
	if (def) {
		def->clear();
	}
	TerrainDeformer3D::Grid grid;
	ERR_FAIL_COND_V_MSG(!TerrainDeformer3D::open_grid(terrain, grid), 0, "TerrainGenerator3D: terrain must be a Terrain3D or a CollisionShape3D with a HeightMapShape3D.");
	const Rect2 &rect = result_rect;
	const real_t sp = grid.spacing;
	const real_t tol = sp * 1.0e-3;
	int written = 0;

	if (grid.backend == TerrainDeformer3D::BACKEND_TERRAIN3D) {
		const int rs = grid.region_size;
		const Vector2i c0(int(Math::ceil((rect.position.x - tol) / sp)), int(Math::ceil((rect.position.y - tol) / sp)));
		const Vector2i c1(int(Math::floor((rect.position.x + rect.size.x + tol) / sp)), int(Math::floor((rect.position.y + rect.size.y + tol) / sp)));
		bool added = false;
		for (int rz = _gen_floor_div(c0.y, rs); rz <= _gen_floor_div(c1.y, rs); rz++) {
			for (int rx = _gen_floor_div(c0.x, rs); rx <= _gen_floor_div(c1.x, rs); rx++) {
				const Vector2i loc(rx, rz);
				if (!bool(grid.data->call("has_region", loc))) {
					grid.data->call("add_region_blank", loc, false);
					added = true;
				}
				Object *region = grid.data->call("get_region", loc);
				ERR_CONTINUE(!region);
				Ref<Image> map = region->call("get_height_map");
				ERR_CONTINUE(map.is_null() || map->get_format() != Image::FORMAT_RF || map->get_width() != rs || map->get_height() != rs);
				Vector<uint8_t> bytes = map->get_data();
				float *px = reinterpret_cast<float *>(bytes.ptrw());
				const int x_lo = MAX(c0.x - rx * rs, 0);
				const int x_hi = MIN(c1.x - rx * rs, rs - 1);
				const int z_lo = MAX(c0.y - rz * rs, 0);
				const int z_hi = MIN(c1.y - rz * rs, rs - 1);
				if (x_hi < x_lo || z_hi < z_lo) {
					continue;
				}
				const int w = x_hi - x_lo + 1;
				const int d = z_hi - z_lo + 1;
				LocalVector<float> block;
				block.resize(w * d);
				_fill((rx * rs + x_lo) * sp, (rz * rs + z_lo) * sp, sp, w, d, block.ptr());
				for (int z = 0; z < d; z++) {
					for (int x = 0; x < w; x++) {
						const float hgt = block[z * w + x];
						if (!Math::is_nan(hgt)) {
							px[(z_lo + z) * rs + x_lo + x] = hgt;
							written++;
						}
					}
				}
				map->set_data(rs, rs, false, Image::FORMAT_RF, bytes);
				grid.touched_regions.insert(loc);
			}
		}
		TerrainDeformer3D::commit_grid(grid, nullptr);
		if (added) {
			grid.data->call("update_maps", 3 /* TYPE_MAX */, true, false);
		}
	} else if (grid.backend == TerrainDeformer3D::BACKEND_HEIGHT_MAP) {
		LocalVector<float> block;
		block.resize(grid.width * grid.depth);
		_fill(grid.origin.x, grid.origin.y, sp, grid.width, grid.depth, block.ptr());
		for (int z = 0; z < grid.depth; z++) {
			for (int x = 0; x < grid.width; x++) {
				const real_t wx = grid.origin.x + x * sp;
				const real_t wz = grid.origin.y + z * sp;
				if (wx < rect.position.x - tol || wz < rect.position.y - tol || wx > rect.position.x + rect.size.x + tol || wz > rect.position.y + rect.size.y + tol) {
					continue;
				}
				const float hgt = block[z * grid.width + x];
				if (!Math::is_nan(hgt)) {
					TerrainDeformer3D::grid_set(grid, Vector2i(x, z), hgt);
					written++;
				}
			}
		}
		TerrainDeformer3D::commit_grid(grid, mesh);
	}

	if (def) {
		def->apply();
	}
	last_write_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("written"), written);
	return written;
}

int TerrainGenerator3D::bake() {
	if (!generate()) {
		return 0;
	}
	const int written = write_to_terrain();
	print_line(vformat("TerrainGenerator3D: %dx%d grid, %d steps in %.0f ms; %d vertices written in %.0f ms.", nx, ny, last_steps, last_generate_msec, written, last_write_msec));
	return written;
}

Callable TerrainGenerator3D::get_bake_button() {
	return callable_mp(this, &TerrainGenerator3D::bake);
}

PackedStringArray TerrainGenerator3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (is_inside_tree() && !_get_terrain_node()) {
		warnings.push_back(RTR("Set terrain to a Terrain3D or a CollisionShape3D with a HeightMapShape3D (or set a deformer that has one)."));
	}
	if (detail_enabled && detail_octaves > 0 && is_inside_tree()) {
		TerrainDeformer3D::Grid grid;
		const real_t smallest = detail_wavelength / Math::pow(real_t(2.0), real_t(detail_octaves - 1));
		if (TerrainDeformer3D::open_grid(_get_terrain_node(), grid) && smallest < grid.spacing * 4.0) {
			warnings.push_back(vformat(RTR("The smallest detail wavelength (%.1f m) is under 4 terrain vertices (%.1f m): it will alias into spikes. Use fewer detail octaves or a longer wavelength."), smallest, grid.spacing * 4.0));
		}
	}
	if (size.x > 0.0 && size.y > 0.0 && (size.x / cell_size) * (size.y / cell_size) > 4.2e6) {
		warnings.push_back(RTR("More than 2048 x 2048 cells: generation takes minutes. Raise cell_size; finer detail is a later stage."));
	}
	return warnings;
}

void TerrainGenerator3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &TerrainGenerator3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &TerrainGenerator3D::get_size);
	ClassDB::bind_method(D_METHOD("set_cell_size", "cell_size"), &TerrainGenerator3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &TerrainGenerator3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &TerrainGenerator3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &TerrainGenerator3D::get_seed);

	ClassDB::bind_method(D_METHOD("set_uplift", "uplift"), &TerrainGenerator3D::set_uplift);
	ClassDB::bind_method(D_METHOD("get_uplift"), &TerrainGenerator3D::get_uplift);
	ClassDB::bind_method(D_METHOD("set_base_uplift", "strength"), &TerrainGenerator3D::set_base_uplift);
	ClassDB::bind_method(D_METHOD("get_base_uplift"), &TerrainGenerator3D::get_base_uplift);
	ClassDB::bind_method(D_METHOD("set_uplift_map", "map"), &TerrainGenerator3D::set_uplift_map);
	ClassDB::bind_method(D_METHOD("get_uplift_map"), &TerrainGenerator3D::get_uplift_map);
	ClassDB::bind_method(D_METHOD("set_ranges", "ranges"), &TerrainGenerator3D::set_ranges);
	ClassDB::bind_method(D_METHOD("get_ranges"), &TerrainGenerator3D::get_ranges);
	ClassDB::bind_method(D_METHOD("set_range_width", "width"), &TerrainGenerator3D::set_range_width);
	ClassDB::bind_method(D_METHOD("get_range_width"), &TerrainGenerator3D::get_range_width);
	ClassDB::bind_method(D_METHOD("set_uplift_noise", "noise"), &TerrainGenerator3D::set_uplift_noise);
	ClassDB::bind_method(D_METHOD("get_uplift_noise"), &TerrainGenerator3D::get_uplift_noise);
	ClassDB::bind_method(D_METHOD("set_uplift_noise_strength", "strength"), &TerrainGenerator3D::set_uplift_noise_strength);
	ClassDB::bind_method(D_METHOD("get_uplift_noise_strength"), &TerrainGenerator3D::get_uplift_noise_strength);

	ClassDB::bind_method(D_METHOD("set_erodibility", "erodibility"), &TerrainGenerator3D::set_erodibility);
	ClassDB::bind_method(D_METHOD("get_erodibility"), &TerrainGenerator3D::get_erodibility);
	ClassDB::bind_method(D_METHOD("set_area_exponent", "exponent"), &TerrainGenerator3D::set_area_exponent);
	ClassDB::bind_method(D_METHOD("get_area_exponent"), &TerrainGenerator3D::get_area_exponent);
	ClassDB::bind_method(D_METHOD("set_hillslope_diffusion", "diffusion"), &TerrainGenerator3D::set_hillslope_diffusion);
	ClassDB::bind_method(D_METHOD("get_hillslope_diffusion"), &TerrainGenerator3D::get_hillslope_diffusion);
	ClassDB::bind_method(D_METHOD("set_deposition", "deposition"), &TerrainGenerator3D::set_deposition);
	ClassDB::bind_method(D_METHOD("get_deposition"), &TerrainGenerator3D::get_deposition);
	ClassDB::bind_method(D_METHOD("set_critical_slope", "degrees"), &TerrainGenerator3D::set_critical_slope);
	ClassDB::bind_method(D_METHOD("get_critical_slope"), &TerrainGenerator3D::get_critical_slope);
	ClassDB::bind_method(D_METHOD("set_flow_exponent", "exponent"), &TerrainGenerator3D::set_flow_exponent);
	ClassDB::bind_method(D_METHOD("get_flow_exponent"), &TerrainGenerator3D::get_flow_exponent);
	ClassDB::bind_method(D_METHOD("set_time_step", "years"), &TerrainGenerator3D::set_time_step);
	ClassDB::bind_method(D_METHOD("get_time_step"), &TerrainGenerator3D::get_time_step);
	ClassDB::bind_method(D_METHOD("set_steps", "steps"), &TerrainGenerator3D::set_steps);
	ClassDB::bind_method(D_METHOD("get_steps"), &TerrainGenerator3D::get_steps);
	ClassDB::bind_method(D_METHOD("set_coarse_levels", "levels"), &TerrainGenerator3D::set_coarse_levels);
	ClassDB::bind_method(D_METHOD("get_coarse_levels"), &TerrainGenerator3D::get_coarse_levels);
	ClassDB::bind_method(D_METHOD("set_refine_steps", "steps"), &TerrainGenerator3D::set_refine_steps);
	ClassDB::bind_method(D_METHOD("get_refine_steps"), &TerrainGenerator3D::get_refine_steps);
	ClassDB::bind_method(D_METHOD("get_last_coarse_levels"), &TerrainGenerator3D::get_last_coarse_levels);
	ClassDB::bind_method(D_METHOD("set_initial_roughness", "metres"), &TerrainGenerator3D::set_initial_roughness);
	ClassDB::bind_method(D_METHOD("get_initial_roughness"), &TerrainGenerator3D::get_initial_roughness);
	ClassDB::bind_method(D_METHOD("set_outlet_edges", "edges"), &TerrainGenerator3D::set_outlet_edges);
	ClassDB::bind_method(D_METHOD("get_outlet_edges"), &TerrainGenerator3D::get_outlet_edges);
	ClassDB::bind_method(D_METHOD("set_sea_level", "height"), &TerrainGenerator3D::set_sea_level);
	ClassDB::bind_method(D_METHOD("get_sea_level"), &TerrainGenerator3D::get_sea_level);
	ClassDB::bind_method(D_METHOD("set_height_offset", "offset"), &TerrainGenerator3D::set_height_offset);
	ClassDB::bind_method(D_METHOD("get_height_offset"), &TerrainGenerator3D::get_height_offset);
	ClassDB::bind_method(D_METHOD("set_max_height", "height"), &TerrainGenerator3D::set_max_height);
	ClassDB::bind_method(D_METHOD("get_max_height"), &TerrainGenerator3D::get_max_height);

	ClassDB::bind_method(D_METHOD("set_detail_enabled", "enabled"), &TerrainGenerator3D::set_detail_enabled);
	ClassDB::bind_method(D_METHOD("is_detail_enabled"), &TerrainGenerator3D::is_detail_enabled);
	ClassDB::bind_method(D_METHOD("set_detail_wavelength", "metres"), &TerrainGenerator3D::set_detail_wavelength);
	ClassDB::bind_method(D_METHOD("get_detail_wavelength"), &TerrainGenerator3D::get_detail_wavelength);
	ClassDB::bind_method(D_METHOD("set_detail_octaves", "octaves"), &TerrainGenerator3D::set_detail_octaves);
	ClassDB::bind_method(D_METHOD("get_detail_octaves"), &TerrainGenerator3D::get_detail_octaves);
	ClassDB::bind_method(D_METHOD("set_detail_depth", "depth"), &TerrainGenerator3D::set_detail_depth);
	ClassDB::bind_method(D_METHOD("get_detail_depth"), &TerrainGenerator3D::get_detail_depth);
	ClassDB::bind_method(D_METHOD("set_detail_reach", "reach"), &TerrainGenerator3D::set_detail_reach);
	ClassDB::bind_method(D_METHOD("get_detail_reach"), &TerrainGenerator3D::get_detail_reach);
	ClassDB::bind_method(D_METHOD("set_detail_gully_weight", "weight"), &TerrainGenerator3D::set_detail_gully_weight);
	ClassDB::bind_method(D_METHOD("get_detail_gully_weight"), &TerrainGenerator3D::get_detail_gully_weight);
	ClassDB::bind_method(D_METHOD("set_detail_crest", "amount"), &TerrainGenerator3D::set_detail_crest);
	ClassDB::bind_method(D_METHOD("get_detail_crest"), &TerrainGenerator3D::get_detail_crest);
	ClassDB::bind_method(D_METHOD("set_detail_assumed_slope", "slope"), &TerrainGenerator3D::set_detail_assumed_slope);
	ClassDB::bind_method(D_METHOD("get_detail_assumed_slope"), &TerrainGenerator3D::get_detail_assumed_slope);

	ClassDB::bind_method(D_METHOD("set_terrain", "path"), &TerrainGenerator3D::set_terrain);
	ClassDB::bind_method(D_METHOD("get_terrain"), &TerrainGenerator3D::get_terrain);
	ClassDB::bind_method(D_METHOD("set_deformer", "path"), &TerrainGenerator3D::set_deformer);
	ClassDB::bind_method(D_METHOD("get_deformer"), &TerrainGenerator3D::get_deformer);
	ClassDB::bind_method(D_METHOD("set_ground_mesh", "path"), &TerrainGenerator3D::set_ground_mesh);
	ClassDB::bind_method(D_METHOD("get_ground_mesh"), &TerrainGenerator3D::get_ground_mesh);

	ClassDB::bind_method(D_METHOD("get_world_rect"), &TerrainGenerator3D::get_world_rect);
	ClassDB::bind_method(D_METHOD("get_grid_size"), &TerrainGenerator3D::get_grid_size);
	ClassDB::bind_method(D_METHOD("generate"), &TerrainGenerator3D::generate);
	ClassDB::bind_method(D_METHOD("write_to_terrain"), &TerrainGenerator3D::write_to_terrain);
	ClassDB::bind_method(D_METHOD("bake"), &TerrainGenerator3D::bake);
	ClassDB::bind_method(D_METHOD("has_result"), &TerrainGenerator3D::has_result);
	ClassDB::bind_method(D_METHOD("clear_result"), &TerrainGenerator3D::clear_result);
	ClassDB::bind_method(D_METHOD("get_height", "global_position"), &TerrainGenerator3D::get_height);
	ClassDB::bind_method(D_METHOD("get_base_height", "global_position"), &TerrainGenerator3D::get_base_height);
	ClassDB::bind_method(D_METHOD("sample_heights", "origin", "spacing", "size"), &TerrainGenerator3D::sample_heights);
	ClassDB::bind_method(D_METHOD("get_tile_flow", "global_position"), &TerrainGenerator3D::get_tile_flow);
	ClassDB::bind_method(D_METHOD("get_tile_deposit", "global_position"), &TerrainGenerator3D::get_tile_deposit);
	ClassDB::bind_method(D_METHOD("get_last_tile_erosion_msec"), &TerrainGenerator3D::get_last_tile_erosion_msec);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_enabled", "enable"), &TerrainGenerator3D::set_tile_erosion_enabled);
	ClassDB::bind_method(D_METHOD("is_tile_erosion_enabled"), &TerrainGenerator3D::is_tile_erosion_enabled);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_spacing", "spacing"), &TerrainGenerator3D::set_tile_erosion_spacing);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_spacing"), &TerrainGenerator3D::get_tile_erosion_spacing);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_tile_size", "size"), &TerrainGenerator3D::set_tile_erosion_tile_size);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_tile_size"), &TerrainGenerator3D::get_tile_erosion_tile_size);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_apron", "apron"), &TerrainGenerator3D::set_tile_erosion_apron);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_apron"), &TerrainGenerator3D::get_tile_erosion_apron);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_droplets", "droplets"), &TerrainGenerator3D::set_tile_erosion_droplets);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_droplets"), &TerrainGenerator3D::get_tile_erosion_droplets);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_max_depth", "depth"), &TerrainGenerator3D::set_tile_erosion_max_depth);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_max_depth"), &TerrainGenerator3D::get_tile_erosion_max_depth);
	ClassDB::bind_method(D_METHOD("set_tile_erosion_rock_hardness", "hardness"), &TerrainGenerator3D::set_tile_erosion_rock_hardness);
	ClassDB::bind_method(D_METHOD("get_tile_erosion_rock_hardness"), &TerrainGenerator3D::get_tile_erosion_rock_hardness);
	ClassDB::bind_method(D_METHOD("get_soil_depth", "global_position"), &TerrainGenerator3D::get_soil_depth);
	ClassDB::bind_method(D_METHOD("get_rock_exposure", "global_position"), &TerrainGenerator3D::get_rock_exposure);
	ClassDB::bind_method(D_METHOD("get_talus", "global_position"), &TerrainGenerator3D::get_talus);
	ClassDB::bind_method(D_METHOD("get_geology_at", "global_position"), &TerrainGenerator3D::get_geology_at);
	ClassDB::bind_method(D_METHOD("set_cliff_ledge_spacing", "value"), &TerrainGenerator3D::set_cliff_ledge_spacing);
	ClassDB::bind_method(D_METHOD("get_cliff_ledge_spacing"), &TerrainGenerator3D::get_cliff_ledge_spacing);
	ClassDB::bind_method(D_METHOD("set_cliff_ledge_strength", "value"), &TerrainGenerator3D::set_cliff_ledge_strength);
	ClassDB::bind_method(D_METHOD("get_cliff_ledge_strength"), &TerrainGenerator3D::get_cliff_ledge_strength);
	ClassDB::bind_method(D_METHOD("set_glacial_enabled", "value"), &TerrainGenerator3D::set_glacial_enabled);
	ClassDB::bind_method(D_METHOD("is_glacial_enabled"), &TerrainGenerator3D::is_glacial_enabled);
	ClassDB::bind_method(D_METHOD("set_glacial_ela", "value"), &TerrainGenerator3D::set_glacial_ela);
	ClassDB::bind_method(D_METHOD("get_glacial_ela"), &TerrainGenerator3D::get_glacial_ela);
	ClassDB::bind_method(D_METHOD("set_glacial_share", "value"), &TerrainGenerator3D::set_glacial_share);
	ClassDB::bind_method(D_METHOD("get_glacial_share"), &TerrainGenerator3D::get_glacial_share);
	ClassDB::bind_method(D_METHOD("set_glacial_final", "value"), &TerrainGenerator3D::set_glacial_final);
	ClassDB::bind_method(D_METHOD("get_glacial_final"), &TerrainGenerator3D::get_glacial_final);
	ClassDB::bind_method(D_METHOD("set_glacial_erosion", "value"), &TerrainGenerator3D::set_glacial_erosion);
	ClassDB::bind_method(D_METHOD("get_glacial_erosion"), &TerrainGenerator3D::get_glacial_erosion);
	ClassDB::bind_method(D_METHOD("set_glacial_rock_variation", "value"), &TerrainGenerator3D::set_glacial_rock_variation);
	ClassDB::bind_method(D_METHOD("get_glacial_rock_variation"), &TerrainGenerator3D::get_glacial_rock_variation);
	ClassDB::bind_method(D_METHOD("set_glacial_rock_scale", "value"), &TerrainGenerator3D::set_glacial_rock_scale);
	ClassDB::bind_method(D_METHOD("get_glacial_rock_scale"), &TerrainGenerator3D::get_glacial_rock_scale);
	ClassDB::bind_method(D_METHOD("set_glacial_inflow_balance", "value"), &TerrainGenerator3D::set_glacial_inflow_balance);
	ClassDB::bind_method(D_METHOD("get_glacial_inflow_balance"), &TerrainGenerator3D::get_glacial_inflow_balance);
	ClassDB::bind_method(D_METHOD("set_glacial_coarse_cell", "metres"), &TerrainGenerator3D::set_glacial_coarse_cell);
	ClassDB::bind_method(D_METHOD("get_glacial_coarse_cell"), &TerrainGenerator3D::get_glacial_coarse_cell);
	ClassDB::bind_method(D_METHOD("set_ice_worn_detail", "enabled"), &TerrainGenerator3D::set_ice_worn_detail);
	ClassDB::bind_method(D_METHOD("is_ice_worn_detail"), &TerrainGenerator3D::is_ice_worn_detail);
	ClassDB::bind_method(D_METHOD("set_glacial_sliding", "value"), &TerrainGenerator3D::set_glacial_sliding);
	ClassDB::bind_method(D_METHOD("get_glacial_sliding"), &TerrainGenerator3D::get_glacial_sliding);
	ClassDB::bind_method(D_METHOD("get_last_glacial_eroded"), &TerrainGenerator3D::get_last_glacial_eroded);
	ClassDB::bind_method(D_METHOD("get_last_glacial_ice_area"), &TerrainGenerator3D::get_last_glacial_ice_area);
	ClassDB::bind_method(D_METHOD("get_last_glacial_msec"), &TerrainGenerator3D::get_last_glacial_msec);
	ClassDB::bind_method(D_METHOD("get_glacier_depth", "global_position"), &TerrainGenerator3D::get_glacier_depth);
	ClassDB::bind_method(D_METHOD("set_strata_enabled", "value"), &TerrainGenerator3D::set_strata_enabled);
	ClassDB::bind_method(D_METHOD("is_strata_enabled"), &TerrainGenerator3D::is_strata_enabled);
	ClassDB::bind_method(D_METHOD("set_strata_spacing", "value"), &TerrainGenerator3D::set_strata_spacing);
	ClassDB::bind_method(D_METHOD("get_strata_spacing"), &TerrainGenerator3D::get_strata_spacing);
	ClassDB::bind_method(D_METHOD("set_strata_hard_fraction", "value"), &TerrainGenerator3D::set_strata_hard_fraction);
	ClassDB::bind_method(D_METHOD("get_strata_hard_fraction"), &TerrainGenerator3D::get_strata_hard_fraction);
	ClassDB::bind_method(D_METHOD("set_strata_hardness", "value"), &TerrainGenerator3D::set_strata_hardness);
	ClassDB::bind_method(D_METHOD("get_strata_hardness"), &TerrainGenerator3D::get_strata_hardness);
	ClassDB::bind_method(D_METHOD("set_strata_cliff_angle", "value"), &TerrainGenerator3D::set_strata_cliff_angle);
	ClassDB::bind_method(D_METHOD("get_strata_cliff_angle"), &TerrainGenerator3D::get_strata_cliff_angle);
	ClassDB::bind_method(D_METHOD("set_strata_dip", "value"), &TerrainGenerator3D::set_strata_dip);
	ClassDB::bind_method(D_METHOD("get_strata_dip"), &TerrainGenerator3D::get_strata_dip);
	ClassDB::bind_method(D_METHOD("set_strata_dip_direction", "value"), &TerrainGenerator3D::set_strata_dip_direction);
	ClassDB::bind_method(D_METHOD("get_strata_dip_direction"), &TerrainGenerator3D::get_strata_dip_direction);
	ClassDB::bind_method(D_METHOD("set_strata_wobble", "value"), &TerrainGenerator3D::set_strata_wobble);
	ClassDB::bind_method(D_METHOD("get_strata_wobble"), &TerrainGenerator3D::get_strata_wobble);
	ClassDB::bind_method(D_METHOD("set_strata_channel_area", "area"), &TerrainGenerator3D::set_strata_channel_area);
	ClassDB::bind_method(D_METHOD("get_strata_channel_area"), &TerrainGenerator3D::get_strata_channel_area);
	ClassDB::bind_method(D_METHOD("get_rock_hardness", "global_position"), &TerrainGenerator3D::get_rock_hardness);
	ClassDB::bind_method(D_METHOD("get_waterfalls", "min_area", "min_height"), &TerrainGenerator3D::get_waterfalls, DEFVAL(1.0e5), DEFVAL(5.0));
	ClassDB::bind_method(D_METHOD("get_river_sites", "min_area", "min_discharge", "run"), &TerrainGenerator3D::get_river_sites, DEFVAL(1.0e5), DEFVAL(0.0), DEFVAL(200.0));
	ClassDB::bind_method(D_METHOD("get_geology_image"), &TerrainGenerator3D::get_geology_image);
	ClassDB::bind_method(D_METHOD("get_geology_texture"), &TerrainGenerator3D::get_geology_texture);
	ClassDB::bind_static_method("TerrainGenerator3D", D_METHOD("get_geology_shader_code"), &TerrainGenerator3D::get_geology_shader_code);
	ClassDB::bind_method(D_METHOD("get_detail_shader_code"), &TerrainGenerator3D::get_detail_shader_code);
	ClassDB::bind_method(D_METHOD("get_discharge", "area"), &TerrainGenerator3D::get_discharge);
	ClassDB::bind_method(D_METHOD("set_climate_recipe", "recipe"), &TerrainGenerator3D::set_climate_recipe);
	ClassDB::bind_method(D_METHOD("get_climate_recipe"), &TerrainGenerator3D::get_climate_recipe);
	ClassDB::bind_method(D_METHOD("set_river_month", "month"), &TerrainGenerator3D::set_river_month);
	ClassDB::bind_method(D_METHOD("get_river_month"), &TerrainGenerator3D::get_river_month);
	ClassDB::bind_method(D_METHOD("has_runoff"), &TerrainGenerator3D::has_runoff);
	ClassDB::bind_method(D_METHOD("get_last_runoff_msec"), &TerrainGenerator3D::get_last_runoff_msec);
	ClassDB::bind_method(D_METHOD("get_mean_runoff"), &TerrainGenerator3D::get_mean_runoff);
	ClassDB::bind_method(D_METHOD("get_runoff_at", "global_position", "month"), &TerrainGenerator3D::get_runoff_at, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("get_snow_water_at", "global_position", "month"), &TerrainGenerator3D::get_snow_water_at);
	ClassDB::bind_method(D_METHOD("get_discharge_at", "global_position", "month"), &TerrainGenerator3D::get_discharge_at, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("set_ocean_edges", "edges"), &TerrainGenerator3D::set_ocean_edges);
	ClassDB::bind_method(D_METHOD("get_ocean_edges"), &TerrainGenerator3D::get_ocean_edges);
	ClassDB::bind_method(D_METHOD("set_coast_distance", "distance"), &TerrainGenerator3D::set_coast_distance);
	ClassDB::bind_method(D_METHOD("get_coast_distance"), &TerrainGenerator3D::get_coast_distance);
	ClassDB::bind_method(D_METHOD("set_coast_roughness", "distance"), &TerrainGenerator3D::set_coast_roughness);
	ClassDB::bind_method(D_METHOD("get_coast_roughness"), &TerrainGenerator3D::get_coast_roughness);
	ClassDB::bind_method(D_METHOD("set_coast_wavelength", "distance"), &TerrainGenerator3D::set_coast_wavelength);
	ClassDB::bind_method(D_METHOD("get_coast_wavelength"), &TerrainGenerator3D::get_coast_wavelength);
	ClassDB::bind_method(D_METHOD("set_sea_map", "map"), &TerrainGenerator3D::set_sea_map);
	ClassDB::bind_method(D_METHOD("get_sea_map"), &TerrainGenerator3D::get_sea_map);
	ClassDB::bind_method(D_METHOD("set_seabed_profile", "value"), &TerrainGenerator3D::set_seabed_profile);
	ClassDB::bind_method(D_METHOD("get_seabed_profile"), &TerrainGenerator3D::get_seabed_profile);
	ClassDB::bind_method(D_METHOD("set_shelf_slope", "value"), &TerrainGenerator3D::set_shelf_slope);
	ClassDB::bind_method(D_METHOD("get_shelf_slope"), &TerrainGenerator3D::get_shelf_slope);
	ClassDB::bind_method(D_METHOD("set_max_sea_depth", "depth"), &TerrainGenerator3D::set_max_sea_depth);
	ClassDB::bind_method(D_METHOD("get_max_sea_depth"), &TerrainGenerator3D::get_max_sea_depth);
	ClassDB::bind_method(D_METHOD("set_beach_width", "width"), &TerrainGenerator3D::set_beach_width);
	ClassDB::bind_method(D_METHOD("get_beach_width"), &TerrainGenerator3D::get_beach_width);
	ClassDB::bind_method(D_METHOD("set_beach_slope", "slope"), &TerrainGenerator3D::set_beach_slope);
	ClassDB::bind_method(D_METHOD("get_beach_slope"), &TerrainGenerator3D::get_beach_slope);
	ClassDB::bind_method(D_METHOD("set_lake_count", "count"), &TerrainGenerator3D::set_lake_count);
	ClassDB::bind_method(D_METHOD("get_lake_count"), &TerrainGenerator3D::get_lake_count);
	ClassDB::bind_method(D_METHOD("get_boulders", "rect", "density"), &TerrainGenerator3D::get_boulders, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("set_spray_cell", "metres"), &TerrainGenerator3D::set_spray_cell);
	ClassDB::bind_method(D_METHOD("get_spray_cell"), &TerrainGenerator3D::get_spray_cell);
	ClassDB::bind_method(D_METHOD("get_spray_potential", "global_position"), &TerrainGenerator3D::get_spray_potential);
	ClassDB::bind_method(D_METHOD("get_spray_image"), &TerrainGenerator3D::get_spray_image);
	ClassDB::bind_method(D_METHOD("set_sky_occlusion", "enable"), &TerrainGenerator3D::set_sky_occlusion);
	ClassDB::bind_method(D_METHOD("is_sky_occlusion_enabled"), &TerrainGenerator3D::is_sky_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("set_sky_cell", "metres"), &TerrainGenerator3D::set_sky_cell);
	ClassDB::bind_method(D_METHOD("get_sky_cell"), &TerrainGenerator3D::get_sky_cell);
	ClassDB::bind_method(D_METHOD("set_sky_distance", "metres"), &TerrainGenerator3D::set_sky_distance);
	ClassDB::bind_method(D_METHOD("get_sky_distance"), &TerrainGenerator3D::get_sky_distance);
	ClassDB::bind_method(D_METHOD("get_sky_visibility", "global_position"), &TerrainGenerator3D::get_sky_visibility);
	ClassDB::bind_method(D_METHOD("get_sky_image", "layer"), &TerrainGenerator3D::get_sky_image, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_last_sky_msec"), &TerrainGenerator3D::get_last_sky_msec);
	ClassDB::bind_method(D_METHOD("get_last_sky_light_msec"), &TerrainGenerator3D::get_last_sky_light_msec);
	ClassDB::bind_method(D_METHOD("update_sky_light", "panorama"), &TerrainGenerator3D::update_sky_light);
	ClassDB::bind_method(D_METHOD("set_sky_light_source", "path"), &TerrainGenerator3D::set_sky_light_source);
	ClassDB::bind_method(D_METHOD("get_sky_light_source"), &TerrainGenerator3D::get_sky_light_source);
	ClassDB::bind_method(D_METHOD("set_sky_light_interval", "seconds"), &TerrainGenerator3D::set_sky_light_interval);
	ClassDB::bind_method(D_METHOD("get_sky_light_interval"), &TerrainGenerator3D::get_sky_light_interval);
	ClassDB::bind_method(D_METHOD("set_gorge_age", "years"), &TerrainGenerator3D::set_gorge_age);
	ClassDB::bind_method(D_METHOD("get_gorge_age"), &TerrainGenerator3D::get_gorge_age);
	ClassDB::bind_method(D_METHOD("set_moraine_age", "years"), &TerrainGenerator3D::set_moraine_age);
	ClassDB::bind_method(D_METHOD("get_moraine_age"), &TerrainGenerator3D::get_moraine_age);
	ClassDB::bind_method(D_METHOD("get_last_moraine_volume"), &TerrainGenerator3D::get_last_moraine_volume);
	ClassDB::bind_method(D_METHOD("set_scree_age", "years"), &TerrainGenerator3D::set_scree_age);
	ClassDB::bind_method(D_METHOD("get_scree_age"), &TerrainGenerator3D::get_scree_age);
	ClassDB::bind_method(D_METHOD("get_last_scree_volume"), &TerrainGenerator3D::get_last_scree_volume);
	ClassDB::bind_method(D_METHOD("set_meander_age", "years"), &TerrainGenerator3D::set_meander_age);
	ClassDB::bind_method(D_METHOD("get_meander_age"), &TerrainGenerator3D::get_meander_age);
	ClassDB::bind_method(D_METHOD("get_last_meander_count"), &TerrainGenerator3D::get_last_meander_count);
	ClassDB::bind_method(D_METHOD("get_last_oxbow_count"), &TerrainGenerator3D::get_last_oxbow_count);
	ClassDB::bind_method(D_METHOD("set_outwash_age", "years"), &TerrainGenerator3D::set_outwash_age);
	ClassDB::bind_method(D_METHOD("get_outwash_age"), &TerrainGenerator3D::get_outwash_age);
	ClassDB::bind_method(D_METHOD("get_last_braid_count"), &TerrainGenerator3D::get_last_braid_count);
	ClassDB::bind_method(D_METHOD("get_braidplains"), &TerrainGenerator3D::get_braidplains);
	ClassDB::bind_method(D_METHOD("set_gorge_rate", "rate"), &TerrainGenerator3D::set_gorge_rate);
	ClassDB::bind_method(D_METHOD("get_gorge_rate"), &TerrainGenerator3D::get_gorge_rate);
	ClassDB::bind_method(D_METHOD("set_fan_backfill", "enable"), &TerrainGenerator3D::set_fan_backfill);
	ClassDB::bind_method(D_METHOD("get_fan_backfill"), &TerrainGenerator3D::get_fan_backfill);
	ClassDB::bind_method(D_METHOD("get_last_slots_filled"), &TerrainGenerator3D::get_last_slots_filled);
	ClassDB::bind_method(D_METHOD("set_plunge_pools", "enable"), &TerrainGenerator3D::set_plunge_pools);
	ClassDB::bind_method(D_METHOD("get_plunge_pools"), &TerrainGenerator3D::get_plunge_pools);
	ClassDB::bind_method(D_METHOD("set_fall_lips", "enable"), &TerrainGenerator3D::set_fall_lips);
	ClassDB::bind_method(D_METHOD("get_fall_lips"), &TerrainGenerator3D::get_fall_lips);
	ClassDB::bind_method(D_METHOD("set_inflows", "markers"), &TerrainGenerator3D::set_inflows);
	ClassDB::bind_method(D_METHOD("get_inflows"), &TerrainGenerator3D::get_inflows);
	ClassDB::bind_method(D_METHOD("set_inflow_area", "km2"), &TerrainGenerator3D::set_inflow_area);
	ClassDB::bind_method(D_METHOD("get_inflow_area"), &TerrainGenerator3D::get_inflow_area);
	ClassDB::bind_method(D_METHOD("get_inflow_cells"), &TerrainGenerator3D::get_inflow_cells);
	ClassDB::bind_method(D_METHOD("set_lakes", "markers"), &TerrainGenerator3D::set_lakes);
	ClassDB::bind_method(D_METHOD("get_lakes"), &TerrainGenerator3D::get_lakes);
	ClassDB::bind_method(D_METHOD("set_lake_depth", "depth"), &TerrainGenerator3D::set_lake_depth);
	ClassDB::bind_method(D_METHOD("get_lake_depth"), &TerrainGenerator3D::get_lake_depth);
	ClassDB::bind_method(D_METHOD("set_lake_catchment", "area"), &TerrainGenerator3D::set_lake_catchment);
	ClassDB::bind_method(D_METHOD("get_lake_catchment"), &TerrainGenerator3D::get_lake_catchment);
	ClassDB::bind_method(D_METHOD("set_lake_min_area", "area"), &TerrainGenerator3D::set_lake_min_area);
	ClassDB::bind_method(D_METHOD("get_lake_min_area"), &TerrainGenerator3D::get_lake_min_area);
	ClassDB::bind_method(D_METHOD("set_lake_spacing", "distance"), &TerrainGenerator3D::set_lake_spacing);
	ClassDB::bind_method(D_METHOD("get_lake_spacing"), &TerrainGenerator3D::get_lake_spacing);
	ClassDB::bind_method(D_METHOD("set_lake_evaporation", "rate"), &TerrainGenerator3D::set_lake_evaporation);
	ClassDB::bind_method(D_METHOD("get_lake_evaporation"), &TerrainGenerator3D::get_lake_evaporation);
	ClassDB::bind_method(D_METHOD("has_sea_result"), &TerrainGenerator3D::has_sea_result);
	ClassDB::bind_method(D_METHOD("get_last_water_msec"), &TerrainGenerator3D::get_last_water_msec);
	ClassDB::bind_method(D_METHOD("get_lake_result_count"), &TerrainGenerator3D::get_lake_result_count);
	ClassDB::bind_method(D_METHOD("get_lake_results"), &TerrainGenerator3D::get_lake_results);
	ClassDB::bind_method(D_METHOD("get_water_level", "global_position"), &TerrainGenerator3D::get_water_level);
	ClassDB::bind_method(D_METHOD("get_water_depth", "global_position"), &TerrainGenerator3D::get_water_depth);
	ClassDB::bind_method(D_METHOD("get_water_image"), &TerrainGenerator3D::get_water_image);
	ClassDB::bind_method(D_METHOD("build_water", "parent"), &TerrainGenerator3D::build_water);
	ClassDB::bind_method(D_METHOD("get_rivers", "min_area"), &TerrainGenerator3D::get_rivers);
	ClassDB::bind_method(D_METHOD("build_river_paths", "parent", "min_area"), &TerrainGenerator3D::build_river_paths);
	ClassDB::bind_method(D_METHOD("set_soil_production", "rate"), &TerrainGenerator3D::set_soil_production);
	ClassDB::bind_method(D_METHOD("get_soil_production"), &TerrainGenerator3D::get_soil_production);
	ClassDB::bind_method(D_METHOD("set_soil_depth_scale", "depth"), &TerrainGenerator3D::set_soil_depth_scale);
	ClassDB::bind_method(D_METHOD("get_soil_depth_scale"), &TerrainGenerator3D::get_soil_depth_scale);
	ClassDB::bind_method(D_METHOD("set_max_soil_depth", "depth"), &TerrainGenerator3D::set_max_soil_depth);
	ClassDB::bind_method(D_METHOD("get_max_soil_depth"), &TerrainGenerator3D::get_max_soil_depth);
	ClassDB::bind_method(D_METHOD("set_talus_angle", "degrees"), &TerrainGenerator3D::set_talus_angle);
	ClassDB::bind_method(D_METHOD("get_talus_angle"), &TerrainGenerator3D::get_talus_angle);
	ClassDB::bind_method(D_METHOD("set_talus_reach", "distance"), &TerrainGenerator3D::set_talus_reach);
	ClassDB::bind_method(D_METHOD("get_talus_reach"), &TerrainGenerator3D::get_talus_reach);
	ClassDB::bind_method(D_METHOD("set_river_runoff", "runoff"), &TerrainGenerator3D::set_river_runoff);
	ClassDB::bind_method(D_METHOD("get_river_runoff"), &TerrainGenerator3D::get_river_runoff);
	ClassDB::bind_method(D_METHOD("set_river_width_coefficient", "coefficient"), &TerrainGenerator3D::set_river_width_coefficient);
	ClassDB::bind_method(D_METHOD("get_river_width_coefficient"), &TerrainGenerator3D::get_river_width_coefficient);
	ClassDB::bind_method(D_METHOD("set_publish_shader_globals", "enable"), &TerrainGenerator3D::set_publish_shader_globals);
	ClassDB::bind_method(D_METHOD("is_publishing_shader_globals"), &TerrainGenerator3D::is_publishing_shader_globals);
	ClassDB::bind_method(D_METHOD("get_drainage_area", "global_position"), &TerrainGenerator3D::get_drainage_area);
	ClassDB::bind_method(D_METHOD("get_sediment_depth", "global_position"), &TerrainGenerator3D::get_sediment_depth);
	ClassDB::bind_method(D_METHOD("get_height_image"), &TerrainGenerator3D::get_height_image);
	ClassDB::bind_method(D_METHOD("get_drainage_image"), &TerrainGenerator3D::get_drainage_image);
	ClassDB::bind_method(D_METHOD("get_sediment_image"), &TerrainGenerator3D::get_sediment_image);
	ClassDB::bind_method(D_METHOD("get_uplift_image"), &TerrainGenerator3D::get_uplift_image);
	ClassDB::bind_method(D_METHOD("get_last_generate_msec"), &TerrainGenerator3D::get_last_generate_msec);
	ClassDB::bind_method(D_METHOD("get_last_write_msec"), &TerrainGenerator3D::get_last_write_msec);
	ClassDB::bind_method(D_METHOD("get_last_steps"), &TerrainGenerator3D::get_last_steps);
	ClassDB::bind_method(D_METHOD("get_last_deposition_passes"), &TerrainGenerator3D::get_last_deposition_passes);
	ClassDB::bind_method(D_METHOD("get_last_change"), &TerrainGenerator3D::get_last_change);
	ClassDB::bind_method(D_METHOD("get_bake_button"), &TerrainGenerator3D::get_bake_button);

	ADD_PROPERTY(PropertyInfo(Variant::CALLABLE, "generate_button", PROPERTY_HINT_TOOL_BUTTON, "Generate,Terrain", PROPERTY_USAGE_EDITOR), "", "get_bake_button");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.25,256,0.25,or_greater,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");

	ADD_GROUP("Uplift", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uplift", PROPERTY_HINT_RANGE, "0,20,0.01,or_greater,suffix:mm/yr"), "set_uplift", "get_uplift");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_uplift", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_base_uplift", "get_base_uplift");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "uplift_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_uplift_map", "get_uplift_map");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "ranges", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_ranges", "get_ranges");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range_width", PROPERTY_HINT_RANGE, "1,10000,1,or_greater,suffix:m"), "set_range_width", "get_range_width");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "uplift_noise", PROPERTY_HINT_RESOURCE_TYPE, "Noise"), "set_uplift_noise", "get_uplift_noise");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uplift_noise_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_uplift_noise_strength", "get_uplift_noise_strength");

	ADD_GROUP("Erosion", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "erodibility", PROPERTY_HINT_RANGE, "0,0.001,0.0000001,or_greater,exp"), "set_erodibility", "get_erodibility");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "area_exponent", PROPERTY_HINT_RANGE, "0.1,1,0.01"), "set_area_exponent", "get_area_exponent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hillslope_diffusion", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m²/yr"), "set_hillslope_diffusion", "get_hillslope_diffusion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "deposition", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_deposition", "get_deposition");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "critical_slope", PROPERTY_HINT_RANGE, "0,80,0.1,degrees"), "set_critical_slope", "get_critical_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_exponent", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_flow_exponent", "get_flow_exponent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_step", PROPERTY_HINT_RANGE, "1,1000000,1,or_greater,suffix:yr"), "set_time_step", "get_time_step");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "steps", PROPERTY_HINT_RANGE, "0,5000,1,or_greater"), "set_steps", "get_steps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "coarse_levels", PROPERTY_HINT_RANGE, "-1,8,1"), "set_coarse_levels", "get_coarse_levels");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "refine_steps", PROPERTY_HINT_RANGE, "0,1000,1,or_greater"), "set_refine_steps", "get_refine_steps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "initial_roughness", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_initial_roughness", "get_initial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "outlet_edges", PROPERTY_HINT_FLAGS, "-X,+X,-Z,+Z"), "set_outlet_edges", "get_outlet_edges");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sea_level", PROPERTY_HINT_NONE, "suffix:m"), "set_sea_level", "get_sea_level");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_height_offset", "get_height_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_height", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_max_height", "get_max_height");

	ADD_GROUP("Cliffs", "cliff_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cliff_ledge_spacing", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m"), "set_cliff_ledge_spacing", "get_cliff_ledge_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cliff_ledge_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_cliff_ledge_strength", "get_cliff_ledge_strength");
	ADD_GROUP("Detail", "detail_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "detail_enabled"), "set_detail_enabled", "is_detail_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_wavelength", PROPERTY_HINT_RANGE, "1,2000,1,or_greater,suffix:m"), "set_detail_wavelength", "get_detail_wavelength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_octaves", PROPERTY_HINT_RANGE, "0,12,1"), "set_detail_octaves", "get_detail_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_depth", PROPERTY_HINT_RANGE, "0,0.5,0.001"), "set_detail_depth", "get_detail_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_reach", PROPERTY_HINT_RANGE, "0.1,5,0.01"), "set_detail_reach", "get_detail_reach");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_gully_weight", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_detail_gully_weight", "get_detail_gully_weight");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_crest", PROPERTY_HINT_RANGE, "0,3,0.01"), "set_detail_crest", "get_detail_crest");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_assumed_slope", PROPERTY_HINT_RANGE, "0.01,3,0.01"), "set_detail_assumed_slope", "get_detail_assumed_slope");

	ADD_GROUP("Tile Erosion", "tile_erosion_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tile_erosion_enabled"), "set_tile_erosion_enabled", "is_tile_erosion_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_spacing", PROPERTY_HINT_RANGE, "0.25,16,0.25,or_greater,suffix:m"), "set_tile_erosion_spacing", "get_tile_erosion_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_tile_size", PROPERTY_HINT_RANGE, "16,4096,1,or_greater,suffix:m"), "set_tile_erosion_tile_size", "get_tile_erosion_tile_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_apron", PROPERTY_HINT_RANGE, "0,1024,1,or_greater,suffix:m"), "set_tile_erosion_apron", "get_tile_erosion_apron");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_droplets", PROPERTY_HINT_RANGE, "0,8,0.05,or_greater"), "set_tile_erosion_droplets", "get_tile_erosion_droplets");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_max_depth", PROPERTY_HINT_RANGE, "0.1,100,0.1,or_greater,suffix:m"), "set_tile_erosion_max_depth", "get_tile_erosion_max_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tile_erosion_rock_hardness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_tile_erosion_rock_hardness", "get_tile_erosion_rock_hardness");

	ADD_GROUP("Geology", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soil_production", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:mm/yr"), "set_soil_production", "get_soil_production");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "soil_depth_scale", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater,suffix:m"), "set_soil_depth_scale", "get_soil_depth_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_soil_depth", PROPERTY_HINT_RANGE, "0,20,0.1,or_greater,suffix:m"), "set_max_soil_depth", "get_max_soil_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "talus_angle", PROPERTY_HINT_RANGE, "5,80,0.5,degrees"), "set_talus_angle", "get_talus_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "talus_reach", PROPERTY_HINT_RANGE, "1,1000,1,or_greater,suffix:m"), "set_talus_reach", "get_talus_reach");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "climate_recipe", PROPERTY_HINT_RESOURCE_TYPE, "ClimateRecipe"), "set_climate_recipe", "get_climate_recipe");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "river_month", PROPERTY_HINT_ENUM, "Annual mean:-1,January:0,February:1,March:2,April:3,May:4,June:5,July:6,August:7,September:8,October:9,November:10,December:11"), "set_river_month", "get_river_month");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "river_runoff", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:mm/yr"), "set_river_runoff", "get_river_runoff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "river_width_coefficient", PROPERTY_HINT_RANGE, "0,50,0.1,or_greater"), "set_river_width_coefficient", "get_river_width_coefficient");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "publish_shader_globals"), "set_publish_shader_globals", "is_publishing_shader_globals");

	ADD_GROUP("Strata", "strata_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "strata_enabled"), "set_strata_enabled", "is_strata_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_spacing", PROPERTY_HINT_RANGE, "1,1000,0.1,or_greater,suffix:m"), "set_strata_spacing", "get_strata_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_hard_fraction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_strata_hard_fraction", "get_strata_hard_fraction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_hardness", PROPERTY_HINT_RANGE, "1,1000,0.1,or_greater,exp"), "set_strata_hardness", "get_strata_hardness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_cliff_angle", PROPERTY_HINT_RANGE, "10,85,0.1,degrees"), "set_strata_cliff_angle", "get_strata_cliff_angle");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_dip", PROPERTY_HINT_RANGE, "0,80,0.1,degrees"), "set_strata_dip", "get_strata_dip");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_dip_direction", PROPERTY_HINT_RANGE, "0,360,0.1,degrees"), "set_strata_dip_direction", "get_strata_dip_direction");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_channel_area", PROPERTY_HINT_RANGE, "0,1000000,1,or_greater,exp,suffix:m²"), "set_strata_channel_area", "get_strata_channel_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strata_wobble", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater,suffix:m"), "set_strata_wobble", "get_strata_wobble");

	ADD_GROUP("Glacial", "glacial_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "glacial_enabled"), "set_glacial_enabled", "is_glacial_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_ela", PROPERTY_HINT_RANGE, "-1000,5000,1,or_greater,or_less,suffix:m"), "set_glacial_ela", "get_glacial_ela");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_glacial_share", "get_glacial_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_final", PROPERTY_HINT_RANGE, "0,1000000,100,or_greater,suffix:yr"), "set_glacial_final", "get_glacial_final");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_erosion", PROPERTY_HINT_RANGE, "0,100,0.01,or_greater,exp"), "set_glacial_erosion", "get_glacial_erosion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_sliding", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_glacial_sliding", "get_glacial_sliding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_rock_variation", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_glacial_rock_variation", "get_glacial_rock_variation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_rock_scale", PROPERTY_HINT_RANGE, "16,4000,1,suffix:m"), "set_glacial_rock_scale", "get_glacial_rock_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_inflow_balance", PROPERTY_HINT_RANGE, "0,3,0.01,or_greater,suffix:m/yr"), "set_glacial_inflow_balance", "get_glacial_inflow_balance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "glacial_coarse_cell", PROPERTY_HINT_RANGE, "0,512,1,suffix:m"), "set_glacial_coarse_cell", "get_glacial_coarse_cell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ice_worn_detail"), "set_ice_worn_detail", "is_ice_worn_detail");

	ADD_GROUP("Coast", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ocean_edges", PROPERTY_HINT_FLAGS, "-X,+X,-Z,+Z"), "set_ocean_edges", "get_ocean_edges");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_distance", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_coast_distance", "get_coast_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_roughness", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:m"), "set_coast_roughness", "get_coast_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_wavelength", PROPERTY_HINT_RANGE, "10,20000,1,or_greater,suffix:m"), "set_coast_wavelength", "get_coast_wavelength");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sea_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_sea_map", "get_sea_map");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "seabed_profile", PROPERTY_HINT_RANGE, "0,0.5,0.001,or_greater"), "set_seabed_profile", "get_seabed_profile");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shelf_slope", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater"), "set_shelf_slope", "get_shelf_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_sea_depth", PROPERTY_HINT_RANGE, "0.1,1000,0.1,or_greater,suffix:m"), "set_max_sea_depth", "get_max_sea_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "beach_width", PROPERTY_HINT_RANGE, "0,500,0.1,or_greater,suffix:m"), "set_beach_width", "get_beach_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "beach_slope", PROPERTY_HINT_RANGE, "0.001,1,0.001"), "set_beach_slope", "get_beach_slope");
	ADD_GROUP("Lakes", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "lake_count", PROPERTY_HINT_RANGE, "0,256,1"), "set_lake_count", "get_lake_count");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lakes", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_lakes", "get_lakes");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lake_depth", PROPERTY_HINT_RANGE, "1,200,0.1,or_greater,suffix:m"), "set_lake_depth", "get_lake_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lake_catchment", PROPERTY_HINT_RANGE, "0,1e9,1,or_greater,suffix:m²"), "set_lake_catchment", "get_lake_catchment");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lake_min_area", PROPERTY_HINT_RANGE, "0,1e8,1,or_greater,suffix:m²"), "set_lake_min_area", "get_lake_min_area");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lake_spacing", PROPERTY_HINT_RANGE, "0,20000,1,or_greater,suffix:m"), "set_lake_spacing", "get_lake_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lake_evaporation", PROPERTY_HINT_RANGE, "0,5000,1,or_greater,suffix:mm/yr"), "set_lake_evaporation", "get_lake_evaporation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fall_lips"), "set_fall_lips", "get_fall_lips");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "plunge_pools"), "set_plunge_pools", "get_plunge_pools");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fan_backfill"), "set_fan_backfill", "get_fan_backfill");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gorge_age", PROPERTY_HINT_RANGE, "0,100000,100,or_greater,suffix:yr"), "set_gorge_age", "get_gorge_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moraine_age", PROPERTY_HINT_RANGE, "0,20000,100,or_greater,suffix:yr"), "set_moraine_age", "get_moraine_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scree_age", PROPERTY_HINT_RANGE, "0,20000,100,or_greater,suffix:yr"), "set_scree_age", "get_scree_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "meander_age", PROPERTY_HINT_RANGE, "0,20000,100,or_greater,suffix:yr"), "set_meander_age", "get_meander_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outwash_age", PROPERTY_HINT_RANGE, "0,20000,100,or_greater,suffix:yr"), "set_outwash_age", "get_outwash_age");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gorge_rate", PROPERTY_HINT_RANGE, "0,0.1,0.0001,or_greater"), "set_gorge_rate", "get_gorge_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spray_cell", PROPERTY_HINT_RANGE, "0,32,0.5,suffix:m"), "set_spray_cell", "get_spray_cell");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sky_occlusion"), "set_sky_occlusion", "is_sky_occlusion_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_cell", PROPERTY_HINT_RANGE, "1,64,0.5,suffix:m"), "set_sky_cell", "get_sky_cell");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sky_light_source", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WorldEnvironment,Camera3D"), "set_sky_light_source", "get_sky_light_source");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_light_interval", PROPERTY_HINT_RANGE, "0,60,0.1,suffix:s"), "set_sky_light_interval", "get_sky_light_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sky_distance", PROPERTY_HINT_RANGE, "0,20000,10,suffix:m"), "set_sky_distance", "get_sky_distance");
	ADD_GROUP("Inflows", "inflow");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "inflows", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_inflows", "get_inflows");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "inflow_area", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater,suffix:km²"), "set_inflow_area", "get_inflow_area");

	ADD_GROUP("Output", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain"), "set_terrain", "get_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "deformer", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDeformer3D"), "set_deformer", "get_deformer");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_mesh", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D"), "set_ground_mesh", "get_ground_mesh");

	ADD_SIGNAL(MethodInfo("changing"));
	ADD_SIGNAL(MethodInfo("generated"));
	ADD_SIGNAL(MethodInfo("written", PropertyInfo(Variant::INT, "vertices")));

	BIND_ENUM_CONSTANT(OUTLET_NEGATIVE_X);
	BIND_ENUM_CONSTANT(OUTLET_POSITIVE_X);
	BIND_ENUM_CONSTANT(OUTLET_NEGATIVE_Z);
	BIND_ENUM_CONSTANT(OUTLET_POSITIVE_Z);
}
