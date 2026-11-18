/**************************************************************************/
/*  contact_trample.h                                                     */
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

#include "modules/world/world_contracts.h"
#include "water_current_bend.h"
#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/3d/box_shape_3d.h"
#include "scene/resources/3d/capsule_shape_3d.h"
#include "scene/resources/3d/cylinder_shape_3d.h"
#include "scene/resources/3d/sphere_shape_3d.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"

// The ground cover's trample field where no GrassField3D keeps one (docs/ideas/world-links.md, contact 2): the
// same field GrassField3D publishes as grass_trample_map / grass_trample_rect, which every PlantGenerator plant
// (herbs, ferns, flowers, heath) reads to lay down under bodies, so plants on scree, dunes or a heath with no
// grass bend too. Bodies from grass_trample and world_contact, pressed flat under them and laid away from them and
// forward along their motion (Jahrmann and Wimmer 2017), recovering with a half-life; the grass's maths.
class ContactTrample {
	int res = 128;
	float size = 32.0f; // m
	float recovery = 6.0f; // s, half-life
	LocalVector<Vector2> bend;
	Vector2i origin;
	bool active = false;
	bool published = false;
	HashMap<ObjectID, Vector3> last;
	WaterCurrentBend current;
	Ref<Image> image;
	Ref<ImageTexture> texture;

	static Vector2 _extent(const Node3D *p_node) { return WorldContracts::body_extent(p_node, 0.4f); }

	void _recenter(const Vector3 &p_center) {
		const float texel = size / res;
		const Vector2i o(int(Math::floor(p_center.x / texel)) - res / 2, int(Math::floor(p_center.z / texel)) - res / 2);
		if ((int)bend.size() != res * res) {
			bend.resize(res * res);
			for (Vector2 &b : bend) {
				b = Vector2();
			}
			origin = o;
			return;
		}
		if (o == origin) {
			return;
		}
		LocalVector<Vector2> moved;
		moved.resize(res * res);
		for (int z = 0; z < res; z++) {
			for (int x = 0; x < res; x++) {
				const int sx = x + o.x - origin.x, sz = z + o.y - origin.y;
				moved[z * res + x] = (sx >= 0 && sz >= 0 && sx < res && sz < res) ? bend[sz * res + sx] : Vector2();
			}
		}
		bend = moved;
		origin = o;
	}

	void _press(const Vector3 &p_position, float p_radius, const Vector3 &p_velocity) {
		const float texel = size / res;
		const float reach = p_radius * 1.25f;
		const Vector2 c(p_position.x, p_position.z);
		const Vector2 v(p_velocity.x, p_velocity.z);
		const float speed = v.length();
		const Vector2 vd = speed > 1e-3f ? v / speed : Vector2();
		const int x0 = MAX(int(Math::floor((c.x - reach) / texel)) - origin.x, 0);
		const int x1 = MIN(int(Math::floor((c.x + reach) / texel)) - origin.x, res - 1);
		const int z0 = MAX(int(Math::floor((c.y - reach) / texel)) - origin.y, 0);
		const int z1 = MIN(int(Math::floor((c.y + reach) / texel)) - origin.y, res - 1);
		for (int z = z0; z <= z1; z++) {
			for (int x = x0; x <= x1; x++) {
				const Vector2 q((origin.x + x + 0.5f) * texel, (origin.y + z + 0.5f) * texel);
				const Vector2 rel = q - c;
				const float d = rel.length();
				if (d >= reach) {
					continue;
				}
				const float f = 1.0f - Math::smoothstep(p_radius * 0.6f, reach, d);
				Vector2 dir = (d > 1e-3f ? rel / d : Vector2()) * 0.6f + vd * MIN(speed, 1.0f);
				if (dir.length() < 1e-3f) {
					dir = Vector2(1.0f, 0.0f);
				}
				const Vector2 target = dir.normalized() * f;
				Vector2 &b = bend[z * res + x];
				if (target.length_squared() > b.length_squared()) {
					b = target;
					active = true;
				}
			}
		}
	}

	void _upload() {
		RenderingServer *rs = RenderingServer::get_singleton();
		Vector<uint8_t> data;
		data.resize(res * res * 4);
		uint16_t *w = (uint16_t *)data.ptrw();
		for (int i = 0; i < res * res; i++) {
			w[i * 2 + 0] = Math::make_half_float(bend[i].x);
			w[i * 2 + 1] = Math::make_half_float(bend[i].y);
		}
		if (image.is_valid() && image->get_width() == res) {
			image->set_data(res, res, false, Image::FORMAT_RGH, data);
		} else {
			image = Image::create_from_data(res, res, false, Image::FORMAT_RGH, data);
		}
		if (texture.is_valid() && texture->get_width() == res) {
			texture->update(image);
		} else {
			texture = ImageTexture::create_from_image(image);
		}
		const float texel = size / res;
		rs->global_shader_parameter_set("grass_trample_map", texture->get_rid());
		rs->global_shader_parameter_set("grass_trample_rect", Vector4(origin.x * texel, origin.y * texel, size, size));
		published = true;
	}

public:
	// One step round p_center; p_ground(pos) gives the ground height under a body, p_top the plants' height.
	template <typename G>
	void step(SceneTree *p_tree, const Vector3 &p_center, double p_delta, float p_top, const G &p_ground) {
		if (!p_tree || p_delta <= 0.0) {
			return;
		}
		_recenter(p_center);
		if (active) {
			const float keep = recovery > 0.0f ? Math::pow(0.5f, float(p_delta) / recovery) : 0.0f;
			float largest = 0.0f;
			for (Vector2 &b : bend) {
				b *= keep;
				largest = MAX(largest, b.length_squared());
			}
			if (largest < 1e-6f) {
				for (Vector2 &b : bend) {
					b = Vector2();
				}
				active = false;
				_upload();
			}
		}
		Vector<Node *> nodes = p_tree->get_nodes_in_group(SNAME("grass_trample"));
		nodes.append_array(p_tree->get_nodes_in_group(SNAME("world_contact")));
		HashMap<ObjectID, Vector3> seen;
		for (Node *n : nodes) {
			const Node3D *n3 = Object::cast_to<Node3D>(n);
			if (!n3 || !n3->is_visible_in_tree() || seen.has(n3->get_instance_id())) {
				continue;
			}
			const Vector3 pos = n3->get_global_position();
			const Vector3 *prev = last.getptr(n3->get_instance_id());
			Vector3 vel = prev ? (pos - *prev) / float(p_delta) : Vector3();
			if (vel.length_squared() > 30.0f * 30.0f) {
				vel = Vector3();
			}
			seen.insert(n3->get_instance_id(), pos);
			const Vector2 ext = _extent(n3);
			if (pos.y - ext.y - p_ground(pos) > p_top) {
				continue; // above the plants
			}
			_press(pos, ext.x, vel);
		}
		last = seen;
		// The current in any water over the ground lays the plants downstream (WaterCurrentBend).
		current.update(origin, res, size / res, p_delta, p_ground);
		active = current.apply(bend, res) || active;
		if (active) {
			_upload();
		}
	}

	Vector2 get_at(const Vector3 &p_position) const {
		if ((int)bend.size() != res * res) {
			return Vector2();
		}
		const float texel = size / res;
		const int x = int(Math::floor(p_position.x / texel)) - origin.x;
		const int z = int(Math::floor(p_position.z / texel)) - origin.y;
		return (x >= 0 && z >= 0 && x < res && z < res) ? bend[z * res + x] : Vector2();
	}

	// Gives the globals back (another field took over, or this one leaves).
	void release() {
		if (published) {
			RenderingServer *rs = RenderingServer::get_singleton();
			if (rs) {
				rs->global_shader_parameter_set("grass_trample_rect", Vector4());
			}
			published = false;
		}
		bend.clear();
		last.clear();
		active = false;
	}
	bool is_published() const { return published; }
};
