/**************************************************************************/
/*  hair_groom.cpp                                                        */
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

#include "hair_groom.h"

#include "core/io/file_access.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

void HairGroom::set_points(const PackedVector3Array &p_points) {
	points = p_points;
	emit_changed();
}

PackedVector3Array HairGroom::get_points() const {
	return points;
}

void HairGroom::set_strand_point_counts(const PackedInt32Array &p_counts) {
	strand_point_counts = p_counts;
	emit_changed();
}

PackedInt32Array HairGroom::get_strand_point_counts() const {
	return strand_point_counts;
}

void HairGroom::set_root_triangles(const PackedInt32Array &p_triangles) {
	root_triangles = p_triangles;
}

PackedInt32Array HairGroom::get_root_triangles() const {
	return root_triangles;
}

void HairGroom::set_root_barycentrics(const PackedVector3Array &p_barycentrics) {
	root_barycentrics = p_barycentrics;
}

PackedVector3Array HairGroom::get_root_barycentrics() const {
	return root_barycentrics;
}

int HairGroom::get_strand_count() const {
	return strand_point_counts.size();
}

int HairGroom::get_point_count() const {
	return points.size();
}

bool HairGroom::is_valid() const {
	int64_t total = 0;
	for (int count : strand_point_counts) {
		if (count < 0) {
			return false;
		}
		total += count;
	}
	return total == points.size();
}

bool HairGroom::grow_on_mesh(const Ref<Mesh> &p_mesh, int p_surface, int p_count, float p_length, int p_points_per_strand,
		const Ref<Image> &p_mask, const Vector3 &p_crown, float p_comb, float p_droop, float p_curl_radius,
		float p_curls_per_metre, float p_frizz, int p_seed) {
	ERR_FAIL_COND_V(p_mesh.is_null(), false);
	ERR_FAIL_INDEX_V(p_surface, p_mesh->get_surface_count(), false);
	ERR_FAIL_COND_V(p_mesh->surface_get_primitive_type(p_surface) != Mesh::PRIMITIVE_TRIANGLES, false);
	ERR_FAIL_COND_V(p_count <= 0 || p_length <= 0.0f || p_points_per_strand < 2, false);

	const Array arrays = p_mesh->surface_get_arrays(p_surface);
	const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
	const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
	PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	if (indices.is_empty()) {
		indices.resize(verts.size() - verts.size() % 3);
		for (int i = 0; i < indices.size(); i++) {
			indices.write[i] = i;
		}
	}
	const int tri_count = indices.size() / 3;
	ERR_FAIL_COND_V(tri_count == 0, false);

	Ref<Image> mask;
	if (p_mask.is_valid() && !p_mask->is_empty() && uvs.size() == verts.size()) {
		mask = p_mask->duplicate();
		if (mask->is_compressed()) {
			mask->decompress();
		}
	}
	auto mask_at = [&](const Vector2 &p_uv) -> float {
		if (mask.is_null()) {
			return 1.0f;
		}
		const int x = CLAMP(int(Math::fposmod(p_uv.x, 1.0f) * mask->get_width()), 0, mask->get_width() - 1);
		const int y = CLAMP(int(Math::fposmod(p_uv.y, 1.0f) * mask->get_height()), 0, mask->get_height() - 1);
		return mask->get_pixel(x, y).get_v();
	};

	// Roots spread by area over the triangles under the mask.
	LocalVector<float> cdf;
	cdf.resize(tri_count);
	float total_area = 0.0f;
	AABB bounds(verts[0], Vector3());
	for (int t = 0; t < tri_count; t++) {
		const int a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
		ERR_FAIL_INDEX_V(a, verts.size(), false);
		ERR_FAIL_INDEX_V(b, verts.size(), false);
		ERR_FAIL_INDEX_V(c, verts.size(), false);
		bounds.expand_to(verts[a]);
		bounds.expand_to(verts[b]);
		bounds.expand_to(verts[c]);
		float area = 0.5f * (verts[b] - verts[a]).cross(verts[c] - verts[a]).length();
		if (mask.is_valid() && mask_at((uvs[a] + uvs[b] + uvs[c]) / 3.0f) < 0.5f) {
			area = 0.0f;
		}
		total_area += area;
		cdf[t] = total_area;
	}
	ERR_FAIL_COND_V_MSG(total_area <= 0.0f, false, "No surface under the hair mask.");

	// The head stand-in: the ellipsoid in the surface's bounds, which strands may not enter.
	const Vector3 centre = bounds.get_center();
	const Vector3 radii = (bounds.size * 0.5f).max(Vector3(1e-3f, 1e-3f, 1e-3f));
	const float margin = 1.002f;

	RandomPCG rng{ uint64_t(p_seed) };
	const int pps = p_points_per_strand;
	const float seg_len = p_length / float(pps - 1);
	const Vector3 down(0.0f, -1.0f, 0.0f);

	points.resize(p_count * pps);
	strand_point_counts.resize(p_count);
	root_triangles.resize(p_count);
	root_barycentrics.resize(p_count);
	Vector3 *w = points.ptrw();

	for (int s = 0; s < p_count; s++) {
		const float pick = rng.randf() * total_area;
		int lo = 0, hi = tri_count - 1;
		while (lo < hi) {
			const int mid = (lo + hi) / 2;
			if (cdf[mid] < pick) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		const int t = lo;
		const int a = indices[t * 3], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
		const float r1 = Math::sqrt(rng.randf());
		const float r2 = rng.randf();
		const Vector3 bary(1.0f - r1, r1 * (1.0f - r2), r1 * r2);
		const Vector3 root = verts[a] * bary.x + verts[b] * bary.y + verts[c] * bary.z;
		Vector3 face_n = (verts[b] - verts[a]).cross(verts[c] - verts[a]);
		Vector3 n = normals.size() == verts.size() ? normals[a] * bary.x + normals[b] * bary.y + normals[c] * bary.z : face_n;
		n = n.length_squared() > 1e-12f ? n.normalized() : Vector3(0.0f, 1.0f, 0.0f);

		// Leave along the normal, combed away from the crown.
		Vector3 comb = root - p_crown;
		comb -= n * comb.dot(n);
		Vector3 d = n;
		if (comb.length_squared() > 1e-10f) {
			d = (n * (1.0f - CLAMP(p_comb, 0.0f, 1.0f)) + comb.normalized() * CLAMP(p_comb, 0.0f, 1.0f)).normalized();
		}

		// A frame along the strand for the curls, carried by parallel transport.
		Vector3 u = d.cross(Math::abs(d.y) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f)).normalized();
		const float phase = rng.randf() * Math::TAU;

		Vector3 path = root;
		w[s * pps] = root;
		for (int i = 1; i < pps; i++) {
			const float along = float(i) / float(pps - 1);
			const Vector3 prev_d = d;
			d = (d + down * (p_droop * seg_len * 20.0f)).normalized();
			if (p_frizz > 0.0f) {
				const Vector3 jitter(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f);
				d = (d + jitter * (p_frizz * 0.3f)).normalized();
			}
			Vector3 next = path + d * seg_len;
			// Outside the head: slide along the ellipsoid instead of entering it.
			const Vector3 q = (next - centre) / radii;
			if (q.length() < margin) {
				next = centre + radii * (q.normalized() * margin);
				const Vector3 out = (q / radii).normalized();
				const float into = d.dot(out);
				if (into < 0.0f) {
					d = (d - out * into).normalized();
				}
			}
			d = (next - path).normalized();
			path = next;

			// Parallel transport of the curl frame from the previous direction to this one.
			const Vector3 axis = prev_d.cross(d);
			if (axis.length_squared() > 1e-12f) {
				u = u.rotated(axis.normalized(), prev_d.angle_to(d));
			}
			u = (u - d * u.dot(d)).normalized();
			const Vector3 v = d.cross(u);

			Vector3 p = path;
			if (p_curl_radius > 0.0f && p_curls_per_metre > 0.0f) {
				const float angle = phase + Math::TAU * p_curls_per_metre * float(i) * seg_len;
				const float ramp = CLAMP(along / 0.15f, 0.0f, 1.0f); // Curls start a little off the scalp.
				p += (u * Math::cos(angle) + v * Math::sin(angle)) * (p_curl_radius * ramp * ramp * (3.0f - 2.0f * ramp));
			}
			w[s * pps + i] = p;
		}
		strand_point_counts.write[s] = pps;
		root_triangles.write[s] = t;
		root_barycentrics.write[s] = bary;
	}
	emit_changed();
	return true;
}

Error HairGroom::load_hair_file(const String &p_path, float p_scale, bool p_z_up) {
	Error err;
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ, &err);
	ERR_FAIL_COND_V_MSG(f.is_null(), err, vformat("Cannot open HAIR file '%s'.", p_path));
	uint8_t sig[4];
	f->get_buffer(sig, 4);
	ERR_FAIL_COND_V_MSG(sig[0] != 'H' || sig[1] != 'A' || sig[2] != 'I' || sig[3] != 'R', ERR_FILE_UNRECOGNIZED, "Not a HAIR file.");
	const uint32_t hair_count = f->get_32();
	const uint32_t point_count = f->get_32();
	const uint32_t flags = f->get_32();
	const uint32_t default_segments = f->get_32();
	f->seek(128); // Default thickness, transparency, colour and the info text: unused.
	ERR_FAIL_COND_V_MSG((flags & 2u) == 0u, ERR_FILE_CORRUPT, "HAIR file without points.");
	ERR_FAIL_COND_V(hair_count == 0 || point_count == 0 || point_count > (1u << 28), ERR_FILE_CORRUPT);

	PackedInt32Array counts;
	counts.resize(hair_count);
	int64_t total = 0;
	for (uint32_t i = 0; i < hair_count; i++) {
		const uint32_t segments = (flags & 1u) ? f->get_16() : default_segments;
		counts.write[i] = int(segments) + 1;
		total += segments + 1;
	}
	ERR_FAIL_COND_V_MSG(total != int64_t(point_count), ERR_FILE_CORRUPT, "HAIR file segment counts do not match its points.");

	PackedVector3Array pts;
	pts.resize(point_count);
	Vector3 *w = pts.ptrw();
	for (uint32_t i = 0; i < point_count; i++) {
		const float x = f->get_float();
		const float y = f->get_float();
		const float z = f->get_float();
		w[i] = (p_z_up ? Vector3(x, z, -y) : Vector3(x, y, z)) * p_scale;
	}
	ERR_FAIL_COND_V(f->eof_reached(), ERR_FILE_CORRUPT);

	points = pts;
	strand_point_counts = counts;
	root_triangles.clear();
	root_barycentrics.clear();
	emit_changed();
	return OK;
}

void HairGroom::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_points", "points"), &HairGroom::set_points);
	ClassDB::bind_method(D_METHOD("get_points"), &HairGroom::get_points);
	ClassDB::bind_method(D_METHOD("set_strand_point_counts", "counts"), &HairGroom::set_strand_point_counts);
	ClassDB::bind_method(D_METHOD("get_strand_point_counts"), &HairGroom::get_strand_point_counts);
	ClassDB::bind_method(D_METHOD("set_root_triangles", "triangles"), &HairGroom::set_root_triangles);
	ClassDB::bind_method(D_METHOD("get_root_triangles"), &HairGroom::get_root_triangles);
	ClassDB::bind_method(D_METHOD("set_root_barycentrics", "barycentrics"), &HairGroom::set_root_barycentrics);
	ClassDB::bind_method(D_METHOD("get_root_barycentrics"), &HairGroom::get_root_barycentrics);
	ClassDB::bind_method(D_METHOD("get_strand_count"), &HairGroom::get_strand_count);
	ClassDB::bind_method(D_METHOD("get_point_count"), &HairGroom::get_point_count);
	ClassDB::bind_method(D_METHOD("is_valid"), &HairGroom::is_valid);
	ClassDB::bind_method(D_METHOD("grow_on_mesh", "mesh", "surface", "count", "length", "points_per_strand", "mask", "crown", "comb", "droop", "curl_radius", "curls_per_metre", "frizz", "seed"), &HairGroom::grow_on_mesh);
	ClassDB::bind_method(D_METHOD("load_hair_file", "path", "scale", "z_up"), &HairGroom::load_hair_file, DEFVAL(1.0f), DEFVAL(true));

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "points"), "set_points", "get_points");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "strand_point_counts"), "set_strand_point_counts", "get_strand_point_counts");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "root_triangles"), "set_root_triangles", "get_root_triangles");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "root_barycentrics"), "set_root_barycentrics", "get_root_barycentrics");
}

// HairMesh

void HairMesh::_groom_changed() {
	request_update();
}

void HairMesh::set_groom(const Ref<HairGroom> &p_groom) {
	if (groom == p_groom) {
		return;
	}
	if (groom.is_valid()) {
		groom->disconnect_changed(callable_mp(this, &HairMesh::_groom_changed));
	}
	groom = p_groom;
	if (groom.is_valid()) {
		groom->connect_changed(callable_mp(this, &HairMesh::_groom_changed));
	}
	request_update();
}

Ref<HairGroom> HairMesh::get_groom() const {
	return groom;
}

void HairMesh::set_root_width(float p_width) {
	root_width = MAX(p_width, 0.0f);
	request_update();
}

float HairMesh::get_root_width() const {
	return root_width;
}

void HairMesh::set_tip_width(float p_width) {
	tip_width = MAX(p_width, 0.0f);
	request_update();
}

float HairMesh::get_tip_width() const {
	return tip_width;
}

void HairMesh::set_crossed(bool p_crossed) {
	crossed = p_crossed;
	request_update();
}

bool HairMesh::is_crossed() const {
	return crossed;
}

void HairMesh::_create_mesh_array(Array &p_arr) const {
	PackedVector3Array src;
	PackedInt32Array counts;
	if (groom.is_valid() && groom->is_valid() && groom->get_point_count() > 0) {
		src = groom->get_points();
		counts = groom->get_strand_point_counts();
	} else {
		// No groom: one 10 cm strand, so the mesh is never empty.
		for (int i = 0; i < 8; i++) {
			src.push_back(Vector3(0.0f, 0.1f * float(i) / 7.0f, 0.0f));
		}
		counts.push_back(8);
	}

	const int strips = crossed ? 2 : 1;
	LocalVector<Vector3> verts;
	LocalVector<Vector3> normals;
	LocalVector<float> tangents;
	LocalVector<Vector2> uvs;
	LocalVector<int> indices;
	verts.reserve(src.size() * strips * 2);
	normals.reserve(src.size() * strips * 2);
	tangents.reserve(src.size() * strips * 8);
	uvs.reserve(src.size() * strips * 2);

	int start = 0;
	for (int count : counts) {
		if (count < 2) {
			start += MAX(count, 0);
			continue;
		}
		const Vector3 *p = src.ptr() + start;
		// Length along the strand, for UV.y and the taper.
		LocalVector<float> along;
		along.resize(count);
		along[0] = 0.0f;
		for (int i = 1; i < count; i++) {
			along[i] = along[i - 1] + p[i].distance_to(p[i - 1]);
		}
		const float length = MAX(along[count - 1], 1e-6f);

		Vector3 t0 = (p[1] - p[0]).normalized();
		if (t0.length_squared() < 0.5f) {
			t0 = Vector3(0.0f, 1.0f, 0.0f);
		}
		Vector3 a = t0.cross(Math::abs(t0.y) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f)).normalized();
		Vector3 prev_t = t0;
		const int base = verts.size();
		for (int i = 0; i < count; i++) {
			Vector3 t = (p[MIN(i + 1, count - 1)] - p[MAX(i - 1, 0)]);
			t = t.length_squared() > 1e-16f ? t.normalized() : prev_t;
			// Parallel transport keeps the strips from twisting along the strand.
			const Vector3 axis = prev_t.cross(t);
			if (axis.length_squared() > 1e-12f) {
				a = a.rotated(axis.normalized(), prev_t.angle_to(t));
			}
			a = (a - t * a.dot(t)).normalized();
			prev_t = t;
			const float s = along[i] / length;
			const float half = 0.5f * Math::lerp(root_width, tip_width, s);
			for (int k = 0; k < strips; k++) {
				const Vector3 across = k == 0 ? a : t.cross(a);
				const Vector3 n = across.cross(t); // Godot's binormal, cross(normal, tangent), is then +t.
				for (int side = 0; side < 2; side++) {
					verts.push_back(p[i] + across * (side == 0 ? -half : half));
					normals.push_back(n);
					tangents.push_back(across.x);
					tangents.push_back(across.y);
					tangents.push_back(across.z);
					tangents.push_back(1.0f);
					uvs.push_back(Vector2(float(side), s));
				}
			}
		}
		const int stride = strips * 2;
		for (int i = 0; i + 1 < count; i++) {
			for (int k = 0; k < strips; k++) {
				const int v0 = base + i * stride + k * 2;
				const int v1 = v0 + 1;
				const int v2 = v0 + stride;
				const int v3 = v2 + 1;
				indices.push_back(v0);
				indices.push_back(v1);
				indices.push_back(v2);
				indices.push_back(v1);
				indices.push_back(v3);
				indices.push_back(v2);
			}
		}
		start += count;
	}

	p_arr[RSE::ARRAY_VERTEX] = Vector<Vector3>(verts);
	p_arr[RSE::ARRAY_NORMAL] = Vector<Vector3>(normals);
	p_arr[RSE::ARRAY_TANGENT] = Vector<float>(tangents);
	p_arr[RSE::ARRAY_TEX_UV] = Vector<Vector2>(uvs);
	p_arr[RSE::ARRAY_INDEX] = Vector<int>(indices);
}

void HairMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_groom", "groom"), &HairMesh::set_groom);
	ClassDB::bind_method(D_METHOD("get_groom"), &HairMesh::get_groom);
	ClassDB::bind_method(D_METHOD("set_root_width", "width"), &HairMesh::set_root_width);
	ClassDB::bind_method(D_METHOD("get_root_width"), &HairMesh::get_root_width);
	ClassDB::bind_method(D_METHOD("set_tip_width", "width"), &HairMesh::set_tip_width);
	ClassDB::bind_method(D_METHOD("get_tip_width"), &HairMesh::get_tip_width);
	ClassDB::bind_method(D_METHOD("set_crossed", "crossed"), &HairMesh::set_crossed);
	ClassDB::bind_method(D_METHOD("is_crossed"), &HairMesh::is_crossed);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "groom", PROPERTY_HINT_RESOURCE_TYPE, HairGroom::get_class_static()), "set_groom", "get_groom");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "root_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_root_width", "get_root_width");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_width", PROPERTY_HINT_RANGE, "0,0.01,0.00001,or_greater,suffix:m"), "set_tip_width", "get_tip_width");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "crossed"), "set_crossed", "is_crossed");
}

HairMesh::HairMesh() {
}
