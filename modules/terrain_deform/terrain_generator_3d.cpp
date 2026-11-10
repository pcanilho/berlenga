/**************************************************************************/
/*  terrain_generator_3d.cpp                                              */
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

#include "terrain_generator_3d.h"

#include "terrain_deformer_3d.h"
#include "terrain_erosion_filter.h"
#include "terrain_stream_power.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/path_3d.h"
#include "scene/resources/curve.h"

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
		if (w <= 0.0) {
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
	sed.resize(n);
	rd.resize(n);
	scratch.resize(n);
	temp.resize(n);
	rec.resize(n);
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

	// With no outlet edge, one corner cell is the outlet so water can still leave.
	LocalVector<uint8_t> outlet_mask;
	if (outlet_edges == 0) {
		outlet_mask.resize(n);
		memset(outlet_mask.ptr(), 0, n);
		outlet_mask[0] = 1;
		sp.b.outlet = outlet_mask.ptr();
	}

	double change = 0.0;
	for (int s = 0; s < steps; s++) {
		change = sp.step();
	}
	sp.route();
	last_steps = steps;
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
	last_generate_msec = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	emit_signal(SNAME("generated"));
	return true;
}

void TerrainGenerator3D::clear_result() {
	nx = 0;
	ny = 0;
	heights.clear();
	drainage.clear();
	sediment.clear();
	uplift_field.clear();
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

float TerrainGenerator3D::_ground(real_t p_x, real_t p_z) const {
	const float base = _sample(heights, p_x, p_z);
	if (!detail_enabled || detail_octaves <= 0 || detail_depth <= 0.0 || Math::is_nan(base)) {
		return base;
	}
	// Gradient of the simulated surface by central differences over a grid cell either side, clamped inside the
	// rectangle so border vertices still get one. The wide stencil makes the gradient fall to zero across sharp
	// ridge crests and valley floors (opposite slopes average out), which is where the filter fades to the fade
	// target; a point gradient flips at a crest and leaves a seam of teeth along it.
	const Rect2 &rect = result_rect;
	const real_t e = result_cell;
	const real_t xa = MAX(p_x - e, rect.position.x);
	const real_t xb = MIN(p_x + e, rect.position.x + rect.size.x);
	const real_t za = MAX(p_z - e, rect.position.y);
	const real_t zb = MIN(p_z + e, rect.position.y + rect.size.y);
	const double gx = (double(_sample(heights, xb, p_z)) - _sample(heights, xa, p_z)) / MAX(double(xb - xa), 1e-6);
	const double gz = (double(_sample(heights, p_x, zb)) - _sample(heights, p_x, za)) / MAX(double(zb - za), 1e-6);
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
	const double off = TerrainErosionFilter::offset(ep, p_x, p_z, base, gx, gz);
	// Never below the outlets: the filter must not cut the coast or the sea floor.
	return float(MAX(double(base) + off, double(MIN(base, result_min))));
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

Ref<Image> TerrainGenerator3D::get_drainage_image() const {
	return _grid_image(drainage);
}

Ref<Image> TerrainGenerator3D::get_sediment_image() const {
	return _grid_image(sediment);
}

Ref<Image> TerrainGenerator3D::get_uplift_image() const {
	return _grid_image(uplift_field);
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
	ClassDB::bind_method(D_METHOD("set_critical_slope", "degrees"), &TerrainGenerator3D::set_critical_slope);
	ClassDB::bind_method(D_METHOD("get_critical_slope"), &TerrainGenerator3D::get_critical_slope);
	ClassDB::bind_method(D_METHOD("set_flow_exponent", "exponent"), &TerrainGenerator3D::set_flow_exponent);
	ClassDB::bind_method(D_METHOD("get_flow_exponent"), &TerrainGenerator3D::get_flow_exponent);
	ClassDB::bind_method(D_METHOD("set_time_step", "years"), &TerrainGenerator3D::set_time_step);
	ClassDB::bind_method(D_METHOD("get_time_step"), &TerrainGenerator3D::get_time_step);
	ClassDB::bind_method(D_METHOD("set_steps", "steps"), &TerrainGenerator3D::set_steps);
	ClassDB::bind_method(D_METHOD("get_steps"), &TerrainGenerator3D::get_steps);
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
	ClassDB::bind_method(D_METHOD("get_drainage_area", "global_position"), &TerrainGenerator3D::get_drainage_area);
	ClassDB::bind_method(D_METHOD("get_sediment_depth", "global_position"), &TerrainGenerator3D::get_sediment_depth);
	ClassDB::bind_method(D_METHOD("get_height_image"), &TerrainGenerator3D::get_height_image);
	ClassDB::bind_method(D_METHOD("get_drainage_image"), &TerrainGenerator3D::get_drainage_image);
	ClassDB::bind_method(D_METHOD("get_sediment_image"), &TerrainGenerator3D::get_sediment_image);
	ClassDB::bind_method(D_METHOD("get_uplift_image"), &TerrainGenerator3D::get_uplift_image);
	ClassDB::bind_method(D_METHOD("get_last_generate_msec"), &TerrainGenerator3D::get_last_generate_msec);
	ClassDB::bind_method(D_METHOD("get_last_write_msec"), &TerrainGenerator3D::get_last_write_msec);
	ClassDB::bind_method(D_METHOD("get_last_steps"), &TerrainGenerator3D::get_last_steps);
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
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "critical_slope", PROPERTY_HINT_RANGE, "0,80,0.1,degrees"), "set_critical_slope", "get_critical_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flow_exponent", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_flow_exponent", "get_flow_exponent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_step", PROPERTY_HINT_RANGE, "1,1000000,1,or_greater,suffix:yr"), "set_time_step", "get_time_step");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "steps", PROPERTY_HINT_RANGE, "0,5000,1,or_greater"), "set_steps", "get_steps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "initial_roughness", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:m"), "set_initial_roughness", "get_initial_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "outlet_edges", PROPERTY_HINT_FLAGS, "-X,+X,-Z,+Z"), "set_outlet_edges", "get_outlet_edges");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sea_level", PROPERTY_HINT_NONE, "suffix:m"), "set_sea_level", "get_sea_level");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_height_offset", "get_height_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_height", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_max_height", "get_max_height");

	ADD_GROUP("Detail", "detail_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "detail_enabled"), "set_detail_enabled", "is_detail_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_wavelength", PROPERTY_HINT_RANGE, "1,2000,1,or_greater,suffix:m"), "set_detail_wavelength", "get_detail_wavelength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_octaves", PROPERTY_HINT_RANGE, "0,12,1"), "set_detail_octaves", "get_detail_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_depth", PROPERTY_HINT_RANGE, "0,0.5,0.001"), "set_detail_depth", "get_detail_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_reach", PROPERTY_HINT_RANGE, "0.1,5,0.01"), "set_detail_reach", "get_detail_reach");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_gully_weight", PROPERTY_HINT_RANGE, "0.05,1,0.01"), "set_detail_gully_weight", "get_detail_gully_weight");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail_assumed_slope", PROPERTY_HINT_RANGE, "0.01,3,0.01"), "set_detail_assumed_slope", "get_detail_assumed_slope");

	ADD_GROUP("Output", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain"), "set_terrain", "get_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "deformer", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "TerrainDeformer3D"), "set_deformer", "get_deformer");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "ground_mesh", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "MeshInstance3D"), "set_ground_mesh", "get_ground_mesh");

	ADD_SIGNAL(MethodInfo("generated"));
	ADD_SIGNAL(MethodInfo("written", PropertyInfo(Variant::INT, "vertices")));

	BIND_ENUM_CONSTANT(OUTLET_NEGATIVE_X);
	BIND_ENUM_CONSTANT(OUTLET_POSITIVE_X);
	BIND_ENUM_CONSTANT(OUTLET_NEGATIVE_Z);
	BIND_ENUM_CONSTANT(OUTLET_POSITIVE_Z);
}
