/**************************************************************************/
/*  world_cache.h                                                         */
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

// World cache: the results of the terrain's expensive deterministic stages (dune tiles, erosion tiles, rock obstacle
// cells, the snow climate), kept on disk under a hash of everything a piece reads, so a later launch loads the same
// bytes instead of computing them again (docs/ideas/optimisation.md, "world cache"). Pieces are per stage and per
// zone (a tile or cell), so an edit recomputes only the zones whose inputs changed. The key also holds the running
// executable's size and modification time, so a rebuilt engine never reads results of other code.
//
// Settings: world/cache/enabled (default on) and world/cache/path (default: the OS cache folder's berlenga/world).
// Thread-safe: stages call it from worker threads.

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"

#include <cstdint>

class WorldCacheKey {
	void *ctx = nullptr;

public:
	explicit WorldCacheKey(const char *p_stage);
	~WorldCacheKey();
	WorldCacheKey(const WorldCacheKey &) = delete;
	WorldCacheKey &operator=(const WorldCacheKey &) = delete;

	void bytes(const void *p_data, size_t p_size);
	template <typename T>
	void value(const T &p_value) { bytes(&p_value, sizeof(T)); }
	template <typename T>
	void array(const T *p_data, size_t p_count) {
		value(uint64_t(p_count));
		bytes(p_data, sizeof(T) * p_count);
	}
	// The stage's file name for everything added so far. Call once.
	String finish();

	const char *stage = "";
};

class WorldCache {
public:
	static bool is_enabled();
	// The bytes stored under p_name for p_stage, when they are there and exactly p_size long.
	static bool load(const char *p_stage, const String &p_name, uint8_t *r_data, size_t p_size);
	static void store(const char *p_stage, const String &p_name, const uint8_t *p_data, size_t p_size);
	// Counters for the log: pieces loaded and stored since the start.
	static uint64_t get_loaded();
	static uint64_t get_stored();
};
