/**************************************************************************/
/*  physx_blast_authoring.cpp                                             */
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

#include "physx_blast_authoring.h"

#include "../godot_physx_server_3d.h"

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture.h"

#include <NvBlast.h>
#include <NvBlastExtAuthoring.h>
#include <NvBlastExtAuthoringBondGenerator.h>
#include <NvBlastExtAuthoringConvexMeshBuilder.h>
#include <NvBlastExtAuthoringCutout.h>
#include <NvBlastExtAuthoringFractureTool.h>
#include <NvBlastExtAuthoringMesh.h>
#include <NvBlastExtAuthoringMeshCleaner.h>
#include <NvBlastExtAuthoringTypes.h>
#include <NvBlastTypes.h>
#include <PxPhysicsAPI.h>

#include <cstdlib>
#include <vector>

// Deliberately NOT `using namespace Nv::Blast;` -- Nv::Blast::Mesh collides
// with Godot's own ::Mesh (scene/resources/mesh.h), which this file also
// uses for the p_mesh parameter. Every Blast SDK type below is qualified
// explicitly instead.
using namespace physx;

namespace {

void blast_authoring_log(int32_t p_type, const char *p_msg, const char *p_file, int32_t p_line) {
	if (p_type <= 1) {
		ERR_PRINT(vformat("Blast authoring: %s (%s:%d)", p_msg, p_file, p_line));
	}
}

// Ported verbatim from the standalone blast_test_gen.cpp prototype that
// proved this whole bridge out -- ConvexMeshBuilder ships as a pure
// interface in this SDK (no built-in implementation, see that file's
// comments), so every caller has to provide one. Same "cook a real hull so
// ProcessFracture doesn't choke on a degenerate one, but the result is never
// actually consumed downstream" reasoning as the prototype: PhysXDestructible3D
// cooks its own PhysX convex hulls from each chunk's render-mesh points, not
// from Blast's authored CollisionHull data.
class GodotConvexMeshBuilder final : public Nv::Blast::ConvexMeshBuilder {
public:
	explicit GodotConvexMeshBuilder(PxPhysics *p_physics) :
			physics(p_physics) {}

	void release() override { delete this; }

	Nv::Blast::CollisionHull *buildCollisionGeometry(uint32_t p_count, const NvcVec3 *p_verts) override {
		std::vector<PxVec3> pts(p_count);
		for (uint32_t i = 0; i < p_count; i++) {
			pts[i] = PxVec3(p_verts[i].x, p_verts[i].y, p_verts[i].z);
		}
		PxConvexMeshDesc desc;
		desc.points.count = p_count;
		desc.points.stride = sizeof(PxVec3);
		desc.points.data = pts.data();
		desc.flags = PxConvexFlag::eCOMPUTE_CONVEX;
		PxCookingParams cook_params(physics->getTolerancesScale());
		PxConvexMesh *mesh = PxCreateConvexMesh(cook_params, desc, *PxGetStandaloneInsertionCallback());

		Nv::Blast::CollisionHull *hull = new Nv::Blast::CollisionHull();
		memset(hull, 0, sizeof(Nv::Blast::CollisionHull));
		if (!mesh) {
			return hull; // empty hull -- fine, unused downstream
		}

		const PxU32 nb_verts = mesh->getNbVertices();
		const PxVec3 *src_verts = mesh->getVertices();
		hull->pointsCount = nb_verts;
		hull->points = new NvcVec3[nb_verts];
		for (PxU32 i = 0; i < nb_verts; i++) {
			hull->points[i] = { src_verts[i].x, src_verts[i].y, src_verts[i].z };
		}

		const PxU32 nb_polys = mesh->getNbPolygons();
		hull->polygonDataCount = nb_polys;
		hull->polygonData = new Nv::Blast::HullPolygon[nb_polys];
		std::vector<PxU8> all_indices;
		for (PxU32 i = 0; i < nb_polys; i++) {
			PxHullPolygon pd;
			mesh->getPolygonData(i, pd);
			hull->polygonData[i].plane[0] = pd.mPlane[0];
			hull->polygonData[i].plane[1] = pd.mPlane[1];
			hull->polygonData[i].plane[2] = pd.mPlane[2];
			hull->polygonData[i].plane[3] = pd.mPlane[3];
			hull->polygonData[i].vertexCount = pd.mNbVerts;
			hull->polygonData[i].indexBase = (uint16_t)all_indices.size();
			const PxU8 *ib = mesh->getIndexBuffer();
			for (PxU16 j = 0; j < pd.mNbVerts; j++) {
				all_indices.push_back(ib[pd.mIndexBase + j]);
			}
		}
		hull->indicesCount = (uint32_t)all_indices.size();
		hull->indices = new uint32_t[all_indices.size()];
		for (size_t i = 0; i < all_indices.size(); i++) {
			hull->indices[i] = all_indices[i];
		}

		mesh->release();
		return hull;
	}

	void releaseCollisionHull(Nv::Blast::CollisionHull *hull) const override {
		if (!hull) {
			return;
		}
		delete[] hull->points;
		delete[] hull->indices;
		delete[] hull->polygonData;
		delete hull;
	}

private:
	PxPhysics *physics;
};

class GodotRandomGenerator : public Nv::Blast::RandomGeneratorBase {
public:
	void seed(int32_t p_seed) override { std::srand((unsigned)p_seed); }
	float getRandomValue() override { return (float)std::rand() / (float)RAND_MAX; }
};

// Pre-flight check for PATTERN_CUTOUT patterns, added after two different
// real crashes deep inside NvBlastExtAuthoringFractureToolImpl::cutout()
// (SweepingAccelerator construction) -- both traced to the same cause: a
// crack/wall shape that doesn't reach the pattern's edge leaves the fill
// region wrapping around it with a hole in the middle (unlike a real broken
// pane, where every shard is a closed polygon and every crack reaches a
// boundary). Confirmed by fixing exactly that -- extending the crack lines
// to the image edge -- and nothing else, which made an otherwise-identical
// pattern fracture cleanly.
//
// Rule enforced here, directly from that finding: every connected wall
// (crack-line) shape must touch the image boundary somewhere. A wall
// component with zero boundary-touching pixels is a "floating island" --
// the exact topology that crashed. Uses the same weighted-RGB threshold
// NvBlastExtAuthoringCutoutImpl.cpp's own createCutoutSet() applies (see
// PhysXBlastAuthoring::fracture_mesh()'s own comment on why it's 3 bytes/
// pixel, not the 1-byte/pixel the public header claims) so this reads the
// pattern exactly the way the SDK itself will.
bool cutout_pattern_has_floating_wall_island(const PackedByteArray &p_gray, uint32_t p_w, uint32_t p_h) {
	const int64_t n = (int64_t)p_w * (int64_t)p_h;
	if (n == 0) {
		return false;
	}

	LocalVector<bool> is_wall;
	is_wall.resize((uint32_t)n);
	{
		const uint8_t *g = p_gray.ptr();
		for (int64_t i = 0; i < n; i++) {
			const uint32_t pix = 16777216u * (uint32_t)g[i]; // (5033165+9898557+1845494) * gray, R=G=B
			is_wall[(uint32_t)i] = (pix >> 28) != 0;
		}
	}

	LocalVector<int32_t> visited; // -1 = unvisited wall pixel, else already assigned to a checked component
	visited.resize((uint32_t)n);
	for (uint32_t i = 0; i < (uint32_t)n; i++) {
		visited[i] = -1;
	}

	LocalVector<uint32_t> stack;
	for (uint32_t y = 0; y < p_h; y++) {
		for (uint32_t x = 0; x < p_w; x++) {
			const uint32_t start = y * p_w + x;
			if (!is_wall[start] || visited[start] != -1) {
				continue;
			}

			bool touches_border = false;
			stack.clear();
			stack.push_back(start);
			visited[start] = 1;
			while (!stack.is_empty()) {
				const uint32_t cur = stack[stack.size() - 1];
				stack.remove_at(stack.size() - 1);
				const uint32_t cx = cur % p_w;
				const uint32_t cy = cur / p_w;
				if (cx == 0 || cx == p_w - 1 || cy == 0 || cy == p_h - 1) {
					touches_border = true;
				}
				if (cx > 0) {
					const uint32_t nb = cur - 1;
					if (is_wall[nb] && visited[nb] == -1) {
						visited[nb] = 1;
						stack.push_back(nb);
					}
				}
				if (cx + 1 < p_w) {
					const uint32_t nb = cur + 1;
					if (is_wall[nb] && visited[nb] == -1) {
						visited[nb] = 1;
						stack.push_back(nb);
					}
				}
				if (cy > 0) {
					const uint32_t nb = cur - p_w;
					if (is_wall[nb] && visited[nb] == -1) {
						visited[nb] = 1;
						stack.push_back(nb);
					}
				}
				if (cy + 1 < p_h) {
					const uint32_t nb = cur + p_w;
					if (is_wall[nb] && visited[nb] == -1) {
						visited[nb] = 1;
						stack.push_back(nb);
					}
				}
			}

			if (!touches_border) {
				return true; // a floating island -- reject the whole pattern
			}
		}
	}
	return false;
}

// Whether every edge (by vertex index, the way Blast sees it) is shared by exactly two triangles: a closed mesh
// that doesn't need the mesh cleaner.
bool is_closed(const LocalVector<uint32_t> &p_indices) {
	HashMap<uint64_t, int> edges;
	for (uint32_t t = 0; t + 2 < p_indices.size(); t += 3) {
		for (int e = 0; e < 3; e++) {
			const uint64_t a = p_indices[t + e];
			const uint64_t b = p_indices[t + (e + 1) % 3];
			if (a == b) {
				return false; // a degenerate triangle
			}
			edges[MIN(a, b) << 32 | MAX(a, b)]++;
		}
	}
	for (const KeyValue<uint64_t, int> &kv : edges) {
		if (kv.value != 2) {
			return false;
		}
	}
	return !edges.is_empty();
}

} //namespace

void PhysXBlastAuthoring::_bind_methods() {
	ClassDB::bind_method(D_METHOD("fracture_mesh", "mesh", "site_count", "seed", "pattern", "cutout_pattern"), &PhysXBlastAuthoring::fracture_mesh, DEFVAL(PATTERN_VORONOI), DEFVAL(Ref<Texture2D>()));

	ClassDB::bind_method(D_METHOD("build_prefractured", "root", "pieces", "bonds", "materials"), &PhysXBlastAuthoring::build_prefractured, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("get_last_error"), &PhysXBlastAuthoring::get_last_error);
	ClassDB::bind_method(D_METHOD("get_last_warnings"), &PhysXBlastAuthoring::get_last_warnings);

	BIND_ENUM_CONSTANT(PATTERN_VORONOI);
	BIND_ENUM_CONSTANT(PATTERN_SLICING);
	BIND_ENUM_CONSTANT(PATTERN_CUTOUT);
}

// Printed as before, and kept for get_last_error() / get_last_warnings() (the fracture dialog shows them).
Ref<PhysXBlastAsset> PhysXBlastAuthoring::_fail(const String &p_message) {
	_error(p_message);
	return Ref<PhysXBlastAsset>();
}

void PhysXBlastAuthoring::_error(const String &p_message) {
	ERR_PRINT(p_message);
	last_error = p_message.trim_prefix("PhysXBlastAuthoring: ");
}

void PhysXBlastAuthoring::_warn(const String &p_message) {
	WARN_PRINT(p_message);
	last_warnings.push_back(p_message.trim_prefix("PhysXBlastAuthoring: "));
}

Ref<PhysXBlastAsset> PhysXBlastAuthoring::fracture_mesh(const Ref<Mesh> &p_mesh, int p_site_count, int p_seed, FracturePattern p_pattern, const Ref<Texture2D> &p_cutout_pattern) {
	last_error = String();
	last_warnings.clear();
	if (p_mesh.is_null() || p_mesh->get_surface_count() < 1) {
		return _fail("PhysXBlastAuthoring: mesh is null or has no surfaces.");
	}
	PxPhysics *physics = GodotPhysXServer3D::get_singleton() ? GodotPhysXServer3D::get_singleton()->get_px_physics() : nullptr;
	if (!physics) {
		return _fail("PhysXBlastAuthoring: PhysX not initialized.");
	}

	if (p_mesh->surface_get_primitive_type(0) != Mesh::PRIMITIVE_TRIANGLES) {
		return _fail("PhysXBlastAuthoring: the mesh's first surface isn't a triangle list.");
	}
	const Array arrays = p_mesh->surface_get_arrays(0);
	const PackedVector3Array verts = arrays[Mesh::ARRAY_VERTEX];
	if (verts.size() < 4) {
		return _fail("PhysXBlastAuthoring: mesh has too few vertices.");
	}
	PackedVector3Array src_normals = arrays[Mesh::ARRAY_NORMAL];
	PackedInt32Array src_indices = arrays[Mesh::ARRAY_INDEX];

	LocalVector<NvcVec3> positions;
	LocalVector<NvcVec3> normals;
	LocalVector<NvcVec2> uvs;
	positions.resize(verts.size());
	normals.resize(verts.size());
	uvs.resize(verts.size());
	for (int i = 0; i < verts.size(); i++) {
		positions[i] = { verts[i].x, verts[i].y, verts[i].z };
		// A flat placeholder if the mesh has none -- Blast's Voronoi split
		// doesn't need correct normals, and nothing downstream ever reads
		// them back (see GodotConvexMeshBuilder's own comment).
		const Vector3 n = (i < src_normals.size()) ? src_normals[i] : Vector3(0, 1, 0);
		normals[i] = { n.x, n.y, n.z };
		uvs[i] = { 0.0f, 0.0f };
	}

	LocalVector<uint32_t> indices;
	if (src_indices.size() >= 3) {
		indices.resize(src_indices.size());
		for (int i = 0; i < src_indices.size(); i++) {
			indices[i] = (uint32_t)src_indices[i];
		}
	} else {
		// Non-indexed mesh -- vertices are already one triangle list.
		indices.resize(verts.size());
		for (uint32_t i = 0; i < (uint32_t)verts.size(); i++) {
			indices[i] = i;
		}
	}

	// Blast trusts its input: an index past the vertices was read out of bounds, a partial triangle crashed, and a
	// NaN vertex or a mesh collapsed to a point spun the mesh cleaner forever. Checked here instead.
	if (indices.size() < 3 || indices.size() % 3 != 0) {
		return _fail(vformat("PhysXBlastAuthoring: %d indices isn't a whole number of triangles.", (int)indices.size()));
	}
	for (uint32_t i = 0; i < indices.size(); i++) {
		if (indices[i] >= (uint32_t)verts.size()) {
			return _fail(vformat("PhysXBlastAuthoring: index %d points past the mesh's %d vertices.", (int)indices[i], verts.size()));
		}
	}
	AABB bounds(verts[0], Vector3());
	for (int i = 0; i < verts.size(); i++) {
		if (!verts[i].is_finite()) {
			return _fail(vformat("PhysXBlastAuthoring: vertex %d isn't a finite position.", i));
		}
		bounds.expand_to(verts[i]);
	}
	const real_t thinnest = bounds.size[bounds.get_shortest_axis_index()];
	if (thinnest <= MAX(bounds.get_longest_axis_size(), (real_t)1.0) * (real_t)1e-5) {
		return _fail(vformat("PhysXBlastAuthoring: the mesh has no volume to fracture (its bounds are %s).", bounds.size));
	}

	// Inside out (its triangles face inward: a negative volume, Godot's front faces being clockwise): Blast keeps the
	// winding it's given, so every piece's outer faces would face in, culled -- the pieces look hollow. Flipped.
	real_t signed_volume = 0.0;
	for (uint32_t t = 0; t < indices.size(); t += 3) {
		const Vector3 &a = verts[indices[t]];
		const Vector3 &b = verts[indices[t + 1]];
		const Vector3 &c = verts[indices[t + 2]];
		signed_volume += a.dot(c.cross(b));
	}
	if (signed_volume < 0.0) {
		for (uint32_t t = 0; t < indices.size(); t += 3) {
			SWAP(indices[t + 1], indices[t + 2]);
		}
		_warn("PhysXBlastAuthoring: the mesh was inside out (its triangles faced inward); fractured with its triangles flipped.");
	}

	Nv::Blast::Mesh *raw_mesh = NvBlastExtAuthoringCreateMesh(positions.ptr(), normals.ptr(), uvs.ptr(),
			(uint32_t)positions.size(), indices.ptr(), (uint32_t)indices.size());
	if (!raw_mesh) {
		return _fail("PhysXBlastAuthoring: NvBlastExtAuthoringCreateMesh failed.");
	}

	// FractureTool requires a closed (watertight), self-intersection-free,
	// open-edge-free mesh (NvBlastExtAuthoringMeshCleaner.h) -- a Godot
	// PrimitiveMesh like BoxMesh doesn't weld vertices across face
	// boundaries (24 verts for 6 faces, not 8 shared corners), which reads
	// as "open edges" to Blast. Voronoi/Slicing tolerated that in practice;
	// Cutout's boolean CSG did not (crashed/produced a 1-chunk degenerate
	// result with "Not equal number of starting and ending vertices" until
	// this was added) -- clean unconditionally rather than special-case it
	// per pattern, since a strictly-valid mesh can only help the others too.
	// A mesh that's already closed (welded, every edge shared by two triangles) skips it: there's nothing to clean.
	// What the cleaner returns is checked against the source: empty or reaching outside the source's bounds (it broke
	// the shape), and the raw mesh is fractured instead; smaller than the source (it dropped pieces that weren't part
	// of a closed solid, a stray triangle), and it's kept, with a warning.
	Nv::Blast::MeshCleaner *mesh_cleaner = nullptr;
	Nv::Blast::Mesh *cleaned_mesh = nullptr;
	if (!is_closed(indices)) {
		mesh_cleaner = NvBlastExtAuthoringCreateMeshCleaner();
		cleaned_mesh = mesh_cleaner->cleanMesh(raw_mesh);
		if (!cleaned_mesh) {
			_warn("PhysXBlastAuthoring: the mesh cleaner failed; fracturing the mesh as it is.");
		} else {
			const NvcBounds3 &cb = cleaned_mesh->getBoundingBox();
			const AABB cleaned_bounds(Vector3(cb.minimum.x, cb.minimum.y, cb.minimum.z),
					Vector3(cb.maximum.x - cb.minimum.x, cb.maximum.y - cb.minimum.y, cb.maximum.z - cb.minimum.z));
			const real_t tolerance = bounds.get_longest_axis_size() * (real_t)0.01;
			const AABB allowed = bounds.grow(tolerance);
			if (cleaned_mesh->getFacetCount() == 0 || !allowed.encloses(cleaned_bounds)) {
				_warn(vformat("PhysXBlastAuthoring: the mesh cleaner broke the mesh (bounds %s, was %s); fracturing the mesh as it is.", cleaned_bounds, bounds));
				cleaned_mesh->release();
				cleaned_mesh = nullptr;
			} else if (!cleaned_bounds.grow(tolerance).encloses(bounds)) {
				_warn(vformat("PhysXBlastAuthoring: the mesh cleaner dropped parts of the mesh that weren't a closed solid (bounds %s, was %s).", cleaned_bounds, bounds));
			}
		}
	}
	Nv::Blast::Mesh *blast_mesh = cleaned_mesh ? cleaned_mesh : raw_mesh;

	Nv::Blast::FractureTool *fTool = NvBlastExtAuthoringCreateFractureTool();
	const Nv::Blast::Mesh *src_meshes[1] = { blast_mesh };
	fTool->setSourceMeshes(src_meshes, 1);

	GodotRandomGenerator rng;
	rng.seed(p_seed);

	Ref<PhysXBlastAsset> result;
	Nv::Blast::VoronoiSitesGenerator *sites_gen = nullptr;
	int32_t frac_result;

	if (p_pattern == PATTERN_SLICING) {
		// slicing() works in slice-counts-per-axis, not a single site count --
		// approximate p_site_count with a roughly-cube-root split so the
		// property still reads as "about this many pieces" regardless of
		// which pattern is picked. A small amount of offset/angle variation
		// by default so the cuts don't look like a perfectly uniform grid.
		int per_axis = 1;
		const int target = MAX(p_site_count, 1);
		while ((per_axis + 1) * (per_axis + 1) * (per_axis + 1) <= target) {
			per_axis++;
		}
		Nv::Blast::SlicingConfiguration conf;
		conf.x_slices = per_axis;
		conf.y_slices = per_axis;
		conf.z_slices = per_axis;
		conf.offset_variations = 0.2f;
		conf.angle_variations = 0.2f;
		frac_result = fTool->slicing(0, conf, false, &rng);
		if (frac_result != 0) {
			_error(vformat("PhysXBlastAuthoring: slicing failed, code=%d.", frac_result));
		}
	} else if (p_pattern == PATTERN_CUTOUT) {
		// Unlike Voronoi/Slicing, NvBlast doesn't generate this pattern itself
		// -- same as Unreal's own Blast integration, the caller supplies a
		// grayscale bitmap (see this class's header for why) and
		// NvBlastExtAuthoringBuildCutoutSet segments it into per-region loops.
		if (p_cutout_pattern.is_null()) {
			_error("PhysXBlastAuthoring: PATTERN_CUTOUT requires a cutout_pattern texture.");
			frac_result = -1;
		} else {
			Ref<Image> pattern_img = p_cutout_pattern->get_image();
			if (pattern_img.is_null() || pattern_img->is_empty()) {
				_error("PhysXBlastAuthoring: cutout_pattern texture has no image data.");
				frac_result = -1;
			} else {
				if (pattern_img->is_compressed()) {
					pattern_img->decompress();
				}
				// NvBlastExtAuthoringCutout.h's own doc comment claims "each
				// pixel is represented by one byte," but this SDK build's
				// actual createCutoutSet() (NvBlastExtAuthoringCutoutImpl.cpp)
				// reads 3 bytes/pixel -- a weighted RGB luminance, thresholded
				// near-binary (roughly: value below ~1/16 of max is interior/
				// fill, anything brighter is a wall/crack line). Confirmed by
				// reading that source directly after the stale 1-byte-per-
				// pixel assumption crashed with an out-of-bounds read -- it
				// walks 3 bytes per pixel regardless of what the header says.
				// Matches Unreal's own Blast integration, which independently
				// packs the same 3-bytes-per-pixel format for the same call.
				pattern_img->convert(Image::FORMAT_L8);
				const PackedByteArray gray = pattern_img->get_data();
				const uint32_t pw = (uint32_t)pattern_img->get_width();
				const uint32_t ph = (uint32_t)pattern_img->get_height();
				PackedByteArray pixels;
				pixels.resize((int64_t)pw * ph * 3);
				{
					uint8_t *dst = pixels.ptrw();
					const uint8_t *src = gray.ptr();
					const int64_t n = (int64_t)pw * ph;
					for (int64_t i = 0; i < n; i++) {
						dst[i * 3 + 0] = src[i];
						dst[i * 3 + 1] = src[i];
						dst[i * 3 + 2] = src[i];
					}
				}

				// Refuse rather than risk the crash a floating (edge-
				// unreached) crack island caused twice while building this
				// pattern -- see cutout_pattern_has_floating_wall_island()'s
				// own comment for the full story. A native SDK crash can't
				// be caught from here (it takes the whole process down), so
				// this has to be ruled out before ever calling cutout().
				if (cutout_pattern_has_floating_wall_island(gray, pw, ph)) {
					_error("PhysXBlastAuthoring: cutout_pattern rejected -- it has a crack/wall shape that "
						   "doesn't reach the image edge (a 'floating island'). Every crack line needs to "
						   "reach the pattern's boundary, the same way a real broken pane's cracks do -- "
						   "otherwise the fill region wraps around it with a hole in the middle, which is "
						   "known to crash the underlying Blast SDK's Cutout code rather than fail cleanly.");
					frac_result = -1;
				} else {
					Nv::Blast::CutoutSet *cutout_set = NvBlastExtAuthoringCreateCutoutSet();
					NvBlastExtAuthoringBuildCutoutSet(*cutout_set, pixels.ptr(), pw, ph,
							/*segmentationErrorThreshold*/ 1e-3f, /*snapThreshold*/ 1.0f,
							/*periodic*/ false, /*expandGaps*/ false);

					Nv::Blast::CutoutConfiguration conf;
					conf.cutoutSet = cutout_set;
					// Defaults otherwise: scale (-1,-1) auto-fits the pattern
					// to the chunk's own AABB, isRelativeTransform=true
					// centers it on the chunk -- exactly what a "just works"
					// first pass wants without per-mesh sizing math of our own.
					frac_result = fTool->cutout(0, conf, false, &rng);
					if (frac_result != 0) {
						_error(vformat("PhysXBlastAuthoring: cutout failed, code=%d.", frac_result));
					}
					cutout_set->release();
				}
			}
		}
	} else {
		sites_gen = NvBlastExtAuthoringCreateVoronoiSitesGenerator(blast_mesh, &rng);
		sites_gen->uniformlyGenerateSitesInMesh((uint32_t)MAX(p_site_count, 1));
		const NvcVec3 *sites = nullptr;
		const uint32_t site_count = sites_gen->getVoronoiSites(sites);
		frac_result = fTool->voronoiFracturing(0, site_count, sites, false);
		if (frac_result != 0) {
			_error(vformat("PhysXBlastAuthoring: voronoiFracturing failed, code=%d.", frac_result));
		}
	}

	if (frac_result == 0) {
		fTool->finalizeFracturing();

		GodotConvexMeshBuilder collision_builder(physics);
		Nv::Blast::BlastBondGenerator *bond_gen = NvBlastExtAuthoringCreateBondGenerator(&collision_builder);
		Nv::Blast::ConvexDecompositionParams cparams;
		// Voronoi cells and slicing planes of a convex source stay convex,
		// but a cutout region can easily be concave (an L-shaped brick, a
		// crack loop that isn't itself convex) -- allow real decomposition
		// there instead of silently producing a wrong/degenerate hull.
		cparams.maximumNumberOfHulls = (p_pattern == PATTERN_CUTOUT) ? 8 : 1;

		Nv::Blast::AuthoringResult *ares = NvBlastExtAuthoringProcessFracture(*fTool, *bond_gen, collision_builder, cparams);
		if (!ares) {
			_error("PhysXBlastAuthoring: NvBlastExtAuthoringProcessFracture failed.");
		} else {
			const uint32_t asset_size = NvBlastAssetGetSize(ares->asset, blast_authoring_log);
			PackedByteArray asset_bytes;
			asset_bytes.resize(asset_size);
			memcpy(asset_bytes.ptrw(), ares->asset, asset_size);

			Array chunk_points;
			chunk_points.resize(ares->chunkCount);
			for (uint32_t c = 0; c < ares->chunkCount; c++) {
				const uint32_t begin = ares->geometryOffset[c];
				const uint32_t end = ares->geometryOffset[c + 1];
				PackedVector3Array points;
				points.resize((end - begin) * 3);
				for (uint32_t t = begin; t < end; t++) {
					const Nv::Blast::Triangle &tri = ares->geometry[t];
					points.write[(t - begin) * 3 + 0] = Vector3(tri.a.p.x, tri.a.p.y, tri.a.p.z);
					points.write[(t - begin) * 3 + 1] = Vector3(tri.b.p.x, tri.b.p.y, tri.b.p.z);
					points.write[(t - begin) * 3 + 2] = Vector3(tri.c.p.x, tri.c.p.y, tri.c.p.z);
				}
				chunk_points[c] = points;
			}

			result.instantiate();
			result->set_asset_bytes(asset_bytes);
			result->set_chunk_points(chunk_points);

			NvBlastExtAuthoringReleaseAuthoringResult(collision_builder, ares);
		}
		bond_gen->release();
	}

	if (sites_gen) {
		sites_gen->release();
	}
	fTool->release();
	if (cleaned_mesh) {
		cleaned_mesh->release();
	}
	raw_mesh->release();
	if (mesh_cleaner) {
		mesh_cleaner->release();
	}
	return result;
}

namespace {

// A Mesh arrays Array as a triangle soup (the chunk_points format), with its volume and centroid (divergence
// theorem over the closed surface).
bool blast_soup(const Array &p_arrays, PackedVector3Array &r_points, double &r_volume, Vector3 &r_centroid) {
	if (p_arrays.size() != Mesh::ARRAY_MAX) {
		return false;
	}
	const PackedVector3Array verts = p_arrays[Mesh::ARRAY_VERTEX];
	const PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
	const int count = indices.is_empty() ? verts.size() : indices.size();
	if (count < 12) {
		return false;
	}
	r_points.resize(count);
	Vector3 *w = r_points.ptrw();
	for (int i = 0; i < count; i++) {
		const int vi = indices.is_empty() ? i : indices[i];
		ERR_FAIL_INDEX_V(vi, verts.size(), false);
		w[i] = verts[vi];
	}
	double vol = 0.0;
	Vector3 c;
	for (int t = 0; t + 2 < count; t += 3) {
		const Vector3 &a = w[t], &b = w[t + 1], &d = w[t + 2];
		const double v = double(a.dot(b.cross(d))) / 6.0;
		vol += v;
		c += (a + b + d) * real_t(v / 4.0);
	}
	r_volume = Math::abs(vol);
	r_centroid = Math::abs(vol) > 1e-12 ? c / real_t(vol) : Vector3();
	return r_volume > 0.0;
}

// A chunk_meshes entry's first surface (a Mesh's surface 0, the first of an Array of surfaces, or a single arrays
// Array as it is): the closed shell that gives the chunk its collision, volume and centroid.
Array blast_first_surface(const Variant &p_piece) {
	const Ref<Mesh> mesh = p_piece;
	if (mesh.is_valid()) {
		return mesh->get_surface_count() > 0 ? mesh->surface_get_arrays(0) : Array();
	}
	const Array a = p_piece;
	if (a.size() == Mesh::ARRAY_MAX && a[Mesh::ARRAY_VERTEX].get_type() == Variant::PACKED_VECTOR3_ARRAY) {
		return a;
	}
	if (a.is_empty()) {
		return Array();
	}
	if (a[0].get_type() == Variant::DICTIONARY) {
		return Dictionary(a[0]).get("arrays", Array());
	}
	return a[0];
}

} // namespace

Ref<PhysXBlastAsset> PhysXBlastAuthoring::build_prefractured(const Variant &p_root, const Array &p_pieces, const Array &p_bonds, const Array &p_materials) {
	last_error = String();
	last_warnings.clear();
	const int leaves = p_pieces.size();
	if (leaves < 2) {
		return _fail("PhysXBlastAuthoring: build_prefractured needs at least two pieces.");
	}
	Array points;
	points.resize(leaves + 1);
	std::vector<NvBlastChunkDesc> chunks((size_t)leaves + 1);
	for (int c = 0; c <= leaves; c++) {
		PackedVector3Array soup;
		double volume = 0.0;
		Vector3 centroid;
		if (!blast_soup(blast_first_surface(c == 0 ? p_root : p_pieces[c - 1]), soup, volume, centroid)) {
			return _fail(vformat("PhysXBlastAuthoring: build_prefractured: chunk %d is not a closed triangle mesh.", c));
		}
		points[c] = soup;
		NvBlastChunkDesc &d = chunks[c];
		d.centroid[0] = centroid.x;
		d.centroid[1] = centroid.y;
		d.centroid[2] = centroid.z;
		d.volume = float(volume);
		d.parentChunkDescIndex = c == 0 ? UINT32_MAX : 0;
		d.flags = c == 0 ? NvBlastChunkDesc::NoFlags : NvBlastChunkDesc::SupportFlag;
		d.userData = uint32_t(c);
	}
	std::vector<NvBlastBondDesc> bonds;
	// Per-bond health by chunk pair (asset chunk indices, low << 32 | high), when any bond gives one.
	HashMap<uint64_t, float> pair_health;
	for (int b = 0; b < p_bonds.size(); b++) {
		const Dictionary bd = p_bonds[b];
		const Vector2i pair = bd.get("chunks", Vector2i(-1, -1));
		if (pair.x < 0 || pair.y < 0 || pair.x >= leaves || pair.y >= leaves || pair.x == pair.y) {
			_warn(vformat("PhysXBlastAuthoring: build_prefractured: bond %d names no two pieces, skipped.", b));
			continue;
		}
		const Vector3 n = Vector3(bd.get("normal", Vector3(0, 1, 0))).normalized();
		const Vector3 c = bd.get("centroid", Vector3());
		NvBlastBondDesc desc;
		desc.chunkIndices[0] = uint32_t(pair.x + 1);
		desc.chunkIndices[1] = uint32_t(pair.y + 1);
		desc.bond.normal[0] = n.x;
		desc.bond.normal[1] = n.y;
		desc.bond.normal[2] = n.z;
		desc.bond.area = MAX(float(bd.get("area", 0.0)), 1e-6f);
		desc.bond.centroid[0] = c.x;
		desc.bond.centroid[1] = c.y;
		desc.bond.centroid[2] = c.z;
		desc.bond.userData = 0;
		bonds.push_back(desc);
		if (bd.has("health")) {
			const uint64_t lo = MIN(desc.chunkIndices[0], desc.chunkIndices[1]), hi = MAX(desc.chunkIndices[0], desc.chunkIndices[1]);
			pair_health[(lo << 32) | hi] = MAX(float(bd["health"]), 0.0f);
		}
	}
	if (bonds.empty()) {
		return _fail("PhysXBlastAuthoring: build_prefractured: no bonds, the pieces would never hold together.");
	}
	NvBlastAssetDesc desc;
	desc.chunkCount = uint32_t(chunks.size());
	desc.chunkDescs = chunks.data();
	desc.bondCount = uint32_t(bonds.size());
	desc.bondDescs = bonds.data();
	std::vector<char> scratch(NvBlastGetRequiredScratchForCreateAsset(&desc, blast_authoring_log) + 16);
	const size_t size = NvBlastGetAssetMemorySize(&desc, blast_authoring_log);
	if (size == 0) {
		return _fail("PhysXBlastAuthoring: build_prefractured: Blast rejected the chunk and bond descriptors.");
	}
	void *mem = Memory::alloc_aligned_static(size, 16);
	NvBlastAsset *asset = NvBlastCreateAsset(mem, &desc, scratch.data(), blast_authoring_log);
	if (!asset) {
		Memory::free_aligned_static(mem);
		return _fail("PhysXBlastAuthoring: build_prefractured: NvBlastCreateAsset failed.");
	}
	// Bond healths in the asset's own bond order (NvBlastCreateAsset orders bonds by the support graph), found
	// through the graph's node pairs; chunks keep their authored index in userData.
	PackedFloat32Array bond_healths;
	if (!pair_health.is_empty()) {
		bond_healths.resize(NvBlastAssetGetBondCount(asset, blast_authoring_log));
		bond_healths.fill(1.0f);
		const NvBlastSupportGraph graph = NvBlastAssetGetSupportGraph(asset, blast_authoring_log);
		const NvBlastChunk *asset_chunks = NvBlastAssetGetChunks(asset, blast_authoring_log);
		for (uint32_t node = 0; node < graph.nodeCount; node++) {
			for (uint32_t a = graph.adjacencyPartition[node]; a < graph.adjacencyPartition[node + 1]; a++) {
				const uint32_t bond = graph.adjacentBondIndices[a];
				const uint64_t c0 = asset_chunks[graph.chunkIndices[node]].userData;
				const uint64_t c1 = asset_chunks[graph.chunkIndices[graph.adjacentNodeIndices[a]]].userData;
				const float *h = pair_health.getptr((MIN(c0, c1) << 32) | MAX(c0, c1));
				if (h && bond < uint32_t(bond_healths.size())) {
					bond_healths.set(bond, *h);
				}
			}
		}
	}
	const uint32_t asset_size = NvBlastAssetGetSize(asset, blast_authoring_log);
	PackedByteArray bytes;
	bytes.resize(asset_size);
	memcpy(bytes.ptrw(), asset, asset_size);
	Memory::free_aligned_static(mem);

	Array meshes;
	meshes.push_back(p_root);
	for (int c = 0; c < leaves; c++) {
		meshes.push_back(p_pieces[c]);
	}
	Ref<PhysXBlastAsset> result;
	result.instantiate();
	result->set_asset_bytes(bytes);
	result->set_chunk_points(points);
	result->set_chunk_meshes(meshes);
	result->set_materials(p_materials);
	result->set_bond_healths(bond_healths);
	return result;
}
