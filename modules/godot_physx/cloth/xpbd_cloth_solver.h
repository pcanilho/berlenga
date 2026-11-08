/**************************************************************************/
/*  xpbd_cloth_solver.h                                                   */
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

#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// A small extended position-based-dynamics (XPBD) solver -- no PhysX and no
// scene dependency, so it runs on any platform. It is the CPU fallback for the
// PhysX GPU deformable-surface path (cloth) and also backs the module's
// SoftBody3D implementation: a closed triangle mesh whose edge constraints hold
// its shape and whose optional volume constraint (pressure > 0) stops it
// collapsing. (Named ...ClothSolver for historical reasons.)
//
// It uses the "small steps" formulation (Macklin et al. 2019): each frame is
// split into a number of substeps, and every substep does a single Gauss-Seidel
// pass over the constraints. Stiffness is expressed as XPBD compliance, so it
// stays consistent regardless of the substep count or frame rate.
class XPBDClothSolver {
public:
	struct Settings {
		int substeps = 12;
		// Compliance in m/N. 0 == rigid; larger == softer. Structural (stretch)
		// wants to stay near 0; shear and bending are softer by default.
		float stretch_compliance = 0.0f;
		float shear_compliance = 2.0e-4f;
		float bend_compliance = 1.0e-3f;
		float damping = 0.03f; // fraction of relative velocity removed per second
		float drag = 1.0f; // air drag normal to each triangle
		float lift = 0.2f; // sideways push from air flowing across a triangle
		float density = 0.2f; // kg/m^2, sets per-vertex mass
		// Closed-mesh volume preservation. `pressure` is the target enclosed
		// volume as a fraction of the rest volume: 0 disables the constraint
		// (plain cloth), 1 holds the rest shape, >1 inflates. `pressure_stiffness`
		// (0..1] is how hard it pulls back -- 1 is near-incompressible, small
		// values let it squish. Only meaningful for a watertight mesh.
		float pressure = 0.0f;
		float pressure_stiffness = 0.5f;
		float max_speed = 0.0f; // 0 = uncapped; else clamps per-vertex speed
		// Strain tearing, as PhysX 2's tearable cloth (tearFactor): after each step, a structural edge
		// stretched past tear_factor times its rest length breaks by splitting a vertex. 0 = off.
		float tear_factor = 0.0f;
	};

	// Most vertex splits made per step: the rest of the overloaded edges wait a step, so a rip runs
	// across the cloth over a few frames instead of shattering it at once.
	static constexpr int MAX_TEARS_PER_STEP = 4;
	// Seconds the stretch is averaged over before it is compared with tear_factor. One Gauss-Seidel
	// pass a substep leaves a thin strip's edges briefly over-long as a wave runs down it; a material
	// breaks under a load it carries, not under one step's solver error.
	static constexpr float TEAR_SMOOTH = 0.05f;

	Settings settings;

	void clear();

	// Build an (p_cols x p_rows) vertex grid spanning a local-space rectangle of
	// p_size meters, centered on the origin in the XY plane (facing +Z).
	void build_grid(int p_cols, int p_rows, const Vector2 &p_size);
	// Build from an arbitrary triangle mesh. Structural constraints follow the
	// unique edges; bending constraints span the shared edge of adjacent tris.
	// p_uvs (optional, one per vertex) are carried through vertex splits; a grid makes its own.
	void build_mesh(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices, const PackedVector2Array &p_uvs = PackedVector2Array());

	// Place the rest shape into world space and seed the simulation there.
	void reset(const Transform3D &p_xform);

	bool is_built() const { return !rest_positions.is_empty(); }
	int vertex_count() const { return positions.size(); }
	int grid_cols() const { return cols; }
	int grid_rows() const { return rows; }

	// inv_mass 0 pins a vertex. pin_target, when finite, drags a pinned vertex to
	// a world position each step (for attachment to a moving anchor).
	void set_pinned(int p_vertex, bool p_pinned);
	bool is_pinned(int p_vertex) const;
	void set_pin_target(int p_vertex, const Vector3 &p_world_pos);
	void clear_pin_target(int p_vertex);

	// Advance the simulation. p_wind is a world-space air velocity.
	// Returns how many vertices tore apart (split) at the end of this step (settings.tear_factor).
	int step(float p_dt, const Vector3 &p_gravity, const Vector3 &p_wind);

	// Mutable access for the node's post-step world-collision pass.
	LocalVector<Vector3> &positions_mut() { return positions; }
	LocalVector<Vector3> &velocities_mut() { return velocities; }
	const LocalVector<Vector3> &get_positions() const { return positions; }
	const LocalVector<Vector3> &get_velocities() const { return velocities; }
	const LocalVector<int> &get_indices() const { return indices; }
	// Texture coordinates per vertex (empty for a mesh built without them).
	const LocalVector<Vector2> &get_uvs() const { return uvs; }
	// World positions of the vertices split by the last step's tearing (where each rip opened).
	const LocalVector<Vector3> &get_last_tears() const { return last_tears; }
	// The largest smoothed stretch (length / rest) of any structural edge, as tearing measures it
	// (kept only while tear_factor is on).
	float get_max_strain() const;
	float vertex_radius() const { return thickness; }
	// Total rest surface area, for turning a target total mass into an areal density.
	float rest_surface_area() const;
	// Scale every constraint's rest length (SoftBody3D shrinking_factor).
	void set_rest_length_scale(float p_scale);
	// Cut the cloth along a segment in its local rest XY plane: constraints and triangles whose
	// rest edges cross it are removed, so the two sides separate. Returns the constraints removed.
	int tear(const Vector2 &p_from, const Vector2 &p_to);

private:
	struct DistanceConstraint {
		int a = 0;
		int b = 0;
		float rest = 0.0f;
		float compliance = 0.0f;
		// What holds the two ends together through the mesh, checked after a tear: neither (-1, -1),
		// a structural edge, valid while a triangle has both ends; one vertex (via_a only), a bend
		// over it, valid while a triangle holds a and via_a and another via_a and b; or an edge
		// (via_a, via_b), a shear or bend across it, valid while a triangle holds a and that edge
		// and another b and that edge.
		int via_a = -1;
		int via_b = -1;
		bool structural = false;
		float strain = 1.0f; // length / rest, smoothed over TEAR_SMOOTH (structural only)
	};

	LocalVector<Vector3> rest_positions; // local space
	LocalVector<Vector3> positions; // world space, current
	LocalVector<Vector3> prev; // world space, start of substep
	LocalVector<Vector3> velocities;
	LocalVector<float> inv_mass;
	LocalVector<float> base_inv_mass; // inv_mass with pinning ignored
	LocalVector<int> pinned; // 1 == pinned
	LocalVector<Vector3> pin_target; // NaN component == no target
	LocalVector<DistanceConstraint> constraints;
	LocalVector<int> indices;
	LocalVector<Vector2> uvs;
	LocalVector<Vector3> last_tears;

	int cols = 0;
	int rows = 0;
	float thickness = 0.01f; // half-spacing, used as the collision radius
	float rest_volume = 0.0f; // signed enclosed volume of the rest mesh
	float applied_rest_scale = 1.0f; // last shrinking_factor applied to constraint rest lengths

	void _add_constraint(int p_a, int p_b, float p_compliance, bool p_structural = false, int p_via_a = -1, int p_via_b = -1);
	int _tear_overstretched(float p_dt);
	bool _split_vertex(int p_v, int p_toward);
	void _drop_broken_constraints();
	void _vertex_mass(int p_v);
	void _finalize();
	void _apply_aero(float p_dt, const Vector3 &p_wind);
	void _solve_volume(float p_sdt);
	float _mesh_volume(const LocalVector<Vector3> &p_pos) const;
};
