/**************************************************************************/
/*  ground_lump_store.cpp                                                 */
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


#include "ground_lump_store.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"

String GroundLumpStore::_path(Vector2i p_tile) const {
	return dir.path_join(prefix + itos(p_tile.x) + "_" + itos(p_tile.y) + ".lmp");
}

void GroundLumpStore::configure(const String &p_dir, const String &p_kind) {
	const String pre = "lumps_" + p_kind + "_";
	if (p_dir == dir && pre == prefix) {
		return;
	}
	flush();
	dir = p_dir;
	prefix = pre;
	scanned = false;
	on_disk.clear();
}

void GroundLumpStore::_scan() {
	scanned = true;
	on_disk.clear();
	if (dir.is_empty() || !DirAccess::dir_exists_absolute(dir)) {
		return;
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String f = da->get_next(); !f.is_empty(); f = da->get_next()) {
		if (da->current_is_dir() || !f.begins_with(prefix) || !f.ends_with(".lmp")) {
			continue;
		}
		const PackedStringArray xz = f.substr(prefix.length(), f.length() - prefix.length() - 4).split("_");
		if (xz.size() == 2 && xz[0].is_valid_int() && xz[1].is_valid_int()) {
			on_disk.insert(Vector2i(xz[0].to_int(), xz[1].to_int()));
		}
	}
	da->list_dir_end();
}

void GroundLumpStore::park(const GroundLump &p_lump) {
	const Vector2i t = tile_of(p_lump.position);
	if (!dir.is_empty() && !parked.has(t)) {
		_load(t); // a tile half on disk gets its file's lumps back first, so the write keeps them
	}
	parked[t].push_back(p_lump);
	dirty.insert(t);
}

void GroundLumpStore::_load(Vector2i p_tile) {
	if (!scanned) {
		_scan();
	}
	if (!on_disk.has(p_tile)) {
		return;
	}
	on_disk.erase(p_tile);
	const String path = _path(p_tile);
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
	if (f.is_valid()) {
		const uint32_t n = f->get_32();
		LocalVector<GroundLump> &list = parked[p_tile];
		float v[FLOATS];
		for (uint32_t i = 0; i < n && !f->eof_reached(); i++) {
			for (int k = 0; k < FLOATS; k++) {
				v[k] = f->get_float();
			}
			GroundLump l;
			l.position = Vector3(v[0], v[1], v[2]);
			l.basis = Basis(v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
			l.size = v[12];
			l.relief = v[13];
			l.buried = v[14];
			l.color = Color(v[15], v[16], v[17]);
			l.shape = uint8_t(v[18]);
			list.push_back(l);
		}
		f.unref();
	}
	// The lumps live in memory now; the file is written again when they leave.
	DirAccess::remove_absolute(path);
}

void GroundLumpStore::take(const Rect2 &p_rect, LocalVector<GroundLump> &r_out) {
	const Vector2i a(int(Math::floor(p_rect.position.x / TILE)), int(Math::floor(p_rect.position.y / TILE)));
	const Vector2i b(int(Math::floor(p_rect.get_end().x / TILE)), int(Math::floor(p_rect.get_end().y / TILE)));
	for (int z = a.y; z <= b.y; z++) {
		for (int x = a.x; x <= b.x; x++) {
			const Vector2i t(x, z);
			if (!dir.is_empty()) {
				_load(t);
			}
			LocalVector<GroundLump> *list = parked.getptr(t);
			if (!list) {
				continue;
			}
			for (uint32_t i = 0; i < list->size();) {
				const GroundLump &l = (*list)[i];
				if (p_rect.has_point(Vector2(l.position.x, l.position.z))) {
					r_out.push_back(l);
					(*list)[i] = (*list)[list->size() - 1];
					list->resize(list->size() - 1);
				} else {
					i++;
				}
			}
			if (list->is_empty()) {
				parked.erase(t);
				dirty.erase(t);
			}
		}
	}
}

void GroundLumpStore::flush() {
	if (dir.is_empty()) {
		dirty.clear();
		return;
	}
	if (!DirAccess::dir_exists_absolute(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	for (const Vector2i &t : dirty) {
		const LocalVector<GroundLump> *list = parked.getptr(t);
		if (!list) {
			continue;
		}
		Ref<FileAccess> f = FileAccess::open_compressed(_path(t), FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
		if (f.is_null()) {
			continue;
		}
		f->store_32(list->size());
		for (const GroundLump &l : *list) {
			const float v[FLOATS] = { l.position.x, l.position.y, l.position.z, l.basis.rows[0].x, l.basis.rows[0].y, l.basis.rows[0].z, l.basis.rows[1].x, l.basis.rows[1].y, l.basis.rows[1].z, l.basis.rows[2].x, l.basis.rows[2].y, l.basis.rows[2].z, l.size, l.relief, l.buried, l.color.r, l.color.g, l.color.b, float(l.shape), 0.0f };
			for (int k = 0; k < FLOATS; k++) {
				f->store_float(v[k]);
			}
		}
		f.unref();
		on_disk.insert(t);
		parked.erase(t);
	}
	dirty.clear();
}

int GroundLumpStore::get_parked_count() const {
	int n = 0;
	for (const KeyValue<Vector2i, LocalVector<GroundLump>> &kv : parked) {
		n += int(kv.value.size());
	}
	return n;
}

void GroundLumpStore::clear(bool p_files) {
	parked.clear();
	dirty.clear();
	if (p_files && !dir.is_empty()) {
		if (!scanned) {
			_scan();
		}
		for (const Vector2i &t : on_disk) {
			DirAccess::remove_absolute(_path(t));
		}
	}
	on_disk.clear();
}
