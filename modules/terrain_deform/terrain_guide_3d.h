/**************************************************************************/
/*  terrain_guide_3d.h                                                    */
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

#include "scene/3d/path_3d.h"

// A hand-drawn guide for TerrainGenerator3D, read before erosion (docs/ideas/terrain-generation.md, "Sculpting"): the
// generator still makes the relief, so every guided feature drains and looks eroded (authoring by uplift: Cordonnier
// et al. 2016, Schott et al. 2023; hard rock that erosion leaves standing: Cordonnier et al. 2018).
//
// The curve is the guide's line (one point: a circle round it). Guides under the generator in the tree are read in
// tree order; a raise or lower guide acts like a range or basin path.
// - Raise: the uplift rises to `strength` (0..1, as the uplift map) along the line, a raised cosine over `width`.
//   A ridge, a range, a peak.
// - Lower: the uplift is scaled down to `strength` along the line. A valley, a bowl, a pass.
// - Bar: a band of resistant rock along the line that the rivers and ice wear down only to each curve point's height
//   (interpolated along it): a riegel, a rock step, a lip a lake spills over. Its lowest point is the lip. With
//   `apron_slope` above 0 the side to the right of the line (looking along it) falls away as a rock slope of that
//   rise over run instead of a sheer wall, except within `lip_clear` of the lip.
// - Coast: sea where the line says, joined to the generator's ocean edges and sea map: inside a closed curve (outside
//   with `invert`: an island), or left of an open one (looking along it). Sea cells are the run's outlets, so rivers
//   end on it and the land rises from it; the shoreface and beaches follow as for any coast. `beach_width` 0 or more
//   sets the beach along it (0: cliffs); -1 keeps the generator's own beaches. The shore wanders up to `roughness`
//   either way of the line.
// - Beach: a stretch of coast within `width` of the line gets a beach of `beach_width` (0: a cliff coast), blended
//   in over the band's edge.
class TerrainGuide3D : public Path3D {
	GDCLASS(TerrainGuide3D, Path3D);

public:
	enum Mode {
		MODE_RAISE,
		MODE_LOWER,
		MODE_BAR,
		MODE_COAST,
		MODE_BEACH,
	};

	struct BarPoint {
		Vector2 position;
		real_t radius = 0.0;
		real_t level = 0.0;
		real_t slope = 0.0; // apron: rock falling away past the radius at this rise over run
		Vector2 side; // the apron's side (zero: none)
	};

private:
	Mode mode = MODE_RAISE;
	real_t width = 300.0;
	real_t strength = 1.0;
	real_t bar_width = 40.0;
	real_t apron_slope = 0.0;
	real_t lip_clear = 160.0;
	real_t beach_width = -1.0;
	real_t roughness = 120.0;
	bool invert = false;
	bool enabled = true;

	void _changed();

protected:
	void _notification(int p_what);
	void _validate_property(PropertyInfo &p_property) const;
	static void _bind_methods();

public:
	void set_mode(Mode p_mode);
	Mode get_mode() const { return mode; }
	void set_width(real_t p_width);
	real_t get_width() const { return width; }
	void set_strength(real_t p_strength);
	real_t get_strength() const { return strength; }
	void set_bar_width(real_t p_width);
	real_t get_bar_width() const { return bar_width; }
	void set_apron_slope(real_t p_slope);
	real_t get_apron_slope() const { return apron_slope; }
	void set_lip_clear(real_t p_distance);
	real_t get_lip_clear() const { return lip_clear; }
	void set_beach_width(real_t p_width);
	real_t get_beach_width() const { return beach_width; }
	void set_roughness(real_t p_roughness);
	real_t get_roughness() const { return roughness; }
	void set_invert(bool p_invert);
	bool is_invert() const { return invert; }
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }

	// The line in global space (one point: the centre; a closed curve ends where it starts).
	PackedVector3Array get_global_points() const;
	// Coast: whether a point (global x, z) is sea by this guide.
	bool is_sea_at(const Vector2 &p_point) const;
	// Distance (global x, z) to the line.
	real_t distance_to(const Vector2 &p_point) const;
	// Bar: the resistant rock as discs every 32 m along the line, global x/z, level in metres (apron in slope, side).
	Vector<BarPoint> get_bar_points() const;
	PackedVector3Array get_bar_points_array() const; // x, level, z per disc (for scripts and tests)
};

VARIANT_ENUM_CAST(TerrainGuide3D::Mode);
