/**************************************************************************/
/*  tree_bark_detail.h                                                    */
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

#pragma once

#include "tree_generator.h"

#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/multimesh.h"

// Close-up bark (docs/ideas/ground-detail.md, "Close-up bark"): loose bark pieces as real instanced geometry on the
// trunks near the camera, over LOD 0's displaced relief: lifting plates of pine, small scales, flakes curling at an
// edge, and curls of paper bark (bark as elements laid on the stem, Pixar's "Elemental Bark"). Trunks come from a
// VegetationField3D (get_trunks_near) or set_trunks; each piece kind is drawn out to where its pieces are still
// detail_pixels tall, as one ray-traced instance each over a shared shape.
class BarkDetail3D : public Node3D {
	GDCLASS(BarkDetail3D, Node3D);

public:
	enum PieceKind {
		PIECE_PLATE, // a plate lifting from the stem at its lower edge (pines; thick plates)
		PIECE_SCALE, // a small square scale (holm oak, blackthorn)
		PIECE_FLAKE, // a thin flake curling out at one end (flaking bark)
		PIECE_CURL, // a strip of paper bark rolled outward (birch), across the stem
		PIECE_MAX,
	};
	static constexpr int SHAPES = 8; // shapes per kind and bark family, so no two neighbours repeat

private:
	struct Piece {
		Transform3D xform;
		Color custom; // texture offset (x, y), tone, random
		int shape = 0;
	};
	struct Trunk {
		uint64_t key = 0;
		int bark = 0; // TreeGenerator::BarkKind
		Vector3 base;
		real_t reach = 0.0; // m from the camera its pieces are drawn to
		LocalVector<Piece> pieces;
		uint64_t seen = 0; // update it was last wanted
	};
	struct Family {
		Ref<ArrayMesh> meshes[SHAPES];
		Ref<MultiMesh> multimeshes[SHAPES];
		MultiMeshInstance3D *instances[SHAPES] = {};
		Ref<ShaderMaterial> material;
		Vector2 shape_size[SHAPES]; // each shape's extent round the stem and up it (m)
		bool textured = false;
	};

	bool enabled = true;
	NodePath vegetation_path;
	NodePath target_path;
	real_t height = 2.5; // m up the trunk
	real_t density = 1.0;
	real_t detail_pixels = 3.0;
	int reference_height = 0; // px; 0: the viewport's
	real_t max_reach = 25.0;

	// A trunk mesh's real bark surface round its stem (mesh space): the outermost bark vertex in each cell of a grid
	// along the stem's capsule chain and round it, with its vertex colour (the species' bark tint), so pieces lie on
	// LOD 0's displaced plates, not on the capsules, and take the stem's own colour. Built once a mesh, on the main
	// thread (its arrays come from the RenderingServer).
	struct Profile {
		LocalVector<Vector3> a, u, e1, e2;
		LocalVector<float> len, r, along0;
		float step = 0.025f; // m along
		int na = 96; // cells round
		int nl = 0;
		float total = 0.0f;
		Vector3 a0;
		LocalVector<float> rad;
		LocalVector<Color> col;
		PackedVector3Array verts; // the bark surface, until it is binned (on the worker)
		PackedColorArray colors;
		bool built = false;
	};
	HashMap<const Mesh *, Profile *> profiles;
	Profile *_profile(const Ref<Mesh> &p_mesh);
	static void _build_profile(Profile *p_profile);
	static Profile *_profile_chain(const PackedFloat32Array &p_colliders, float p_cap);
	static constexpr float PROFILE_CAP = 4.2f; // m up the stem a tree's own profile covers
	Ref<Mesh> _trunk_mesh_of(const Dictionary &p_trunk) const;

	// New trunks are placed on a worker (the first trunk of a family builds its bark height set there too).
	struct PlaceJob {
		const BarkDetail3D *field = nullptr;
		LocalVector<Dictionary> dicts;
		LocalVector<Profile *> profs;
		LocalVector<Trunk> out;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
	};
	PlaceJob *job = nullptr;
	static void _place_job(void *p_job);
	void _finish_job(bool p_wait);
	bool sync = false; // place on the main thread (tests)

	bool manual = false; // set_trunks: no VegetationField3D
	Array manual_trunks;
	HashMap<uint64_t, Trunk> trunks;
	Family families[TreeGenerator::BARK_MAX];
	uint64_t updates = 0;
	Vector3 last_center = Vector3(1e9, 1e9, 1e9);
	double since = 1e9;
	bool dirty = true;
	int piece_count = 0;
	double update_msec_sum = 0.0;
	double update_msec_worst = 0.0;
	int update_runs = 0;

	Ref<Shader> shader;
	Ref<Shader> _shader();
	uint64_t last_sig[TreeGenerator::BARK_MAX] = {};
	static PieceKind _piece_kind(TreeGenerator::BarkKind p_bark);
	static Ref<ArrayMesh> _make_shape(TreeGenerator::BarkKind p_bark, int p_shape);
	void _ensure_family(TreeGenerator::BarkKind p_bark, const Ref<Material> &p_bark_material);
	void _place(Trunk &r_trunk, const Dictionary &p_trunk, const Profile *p_profile) const;
	real_t _pixel_angle() const;
	Vector3 _center() const;
	Ref<Material> _bark_material_of(const Dictionary &p_trunk) const;
	void _draw();
	void _clear();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_enabled(bool p_enabled);
	bool is_enabled() const { return enabled; }
	void set_vegetation(const NodePath &p_path);
	NodePath get_vegetation() const { return vegetation_path; }
	void set_target(const NodePath &p_path);
	NodePath get_target() const { return target_path; }
	void set_height(real_t p_height);
	real_t get_height() const { return height; }
	void set_density(real_t p_density);
	real_t get_density() const { return density; }
	void set_detail_pixels(real_t p_pixels);
	real_t get_detail_pixels() const { return detail_pixels; }
	void set_reference_height(int p_height);
	int get_reference_height() const { return reference_height; }
	void set_max_reach(real_t p_reach);
	real_t get_max_reach() const { return max_reach; }

	void set_trunks(const Array &p_trunks);
	void set_sync(bool p_sync) { sync = p_sync; }
	bool is_sync() const { return sync; }
	bool is_placing() const { return job != nullptr; }
	void update_now();
	int get_piece_count() const { return piece_count; }
	int get_trunk_count() const { return trunks.size(); }
	double get_update_msec_mean() const { return update_runs > 0 ? update_msec_sum / update_runs : 0.0; }
	double get_update_msec_worst() const { return update_msec_worst; }
	// Metres a piece of this bark family is drawn out to from the camera (the pixel rule).
	real_t get_reach(int p_bark) const;
	static real_t get_piece_size(int p_bark);
	static int get_bark_kind(int p_preset) { return int(TreeGenerator::bark_kind_for_preset(p_preset)); }
	// A trunk mesh's bark surface binned round its stem, for its "tree_bark_profile" meta (TreeGenerator sets it from
	// the bark surface's arrays when it builds the mesh, so nothing is read back from the GPU later): [header
	// (cells round, rows, row step m), outermost radius per cell (m), vertex colour per cell (RGB8, 0..2)].
	static Array make_bark_profile(const PackedVector3Array &p_vertices, const PackedColorArray &p_colors, const PackedFloat32Array &p_colliders);

	BarkDetail3D();
	~BarkDetail3D();
};

VARIANT_ENUM_CAST(BarkDetail3D::PieceKind);
