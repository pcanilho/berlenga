/**************************************************************************/
/*  bindless_block.h                                                      */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/pair.h"
#include "servers/rendering/rendering_device.h"

class BindlessBlock {
public:
	constexpr static uint32_t MAX_BINDLESS_TEXTURES = 128000;

	uint32_t add_texture(RID p_texture);

	void initialize(RenderingDevice *p_rd);

	void begin_frame();

	void finalize(RID p_shader, uint32_t p_set_index);

	// For consumers whose set layout differs from the finalized one (e.g. other shader stages).
	// Recreate when get_version() changes.
	RID create_uniform_set(RID p_shader, uint32_t p_set_index) const;
	uint64_t get_version() const { return version; }

	RID get_uniform_set() const { return uniform_set; }
	uint32_t get_texture_count() const { return textures.size(); }
	bool is_initialized() const { return rd != nullptr; }
	bool is_finalized() const { return uniform_set.is_valid(); }

	void clear();
	~BindlessBlock();

	// A texture swapped in place (TextureStorage::texture_replace_compatible, RenderingDevice::texture_replace_rid:
	// texture streaming changes a texture's mips this way). RenderingDevice patches the uniform set; every block
	// moves its slot to the new texture, so materials keep their indices. Call it before
	// RenderingDevice::texture_replace_rid: the block releases its sets instead of letting RenderingDevice rebuild
	// the whole table once per swapped texture, and builds them once at the next finalize.
	static void texture_replaced(RID p_old, RID p_new);
	// Others that keep RenderingDevice texture RIDs of materials (the path tracer's material cache) follow too.
	typedef void (*ReplaceListener)(void *p_userdata, RID p_old, RID p_new);
	static void add_replace_listener(ReplaceListener p_listener, void *p_userdata);
	static void remove_replace_listener(ReplaceListener p_listener, void *p_userdata);

private:
	RenderingDevice *rd = nullptr;
	LocalVector<RID> textures;
	HashMap<RID, uint32_t> texture_to_index;
	LocalVector<uint32_t> free_indices;
	uint32_t free_indices_count = 0; // logical top to avoid resize of free_indices array
	RID default_texture;
	RID uniform_set;
	bool needs_refinalize = false;
	bool released_for_swap = false; // Sets released by texture_replaced(), not by a texture going away.
	uint64_t version = 0;
	mutable LocalVector<RID> consumer_sets; // Made by create_uniform_set(); released on a swap too.

	void _texture_replaced(RID p_old, RID p_new);
	static LocalVector<BindlessBlock *> live_blocks;
	static LocalVector<Pair<ReplaceListener, void *>> replace_listeners;
};
