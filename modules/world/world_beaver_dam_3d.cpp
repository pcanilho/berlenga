/**************************************************************************/
/*  world_beaver_dam_3d.cpp                                               */
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

#include "world_beaver_dam_3d.h"

#include "core/object/class_db.h"

void WorldBeaverDam3D::_drop() {
	if (event_id != 0) {
		WorldContracts::stand()->remove_disturbance(event_id);
		event_id = 0;
	}
}

// The stand holds the meadow while the dam is breached and in the tree; it follows the dam's place and size.
void WorldBeaverDam3D::_sync() {
	_drop();
	if (state == DAM_BREACHED && is_inside_tree()) {
		event_id = WorldContracts::stand()->add_disturbance(get_global_position(), pond_radius, WorldStand::DISTURBANCE_BEAVER, years_drained);
	}
}

void WorldBeaverDam3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_notify_transform(true);
			_sync();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (event_id != 0) {
				_sync();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_drop();
		} break;
	}
}

void WorldBeaverDam3D::set_state(DamState p_state) {
	if (state == p_state) {
		return;
	}
	state = p_state;
	_sync();
}

void WorldBeaverDam3D::set_pond_radius(float p_radius) {
	pond_radius = MAX(p_radius, 1.0f);
	if (event_id != 0) {
		_sync();
	}
}

void WorldBeaverDam3D::set_years_drained(float p_years) {
	years_drained = MAX(p_years, 0.0f);
	if (event_id != 0) {
		WorldContracts::stand()->set_disturbance_age(event_id, years_drained);
	}
}

void WorldBeaverDam3D::breach() {
	if (state == DAM_BREACHED) {
		return;
	}
	years_drained = 0.0f;
	set_state(DAM_BREACHED);
	emit_signal(SNAME("breached"));
}

void WorldBeaverDam3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_state", "state"), &WorldBeaverDam3D::set_state);
	ClassDB::bind_method(D_METHOD("get_state"), &WorldBeaverDam3D::get_state);
	ClassDB::bind_method(D_METHOD("set_pond_radius", "radius"), &WorldBeaverDam3D::set_pond_radius);
	ClassDB::bind_method(D_METHOD("get_pond_radius"), &WorldBeaverDam3D::get_pond_radius);
	ClassDB::bind_method(D_METHOD("set_years_drained", "years"), &WorldBeaverDam3D::set_years_drained);
	ClassDB::bind_method(D_METHOD("get_years_drained"), &WorldBeaverDam3D::get_years_drained);
	ClassDB::bind_method(D_METHOD("breach"), &WorldBeaverDam3D::breach);
	ClassDB::bind_method(D_METHOD("get_disturbance_id"), &WorldBeaverDam3D::get_disturbance_id);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "state", PROPERTY_HINT_ENUM, "Active,Breached"), "set_state", "get_state");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pond_radius", PROPERTY_HINT_RANGE, "1,500,0.5,or_greater,suffix:m"), "set_pond_radius", "get_pond_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "years_drained", PROPERTY_HINT_RANGE, "0,200,0.1,or_greater"), "set_years_drained", "get_years_drained");
	ADD_SIGNAL(MethodInfo("breached"));
	BIND_ENUM_CONSTANT(DAM_ACTIVE);
	BIND_ENUM_CONSTANT(DAM_BREACHED);
}
