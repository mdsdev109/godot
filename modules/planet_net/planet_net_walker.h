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
#include "core/templates/local_vector.h"
#include "scene/resources/3d/planet_field_3d.h"

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
	// Climbing: the horizontal direction into the face, i.e. up it. Followed as the face curves.
	Vector3 facing;
	uint8_t flags = 0; // WalkerFlag bits
	// Climbing: ticks left of a climb jump, a quick lunge along the face.
	uint8_t dash = 0;
};

// planet-demo docs/CHARACTER.md. Three modes: on the ground, in the air, climbing.
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
	// Slopes as rise over run. Not degrees: a tangent here would be one more transcendental to keep
	// identical across machines, for nothing. tan(45) and tan(35).
	double walk_slope = 1.0;
	double climb_exit_slope = 0.7002075382097097;
	// Stand-in for climbable materials and objects: false makes nothing climbable.
	bool can_climb = true;
	double radius = 150000.0; // used when there is no field
	Ref<PlanetField3D> field;
	// Dug-out spheres, planet space (planet-demo docs/DIGGING.md): the walker stands on the ground left
	// after them, not on f. Both ends must hold the same list for predictions to match.
	LocalVector<Vector3> carve_centres;
	LocalVector<double> carve_radii;

	// The ground along a direction: f, then, while that point lies inside a carve, the lower point where
	// the radial line leaves the carve's sphere -- the floor of the hole. Spheres only, so it is exact
	// and plain arithmetic: the same bits on every machine. It is the analytic twin of the site's voxels,
	// which approximate the same surface to within their mesher's error.
	double ground_radius(const Vector3 &p_dir) const {
		double r = field.is_valid() ? double(field->eval_radius(p_dir)) : radius;
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

	// Whether the ground in this direction can be climbed. Materials and objects that cannot will
	// answer here.
	bool climbable(const Vector3 &p_dir) const {
		(void)p_dir;
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

	// Rise over run from a to b: positive uphill. The run is measured in a's tangent plane.
	static double slope_between(const Vector3 &p_a, const Vector3 &p_b) {
		const Vector3 up = p_a.normalized();
		const Vector3 d = p_b - p_a;
		const double rise = p_b.length() - p_a.length();
		const double run = (d - up * d.dot(up)).length();
		if (run > 1e-9) {
			return rise / run;
		}
		return rise > 0.0 ? 1e9 : (rise < 0.0 ? -1e9 : 0.0);
	}

	// The ground's uphill gradient at a point, in its tangent plane: its direction is straight up the
	// slope, its length the slope (rise over run). Central differences 0.25 m either side.
	Vector3 gradient(const Vector3 &p_position, const Vector3 &p_north) const {
		const Vector3 up = p_position.normalized();
		const Vector3 north = transport(p_north, up);
		const Vector3 east = north.cross(up);
		const Vector3 base = up * ground_radius(up);
		const double h = 0.25;
		const double rn = ground_radius((base + north * h).normalized());
		const double rs = ground_radius((base - north * h).normalized());
		const double re = ground_radius((base + east * h).normalized());
		const double rw = ground_radius((base - east * h).normalized());
		return north * ((rn - rs) / (2.0 * h)) + east * ((re - rw) / (2.0 * h));
	}

	// The ground point reached by going p_distance along the surface -- measured in 3D, so up a wall it
	// is mostly height -- in a horizontal direction. Bisects how far to go sideways.
	Vector3 along_surface(const Vector3 &p_from, const Vector3 &p_direction, double p_distance) const {
		const Vector3 whole = surface_point(p_from + p_direction * p_distance);
		if (whole.distance_to(p_from) <= p_distance) {
			return whole;
		}
		double lo = 0.0;
		double hi = p_distance;
		for (int i = 0; i < 14; ++i) {
			const double mid = 0.5 * (lo + hi);
			if (surface_point(p_from + p_direction * mid).distance_to(p_from) > p_distance) {
				hi = mid;
			} else {
				lo = mid;
			}
		}
		return surface_point(p_from + p_direction * lo);
	}

	// Puts a walker on the ground at a point, standing: teleports and spawns.
	WalkerState place(const WalkerState &p_state, const Vector3 &p_point) const {
		WalkerState out = p_state;
		out.position = surface_point(p_point);
		out.velocity = Vector3();
		out.mode = MODE_GROUND;
		out.facing = Vector3();
		out.flags = 0;
		out.dash = 0;
		out.reference = transport(p_state.reference, out.position.normalized());
		return out;
	}

	static Quaternion facing_rotation(const Vector3 &p_forward, const Vector3 &p_up) {
		return Basis(p_forward.cross(p_up), p_up, -p_forward).get_rotation_quaternion();
	}

	WalkerState step(const WalkerState &p_state, const WalkerInput &p_input, double p_dt) const {
		const Vector3 up = p_state.position.normalized();
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
				_climb(out, p_state, up, north, mx, my, pressed, p_dt);
				break;
			default:
				if (pressed & BUTTON_JUMP) {
					out.mode = MODE_AIR;
					out.velocity = wish * run_speed + up * jump_speed;
					const WalkerState launched = out;
					_air(out, launched, up, wish * run_speed, p_dt);
				} else {
					_ground(out, p_state, up, north, wish * run_speed, p_dt);
				}
				break;
		}
		out.reference = transport(north, out.position.normalized());
		return out;
	}

private:
	void _ground(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_north, const Vector3 &p_velocity, double p_dt) const {
		const Vector3 here = surface_point(p_in.position);
		if (p_velocity.length_squared() == 0.0) {
			r_out.position = here;
			r_out.velocity = Vector3();
			return;
		}
		const Vector3 next = surface_point(p_in.position + p_velocity * p_dt);
		const double slope = slope_between(here, next);
		if (slope > walk_slope) {
			// Too steep to walk up: climb it, or be stopped by it.
			r_out.position = here;
			r_out.velocity = Vector3();
			if (climbable(next.normalized())) {
				r_out.mode = MODE_CLIMB;
				const Vector3 g = gradient(next, p_north);
				const Vector3 into = g.length_squared() > 1e-6 ? g : p_velocity;
				r_out.facing = into.normalized();
				r_out.rotation = facing_rotation(r_out.facing, p_up);
			}
			return;
		}
		if (slope < -walk_slope) {
			// The ground falls away more steeply than a walk: off the edge.
			r_out.mode = MODE_AIR;
			r_out.velocity = p_velocity;
			r_out.position = here + p_velocity * p_dt;
			r_out.rotation = facing_rotation(p_velocity.normalized(), p_up);
			return;
		}
		r_out.position = next;
		r_out.velocity = (next - p_in.position) / p_dt;
		r_out.rotation = facing_rotation(p_velocity.normalized(), p_up);
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
		if (to.length() > ground_radius(to.normalized())) {
			r_out.position = to;
			r_out.velocity = v;
			return;
		}
		// Landed this tick. Find where the path met the ground, so that flying into a cliff stops on its
		// face rather than on top of it.
		double lo = 0.0;
		double hi = 1.0;
		if (from.length() <= ground_radius(from.normalized())) {
			hi = 0.0;
		}
		for (int i = 0; i < 12 && hi > 0.0; ++i) {
			const double mid = 0.5 * (lo + hi);
			const Vector3 p = from + (to - from) * mid;
			if (p.length() > ground_radius(p.normalized())) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		const Vector3 land = surface_point(from + (to - from) * lo);
		const Vector3 land_up = land.normalized();
		const Vector3 g = gradient(land, transport(p_in.reference, land_up));
		r_out.position = land;
		const bool steep = g.length_squared() > walk_slope * walk_slope;
		if (!steep) {
			r_out.mode = MODE_GROUND;
			r_out.velocity = vh;
			r_out.flags &= ~FLAG_NO_GRAB;
		} else if (!(p_in.flags & FLAG_NO_GRAB) && climbable(land_up)) {
			r_out.mode = MODE_CLIMB;
			r_out.velocity = Vector3();
			r_out.facing = g.normalized();
			r_out.rotation = facing_rotation(r_out.facing, land_up);
		} else {
			// Not holding on: slide down the face. Keep falling, with what of the velocity runs along it.
			const Vector3 n = (land_up - g).normalized();
			r_out.mode = MODE_AIR;
			r_out.velocity = v - n * MIN(v.dot(n), 0.0);
			// Just off the face, so the next tick's path starts above the ground and can meet it again.
			r_out.position = land + n * 0.02;
		}
	}

	void _climb(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_north, double p_mx, double p_my, uint8_t p_pressed, double p_dt) const {
		const Vector3 here = surface_point(p_in.position);
		// Follow the face round as it curves; keep the old direction where it is too flat to say.
		const Vector3 g = gradient(here, p_north);
		Vector3 into = transport(p_in.facing, p_up);
		if (g.length_squared() > 0.04) {
			into = g.normalized();
		}
		const Vector3 side = into.cross(p_up);
		r_out.facing = into;
		r_out.rotation = facing_rotation(into, p_up);
		if (p_pressed & BUTTON_DROP) {
			r_out.mode = MODE_AIR;
			r_out.flags |= FLAG_NO_GRAB;
			r_out.dash = 0;
			r_out.position = here - into * 0.05;
			r_out.velocity = -into;
			return;
		}
		if ((p_pressed & BUTTON_JUMP) && p_my < 0.0) {
			// Jump with back held: leap off the wall.
			r_out.mode = MODE_AIR;
			r_out.dash = 0;
			r_out.position = here;
			r_out.velocity = p_up * jump_speed * 0.8 - into * 4.0;
			return;
		}
		if ((p_pressed & BUTTON_JUMP) && p_in.dash == 0) {
			// A climb jump: a lunge along the face, the way the stick points (up if it points nowhere).
			r_out.dash = dash_ticks;
		}
		Vector3 dir = into * p_my + side * p_mx;
		double amount = MIN(dir.length(), 1.0);
		double rate = climb_speed;
		if (r_out.dash > 0) {
			--r_out.dash;
			rate = dash_speed;
			if (amount < 1e-9) {
				dir = into;
			}
			amount = 1.0;
		}
		if (amount < 1e-9) {
			r_out.position = here;
			r_out.velocity = Vector3();
			return;
		}
		dir = dir.normalized();
		const Vector3 next = along_surface(here, dir, rate * amount * p_dt);
		r_out.position = next;
		r_out.velocity = (next - p_in.position) / p_dt;
		// Over the top, or down at the bottom: walking again once the ground is gentle.
		const Vector3 g_next = gradient(next, p_north);
		if (g_next.length_squared() < climb_exit_slope * climb_exit_slope) {
			r_out.mode = MODE_GROUND;
			r_out.facing = Vector3();
			r_out.dash = 0;
		}
	}
};

} // namespace PlanetNet
