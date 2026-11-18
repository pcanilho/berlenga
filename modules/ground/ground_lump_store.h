/**************************************************************************/
/*  ground_lump_store.h                                                   */
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

#include "core/math/basis.h"
#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

// A lump lying on the ground near the camera: a snow clump, a clod (docs/ideas/ground-detail.md).
struct GroundLump {
	Vector3 position; // its centre, world
	Basis basis; // rotation and a stretch of its own (unit size)
	float size = 0.0f; // m across
	float relief = 0.0f; // the print field's rim less dent under it when it landed
	float buried = 0.0f; // m of new snow over it since it fell
	Color color = Color(1, 1, 1); // sRGB
	uint8_t shape = 0;
	uint8_t tries = 0; // times it was taken back before the field under it was there
};

// Lumps kept while the camera is away, so a trail looks the same when it comes back (as the print field keeps its
// ruts): parked by world tile in memory, and on disk in p_dir when one is given (one zstd file per tile, beside the
// kept ruts), read back as the field comes over them again.
class GroundLumpStore {
public:
	static constexpr float TILE = 16.0f; // m
	static constexpr int FLOATS = 20; // per lump on disk

private:
	String dir;
	String prefix;
	bool scanned = false;
	HashSet<Vector2i> on_disk;
	HashMap<Vector2i, LocalVector<GroundLump>> parked;
	HashSet<Vector2i> dirty;

	String _path(Vector2i p_tile) const;
	void _scan();
	void _load(Vector2i p_tile);

public:
	static Vector2i tile_of(const Vector3 &p_at) { return Vector2i(int(Math::floor(p_at.x / TILE)), int(Math::floor(p_at.z / TILE))); }

	// p_dir empty: memory only. p_kind names the files (clumps, clods).
	void configure(const String &p_dir, const String &p_kind);
	void park(const GroundLump &p_lump);
	// Moves the lumps lying inside p_rect from the store to r_out (appended).
	void take(const Rect2 &p_rect, LocalVector<GroundLump> &r_out);
	// Writes the tiles parked since the last flush and lets them go from memory (only with a folder).
	void flush();
	int get_parked_count() const;
	void clear(bool p_files);
};
