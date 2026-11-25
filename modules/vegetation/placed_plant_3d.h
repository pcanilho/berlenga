/**************************************************************************/
/*  placed_plant_3d.h                                                     */
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

#include "plant_species.h"

#include "scene/3d/node_3d.h"

class MeshInstance3D;
class WorldExclusion3D;

// One plant placed by hand (docs/ideas/editor-tools.md, C; Unreal foliage mode's single place): a variant of a
// PlantSpecies at this node's transform. It draws the species' mesh and keeps the procedural plants (and, with
// clear_grass, the grass) out of its footprint, so a regenerated field never covers or removes it.
class PlacedPlant3D : public Node3D {
	GDCLASS(PlacedPlant3D, Node3D);

	Ref<PlantSpecies> species;
	int variant = 0;
	bool clear_grass = false;
	real_t clearance = -1.0; // m; < 0: the species' footprint
	MeshInstance3D *mesh_instance = nullptr;
	WorldExclusion3D *keep_out = nullptr;

	void _update();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_species(const Ref<PlantSpecies> &p_species);
	Ref<PlantSpecies> get_species() const { return species; }
	void set_variant(int p_variant);
	int get_variant() const { return variant; }
	void set_clear_grass(bool p_enable);
	bool is_clear_grass() const { return clear_grass; }
	void set_clearance(real_t p_metres);
	real_t get_clearance() const { return clearance; }
	real_t get_effective_clearance() const;

	PackedStringArray get_configuration_warnings() const override;
};
