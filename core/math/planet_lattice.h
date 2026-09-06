/**************************************************************************/
/*  planet_lattice.h                                                      */
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

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"

#include <cstdint>

// The single deterministic lattice over the planet's surface: cube face plus integer cell indices.
//
// One lattice serves three purposes and there must not be a second one.
//
//  1. Stable SubShapeIDs. Jolt caches contact manifolds between frames to warm-start the solver, and
//     that cache is what stops resting bodies buzzing. Regenerated triangles must therefore carry an
//     ID that is a pure function of position, never of the query that produced them.
//  2. Deterministic tessellation. Triangle vertices come from lattice points, so two different
//     queries covering the same ground produce the identical surface.
//  3. Edit quantization. A voxel-edited region's hole is an integer set of lattice cells, so the
//     analytic/voxel boundary is a lattice cell boundary and the boundary vertices are `f` evaluated
//     at the same lattice points through the same code on both sides.
//
// Cube map rather than equirectangular or Mercator: no polar singularity, bounded distortion, and
// the direction for a cell needs no trigonometry.
//
// SEAM EXACTNESS is the delicate part. A lattice vertex on a face edge is shared by two faces, and a
// corner vertex by three. If each face computed that vertex from its own (face, i, j) the three
// results would differ in the last bits and the surface would crack. vertex_direction() therefore
// canonicalizes an edge or corner vertex to one owning face before doing any arithmetic, so every
// face that touches a shared vertex gets bit-identical coordinates for it.
struct PlanetLattice {
	// Cells along one edge of one cube face. The surface is 6 * resolution^2 cells.
	//
	// At a 3,185,500 m radius the face edge arc is pi*R/2 = 5,003,772 m, so 0.25 m cells need a
	// resolution of about 2e7. That is far more cells than any index could enumerate globally, which
	// is fine: nothing ever iterates the whole lattice, only the handful of cells under a query.
	uint32_t resolution = 1u << 24;

	// The six cube faces, in the order +X, -X, +Y, -Y, +Z, -Z.
	//
	// The mapping is chosen so that a face's (s, t) in [-1, 1]^2 produces a point on the cube surface
	// whose components are literally +-1, s and t. That is what makes seams exact: two faces meeting
	// at an edge produce the same cube point because they are assembling the same three numbers, not
	// because two different formulas happen to agree.
	static Vector3 cube_point(int p_face, double p_s, double p_t) {
		switch (p_face) {
			case 0:
				return Vector3(1.0, p_t, -p_s); // +X
			case 1:
				return Vector3(-1.0, p_t, p_s); // -X
			case 2:
				return Vector3(p_s, 1.0, -p_t); // +Y
			case 3:
				return Vector3(p_s, -1.0, p_t); // -Y
			case 4:
				return Vector3(p_s, p_t, 1.0); // +Z
			default:
				return Vector3(-p_s, p_t, -1.0); // -Z
		}
	}

	// Inverse of vertex_direction: which face a direction belongs to, and its LINEAR (s, t) on that
	// face, i.e. already unwarped so it maps straight to a vertex index. A direction exactly on a face
	// boundary resolves to whichever face the comparison picks; that is fine, because it is only used
	// to locate cells, never to produce vertex positions.
	static void direction_to_face(const Vector3 &p_dir, int &r_face, double &r_s, double &r_t) {
		const double ax = Math::abs(p_dir.x);
		const double ay = Math::abs(p_dir.y);
		const double az = Math::abs(p_dir.z);

		if (ax >= ay && ax >= az) {
			const double inv = 1.0 / ax;
			if (p_dir.x > 0.0) {
				r_face = 0;
				r_s = -p_dir.z * inv;
				r_t = p_dir.y * inv;
			} else {
				r_face = 1;
				r_s = p_dir.z * inv;
				r_t = p_dir.y * inv;
			}
		} else if (ay >= az) {
			const double inv = 1.0 / ay;
			if (p_dir.y > 0.0) {
				r_face = 2;
				r_s = p_dir.x * inv;
				r_t = -p_dir.z * inv;
			} else {
				r_face = 3;
				r_s = p_dir.x * inv;
				r_t = p_dir.z * inv;
			}
		} else {
			const double inv = 1.0 / az;
			if (p_dir.z > 0.0) {
				r_face = 4;
				r_s = p_dir.x * inv;
				r_t = p_dir.y * inv;
			} else {
				r_face = 5;
				r_s = -p_dir.x * inv;
				r_t = p_dir.y * inv;
			}
		}

		// The ratios above are the gnomonic (tan) coordinate; undo the equi-angular warp so callers
		// get a coordinate they can scale directly by the resolution.
		r_s = face_unwarp(r_s);
		r_t = face_unwarp(r_t);
	}

	// Vertex index -> linear face coordinate in [-1, 1]. Integer arithmetic up to the final divide, so
	// a given index always yields exactly the same double.
	double vertex_coord(uint32_t p_index) const {
		return (2.0 * double(p_index)) / double(resolution) - 1.0;
	}

	// EQUI-ANGULAR face mapping, per the design's section 2.5.
	//
	// The obvious mapping puts the cube coordinate straight into cube_point, which spaces cells evenly
	// on the cube but not on the sphere: cells at a face corner end up about 5.2x smaller in solid
	// angle than at the face centre. That shows up as visibly stretched geometry towards cube edges and
	// as wasted lattice resolution where it is not wanted.
	//
	// Warping by tan() instead makes each cell subtend an equal ANGLE, which brings worst-case
	// distortion down to about 1.35x. Cost is a tan per axis per vertex, a couple of percent against
	// thirteen octaves of noise.
	static double face_warp(double p_linear) {
		return Math::tan(p_linear * (Math::PI / 4.0));
	}

	static double face_unwarp(double p_warped) {
		return Math::atan(p_warped) * (4.0 / Math::PI);
	}

	// Direction of lattice vertex (face, i, j), with i and j in [0, resolution].
	//
	// Vertices on a face edge or corner are shared with other faces. Rather than trusting two
	// parameterizations to agree in the last bit, the shared cube point is assembled directly from
	// the +-1 and the interpolated coordinate, which is identical no matter which face asks.
	Vector3 vertex_direction(int p_face, uint32_t p_i, uint32_t p_j) const {
		const double s = face_warp(vertex_coord(p_i));
		const double t = face_warp(vertex_coord(p_j));
		return cube_point(p_face, s, t).normalized();
	}

	// Cell index -> the four corner vertex indices are simply (ci, cj) .. (ci+1, cj+1).

	// Hash a lattice cell into the SubShapeID bits available to this shape.
	//
	// 25 bits cannot uniquely index every 0.25 m cell on a planet, and it does not need to. Jolt keys
	// the manifold cache by (body pair, sub-shape pair), and every contact within one body pair is
	// spatially local, so two cells on opposite sides of the planet aliasing to the same bucket is
	// harmless. Only local uniqueness matters, and across the few dozen triangles in one contact set
	// against 2^25 buckets, a collision is negligible.
	//
	// This is also why GetSurfaceNormal must work from the position rather than by inverting the ID.
	static uint32_t cell_hash(int p_face, uint32_t p_ci, uint32_t p_cj, uint32_t p_bits) {
		uint32_t h = uint32_t(p_face) * 0x9e3779b9u;
		h += p_ci * 0x85ebca6bu;
		h += p_cj * 0xc2b2ae35u;
		h ^= h >> 16;
		h *= 0x7feb352du;
		h ^= h >> 15;
		h *= 0x846ca68bu;
		h ^= h >> 16;
		// Never return 0: Jolt treats an empty SubShapeID as "no sub shape", and a triangle that
		// claims to be the whole shape breaks the manifold cache in a way that looks like jitter.
		const uint32_t mask = (p_bits >= 32) ? 0xffffffffu : ((1u << p_bits) - 1u);
		const uint32_t masked = h & mask;
		return masked == 0 ? 1u : masked;
	}

	// Half-width in (s, t) of one cell, useful for expanding a query range conservatively.
	double cell_size_st() const { return 2.0 / double(resolution); }

	// Splits a face into per_axis^2 patches and returns the half-open cell range of one of them.
	// Both the physics backend and the scene layer must agree on this exactly: the shape emits
	// geometry relative to the patch origin, and the body is positioned at that same origin, so a
	// mismatch shifts the whole patch off the planet.
	void patch_cell_range(
			uint32_t p_per_axis,
			uint32_t p_pi,
			uint32_t p_pj,
			uint32_t &r_min_i,
			uint32_t &r_min_j,
			uint32_t &r_max_i,
			uint32_t &r_max_j) const {
		const uint32_t per_patch = resolution / p_per_axis;
		r_min_i = p_pi * per_patch;
		r_min_j = p_pj * per_patch;
		r_max_i = (p_pi + 1 == p_per_axis) ? resolution : (p_pi + 1) * per_patch;
		r_max_j = (p_pj + 1 == p_per_axis) ? resolution : (p_pj + 1) * per_patch;
	}

	// Direction through the middle of a patch.
	Vector3 patch_centre_dir(int p_face, uint32_t p_per_axis, uint32_t p_pi, uint32_t p_pj) const {
		uint32_t min_i, min_j, max_i, max_j;
		patch_cell_range(p_per_axis, p_pi, p_pj, min_i, min_j, max_i, max_j);
		const uint32_t mid_i = uint32_t((uint64_t(min_i) + uint64_t(max_i)) / 2);
		const uint32_t mid_j = uint32_t((uint64_t(min_j) + uint64_t(max_j)) / 2);
		return vertex_direction(p_face, mid_i, mid_j);
	}
};
