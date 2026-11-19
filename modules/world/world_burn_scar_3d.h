/**************************************************************************/
/*  world_burn_scar_3d.h                                                  */
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

#include "world_contracts.h"

#include "scene/3d/node_3d.h"

// An old burn authored into a scene (docs/ideas/world-links.md, fire → regrowth): ground that burnt age_years before
// the scene's present, as a WorldFire producer answering burn_age_at() over its area. The area is the polygon (local
// x, z, metres) when it has three or more points, else a size.x by size.y rectangle centred on the node; both turn
// with the node. Vegetation reads the age for its regrowth stage; fires in play answer for the burns they make.
class WorldBurnScar3D : public Node3D, public WorldFireProvider {
	GDCLASS(WorldBurnScar3D, Node3D);

	float age_years = 5.0f;
	Vector2 size = Vector2(40.0f, 40.0f);
	PackedVector2Array polygon;

	Rect2 reported; // The area last reported to WorldFire.burn_changed().
	void _report();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_age_years(float p_years);
	float get_age_years() const { return age_years; }
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const { return size; }
	void set_polygon(const PackedVector2Array &p_polygon);
	PackedVector2Array get_polygon() const { return polygon; }

	bool covers(const Vector3 &p_position) const;
	Rect2 get_area() const; // x, z in metres, the scar's world bounds

	float contract_fire_heat_at(const Vector3 &p_position) const override { return 0.0f; }
	float contract_fire_burn_age_at(const Vector3 &p_position) const override { return covers(p_position) ? age_years : -1.0f; }
};
