/**************************************************************************/
/*  planet_net_moves.h                                                    */
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

// Every attack, ability, dodge and hit reaction as a row of one table (planet-demo docs/COMBAT_DESIGN.md),
// played by one runner in the walker (Walker::_run_move). The server and every client compile the same
// table; its hash goes in the welcome, and a client whose table differs is refused rather than left
// mispredicting every move.
//
// Times are walker ticks at 30 Hz, counted from 0 on the tick a move starts; a window [from, to) covers
// the ticks from `from` up to but not including `to`. Speeds are m/s, distances m, angles degrees.

#include "core/math/planet_det_math.h"

#include <cstdint>
#include <cstring>

namespace PlanetNet {

// What the walker is doing on top of its mode: a move's id. The first nine are the actions from before
// the table, with their numbers; abilities start at 16.
enum WalkerAction : uint8_t {
	ACTION_NONE = 0,
	ACTION_DODGE = 1, // a committed roll along the ground
	// Combat (planet-demo docs/COMBAT.md). The two lights alternate along a combo, so a body on another
	// machine, which sees only the move, sees each swing start.
	ACTION_LIGHT = 2,
	ACTION_LIGHT_2 = 3,
	ACTION_HEAVY = 4,
	ACTION_BLOCK = 5, // the guard: lasts while it is held
	ACTION_STAGGER = 6, // hit hard: knocked back, no control (set by the server: Walker::recoil)
	ACTION_BLOCKED = 7, // a hit taken on the guard: pushed back a little
	ACTION_CLIMB_JUMP = 8, // a climb jump, a dyno (Walker::dash_windup, dash_ticks): the wall's, not the table's
	ACTION_FLINCH = 9, // hit, poise broken by a light blow: a short interruption
	ACTION_KNOCKDOWN = 10, // hit, poise broken by a crushing blow: on the floor
	// Abilities: not the weapon's (COMBAT_DESIGN.md section 4). Slotted in WalkerState::loadout.
	ACTION_EMBER = 16, // an aimed burst on the ground: mobile, fragile
	ACTION_GUST = 17, // a cone that blows back what is in front: mobile, fragile
	ACTION_SLAM = 18, // a leap that crushes what is round where it lands: committed, armoured
	ACTION_COUNT = 19,
};

enum MoveKind : uint8_t {
	KIND_NONE = 0, // not a move: no row
	KIND_ATTACK, // the weapon's
	KIND_DODGE,
	KIND_GUARD, // held, not timed: anything may start from it, as from standing
	KIND_REACTION, // done to the walker by a hit (Walker::recoil): no control
	KIND_ABILITY,
	KIND_WALL, // climbing's own (the climb jump); the ground's runner leaves it alone
};

// How a move moves the body on its own, on top of what the stick still may (Move::steer).
enum MoveRoot : uint8_t {
	ROOT_NONE = 0,
	ROOT_FACING, // a lunge along the way the body faces, eased into like the stick
	ROOT_ROLL, // along the way it started, at a speed easing from root_speed to root_end_speed (the dodge)
	ROOT_BRAKE, // what it was pushed with, braking at Walker::recoil_brake (a hit reaction)
};

// Where a move's hit lands.
enum HitShape : uint8_t {
	SHAPE_NONE = 0,
	SHAPE_ARC, // within reach of the body, within `arc` either side of its facing
	SHAPE_AROUND, // within `radius` of the body, all round
	SHAPE_AREA, // within `radius` of the aim point (WalkerState::aim), which is within `range` of the body
};

// What a hit does to a body whose poise it breaks.
enum Reaction : uint8_t {
	REACT_NONE = 0,
	REACT_FLINCH,
	REACT_STAGGER,
	REACT_KNOCKDOWN,
};

// What a press can start, for cancel windows and the input buffer.
enum Intent : uint8_t {
	INTENT_NONE = 0,
	INTENT_LIGHT = 1,
	INTENT_HEAVY = 2,
	INTENT_DODGE = 3,
	INTENT_ABILITY = 4, // + the slot, 0..5: 4..9
};
constexpr int ABILITY_SLOTS = 6;

enum CancelInto : uint8_t {
	INTO_LIGHT = 1,
	INTO_HEAVY = 2,
	INTO_DODGE = 4,
	INTO_ABILITY = 8,
};

struct MoveCancel {
	uint8_t into = 0; // CancelInto bits
	uint8_t from = 0;
	uint8_t to = 255;
	uint16_t extra_stamina = 0; // on top of what the move started costs (a dodge out of a swing)
};

struct Move {
	const char *name = "";
	uint8_t kind = KIND_NONE;
	uint8_t ticks = 0; // how long; 0 for the guard, held
	// What starting it costs.
	uint16_t stamina = 0;
	uint16_t focus = 0;
	uint16_t cooldown = 0; // ticks before its slot may start it again (abilities)

	// Steering: how much of the walk speed the stick still moves the body with; whether the legs keep
	// walking under it (drawn on the upper body only); and tracking -- until `track_to` the body may still
	// turn toward the locked-on target (or the stick, not locked), at most `track_rate` degrees a tick
	// (0: at once), and after that it holds the way it faces.
	double steer = 0.0;
	bool upper_body = false;
	uint8_t track_to = 0;
	double track_rate = 0.0;

	uint8_t root = ROOT_NONE;
	uint8_t root_from = 0;
	uint8_t root_to = 0;
	double root_speed = 0.0;
	double root_end_speed = 0.0;
	bool ends_when_stopped = false; // a roll that has stopped (into a wall, off an edge) is over

	// Hits pass through the body in this window.
	uint8_t invulnerable_from = 0;
	uint8_t invulnerable_to = 0;
	// Poise: in this window the poise damage the body takes is less by `armour` (hyper-armour); a
	// fragile move is interrupted by any hit, its poise or not.
	uint16_t armour = 0;
	uint8_t armour_from = 0;
	uint8_t armour_to = 0;
	bool fragile = false;

	// Its hit, live in [hit_from, hit_to).
	uint8_t shape = SHAPE_NONE;
	uint8_t hit_from = 0;
	uint8_t hit_to = 0;
	double reach = 0.0; // SHAPE_ARC: from the body's centre to the target's surface
	double arc = 0.0; // SHAPE_ARC: degrees either side of the facing
	double radius = 0.0; // SHAPE_AROUND, SHAPE_AREA
	double range = 0.0; // SHAPE_AREA: how far from the body the aim point may be
	double height = 1.6; // a target's feet this far above or below still in it
	uint16_t damage = 0;
	uint16_t poise_damage = 0;
	uint8_t reaction = REACT_NONE; // when poise breaks
	double push = 0.0; // m/s the struck body is knocked back with
	uint16_t guard_cost = 0; // stamina a guard spends taking it; without that much it breaks
	double guard_push = 0.0;

	// Cancel windows: what a press may start part way through it.
	MoveCancel cancels[4];
	uint8_t cancel_count = 0;
	uint8_t next_light = ACTION_LIGHT; // the light a light press continues the combo with

	bool hits() const { return shape != SHAPE_NONE; }
	bool committed() const { return kind == KIND_ATTACK || kind == KIND_DODGE || kind == KIND_REACTION || kind == KIND_ABILITY; }
	void cancel(uint8_t p_into, uint8_t p_from, uint8_t p_to = 255, uint16_t p_extra = 0) {
		if (cancel_count < 4) {
			cancels[cancel_count++] = MoveCancel{ p_into, p_from, p_to, p_extra };
		}
	}
};

struct MoveTable {
	Move moves[ACTION_COUNT];
	uint64_t hash = 0;

	const Move &get(uint8_t p_id) const { return moves[p_id < ACTION_COUNT ? p_id : 0]; }

	// The table. Timings were measured from the clips that draw the weapon's moves (docs/COMBAT.md);
	// abilities' are chosen, and the clips stretched to them (COMBAT_DESIGN.md section 4).
	static MoveTable build() {
		MoveTable t;
		Move &none = t.moves[ACTION_NONE];
		none.name = "none";

		// The dodge: a roll along the ground the way the stick points (or the body faces), easing from 7.5
		// to 3 m/s, about 3.3 m; hits pass through ticks 2-11.
		Move &dodge = t.moves[ACTION_DODGE];
		dodge.name = "dodge";
		dodge.kind = KIND_DODGE;
		dodge.ticks = 18;
		dodge.stamina = 150;
		dodge.root = ROOT_ROLL;
		dodge.root_from = 0;
		dodge.root_to = 18;
		dodge.root_speed = 7.5;
		dodge.root_end_speed = 3.0;
		dodge.ends_when_stopped = true;
		dodge.invulnerable_from = 2;
		dodge.invulnerable_to = 12;

		// The lights: 22 ticks, live 9-13, a 1.5 m/s lunge ticks 3-13 (half a metre). Tracking the target
		// (or the stick) through the windup, 20 degrees a tick, then committed to the way it faces. Late in
		// one, the next of the combo or a heavy; once past its live ticks, a dodge out of it, dearer, or an
		// ability.
		const uint8_t lights[2] = { ACTION_LIGHT, ACTION_LIGHT_2 };
		for (uint8_t id : lights) {
			Move &m = t.moves[id];
			m.name = id == ACTION_LIGHT ? "light" : "light_2";
			m.kind = KIND_ATTACK;
			m.ticks = 22;
			m.stamina = 60;
			m.steer = 0.2;
			m.track_to = 9;
			m.track_rate = 20.0;
			m.root = ROOT_FACING;
			m.root_from = 3;
			m.root_to = 14;
			m.root_speed = 1.5;
			m.shape = SHAPE_ARC;
			m.hit_from = 9;
			m.hit_to = 14;
			m.reach = 2.0;
			m.arc = 60.0;
			m.damage = 10;
			m.poise_damage = 35;
			m.reaction = REACT_STAGGER;
			m.push = 3.0;
			m.guard_cost = 80;
			m.guard_push = 2.0;
			m.cancel(INTO_LIGHT | INTO_HEAVY, 13);
			m.cancel(INTO_DODGE, 14, 255, 30);
			m.cancel(INTO_ABILITY, 14);
			m.next_light = id == ACTION_LIGHT ? ACTION_LIGHT_2 : ACTION_LIGHT;
		}

		// The heavy: 36 ticks, live 14-19, a 2 m/s lunge ticks 9-19; tracking slower and only through the
		// windup; a dodge or an ability late in its recovery, a light at its very end.
		Move &heavy = t.moves[ACTION_HEAVY];
		heavy.name = "heavy";
		heavy.kind = KIND_ATTACK;
		heavy.ticks = 36;
		heavy.stamina = 150;
		heavy.steer = 0.2;
		heavy.track_to = 14;
		heavy.track_rate = 12.0;
		heavy.root = ROOT_FACING;
		heavy.root_from = 9;
		heavy.root_to = 20;
		heavy.root_speed = 2.0;
		heavy.shape = SHAPE_ARC;
		heavy.hit_from = 14;
		heavy.hit_to = 20;
		heavy.reach = 2.4;
		heavy.arc = 80.0;
		heavy.damage = 25;
		heavy.poise_damage = 70;
		heavy.reaction = REACT_STAGGER;
		heavy.push = 5.0;
		heavy.guard_cost = 220;
		heavy.guard_push = 3.0;
		heavy.cancel(INTO_DODGE, 22, 255, 40);
		heavy.cancel(INTO_ABILITY, 22);
		heavy.cancel(INTO_LIGHT, 28);

		// The guard: while held, slow, facing the way of yaw.
		Move &guard = t.moves[ACTION_BLOCK];
		guard.name = "guard";
		guard.kind = KIND_GUARD;
		guard.steer = 0.35;

		// Reactions: pushed, braking, no control.
		Move &stagger = t.moves[ACTION_STAGGER];
		stagger.name = "stagger";
		stagger.kind = KIND_REACTION;
		stagger.ticks = 20;
		stagger.root = ROOT_BRAKE;
		Move &blocked = t.moves[ACTION_BLOCKED];
		blocked.name = "blocked";
		blocked.kind = KIND_REACTION;
		blocked.ticks = 10;
		blocked.root = ROOT_BRAKE;

		Move &climb_jump = t.moves[ACTION_CLIMB_JUMP];
		climb_jump.name = "climb_jump";
		climb_jump.kind = KIND_WALL;

		t.hash = t.compute_hash();
		return t;
	}

	// FNV-1a over every field, written out one by one (not the struct's bytes, whose padding is
	// anything).
	uint64_t compute_hash() const {
		uint64_t h = 1469598103934665603ull;
		auto bytes = [&h](const void *p_data, size_t p_size) {
			const uint8_t *b = static_cast<const uint8_t *>(p_data);
			for (size_t i = 0; i < p_size; ++i) {
				h = (h ^ b[i]) * 1099511628211ull;
			}
		};
		auto u = [&bytes](uint64_t p_v) { bytes(&p_v, sizeof(p_v)); };
		auto d = [&bytes](double p_v) { bytes(&p_v, sizeof(p_v)); };
		for (const Move &m : moves) {
			bytes(m.name, std::strlen(m.name));
			u(m.kind);
			u(m.ticks);
			u(m.stamina);
			u(m.focus);
			u(m.cooldown);
			d(m.steer);
			u(m.upper_body);
			u(m.track_to);
			d(m.track_rate);
			u(m.root);
			u(m.root_from);
			u(m.root_to);
			d(m.root_speed);
			d(m.root_end_speed);
			u(m.ends_when_stopped);
			u(m.invulnerable_from);
			u(m.invulnerable_to);
			u(m.armour);
			u(m.armour_from);
			u(m.armour_to);
			u(m.fragile);
			u(m.shape);
			u(m.hit_from);
			u(m.hit_to);
			d(m.reach);
			d(m.arc);
			d(m.radius);
			d(m.range);
			d(m.height);
			u(m.damage);
			u(m.poise_damage);
			u(m.reaction);
			d(m.push);
			u(m.guard_cost);
			d(m.guard_push);
			u(m.cancel_count);
			for (uint8_t i = 0; i < m.cancel_count; ++i) {
				u(m.cancels[i].into);
				u(m.cancels[i].from);
				u(m.cancels[i].to);
				u(m.cancels[i].extra_stamina);
			}
			u(m.next_light);
		}
		return h;
	}
};

inline const MoveTable &moves() {
	static const MoveTable table = MoveTable::build();
	return table;
}

} // namespace PlanetNet
