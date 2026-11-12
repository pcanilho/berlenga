/**************************************************************************/
/*  hair_groom.h                                                          */
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

#include "core/io/image.h"
#include "core/io/resource.h"
#include "scene/resources/3d/primitive_meshes.h"

// Hair as strands: polylines of points, one per fibre (or per rendered clump of fibres), grown from a
// scalp. Drawn by HairMesh; simulated later from guide strands. Built by grow_on_mesh() or read from
// Cem Yuksel's HAIR file format (load_hair_file()), which a few lines of Blender Python can write.
class HairGroom : public Resource {
	GDCLASS(HairGroom, Resource);
	RES_BASE_EXTENSION("tres");

	PackedVector3Array points; // Every strand's points, root first, strand after strand.
	PackedInt32Array strand_point_counts; // Points per strand.
	// Where each strand grows, when known: the scalp surface's triangle and barycentric coordinates.
	PackedInt32Array root_triangles;
	PackedVector3Array root_barycentrics;

protected:
	static void _bind_methods();

public:
	void set_points(const PackedVector3Array &p_points);
	PackedVector3Array get_points() const;
	void set_strand_point_counts(const PackedInt32Array &p_counts);
	PackedInt32Array get_strand_point_counts() const;
	void set_root_triangles(const PackedInt32Array &p_triangles);
	PackedInt32Array get_root_triangles() const;
	void set_root_barycentrics(const PackedVector3Array &p_barycentrics);
	PackedVector3Array get_root_barycentrics() const;

	int get_strand_count() const;
	int get_point_count() const;
	bool is_valid() const; // The counts add up to the points.

	// Grows p_count strands of p_length metres from a mesh surface, roots spread by area where p_mask
	// (optional, sampled at UV) is above one half. Strands leave the surface along its normal combed
	// away from p_crown, fall under p_droop (0 stiff, 1 limp), curl in helices of p_curl_radius metres
	// with p_curls_per_metre turns, and wander by p_frizz. They are kept outside the ellipsoid that
	// fits the surface's bounds, a stand-in for the head. Returns false if the surface has no triangles
	// (or none under the mask). p_comb: 0 leaves the surface straight out, 1 lies flat along it.
	bool grow_on_mesh(const Ref<Mesh> &p_mesh, int p_surface, int p_count, float p_length, int p_points_per_strand,
			const Ref<Image> &p_mask, const Vector3 &p_crown, float p_comb, float p_droop, float p_curl_radius,
			float p_curls_per_metre, float p_frizz, int p_seed);

	// Cem Yuksel's HAIR format (cemyuksel.com/research/hairmodels): a 128-byte header, then segment
	// counts, points and optional per-point thickness, transparency and colour (the last three ignored).
	// Points are scaled by p_scale and turned from the format's Z-up to Y-up when p_z_up.
	Error load_hair_file(const String &p_path, float p_scale = 1.0f, bool p_z_up = true);
};

// Draws a HairGroom's strands as two crossed strips per segment (the DOTS layout NVIDIA uses for ray
// traced hair), so they have width from every side. UV.x runs across a strip (its tangent), UV.y
// from root (0) to tip (1) along the strand (its bitangent). With a BaseMaterial3D whose Hair
// direction is Strands, the path tracer turns UV.x into the true offset across the fibre.
class HairMesh : public PrimitiveMesh {
	GDCLASS(HairMesh, PrimitiveMesh);

	Ref<HairGroom> groom;
	float root_width = 0.0004f;
	float tip_width = 0.0001f;
	bool crossed = true;

	void _groom_changed();

protected:
	static void _bind_methods();
	virtual void _create_mesh_array(Array &p_arr) const override;

public:
	void set_groom(const Ref<HairGroom> &p_groom);
	Ref<HairGroom> get_groom() const;
	void set_root_width(float p_width);
	float get_root_width() const;
	void set_tip_width(float p_width);
	float get_tip_width() const;
	void set_crossed(bool p_crossed);
	bool is_crossed() const;

	HairMesh();
};
