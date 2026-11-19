/**************************************************************************/
/*  desert_springs.h                                                      */
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

#include "modules/terrain_deform/terrain_generator_3d.h"

#include "core/math/rect2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

// Oases at desert springs (docs/ideas/vegetation.md, "Oases at desert springs"): the terrain's springs
// (TerrainGenerator3D.get_springs()) and the wetness round them: the ground is wet as far as it lies within a palm's
// root reach of a spring's water level. VegetationField3D (PlantSpecies.RULE_SPRING) and GrassField3D read the same.
struct DesertSprings {
	static constexpr float ROOT_REACH = 5.0f; // m above the water where the wetness reaches 0.
	static constexpr float REACH = 250.0f; // m from a spring where it reaches 0.

	LocalVector<float> wet; // 0 dry to 1 wet; -1 off the sand sea.
	LocalVector<Vector3> springs; // x, water level, z.
	LocalVector<float> discharge; // L/s, one per spring; 0: the pools' default.
	Rect2 rect;
	int nx = 0;
	int ny = 0;

	void build(const TerrainGenerator3D *p_gen);
	void clear();
	bool is_empty() const { return wet.is_empty(); }
	// Wetness at a position (x, z), bilinear; -1 off the sand sea (or without one).
	float at(const Vector2 &p_xz) const;
};
