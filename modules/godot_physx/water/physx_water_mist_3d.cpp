/**************************************************************************/
/*  physx_water_mist_3d.cpp                                               */
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

#include "physx_water_mist_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/fog_volume.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/fog_material.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_raytracing_volumes.h"

#include "modules/modules_enabled.gen.h" // For weather.
#ifdef MODULE_WEATHER_ENABLED
#include "modules/weather/weather_map_3d.h"
#endif

namespace {

constexpr float RHO_WATER = 1000.0f; // kg/m^3
constexpr float AIR_VISCOSITY = 1.81e-5f; // Pa s
constexpr float GROWTH_G = 1.0e-10f; // m^2/s: r dr/dt = G (S - 1) near 10-20 C (Rogers and Yau, ch. 7)

// Every mist in the tree; the path tracer gets them all as one atlas.
LocalVector<PhysXWaterMist3D *> mists;
Ref<ImageTexture3D> atlas;

// Raster mist is a FogVolume, which shows only with the Environment's volumetric fog on. While a mist is
// live and the Environment has it off, it is turned on with no fog of its own (density 0) and lit by the
// sky (ambient inject 1), as WeatherMap3D's haze layers do; put back when the last mist goes, unless
// something else changed those settings meanwhile.
struct FogClaim {
	Ref<Environment> env;
	bool enabled = false;
	float density = 0.0f;
	float ambient_inject = 0.0f;
	float length = 64.0f;
} fog_claim;

Ref<Environment> find_environment(Node *p_node) {
	Viewport *vp = p_node->get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	if (cam && cam->get_environment().is_valid()) {
		return cam->get_environment();
	}
	Ref<World3D> world = vp ? vp->find_world_3d() : Ref<World3D>();
	if (world.is_valid()) {
		return world->get_environment().is_valid() ? world->get_environment() : world->get_fallback_environment();
	}
	return Ref<Environment>();
}

void release_fog() {
	if (fog_claim.env.is_valid()) {
		Ref<Environment> env = fog_claim.env;
		// Only what is still as the mist left it.
		if (env->is_volumetric_fog_enabled() && env->get_volumetric_fog_density() == 0.0f) {
			env->set_volumetric_fog_enabled(fog_claim.enabled);
			env->set_volumetric_fog_density(fog_claim.density);
			env->set_volumetric_fog_ambient_inject(fog_claim.ambient_inject);
			env->set_volumetric_fog_length(fog_claim.length);
		}
	}
	fog_claim = FogClaim();
}

void claim_fog(Node *p_node) {
	Ref<Environment> env = find_environment(p_node);
	if (env != fog_claim.env) {
		release_fog();
	}
	if (env.is_null() || fog_claim.env.is_valid() || env->is_volumetric_fog_enabled()) {
		return; // already on: the project's own volumetric fog shows the mist
	}
	fog_claim.env = env;
	fog_claim.enabled = false;
	fog_claim.density = env->get_volumetric_fog_density();
	fog_claim.ambient_inject = env->get_volumetric_fog_ambient_inject();
	fog_claim.length = env->get_volumetric_fog_length();
	env->set_volumetric_fog_enabled(true);
	env->set_volumetric_fog_density(0.0f);
	if (fog_claim.ambient_inject <= 0.0f) {
		env->set_volumetric_fog_ambient_inject(1.0f);
	}
	// The default 64 m reach would cut a fall's mist off seen from across a valley.
	env->set_volumetric_fog_length(MAX(fog_claim.length, 200.0f));
}

template <typename F>
struct MistJob {
	F *fn = nullptr;
	void run(uint32_t p_index, void *p_unused) { (*fn)(p_index); }
};

template <typename F>
void parallel_for(int p_count, F &p_fn, const char *p_name) {
	if (p_count < 4) {
		for (int i = 0; i < p_count; i++) {
			p_fn(i);
		}
		return;
	}
	MistJob<F> job;
	job.fn = &p_fn;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const WorkerThreadPool::GroupID gid = pool->add_template_group_task(&job, &MistJob<F>::run, (void *)nullptr, p_count, -1, true, StringName(p_name));
	pool->wait_for_group_task_completion(gid);
}

} // namespace

float PhysXWaterMist3D::settling_speed(float p_radius_um) {
	const float r = p_radius_um * 1e-6f;
	return 2.0f / 9.0f * RHO_WATER * 9.81f * r * r / AIR_VISCOSITY; // Stokes: 20 um -> 4.8 cm/s
}

float PhysXWaterMist3D::evaporation_time(float p_radius_um, float p_humidity) {
	const float r = p_radius_um * 1e-6f;
	const float deficit = MAX(1.0f - CLAMP(p_humidity, 0.0f, 1.0f), 0.01f);
	return MIN(r * r / (2.0f * GROWTH_G * deficit), 600.0f); // 20 um at 85%: 13 s
}

float PhysXWaterMist3D::extinction_per_density(float p_radius_um) {
	// sigma = 3 Q_ext LWC / (4 rho_w r), Q_ext ~ 2 for drops much larger than the wavelength.
	const float r = MAX(p_radius_um, 1.0f) * 1e-6f;
	return 3.0f * 2.0f * 1e-3f / (4.0f * RHO_WATER * r); // per g/m^3: 20 um -> 0.075 /m
}

Vector3 PhysXWaterMist3D::_ambient_wind() const {
#ifdef MODULE_WEATHER_ENABLED
	if (follow_weather) {
		if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
			return weather->get_wind_at(get_global_position() + Vector3(0.0f, 2.0f, 0.0f), 2.0f);
		}
	}
#endif
	return wind;
}

float PhysXWaterMist3D::_humidity() const {
#ifdef MODULE_WEATHER_ENABLED
	if (follow_weather) {
		if (const WeatherMap3D *weather = WeatherMap3D::get_active()) {
			return weather->get_air_humidity();
		}
	}
#endif
	return humidity;
}

void PhysXWaterMist3D::_build_grid() {
	grid_dirty = false;
	const float longest = MAX(size.x, MAX(size.y, size.z));
	cell = MAX(cell_size, longest / MAX_CELLS);
	const Vector3i d(CLAMP((int)Math::ceil(size.x / cell), 2, MAX_CELLS), CLAMP((int)Math::ceil(size.y / cell), 2, MAX_CELLS), CLAMP((int)Math::ceil(size.z / cell), 2, MAX_CELLS));
	const int n = d.x * d.y * d.z;
	if (d != dims) {
		dims = d;
		density.resize(n);
		scratch.resize(n);
		for (int i = 0; i < n; i++) {
			density[i] = 0.0f;
		}
		texture.unref();
	}
	jet.resize(n);
	source.resize(n);
	const Transform3D xf = is_inside_tree() ? get_global_transform() : get_transform();
	grid_min = xf.origin - Vector3(0.5f * dims.x * cell, 0.0f, 0.5f * dims.z * cell);
	const Basis basis = xf.basis.orthonormalized();
	const Basis inv = basis.transposed();
	const Vector3 src = xf.xform(source_position);
	Vector3 jd = Vector3(jet_direction.x, 0.0f, jet_direction.z);
	jd = jd.length_squared() > 1e-8f ? jd.normalized() : Vector3(0.0f, 0.0f, -1.0f);
	const Vector3 ext = source_extents.abs().maxf(0.05f);
	const float r0 = MAX(2.0f * ext.z, 1.5f);
	float weight_sum = 0.0f;
	// The falling column over the source (a tall free fall sheds mist all the way down): a second soft box from the
	// source's top up to column_height, fed with column_share of the rate.
	const bool has_column = column_height > ext.y && column_share > 0.0f;
	const float col_mid = 0.5f * (ext.y + column_height);
	const Vector3 col_ext(ext.x, 0.5f * (column_height - ext.y), ext.z);
	LocalVector<float> column_w;
	float column_sum = 0.0f;
	if (has_column) {
		column_w.resize(n);
	}
	for (int z = 0; z < dims.z; z++) {
		for (int y = 0; y < dims.y; y++) {
			for (int x = 0; x < dims.x; x++) {
				const int c = (z * dims.y + y) * dims.x + x;
				const Vector3 p = grid_min + (Vector3(x, y, z) + Vector3(0.5f, 0.5f, 0.5f)) * cell;
				// In the source's own axes (local to the node, about the source's centre).
				const Vector3 q = inv.xform(p - src);
				// The source: a soft box (Gaussian edges a cell wide, so a small source still feeds a cell).
				const Vector3 out_of = (q.abs() - ext).maxf(0.0f) / cell;
				const float w = Math::exp(-out_of.length_squared());
				source[c] = w;
				weight_sum += w;
				if (has_column) {
					const Vector3 out_col = ((q - Vector3(0.0f, col_mid, 0.0f)).abs() - col_ext).maxf(0.0f) / cell;
					const float wc = Math::exp(-out_col.length_squared());
					column_w[c] = wc;
					column_sum += wc;
				}
				// The spray wind: the air the fall drags down leaves the impact as a wall jet, spreading out
				// from the impact line and slowing as it widens (r0 / (r0 + r)), its layer thickening with
				// distance (about 0.1 r, Glauert 1956); none goes back against the jet (the cliff). Near the
				// impact some of it rises: the billow of mist up the fall's face.
				const float a = CLAMP(q.x, -ext.x, ext.x);
				Vector3 qh(q.x - a, 0.0f, q.z);
				const float r = qh.length();
				const Vector3 dir = r > 1e-3f ? qh / r : jd;
				const float behind = CLAMP(1.0f + 1.5f * dir.dot(jd), 0.0f, 1.0f);
				const float above = MAX(q.y, 0.0f);
				const float layer = Math::exp(-above / (1.0f + 0.1f * r));
				const float speed = jet_speed * r0 / (r0 + r) * behind * layer;
				const float rise = 0.35f * jet_speed * Math::exp(-r / r0) * Math::exp(-above / (4.0f * r0));
				jet[c] = basis.xform(dir * speed) + Vector3(0.0f, rise, 0.0f);
			}
		}
	}
	// Normalised so the source adds source_rate grams a second in all.
	const float share = has_column && column_sum > 0.0f ? CLAMP(column_share, 0.0f, 1.0f) : 0.0f;
	const float scale = (emitting && weight_sum > 0.0f) ? (1.0f - share) * source_rate / (weight_sum * cell * cell * cell) : 0.0f;
	const float column_scale = (emitting && share > 0.0f) ? share * source_rate / (column_sum * cell * cell * cell) : 0.0f;
	for (int i = 0; i < n; i++) {
		source[i] *= scale;
		if (share > 0.0f) {
			source[i] += column_w[i] * column_scale;
		}
	}
}

void PhysXWaterMist3D::_step(float p_dt) {
	if (grid_dirty) {
		_build_grid();
	}
	const Vector3i d = dims;
	const Vector3 ambient = _ambient_wind();
	const Vector3 settle(0.0f, -settling_speed(drop_radius), 0.0f);
	const float decay = Math::exp(-p_dt / evaporation_time(drop_radius, _humidity()));
	const float k = MIN(diffusion * p_dt / (cell * cell), 0.16f); // explicit diffusion stays stable below 1/6
	const float inv_cell = 1.0f / cell;
	const int sx = 1;
	const int sy = d.x;
	const int sz = d.x * d.y;
	const float *rho = density.ptr();
	float *tmp = scratch.ptr();
	const Vector3 *u = jet.ptr();
	// Advection: each cell takes the density where its air came from (trilinear, zero outside the box).
	auto advect = [&](uint32_t p_z) {
		const int z = (int)p_z;
		for (int y = 0; y < d.y; y++) {
			for (int x = 0; x < d.x; x++) {
				const int c = z * sz + y * sy + x;
				const Vector3 v = u[c] + ambient + settle;
				const Vector3 g = Vector3(x, y, z) - v * (p_dt * inv_cell);
				const int x0 = (int)Math::floor(g.x);
				const int y0 = (int)Math::floor(g.y);
				const int z0 = (int)Math::floor(g.z);
				const float fx = g.x - x0;
				const float fy = g.y - y0;
				const float fz = g.z - z0;
				float s = 0.0f;
				for (int k3 = 0; k3 < 8; k3++) {
					const int xi = x0 + (k3 & 1);
					const int yi = y0 + ((k3 >> 1) & 1);
					const int zi = z0 + (k3 >> 2);
					if (xi < 0 || yi < 0 || zi < 0 || xi >= d.x || yi >= d.y || zi >= d.z) {
						continue;
					}
					const float w = ((k3 & 1) ? fx : 1.0f - fx) * (((k3 >> 1) & 1) ? fy : 1.0f - fy) * ((k3 >> 2) ? fz : 1.0f - fz);
					s += w * rho[zi * sz + yi * sy + xi];
				}
				tmp[c] = s;
			}
		}
	};
	parallel_for(d.z, advect, "WaterMistAdvect");
	// Turbulent diffusion, evaporation and the source.
	float *out = density.ptr();
	const float *src = source.ptr();
	LocalVector<float> slab_max;
	slab_max.resize(d.z);
	float *smax = slab_max.ptr();
	auto mix = [&](uint32_t p_z) {
		const int z = (int)p_z;
		float m = 0.0f;
		for (int y = 0; y < d.y; y++) {
			for (int x = 0; x < d.x; x++) {
				const int c = z * sz + y * sy + x;
				const float t = tmp[c];
				float lap = -6.0f * t;
				lap += x > 0 ? tmp[c - sx] : 0.0f;
				lap += x < d.x - 1 ? tmp[c + sx] : 0.0f;
				lap += y > 0 ? tmp[c - sy] : t; // the ground below holds its mist in (no flux down by mixing)
				lap += y < d.y - 1 ? tmp[c + sy] : 0.0f;
				lap += z > 0 ? tmp[c - sz] : 0.0f;
				lap += z < d.z - 1 ? tmp[c + sz] : 0.0f;
				const float v = MAX((t + k * lap) * decay + src[c] * p_dt, 0.0f);
				out[c] = v;
				m = MAX(m, v);
			}
		}
		smax[z] = m;
	};
	parallel_for(d.z, mix, "WaterMistMix");
	max_density = 0.0f;
	for (int z = 0; z < d.z; z++) {
		max_density = MAX(max_density, smax[z]);
	}
}

// Path tracing: every mist in one atlas (each box a slab along z, a zero texel between), the first
// RTDenseVolumes::MAX_BOXES of them.
static void _publish_mists() {
	RTDenseVolumes dv;
	LocalVector<PhysXWaterMist3D *> used;
	for (PhysXWaterMist3D *m : mists) {
		if (m->is_visible_in_tree() && m->get_max_density() > 0.0f && (int)used.size() < RTDenseVolumes::MAX_BOXES) {
			used.push_back(m);
		}
	}
	if (used.is_empty()) {
		atlas.unref();
		release_fog();
		RTVolumes::set_dense(dv);
		return;
	}
	claim_fog(used[0]);
	const PhysXWaterMist3D *first = used[0];
	dv.extinction = PhysXWaterMist3D::extinction_per_density(first->get_drop_radius());
	dv.albedo = 0.999f; // water barely absorbs over these paths
	dv.anisotropy = 0.85f; // Henyey-Greenstein fit to Mie drops of 10-50 um
	float top = 0.0f;
	for (PhysXWaterMist3D *m : used) {
		top = MAX(top, m->get_max_density());
	}
	dv.max_density = top;
	if (used.size() == 1) {
		Ref<ImageTexture3D> t = first->get_texture();
		if (t.is_null()) {
			RTVolumes::set_dense(RTDenseVolumes());
			return;
		}
		dv.texture = t->get_rid();
		RTDenseBox b;
		b.origin = first->get_grid_min();
		b.cell = first->get_grid_cell();
		b.size = first->get_grid_dims();
		b.atlas_z = 0;
		dv.boxes.push_back(b);
		atlas.unref();
		RTVolumes::set_dense(dv);
		return;
	}
	Vector3i asize;
	for (PhysXWaterMist3D *m : used) {
		const Vector3i d = m->get_grid_dims();
		asize.x = MAX(asize.x, d.x);
		asize.y = MAX(asize.y, d.y);
		asize.z += d.z + 1;
	}
	Vector<Ref<Image>> slices;
	slices.resize(asize.z);
	int z0 = 0;
	for (PhysXWaterMist3D *m : used) {
		const Vector3i d = m->get_grid_dims();
		RTDenseBox b;
		b.origin = m->get_grid_min();
		b.cell = m->get_grid_cell();
		b.size = d;
		b.atlas_z = z0;
		dv.boxes.push_back(b);
		for (int z = 0; z <= d.z; z++) {
			PackedByteArray bytes;
			bytes.resize(asize.x * asize.y * 2);
			memset(bytes.ptrw(), 0, bytes.size());
			if (z < d.z) {
				uint16_t *w = (uint16_t *)bytes.ptrw();
				for (int y = 0; y < d.y; y++) {
					for (int x = 0; x < d.x; x++) {
						w[y * asize.x + x] = Math::make_half_float(m->get_cell_density(x, y, z));
					}
				}
			}
			slices.write[z0 + z] = Image::create_from_data(asize.x, asize.y, false, Image::FORMAT_RH, bytes);
		}
		z0 += d.z + 1;
	}
	if (atlas.is_null() || atlas->get_width() != asize.x || atlas->get_height() != asize.y || atlas->get_depth() != asize.z) {
		atlas.instantiate();
		atlas->create(Image::FORMAT_RH, asize.x, asize.y, asize.z, false, slices);
	} else {
		atlas->update(slices);
	}
	dv.texture = atlas->get_rid();
	RTVolumes::set_dense(dv);
}

void PhysXWaterMist3D::_upload() {
	const Vector3i d = dims;
	if (d.x < 2) {
		return;
	}
	Vector<Ref<Image>> slices;
	slices.resize(d.z);
	for (int z = 0; z < d.z; z++) {
		PackedByteArray bytes;
		bytes.resize(d.x * d.y * 2);
		uint16_t *w = (uint16_t *)bytes.ptrw();
		const float *rho = density.ptr() + z * d.x * d.y;
		for (int i = 0; i < d.x * d.y; i++) {
			w[i] = Math::make_half_float(rho[i] * _wall_fade(i % d.x, i / d.x, z));
		}
		slices.write[z] = Image::create_from_data(d.x, d.y, false, Image::FORMAT_RH, bytes);
	}
	if (texture.is_null()) {
		texture.instantiate();
		texture->create(Image::FORMAT_RH, d.x, d.y, d.z, false, slices);
	} else {
		texture->update(slices);
	}
	// Raster: a FogVolume over the box with the grid as its density (FogMaterial reads r).
	if (fog == nullptr) {
		fog = memnew(FogVolume);
		fog->set_name("Mist");
		fog->set_as_top_level(true);
		add_child(fog, false, INTERNAL_MODE_BACK);
		fog_material.instantiate();
		fog_material->set_albedo(Color(1, 1, 1));
		fog_material->set_edge_fade(0.02f);
		fog_material->set_height_falloff(0.0f);
		fog->set_material(fog_material);
	}
	const Vector3 extent = Vector3(d) * cell;
	fog->set_global_transform(Transform3D(Basis(), grid_min + 0.5f * extent));
	fog->set_size(extent);
	fog_material->set_density(extinction_per_density(drop_radius));
	fog_material->set_density_texture(texture);
	_publish_mists();
}

void PhysXWaterMist3D::advance(float p_seconds) {
	const float dt = 1.0f / MAX(update_rate, 1.0f);
	for (float t = 0.0f; t < p_seconds; t += dt) {
		_step(dt);
	}
	_upload();
}

// Soft walls: what is drawn fades to nothing over the last cells at the sides and the top of the box, so the box
// never shows as an outline (the mist inside it reaches the walls wherever the air carries it). The floor keeps its
// mist: the box stands on the ground.
float PhysXWaterMist3D::_wall_fade(int p_x, int p_y, int p_z) const {
	auto ramp = [](int p_i, int p_n) {
		const float w = MIN(3.0f, 0.25f * p_n);
		const float t = CLAMP(float(MIN(p_i, p_n - 1 - p_i)) / MAX(w, 1.0f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	};
	const float top = CLAMP(float(dims.y - 1 - p_y) / MAX(MIN(3.0f, 0.25f * dims.y), 1.0f), 0.0f, 1.0f);
	return ramp(p_x, dims.x) * ramp(p_z, dims.z) * top * top * (3.0f - 2.0f * top);
}

float PhysXWaterMist3D::get_cell_density(int p_x, int p_y, int p_z) const {
	if (p_x < 0 || p_y < 0 || p_z < 0 || p_x >= dims.x || p_y >= dims.y || p_z >= dims.z) {
		return 0.0f;
	}
	return density[(p_z * dims.y + p_y) * dims.x + p_x] * _wall_fade(p_x, p_y, p_z);
}

float PhysXWaterMist3D::sample_density(const Vector3 &p_world) const {
	if (dims.x < 2) {
		return 0.0f;
	}
	const Vector3 g = (p_world - grid_min) / cell - Vector3(0.5f, 0.5f, 0.5f);
	const int x0 = (int)Math::floor(g.x);
	const int y0 = (int)Math::floor(g.y);
	const int z0 = (int)Math::floor(g.z);
	const Vector3 f = g - Vector3(x0, y0, z0);
	float s = 0.0f;
	for (int k = 0; k < 8; k++) {
		const float w = ((k & 1) ? f.x : 1.0f - f.x) * (((k >> 1) & 1) ? f.y : 1.0f - f.y) * ((k >> 2) ? f.z : 1.0f - f.z);
		s += w * get_cell_density(x0 + (k & 1), y0 + ((k >> 1) & 1), z0 + (k >> 2));
	}
	return s;
}

void PhysXWaterMist3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			mists.push_back(this);
			grid_dirty = true;
			set_notify_transform(true);
			set_physics_process_internal(!Engine::get_singleton()->is_editor_hint());
		} break;
		case NOTIFICATION_EXIT_TREE: {
			mists.erase(this);
			_publish_mists();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			grid_dirty = true;
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_publish_mists();
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (!emitting && max_density <= 0.0f) {
				return;
			}
			const float dt = 1.0f / MAX(update_rate, 1.0f);
			accum += get_physics_process_delta_time();
			int steps = 0;
			while (accum >= dt && steps < 3) {
				accum -= dt;
				_step(dt);
				steps++;
			}
			accum = MIN(accum, (double)dt);
			if (steps > 0) {
				_upload();
			}
		} break;
	}
}

#define MIST_SETTER(m_name, m_type, m_member, m_expr) \
	void PhysXWaterMist3D::set_##m_name(m_type p_value) { \
		m_member = m_expr; \
		grid_dirty = true; \
	}

MIST_SETTER(size, const Vector3 &, size, p_value.maxf(0.5f))
MIST_SETTER(cell_size, float, cell_size, MAX(p_value, 0.05f))
MIST_SETTER(source_position, const Vector3 &, source_position, p_value)
MIST_SETTER(source_extents, const Vector3 &, source_extents, p_value.abs())
MIST_SETTER(jet_speed, float, jet_speed, MAX(p_value, 0.0f))
MIST_SETTER(jet_direction, const Vector3 &, jet_direction, p_value)

void PhysXWaterMist3D::set_source_rate(float p_rate) {
	source_rate = MAX(p_rate, 0.0f);
	grid_dirty = true;
}

void PhysXWaterMist3D::set_column_height(float p_height) {
	column_height = MAX(p_height, 0.0f);
	grid_dirty = true;
}

void PhysXWaterMist3D::set_column_share(float p_share) {
	column_share = CLAMP(p_share, 0.0f, 1.0f);
	grid_dirty = true;
}

void PhysXWaterMist3D::set_emitting(bool p_emitting) {
	emitting = p_emitting;
	grid_dirty = true;
}

void PhysXWaterMist3D::set_drop_radius(float p_radius) {
	drop_radius = CLAMP(p_radius, 1.0f, 200.0f);
}

void PhysXWaterMist3D::set_humidity(float p_humidity) {
	humidity = CLAMP(p_humidity, 0.0f, 1.0f);
}

void PhysXWaterMist3D::set_wind(const Vector3 &p_wind) {
	wind = p_wind;
}

void PhysXWaterMist3D::set_follow_weather(bool p_enabled) {
	follow_weather = p_enabled;
}

void PhysXWaterMist3D::set_diffusion(float p_diffusion) {
	diffusion = MAX(p_diffusion, 0.0f);
}

void PhysXWaterMist3D::set_update_rate(float p_rate) {
	update_rate = CLAMP(p_rate, 1.0f, 120.0f);
}

void PhysXWaterMist3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PhysXWaterMist3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PhysXWaterMist3D::get_size);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &PhysXWaterMist3D::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &PhysXWaterMist3D::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_source_position", "position"), &PhysXWaterMist3D::set_source_position);
	ClassDB::bind_method(D_METHOD("get_source_position"), &PhysXWaterMist3D::get_source_position);
	ClassDB::bind_method(D_METHOD("set_source_extents", "extents"), &PhysXWaterMist3D::set_source_extents);
	ClassDB::bind_method(D_METHOD("get_source_extents"), &PhysXWaterMist3D::get_source_extents);
	ClassDB::bind_method(D_METHOD("set_column_height", "height"), &PhysXWaterMist3D::set_column_height);
	ClassDB::bind_method(D_METHOD("get_column_height"), &PhysXWaterMist3D::get_column_height);
	ClassDB::bind_method(D_METHOD("set_column_share", "share"), &PhysXWaterMist3D::set_column_share);
	ClassDB::bind_method(D_METHOD("get_column_share"), &PhysXWaterMist3D::get_column_share);
	ClassDB::bind_method(D_METHOD("set_source_rate", "grams_per_second"), &PhysXWaterMist3D::set_source_rate);
	ClassDB::bind_method(D_METHOD("get_source_rate"), &PhysXWaterMist3D::get_source_rate);
	ClassDB::bind_method(D_METHOD("set_jet_speed", "speed"), &PhysXWaterMist3D::set_jet_speed);
	ClassDB::bind_method(D_METHOD("get_jet_speed"), &PhysXWaterMist3D::get_jet_speed);
	ClassDB::bind_method(D_METHOD("set_jet_direction", "direction"), &PhysXWaterMist3D::set_jet_direction);
	ClassDB::bind_method(D_METHOD("get_jet_direction"), &PhysXWaterMist3D::get_jet_direction);
	ClassDB::bind_method(D_METHOD("set_drop_radius", "micrometres"), &PhysXWaterMist3D::set_drop_radius);
	ClassDB::bind_method(D_METHOD("get_drop_radius"), &PhysXWaterMist3D::get_drop_radius);
	ClassDB::bind_method(D_METHOD("set_humidity", "humidity"), &PhysXWaterMist3D::set_humidity);
	ClassDB::bind_method(D_METHOD("get_humidity"), &PhysXWaterMist3D::get_humidity);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &PhysXWaterMist3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &PhysXWaterMist3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_follow_weather", "enabled"), &PhysXWaterMist3D::set_follow_weather);
	ClassDB::bind_method(D_METHOD("get_follow_weather"), &PhysXWaterMist3D::get_follow_weather);
	ClassDB::bind_method(D_METHOD("set_diffusion", "diffusion"), &PhysXWaterMist3D::set_diffusion);
	ClassDB::bind_method(D_METHOD("get_diffusion"), &PhysXWaterMist3D::get_diffusion);
	ClassDB::bind_method(D_METHOD("set_update_rate", "hz"), &PhysXWaterMist3D::set_update_rate);
	ClassDB::bind_method(D_METHOD("get_update_rate"), &PhysXWaterMist3D::get_update_rate);
	ClassDB::bind_method(D_METHOD("set_emitting", "emitting"), &PhysXWaterMist3D::set_emitting);
	ClassDB::bind_method(D_METHOD("is_emitting"), &PhysXWaterMist3D::is_emitting);
	ClassDB::bind_method(D_METHOD("sample_density", "world_position"), &PhysXWaterMist3D::sample_density);
	ClassDB::bind_method(D_METHOD("get_max_density"), &PhysXWaterMist3D::get_max_density);
	ClassDB::bind_method(D_METHOD("advance", "seconds"), &PhysXWaterMist3D::advance);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.1,4,0.05,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_GROUP("Source", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "emitting"), "set_emitting", "is_emitting");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "source_position", PROPERTY_HINT_NONE, "suffix:m"), "set_source_position", "get_source_position");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "source_extents", PROPERTY_HINT_NONE, "suffix:m"), "set_source_extents", "get_source_extents");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "source_rate", PROPERTY_HINT_RANGE, "0,100000,1,or_greater,suffix:g/s"), "set_source_rate", "get_source_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "column_height", PROPERTY_HINT_RANGE, "0,500,0.5,or_greater,suffix:m"), "set_column_height", "get_column_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "column_share", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_column_share", "get_column_share");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "jet_speed", PROPERTY_HINT_RANGE, "0,30,0.1,suffix:m/s"), "set_jet_speed", "get_jet_speed");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "jet_direction"), "set_jet_direction", "get_jet_direction");
	ADD_GROUP("Air", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drop_radius", PROPERTY_HINT_RANGE, "1,200,0.5,suffix:um"), "set_drop_radius", "get_drop_radius");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_weather"), "set_follow_weather", "get_follow_weather");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "humidity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_humidity", "get_humidity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "diffusion", PROPERTY_HINT_RANGE, "0,5,0.05,suffix:m²/s"), "set_diffusion", "get_diffusion");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_rate", PROPERTY_HINT_RANGE, "1,60,1,suffix:Hz"), "set_update_rate", "get_update_rate");
}

PhysXWaterMist3D::PhysXWaterMist3D() {
}

PhysXWaterMist3D::~PhysXWaterMist3D() {
	mists.erase(this);
}
