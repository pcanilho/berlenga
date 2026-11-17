/**************************************************************************/
/*  vegetation_far_match.cpp                                              */
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

// The live-view match of the far plant colour (VegetationField3D.far_match_view). Plants inside
// their range darken the ground they stand on (cast shadows, shaded sides, the gaps between them
// in their shade); the far map that replaces them past the range is their colour from above on a
// flat lit ground, so a band of plants read darker near and lighter far, a ring where the plants
// thin (the "lime ring" of the showcase scene, docs/ideas/vegetation.md). The measured cure, as
// GrassField3D's far match for the sward: read the frame back (async, no stall) and compare, at
// the same distance from the camera so the ground's own trend with distance cancels, ground under
// plants with bare ground near (plants drawn) and far (far colour drawn); then scale the far
// colour's share (vegetation_far_shade, vegetation_far()) until the far contrast is the near one.

#include "vegetation_field_3d.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#include "modules/terrain_deform/terrain_data_map_3d.h"

float VegetationField3D::far_shade_published = 1.0f;

void VegetationField3D::_publish_far_shade() {
	far_shade_published = far_shade;
	RenderingServer::get_singleton()->global_shader_parameter_set("vegetation_far_shade", far_shade);
	if (far_volume_material.is_valid()) {
		// The far volume stands for the same plants: the same darkening (linear factor).
		far_volume_material->set_albedo(Color(far_shade, far_shade, far_shade).linear_to_srgb());
	}
}

void VegetationField3D::set_far_match_view(bool p_enabled) {
	if (far_match_view == p_enabled) {
		return;
	}
	far_match_view = p_enabled;
	far_view_round = 0;
	if (!p_enabled) {
		far_shade = 1.0f;
		if (far_view_state == 1) {
			far_view_state = 0;
		}
	} else {
		_far_view_start(20);
		if (is_inside_tree()) {
			set_process_internal(true);
		}
	}
	if (far_image.is_valid()) {
		_publish_far_shade();
	}
}

Dictionary VegetationField3D::get_far_match_stats() const {
	Dictionary d;
	d["shade"] = far_shade;
	d["ratio"] = far_view_ratio;
	d["groups"] = far_view_groups;
	d["round"] = far_view_round;
	d["state"] = far_view_state;
	d["misses"] = far_view_misses;
	d["gain"] = far_view_gain;
	return d;
}

void VegetationField3D::_far_view_start(int p_wait) {
	if (!far_match_view || far_view_state == 2) {
		return;
	}
	far_view_state = 1;
	far_view_wait = p_wait;
}

void VegetationField3D::_far_view_step() {
	if (far_view_state == 2) {
		return;
	}
	if (_path_traced()) {
		// A path traced frame is still converging (and noisy without a denoiser): keep the shade
		// the raster frames measured, and look again once raster is back.
		return;
	}
	Viewport *vp = get_viewport();
	Camera3D *cam = vp ? vp->get_camera_3d() : nullptr;
	if (!cam) {
		return;
	}
	if (far_view_state == 0) {
		// Again once the camera has gone a good way (a fifth of the range).
		if (cam->get_global_position().distance_to(far_view_camera) > 0.2f * far_image_range) {
			far_view_round = 0;
			far_view_misses = 0;
			_far_view_start(20);
		}
		return;
	}
	if (--far_view_wait > 0) {
		return;
	}
	if (vp->get_texture().is_null() || far_image.is_null() || far_image->is_empty()) {
		return;
	}
	const Vector2 size = vp->get_visible_rect().size;
	TerrainDataMap3D *map = Object::cast_to<TerrainDataMap3D>(get_node_or_null(data_map));
	const Vector3 cp = cam->get_global_position();
	const float range = far_image_range;
	// Plants all drawn up to 0.35 of the range (vegetation_keep()); the far colour alone past it.
	// Six distances a band; only spots on screen count, so a camera high up (the near ground under
	// it out of view) still finds some.
	constexpr int dists = 6;
	const float d_near[dists] = { 0.1f * range, 0.15f * range, 0.2f * range, 0.25f * range, 0.3f * range, 0.34f * range };
	const float d_far[dists] = { 1.03f * range, 1.08f * range, 1.13f * range, 1.18f * range, 1.24f * range, 1.3f * range };
	const Vector3 fwd = -cam->get_global_transform().basis.get_column(Vector3::AXIS_Z);
	Vector2 f2(fwd.x, fwd.z);
	if (f2.length() < 0.05f) {
		const Vector3 up = cam->get_global_transform().basis.get_column(Vector3::AXIS_Y);
		f2 = Vector2(up.x, up.z);
	}
	const float a0 = Math::atan2(f2.y, f2.x);
	const int fw = far_image->get_width();
	const int fh = far_image->get_height();
	// The far map's cover at a point (alpha: the share of ground the plants hide).
	auto cover_at = [&](const Vector2 &p_xz) -> float {
		const Vector2 uv = (p_xz - far_image_rect.position) / far_image_rect.size;
		if (uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f) {
			return -1.0f;
		}
		return far_image->get_pixel(CLAMP(int(uv.x * fw), 0, fw - 1), CLAMP(int(uv.y * fh), 0, fh - 1)).a;
	};
	auto to_px = [&](const Vector2 &p_xz, Vector2 &r_px) -> bool {
		Vector3 p(p_xz.x, cp.y, p_xz.y);
		if (map) {
			p.y = map->get_height_at(p);
		}
		if (cam->is_position_behind(p)) {
			return false;
		}
		r_px = cam->unproject_position(p);
		return r_px.x >= 4.0f && r_px.y >= 4.0f && r_px.x < size.x - 4.0f && r_px.y < size.y - 4.0f;
	};
	// Along arcs at four distances per band: the most covered and the barest spots within a few
	// metres of each other, so both see the same ground, light and haze.
	far_view_px.clear();
	const int arcs = 16;
	const int steps = 12;
	for (int i = 0; i < arcs; i++) {
		const float base = a0 + (float(i) / float(arcs - 1) - 0.5f) * 1.4f;
		Vector2 got[4];
		bool ok = true;
		for (int band = 0; band < 2 && ok; band++) {
			const float *dist = band == 0 ? d_near : d_far;
			float best_hi = -1.0f, best_lo = 2.0f;
			Vector2 px_hi, px_lo;
			for (int k = 0; k < dists; k++) {
				// About 8 m of ground across the arc: patches are metres wide, and within that the
				// light and haze barely change.
				const float span = MIN(8.0f / dist[k], 0.6f);
				for (int j = 0; j < steps; j++) {
					const float a = base + (float(j) / float(steps - 1) - 0.5f) * span;
					const Vector2 xz = Vector2(cp.x, cp.z) + Vector2(Math::cos(a), Math::sin(a)) * dist[k];
					const float c = cover_at(xz);
					Vector2 px;
					if (c < 0.0f || (c <= best_hi && c >= best_lo) || !to_px(xz, px)) {
						continue;
					}
					if (c > best_hi) {
						best_hi = c;
						px_hi = px;
					}
					if (c < best_lo) {
						best_lo = c;
						px_lo = px;
					}
				}
			}
			ok = best_hi >= 0.55f && best_lo <= 0.1f;
			got[band * 2] = px_hi;
			got[band * 2 + 1] = px_lo;
		}
		if (ok) {
			for (int k = 0; k < 4; k++) {
				far_view_px.push_back(got[k]);
			}
		}
	}
	far_view_camera = cp;
	far_view_size = size;
	if (far_view_px.size() < 4 * 6) {
		_far_view_apply(1.0f, far_view_px.size() / 4); // Too few bare and covered spots in view.
		return;
	}
	far_view_state = 2;
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &VegetationField3D::_far_view_issue).bind(vp->get_texture()->get_rid()));
}

void VegetationField3D::_far_view_issue(RID p_viewport_texture) {
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs->get_rendering_device();
	const RID tex = rd ? rs->texture_get_rd_texture(p_viewport_texture) : RID();
	if (!tex.is_valid() || !rd->texture_is_valid(tex)) {
		callable_mp(this, &VegetationField3D::_far_view_apply).call_deferred(1.0f, 0);
		return;
	}
	const RD::TextureFormat tf = rd->texture_get_format(tex);
	const Error err = rd->texture_get_data_async(tex, 0, callable_mp(this, &VegetationField3D::_far_view_on_data).bind(int(tf.width), int(tf.height), int(tf.format)));
	if (err != OK) {
		callable_mp(this, &VegetationField3D::_far_view_apply).call_deferred(1.0f, 0);
	}
}

// When the copy lands: the contrast of covered against bare ground near, over the same far.
void VegetationField3D::_far_view_on_data(const PackedByteArray &p_data, int p_width, int p_height, int p_format) {
	int bpp = 0;
	bool half = false, bgr = false, srgb = true;
	switch (p_format) {
		case RD::DATA_FORMAT_R8G8B8A8_UNORM:
		case RD::DATA_FORMAT_R8G8B8A8_SRGB:
			bpp = 4;
			break;
		case RD::DATA_FORMAT_B8G8R8A8_UNORM:
		case RD::DATA_FORMAT_B8G8R8A8_SRGB:
			bpp = 4;
			bgr = true;
			break;
		case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
			bpp = 8;
			half = true;
			srgb = false;
			break;
		default:
			break;
	}
	if (bpp == 0 || p_width <= 0 || p_height <= 0 || p_data.size() < int64_t(p_width) * p_height * bpp || far_view_size.x <= 0.0f) {
		callable_mp(this, &VegetationField3D::_far_view_apply).call_deferred(1.0f, 0);
		return;
	}
	const uint8_t *bytes = p_data.ptr();
	const Vector2 scale(float(p_width) / far_view_size.x, float(p_height) / far_view_size.y);
	const int half_box = MAX(1, p_height / 180);
	auto lum = [&](const Vector2 &p_at) -> float {
		const int cx = int(p_at.x * scale.x), cy = int(p_at.y * scale.y);
		float sum = 0.0f;
		int n = 0;
		for (int y = MAX(cy - half_box, 0); y <= MIN(cy + half_box, p_height - 1); y++) {
			for (int x = MAX(cx - 2 * half_box, 0); x <= MIN(cx + 2 * half_box, p_width - 1); x++) {
				const uint8_t *px = bytes + (int64_t(y) * p_width + x) * bpp;
				Color col;
				if (half) {
					const uint16_t *h = reinterpret_cast<const uint16_t *>(px);
					col = Color(Math::half_to_float(h[0]), Math::half_to_float(h[1]), Math::half_to_float(h[2]), 1.0f);
				} else {
					col = Color(px[bgr ? 2 : 0] / 255.0f, px[1] / 255.0f, px[bgr ? 0 : 2] / 255.0f, 1.0f);
				}
				if (srgb) {
					col = col.srgb_to_linear();
				}
				sum += col.r * 0.2126f + col.g * 0.7152f + col.b * 0.0722f;
				n++;
			}
		}
		return n ? sum / float(n) : 0.0f;
	};
	float nh = 0.0f, nb = 0.0f, fh = 0.0f, fb = 0.0f;
	int groups = 0;
	for (int i = 0; i + 3 < far_view_px.size(); i += 4) {
		const float a = lum(far_view_px[i]), b = lum(far_view_px[i + 1]), c = lum(far_view_px[i + 2]), d = lum(far_view_px[i + 3]);
		if (a < 1e-4f || b < 1e-4f || c < 1e-4f || d < 1e-4f) {
			continue;
		}
		nh += a;
		nb += b;
		fh += c;
		fb += d;
		groups++;
	}
	float ratio = 1.0f;
	if (groups > 0) {
		// Near contrast over far contrast: below 1, the far plants read lighter than the near ones.
		ratio = (nh / nb) / MAX(fh / fb, 1e-4f);
	}
	callable_mp(this, &VegetationField3D::_far_view_apply).call_deferred(ratio, groups);
}

void VegetationField3D::_far_view_apply(float p_ratio, int p_groups) {
	far_view_state = 0;
	far_view_groups = p_groups;
	if (p_groups < 6) {
		if (++far_view_misses < 10) {
			_far_view_start(60);
		}
		return;
	}
	far_view_misses = 0;
	far_view_ratio = p_ratio;
	far_view_round++;
	if (far_view_round > 1 && Math::abs(far_view_step_shade - far_shade) > 0.02f) {
		// How far the last step moved the contrast, as a power of the shade's change: the far
		// colour is only the diffuse part of the ground's light (sky reflection and haze do not
		// follow it), so on dark ground a step moves the contrast much less than itself.
		const float gain = -Math::log(p_ratio / far_view_step_ratio) / Math::log(far_shade / far_view_step_shade);
		if (gain < 0.05f) {
			// It barely moved: the ground there does not draw the far colour (no vegetation_far(),
			// or something covers it). Undo the steps and stop.
			far_shade = 1.0f;
			_publish_far_shade();
			print_verbose(vformat("VegetationField3D \"%s\": far match: the ground past the range does not follow the far colour (gain %.3f), shade left at 1.", get_name(), gain));
			far_view_round = 0;
			return;
		}
		far_view_gain = CLAMP(gain, 0.05f, 1.0f);
	} else if (far_view_round == 1) {
		far_view_gain = 1.0f;
	}
	const bool close = Math::abs(p_ratio - 1.0f) < 0.03f;
	far_view_step_shade = far_shade;
	far_view_step_ratio = p_ratio;
	if (!close) {
		// Contrast goes as shade^-gain: step by the ratio's root, a bounded step a round.
		const float step = Math::pow(p_ratio, 1.0f / far_view_gain);
		far_shade = CLAMP(far_shade * CLAMP(step, 0.6f, 1.5f), 0.3f, 1.2f);
		_publish_far_shade();
	}
	print_verbose(vformat("VegetationField3D \"%s\": far match round %d: near/far contrast %.3f over %d groups, shade now %.3f.", get_name(), far_view_round, p_ratio, p_groups, far_shade));
	if (!close && far_view_round < 8) {
		_far_view_start(10);
	} else {
		far_view_round = 0;
	}
}
