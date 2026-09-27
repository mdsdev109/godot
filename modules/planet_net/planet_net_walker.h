/**************************************************************************/
/*  planet_net_walker.h                                                   */
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

// The player movement step, shared by server and client so that the client can predict its own player
// and the server can confirm it. Both run this exact code on the same inputs; it uses only IEEE
// arithmetic, PlanetDetMath and the planet field, all of which give the same bits on every machine
// (planet-demo docs/DETERMINISM.md). So whenever the server has the client's input for a tick, the two
// results are identical and there is nothing to correct. Keep it that way: no libm, no platform
// branches, no state that is not in WalkerState.

#include "core/math/planet_det_math.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "scene/resources/3d/planet_field_3d.h"

namespace PlanetNet {

struct WalkerInput {
	Vector2 move; // (right, forward), each in [-1, 1]
	float yaw = 0.f; // heading in radians from local north
};

struct WalkerState {
	Vector3 position;
	Vector3 velocity;
	Quaternion rotation;
};

struct Walker {
	double speed = 5.0;
	double radius = 150000.0; // used when there is no field
	Ref<PlanetField3D> field;

	Vector3 surface_point(const Vector3 &p_direction) const {
		const Vector3 dir = p_direction.normalized();
		return dir * (field.is_valid() ? double(field->eval_radius(dir)) : radius);
	}

	WalkerState step(const WalkerState &p_state, const WalkerInput &p_input, double p_dt) const {
		const Vector3 up = p_state.position.normalized();
		Vector3 north = Vector3(0, 1, 0) - up * up.y;
		if (north.length_squared() < 1e-12) {
			north = Vector3(0, 0, -1) - up * up.z;
		}
		north.normalize();
		const Vector3 east = north.cross(up);
		const double yaw = double(p_input.yaw);
		const Vector3 forward = north * PlanetDetMath::cos(yaw) + east * PlanetDetMath::sin(yaw);
		const Vector3 right = forward.cross(up);
		const double mx = CLAMP(double(p_input.move.x), -1.0, 1.0);
		const double my = CLAMP(double(p_input.move.y), -1.0, 1.0);
		Vector3 velocity = (forward * my + right * mx) * speed;
		if (velocity.length_squared() > speed * speed) {
			velocity = velocity.normalized() * speed;
		}
		WalkerState out;
		out.position = surface_point(p_state.position + velocity * p_dt);
		out.velocity = (out.position - p_state.position) / p_dt;
		out.rotation = Basis(right, up, -forward).get_rotation_quaternion();
		return out;
	}
};

} // namespace PlanetNet
