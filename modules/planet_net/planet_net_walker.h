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
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "scene/resources/3d/planet_field_3d.h"
#include "servers/physics_3d/direct_states/physics_direct_space_state_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace PlanetNet {

enum WalkerMode : uint8_t {
	MODE_GROUND = 0,
	MODE_AIR = 1,
	MODE_CLIMB = 2,
};

enum WalkerFlag : uint8_t {
	// Let go of a wall: slide down faces rather than grab them, until standing on walkable ground.
	FLAG_NO_GRAB = 1,
};

enum WalkerButton : uint8_t {
	BUTTON_JUMP = 1,
	BUTTON_SPRINT = 2,
	BUTTON_DROP = 4, // let go of a wall
};

struct WalkerInput {
	Vector2 move; // (right, forward), each in [-1, 1]
	float yaw = 0.f; // heading in radians from the walker's reference direction
	uint8_t buttons = 0; // WalkerButton bits held this tick
};

struct WalkerState {
	Vector3 position;
	Vector3 velocity;
	Quaternion rotation;
	// A tangent direction carried along with the walker, from which yaw is measured. It starts as local
	// north and is parallel-transported every step. Geographic north cannot be used: it does not exist
	// at the poles, and a walker heading "north" stopped dead on reaching one.
	Vector3 reference;
	uint8_t mode = MODE_GROUND;
	// The buttons held last tick, so that holding jump jumps once.
	uint8_t latch = 0;
	// Climbing: the outward normal of the face held, followed as it bends.
	Vector3 normal;
	uint8_t flags = 0; // WalkerFlag bits
	// Climbing: ticks left of a climb jump, a quick lunge along the face.
	uint8_t dash = 0;
};

// planet-demo docs/CHARACTER.md. Three modes: on the ground, in the air, climbing.
//
// The walker knows the world only through ray casts: the first surface met going from one point to
// another. Rays work against every kind of ground -- convex colliders, trimeshes (which have no inside),
// the planet's own collider, and the analytic planet below -- so the same walker climbs a planet, a box
// level made of StaticBody3Ds, or the inside of a hollow sphere. Two sources, either or both, nearest hit
// wins:
//
//  - the physics space (`space`): any colliders, filtered by `collision_mask`, less `exclude` (the
//    player's own body). Not yet on the network: the headless server has no physics world, so online
//    play uses only the analytic ground on both ends, which agree to the bit.
//  - the analytic ground: the planet's field below f, less dug-out spheres, plus shells (a ball, a
//    hollow sphere). Exact and cheap, and what the server and clients share.
//
// Down is toward `gravity_centre` (a planet), or along `uniform_down` when that is set (a level).
struct Walker {
	double speed = 5.0;
	double sprint_speed = 9.0;
	double climb_speed = 2.0;
	double jump_speed = 7.0;
	double gravity = 20.0;
	double air_accel = 6.0;
	// A climb jump: dash_ticks at dash_speed along the face (1.8 m in 0.3 s).
	double dash_speed = 6.0;
	uint8_t dash_ticks = 9;
	// The walk limit as rise over run: tan(45).
	double walk_slope = 1.0;
	// How high a step is walked up rather than climbed, and how far in front of the feet the body
	// meets a wall.
	double step_height = 0.45;
	double body_radius = 0.3;
	// Stand-in for climbable materials and objects: false makes nothing climbable.
	bool can_climb = true;

	// Down.
	Vector3 gravity_centre;
	Vector3 uniform_down; // zero: toward gravity_centre

	// The analytic ground.
	bool use_terrain = true; // the planet's field below f
	double radius = 150000.0; // the planet when there is no field
	Ref<PlanetField3D> field;
	// Dug-out spheres, planet space (planet-demo docs/DIGGING.md). Both ends must hold the same list
	// for predictions to match.
	LocalVector<Vector3> carve_centres;
	LocalVector<double> carve_radii;
	// Shells: solid between an inner and an outer radius about a centre. Inner 0 is a ball. Not cut by
	// carves.
	LocalVector<Vector3> shell_centres;
	LocalVector<double> shell_inner;
	LocalVector<double> shell_outer;

	// The physics space.
	RID space;
	uint32_t collision_mask = UINT32_MAX;
	HashSet<RID> exclude;

	// cos(45) and cos(35): the walk limit and the climb's exit, as the vertical part of a normal.
	static constexpr double COS_WALK = 0.7071067811865476;
	static constexpr double COS_CLIMB_EXIT = 0.8191520442889918;

	Vector3 up_at(const Vector3 &p_point) const {
		if (uniform_down.length_squared() > 0.0) {
			return -uniform_down.normalized();
		}
		const Vector3 d = p_point - gravity_centre;
		return d.length_squared() > 0.0 ? d.normalized() : Vector3(0, 1, 0);
	}

	double field_radius(const Vector3 &p_dir) const {
		return field.is_valid() ? double(field->eval_radius(p_dir)) : radius;
	}

	// The ground as a height along a direction from the planet's centre: f, then, while that point lies
	// inside a carve, the lower point where the radial line leaves the carve's sphere. Not what the
	// walker stands on (see cast()), but what the navigator and the deer do, and the camera's guess.
	double ground_radius(const Vector3 &p_dir) const {
		double r = field_radius(p_dir);
		for (int pass = 0; pass < 16 && !carve_centres.is_empty(); ++pass) {
			bool moved = false;
			for (uint32_t i = 0; i < carve_centres.size(); ++i) {
				const Vector3 &c = carve_centres[i];
				const double rc = carve_radii[i];
				const Vector3 d = p_dir * r - c;
				if (d.length_squared() >= rc * rc) {
					continue;
				}
				// |dir * t - c| = rc: t = (dir . c) -/+ sqrt((dir . c)^2 - |c|^2 + rc^2). The lower root.
				const double b = p_dir.dot(c);
				const double disc = b * b - c.length_squared() + rc * rc;
				if (disc <= 0.0) {
					continue;
				}
				const double t = b - Math::sqrt(disc);
				if (t < r) {
					r = t;
					moved = true;
				}
			}
			if (!moved) {
				break;
			}
		}
		return r;
	}

	Vector3 surface_point(const Vector3 &p_direction) const {
		const Vector3 dir = p_direction.normalized();
		return dir * ground_radius(dir);
	}

	// The analytic solid: ground below f not dug out, or any shell.
	bool inside(const Vector3 &p_point) const {
		for (uint32_t i = 0; i < shell_centres.size(); ++i) {
			const double d2 = (p_point - shell_centres[i]).length_squared();
			if (d2 <= shell_outer[i] * shell_outer[i] && d2 >= shell_inner[i] * shell_inner[i]) {
				return true;
			}
		}
		if (!use_terrain || p_point.length() > field_radius(p_point.normalized())) {
			return false;
		}
		for (uint32_t i = 0; i < carve_centres.size(); ++i) {
			if ((p_point - carve_centres[i]).length_squared() < carve_radii[i] * carve_radii[i]) {
				return false;
			}
		}
		return true;
	}

	bool has_analytic() const {
		return use_terrain || !shell_centres.is_empty();
	}

	// The analytic solid's surface between a point outside it and one inside, to a fraction of a
	// millimetre. Returns the outside end of the final interval, so the result is never inside.
	Vector3 boundary(const Vector3 &p_out, const Vector3 &p_in) const {
		Vector3 o = p_out;
		Vector3 i = p_in;
		for (int k = 0; k < 12; ++k) {
			const Vector3 mid = (o + i) * 0.5;
			if (inside(mid)) {
				i = mid;
			} else {
				o = mid;
			}
		}
		return o;
	}

	// A point on the analytic ground, put exactly on the terrain when that is what it is on: the height
	// the renderer and collider show, to the bit, rather than a bisection's sub-millimetre near it.
	Vector3 settle(const Vector3 &p_on) const {
		if (!use_terrain) {
			return p_on;
		}
		const Vector3 dir = p_on.normalized();
		const Vector3 exact = dir * field_radius(dir);
		// The exact point can round to either side of the solid's own test, so ask instead whether the
		// surface crosses the radial line within 2 mm of it.
		if ((exact - p_on).length_squared() < 1e-4 && !inside(exact + dir * 0.002) && inside(exact - dir * 0.002)) {
			return exact;
		}
		return p_on;
	}

	// The first analytic surface from a (outside) toward b: marched, then bisected. The march step is a
	// sixteenth of the way, between 5 cm and 50 cm, so short probes find thin shells from their near side
	// and long casts stay affordable. False if a starts inside.
	bool analytic_cast(const Vector3 &p_from, const Vector3 &p_to, Vector3 &r_on) const {
		if (!has_analytic() || inside(p_from)) {
			return false;
		}
		const double length = (p_to - p_from).length();
		const double stride = CLAMP(length / 16.0, 0.05, 0.5);
		const int steps = MAX(1, int(Math::ceil(length / stride)));
		Vector3 prev = p_from;
		for (int k = 1; k <= steps; ++k) {
			const Vector3 q = p_from + (p_to - p_from) * (double(k) / steps);
			if (inside(q)) {
				r_on = settle(boundary(prev, q));
				return true;
			}
			prev = q;
		}
		return false;
	}

	bool physics_cast(const Vector3 &p_from, const Vector3 &p_to, Vector3 &r_on, Vector3 *r_normal = nullptr) const {
		if (!space.is_valid() || PhysicsServer3D::get_singleton() == nullptr) {
			return false;
		}
		PhysicsDirectSpaceState3D *state = PhysicsServer3D::get_singleton()->space_get_direct_state(space);
		if (state == nullptr) {
			return false;
		}
		PS3DT::RayParameters params;
		params.exclude = exclude;
		params.collision_mask = collision_mask;
		params.hit_from_inside = false;
		params.hit_back_faces = true;
		PS3DT::RayResult result;
		// A ray exactly along an edge between a trimesh's triangles can slip through between them -- a
		// walker heading straight down a sphere's meridian fell out of it. A miss is tried again nudged
		// 2 mm to either side before it is believed.
		const Vector3 d = p_to - p_from;
		Vector3 side_a;
		Vector3 side_b;
		tangents(d.length_squared() > 0.0 ? d.normalized() : Vector3(0, 1, 0), Vector3(0.267, 0.534, 0.802), side_a, side_b);
		const Vector3 nudges[3] = { Vector3(), side_a * 0.002, side_b * -0.002 };
		for (int k = 0; k < 3; ++k) {
			params.from = p_from + nudges[k];
			params.to = p_to + nudges[k];
			if (state->intersect_ray(params, result)) {
				r_on = result.position - nudges[k];
				if (r_normal != nullptr) {
					// The face as seen by the ray: a trimesh's back face reports the front's normal.
					*r_normal = result.normal.dot(d) > 0.0 ? -result.normal : result.normal;
				}
				return true;
			}
		}
		return false;
	}

	// (tangents() is below; the compiler sees the whole struct.)
	// The first surface met going from a to b: the nearer of the analytic ground and the colliders.
	// r_normal, if asked for, is the face's own normal where a collider was hit, and zero where the
	// analytic ground was (which has none to hand; see surface_normal()).
	bool cast(const Vector3 &p_from, const Vector3 &p_to, Vector3 &r_on, Vector3 *r_normal = nullptr) const {
		Vector3 a;
		Vector3 b;
		Vector3 nb;
		const bool ha = analytic_cast(p_from, p_to, a);
		const bool hb = physics_cast(p_from, p_to, b, &nb);
		const bool use_b = hb && (!ha || b.distance_squared_to(p_from) < a.distance_squared_to(p_from));
		if (use_b) {
			r_on = b;
		} else if (ha) {
			r_on = a;
		}
		if (r_normal != nullptr) {
			*r_normal = use_b ? nb : Vector3();
		}
		return ha || hb;
	}

	// Whether the ground at a point can be climbed. Materials and objects that cannot will answer here.
	bool climbable(const Vector3 &p_point) const {
		(void)p_point;
		return can_climb;
	}

	// Local north, or any tangent where north is undefined. Only used to start a reference.
	static Vector3 local_north(const Vector3 &p_up) {
		Vector3 north = Vector3(0, 1, 0) - p_up * p_up.y;
		if (north.length_squared() < 1e-12) {
			north = Vector3(0, 0, -1) - p_up * p_up.z;
		}
		return north.normalized();
	}

	// The reference moved into the tangent plane at p_up, or a fresh one if there is none.
	static Vector3 transport(const Vector3 &p_reference, const Vector3 &p_up) {
		const Vector3 projected = p_reference - p_up * p_reference.dot(p_up);
		if (projected.length_squared() < 1e-12) {
			return local_north(p_up);
		}
		return projected.normalized();
	}

	// Rise over run from a to b: positive uphill.
	static double slope_between(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_up) {
		const Vector3 d = p_b - p_a;
		const double rise = d.dot(p_up);
		const double run = (d - p_up * rise).length();
		if (run > 1e-9) {
			return rise / run;
		}
		return rise > 0.0 ? 1e9 : (rise < 0.0 ? -1e9 : 0.0);
	}

	// Sticks a point to the surface near it: into the face along -normal, from the nearest clear stand-off
	// (in the gap under a low overhang, far out from the face is already inside something else);
	// failing that, along down (over a convex edge onto the top, or onto the floor at the bottom); failing
	// that, back around a corner the way it came. p_along is the direction it moved.
	bool attach(const Vector3 &p_point, const Vector3 &p_normal, const Vector3 &p_along, Vector3 &r_on, Vector3 *r_normal = nullptr) const {
		const double a = 0.6;
		const Vector3 up = up_at(p_point);
		const Vector3 pairs[10] = {
			p_point + p_normal * 0.15, p_point - p_normal * a,
			p_point + p_normal * 0.03, p_point - p_normal * a,
			p_point + p_normal * a, p_point - p_normal * a,
			p_point + up * a, p_point - up * a,
			p_point - p_normal * a, p_point - p_normal * a - p_along * a,
		};
		for (int k = 0; k < 10; k += 2) {
			if (cast(pairs[k], pairs[k + 1], r_on, r_normal)) {
				return true;
			}
		}
		return false;
	}

	// Two tangents of a surface with this normal: any pair, for probing around a point.
	static void tangents(const Vector3 &p_normal, const Vector3 &p_hint, Vector3 &r_a, Vector3 &r_b) {
		r_a = p_hint - p_normal * p_normal.dot(p_hint);
		if (r_a.length_squared() < 1e-6) {
			r_a = local_north(p_normal);
		}
		r_a = r_a.normalized();
		r_b = p_normal.cross(r_a).normalized();
	}

	// The face's outward normal at a point on it, from four probes 0.25 m around it, each stuck to the
	// surface. Across an edge or a bend the probes land on both sides, so the normal turns smoothly and
	// a climber rolls over the top or round a corner instead of stopping at it. A collider's own normal
	// would not: it jumps at every edge of a box. Oriented to agree with p_normal, the guess.
	Vector3 surface_normal(const Vector3 &p_point, const Vector3 &p_normal) const {
		Vector3 a;
		Vector3 b;
		tangents(p_normal, up_at(p_point), a, b);
		const double h = 0.25;
		const Vector3 offsets[4] = { a, -a, b, -b };
		Vector3 q[4];
		Vector3 summed;
		int reported = 0;
		for (int k = 0; k < 4; ++k) {
			Vector3 face;
			if (!attach(p_point + offsets[k] * h, p_normal, offsets[k], q[k], &face)) {
				q[k] = p_point + offsets[k] * h;
			} else if (face.length_squared() > 0.5) {
				summed += face;
				++reported;
			}
		}
		// Colliders report their faces' normals, outward from where each probe met them: their average
		// blends across an edge just as smoothly, and unlike the probes' positions cannot come out
		// facing inward when a probe round a box's edge misses.
		if (reported >= 2 && summed.length_squared() > 1e-6) {
			return summed.normalized();
		}
		Vector3 n = (q[0] - q[1]).cross(q[2] - q[3]);
		if (n.length_squared() < 1e-12) {
			return p_normal;
		}
		n = n.normalized();
		return n.dot(p_normal) < 0.0 ? -n : n;
	}

	// Stands a walker on the first ground below a point: teleports and spawns. From inside something it
	// looks from 20 km up instead.
	WalkerState place(const WalkerState &p_state, const Vector3 &p_point) const {
		const Vector3 up = up_at(p_point);
		Vector3 on;
		if (!cast(p_point, p_point - up * 20000.0, on) && !cast(p_point + up * 20000.0, p_point - up * 20000.0, on)) {
			on = has_analytic() && use_terrain ? surface_point(p_point) : p_point;
		}
		WalkerState out = p_state;
		out.position = on;
		out.velocity = Vector3();
		out.mode = MODE_GROUND;
		out.normal = Vector3();
		out.flags = 0;
		out.dash = 0;
		out.reference = transport(p_state.reference, up_at(on));
		return out;
	}

	static Quaternion facing_rotation(const Vector3 &p_forward, const Vector3 &p_up) {
		return Basis(p_forward.cross(p_up), p_up, -p_forward).get_rotation_quaternion();
	}

	// Facing a face: into it, upright. On a ceiling or floor, where into it is straight up or down, the
	// fallback.
	static Vector3 face_forward(const Vector3 &p_normal, const Vector3 &p_up, const Vector3 &p_fallback) {
		Vector3 f = -(p_normal - p_up * p_normal.dot(p_up));
		if (f.length_squared() < 0.01) {
			f = p_fallback - p_up * p_fallback.dot(p_up);
		}
		if (f.length_squared() < 1e-12) {
			f = local_north(p_up);
		}
		return f.normalized();
	}

	WalkerState step(const WalkerState &p_state, const WalkerInput &p_input, double p_dt) const {
		const Vector3 up = up_at(p_state.position);
		const Vector3 north = transport(p_state.reference, up);
		const Vector3 east = north.cross(up);
		const double yaw = double(p_input.yaw);
		const Vector3 forward = north * PlanetDetMath::cos(yaw) + east * PlanetDetMath::sin(yaw);
		const Vector3 right = forward.cross(up);
		const double mx = CLAMP(double(p_input.move.x), -1.0, 1.0);
		const double my = CLAMP(double(p_input.move.y), -1.0, 1.0);
		Vector3 wish = forward * my + right * mx;
		if (wish.length_squared() > 1.0) {
			wish = wish.normalized();
		}
		const uint8_t pressed = p_input.buttons & ~p_state.latch;
		const double run_speed = (p_input.buttons & BUTTON_SPRINT) ? sprint_speed : speed;

		WalkerState out = p_state;
		out.latch = p_input.buttons;
		switch (p_state.mode) {
			case MODE_AIR:
				_air(out, p_state, up, wish * run_speed, p_dt);
				break;
			case MODE_CLIMB:
				_climb(out, p_state, forward, right, mx, my, pressed, p_dt);
				break;
			default:
				if (pressed & BUTTON_JUMP) {
					out.mode = MODE_AIR;
					out.velocity = wish * run_speed + up * jump_speed;
					const WalkerState launched = out;
					_air(out, launched, up, wish * run_speed, p_dt);
				} else {
					_ground(out, p_state, up, wish * run_speed, p_dt);
				}
				break;
		}
		out.reference = transport(north, up_at(out.position));
		return out;
	}

private:
	// Grabs whatever is in front of the body -- chest high first, then lower -- not what is at the feet:
	// under an overhang the feet are in the crease where it meets the ground, where the surface has no
	// one direction to climb.
	bool _grab(WalkerState &r_out, const Vector3 &p_feet, const Vector3 &p_up, const Vector3 &p_ahead) const {
		const double heights[3] = { 1.0, 0.6, 0.25 };
		for (int k = 0; k < 3; ++k) {
			const Vector3 from = p_feet + p_up * heights[k];
			Vector3 grip;
			if (cast(from, from + p_ahead * (body_radius + 0.5), grip)) {
				if (!climbable(grip)) {
					return false;
				}
				r_out.mode = MODE_CLIMB;
				r_out.position = grip;
				r_out.velocity = Vector3();
				r_out.normal = surface_normal(grip, -p_ahead);
				r_out.rotation = facing_rotation(p_ahead, p_up);
				return true;
			}
		}
		return false;
	}

	void _ground(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_velocity, double p_dt) const {
		// Still standing? The ground can be dug out from under a walker, or a platform end.
		Vector3 here;
		if (!cast(p_in.position + p_up * 0.3, p_in.position - p_up * 0.3, here)) {
			r_out.mode = MODE_AIR;
			r_out.velocity = Vector3();
			return;
		}
		r_out.position = here;
		r_out.velocity = Vector3();
		if (p_velocity.length_squared() == 0.0) {
			return;
		}
		const Vector3 ahead = (p_velocity - p_up * p_velocity.dot(p_up)).normalized();
		const Vector3 next_feet = here + p_velocity * p_dt;
		// A wall in front, taller than a step: climb it, or be stopped by it.
		const Vector3 knee = here + p_up * step_height;
		Vector3 wall;
		if (cast(knee, next_feet + p_up * step_height + ahead * body_radius, wall)) {
			_grab(r_out, here, p_up, ahead);
			return;
		}
		// The ground under the next step.
		Vector3 next;
		if (!cast(next_feet + p_up * step_height, next_feet - p_up * 0.5, next)) {
			// Nothing within reach below: off the edge.
			r_out.mode = MODE_AIR;
			r_out.velocity = p_velocity;
			r_out.position = next_feet + p_up * 0.01;
			r_out.rotation = facing_rotation(ahead, p_up);
			return;
		}
		double slope = slope_between(here, next, p_up);
		if (Math::abs(slope) > walk_slope && Math::abs((next - here).dot(p_up)) <= step_height &&
				surface_normal(next, p_up).dot(p_up) >= COS_WALK) {
			// A stair or a kerb, up or down: a rise no taller than a step onto ground that is itself level
			// enough to walk. Steep over a step's short run, but walked, not climbed or fallen from.
			slope = 0.0;
		}
		if (slope > walk_slope) {
			// A slope too steep to walk up.
			if (!_grab(r_out, here, p_up, ahead) && climbable(next)) {
				r_out.mode = MODE_CLIMB;
				r_out.position = next;
				r_out.normal = surface_normal(next, -ahead);
				r_out.rotation = facing_rotation(ahead, p_up);
			}
			return;
		}
		if (slope < -walk_slope) {
			// The ground falls away more steeply than a walk: off the edge.
			r_out.mode = MODE_AIR;
			r_out.velocity = p_velocity;
			r_out.position = next_feet + p_up * 0.01;
			r_out.rotation = facing_rotation(ahead, p_up);
			return;
		}
		r_out.position = next;
		r_out.velocity = (next - p_in.position) / p_dt;
		r_out.rotation = facing_rotation(ahead, p_up);
	}

	void _air(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_wish, double p_dt) const {
		const double vr = p_in.velocity.dot(p_up);
		Vector3 vh = p_in.velocity - p_up * vr;
		Vector3 dv = p_wish - vh;
		const double max_dv = air_accel * p_dt;
		if (dv.length_squared() > max_dv * max_dv) {
			dv = dv.normalized() * max_dv;
		}
		vh += dv;
		const Vector3 v = vh + p_up * (vr - gravity * p_dt);
		const Vector3 from = p_in.position;
		const Vector3 to = from + v * p_dt;
		if (vh.length_squared() > 1e-6) {
			r_out.rotation = facing_rotation(vh.normalized(), p_up);
		}
		Vector3 land;
		if (!cast(from, to, land)) {
			r_out.position = to;
			r_out.velocity = v;
			return;
		}
		// Landed this tick, where the path met the ground: a floor, a face, or -- jumping into one -- a
		// ceiling. The surface faces against the way we were going.
		const Vector3 land_up = up_at(land);
		const Vector3 n = surface_normal(land, v.length_squared() > 1e-12 ? -v.normalized() : land_up);
		if (n.dot(land_up) >= COS_WALK) {
			r_out.mode = MODE_GROUND;
			r_out.position = land;
			r_out.velocity = vh;
			r_out.normal = Vector3();
			r_out.flags &= ~FLAG_NO_GRAB;
		} else if (!(p_in.flags & FLAG_NO_GRAB) && climbable(land)) {
			r_out.mode = MODE_CLIMB;
			r_out.position = land;
			r_out.velocity = Vector3();
			r_out.normal = n;
			r_out.rotation = facing_rotation(face_forward(n, land_up, vh), land_up);
		} else {
			// Not holding on: slide down the face, keeping what of the velocity runs along it, just off
			// it so the next tick's path starts outside and can meet it again.
			r_out.mode = MODE_AIR;
			r_out.velocity = v - n * MIN(v.dot(n), 0.0);
			r_out.position = land + n * 0.02;
		}
	}

	// Climbing input. Forward is up the face -- up flattened onto it, whichever way the face leans, so on
	// an overhang's underside it climbs out toward the lip rather than back into the corner. Where up the
	// face stops meaning anything (a ceiling) forward becomes the camera's forward flattened onto it
	// instead, blended in as the face levels out. Right is the camera's right, flattened onto the face.
	static Vector3 climb_direction(const Vector3 &p_normal, const Vector3 &p_up, const Vector3 &p_forward, const Vector3 &p_right, double p_mx, double p_my) {
		const Vector3 u = p_up - p_normal * p_normal.dot(p_up);
		const Vector3 f = p_forward - p_normal * p_normal.dot(p_forward);
		const double ul = u.length();
		const double fl = f.length();
		const double w = CLAMP((ul - 0.2) / 0.3, 0.0, 1.0);
		Vector3 ahead = (ul > 1e-9 ? u / ul : Vector3()) * w + (fl > 1e-9 ? f / fl : Vector3()) * (1.0 - w);
		Vector3 side = p_right - p_normal * p_normal.dot(p_right);
		if (side.length_squared() > 1e-12) {
			side = side.normalized();
		}
		return ahead * p_my + side * p_mx;
	}

	void _climb(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_forward, const Vector3 &p_right, double p_mx, double p_my, uint8_t p_pressed, double p_dt) const {
		const Vector3 here = p_in.position;
		const Vector3 up = up_at(here);
		Vector3 n = p_in.normal;
		if (n.length_squared() < 0.5) {
			n = up;
		}
		r_out.rotation = facing_rotation(face_forward(n, up, p_forward), up);
		if (p_pressed & BUTTON_DROP) {
			r_out.mode = MODE_AIR;
			r_out.flags |= FLAG_NO_GRAB;
			r_out.dash = 0;
			r_out.position = here + n * 0.05;
			r_out.velocity = n;
			return;
		}
		if ((p_pressed & BUTTON_JUMP) && p_my < 0.0) {
			// Jump with back held: leap off.
			r_out.mode = MODE_AIR;
			r_out.dash = 0;
			r_out.position = here + n * 0.05;
			r_out.velocity = up * jump_speed * 0.8 + n * 4.0;
			return;
		}
		if ((p_pressed & BUTTON_JUMP) && p_in.dash == 0) {
			// A climb jump: a lunge along the face, the way the stick points (onward if it points nowhere).
			r_out.dash = dash_ticks;
		}
		double my = p_my;
		double amount = MIN(Vector2(p_mx, p_my).length(), 1.0);
		double rate = climb_speed;
		if (r_out.dash > 0) {
			--r_out.dash;
			rate = dash_speed;
			if (amount < 1e-9) {
				my = 1.0;
			}
			amount = 1.0;
		}
		r_out.position = here;
		r_out.velocity = Vector3();
		Vector3 dir = climb_direction(n, up, p_forward, p_right, p_mx, my);
		if (amount < 1e-9 || dir.length_squared() < 1e-6) {
			return;
		}
		dir = dir.normalized();
		// Along the face, then back onto it -- in pieces no longer than a probe's reach is safe for.
		const double total = rate * amount * p_dt;
		const int pieces = int(Math::ceil(total / 0.1));
		Vector3 at = here;
		for (int k = 0; k < pieces; ++k) {
			const Vector3 target = at + dir * (total / pieces);
			Vector3 on;
			// Something across the way -- an inside corner, a slab's underside above a wall -- is the next
			// face: hold that. Checked along the path itself, just off the face, because a probe from a
			// point already inside a collider does not see that collider at all.
			Vector3 hit_normal;
			if (cast(at + n * 0.02, target + n * 0.02, on, &hit_normal)) {
				// A collider says which way its face faces: take it -- at an inside corner the probes
				// straddle both faces and cannot. On the analytic ground, estimate it, guessing back along
				// the way we came and out from the old face: right for an inside corner and for the same
				// face curving into the path, which the old normal alone or -dir alone each gets wrong for
				// one of them.
				n = hit_normal.length_squared() > 0.5 ? hit_normal.normalized() : surface_normal(on, (n - dir).normalized());
				at = on;
				break;
			}
			if (!attach(target, n, dir, on)) {
				break; // nowhere to hold: stay where the last piece left us
			}
			n = surface_normal(on, n);
			at = on;
			const Vector3 next_dir = climb_direction(n, up_at(at), p_forward, p_right, p_mx, my);
			if (next_dir.length_squared() < 1e-6) {
				break;
			}
			dir = next_dir.normalized();
		}
		const Vector3 at_up = up_at(at);
		r_out.position = at;
		r_out.velocity = (at - p_in.position) / p_dt;
		r_out.normal = n;
		r_out.rotation = facing_rotation(face_forward(n, at_up, p_forward), at_up);
		// Over the top, or down at the bottom: walking again once the ground is gentle.
		if (n.dot(at_up) >= COS_CLIMB_EXIT) {
			r_out.mode = MODE_GROUND;
			r_out.normal = Vector3();
			r_out.dash = 0;
			// Over a top, mantle: step a body's width onto it, away from the wall just climbed, rather
			// than stand on the very lip.
			Vector3 wall = p_in.normal - at_up * p_in.normal.dot(at_up);
			if (wall.length_squared() > 0.25 && my > 0.0) {
				wall = wall.normalized();
				const Vector3 onto = at - wall * body_radius;
				Vector3 top;
				if (cast(onto + at_up * 0.3, onto - at_up * 0.5, top) && slope_between(at, top, at_up) > -walk_slope) {
					r_out.position = top;
				}
			}
		}
	}
};

} // namespace PlanetNet
