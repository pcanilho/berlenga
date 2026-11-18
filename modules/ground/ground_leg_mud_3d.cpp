/**************************************************************************/
/*  ground_leg_mud_3d.cpp                                                 */
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

#include "ground_leg_mud_3d.h"

#include "ground_surface_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/resources/material.h"

// Fresh mud dries over about five minutes (as the vehicle body's).
static constexpr float LEG_MUD_DRY_TIME = 300.0f;
// A foot is planted when its sole is this close to the surface.
static constexpr float LEG_PLANTED = 0.05f;

void GroundLegMud3D::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	set_physics_process_internal(enabled && is_inside_tree());
	if (!enabled) {
		clear_mud();
	}
}

void GroundLegMud3D::set_skeleton(const NodePath &p_path) {
	skeleton = p_path;
	skeleton_id = ObjectID();
	bone_foot[0] = bone_foot[1] = bone_knee[0] = bone_knee[1] = -1;
}

void GroundLegMud3D::set_foot_bone(Leg p_leg, const String &p_name) {
	ERR_FAIL_INDEX(int(p_leg), 2);
	foot_bones[p_leg] = p_name;
	skeleton_id = ObjectID();
}

void GroundLegMud3D::set_knee_bone(Leg p_leg, const String &p_name) {
	ERR_FAIL_INDEX(int(p_leg), 2);
	knee_bones[p_leg] = p_name;
	skeleton_id = ObjectID();
}

void GroundLegMud3D::clear_mud() {
	line[0] = line[1] = 0.0f;
	flecks[0] = flecks[1] = 0.0f;
	wet = 0.0f;
	for (const ObjectID &id : meshes) {
		if (GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(id)) {
			gi->set_instance_shader_parameter("mud_fill", Vector4());
			gi->set_instance_shader_parameter("mud_body", Vector4());
			gi->set_instance_shader_parameter("mud_wheel0", Vector4());
			gi->set_instance_shader_parameter("mud_wheel1", Vector4());
			gi->set_instance_shader_parameter("mud_wheel2", Vector4());
			gi->set_instance_shader_parameter("mud_wheel3", Vector4());
		}
	}
	written = false;
}

Skeleton3D *GroundLegMud3D::_skeleton() {
	if (skeleton_id.is_valid()) {
		if (Skeleton3D *s = ObjectDB::get_instance<Skeleton3D>(skeleton_id)) {
			return s;
		}
	}
	Skeleton3D *s = nullptr;
	if (!skeleton.is_empty()) {
		s = Object::cast_to<Skeleton3D>(get_node_or_null(skeleton));
	} else if (Node *root = get_parent()) {
		List<Node *> stack;
		stack.push_back(root);
		while (!stack.is_empty() && !s) {
			Node *n = stack.front()->get();
			stack.pop_front();
			s = Object::cast_to<Skeleton3D>(n);
			for (int i = 0; i < n->get_child_count(); i++) {
				stack.push_back(n->get_child(i));
			}
		}
	}
	if (s) {
		skeleton_id = s->get_instance_id();
		_find_bones(s);
	}
	return s;
}

// The side a bone name is on: 0 left, 1 right, -1 neither ("LeftFoot", "foot.L", "foot_l", "L_Foot", "mixamorig:LeftLeg").
static int _bone_side(const String &p_name) {
	const String n = p_name.to_lower();
	if (n.contains("left") || n.ends_with(".l") || n.ends_with("_l") || n.begins_with("l_") || n.contains("_l_") || n.begins_with("l.")) {
		return 0;
	}
	if (n.contains("right") || n.ends_with(".r") || n.ends_with("_r") || n.begins_with("r_") || n.contains("_r_") || n.begins_with("r.")) {
		return 1;
	}
	return -1;
}

void GroundLegMud3D::_find_bones(Skeleton3D *p_skeleton) {
	for (int leg = 0; leg < 2; leg++) {
		bone_foot[leg] = foot_bones[leg].is_empty() ? -1 : p_skeleton->find_bone(foot_bones[leg]);
		bone_knee[leg] = knee_bones[leg].is_empty() ? -1 : p_skeleton->find_bone(knee_bones[leg]);
	}
	for (int b = 0; b < p_skeleton->get_bone_count(); b++) {
		const String name = p_skeleton->get_bone_name(b);
		const int side = _bone_side(name);
		if (side < 0) {
			continue;
		}
		const String n = name.to_lower();
		if (bone_foot[side] < 0 && foot_bones[side].is_empty() && (n.contains("foot") || n.contains("ankle"))) {
			bone_foot[side] = b;
		}
		const bool upper = n.contains("upleg") || n.contains("upper") || n.contains("thigh");
		if (bone_knee[side] < 0 && knee_bones[side].is_empty() && !upper &&
				(n.contains("calf") || n.contains("shin") || n.contains("knee") || n.contains("lowerleg") || n.contains("leg"))) {
			bone_knee[side] = b;
		}
	}
	// The lower leg's bone sits at the knee; with only a foot found, its parent is it.
	for (int leg = 0; leg < 2; leg++) {
		if (bone_knee[leg] < 0 && bone_foot[leg] >= 0) {
			bone_knee[leg] = p_skeleton->get_bone_parent(bone_foot[leg]);
		}
	}
}

void GroundLegMud3D::_find_meshes() {
	meshes.clear();
	Node *root = get_parent();
	if (!root) {
		return;
	}
	List<Node *> stack;
	stack.push_back(root);
	while (!stack.is_empty()) {
		Node *n = stack.front()->get();
		stack.pop_front();
		for (int i = 0; i < n->get_child_count(); i++) {
			stack.push_back(n->get_child(i));
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(n);
		if (!mi || mi->get_mesh().is_null()) {
			continue;
		}
		bool any = false;
		Ref<BaseMaterial3D> over = mi->get_material_override();
		if (over.is_valid()) {
			if (!over->get_feature(BaseMaterial3D::FEATURE_MUD)) {
				over = over->duplicate();
				over->set_feature(BaseMaterial3D::FEATURE_MUD, true);
				mi->set_material_override(over);
			}
			any = true;
		} else if (mi->get_material_override().is_null()) {
			for (int s = 0; s < mi->get_surface_override_material_count(); s++) {
				Ref<BaseMaterial3D> m = mi->get_active_material(s);
				if (m.is_null()) {
					continue;
				}
				if (!m->get_feature(BaseMaterial3D::FEATURE_MUD)) {
					m = m->duplicate();
					m->set_feature(BaseMaterial3D::FEATURE_MUD, true);
					mi->set_surface_override_material(s, m);
				}
				any = true;
			}
		}
		if (any) {
			meshes.push_back(mi->get_instance_id());
		}
	}
	meshes_ready = true;
}

bool GroundLegMud3D::_leg_points(int p_leg, Vector3 &r_sole, Vector3 &r_knee) {
	Node3D *sole = sole_nodes[p_leg].is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(sole_nodes[p_leg]));
	Node3D *knee = knee_nodes[p_leg].is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(knee_nodes[p_leg]));
	if (sole && knee) {
		r_sole = sole->get_global_position();
		r_knee = knee->get_global_position();
		return true;
	}
	Skeleton3D *s = _skeleton();
	if (!s || bone_foot[p_leg] < 0 || bone_knee[p_leg] < 0) {
		return false;
	}
	const Transform3D g = s->get_global_transform();
	const Vector3 up = g.basis.get_column(1).normalized();
	r_sole = g.xform(s->get_bone_global_pose(bone_foot[p_leg]).origin) - up * ankle_height;
	r_knee = g.xform(s->get_bone_global_pose(bone_knee[p_leg]).origin);
	return true;
}

void GroundLegMud3D::_step(double p_delta) {
	if (!meshes_ready) {
		_find_meshes();
	}
	Vector3 soles[2];
	Vector3 knees[2];
	if (!_leg_points(LEG_LEFT, soles[0], knees[0]) || !_leg_points(LEG_RIGHT, soles[1], knees[1])) {
		return;
	}
	const float dt = float(p_delta);
	Node3D *body = Object::cast_to<Node3D>(get_parent());
	const Vector3 here = body ? body->get_global_position() : get_global_position();
	float speed = 0.0f;
	if (have_last && dt > 0.0f) {
		const Vector3 d = here - last_position;
		speed = Vector2(d.x, d.z).length() / dt;
	}
	last_position = here;
	have_last = true;

	bool fresh = false;
	GroundSurface3D *ground = GroundSurface3D::get_active();
	if (ground) {
		for (int leg = 0; leg < 2; leg++) {
			const Vector3 &p = soles[leg];
			const float surface = ground->get_surface_height_at(p);
			const float water = ground->get_standing_water_at(p);
			const bool planted = p.y - surface < LEG_PLANTED;
			// Under more than a few cm of standing water the boot comes out rinsed, not coated.
			in_water[leg] = water > 0.03f ? 0.0f : in_water[leg] + dt;
			if (planted && in_water[leg] > 0.5f) {
				const GroundSoil soil = ground->get_soil(p);
				const float mud = ground->get_mud_depth_at(p);
				if (soil.soft && (soil.stickiness >= 0.2f || mud > 0.0f)) {
					// The boot sinks to the bottom of the slurry, or as far as a person's weight on a boot sole
					// presses the soil (Bekker), and comes out coated a little above it.
					const float sink = MAX(MAX(mud, ground->get_sinkage_at(p, 40.0f, 0.03f, 0.1f)), surface - p.y);
					const float reach = MIN(sink + 0.02f, 0.6f);
					if (reach > line[leg]) {
						line[leg] = reach;
						fresh = true;
					}
					// Each stride flicks clay up the back of the other shin: more as the walk quickens to a run.
					const float f = MIN(speed / 4.0f, 1.0f) * MAX(soil.stickiness, mud > 0.0f ? 0.5f : 0.0f);
					if (f > flecks[leg]) {
						flecks[leg] = MIN(flecks[leg] + (f - flecks[leg]) * MIN(2.0f * dt, 1.0f), 1.0f);
						fresh = true;
					}
					const Color clay = ground->get_mud_color_at(p).srgb_to_linear();
					mud_rgb = mud_rgb.lerp(Vector3(clay.r, clay.g, clay.b), MIN(speed * dt / 3.0f, 1.0f));
				}
			}
			if (water > 0.01f && planted && speed > 0.1f) {
				// A boot walking through a puddle stirs the clay of its bed into the water (muddy water, round 3):
				// about a tenth of a kilo a metre walked.
				ground->stir_water(p, 0.12f, 0.1f * speed * dt);
			}
			if (water > 0.01f) {
				// Standing water washes the leg below its surface, faster as the leg moves through it (as the body's).
				const float rate = MIN(water / 0.05f, 1.0f) * 0.3f * (1.0f + 0.5f * speed);
				const float level = surface + water - p.y;
				if (level >= line[leg]) {
					line[leg] -= line[leg] * MIN(rate * dt, 1.0f);
				}
				flecks[leg] -= flecks[leg] * MIN(rate * dt * (level > 0.1f ? 1.0f : 0.3f), 1.0f);
				wet = 1.0f;
			}
		}
	}
	wet = fresh ? 1.0f : MAX(wet - dt / LEG_MUD_DRY_TIME, 0.0f);

	const float total = line[0] + line[1] + flecks[0] + flecks[1];
	if (total <= 1e-4f && !written) {
		return;
	}
	written = total > 1e-4f;
	Vector3 facing = body ? body->get_global_basis().get_column(2) : get_global_basis().get_column(2);
	facing = facing.length_squared() > 1e-8f ? facing.normalized() : Vector3(0, 0, 1);
	static const StringName names[12] = { "mud_frame0", "mud_frame1", "mud_frame2", "mud_wheel0", "mud_wheel1", "mud_wheel2",
		"mud_wheel3", "mud_fill", "mud_spray", "mud_body", "mud_color", "mud_splat" };
	const Vector4 values[12] = {
		Vector4(facing.x, facing.y, facing.z, 0.0f), Vector4(), Vector4(),
		Vector4(soles[0].x, soles[0].y, soles[0].z, line[0]), Vector4(knees[0].x, knees[0].y, knees[0].z, flecks[0]),
		Vector4(soles[1].x, soles[1].y, soles[1].z, line[1]), Vector4(knees[1].x, knees[1].y, knees[1].z, flecks[1]),
		Vector4(shin_radius, 0.0f, 0.0f, 0.0f), Vector4(), Vector4(0.0f, 0.0f, wet, 1.0f),
		Vector4(mud_rgb.x, mud_rgb.y, mud_rgb.z, 1.0f), Vector4()
	};
	for (const ObjectID &id : meshes) {
		GeometryInstance3D *gi = ObjectDB::get_instance<GeometryInstance3D>(id);
		if (!gi) {
			continue;
		}
		for (int k = 0; k < 12; k++) {
			gi->set_instance_shader_parameter(names[k], values[k]);
		}
	}
}

void GroundLegMud3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			meshes_ready = false;
			have_last = false;
			skeleton_id = ObjectID();
			set_physics_process_internal(enabled);
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_step(get_physics_process_delta_time());
		} break;
	}
}

void GroundLegMud3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &GroundLegMud3D::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &GroundLegMud3D::is_enabled);
	ClassDB::bind_method(D_METHOD("set_skeleton", "path"), &GroundLegMud3D::set_skeleton);
	ClassDB::bind_method(D_METHOD("get_skeleton"), &GroundLegMud3D::get_skeleton);
	ClassDB::bind_method(D_METHOD("set_foot_bone", "leg", "name"), &GroundLegMud3D::set_foot_bone);
	ClassDB::bind_method(D_METHOD("get_foot_bone", "leg"), &GroundLegMud3D::get_foot_bone);
	ClassDB::bind_method(D_METHOD("set_knee_bone", "leg", "name"), &GroundLegMud3D::set_knee_bone);
	ClassDB::bind_method(D_METHOD("get_knee_bone", "leg"), &GroundLegMud3D::get_knee_bone);
	ClassDB::bind_method(D_METHOD("set_sole_node", "leg", "path"), &GroundLegMud3D::set_sole_node);
	ClassDB::bind_method(D_METHOD("get_sole_node", "leg"), &GroundLegMud3D::get_sole_node);
	ClassDB::bind_method(D_METHOD("set_knee_node", "leg", "path"), &GroundLegMud3D::set_knee_node);
	ClassDB::bind_method(D_METHOD("get_knee_node", "leg"), &GroundLegMud3D::get_knee_node);
	ClassDB::bind_method(D_METHOD("set_ankle_height", "height"), &GroundLegMud3D::set_ankle_height);
	ClassDB::bind_method(D_METHOD("get_ankle_height"), &GroundLegMud3D::get_ankle_height);
	ClassDB::bind_method(D_METHOD("set_shin_radius", "radius"), &GroundLegMud3D::set_shin_radius);
	ClassDB::bind_method(D_METHOD("get_shin_radius"), &GroundLegMud3D::get_shin_radius);
	ClassDB::bind_method(D_METHOD("get_mud_line", "leg"), &GroundLegMud3D::get_mud_line);
	ClassDB::bind_method(D_METHOD("get_mud_flecks", "leg"), &GroundLegMud3D::get_mud_flecks);
	ClassDB::bind_method(D_METHOD("get_mud_wetness"), &GroundLegMud3D::get_mud_wetness);
	ClassDB::bind_method(D_METHOD("clear_mud"), &GroundLegMud3D::clear_mud);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "skeleton", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D"), "set_skeleton", "get_skeleton");
	ADD_GROUP("Bones", "");
	ADD_PROPERTYI(PropertyInfo(Variant::STRING, "left_foot_bone"), "set_foot_bone", "get_foot_bone", LEG_LEFT);
	ADD_PROPERTYI(PropertyInfo(Variant::STRING, "right_foot_bone"), "set_foot_bone", "get_foot_bone", LEG_RIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::STRING, "left_knee_bone"), "set_knee_bone", "get_knee_bone", LEG_LEFT);
	ADD_PROPERTYI(PropertyInfo(Variant::STRING, "right_knee_bone"), "set_knee_bone", "get_knee_bone", LEG_RIGHT);
	ADD_GROUP("Nodes", "");
	ADD_PROPERTYI(PropertyInfo(Variant::NODE_PATH, "left_sole_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_sole_node", "get_sole_node", LEG_LEFT);
	ADD_PROPERTYI(PropertyInfo(Variant::NODE_PATH, "right_sole_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_sole_node", "get_sole_node", LEG_RIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::NODE_PATH, "left_knee_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_knee_node", "get_knee_node", LEG_LEFT);
	ADD_PROPERTYI(PropertyInfo(Variant::NODE_PATH, "right_knee_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_knee_node", "get_knee_node", LEG_RIGHT);
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ankle_height", PROPERTY_HINT_RANGE, "0,0.3,0.005,suffix:m"), "set_ankle_height", "get_ankle_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shin_radius", PROPERTY_HINT_RANGE, "0.01,0.2,0.005,suffix:m"), "set_shin_radius", "get_shin_radius");

	BIND_ENUM_CONSTANT(LEG_LEFT);
	BIND_ENUM_CONSTANT(LEG_RIGHT);
}
