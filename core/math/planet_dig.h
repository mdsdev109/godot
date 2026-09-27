/**************************************************************************/
/*  planet_dig.h                                                          */
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

// The ground a dig site has taken over from the analytic surface (planet-demo docs/DIGGING.md). Inside
// a footprint the site's voxels are the ground, so the analytic collider must produce nothing there and
// the renderer must lower the analytic surface out of sight.

#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

struct PlanetFootprint {
	// A planet-space point at the centre of the footprint, near the surface.
	Vector3 centre;
	// Unit tangents spanning the footprint.
	Vector3 axis_u;
	Vector3 axis_v;
	double half_u = 0.0;
	double half_v = 0.0;

	// Whether a planet-space point near the surface lies in the footprint's column. Points far from the
	// centre -- including anything on the other side of the planet -- are rejected by distance first.
	bool contains(const Vector3 &p_point) const {
		const Vector3 d = p_point - centre;
		const double reach = 2.0 * (half_u + half_v) + 1000.0;
		if (d.length_squared() > reach * reach) {
			return false;
		}
		return Math::abs(d.dot(axis_u)) < half_u && Math::abs(d.dot(axis_v)) < half_v;
	}

	bool operator==(const PlanetFootprint &p_other) const {
		return centre == p_other.centre && axis_u == p_other.axis_u && axis_v == p_other.axis_v &&
				half_u == p_other.half_u && half_v == p_other.half_v;
	}
};

// Footprints cross the physics server as a flat PackedFloat64Array, 11 doubles each: centre (3),
// axis_u (3), axis_v (3), half_u, half_v.
constexpr int PLANET_FOOTPRINT_STRIDE = 11;

inline void planet_footprints_unpack(const PackedFloat64Array &p_packed, LocalVector<PlanetFootprint> &r_footprints) {
	r_footprints.clear();
	const int n = p_packed.size() / PLANET_FOOTPRINT_STRIDE;
	const double *v = p_packed.ptr();
	for (int i = 0; i < n; ++i) {
		const double *f = v + i * PLANET_FOOTPRINT_STRIDE;
		PlanetFootprint fp;
		fp.centre = Vector3(f[0], f[1], f[2]);
		fp.axis_u = Vector3(f[3], f[4], f[5]).normalized();
		fp.axis_v = Vector3(f[6], f[7], f[8]).normalized();
		fp.half_u = f[9];
		fp.half_v = f[10];
		r_footprints.push_back(fp);
	}
}

inline bool planet_footprints_equal(const LocalVector<PlanetFootprint> &p_a, const LocalVector<PlanetFootprint> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); ++i) {
		if (!(p_a[i] == p_b[i])) {
			return false;
		}
	}
	return true;
}
