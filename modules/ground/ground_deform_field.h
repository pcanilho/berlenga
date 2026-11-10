/**************************************************************************/
/*  ground_deform_field.h                                                 */
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

#include "core/math/rect2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/object/object_id.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

class Image;
class ImageTexture;
class Node3D;
class SceneTree;

// The deformation field shared by SnowSurface3D and GroundSurface3D (docs/ideas/ground-deform.md):
// a camera-following square of dent and rim heights, in texels of extent / resolution, cut into
// TILE-texel tiles that go to a bounded cache when the field moves off them and come back when
// it returns. Pressing is mass conserving: a pressed column keeps (1 - rim share) of what it
// loses as compaction and pushes the rest into a rim around the contact (Sumner, O'Brien and
// Hodgins 1999). Slumping moves what deformation piled beyond the angle of repose downhill.
// The owner says what each column is made of through Columns.
class GroundDeformField {
public:
	static constexpr int TILE = 64;

	struct Column {
		float surface = 0.0f; // undisturbed surface height (ground, or ground plus snow)
		float max_dent = 0.0f; // deepest the column can be pressed below it
		float rim_share = 0.3f; // of what is pressed out of it, the share pushed sideways
		bool deformable = false;
	};
	// A body over the field, read from its first collision shape.
	struct Body {
		Vector3 center;
		float radius = 0.3f;
		float half_height = 0.3f;
		bool round = true;
		bool wheel = false;
		bool box = false;
		Vector2 box_half;
		float yaw = 0.0f;
		float width = 0.0f; // contact width for Bekker, 0 = from the shape
		float flat_area = 0.0f;
		float mass = 0.0f; // kg on the ground, 0 = only pushes to its underside
		float area = 0.0f; // contact area, m2, 0 = from the shape
	};
	class Columns {
	public:
		virtual Column column(float p_x, float p_z) const = 0;
		// Bekker sinkage of p_body at its position, m.
		virtual float sinkage(const Body &p_body) const = 0;
		virtual ~Columns() {}
	};

	float extent = 32.0f;
	int resolution = 512;
	int max_cached_tiles = 1024;
	bool track_fresh = false; // a third channel, 1 where freshly turned, decayed by the owner
	float press_settle = 0.0f; // seconds of slumping a pressed tile owes (0: none, snow)

	Vector2i origin; // texel index of the first texel
	bool valid = false;
	LocalVector<float> dent; // m below the undisturbed surface
	LocalVector<float> rim; // m on top
	LocalVector<float> fresh;
	LocalVector<uint8_t> tile_active; // something deformed in the tile
	LocalVector<float> tile_settle; // seconds of slumping owed (0: asleep)
	LocalVector<float> tile_wait; // seconds a slow tile has saved up before its next pass
	bool dirty = false; // changed since the last upload
	bool shape_dirty = false; // changed since the owner last read heights (collision)

	float texel() const { return extent / resolution; }
	float tile_size() const { return texel() * TILE; }
	int tiles() const { return resolution / TILE; }
	Rect2 rect() const { return Rect2(origin.x * texel(), origin.y * texel(), extent, extent); }
	// The texel origin a field centred near p_center would have (moves in whole tiles).
	Vector2i origin_for(Vector2 p_center) const;
	Vector2 center_for(Vector2 p_center) const; // the snapped centre
	void shift(Vector2i p_origin);
	void invalidate() { valid = false; }
	void clear();
	int cached_tile_count() const { return cache.size(); }

	float sample(const LocalVector<float> &p_field, float p_x, float p_z) const;
	float dent_at(float p_x, float p_z) const { return valid ? sample(dent, p_x, p_z) : 0.0f; }
	float rim_at(float p_x, float p_z) const { return valid ? sample(rim, p_x, p_z) : 0.0f; }
	Vector2 moved_volume() const; // m3: (dent, rim)

	// Presses down to p_bottom within the footprint (a bowl when p_round, an oriented box when
	// p_box), and p_sink below the undisturbed surface. Returns the volume moved, m3.
	float press(const Columns &p_columns, Vector3 p_position, float p_radius, Vector2 p_half, float p_yaw, bool p_box, float p_bottom, float p_sink, bool p_round, float p_contact_tolerance);
	// Bodies in p_groups press along the path they moved since the last call.
	void press_bodies(SceneTree *p_tree, const PackedStringArray &p_groups, const Columns &p_columns, float p_character_mass, float p_character_area, float p_contact_tolerance, double p_delta);
	static bool read_body(Node3D *p_node, float p_character_mass, float p_character_area, Body &r_body);
	static float bekker_sinkage(float p_pressure_kpa, float p_width, float p_n, float p_kc, float p_kphi); // m
	// Bekker sinkage of a body, with the patch growing with sinkage for round bodies and wheels.
	static float body_sinkage(const Body &p_body, float p_n, float p_kc, float p_kphi);

	// Sumner's erosion step on tiles that owe slumping: where the surface step between neighbours
	// is more than the undisturbed one and more than tan(repose) times their distance, a share of the
	// excess moves down. p_ground(x, z) is the undisturbed surface; p_tan(x, z) the repose slope and
	// p_rate(x, z) the share moved per second (very large: at once). Returns texels changed.
	template <typename G, typename T>
	int slump(double p_delta, const G &p_ground, const T &p_params);
	// Marks the tiles under a rect (world XZ) as owing p_seconds of slumping.
	void settle(Rect2 p_rect, float p_seconds);

	// RGH (dent, rim) or, with p_extra, RGBAH (dent, rim, extra channels).
	void upload(Ref<Image> &r_image, Ref<ImageTexture> &r_texture, const LocalVector<float> *p_b = nullptr, const LocalVector<float> *p_a = nullptr);

private:
	struct CachedTile {
		Vector<uint16_t> data;
		uint64_t used = 0;
	};
	HashMap<Vector2i, CachedTile> cache;
	uint64_t cache_clock = 0;
	struct Mover {
		Vector3 last;
		uint64_t frame = 0;
	};
	HashMap<ObjectID, Mover> movers;
	uint64_t frame = 0;

	int channels() const { return track_fresh ? 3 : 2; }
	void _store_tile(Vector2i p_tile_world, int p_tx, int p_tz);
	bool _load_tile(Vector2i p_tile_world, int p_tx, int p_tz);
	void _trim_cache();
	void _mark(int p_x, int p_z, float p_settle = 0.0f);
};

template <typename G, typename T>
int GroundDeformField::slump(double p_delta, const G &p_ground, const T &p_params) {
	if (!valid) {
		return 0;
	}
	const int n = resolution;
	const int nt = tiles();
	const float t = texel();
	static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	int changed = 0;
	for (int tz = 0; tz < nt; tz++) {
		for (int tx = 0; tx < nt; tx++) {
			float &owed = tile_settle[tz * nt + tx];
			if (owed <= 0.0f) {
				continue;
			}
			// Slow material (mud creeping back) is stepped less often, a twentieth of the way each pass.
			float dt = float(p_delta);
			{
				float c_tan;
				float c_rate = 1e3f;
				float c_max;
				const float cx = (origin.x + tx * TILE + TILE / 2) * t;
				const float cz = (origin.y + tz * TILE + TILE / 2) * t;
				if (p_params(cx, cz, c_tan, c_rate, c_max) && c_rate * (tile_wait[tz * nt + tx] + dt) < 0.05f) {
					tile_wait[tz * nt + tx] += dt;
					continue;
				}
				dt += tile_wait[tz * nt + tx];
				tile_wait[tz * nt + tx] = 0.0f;
			}
			owed = MAX(owed - dt, 0.0f);
			float worst = 0.0f;
			for (int z = MAX(tz * TILE, 1); z < MIN((tz + 1) * TILE, n - 1); z++) {
				for (int x = MAX(tx * TILE, 1); x < MIN((tx + 1) * TILE, n - 1); x++) {
					const int i = z * n + x;
					const float wx = (origin.x + x + 0.5f) * t;
					const float wz = (origin.y + z + 0.5f) * t;
					float tan_repose;
					float rate;
					float max_dent;
					if (!p_params(wx, wz, tan_repose, rate, max_dent)) {
						continue;
					}
					const float gi = p_ground(wx, wz);
					const float si = gi + rim[i] - dent[i];
					float excess[8];
					float sum = 0.0f;
					float big = 0.0f;
					for (int k = 0; k < 8; k++) {
						const int j = (z + DZ[k]) * n + x + DX[k];
						const float dist = k < 4 ? t : t * Math::SQRT2;
						const float gj = p_ground((origin.x + x + DX[k] + 0.5f) * t, (origin.y + z + DZ[k] + 0.5f) * t);
						const float sj = gj + rim[j] - dent[j];
						// Only what deformation added to the step beyond the repose slope (and beyond the step the
						// undisturbed ground already had, so slopes steeper than the repose stay as they are).
						const float limit = MAX(tan_repose * dist, gi - gj);
						const float e = (si - sj) - limit;
						excess[k] = e > 0.0f ? e : 0.0f;
						sum += excess[k];
						big = MAX(big, excess[k]);
					}
					worst = MAX(worst, big);
					if (big <= 1e-5f) {
						continue;
					}
					// Half the largest excess evens the steepest pair; a share of it per step.
					const float share = MIN(1.0f, rate * dt);
					float move = 0.5f * big * share;
					// What the column can give: its rim, then down to its deepest dent.
					move = MIN(move, rim[i] + MAX(max_dent - dent[i], 0.0f));
					if (move <= 1e-6f) {
						continue;
					}
					const float take = MIN(rim[i], move);
					rim[i] -= take;
					dent[i] += move - take;
					for (int k = 0; k < 8; k++) {
						if (excess[k] <= 0.0f) {
							continue;
						}
						const int xj = x + DX[k];
						const int zj = z + DZ[k];
						const int j = zj * n + xj;
						const float add = move * excess[k] / sum;
						const float fill = MIN(dent[j], add);
						dent[j] -= fill;
						rim[j] += add - fill;
						_mark(xj, zj, owed + dt);
					}
					changed++;
				}
			}
			if (worst < 1e-4f) {
				owed = 0.0f; // settled to a tenth of a millimetre
			}
		}
	}
	if (changed) {
		dirty = true;
		shape_dirty = true;
	}
	return changed;
}
