/**************************************************************************/
/*  placed_plant_3d.cpp                                                   */
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

#include "placed_plant_3d.h"

#include "modules/world/world_exclusion_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"

real_t PlacedPlant3D::get_effective_clearance() const {
	if (clearance >= 0.0) {
		return clearance;
	}
	return species.is_valid() ? real_t(MAX(species->get_footprint(), 0.1f)) : real_t(1.0);
}

void PlacedPlant3D::_update() {
	if (!is_inside_tree()) {
		return;
	}
	Ref<Mesh> mesh;
	if (species.is_valid()) {
		const TypedArray<Mesh> meshes = species->get_meshes();
		if (!meshes.is_empty()) {
			mesh = meshes[CLAMP(variant, 0, meshes.size() - 1)];
		}
	}
	if (!mesh_instance) {
		mesh_instance = memnew(MeshInstance3D);
		add_child(mesh_instance, false, INTERNAL_MODE_FRONT); // no owner: never saved
	}
	mesh_instance->set_mesh(mesh);
	if (!keep_out) {
		keep_out = memnew(WorldExclusion3D);
		add_child(keep_out, false, INTERNAL_MODE_FRONT);
	}
	// A square of twice the clearance round the stem; the node's scale is the plant's, not the clearance's.
	const real_t c = get_effective_clearance();
	const Vector3 s = get_global_transform().basis.get_scale();
	keep_out->set_scale(Vector3(1.0 / MAX(s.x, real_t(1e-3)), 1.0, 1.0 / MAX(s.z, real_t(1e-3))));
	keep_out->set_size(Vector2(c * 2.0, c * 2.0));
	keep_out->set_fade(0.0);
	keep_out->set_kinds(WorldExclusion3D::KIND_PLANTS | (clear_grass ? WorldExclusion3D::KIND_GRASS : 0) | WorldExclusion3D::KIND_ROCKS);
}

void PlacedPlant3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update();
		} break;
		case NOTIFICATION_PREDELETE: {
			mesh_instance = nullptr;
			keep_out = nullptr;
		} break;
	}
}

void PlacedPlant3D::set_species(const Ref<PlantSpecies> &p_species) {
	species = p_species;
	_update();
	update_configuration_warnings();
}

void PlacedPlant3D::set_variant(int p_variant) {
	variant = MAX(p_variant, 0);
	_update();
}

void PlacedPlant3D::set_clear_grass(bool p_enable) {
	clear_grass = p_enable;
	_update();
}

void PlacedPlant3D::set_clearance(real_t p_metres) {
	clearance = p_metres;
	_update();
}

PackedStringArray PlacedPlant3D::get_configuration_warnings() const {
	PackedStringArray w = Node3D::get_configuration_warnings();
	if (species.is_null() || species->get_meshes().is_empty()) {
		w.push_back(RTR("Set a PlantSpecies with meshes: it is what this node draws."));
	}
	return w;
}

void PlacedPlant3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_species", "species"), &PlacedPlant3D::set_species);
	ClassDB::bind_method(D_METHOD("get_species"), &PlacedPlant3D::get_species);
	ClassDB::bind_method(D_METHOD("set_variant", "variant"), &PlacedPlant3D::set_variant);
	ClassDB::bind_method(D_METHOD("get_variant"), &PlacedPlant3D::get_variant);
	ClassDB::bind_method(D_METHOD("set_clear_grass", "enable"), &PlacedPlant3D::set_clear_grass);
	ClassDB::bind_method(D_METHOD("is_clear_grass"), &PlacedPlant3D::is_clear_grass);
	ClassDB::bind_method(D_METHOD("set_clearance", "metres"), &PlacedPlant3D::set_clearance);
	ClassDB::bind_method(D_METHOD("get_clearance"), &PlacedPlant3D::get_clearance);
	ClassDB::bind_method(D_METHOD("get_effective_clearance"), &PlacedPlant3D::get_effective_clearance);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "species", PROPERTY_HINT_RESOURCE_TYPE, "PlantSpecies"), "set_species", "get_species");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "variant", PROPERTY_HINT_RANGE, "0,64,1,or_greater"), "set_variant", "get_variant");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "clear_grass"), "set_clear_grass", "is_clear_grass");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clearance", PROPERTY_HINT_RANGE, "-1,50,0.05,or_greater,suffix:m"), "set_clearance", "get_clearance");
}
