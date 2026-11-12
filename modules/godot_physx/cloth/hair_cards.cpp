/**************************************************************************/
/*  hair_cards.cpp                                                        */
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


#include "hair_cards.h"

#include "core/math/random_pcg.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

namespace {

// A strand resampled to p_m points evenly by length.
void resample(const Vector3 *p_p, int p_n, int p_m, Vector3 *r_out) {
	if (p_n <= 1) {
		for (int i = 0; i < p_m; i++) {
			r_out[i] = p_n == 1 ? p_p[0] : Vector3();
		}
		return;
	}
	float total = 0.0f;
	for (int i = 1; i < p_n; i++) {
		total += p_p[i].distance_to(p_p[i - 1]);
	}
	int seg = 1;
	float done = 0.0f; // length up to point seg - 1
	float seg_len = p_p[1].distance_to(p_p[0]);
	for (int k = 0; k < p_m; k++) {
		const float target = total * float(k) / float(MAX(p_m - 1, 1));
		while (seg < p_n - 1 && done + seg_len < target) {
			done += seg_len;
			seg++;
			seg_len = p_p[seg].distance_to(p_p[seg - 1]);
		}
		const float f = seg_len > 1e-9f ? CLAMP((target - done) / seg_len, 0.0f, 1.0f) : 0.0f;
		r_out[k] = p_p[seg - 1].lerp(p_p[seg], f);
	}
}

// The rotation taking unit a to unit b, applied to v.
Vector3 rotate_between(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_v) {
	const Vector3 axis = p_a.cross(p_b);
	const float s = axis.length();
	const float c = p_a.dot(p_b);
	if (s < 1e-7f) {
		return p_v;
	}
	const Vector3 k = axis / s;
	return p_v * c + k.cross(p_v) * s + k * k.dot(p_v) * (1.0f - c);
}

Vector3 any_perpendicular(const Vector3 &p_t) {
	const Vector3 a = Math::abs(p_t.y) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	return p_t.cross(a).normalized();
}

constexpr int FEATURE_POINTS = 8; // points per strand compared in the clustering

Mutex cache_mutex;
HashMap<uint64_t, Ref<HairCardBake>> cache;

} // namespace

Ref<HairCardBake> HairCardBake::bake(const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_cards, int p_samples, float p_root_width, float p_tip_width) {
	const int sc = p_counts.size();
	const int samples = MAX(p_samples, 2);
	if (sc == 0 || p_cards <= 0) {
		return Ref<HairCardBake>();
	}
	const int k_count = MIN(p_cards, sc);

	// Every strand resampled: FEATURE_POINTS for the clustering, samples for the cards.
	LocalVector<Vector3> feat;
	feat.resize(sc * FEATURE_POINTS);
	LocalVector<Vector3> fine;
	fine.resize(sc * samples);
	{
		int start = 0;
		for (int s = 0; s < sc; s++) {
			const int n = MAX(p_counts[s], 0);
			resample(p_points.ptr() + start, n, FEATURE_POINTS, feat.ptr() + s * FEATURE_POINTS);
			resample(p_points.ptr() + start, n, samples, fine.ptr() + s * samples);
			start += n;
		}
	}
	// Wang et al. 2009's distance between strands: the mean squared distance of matching points.
	auto distance = [&](const Vector3 *p_a, const Vector3 *p_b) {
		float d = 0.0f;
		for (int i = 0; i < FEATURE_POINTS; i++) {
			d += p_a[i].distance_squared_to(p_b[i]);
		}
		return d * (1.0f / FEATURE_POINTS);
	};

	// k-means++ seeds (Arthur and Vassilvitskii 2007), then Lloyd's iterations.
	RandomPCG rng{ 0x5eedca7du };
	LocalVector<Vector3> centres;
	centres.resize(k_count * FEATURE_POINTS);
	LocalVector<float> nearest;
	nearest.resize(sc);
	{
		const int first = rng.rand() % (uint32_t)sc;
		for (int i = 0; i < FEATURE_POINTS; i++) {
			centres[i] = feat[first * FEATURE_POINTS + i];
		}
		double total = 0.0;
		for (int s = 0; s < sc; s++) {
			nearest[s] = distance(&feat[s * FEATURE_POINTS], &centres[0]);
			total += nearest[s];
		}
		for (int c = 1; c < k_count; c++) {
			double pick = rng.randf() * total;
			int chosen = sc - 1;
			for (int s = 0; s < sc; s++) {
				pick -= nearest[s];
				if (pick <= 0.0) {
					chosen = s;
					break;
				}
			}
			for (int i = 0; i < FEATURE_POINTS; i++) {
				centres[c * FEATURE_POINTS + i] = feat[chosen * FEATURE_POINTS + i];
			}
			total = 0.0;
			for (int s = 0; s < sc; s++) {
				nearest[s] = MIN(nearest[s], distance(&feat[s * FEATURE_POINTS], &centres[c * FEATURE_POINTS]));
				total += nearest[s];
			}
		}
	}
	LocalVector<int> label;
	label.resize(sc);
	LocalVector<int> next_label;
	next_label.resize(sc);
	// The assignment (strands x clumps distances) is most of the bake: spread over the worker threads.
	struct Assign {
		const Vector3 *feat;
		const Vector3 *centres;
		int *out;
		int k_count;
		void run(uint32_t p_strand, void *) {
			const Vector3 *a = feat + p_strand * FEATURE_POINTS;
			float best = 1e30f;
			int best_c = 0;
			for (int c = 0; c < k_count; c++) {
				const Vector3 *b = centres + c * FEATURE_POINTS;
				float d = 0.0f;
				for (int i = 0; i < FEATURE_POINTS; i++) {
					d += a[i].distance_squared_to(b[i]);
				}
				if (d < best) {
					best = d;
					best_c = c;
				}
			}
			out[p_strand] = best_c;
		}
	};
	for (int iter = 0; iter < 8; iter++) {
		Assign assign{ feat.ptr(), centres.ptr(), next_label.ptr(), k_count };
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(&assign, &Assign::run, (void *)nullptr, sc, -1, true, String("HairCardBake"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		bool changed = false;
		for (int s = 0; s < sc; s++) {
			if (iter == 0 || label[s] != next_label[s]) {
				changed = true;
			}
			label[s] = next_label[s];
		}
		if (!changed) {
			break;
		}
		LocalVector<int> members;
		members.resize(k_count);
		for (int c = 0; c < k_count; c++) {
			members[c] = 0;
			for (int i = 0; i < FEATURE_POINTS; i++) {
				centres[c * FEATURE_POINTS + i] = Vector3();
			}
		}
		for (int s = 0; s < sc; s++) {
			members[label[s]]++;
			for (int i = 0; i < FEATURE_POINTS; i++) {
				centres[label[s] * FEATURE_POINTS + i] += feat[s * FEATURE_POINTS + i];
			}
		}
		for (int c = 0; c < k_count; c++) {
			if (members[c] == 0) {
				// An empty cluster takes a random strand.
				const int s = rng.rand() % (uint32_t)sc;
				for (int i = 0; i < FEATURE_POINTS; i++) {
					centres[c * FEATURE_POINTS + i] = feat[s * FEATURE_POINTS + i];
				}
				continue;
			}
			for (int i = 0; i < FEATURE_POINTS; i++) {
				centres[c * FEATURE_POINTS + i] /= float(members[c]);
			}
		}
	}

	LocalVector<LocalVector<int>> clusters;
	clusters.resize(k_count);
	for (int s = 0; s < sc; s++) {
		clusters[label[s]].push_back(s);
	}
	int cards = 0;
	for (int c = 0; c < k_count; c++) {
		cards += clusters[c].is_empty() ? 0 : 1;
	}

	Ref<HairCardBake> r;
	r.instantiate();
	r->card_count = cards;
	r->samples = samples;
	r->points.resize(cards * samples);
	for (int a = 0; a < 2; a++) {
		r->axis[a].resize(cards * samples);
		r->half_width[a].resize(cards * samples);
	}
	r->tiles.resize(cards * 2 * 4);

	// The atlas: two tiles a card (one per strip), 32 texels across and up to 256 along, in rows of
	// 2048 texels.
	const int tile_w = 32;
	const int atlas_w = 2048;
	const int per_row = atlas_w / tile_w;
	const int rows = (cards * 2 + per_row - 1) / per_row;
	int tile_h = 256;
	while (tile_h > 32 && rows * tile_h > 4096) {
		tile_h /= 2;
	}
	const int atlas_h = MAX(rows * tile_h, 1);
	LocalVector<float> cover, tilt, seed, weight;
	cover.resize(atlas_w * atlas_h);
	tilt.resize(atlas_w * atlas_h);
	seed.resize(atlas_w * atlas_h);
	weight.resize(atlas_w * atlas_h);
	for (int i = 0; i < atlas_w * atlas_h; i++) {
		cover[i] = 0.0f;
		tilt[i] = 0.0f;
		seed[i] = 0.0f;
		weight[i] = 0.0f;
	}

	LocalVector<Vector3> mean, tangent, frame_u, frame_v;
	mean.resize(samples);
	tangent.resize(samples);
	frame_u.resize(samples);
	frame_v.resize(samples);
	LocalVector<float> spread;
	int card = 0;
	for (int c = 0; c < k_count; c++) {
		const LocalVector<int> &list = clusters[c];
		if (list.is_empty()) {
			continue;
		}
		// The card's centre line: the clump's mean strand.
		for (int k = 0; k < samples; k++) {
			Vector3 m;
			for (const int s : list) {
				m += fine[s * samples + k];
			}
			mean[k] = m / float(list.size());
		}
		for (int k = 0; k < samples; k++) {
			Vector3 t = mean[MIN(k + 1, samples - 1)] - mean[MAX(k - 1, 0)];
			if (t.length_squared() < 1e-14f) {
				t = k > 0 ? tangent[k - 1] : Vector3(0, -1, 0);
			}
			tangent[k] = t.normalized();
		}
		// Rotation-minimising (Bishop) frames, carried along by parallel transport.
		frame_u[0] = any_perpendicular(tangent[0]);
		for (int k = 1; k < samples; k++) {
			Vector3 u = rotate_between(tangent[k - 1], tangent[k], frame_u[k - 1]);
			u = (u - tangent[k] * u.dot(tangent[k]));
			frame_u[k] = u.length_squared() > 1e-12f ? u.normalized() : any_perpendicular(tangent[k]);
		}
		for (int k = 0; k < samples; k++) {
			frame_v[k] = tangent[k].cross(frame_u[k]);
		}
		// The card's angle about its centre line: the one with the least spread of the strands out
		// of its plane (DiffHairCard's root orientation search), here in closed form: the principal
		// axis of the strands' offsets in the frames, summed along the card.
		double cuu = 0.0, cuv = 0.0, cvv = 0.0;
		for (const int s : list) {
			for (int k = 1; k < samples; k++) {
				const Vector3 o = fine[s * samples + k] - mean[k];
				const double ou = o.dot(frame_u[k]);
				const double ov = o.dot(frame_v[k]);
				cuu += ou * ou;
				cuv += ou * ov;
				cvv += ov * ov;
			}
		}
		const float angle = 0.5f * (float)Math::atan2(2.0 * cuv, cuu - cvv);
		const float ca = Math::cos(angle);
		const float sa = Math::sin(angle);
		for (int k = 0; k < samples; k++) {
			const int i = card * samples + k;
			r->points.write[i] = mean[k];
			const Vector3 b = frame_u[k] * ca + frame_v[k] * sa;
			r->axis[0].write[i] = b;
			r->axis[1].write[i] = tangent[k].cross(b);
		}
		// Half widths: the strands' offsets along each axis, 95th percentile (a flyaway does not
		// widen the card), at least the strand's own half width.
		for (int a = 0; a < 2; a++) {
			for (int k = 0; k < samples; k++) {
				const int i = card * samples + k;
				spread.clear();
				for (const int s : list) {
					spread.push_back(Math::abs((fine[s * samples + k] - mean[k]).dot(r->axis[a][i])));
				}
				spread.sort();
				const float w95 = spread[MIN((int)(0.95f * spread.size()), (int)spread.size() - 1)];
				const float own = 0.5f * Math::lerp(p_root_width, p_tip_width, float(k) / float(samples - 1));
				r->half_width[a].write[i] = MAX(w95 + own, 4.0f * own);
			}
		}
		// The strands projected into each strip's tile (DiffHairCard projects them as curves rather
		// than rendering them): per row along the card, each strand's place across it, its width
		// over the texel's as coverage, summed as optical depth (-ln(1 - c) each); its tilt across
		// the strip and a per-strand seed averaged by coverage.
		for (int a = 0; a < 2; a++) {
			const int tile = card * 2 + a;
			const int tx = (tile % per_row) * tile_w;
			const int ty = (tile / per_row) * tile_h;
			r->tiles.write[tile * 4 + 0] = tx;
			r->tiles.write[tile * 4 + 1] = ty;
			r->tiles.write[tile * 4 + 2] = tile_w;
			r->tiles.write[tile * 4 + 3] = tile_h;
			for (int y = 0; y < tile_h; y++) {
				const float t = (float(y) + 0.5f) / float(tile_h) * float(samples - 1);
				const int k0 = MIN((int)t, samples - 2);
				const float f = t - float(k0);
				const int i0 = card * samples + k0;
				const Vector3 m = mean[k0].lerp(mean[k0 + 1], f);
				const Vector3 axis = r->axis[a][i0].lerp(r->axis[a][i0 + 1], f).normalized();
				const float hw = Math::lerp(r->half_width[a][i0], r->half_width[a][i0 + 1], f);
				const float texel = 2.0f * hw / float(tile_w);
				const float width = Math::lerp(p_root_width, p_tip_width, t / float(samples - 1));
				const float alpha = MIN(width / MAX(texel, 1e-7f), 1.0f);
				for (const int s : list) {
					const Vector3 p = fine[s * samples + k0].lerp(fine[s * samples + k0 + 1], f);
					const float u = (p - m).dot(axis) / MAX(hw, 1e-7f);
					if (Math::abs(u) > 1.0f) {
						continue;
					}
					const float x = (u * 0.5f + 0.5f) * float(tile_w) - 0.5f;
					const int x0 = (int)Math::floor(x);
					const float fx = x - float(x0);
					Vector3 d = fine[s * samples + k0 + 1] - fine[s * samples + k0];
					const float dl = d.length();
					const float tl = dl > 1e-9f ? d.dot(axis) / dl : 0.0f;
					const float sd = Math::fract(Math::sin(float(s) * 12.9898f) * 43758.5453f);
					for (int o = 0; o < 2; o++) {
						const int xx = x0 + o;
						if (xx < 0 || xx >= tile_w) {
							continue;
						}
						const float cov = alpha * (o == 0 ? 1.0f - fx : fx);
						const int idx = (ty + y) * atlas_w + tx + xx;
						cover[idx] -= Math::log(MAX(1.0f - cov, 1e-4f));
						tilt[idx] += tl * cov;
						seed[idx] += Math::abs(sd) * cov;
						weight[idx] += cov;
					}
				}
			}
		}
		card++;
	}

	// The atlas holds the optical depth, not the coverage, and makes its own mipmaps: a pixel
	// that spans several texels sees the strands of all of them, each by its share of the pixel,
	// so it lets through exp(-mean depth), the transmittance of the mean depth. Mipmaps of the
	// coverage gave the mean transmittance instead, which (exp being convex) is more, and the
	// cards grew see-through with distance where the strands do not. The depth takes 16 bits, R
	// high and B low; filtering is linear in each, so the sampled depth stays exact.
	for (int i = 0; i < atlas_w * atlas_h; i++) {
		const float wt = weight[i];
		tilt[i] = wt > 0.0f ? 0.5f + 0.5f * CLAMP(tilt[i] / wt, -1.0f, 1.0f) : 0.5f;
		seed[i] = wt > 0.0f ? seed[i] / wt : 0.5f;
		cover[i] = MIN(cover[i] / CARD_DEPTH_MAX, 1.0f);
	}
	r->atlas = Image::create_empty(atlas_w, atlas_h, true, Image::FORMAT_RGBA8);
	PackedByteArray data = r->atlas->get_data();
	uint8_t *w = data.ptrw();
	int lw = atlas_w;
	int lh = atlas_h;
	for (int level = 0; level <= r->atlas->get_mipmap_count(); level++) {
		int64_t ofs, size;
		int mw, mh;
		r->atlas->get_mipmap_offset_size_and_dimensions(level, ofs, size, mw, mh);
		if (level > 0) {
			// Box filter from the level above (lw x lh) to this one (mw x mh), in place.
			for (int y = 0; y < mh; y++) {
				for (int x = 0; x < mw; x++) {
					float d = 0.0f, g = 0.0f, a = 0.0f;
					int n = 0;
					for (int oy = 0; oy < 2; oy++) {
						for (int ox = 0; ox < 2; ox++) {
							const int sx = MIN(x * 2 + ox, lw - 1);
							const int sy = MIN(y * 2 + oy, lh - 1);
							const int si = sy * lw + sx;
							d += cover[si];
							g += tilt[si];
							a += seed[si];
							n++;
						}
					}
					const int di = y * mw + x;
					cover[di] = d / n;
					tilt[di] = g / n;
					seed[di] = a / n;
				}
			}
			lw = mw;
			lh = mh;
		}
		uint8_t *o = w + ofs;
		for (int i = 0; i < mw * mh; i++) {
			const int q = (int)Math::round(cover[i] * 65535.0f);
			o[i * 4 + 0] = (uint8_t)(q >> 8);
			o[i * 4 + 1] = (uint8_t)CLAMP(Math::round(tilt[i] * 255.0f), 0.0f, 255.0f);
			o[i * 4 + 2] = (uint8_t)(q & 255);
			o[i * 4 + 3] = (uint8_t)CLAMP(Math::round(seed[i] * 255.0f), 0.0f, 255.0f);
		}
	}
	r->atlas->set_data(atlas_w, atlas_h, true, Image::FORMAT_RGBA8, data);
	return r;
}

Ref<HairCardBake> HairCardBake::get_cached(uint64_t p_key, const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_cards, int p_samples, float p_root_width, float p_tip_width) {
	MutexLock lock(cache_mutex);
	// Bakes no node holds any more are dropped.
	LocalVector<uint64_t> unused;
	for (const KeyValue<uint64_t, Ref<HairCardBake>> &E : cache) {
		if (E.value->get_reference_count() <= 1) {
			unused.push_back(E.key);
		}
	}
	for (const uint64_t k : unused) {
		cache.erase(k);
	}
	if (const Ref<HairCardBake> *found = cache.getptr(p_key)) {
		return *found;
	}
	Ref<HairCardBake> r = bake(p_points, p_counts, p_cards, p_samples, p_root_width, p_tip_width);
	if (r.is_valid()) {
		cache[p_key] = r;
	}
	return r;
}

void HairCardBake::clear_cache() {
	MutexLock lock(cache_mutex);
	cache.clear();
}
