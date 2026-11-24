/**************************************************************************/
/*  world_cache.cpp                                                       */
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

#include "world_cache.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

namespace {

Mutex cache_mutex;
bool resolved = false;
bool enabled = false;
String root;
uint64_t exe_stamp[2] = { 0, 0 };
SafeNumeric<uint64_t> loaded_count;
SafeNumeric<uint64_t> stored_count;
const uint32_t FILE_MAGIC = 0x31435742; // "BWC1"
const uint64_t HEADER_SIZE = 20; // magic, raw size (u64), packed size, reserved

void resolve() {
	MutexLock lock(cache_mutex);
	if (resolved) {
		return;
	}
	resolved = true;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	enabled = ps && bool(ps->get_setting("world/cache/enabled", true));
	if (!enabled) {
		return;
	}
	String path = ps->get_setting("world/cache/path", String());
	if (path.is_empty()) {
		path = OS::get_singleton()->get_cache_path().path_join("berlenga").path_join("world");
	} else if (path.begins_with("res://") || path.begins_with("user://")) {
		path = ps->globalize_path(path);
	}
	const String exe = OS::get_singleton()->get_executable_path();
	exe_stamp[0] = FileAccess::get_modified_time(exe);
	Ref<FileAccess> f = FileAccess::open(exe, FileAccess::READ);
	exe_stamp[1] = f.is_valid() ? f->get_length() : 0;
	if (exe_stamp[0] == 0 || DirAccess::make_dir_recursive_absolute(path) != OK) {
		enabled = false; // no stamp for the engine, or nowhere to write: compute everything
		return;
	}
	root = path;
}

String piece_path(const char *p_stage, const String &p_name) {
	return root.path_join(String(p_stage) + "_" + p_name + ".bin");
}

} // namespace

WorldCacheKey::WorldCacheKey(const char *p_stage) {
	resolve();
	stage = p_stage;
	CryptoCore::SHA256Context *c = memnew(CryptoCore::SHA256Context);
	c->start();
	ctx = c;
	const char tag[] = "berlenga world cache 1";
	bytes(tag, sizeof(tag));
	bytes(p_stage, strlen(p_stage) + 1);
	bytes(exe_stamp, sizeof(exe_stamp));
}

WorldCacheKey::~WorldCacheKey() {
	memdelete(static_cast<CryptoCore::SHA256Context *>(ctx));
}

void WorldCacheKey::bytes(const void *p_data, size_t p_size) {
	if (p_size > 0) {
		static_cast<CryptoCore::SHA256Context *>(ctx)->update(static_cast<const uint8_t *>(p_data), p_size);
	}
}

bool WorldCacheKey::object(const Object *p_object) {
	if (!p_object) {
		value(uint8_t(0));
		return true;
	}
	const Variant v(const_cast<Object *>(p_object));
	int len = 0;
	if (encode_variant(v, nullptr, len, true) != OK || len <= 0) {
		return false;
	}
	Vector<uint8_t> buf;
	buf.resize(len);
	if (encode_variant(v, buf.ptrw(), len, true) != OK) {
		return false;
	}
	array(buf.ptr(), buf.size());
	return true;
}

String WorldCacheKey::finish() {
	unsigned char hash[32];
	static_cast<CryptoCore::SHA256Context *>(ctx)->finish(hash);
	return String::hex_encode_buffer(hash, 16);
}

bool WorldCache::is_enabled() {
	resolve();
	return enabled;
}

namespace {

// The piece's header and packed bytes; false when it is missing, foreign or short.
bool read_piece(const char *p_stage, const String &p_name, uint64_t &r_raw, Vector<uint8_t> &r_packed) {
	Ref<FileAccess> f = FileAccess::open(piece_path(p_stage, p_name), FileAccess::READ);
	if (f.is_null() || f->get_length() < HEADER_SIZE || f->get_32() != FILE_MAGIC) {
		return false;
	}
	r_raw = f->get_64();
	const uint32_t packed = f->get_32();
	if (packed != f->get_length() - HEADER_SIZE || (packed == 0) != (r_raw == 0)) {
		return false;
	}
	f->get_32(); // reserved
	r_packed.resize(packed);
	return f->get_buffer(r_packed.ptrw(), packed) == packed;
}

} // namespace

bool WorldCache::load(const char *p_stage, const String &p_name, uint8_t *r_data, size_t p_size) {
	if (!is_enabled()) {
		return false;
	}
	uint64_t raw = 0;
	Vector<uint8_t> src;
	if (!read_piece(p_stage, p_name, raw, src) || raw != p_size) {
		return false;
	}
	if (p_size > 0 && Compression::decompress(r_data, int64_t(p_size), src.ptr(), src.size(), Compression::MODE_ZSTD) != int64_t(p_size)) {
		return false;
	}
	loaded_count.increment();
	return true;
}

bool WorldCache::load_any(const char *p_stage, const String &p_name, Vector<uint8_t> &r_data) {
	if (!is_enabled()) {
		return false;
	}
	uint64_t raw = 0;
	Vector<uint8_t> src;
	if (!read_piece(p_stage, p_name, raw, src) || raw > (uint64_t(1) << 31)) {
		return false;
	}
	r_data.resize(int64_t(raw));
	if (raw > 0 && Compression::decompress(r_data.ptrw(), int64_t(raw), src.ptr(), src.size(), Compression::MODE_ZSTD) != int64_t(raw)) {
		return false;
	}
	loaded_count.increment();
	return true;
}

void WorldCache::store(const char *p_stage, const String &p_name, const uint8_t *p_data, size_t p_size) {
	if (!is_enabled()) {
		return;
	}
	Vector<uint8_t> dst;
	dst.resize(Compression::get_max_compressed_buffer_size(int64_t(p_size), Compression::MODE_ZSTD));
	const int64_t packed = p_size > 0 ? Compression::compress(dst.ptrw(), p_data, int64_t(p_size), Compression::MODE_ZSTD) : 0;
	if (packed < 0) {
		return;
	}
	// Written beside its name and moved over it, so a reader never sees half a piece.
	const String path = piece_path(p_stage, p_name);
	const String tmp = path + "." + itos(OS::get_singleton()->get_process_id()) + "." + itos(int64_t(Thread::get_caller_id() & 0x7fffffff)) + ".tmp";
	{
		Ref<FileAccess> f = FileAccess::open(tmp, FileAccess::WRITE);
		if (f.is_null()) {
			return;
		}
		f->store_32(FILE_MAGIC);
		f->store_64(p_size);
		f->store_32(uint32_t(packed));
		f->store_32(0);
		f->store_buffer(dst.ptr(), packed);
	}
	if (DirAccess::rename_absolute(tmp, path) != OK) {
		DirAccess::remove_absolute(tmp);
		return;
	}
	stored_count.increment();
}

uint64_t WorldCache::get_loaded() {
	return loaded_count.get();
}

uint64_t WorldCache::get_stored() {
	return stored_count.get();
}
