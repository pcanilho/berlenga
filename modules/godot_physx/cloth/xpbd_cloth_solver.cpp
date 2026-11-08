/**************************************************************************/
/*  xpbd_cloth_solver.cpp                                                 */
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

#include "xpbd_cloth_solver.h"

#include "core/templates/hash_map.h"

void XPBDClothSolver::clear() {
	rest_positions.clear();
	positions.clear();
	prev.clear();
	velocities.clear();
	inv_mass.clear();
	base_inv_mass.clear();
	pinned.clear();
	pin_target.clear();
	constraints.clear();
	indices.clear();
	uvs.clear();
	last_tears.clear();
	cols = 0;
	rows = 0;
	rest_volume = 0.0f;
	applied_rest_scale = 1.0f;
}

// Signed enclosed volume of the closed triangle mesh via the divergence
// theorem: V = (1/6) sum over tris of x_a . (x_b x x_c), in the mesh's own
// winding. Origin-independent for a watertight mesh; the centroid offset just
// keeps the numbers small when the body sits far from the world origin.
float XPBDClothSolver::_mesh_volume(const LocalVector<Vector3> &p_pos) const {
	if (p_pos.is_empty() || indices.size() < 3) {
		return 0.0f;
	}
	Vector3 ref;
	for (uint32_t i = 0; i < p_pos.size(); i++) {
		ref += p_pos[i];
	}
	ref /= (float)p_pos.size();
	double v = 0.0;
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		const Vector3 a = p_pos[indices[t]] - ref;
		const Vector3 b = p_pos[indices[t + 1]] - ref;
		const Vector3 c = p_pos[indices[t + 2]] - ref;
		v += (double)a.dot(b.cross(c));
	}
	return (float)(v / 6.0);
}

float XPBDClothSolver::rest_surface_area() const {
	double area = 0.0;
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		const Vector3 &a = rest_positions[indices[t]];
		const Vector3 &b = rest_positions[indices[t + 1]];
		const Vector3 &c = rest_positions[indices[t + 2]];
		area += 0.5 * (double)(b - a).cross(c - a).length();
	}
	return (float)area;
}

void XPBDClothSolver::set_rest_length_scale(float p_scale) {
	if (p_scale <= 0.0f || Math::is_equal_approx(p_scale, applied_rest_scale)) {
		return;
	}
	const float rel = p_scale / applied_rest_scale;
	for (uint32_t i = 0; i < constraints.size(); i++) {
		constraints[i].rest *= rel;
	}
	applied_rest_scale = p_scale;
}

void XPBDClothSolver::_add_constraint(int p_a, int p_b, float p_compliance, bool p_structural, int p_via_a, int p_via_b) {
	DistanceConstraint c;
	c.a = p_a;
	c.b = p_b;
	c.rest = (rest_positions[p_a] - rest_positions[p_b]).length();
	c.compliance = p_compliance;
	c.structural = p_structural;
	c.via_a = p_via_a;
	c.via_b = p_via_b;
	constraints.push_back(c);
}

void XPBDClothSolver::build_grid(int p_cols, int p_rows, const Vector2 &p_size) {
	clear();
	cols = MAX(p_cols, 2);
	rows = MAX(p_rows, 2);
	const int n = cols * rows;
	rest_positions.resize(n);

	const float dx = p_size.x / (float)(cols - 1);
	const float dy = p_size.y / (float)(rows - 1);
	thickness = 0.5f * MIN(dx, dy);
	uvs.resize(n);
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			rest_positions[y * cols + x] = Vector3(-0.5f * p_size.x + x * dx, 0.5f * p_size.y - y * dy, 0.0f);
			uvs[y * cols + x] = Vector2((float)x / (float)(cols - 1), (float)y / (float)(rows - 1));
		}
	}

	// Triangle mesh (two per quad), clockwise seen from +Z: Godot's front face, so the grid faces +Z.
	for (int y = 0; y < rows - 1; y++) {
		for (int x = 0; x < cols - 1; x++) {
			const int a = y * cols + x;
			const int b = a + 1;
			const int c = a + cols;
			const int d = c + 1;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	const float stretch = settings.stretch_compliance;
	const float shear = settings.shear_compliance;
	const float bend = settings.bend_compliance;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			const int i = y * cols + x;
			if (x + 1 < cols) {
				_add_constraint(i, i + 1, stretch, true); // structural, horizontal
			}
			if (y + 1 < rows) {
				_add_constraint(i, i + cols, stretch, true); // structural, vertical
			}
			if (x + 1 < cols && y + 1 < rows) {
				_add_constraint(i, i + cols + 1, shear, false, i + 1, i + cols); // diagonal, across the quad's split edge
				_add_constraint(i + 1, i + cols, shear); // anti-diagonal: the split edge itself
			}
			if (x + 2 < cols) {
				_add_constraint(i, i + 2, bend, false, i + 1); // bending, horizontal, over i + 1
			}
			if (y + 2 < rows) {
				_add_constraint(i, i + 2 * cols, bend, false, i + cols); // bending, vertical, over i + cols
			}
		}
	}

	_finalize();
}

void XPBDClothSolver::build_mesh(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices, const PackedVector2Array &p_uvs) {
	clear();
	const int n = p_positions.size();
	if (n < 3 || p_indices.size() < 3) {
		return;
	}
	rest_positions.resize(n);
	for (int i = 0; i < n; i++) {
		rest_positions[i] = p_positions[i];
	}
	if (p_uvs.size() == n) {
		uvs.resize(n);
		for (int i = 0; i < n; i++) {
			uvs[i] = p_uvs[i];
		}
	}
	indices.resize(p_indices.size());
	float min_edge = 1.0e20f;
	for (int i = 0; i < (int)p_indices.size(); i++) {
		indices[i] = p_indices[i];
	}

	// Unique edges -> structural constraints; edge -> the two opposite vertices
	// of its adjacent triangles -> bending constraints.
	HashMap<uint64_t, int> edge_other; // packed (min,max) -> opposite vertex of the first tri seen
	auto key = [](int a, int b) -> uint64_t {
		const uint32_t lo = MIN(a, b);
		const uint32_t hi = MAX(a, b);
		return ((uint64_t)hi << 32) | lo;
	};
	const float stretch = settings.stretch_compliance;
	const float bend = settings.bend_compliance;
	HashMap<uint64_t, bool> have_structural;
	for (int t = 0; t + 2 < (int)indices.size(); t += 3) {
		const int v[3] = { indices[t], indices[t + 1], indices[t + 2] };
		for (int e = 0; e < 3; e++) {
			const int a = v[e];
			const int b = v[(e + 1) % 3];
			const int opp = v[(e + 2) % 3];
			const uint64_t k = key(a, b);
			min_edge = MIN(min_edge, (float)(rest_positions[a] - rest_positions[b]).length());
			if (!have_structural.has(k)) {
				have_structural[k] = true;
				_add_constraint(a, b, stretch, true);
			}
			HashMap<uint64_t, int>::Iterator it = edge_other.find(k);
			if (it == edge_other.end()) {
				edge_other[k] = opp;
			} else if (it->value != opp) {
				_add_constraint(it->value, opp, bend, false, a, b); // bend across the shared edge
			}
		}
	}
	thickness = 0.5f * (min_edge < 1.0e19f ? min_edge : 0.02f);
	_finalize();
}

void XPBDClothSolver::_finalize() {
	const int n = rest_positions.size();
	positions.resize(n);
	prev.resize(n);
	velocities.resize(n);
	inv_mass.resize(n);
	base_inv_mass.resize(n);
	pinned.resize(n);
	pin_target.resize(n);

	// Vertex mass from the surrounding triangle area * areal density.
	LocalVector<float> mass;
	mass.resize(n);
	for (int i = 0; i < n; i++) {
		mass[i] = 0.0f;
		velocities[i] = Vector3();
		pinned[i] = 0;
		pin_target[i] = Vector3(NAN, NAN, NAN);
	}
	for (int t = 0; t + 2 < (int)indices.size(); t += 3) {
		const int a = indices[t];
		const int b = indices[t + 1];
		const int c = indices[t + 2];
		const float area = 0.5f * (rest_positions[b] - rest_positions[a]).cross(rest_positions[c] - rest_positions[a]).length();
		const float m = area * settings.density / 3.0f;
		mass[a] += m;
		mass[b] += m;
		mass[c] += m;
	}
	for (int i = 0; i < n; i++) {
		const float m = MAX(mass[i], 1.0e-6f);
		base_inv_mass[i] = 1.0f / m;
		inv_mass[i] = base_inv_mass[i];
	}

	rest_volume = _mesh_volume(rest_positions);
}

// One global XPBD volume constraint: C = V(x) - pressure * rest_volume, with the
// per-vertex gradient dV/dx_i accumulated from the incident triangles. Keeps a
// closed soft body from pancaking under gravity and lets pressure > 1 inflate it.
void XPBDClothSolver::_solve_volume(float p_sdt) {
	if (settings.pressure <= 0.0f || Math::is_zero_approx(rest_volume) || indices.size() < 3) {
		return;
	}
	const int n = positions.size();

	Vector3 ref;
	for (int i = 0; i < n; i++) {
		ref += positions[i];
	}
	ref /= (float)MAX(n, 1);

	LocalVector<Vector3> grad;
	grad.resize(n);
	for (int i = 0; i < n; i++) {
		grad[i] = Vector3();
	}
	double vol = 0.0;
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		const int ia = indices[t];
		const int ib = indices[t + 1];
		const int ic = indices[t + 2];
		const Vector3 a = positions[ia] - ref;
		const Vector3 b = positions[ib] - ref;
		const Vector3 c = positions[ic] - ref;
		vol += (double)a.dot(b.cross(c));
		grad[ia] += b.cross(c) * (1.0f / 6.0f);
		grad[ib] += c.cross(a) * (1.0f / 6.0f);
		grad[ic] += a.cross(b) * (1.0f / 6.0f);
	}
	const float constraint = (float)(vol / 6.0) - settings.pressure * rest_volume;

	float denom = 0.0f;
	for (int i = 0; i < n; i++) {
		denom += inv_mass[i] * grad[i].length_squared();
	}
	if (denom < 1.0e-12f) {
		return;
	}
	// pressure_stiffness in (0,1] -> XPBD compliance: 1 is near-rigid, small
	// values are soft. Scaled by rest_volume^2 so the response is shape-size
	// independent.
	const float k = CLAMP(settings.pressure_stiffness, 0.01f, 1.0f);
	const float compliance = (1.0f - k) * 1.0e-3f * MAX(rest_volume * rest_volume, 1.0e-6f);
	const float alpha = compliance / (p_sdt * p_sdt);
	const float dlambda = -constraint / (denom + alpha);
	for (int i = 0; i < n; i++) {
		if (inv_mass[i] == 0.0f) {
			continue;
		}
		positions[i] += grad[i] * (dlambda * inv_mass[i]);
	}
}

float XPBDClothSolver::get_max_strain() const {
	float m = 1.0f;
	for (uint32_t i = 0; i < constraints.size(); i++) {
		if (constraints[i].structural) {
			m = MAX(m, constraints[i].strain);
		}
	}
	return m;
}

void XPBDClothSolver::reset(const Transform3D &p_xform) {
	for (uint32_t i = 0; i < constraints.size(); i++) {
		constraints[i].strain = 1.0f;
	}
	const int n = rest_positions.size();
	positions.resize(n);
	prev.resize(n);
	velocities.resize(n);
	for (int i = 0; i < n; i++) {
		positions[i] = p_xform.xform(rest_positions[i]);
		prev[i] = positions[i];
		velocities[i] = Vector3();
	}
}

void XPBDClothSolver::set_pinned(int p_vertex, bool p_pinned) {
	if (p_vertex < 0 || p_vertex >= (int)pinned.size()) {
		return;
	}
	pinned[p_vertex] = p_pinned ? 1 : 0;
	inv_mass[p_vertex] = p_pinned ? 0.0f : base_inv_mass[p_vertex];
	if (!p_pinned) {
		pin_target[p_vertex] = Vector3(NAN, NAN, NAN);
	}
}

bool XPBDClothSolver::is_pinned(int p_vertex) const {
	return p_vertex >= 0 && p_vertex < (int)pinned.size() && pinned[p_vertex] != 0;
}

void XPBDClothSolver::set_pin_target(int p_vertex, const Vector3 &p_world_pos) {
	if (p_vertex < 0 || p_vertex >= (int)pin_target.size()) {
		return;
	}
	pin_target[p_vertex] = p_world_pos;
	pinned[p_vertex] = 1;
	inv_mass[p_vertex] = 0.0f;
}

void XPBDClothSolver::clear_pin_target(int p_vertex) {
	if (p_vertex < 0 || p_vertex >= (int)pin_target.size()) {
		return;
	}
	pin_target[p_vertex] = Vector3(NAN, NAN, NAN);
}

void XPBDClothSolver::_apply_aero(float p_dt, const Vector3 &p_wind) {
	if (settings.drag <= 0.0f && settings.lift <= 0.0f) {
		return;
	}
	for (int t = 0; t + 2 < (int)indices.size(); t += 3) {
		const int a = indices[t];
		const int b = indices[t + 1];
		const int c = indices[t + 2];
		const Vector3 e1 = positions[b] - positions[a];
		const Vector3 e2 = positions[c] - positions[a];
		Vector3 cr = e1.cross(e2);
		const float twice_area = cr.length();
		if (twice_area < 1.0e-9f) {
			continue;
		}
		const Vector3 n = cr / twice_area;
		const float area = 0.5f * twice_area;
		const Vector3 v_tri = (velocities[a] + velocities[b] + velocities[c]) / 3.0f;
		const Vector3 rel = p_wind - v_tri;
		const float vn = n.dot(rel);
		const Vector3 tangential = rel - n * vn;
		// Drag pushes along the normal in proportion to how square-on the flow is;
		// lift nudges along the in-plane flow so slack cloth ripples.
		const Vector3 force = n * (settings.drag * area * vn * Math::abs(vn)) + tangential * (settings.lift * area * Math::abs(vn));
		const Vector3 impulse = force * (p_dt / 3.0f);
		velocities[a] += impulse * inv_mass[a];
		velocities[b] += impulse * inv_mass[b];
		velocities[c] += impulse * inv_mass[c];
	}
}

int XPBDClothSolver::step(float p_dt, const Vector3 &p_gravity, const Vector3 &p_wind) {
	last_tears.clear();
	const int n = positions.size();
	if (n == 0 || p_dt <= 0.0f) {
		return 0;
	}
	const int substeps = MAX(settings.substeps, 1);
	const float sdt = p_dt / (float)substeps;
	const float vel_retain = MAX(1.0f - settings.damping * p_dt, 0.0f);

	_apply_aero(p_dt, p_wind);

	for (int s = 0; s < substeps; s++) {
		// Integrate.
		for (int i = 0; i < n; i++) {
			prev[i] = positions[i];
			if (inv_mass[i] == 0.0f) {
				const Vector3 &target = pin_target[i];
				if (!Math::is_nan(target.x)) {
					positions[i] = target; // snap the pin to its anchor
				}
				continue;
			}
			velocities[i] += p_gravity * sdt;
			positions[i] += velocities[i] * sdt;
		}

		// One Gauss-Seidel pass over the distance constraints (small-steps XPBD:
		// lambda resets each substep, so its accumulation term drops out).
		for (uint32_t ci = 0; ci < constraints.size(); ci++) {
			const DistanceConstraint &c = constraints[ci];
			const float wa = inv_mass[c.a];
			const float wb = inv_mass[c.b];
			const float w = wa + wb;
			if (w == 0.0f) {
				continue;
			}
			Vector3 d = positions[c.a] - positions[c.b];
			const float len = d.length();
			if (len < 1.0e-9f) {
				continue;
			}
			d /= len;
			const float constraint = len - c.rest;
			const float alpha = c.compliance / (sdt * sdt);
			const float dlambda = -constraint / (w + alpha);
			positions[c.a] += d * (dlambda * wa);
			positions[c.b] -= d * (dlambda * wb);
		}

		// Optional closed-mesh volume/pressure constraint (soft bodies).
		_solve_volume(sdt);

		// Update velocities from the constrained motion.
		const float inv_sdt = 1.0f / sdt;
		const float max_speed = settings.max_speed;
		for (int i = 0; i < n; i++) {
			if (inv_mass[i] == 0.0f) {
				velocities[i] = Vector3();
				continue;
			}
			Vector3 v = (positions[i] - prev[i]) * inv_sdt * vel_retain;
			if (max_speed > 0.0f) {
				const float sp = v.length();
				if (sp > max_speed) {
					v *= max_speed / sp;
				}
			}
			velocities[i] = v;
		}
	}

	return settings.tear_factor > 1.0f ? _tear_overstretched(p_dt) : 0;
}

static bool _segments_cross(const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c, const Vector2 &p_d) {
	const real_t d1 = (p_b - p_a).cross(p_c - p_a);
	const real_t d2 = (p_b - p_a).cross(p_d - p_a);
	const real_t d3 = (p_d - p_c).cross(p_a - p_c);
	const real_t d4 = (p_d - p_c).cross(p_b - p_c);
	return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

int XPBDClothSolver::tear(const Vector2 &p_from, const Vector2 &p_to) {
	auto crosses = [&](int p_i, int p_j) {
		const Vector3 &a = rest_positions[p_i];
		const Vector3 &b = rest_positions[p_j];
		return _segments_cross(Vector2(a.x, a.y), Vector2(b.x, b.y), p_from, p_to);
	};
	int removed = 0;
	for (uint32_t i = 0; i < constraints.size();) {
		if (crosses(constraints[i].a, constraints[i].b)) {
			constraints.remove_at_unordered(i);
			removed++;
		} else {
			i++;
		}
	}
	LocalVector<int> kept;
	kept.reserve(indices.size());
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		const int a = indices[t];
		const int b = indices[t + 1];
		const int c = indices[t + 2];
		if (crosses(a, b) || crosses(b, c) || crosses(c, a)) {
			continue;
		}
		kept.push_back(a);
		kept.push_back(b);
		kept.push_back(c);
	}
	indices = kept;
	return removed;
}

// --- Strain tearing ---------------------------------------------------------------------------
//
// PhysX 2's tearable cloth (NX_CLF_TEARABLE with tearFactor, the cloth behind the tearing drapes
// of Batman: Arkham Asylum): an edge stretched past tearFactor times its rest length breaks, and
// the mesh opens there by splitting a vertex in two. The split follows the fracture rule of
// O'Brien and Hodgins ("Graphical Modeling and Animation of Brittle Fracture", SIGGRAPH 1999) as
// cloth tearing uses it (Metaaphanon, Bando, Chen and Nishita, "Simulation of Tearing Cloth with
// Frayed Edges", Pacific Graphics 2009): the plane through the vertex across the overloaded edge
// decides which of its triangles go with the copy. Every constraint through the vertex is then
// kept, moved to the copy, or dropped, by whether the triangles still join its two ends.

static int _tri_has(const LocalVector<int> &p_idx, int p_t, int p_v) {
	return p_idx[p_t] == p_v || p_idx[p_t + 1] == p_v || p_idx[p_t + 2] == p_v;
}

// Is there a triangle holding all of p_a, p_b and (when >= 0) p_c?
static bool _has_tri(const LocalVector<int> &p_idx, int p_a, int p_b, int p_c = -1) {
	for (uint32_t t = 0; t + 2 < p_idx.size(); t += 3) {
		if (_tri_has(p_idx, t, p_a) && _tri_has(p_idx, t, p_b) && (p_c < 0 || _tri_has(p_idx, t, p_c))) {
			return true;
		}
	}
	return false;
}

void XPBDClothSolver::_vertex_mass(int p_v) {
	float m = 0.0f;
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		if (!_tri_has(indices, t, p_v)) {
			continue;
		}
		const Vector3 &a = rest_positions[indices[t]];
		const Vector3 &b = rest_positions[indices[t + 1]];
		const Vector3 &c = rest_positions[indices[t + 2]];
		m += 0.5f * (b - a).cross(c - a).length() * settings.density / 3.0f;
	}
	base_inv_mass[p_v] = 1.0f / MAX(m, 1.0e-6f);
	inv_mass[p_v] = pinned[p_v] ? 0.0f : base_inv_mass[p_v];
}

// Splits p_v so that the triangles on p_toward's side of the plane through p_v (normal along the
// edge to p_toward) go with a new copy. A pinned p_v keeps its pin and the copy goes free, so an
// edge can rip off what holds it. Returns false when there is nothing to separate.
bool XPBDClothSolver::_split_vertex(int p_v, int p_toward) {
	Vector3 d = positions[p_toward] - positions[p_v];
	if (d.length_squared() < 1.0e-12f) {
		d = rest_positions[p_toward] - rest_positions[p_v];
	}
	d.normalize();
	LocalVector<int> side_pos;
	int side_neg = 0;
	for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
		if (!_tri_has(indices, t, p_v)) {
			continue;
		}
		const Vector3 centroid = (positions[indices[t]] + positions[indices[t + 1]] + positions[indices[t + 2]]) / 3.0f;
		// The triangles on the overloaded edge itself always go with the copy.
		if (_tri_has(indices, t, p_toward) || (centroid - positions[p_v]).dot(d) > 0.0f) {
			side_pos.push_back(t);
		} else {
			side_neg++;
		}
	}
	if (side_pos.is_empty() || (side_neg == 0 && !pinned[p_v])) {
		return false;
	}

	const int nv = rest_positions.size();
	rest_positions.push_back(rest_positions[p_v]);
	positions.push_back(positions[p_v]);
	prev.push_back(prev[p_v]);
	velocities.push_back(velocities[p_v]);
	inv_mass.push_back(base_inv_mass[p_v]);
	base_inv_mass.push_back(base_inv_mass[p_v]);
	pinned.push_back(0);
	pin_target.push_back(Vector3(NAN, NAN, NAN));
	if (!uvs.is_empty()) {
		uvs.push_back(uvs[p_v]);
	}
	for (uint32_t k = 0; k < side_pos.size(); k++) {
		const int t = side_pos[k];
		for (int e = 0; e < 3; e++) {
			if (indices[t + e] == p_v) {
				indices[t + e] = nv;
			}
		}
	}
	_vertex_mass(p_v);
	_vertex_mass(nv);

	auto valid = [&](const DistanceConstraint &c) {
		if (c.via_a < 0) {
			return _has_tri(indices, c.a, c.b);
		}
		if (c.via_b < 0) {
			return _has_tri(indices, c.a, c.via_a) && _has_tri(indices, c.via_a, c.b);
		}
		return _has_tri(indices, c.a, c.via_a, c.via_b) && _has_tri(indices, c.b, c.via_a, c.via_b);
	};
	auto moved = [&](int p_i) { return p_i == p_v ? nv : p_i; };
	for (uint32_t ci = 0; ci < constraints.size();) {
		DistanceConstraint &c = constraints[ci];
		if (c.a != p_v && c.b != p_v && c.via_a != p_v && c.via_b != p_v) {
			ci++;
			continue;
		}
		if (valid(c)) {
			ci++;
			continue;
		}
		DistanceConstraint m = c;
		m.a = moved(c.a);
		m.b = moved(c.b);
		m.via_a = c.via_a < 0 ? -1 : moved(c.via_a);
		m.via_b = c.via_b < 0 ? -1 : moved(c.via_b);
		if (valid(m)) {
			c = m;
			ci++;
		} else {
			constraints.remove_at_unordered(ci);
		}
	}
	last_tears.push_back(positions[p_v]);
	return true;
}

int XPBDClothSolver::_tear_overstretched(float p_dt) {
	struct Over {
		float ratio = 0.0f;
		int a = 0;
		int b = 0;
	};
	struct MostFirst {
		bool operator()(const Over &p_l, const Over &p_r) const { return p_l.ratio > p_r.ratio; }
	};
	const float limit = settings.tear_factor;
	const float k = 1.0f - Math::exp(-p_dt / TEAR_SMOOTH);
	LocalVector<Over> over;
	for (uint32_t ci = 0; ci < constraints.size(); ci++) {
		DistanceConstraint &c = constraints[ci];
		if (!c.structural || c.rest <= 0.0f) {
			continue;
		}
		c.strain += ((positions[c.a] - positions[c.b]).length() / c.rest - c.strain) * k;
		if (c.strain > limit) {
			over.push_back({ c.strain, c.a, c.b });
		}
	}
	if (over.is_empty()) {
		return 0;
	}
	over.sort_custom<MostFirst>();

	auto tri_count = [&](int p_v) {
		int k = 0;
		for (uint32_t t = 0; t + 2 < indices.size(); t += 3) {
			k += _tri_has(indices, t, p_v) ? 1 : 0;
		}
		return k;
	};
	LocalVector<int> touched;
	int splits = 0;
	for (uint32_t o = 0; o < over.size() && splits < MAX_TEARS_PER_STEP; o++) {
		const int a = over[o].a;
		const int b = over[o].b;
		if (touched.has(a) || touched.has(b) || !_has_tri(indices, a, b)) {
			continue; // already opened this step, or no longer an edge of the mesh
		}
		// A pinned end breaks first (the stress gathers where the cloth is held); otherwise the end
		// with more triangles, so the rip runs into the cloth rather than along its border.
		int first = a;
		int second = b;
		if (pinned[b] && !pinned[a]) {
			SWAP(first, second);
		} else if (pinned[a] == pinned[b] && tri_count(b) > tri_count(a)) {
			SWAP(first, second);
		}
		if (_split_vertex(first, second) || _split_vertex(second, first)) {
			touched.push_back(a);
			touched.push_back(b);
			splits++;
		}
	}
	return splits;
}
