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
#include "planet_net_moves.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "scene/resources/3d/planet_field_3d.h"
#include "servers/physics_3d/direct_states/physics_direct_space_state_3d.h"
#include "servers/physics_3d/physics_server_3d.h"

namespace PlanetNet {

enum WalkerMode : uint8_t {
	MODE_GROUND = 0,
	MODE_AIR = 1,
	MODE_CLIMB = 2,
	MODE_GLIDE = 3, // under the glider: a capped sink, steered
};

enum WalkerFlag : uint8_t {
	// Let go of a wall: slide down faces rather than grab them, until standing on walkable ground.
	FLAG_NO_GRAB = 1,
	// Out of stamina: no sprint, no glider, no holding a wall, until it has refilled completely.
	FLAG_EXHAUSTED = 2,
	// Never climbs: walls stop it, as an unclimbable face stops anyone (an NPC: docs/COMBAT.md, "The
	// bandit"). Kept, unlike the others: whoever makes the walker sets it.
	FLAG_NO_CLIMB = 4,
};

enum WalkerButton : uint8_t {
	BUTTON_JUMP = 1,
	BUTTON_SPRINT = 2,
	BUTTON_DROP = 4, // let go of a wall
	BUTTON_DODGE = 8,
	BUTTON_LIGHT = 16, // a light attack; again late in one, the next of the combo
	BUTTON_HEAVY = 32, // a heavy attack
	BUTTON_BLOCK = 64, // held: guard
	BUTTON_LOCK = 128, // held: face the way of `yaw` (a locked-on target) rather than the way it moves
};

// What the walker is doing on top of its mode is a move: WalkerAction, in planet_net_moves.h.

struct WalkerInput {
	Vector2 move; // (right, forward), each in [-1, 1]
	float yaw = 0.f; // heading in radians from the walker's reference direction
	uint8_t buttons = 0; // WalkerButton bits held this tick
	// The ability slots held (bit k: slot k, 0..5), and where an aimed ability goes: an offset from the
	// walker's position (zero: straight ahead).
	uint8_t abilities = 0;
	Vector3 aim;
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
	// Stamina, in whole units (Walker::stamina_max is a full bar): an integer count, so both ends hold
	// the same. Sprinting, climbing, a climb jump, a dodge and the glider spend it.
	uint16_t stamina = 1000;
	// Ticks before stamina starts to refill, counted down from the last time it was spent.
	uint8_t rest = 0;
	uint8_t action = ACTION_NONE; // the move (WalkerAction, planet_net_moves.h)
	uint8_t action_ticks = 0; // ticks into it
	// Where a move aimed at the ground lands (SHAPE_AREA), set when it starts.
	Vector3 aim;
	// The input buffer: a press that could not start its move yet (an Intent), kept for buffer_ticks
	// more ticks, and started on the first of them that allows it.
	uint8_t buffer = 0;
	uint8_t buffer_ticks = 0;
	// Poise (planet-demo docs/COMBAT_DESIGN.md section 3): what a hit's poise damage comes off. At 0 the
	// hit's reaction lands and it is full again; it refills after poise_rest ticks without a hit.
	uint16_t poise = 60;
	uint8_t poise_rest = 0;
	// Abilities (COMBAT_DESIGN.md section 4): focus, what they spend besides stamina; each slot's move and
	// the ticks before it may start it again; the ability buttons held last tick.
	uint16_t focus = 1000;
	uint8_t loadout[ABILITY_SLOTS] = { ACTION_EMBER, ACTION_GUST, ACTION_SLAM, 0, 0, 0 };
	uint16_t cooldown[ABILITY_SLOTS] = {};
	uint8_t ability_latch = 0;
};

// The walker's state on the wire beyond position and velocity, in one place for the server's writer
// and the client's reader (W and R: PlanetNet::Writer and Reader), so a new member cannot be written by
// one end and missed by the other. Doubles go whole: a prediction must match to the bit.
template <typename W>
inline void write_walk(W &w, const WalkerState &s) {
	w.f64(double(s.reference.x));
	w.f64(double(s.reference.y));
	w.f64(double(s.reference.z));
	w.u8(s.mode);
	w.u8(s.latch);
	w.f64(double(s.normal.x));
	w.f64(double(s.normal.y));
	w.f64(double(s.normal.z));
	w.u8(s.flags);
	w.u8(s.dash);
	w.u16(s.stamina);
	w.u8(s.rest);
	w.u8(s.action);
	w.u8(s.action_ticks);
	w.f64(double(s.aim.x));
	w.f64(double(s.aim.y));
	w.f64(double(s.aim.z));
	w.u8(s.buffer);
	w.u8(s.buffer_ticks);
	w.u16(s.poise);
	w.u8(s.poise_rest);
	w.u16(s.focus);
	for (int k = 0; k < ABILITY_SLOTS; ++k) {
		w.u8(s.loadout[k]);
		w.u16(s.cooldown[k]);
	}
	w.u8(s.ability_latch);
}

template <typename R>
inline void read_walk(R &r, WalkerState &s) {
	s.reference.x = r.f64();
	s.reference.y = r.f64();
	s.reference.z = r.f64();
	s.mode = r.u8();
	s.latch = r.u8();
	s.normal.x = r.f64();
	s.normal.y = r.f64();
	s.normal.z = r.f64();
	s.flags = r.u8();
	s.dash = r.u8();
	s.stamina = r.u16();
	s.rest = r.u8();
	s.action = r.u8();
	s.action_ticks = r.u8();
	s.aim.x = r.f64();
	s.aim.y = r.f64();
	s.aim.z = r.f64();
	s.buffer = r.u8();
	s.buffer_ticks = r.u8();
	s.poise = r.u16();
	s.poise_rest = r.u8();
	s.focus = r.u16();
	for (int k = 0; k < ABILITY_SLOTS; ++k) {
		s.loadout[k] = r.u8();
		s.cooldown[k] = r.u16();
	}
	s.ability_latch = r.u8();
}

// The combat part of a walker state, for scripts (PlanetCharacter, PlanetNetClient).
inline Dictionary combat_of(const WalkerState &p_state) {
	Dictionary d;
	d["poise"] = int(p_state.poise);
	d["focus"] = int(p_state.focus);
	Array loadout;
	Array cooldowns;
	for (int k = 0; k < ABILITY_SLOTS; ++k) {
		loadout.push_back(int(p_state.loadout[k]));
		cooldowns.push_back(int(p_state.cooldown[k]));
	}
	d["loadout"] = loadout;
	d["cooldowns"] = cooldowns;
	d["aim"] = p_state.aim;
	return d;
}

// Whether two walker states are the same to the bit: everything a step reads. What a prediction must
// equal (planet_net_client.cpp).
inline bool same_walk(const WalkerState &a, const WalkerState &b) {
	return a.position == b.position && a.velocity == b.velocity && a.reference == b.reference &&
			a.mode == b.mode && a.normal == b.normal && a.flags == b.flags && a.dash == b.dash &&
			a.stamina == b.stamina && a.rest == b.rest && a.action == b.action && a.action_ticks == b.action_ticks &&
			a.aim == b.aim && a.buffer == b.buffer && a.buffer_ticks == b.buffer_ticks && a.poise == b.poise &&
			a.poise_rest == b.poise_rest && a.focus == b.focus && a.ability_latch == b.ability_latch &&
			std::memcmp(a.loadout, b.loadout, sizeof(a.loadout)) == 0 && std::memcmp(a.cooldown, b.cooldown, sizeof(a.cooldown)) == 0;
}

// What the body is doing, for drawing it on other machines: the mode in the low four bits, the move
// above them. Replicated with every player (EntityState::pose).
inline uint16_t pose_of(const WalkerState &p_state) {
	return uint16_t((p_state.mode & 15u) | (uint16_t(p_state.action) << 4));
}

// planet-demo docs/CHARACTER.md. Four modes: on the ground, in the air, climbing, gliding.
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
	// Breath of the Wild's (planet-demo docs/CHARACTER.md): a run of 5.5 m/s, a sprint of 8, climbing
	// at 0.7 -- unboosted.
	double speed = 5.5;
	double sprint_speed = 8.0;
	double climb_speed = 0.7;
	double jump_speed = 7.0;
	double gravity = 20.0;
	double air_accel = 6.0;
	// A climb jump: a wind-up of dash_windup ticks holding still -- the body loading, as a climber's
	// does before a dyno -- then dash_ticks at dash_speed along the face (1.8 m in 0.3 s).
	double dash_speed = 6.0;
	uint8_t dash_ticks = 9;
	uint8_t dash_windup = 4;
	// The walk limit as rise over run: tan(45).
	double walk_slope = 1.0;
	// How high a step is walked up rather than climbed, and how far in front of the feet the body
	// meets a wall.
	double step_height = 0.45;
	double body_radius = 0.3;
	// Stand-in for climbable materials and objects: false makes nothing climbable.
	bool can_climb = true;

	// Movement feel (planet-demo docs/CHARACTER.md, milestone 4). On the ground the velocity eases
	// toward the stick's instead of taking it in one tick: quick enough to feel direct, slow enough to
	// have weight -- 0 to a run in an eighth of a second, a stop in a twelfth.
	double ground_accel = 40.0; // m/s^2 toward the wished velocity
	double ground_brake = 60.0; // m/s^2 when the stick is let go or turned against the way it goes

	// Stamina, per tick at 30 Hz. A full bar: 8 s of sprint, 11 s climbing, 33 s under the glider.
	uint16_t stamina_max = 1000;
	uint16_t sprint_cost = 4;
	uint16_t climb_cost = 3; // moving on a wall; holding still costs nothing, as in BotW
	uint16_t climb_jump_cost = 120;
	uint16_t glide_cost = 1;
	uint16_t stamina_regen = 12; // once rested: empty to full in under 3 s
	uint8_t stamina_rest_ticks = 30; // a second after spending before it refills

	// The glider: opened by jump in the air once past the top of a jump; a capped sink, steered.
	double glide_sink = 2.2; // m/s down at most
	double glide_brake = 25.0; // m/s^2 slowing a faster fall to the sink
	double glide_speed = 9.0; // horizontal, at full stick
	double glide_accel = 5.0;
	double glide_open_rise = 2.0; // rising faster than this, jump does not open it

	// Combat (planet-demo docs/COMBAT.md, COMBAT_DESIGN.md): every attack, ability, dodge and hit
	// reaction is a row of the move table (planet_net_moves.h), played by _run_move.
	double block_arc = 100.0; // degrees either side of the facing the guard covers
	double recoil_brake = 10.0; // m/s^2 slowing a knock-back
	// How many ticks a press waits for a move to allow it (a cancel window opening, a reaction ending).
	// 0: a press that cannot start at once is dropped.
	uint8_t buffer_ticks = 9;
	// Poise: a light's poise damage is 35 and a heavy's 70, so the second light in a row breaks it and a
	// heavy always does, unless armour takes some off. It refills after 2 s without a hit, in 1 s.
	uint16_t poise_max = 60;
	uint8_t poise_rest_ticks = 60;
	uint16_t poise_regen = 2;
	// Focus: a full bar 1000, back from empty in a little over 8 s.
	uint16_t focus_max = 1000;
	uint16_t focus_regen = 4;
	// An aimed ability with no aim lands this far ahead (or at its range, if shorter).
	double aim_ahead = 6.0;
	// Line of sight (sees()). An aimed ability is cast only where the caster sees the area it makes: from
	// its eye, eye_height over its feet, to cast_lift over the point -- not the ground itself, so a ledge's
	// top seen from below can be thrown onto, and a wall's far side cannot. A blow lands only on a body
	// whose middle, body_middle over its feet, is in sight of where the blow comes from -- an area's point,
	// aim_lift up, or the attacker's own middle.
	double eye_height = 1.5;
	double cast_lift = 1.0;
	double aim_lift = 0.3;
	double body_middle = 0.9;
	// How near its end a sight line may meet something and still see: the line to a point just over the
	// ground skims it, and a bump just short of the point is no wall.
	double sight_slack = 0.75;

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
	// Other bodies -- players, a training dummy -- that this walker cannot walk through: where each
	// stands (its feet) and its radius, set before each step by whoever steps it (the server from its
	// entities, a client from the newest it has of them, a game offline from its own). An upright
	// cylinder BODY_HEIGHT tall round each.
	LocalVector<Vector3> body_feet;
	LocalVector<double> body_radii;
	static constexpr double BODY_HEIGHT = 1.8;
	static constexpr double CLIMB_STEP = 0.3; // m: what stands out of a face no more than this, a climber steps onto
	static constexpr double UNDERSIDE_WALL_REACH = 0.6; // a wall this near behind, on an underside, is the one climbed
	// ... and that wall stands near upright (its normal this level): an inside corner, not the next facet
	// of a curve -- inside a hollow sphere the ceiling's own facets turned a climber back down.
	static constexpr double UNDERSIDE_WALL_LEVEL = 0.3;
	static constexpr double BODY_SEPARATE = 2.0; // m/s at most, pushing apart bodies that overlap
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

	// Whether nothing stands between two points -- the analytic ground, a collider -- but within `p_slack`
	// of the second: a body's own collider, the ground just under a point lifted off it.
	bool sees(const Vector3 &p_from, const Vector3 &p_to, double p_slack = 0.0) const {
		Vector3 hit;
		if (!cast(p_from, p_to, hit)) {
			return true;
		}
		return hit.distance_to(p_to) <= p_slack;
	}

	// Whether a caster standing at `p_feet` sees `p_point` to cast an aimed ability at: from its eye to just
	// over the point.
	bool can_cast_from(const Vector3 &p_feet, const Vector3 &p_point) const {
		return sees(p_feet + up_at(p_feet) * eye_height, p_point + up_at(p_point) * cast_lift, sight_slack);
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
		bool inside_corner = false;
		for (int k = 0; k < 4; ++k) {
			Vector3 face;
			// A probe whose way out along the face is blocked -- a wall or a ceiling at an inside corner, a
			// neighbouring block -- would start inside that and find only a face buried in it (a slab's end
			// against the pillar it juts from, facing into the pillar), or the wall it ran into: neither is
			// this face going on. Left out. Only for a collider, which says which way its face faces: the
			// analytic ground, rough at this scale, blocked probes along a plain cliff and the pit's walls
			// lost their normal.
			Vector3 blocked;
			Vector3 blocked_normal;
			if (cast(p_point + p_normal * 0.02, p_point + p_normal * 0.02 + offsets[k] * h, blocked, &blocked_normal) &&
					blocked_normal.length_squared() > 0.5) {
				q[k] = p_point + offsets[k] * h;
				inside_corner = true;
				continue;
			}
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
		// One is enough when another was left out: the probes' positions then are not all on the surface,
		// and their cross gives back the blend that left a climber under a ceiling beside a wall holding a
		// face 72 degrees down between the two, put back each tick into the corner it was leaving.
		if ((reported >= 2 || (reported >= 1 && inside_corner)) && summed.length_squared() > 1e-6) {
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
		out.flags = p_state.flags & FLAG_NO_CLIMB; // what the walker is, not what it was doing
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

	// The move the walker is in.
	static const Move &move_of(const WalkerState &p_state) { return moves().get(p_state.action); }

	// Whether hits pass through this walker this tick: a move's untouchable window (the dodge's).
	bool invulnerable(const WalkerState &p_state) const {
		if (p_state.action == ACTION_DEATH) {
			return true;
		}
		const Move &m = move_of(p_state);
		return p_state.action_ticks >= m.invulnerable_from && p_state.action_ticks < m.invulnerable_to;
	}

	// Whether this tick's move can hit: one that hits, inside its window.
	bool hitting(const WalkerState &p_state) const {
		const Move &m = move_of(p_state);
		return m.hits() && p_state.action_ticks >= m.hit_from && p_state.action_ticks < m.hit_to;
	}

	// Whether the move in `p_state` reaches a body standing at `p_feet` with radius `p_radius`, by its hit's
	// shape: within reach in the arc in front, within its radius all round, or within its radius of the
	// aim point -- and the body's feet within its height, and its middle in sight of where the blow comes
	// from (a rock landing on the far side of a wall does not hit a body on this side, nor a sword through
	// it). Judged by the server (and offline the game), never predicted.
	bool swing_reaches(const WalkerState &p_state, const Vector3 &p_feet, double p_radius) const {
		const Move &m = move_of(p_state);
		if (!m.hits()) {
			return false;
		}
		const Vector3 up = up_at(p_state.position);
		const Vector3 centre = m.shape == SHAPE_AREA ? p_state.aim : p_state.position;
		const Vector3 d = p_feet - centre;
		const double dv = d.dot(up);
		if (Math::abs(dv) > m.height) {
			return false;
		}
		const Vector3 dh = d - up * dv;
		const double dist = dh.length();
		bool within = false;
		if (m.shape == SHAPE_AROUND || m.shape == SHAPE_AREA) {
			within = dist <= m.radius + p_radius;
		} else if (dist <= m.reach + p_radius) {
			within = dist <= p_radius + body_radius // touching
					|| _facing(p_state, up).dot(dh / dist) >= Math::cos(Math::deg_to_rad(m.arc));
		}
		if (!within) {
			return false;
		}
		const Vector3 from = m.shape == SHAPE_AREA ? p_state.aim + up * aim_lift : p_state.position + up * body_middle;
		return sees(from, p_feet + up_at(p_feet) * body_middle, p_radius + 0.3);
	}

	// Where an aimed move lands, as an offset from the caster: `p_aim` kept within the move's range (its
	// flat part, `r_flat`); with none, straight along `p_face`.
	Vector3 _landing(const Move &p_move, const Vector3 &p_aim, const Vector3 &p_up, const Vector3 &p_face, Vector3 &r_flat) const {
		Vector3 off = p_aim;
		const double rise = off.dot(p_up);
		r_flat = off - p_up * rise;
		const double far = r_flat.length();
		if (far < 0.01) {
			r_flat = p_face * MIN(aim_ahead, p_move.range);
			off = r_flat;
		} else if (far > p_move.range) {
			off = r_flat * (p_move.range / far) + p_up * rise;
			r_flat = r_flat * (p_move.range / far);
		}
		return off;
	}

	// Whether a guard held in `p_state` faces a blow from `p_from`.
	bool guards(const WalkerState &p_state, const Vector3 &p_from) const {
		if (p_state.action != ACTION_BLOCK || p_state.mode != MODE_GROUND) {
			return false;
		}
		const Vector3 up = up_at(p_state.position);
		Vector3 d = p_from - p_state.position;
		d -= up * d.dot(up);
		if (d.length_squared() < 1e-12) {
			return true;
		}
		return _facing(p_state, up).dot(d.normalized()) >= Math::cos(Math::deg_to_rad(block_arc));
	}

	// A hit landing: knocked back along `p_push` (m/s, flattened onto the ground) into a reaction move
	// (ACTION_STAGGER, ACTION_BLOCKED on the guard, ...). On a wall, knocked off it -- as a rock knocks a
	// climber off in Breath of the Wild -- falling, not grabbing a face again until on its feet. In the air
	// only the push.
	WalkerState recoil(const WalkerState &p_state, const Vector3 &p_push, uint8_t p_reaction) const {
		WalkerState out = p_state;
		const Vector3 up = up_at(p_state.position);
		const Vector3 push = p_push - up * p_push.dot(up);
		if (p_state.mode == MODE_CLIMB) {
			// Off the face and away from it: of the push only what goes away from the wall or along it.
			// Its part into the wall (a blow from below, its attacker in front of the face) cancelled the
			// way out and once put the climber through the wall.
			const Vector3 n = p_state.normal;
			const Vector3 along = p_push - n * p_push.dot(n);
			out.mode = MODE_AIR;
			out.flags |= FLAG_NO_GRAB;
			out.position = p_state.position + n * 0.1;
			out.velocity = along + n * MAX(2.5, p_push.dot(n));
			out.action = ACTION_NONE;
			out.action_ticks = 0;
			out.dash = 0;
			return out;
		}
		if (p_state.mode != MODE_GROUND) {
			out.velocity += push;
			return out;
		}
		out.action = p_reaction;
		out.action_ticks = 1;
		out.velocity = push;
		return out;
	}

	// The reaction move for a Reaction.
	static uint8_t reaction_move(uint8_t p_reaction) {
		switch (p_reaction) {
			case REACT_FLINCH:
				return ACTION_FLINCH;
			case REACT_STAGGER:
				return ACTION_STAGGER;
			case REACT_KNOCKDOWN:
				return ACTION_KNOCKDOWN;
			default:
				return ACTION_NONE;
		}
	}

	// A hit by `p_attack` landing (not guarded, not dodged): its poise damage, less the armour of the move
	// the walker is in, comes off its poise. Broken -- poise at 0 -- the hit's reaction lands and poise is
	// full again; a fragile move (a cast) is interrupted by any hit, a flinch at least. Otherwise the walker
	// takes it and carries on: a trade. `r_reaction` is the reaction move that landed, or ACTION_NONE.
	// Judged by the server (and offline the game), never predicted: the client is corrected to it.
	WalkerState take_hit(const WalkerState &p_state, const Move &p_attack, const Vector3 &p_away, uint8_t &r_reaction) const {
		WalkerState out = p_state;
		const Move &now = move_of(p_state);
		uint16_t damage = p_attack.poise_damage;
		if (now.armour > 0 && p_state.action_ticks >= now.armour_from && p_state.action_ticks < now.armour_to) {
			damage = damage > now.armour ? uint16_t(damage - now.armour) : uint16_t(0);
		}
		out.poise = damage >= out.poise ? uint16_t(0) : uint16_t(out.poise - damage);
		out.poise_rest = poise_rest_ticks;
		uint8_t reaction = REACT_NONE;
		if (out.poise == 0) {
			reaction = p_attack.reaction;
			out.poise = poise_max;
		}
		if (now.fragile && reaction == REACT_NONE) {
			reaction = REACT_FLINCH;
		}
		r_reaction = reaction_move(reaction);
		if (r_reaction != ACTION_NONE) {
			out = recoil(out, p_away * p_attack.push, r_reaction);
		}
		return out;
	}

	// A blow by the move in `p_attacker` landing on `r_target` -- the one rule for it, the server's and
	// offline play's alike: through a dodge's untouchable ticks nothing; on a guard facing it, its guard
	// cost in stamina and a push back, or, without that much, the guard broken and the body staggered;
	// otherwise its damage, and its poise damage through Walker::take_hit. `r_target` is changed to what
	// the blow leaves; `r_reaction` is the reaction move it caused, `r_damage` the health it takes (the
	// caller keeps health: it is not walker state). Returns a HitResult.
	uint8_t strike(const WalkerState &p_attacker, WalkerState &r_target, uint8_t &r_reaction, int &r_damage, double p_guard_break_push = 4.0) const {
		const Move &m = move_of(p_attacker);
		const Vector3 up = up_at(r_target.position);
		// Pushed away from where the blow came from: the attacker, or where an area landed.
		Vector3 away = r_target.position - (m.shape == SHAPE_AREA ? p_attacker.aim : p_attacker.position);
		away -= up * away.dot(up);
		away = away.length_squared() > 1e-12 ? away.normalized() : Vector3();
		r_reaction = ACTION_NONE;
		r_damage = 0;
		if (invulnerable(r_target)) {
			return HIT_DODGED;
		}
		if (guards(r_target, m.shape == SHAPE_AREA ? p_attacker.aim : p_attacker.position)) {
			if (r_target.stamina >= m.guard_cost) {
				r_target.stamina = uint16_t(r_target.stamina - m.guard_cost);
				r_target.rest = stamina_rest_ticks;
				r_reaction = ACTION_BLOCKED;
				r_target = recoil(r_target, away * m.guard_push, ACTION_BLOCKED);
				return HIT_BLOCKED;
			}
			r_target.stamina = 0;
			r_target.flags |= FLAG_EXHAUSTED;
			r_reaction = ACTION_STAGGER;
			r_target = recoil(r_target, away * p_guard_break_push, ACTION_STAGGER);
			return HIT_GUARD_BROKEN;
		}
		r_damage = m.damage;
		r_target = take_hit(r_target, m, away, r_reaction);
		return HIT_STRUCK;
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
		const bool moving = wish.length_squared() > 1e-12;
		const bool sprinting = (p_input.buttons & BUTTON_SPRINT) && moving && !(p_state.flags & FLAG_EXHAUSTED);
		const double run_speed = sprinting ? sprint_speed : speed;

		WalkerState out = p_state;
		out.latch = p_input.buttons;
		// Poise refills after a rest without hits; focus all the time; cooldowns run down.
		if (out.poise_rest > 0) {
			--out.poise_rest;
		} else if (out.poise < poise_max) {
			out.poise = uint16_t(MIN(uint32_t(poise_max), uint32_t(out.poise) + poise_regen));
		}
		out.focus = uint16_t(MIN(uint32_t(focus_max), uint32_t(out.focus) + focus_regen));
		for (int k = 0; k < ABILITY_SLOTS; ++k) {
			if (out.cooldown[k] > 0) {
				--out.cooldown[k];
			}
		}
		const uint8_t pressed_abilities = p_input.abilities & ~p_state.ability_latch;
		out.ability_latch = p_input.abilities;
		// The input buffer: this tick's press, or one still waiting.
		const uint8_t pressed_intent = _intent(pressed, pressed_abilities);
		if (pressed_intent != INTENT_NONE) {
			out.buffer = pressed_intent;
			out.buffer_ticks = buffer_ticks;
		}
		const uint8_t intent = out.buffer_ticks > 0 ? out.buffer : uint8_t(INTENT_NONE);
		bool used_intent = false;
		// A climb jump is over once off the wall, however it left it.
		if (out.action == ACTION_CLIMB_JUMP && (p_state.mode != MODE_CLIMB || p_state.dash == 0)) {
			out.action = ACTION_NONE;
			out.action_ticks = 0;
		}
		bool spent = false;
		switch (p_state.mode) {
			case MODE_AIR:
				if ((pressed & BUTTON_JUMP) && p_state.velocity.dot(up) < glide_open_rise && _can_spend(p_state)) {
					out.mode = MODE_GLIDE;
					_glide(out, p_state, up, wish, p_dt);
					spent = _spend(out, glide_cost);
				} else {
					_air(out, p_state, up, wish * run_speed, p_dt);
				}
				break;
			case MODE_GLIDE:
				if ((pressed & (BUTTON_JUMP | BUTTON_DROP)) || !_can_spend(p_state)) {
					// Closed: falling again.
					out.mode = MODE_AIR;
					_air(out, p_state, up, wish * run_speed, p_dt);
				} else {
					_glide(out, p_state, up, wish, p_dt);
					spent = _spend(out, glide_cost);
				}
				break;
			case MODE_CLIMB:
				spent = _climb(out, p_state, forward, right, mx, my, pressed, p_dt);
				break;
			default: {
				const Vector3 vh = p_state.velocity - up * p_state.velocity.dot(up);
				const bool locked = (p_input.buttons & BUTTON_LOCK) && !sprinting;
				const Move &now = move_of(p_state);
				const Ground g{ up, forward, wish, vh, locked, moving, p_input.aim };
				const bool can_start = !now.committed() && intent != INTENT_NONE && _affords(out, intent, _intent_move(p_state, intent), 0)
						&& _castable(p_state, _intent_move(p_state, intent), g);
				if (!now.committed() && intent != INTENT_NONE && !can_start) {
					// Free to move but unable to pay, or to see where it would cast: the press is dropped, not
					// kept until stamina comes back -- the buffer is for presses a move is holding up.
					used_intent = true;
				}
				if (now.committed()) {
					// In a move: what a press may start part way through it (a cancel window), or on with it.
					const uint8_t next = _cancel_into(now, p_state, intent);
					if (next != ACTION_NONE && _affords(out, intent, next, _cancel_extra(now, p_state, intent)) && _castable(p_state, next, g)) {
						spent = _start_move(out, p_state, next, intent, _cancel_extra(now, p_state, intent), g, p_dt);
						used_intent = true;
					} else {
						_run_move(out, p_state, now, g, p_dt);
					}
				} else if (can_start) {
					spent = _start_move(out, p_state, _intent_move(p_state, intent), intent, 0, g, p_dt);
					used_intent = true;
				} else if (pressed & BUTTON_JUMP) {
					out.action = ACTION_NONE;
					out.action_ticks = 0;
					out.mode = MODE_AIR;
					out.velocity = _ease(vh, wish * run_speed, p_dt) + up * jump_speed;
					// From just above the ground, as letting go of a wall starts just off it: a path that
					// starts on the face stood on meets that face first wherever its back collides too (a
					// cliff's skin does, so the hollow inside of one can be climbed), and took it for a
					// ceiling, grabbing the top of the cliff from inside the rock.
					WalkerState launched = out;
					launched.position += up * 0.05;
					_air(out, launched, up, wish * run_speed, p_dt);
				} else if (p_input.buttons & BUTTON_BLOCK) {
					// Guarding, for as long as it is held: slow, facing the way of yaw.
					const Move &guard = moves().get(ACTION_BLOCK);
					out.action = ACTION_BLOCK;
					out.action_ticks = p_state.action == ACTION_BLOCK ? uint8_t(MIN(255, p_state.action_ticks + 1)) : 1;
					_ground(out, p_state, up, _ease(vh, wish * speed * guard.steer, p_dt), p_dt);
					if (out.mode == MODE_GROUND) {
						out.rotation = facing_rotation(forward, up);
					}
				} else {
					out.action = ACTION_NONE;
					out.action_ticks = 0;
					_ground(out, p_state, up, _ease(vh, wish * run_speed, p_dt), p_dt);
					if (sprinting && out.mode == MODE_GROUND) {
						spent = _spend(out, sprint_cost);
					}
					// Locked on: facing the target (yaw), whichever way it moves -- strafing.
					if (locked && out.mode == MODE_GROUND) {
						out.rotation = facing_rotation(forward, up);
					}
				}
			} break;
		}
		// A buffered press is used up by the move it started, or waits out its ticks.
		if (used_intent) {
			out.buffer = INTENT_NONE;
			out.buffer_ticks = 0;
		} else if (out.buffer_ticks > 0 && --out.buffer_ticks == 0) {
			out.buffer = INTENT_NONE;
		}
		// The ground's actions end off the ground; a climb jump is the wall's own (cleared above when it ends).
		if (out.mode != MODE_GROUND && out.action != ACTION_NONE && out.action != ACTION_DEATH && !(out.mode == MODE_CLIMB && out.action == ACTION_CLIMB_JUMP)) {
			out.action = ACTION_NONE;
			out.action_ticks = 0;
		}
		_rest(out, spent);
		out.reference = transport(north, up_at(out.position));
		return out;
	}

private:
	// What a ground tick's input means for the move table.
	struct Ground {
		Vector3 up;
		Vector3 forward; // the way of yaw: the locked-on target's
		Vector3 wish; // the stick, in the ground plane, at most 1 long
		Vector3 vh; // the ground velocity
		bool locked;
		bool moving;
		Vector3 aim; // an aimed ability's: offset from the position (WalkerInput::aim)
	};

	// Whether an aimed move (SHAPE_AREA) starting now could be cast: where it would land in sight of the
	// caster's eye. Any other move, always.
	bool _castable(const WalkerState &p_in, uint8_t p_id, const Ground &p_g) const {
		const Move &m = moves().get(p_id);
		if (m.shape != SHAPE_AREA) {
			return true;
		}
		Vector3 flat;
		return can_cast_from(p_in.position, p_in.position + _landing(m, p_g.aim, p_g.up, _start_face(p_in, m, p_g), flat));
	}

	// The way a move starting now faces, before an aim turns it: a roll the stick's way, else the target
	// locked on, else the stick, else as it faced.
	Vector3 _start_face(const WalkerState &p_in, const Move &p_move, const Ground &p_g) const {
		if (p_move.root == ROOT_ROLL) {
			return p_g.moving ? p_g.wish.normalized() : _facing(p_in, p_g.up);
		}
		return p_g.locked ? p_g.forward : (p_g.moving ? p_g.wish.normalized() : _facing(p_in, p_g.up));
	}

	// What this tick's presses ask for, the strongest first: a dodge, an ability (the lowest slot), a
	// light, a heavy.
	static uint8_t _intent(uint8_t p_pressed, uint8_t p_abilities) {
		if (p_pressed & BUTTON_DODGE) {
			return INTENT_DODGE;
		}
		for (int k = 0; k < ABILITY_SLOTS; ++k) {
			if (p_abilities & (1u << k)) {
				return uint8_t(INTENT_ABILITY + k);
			}
		}
		if (p_pressed & BUTTON_LIGHT) {
			return INTENT_LIGHT;
		}
		if (p_pressed & BUTTON_HEAVY) {
			return INTENT_HEAVY;
		}
		return INTENT_NONE;
	}

	// The move an intent starts from where the walker is.
	static uint8_t _intent_move(const WalkerState &p_state, uint8_t p_intent) {
		switch (p_intent) {
			case INTENT_DODGE:
				return ACTION_DODGE;
			case INTENT_LIGHT:
				return move_of(p_state).kind == KIND_ATTACK ? move_of(p_state).next_light : ACTION_LIGHT;
			case INTENT_HEAVY:
				return ACTION_HEAVY;
			default:
				if (p_intent >= INTENT_ABILITY && p_intent < INTENT_ABILITY + ABILITY_SLOTS) {
					const uint8_t id = p_state.loadout[p_intent - INTENT_ABILITY];
					return moves().get(id).kind == KIND_ABILITY ? id : uint8_t(ACTION_NONE);
				}
				return ACTION_NONE;
		}
	}

	static uint8_t _intent_into(uint8_t p_intent) {
		switch (p_intent) {
			case INTENT_DODGE:
				return INTO_DODGE;
			case INTENT_LIGHT:
				return INTO_LIGHT;
			case INTENT_HEAVY:
				return INTO_HEAVY;
			default:
				return p_intent >= INTENT_ABILITY ? INTO_ABILITY : 0;
		}
	}

	// The cancel window of `p_move` open now for an intent, if any.
	static const MoveCancel *_window(const Move &p_move, const WalkerState &p_state, uint8_t p_intent) {
		const uint8_t into = _intent_into(p_intent);
		for (uint8_t i = 0; i < p_move.cancel_count && into != 0; ++i) {
			const MoveCancel &c = p_move.cancels[i];
			if ((c.into & into) && p_state.action_ticks >= c.from && p_state.action_ticks < c.to) {
				return &c;
			}
		}
		return nullptr;
	}

	static uint8_t _cancel_into(const Move &p_move, const WalkerState &p_state, uint8_t p_intent) {
		return _window(p_move, p_state, p_intent) != nullptr ? _intent_move(p_state, p_intent) : uint8_t(ACTION_NONE);
	}

	static uint16_t _cancel_extra(const Move &p_move, const WalkerState &p_state, uint8_t p_intent) {
		const MoveCancel *c = _window(p_move, p_state, p_intent);
		return c != nullptr ? c->extra_stamina : 0;
	}

	// Whether the move an intent asks for can be started: its stamina (plus a cancel's extra) with the
	// walker not exhausted, its focus, and an ability's slot off cooldown.
	bool _affords(const WalkerState &p_state, uint8_t p_intent, uint8_t p_id, uint16_t p_extra) const {
		const Move &m = moves().get(p_id);
		if (m.kind == KIND_NONE) {
			return false;
		}
		const uint32_t stamina = uint32_t(m.stamina) + p_extra;
		if (stamina > 0 && (p_state.stamina < stamina || (p_state.flags & FLAG_EXHAUSTED))) {
			return false;
		}
		if (p_state.focus < m.focus) {
			return false;
		}
		if (p_intent >= INTENT_ABILITY && p_intent < INTENT_ABILITY + ABILITY_SLOTS && p_state.cooldown[p_intent - INTENT_ABILITY] > 0) {
			return false;
		}
		return true;
	}

	// Starts a move: pays for it -- stamina, focus, an ability's cooldown -- faces the way it goes (an
	// aimed one toward its aim, else the target locked on, else the stick, else as it faced), and runs
	// its first tick. Returns whether it spent stamina.
	bool _start_move(WalkerState &r_out, const WalkerState &p_in, uint8_t p_id, uint8_t p_intent, uint16_t p_extra, const Ground &p_g, double p_dt) const {
		const Move &m = moves().get(p_id);
		r_out.action = p_id;
		r_out.action_ticks = 0;
		const uint32_t stamina = uint32_t(m.stamina) + p_extra;
		if (stamina > 0) {
			_spend(r_out, uint16_t(MIN(stamina, 65535u)));
		}
		r_out.focus = r_out.focus > m.focus ? uint16_t(r_out.focus - m.focus) : uint16_t(0);
		if (p_intent >= INTENT_ABILITY && p_intent < INTENT_ABILITY + ABILITY_SLOTS) {
			r_out.cooldown[p_intent - INTENT_ABILITY] = m.cooldown;
		}
		Vector3 face = _start_face(p_in, m, p_g);
		if (m.shape == SHAPE_AREA) {
			// Where it lands: the aim, flattened and kept within range; with none, straight ahead.
			Vector3 flat;
			r_out.aim = p_in.position + _landing(m, p_g.aim, p_g.up, face, flat);
			if (flat.length_squared() > 1e-6) {
				face = flat.normalized();
			}
		}
		return _begin(r_out, p_in, p_id, face, p_g, p_dt) || stamina > 0;
	}

	bool _begin(WalkerState &r_out, const WalkerState &p_in, uint8_t p_id, const Vector3 &face, const Ground &p_g, double p_dt) const {
		const Move &m = moves().get(p_id);
		WalkerState from = p_in;
		from.action = p_id;
		from.action_ticks = 0;
		from.rotation = facing_rotation(face, p_g.up);
		_run_move(r_out, from, m, p_g, p_dt, &face);
		return false;
	}

	// One tick of a move: steering, tracking and its own motion; over at its last tick. `p_along`, on the
	// tick it starts, is the way it goes.
	void _run_move(WalkerState &r_out, const WalkerState &p_in, const Move &p_move, const Ground &p_g, double p_dt, const Vector3 *p_along = nullptr) const {
		const uint8_t t = p_in.action_ticks;
		r_out.action = p_in.action;
		r_out.action_ticks = uint8_t(MIN(255, t + 1));
		Vector3 face = _facing(p_in, p_g.up);
		if (p_move.kind != KIND_REACTION && t < p_move.track_to) {
			// Tracking, through the windup: turned toward the target locked on, or the way the stick points.
			if (p_g.locked) {
				face = _turn_toward(face, p_g.forward, p_g.up, p_move.track_rate);
			} else if (p_g.moving) {
				face = _turn_toward(face, p_g.wish.normalized(), p_g.up, p_move.track_rate);
			}
		}
		const bool live_root = t >= p_move.root_from && t < p_move.root_to;
		Vector3 velocity;
		switch (p_move.root) {
			case ROOT_ROLL: {
				Vector3 along = p_along != nullptr ? *p_along : (p_g.vh.length_squared() > 1e-12 ? p_g.vh.normalized() : face);
				const double k = p_move.ticks > 0 ? double(t) / double(p_move.ticks) : 0.0;
				velocity = along * (p_move.root_speed + (p_move.root_end_speed - p_move.root_speed) * k);
			} break;
			case ROOT_BRAKE: {
				const double sp = p_g.vh.length();
				const double slower = MAX(0.0, sp - recoil_brake * p_dt);
				velocity = sp > 1e-9 ? p_g.vh * (slower / sp) : Vector3();
			} break;
			case ROOT_FACING:
				velocity = _ease(p_g.vh, p_g.wish * speed * p_move.steer + face * (live_root ? p_move.root_speed : 0.0), p_dt);
				break;
			default:
				velocity = _ease(p_g.vh, p_g.wish * speed * p_move.steer, p_dt);
				break;
		}
		_ground(r_out, p_in, p_g.up, velocity, p_dt);
		if (r_out.mode == MODE_GROUND) {
			if (p_move.kind == KIND_REACTION) {
				r_out.rotation = p_in.rotation; // knocked back facing as it was
			} else if (p_move.root != ROOT_ROLL) {
				r_out.rotation = facing_rotation(face, p_g.up);
			}
		}
		if (r_out.action_ticks >= p_move.ticks && r_out.action != ACTION_DEATH) {
			r_out.action = ACTION_NONE;
			r_out.action_ticks = 0;
		}
	}

	// `p_from` turned about `p_up` toward `p_to` by at most `p_degrees` (0: all the way).
	static Vector3 _turn_toward(const Vector3 &p_from, const Vector3 &p_to, const Vector3 &p_up, double p_degrees) {
		if (p_degrees <= 0.0) {
			return p_to;
		}
		const double rate = p_degrees * (Math::PI / 180.0);
		const double c = PlanetDetMath::cos(rate);
		if (p_from.dot(p_to) >= c) {
			return p_to;
		}
		const Vector3 side = p_up.cross(p_from);
		const double s = PlanetDetMath::sin(rate) * (side.dot(p_to) >= 0.0 ? 1.0 : -1.0);
		return (p_from * c + side * s).normalized();
	}

	// The ground velocity eased toward the wished one: accelerating, or braking when the stick is let go
	// or turned against the way the body goes.
	Vector3 _ease(const Vector3 &p_velocity, const Vector3 &p_wish, double p_dt) const {
		const Vector3 dv = p_wish - p_velocity;
		const bool braking = p_wish.length_squared() < p_velocity.length_squared() || p_wish.dot(p_velocity) < 0.0;
		const double most = (braking ? ground_brake : ground_accel) * p_dt;
		if (dv.length_squared() <= most * most) {
			return p_wish;
		}
		return p_velocity + dv.normalized() * most;
	}

	// The way the body faces, flat on the ground.
	static Vector3 _facing(const WalkerState &p_state, const Vector3 &p_up) {
		Vector3 f = -Basis(p_state.rotation).get_column(2);
		f = f - p_up * f.dot(p_up);
		return f.length_squared() > 1e-12 ? f.normalized() : local_north(p_up);
	}

	bool _can_spend(const WalkerState &p_state) const {
		return p_state.stamina > 0 && !(p_state.flags & FLAG_EXHAUSTED);
	}

	// Spends stamina; empty, the walker is exhausted until it has refilled. Returns true (spent).
	bool _spend(WalkerState &r_out, uint16_t p_amount) const {
		r_out.stamina = p_amount >= r_out.stamina ? 0 : uint16_t(r_out.stamina - p_amount);
		if (r_out.stamina == 0) {
			r_out.flags |= FLAG_EXHAUSTED;
		}
		return true;
	}

	// After a tick: spent, rest starts again; rested, on the ground and not rolling, it refills.
	void _rest(WalkerState &r_out, bool p_spent) const {
		if (r_out.stamina > stamina_max) {
			r_out.stamina = stamina_max;
		}
		if (p_spent) {
			r_out.rest = stamina_rest_ticks;
			return;
		}
		if (r_out.rest > 0) {
			--r_out.rest;
			return;
		}
		if (r_out.mode != MODE_GROUND || r_out.action != ACTION_NONE) {
			return;
		}
		const uint32_t filled = uint32_t(r_out.stamina) + stamina_regen;
		r_out.stamina = uint16_t(MIN(filled, uint32_t(stamina_max)));
		if (r_out.stamina == stamina_max) {
			r_out.flags &= ~FLAG_EXHAUSTED;
		}
	}

	// Under the glider: the fall eased to the sink, the horizontal velocity eased toward the stick's;
	// then the same path and landing as any flight.
	void _glide(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_wish, double p_dt) const {
		double vr = p_in.velocity.dot(p_up) - gravity * p_dt;
		if (vr < -glide_sink) {
			vr = MIN(-glide_sink, p_in.velocity.dot(p_up) + glide_brake * p_dt);
		}
		Vector3 vh = p_in.velocity - p_up * p_in.velocity.dot(p_up);
		Vector3 dv = p_wish * glide_speed - vh;
		const double most = glide_accel * p_dt;
		if (dv.length_squared() > most * most) {
			dv = dv.normalized() * most;
		}
		vh += dv;
		_fly(r_out, p_in, p_up, vh + p_up * vr, vh, p_dt);
	}

	// Grabs whatever is in front of the body -- chest high first, then lower -- not what is at the feet:
	// under an overhang the feet are in the crease where it meets the ground, where the surface has no
	// one direction to climb.
	bool _grab(WalkerState &r_out, const Vector3 &p_feet, const Vector3 &p_up, const Vector3 &p_ahead) const {
		const double heights[3] = { 1.0, 0.6, 0.25 };
		for (int k = 0; k < 3; ++k) {
			const Vector3 from = p_feet + p_up * heights[k];
			Vector3 grip;
			if (cast(from, from + p_ahead * (body_radius + 0.5), grip)) {
				if (!climbable(grip) || (r_out.flags & FLAG_NO_CLIMB)) {
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

	// A velocity along the ground kept out of the other bodies: what would take it into one this tick
	// taken off, so it arrives touching and slides round, and one it overlaps pushed apart, over a tick
	// at most BODY_SEPARATE.
	Vector3 _round_bodies(const Vector3 &p_feet, const Vector3 &p_up, const Vector3 &p_velocity, double p_dt) const {
		Vector3 v = p_velocity;
		for (uint32_t i = 0; i < body_feet.size(); ++i) {
			const Vector3 d = p_feet - body_feet[i];
			const double dv = d.dot(p_up);
			if (dv > BODY_HEIGHT || dv < -BODY_HEIGHT) {
				continue;
			}
			Vector3 dh = d - p_up * dv;
			const double reach = body_radius + body_radii[i];
			const double dist = dh.length();
			// Within a tick's travel of touching: closer than that, it arrives touching, not inside.
			if (dist >= reach + v.length() * p_dt + 0.02) {
				continue;
			}
			if (dist < 1e-6) {
				// Exactly on top of it: out the way it was going, or any way.
				dh = v.length_squared() > 1e-12 ? -v : Walker::local_north(p_up);
				dh -= p_up * dh.dot(p_up);
			}
			const Vector3 out = dh.normalized();
			const double into = v.dot(out);
			const double allowed = MAX(dist - reach, 0.0) / p_dt; // m/s of approach that just touches
			if (into < -allowed) {
				v -= out * (into + allowed);
			}
			const double overlap = reach - dist;
			if (overlap > 0.0) {
				v += out * MIN(overlap / p_dt, BODY_SEPARATE);
			}
		}
		return v;
	}

	void _ground(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &p_velocity_in, double p_dt) const {
		const Vector3 p_velocity = body_feet.is_empty() ? p_velocity_in : _round_bodies(p_in.position, p_up, p_velocity_in, p_dt);
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
			if (!_grab(r_out, here, p_up, ahead) && climbable(next) && !(r_out.flags & FLAG_NO_CLIMB)) {
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
		if (!body_feet.is_empty()) {
			vh = _round_bodies(p_in.position, p_up, vh, p_dt);
		}
		_fly(r_out, p_in, p_up, vh + p_up * (vr - gravity * p_dt), vh, p_dt);
	}

	// One tick of flight at velocity v (vh its horizontal part): along the path to wherever it first
	// meets something, and what that makes of the walker.
	void _fly(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_up, const Vector3 &v, const Vector3 &vh, double p_dt) const {
		const Vector3 from = p_in.position;
		// Inside the ground, however it got there: no path from in there meets anything, and the walker
		// would fall forever. Put it on the ground above it. Only when well in (5 cm): a walker standing
		// on the ground, as one is on the tick it jumps, is on its surface, and counted inside by a hair.
		if (has_analytic() && inside(from) && inside(from + p_up * 0.05)) {
			Vector3 top;
			if (cast(from + p_up * 50.0, from, top)) {
				r_out.mode = MODE_GROUND;
				r_out.position = top;
				r_out.velocity = vh;
				r_out.normal = Vector3();
				return;
			}
		}
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
		} else if (!(p_in.flags & (FLAG_NO_GRAB | FLAG_EXHAUSTED | FLAG_NO_CLIMB)) && climbable(land)) {
			r_out.mode = MODE_CLIMB;
			r_out.position = land;
			r_out.velocity = Vector3();
			r_out.normal = n;
			r_out.rotation = facing_rotation(face_forward(n, land_up, vh), land_up);
		} else {
			// Not holding on: slide down the face, keeping what of the velocity runs along it, just off
			// it so the next tick's path starts outside and can meet it again -- off along the face's
			// normal, or, where that is inside other ground, along its level part or straight up. Below
			// a ball's equator the normal points down, and where the ball is sunk into the ground the
			// step off it along that ended inside the ground: a path from inside it meets nothing, and the
			// walker fell through the planet (a slow climber let go there, out of stamina).
			r_out.mode = MODE_AIR;
			r_out.velocity = v - n * MIN(v.dot(n), 0.0);
			r_out.position = _outside(land, n, land_up);
		}
	}

	// A point just off a surface at p_on and outside all ground: off along p_normal, else its level part,
	// else straight up, else p_on itself.
	Vector3 _outside(const Vector3 &p_on, const Vector3 &p_normal, const Vector3 &p_up) const {
		if (!has_analytic()) {
			return p_on + p_normal * 0.02;
		}
		Vector3 level = p_normal - p_up * p_normal.dot(p_up);
		const Vector3 offs[3] = { p_normal, level.length_squared() > 1e-6 ? level.normalized() : p_up, p_up };
		for (int k = 0; k < 3; ++k) {
			const Vector3 q = p_on + offs[k] * 0.02;
			if (!inside(q)) {
				return q;
			}
		}
		return p_on;
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
		// Sideways is the climber's own right on the face (its up across its normal), as climbing games
		// take it -- the camera's right whenever the camera is behind it. The camera's right flattened onto
		// the face was nothing past an outside corner, where it points into the next face: a climber going
		// round one stopped dead on it.
		Vector3 side;
		if (ahead.length_squared() > 1e-12) {
			ahead = ahead.normalized();
			side = ahead.cross(p_normal).normalized();
			// The stick's right is the climber's left only with the camera in front of it, looking back at it.
			const Vector3 cam_side = p_right - p_normal * p_normal.dot(p_right);
			if (cam_side.length_squared() > 0.25 && cam_side.dot(side) < 0.0) {
				side = -side;
			}
		} else {
			side = p_right - p_normal * p_normal.dot(p_right);
			if (side.length_squared() > 1e-12) {
				side = side.normalized();
			}
		}
		return ahead * p_my + side * p_mx;
	}

	// Returns whether stamina was spent.
	bool _climb(WalkerState &r_out, const WalkerState &p_in, const Vector3 &p_forward, const Vector3 &p_right, double p_mx, double p_my, uint8_t p_pressed, double p_dt) const {
		const Vector3 here = p_in.position;
		const Vector3 up = up_at(here);
		Vector3 n = p_in.normal;
		if (n.length_squared() < 0.5) {
			n = up;
		}
		r_out.rotation = facing_rotation(face_forward(n, up, p_forward), up);
		if ((p_pressed & BUTTON_DROP) || (p_in.flags & FLAG_EXHAUSTED)) {
			// Let go -- or out of stamina, which lets go for you.
			r_out.mode = MODE_AIR;
			r_out.flags |= FLAG_NO_GRAB;
			r_out.dash = 0;
			r_out.position = here + n * 0.05;
			r_out.velocity = n;
			return false;
		}
		if ((p_pressed & BUTTON_JUMP) && p_my < 0.0) {
			// Jump with back held: leap off.
			r_out.mode = MODE_AIR;
			r_out.dash = 0;
			r_out.position = here + n * 0.05;
			r_out.velocity = up * jump_speed * 0.8 + n * 4.0;
			return false;
		}
		bool spent = false;
		if ((p_pressed & BUTTON_JUMP) && p_in.dash == 0 && p_in.stamina >= climb_jump_cost) {
			// A climb jump: a wind-up, then a lunge along the face, the way the stick points (onward if it
			// points nowhere). Its action says so to whoever draws it, here and on other machines.
			r_out.dash = dash_ticks + dash_windup;
			r_out.action = ACTION_CLIMB_JUMP;
			r_out.action_ticks = 0;
			spent = _spend(r_out, climb_jump_cost);
		}
		double my = p_my;
		double amount = MIN(Vector2(p_mx, p_my).length(), 1.0);
		double rate = climb_speed;
		if (r_out.dash > 0) {
			--r_out.dash;
			if (r_out.action == ACTION_CLIMB_JUMP && r_out.action_ticks < 255) {
				++r_out.action_ticks;
			}
			if (r_out.dash >= dash_ticks) {
				// Winding up: held still.
				amount = 0.0;
			} else {
				rate = dash_speed;
				if (amount < 1e-9) {
					my = 1.0;
				}
				amount = 1.0;
			}
			if (r_out.dash == 0 && r_out.action == ACTION_CLIMB_JUMP) {
				r_out.action = ACTION_NONE;
				r_out.action_ticks = 0;
			}
		}
		r_out.position = here;
		r_out.velocity = Vector3();
		Vector3 dir = climb_direction(n, up, p_forward, p_right, p_mx, my);
		if (amount < 1e-9 || dir.length_squared() < 1e-6) {
			return spent;
		}
		dir = dir.normalized();
		// On an underside -- under a hold, an overhang -- pushing on toward the wall it came up is climbing
		// up into the corner: out from under it is the way up, round its edge. Taken as onward into the
		// wall, a climber went onto the underside, back down the wall at the corner, up again, every
		// tick: rubber-banding 2 cm up and down for as long as the stick was held. Decided by the wall
		// being near along the way, not by flipping once, so it does not swing back a tick later; looking
		// out from under (a slab crawled along), forward is out already and nothing is met.
		// Turned out from under (below), it keeps on out while the stick says the same: beyond the probe's
		// reach the way pointed back at the wall, and the climber rocked there, 0.6 m out under a ceiling.
		const Vector3 was = p_in.velocity - n * n.dot(p_in.velocity);
		if (n.dot(up) < -0.5 && my > 0.0 && was.length() > 0.05 && was.normalized().dot(dir.normalized()) < -0.5) {
			dir = was.normalized() * dir.length();
		} else if (n.dot(up) < -0.5 && my > 0.0) {
			Vector3 corner;
			Vector3 corner_normal;
			if (cast(here + n * 0.02, here + n * 0.02 + dir * UNDERSIDE_WALL_REACH, corner, &corner_normal) &&
					corner_normal.length_squared() > 0.5 && Math::abs(corner_normal.normalized().dot(up)) < UNDERSIDE_WALL_LEVEL) {
				// Only the part of the way into the wall is turned away: along it, the climber goes on beside
				// the wall; pushed straight at it, out from under. Turned round whole, a climber under a
				// ceiling going along it and a little toward the wall went back and forth across the reach
				// of this probe, and nowhere.
				const Vector3 cn = corner_normal.normalized();
				const Vector3 along = dir - cn * MIN(dir.dot(cn), 0.0);
				dir = along.length() > 0.3 ? along.normalized() : -dir;
			}
		}
		if (p_in.dash == 0) {
			spent = _spend(r_out, climb_cost);
		}
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
				// Something shallow in the way -- a hold, a bump: no deeper than CLIMB_STEP out from the
				// face, the same face found again in front of it. Stepped onto, as a walker steps up a kerb,
				// rather than taken as the next face: its side faces turned a climber going sideways to
				// face along the wall, where its stick meant nothing, and it stopped dead against the first
				// hold it met. Only for a collider, which says which way its face faces.
				// The air it steps through must be clear -- out from the face, then along it at a step's
				// height -- or under a slab the probe starts inside it and finds the face behind.
				Vector3 over;
				Vector3 over_normal;
				Vector3 blocked;
				if (hit_normal.length_squared() > 0.5 && hit_normal.normalized().dot(dir) < -0.5 &&
						!cast(at + n * 0.02, at + n * CLIMB_STEP, blocked) &&
						!cast(at + n * CLIMB_STEP, target + n * CLIMB_STEP, blocked) &&
						cast(target + n * CLIMB_STEP, target - n * 0.1, over, &over_normal) &&
						over_normal.length_squared() > 0.5 && over_normal.normalized().dot(n) > 0.9 &&
						(over - at).dot(n) > 0.005) {
					at = over;
					continue;
				}
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
		return spent;
	}
};

} // namespace PlanetNet
