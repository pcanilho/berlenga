/**************************************************************************/
/*  ground_rut_store.h                                                    */
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

#include "ground_deform_field.h"

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

// Ruts kept across the world (mud round 3, docs/ideas/mud.md): the field's tiles written to disk as they leave it
// and read back when it comes over them again, so a print made anywhere is there when the player returns, in this
// session or the next. One zstd file per world tile, in the cache's own layout (TILE x TILE texels of two uints:
// half dent | half rim, half fresh tint | half water depth), named by the field's texel so fields of other sizes
// never read each other's.
class GroundRutStore {
	String dir;
	String prefix;
	bool scanned = false;
	HashSet<Vector2i> on_disk;

	String _path(Vector2i p_tile) const;
	void _scan();

public:
	// p_dir empty: off. Rescans when the folder or the field's texel changed.
	void configure(const String &p_dir, const String &p_kind, float p_texel);
	bool is_on() const { return !dir.is_empty(); }
	bool has(Vector2i p_tile);
	// Writes the field's tile (p_tx, p_tz in field tiles) at world tile p_tile, or removes its file if it is flat.
	void store(const GroundDeformField &p_field, Vector2i p_tile, int p_tx, int p_tz);
	bool load(Vector2i p_tile, LocalVector<uint32_t> &r_data);
	// Deletes every tile file of this field.
	void clear();
	int count();
	// The field's tile as the cache packs it; false if it is flat.
	static bool pack(const GroundDeformField &p_field, int p_tx, int p_tz, LocalVector<uint32_t> &r_data);
	// Writes a packed tile into the field at its tile (p_tx, p_tz); marks it active.
	static void unpack(GroundDeformField &r_field, int p_tx, int p_tz, const LocalVector<uint32_t> &p_data);
};
