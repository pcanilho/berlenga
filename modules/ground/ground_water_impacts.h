/**************************************************************************/
/*  ground_water_impacts.h                                                */
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

#pragma once

#include "core/io/image.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/image_texture.h"

// Footsteps, wheels and splashes in the ground's standing water (docs/ideas/rain.md, step 7). Puddles have no ripple
// solver of their own (they are millimetres to centimetres deep over the whole ground), so each impact is drawn as
// one wave train from where it struck (water_impact_ripple(), raytracing_rain_inc.glsl): the latest MAX of them, as a
// row of texels (x, z, age s, amplitude) that the raster and the path tracer both read (water_impacts and
// water_impact_info, published by GroundSurface3D).
class GroundWaterImpacts {
public:
	static constexpr int MAX = 16;
	static constexpr float LIFE = 1.6f; // s, water_impact_ripple()'s
	// A body moving through the water leaves a train every this far or this long, whichever comes first.
	static constexpr float MOVE_SPACING = 0.25f; // m
	static constexpr float MOVE_PERIOD = 0.35f; // s

	struct Impact {
		float x = 0.0f;
		float z = 0.0f;
		float age = 0.0f;
		float amplitude = 0.0f;
	};

	// One impact (amplitude 1: a walker's step), the oldest dropped when the row is full.
	void add(const Vector3 &p_position, float p_amplitude) {
		if (p_amplitude <= 0.01f) {
			return;
		}
		if (live.size() >= uint32_t(MAX)) {
			uint32_t oldest = 0;
			for (uint32_t i = 1; i < live.size(); i++) {
				if (live[i].age > live[oldest].age) {
					oldest = i;
				}
			}
			live.remove_at_unordered(oldest);
		}
		Impact im;
		im.x = p_position.x;
		im.z = p_position.z;
		im.amplitude = MIN(p_amplitude, 2.0f);
		live.push_back(im);
	}

	// A body moving in the water (owner-keyed): a train every MOVE_SPACING m or MOVE_PERIOD s while it moves.
	void move(uint64_t p_owner, const Vector3 &p_position, float p_amplitude) {
		Mover *m = movers.getptr(p_owner);
		if (!m) {
			Mover n;
			n.last = p_position;
			n.seen = clock;
			n.since = clock;
			movers.insert(p_owner, n);
			add(p_position, p_amplitude);
			return;
		}
		m->seen = clock;
		const float d = Vector2(p_position.x - m->last.x, p_position.z - m->last.z).length();
		if (d >= MOVE_SPACING || (d > 0.02f && clock - m->since >= MOVE_PERIOD)) {
			m->last = p_position;
			m->since = clock;
			add(p_position, p_amplitude);
		}
	}
	void clear(uint64_t p_owner) { movers.erase(p_owner); }

	// Ages the impacts by p_delta; true when the row changed and wants publishing (fill_row()).
	bool update(double p_delta) {
		clock += p_delta;
		for (uint32_t i = 0; i < live.size();) {
			live[i].age += float(p_delta);
			if (live[i].age >= LIFE) {
				live.remove_at_unordered(i);
			} else {
				i++;
			}
		}
		LocalVector<uint64_t> gone;
		for (const KeyValue<uint64_t, Mover> &kv : movers) {
			if (clock - kv.value.seen > 1.0) {
				gone.push_back(kv.key);
			}
		}
		for (uint64_t o : gone) {
			movers.erase(o);
		}
		const bool changed = !live.is_empty() || published_count > 0;
		return changed;
	}

	// The row's texture (created on first use) refreshed with the live impacts; their count.
	int fill_row() {
		if (image.is_null()) {
			image = Image::create_empty(MAX, 1, false, Image::FORMAT_RGBAF);
		}
		for (int i = 0; i < MAX; i++) {
			const Impact im = i < int(live.size()) ? live[i] : Impact();
			image->set_pixel(i, 0, Color(im.x, im.z, im.age, im.amplitude));
		}
		if (texture.is_null()) {
			texture = ImageTexture::create_from_image(image);
		} else {
			texture->update(image);
		}
		published_count = int(live.size());
		return published_count;
	}
	Ref<ImageTexture> get_texture() const { return texture; }
	int get_count() const { return int(live.size()); }
	const LocalVector<Impact> &get_impacts() const { return live; }

private:
	struct Mover {
		Vector3 last;
		double seen = 0.0;
		double since = 0.0;
	};
	LocalVector<Impact> live;
	HashMap<uint64_t, Mover> movers;
	double clock = 0.0;
	int published_count = 0;
	Ref<Image> image;
	Ref<ImageTexture> texture;
};
