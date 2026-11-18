/**************************************************************************/
/*  ground_rut_store.cpp                                                  */
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


#include "ground_rut_store.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/math/math_funcs.h"
#include "core/os/time.h"

static const uint32_t RUT_MAGIC = 0x54555247; // "GRUT"
static const uint32_t RUT_VERSION = 3; // 2: the unix time it was written, after the tile size; 3: the water's clay

void GroundRutStore::configure(const String &p_dir, const String &p_kind, float p_texel) {
	const String want_prefix = p_dir.is_empty() ? String() : vformat("%s%d_", p_kind, int(Math::round(p_texel * 1e5f)));
	if (p_dir == dir && want_prefix == prefix) {
		return;
	}
	dir = p_dir;
	prefix = want_prefix;
	scanned = false;
	on_disk.clear();
}

String GroundRutStore::_path(Vector2i p_tile) const {
	return dir.path_join(vformat("%s%d_%d.rut", prefix, p_tile.x, p_tile.y));
}

void GroundRutStore::_scan() {
	scanned = true;
	on_disk.clear();
	if (!DirAccess::dir_exists_absolute(dir)) {
		return;
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String f = da->get_next(); !f.is_empty(); f = da->get_next()) {
		if (da->current_is_dir() || !f.begins_with(prefix) || !f.ends_with(".rut")) {
			continue;
		}
		const PackedStringArray xz = f.substr(prefix.length(), f.length() - prefix.length() - 4).split("_");
		if (xz.size() == 2 && xz[0].is_valid_int() && xz[1].is_valid_int()) {
			on_disk.insert(Vector2i(xz[0].to_int(), xz[1].to_int()));
		}
	}
	da->list_dir_end();
}

bool GroundRutStore::has(Vector2i p_tile) {
	if (dir.is_empty()) {
		return false;
	}
	if (!scanned) {
		_scan();
	}
	return on_disk.has(p_tile);
}

int GroundRutStore::count() {
	if (dir.is_empty()) {
		return 0;
	}
	if (!scanned) {
		_scan();
	}
	return on_disk.size();
}

bool GroundRutStore::pack(const GroundDeformField &p_field, int p_tx, int p_tz, LocalVector<uint32_t> &r_data) {
	const int T = GroundDeformField::TILE;
	const int n = p_field.resolution;
	const bool fresh = int(p_field.fresh.size()) == n * n;
	const bool water = int(p_field.water.size()) == n * n;
	const bool clay = water && int(p_field.water_mud.size()) == n * n;
	const bool grown = int(p_field.regrown.size()) == n * n;
	r_data.resize(T * T * STRIDE);
	bool any = false;
	for (int z = 0; z < T; z++) {
		for (int x = 0; x < T; x++) {
			const int i = (p_tz * T + z) * n + p_tx * T + x;
			const float dn = p_field.dent[i];
			const float rm = p_field.rim[i];
			const float fr = fresh ? p_field.fresh[i] : 0.0f;
			const float wd = water ? p_field.water[i] : 0.0f;
			any = any || dn > 0.0f || rm > 0.0f || fr > 0.0f || wd > 0.0f;
			const float cl = clay ? MAX(p_field.water_mud[i], 0.0f) * wd : 0.0f;
			const int k = z * T + x;
			r_data[k * STRIDE + 0] = uint32_t(Math::make_half_float(dn)) | (uint32_t(Math::make_half_float(rm)) << 16);
			r_data[k * STRIDE + 1] = uint32_t(Math::make_half_float(fr)) | (uint32_t(Math::make_half_float(wd)) << 16);
			const float rg = grown ? CLAMP(p_field.regrown[i], 0.0f, 1.0f) : 0.0f;
			r_data[k * STRIDE + 2] = uint32_t(Math::make_half_float(cl)) | (uint32_t(Math::make_half_float(rg)) << 16);
		}
	}
	return any;
}

void GroundRutStore::unpack(GroundDeformField &r_field, int p_tx, int p_tz, const LocalVector<uint32_t> &p_data) {
	const int T = GroundDeformField::TILE;
	const int n = r_field.resolution;
	const int nt = r_field.tiles();
	if (int(p_data.size()) != T * T * STRIDE || p_tx < 0 || p_tz < 0 || p_tx >= nt || p_tz >= nt) {
		return;
	}
	const bool fresh = int(r_field.fresh.size()) == n * n;
	for (int z = 0; z < T; z++) {
		for (int x = 0; x < T; x++) {
			const int i = (p_tz * T + z) * n + p_tx * T + x;
			const int k = z * T + x;
			r_field.dent[i] = Math::half_to_float(uint16_t(p_data[k * STRIDE] & 0xffff));
			r_field.rim[i] = Math::half_to_float(uint16_t(p_data[k * STRIDE] >> 16));
			if (fresh) {
				r_field.fresh[i] = Math::half_to_float(uint16_t(p_data[k * STRIDE + 1] & 0xffff));
			}
		}
	}
	if (int(r_field.tile_active.size()) == nt * nt) {
		r_field.tile_active[p_tz * nt + p_tx] = 1;
	}
}

void GroundRutStore::store(const GroundDeformField &p_field, Vector2i p_tile, int p_tx, int p_tz) {
	if (dir.is_empty() || !p_field.valid) {
		return;
	}
	if (!scanned) {
		_scan();
	}
	LocalVector<uint32_t> data;
	if (!pack(p_field, p_tx, p_tz, data)) {
		if (on_disk.has(p_tile)) {
			// Flattened since it was kept (relaxed away, cleared): its file goes.
			Ref<DirAccess> da = DirAccess::open(dir);
			if (da.is_valid()) {
				da->remove(_path(p_tile).get_file());
			}
			on_disk.erase(p_tile);
		}
		return;
	}
	if (!DirAccess::exists(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	Ref<FileAccess> f = FileAccess::open_compressed(_path(p_tile), FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	ERR_FAIL_COND_MSG(f.is_null(), "GroundSurface3D: cannot write kept ruts to " + _path(p_tile) + ".");
	f->store_32(RUT_MAGIC);
	f->store_32(RUT_VERSION);
	f->store_32(GroundDeformField::TILE);
	const double now = Time::get_singleton()->get_unix_time_from_system();
	f->store_double(now);
	written[p_tile] = now;
	f->store_buffer(reinterpret_cast<const uint8_t *>(data.ptr()), data.size() * 4);
	on_disk.insert(p_tile);
}

double GroundRutStore::written_ago(Vector2i p_tile) const {
	const double *t = written.getptr(p_tile);
	return t ? Time::get_singleton()->get_unix_time_from_system() - *t : -1.0;
}

bool GroundRutStore::load(Vector2i p_tile, LocalVector<uint32_t> &r_data, double *r_age) {
	if (!has(p_tile)) {
		return false;
	}
	Ref<FileAccess> f = FileAccess::open_compressed(_path(p_tile), FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null()) {
		on_disk.erase(p_tile);
		return false;
	}
	const int T = GroundDeformField::TILE;
	if (f->get_32() != RUT_MAGIC) {
		return false;
	}
	const uint32_t version = f->get_32();
	if (version < 1 || version > RUT_VERSION || int(f->get_32()) != T) {
		return false;
	}
	const double when = version >= 2 ? f->get_double() : 0.0;
	if (r_age) {
		*r_age = when > 0.0 ? MAX(Time::get_singleton()->get_unix_time_from_system() - when, 0.0) : 0.0;
	}
	const int sw = version >= 3 ? STRIDE : 2; // before 3: no clay
	r_data.resize(T * T * STRIDE);
	if (f->get_buffer(reinterpret_cast<uint8_t *>(r_data.ptr()), T * T * sw * 4) != uint64_t(T * T * sw * 4)) {
		return false;
	}
	if (sw != STRIDE) {
		for (int k = T * T - 1; k >= 0; k--) {
			const uint32_t a = r_data[k * sw];
			const uint32_t b = r_data[k * sw + 1];
			r_data[k * STRIDE] = a;
			r_data[k * STRIDE + 1] = b;
			r_data[k * STRIDE + 2] = 0;
		}
	}
	return true;
}

void GroundRutStore::clear() {
	if (dir.is_empty()) {
		return;
	}
	if (!scanned) {
		_scan();
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_valid()) {
		for (const Vector2i &t : on_disk) {
			da->remove(_path(t).get_file());
		}
	}
	on_disk.clear();
	written.clear();
}

static const uint32_t CELL_MAGIC = 0x4c454347; // "GCEL"
static const uint32_t CELL_VERSION = 1;

static Vector2i _cell_block(Vector2i p_cell) {
	const int B = GroundCellStore::B;
	auto fd = [B](int v) { return v >= 0 ? v / B : -((-v + B - 1) / B); };
	return Vector2i(fd(p_cell.x), fd(p_cell.y));
}

void GroundCellStore::configure(const String &p_dir, const String &p_kind, float p_spacing) {
	const String want_prefix = p_dir.is_empty() ? String() : vformat("%s%d_", p_kind, int(Math::round(p_spacing * 1e5f)));
	if (p_dir == dir && want_prefix == prefix) {
		return;
	}
	if (!dir.is_empty()) {
		flush(true);
	}
	blocks.clear();
	dir = p_dir;
	prefix = want_prefix;
	scanned = false;
	on_disk.clear();
}

String GroundCellStore::_path(Vector2i p_block) const {
	return dir.path_join(vformat("%s%d_%d.cel", prefix, p_block.x, p_block.y));
}

void GroundCellStore::_scan() {
	scanned = true;
	on_disk.clear();
	if (!DirAccess::dir_exists_absolute(dir)) {
		return;
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String f = da->get_next(); !f.is_empty(); f = da->get_next()) {
		if (da->current_is_dir() || !f.begins_with(prefix) || !f.ends_with(".cel")) {
			continue;
		}
		const PackedStringArray xz = f.substr(prefix.length(), f.length() - prefix.length() - 4).split("_");
		if (xz.size() == 2 && xz[0].is_valid_int() && xz[1].is_valid_int()) {
			on_disk.insert(Vector2i(xz[0].to_int(), xz[1].to_int()));
		}
	}
	da->list_dir_end();
}

GroundCellStore::Block *GroundCellStore::_block(Vector2i p_block, bool p_create) {
	if (dir.is_empty()) {
		return nullptr;
	}
	if (Block *b = blocks.getptr(p_block)) {
		return b;
	}
	if (!scanned) {
		_scan();
	}
	if (!p_create && !on_disk.has(p_block)) {
		return nullptr;
	}
	if (blocks.size() >= 256) {
		flush(true); // a long way travelled: what is held goes to disk
	}
	Block nb;
	nb.v.resize(B * B * F);
	for (float &x : nb.v) {
		x = 0.0f;
	}
	nb.base = Time::get_singleton()->get_unix_time_from_system();
	if (on_disk.has(p_block)) {
		Ref<FileAccess> f = FileAccess::open_compressed(_path(p_block), FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
		if (f.is_valid() && f->get_32() == CELL_MAGIC && f->get_32() == CELL_VERSION && int(f->get_32()) == B && int(f->get_32()) == F) {
			const double base = f->get_double();
			LocalVector<float> v;
			v.resize(B * B * F);
			if (f->get_buffer(reinterpret_cast<uint8_t *>(v.ptr()), B * B * F * 4) == uint64_t(B * B * F * 4)) {
				nb.v = v;
				nb.base = base;
			}
		} else if (!p_create) {
			return nullptr;
		}
	}
	blocks.insert(p_block, nb);
	return blocks.getptr(p_block);
}

void GroundCellStore::put(Vector2i p_cell, const float *p_values, int p_count) {
	bool any = false;
	for (int k = 0; k < p_count; k++) {
		any = any || p_values[k] != 0.0f;
	}
	const Vector2i bk = _cell_block(p_cell);
	Block *b = _block(bk, any);
	if (!b) {
		return;
	}
	const int i = ((p_cell.y - bk.y * B) * B + (p_cell.x - bk.x * B)) * F;
	for (int k = 0; k < F - 1; k++) {
		b->v[i + k] = k < p_count ? p_values[k] : 0.0f;
	}
	b->v[i + F - 1] = float(Time::get_singleton()->get_unix_time_from_system() - b->base);
	b->dirty = true;
}

bool GroundCellStore::get(Vector2i p_cell, float *r_values, int p_count, double *r_age) {
	const Vector2i bk = _cell_block(p_cell);
	Block *b = _block(bk, false);
	if (!b) {
		return false;
	}
	const int i = ((p_cell.y - bk.y * B) * B + (p_cell.x - bk.x * B)) * F;
	bool any = false;
	for (int k = 0; k < p_count && k < F - 1; k++) {
		r_values[k] = b->v[i + k];
		any = any || r_values[k] != 0.0f;
	}
	if (r_age) {
		*r_age = MAX(Time::get_singleton()->get_unix_time_from_system() - (b->base + double(b->v[i + F - 1])), 0.0);
	}
	return any;
}

void GroundCellStore::_write(Vector2i p_block, const Block &p_b) {
	bool any = false;
	for (int c = 0; c < B * B && !any; c++) {
		for (int k = 0; k < F - 1; k++) {
			any = any || p_b.v[c * F + k] != 0.0f;
		}
	}
	if (!any) {
		if (on_disk.has(p_block)) {
			Ref<DirAccess> da = DirAccess::open(dir);
			if (da.is_valid()) {
				da->remove(_path(p_block).get_file());
			}
			on_disk.erase(p_block);
		}
		return;
	}
	if (!DirAccess::exists(dir)) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	Ref<FileAccess> f = FileAccess::open_compressed(_path(p_block), FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
	ERR_FAIL_COND_MSG(f.is_null(), "GroundSurface3D: cannot write kept ground cells to " + _path(p_block) + ".");
	f->store_32(CELL_MAGIC);
	f->store_32(CELL_VERSION);
	f->store_32(B);
	f->store_32(F);
	f->store_double(p_b.base);
	f->store_buffer(reinterpret_cast<const uint8_t *>(p_b.v.ptr()), p_b.v.size() * 4);
	on_disk.insert(p_block);
}

void GroundCellStore::flush(bool p_drop) {
	if (dir.is_empty()) {
		blocks.clear();
		return;
	}
	if (!scanned) {
		_scan();
	}
	for (KeyValue<Vector2i, Block> &kv : blocks) {
		if (kv.value.dirty) {
			_write(kv.key, kv.value);
			kv.value.dirty = false;
		}
	}
	if (p_drop) {
		blocks.clear();
	}
}

void GroundCellStore::clear() {
	blocks.clear();
	if (dir.is_empty()) {
		return;
	}
	if (!scanned) {
		_scan();
	}
	Ref<DirAccess> da = DirAccess::open(dir);
	if (da.is_valid()) {
		for (const Vector2i &b : on_disk) {
			da->remove(_path(b).get_file());
		}
	}
	on_disk.clear();
}
