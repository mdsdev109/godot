/**************************************************************************/
/*  planet_character.h                                                    */
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

// The player's movement without a network: the same walker the server runs and clients predict with
// (planet_net_walker.h), for a game played offline, and for tests. One controller, so offline and
// online feel the same. See planet-demo docs/CHARACTER.md.

#include "planet_net_walker.h"

#include "core/object/ref_counted.h"

class PlanetCharacter : public RefCounted {
	GDCLASS(PlanetCharacter, RefCounted);

	PlanetNet::Walker walker;
	PlanetNet::WalkerState state;
	uint8_t ability_input = 0;
	Vector3 aim_input;

protected:
	static void _bind_methods();

public:
	void set_field(const Ref<PlanetField3D> &p_field) { walker.field = p_field; }
	Ref<PlanetField3D> get_field() const { return walker.field; }
	void set_planet_radius(double p_radius) { walker.radius = p_radius; }
	double get_planet_radius() const { return walker.radius; }
	void set_walk_speed(double p_speed) { walker.speed = p_speed; }
	double get_walk_speed() const { return walker.speed; }
	void set_sprint_speed(double p_speed) { walker.sprint_speed = p_speed; }
	double get_sprint_speed() const { return walker.sprint_speed; }
	void set_climb_speed(double p_speed) { walker.climb_speed = p_speed; }
	double get_climb_speed() const { return walker.climb_speed; }
	void set_jump_speed(double p_speed) { walker.jump_speed = p_speed; }
	double get_jump_speed() const { return walker.jump_speed; }
	void set_gravity(double p_gravity) { walker.gravity = p_gravity; }
	double get_gravity() const { return walker.gravity; }
	void set_can_climb(bool p_can) { walker.can_climb = p_can; }
	// How many ticks a press waits for its move (the input buffer); 0 drops it. A check's control: online
	// the server's and the client's must be the same.
	void set_input_buffer_ticks(int p_ticks) { walker.buffer_ticks = uint8_t(CLAMP(p_ticks, 0, 255)); }
	int get_input_buffer_ticks() const { return walker.buffer_ticks; }
	bool get_can_climb() const { return walker.can_climb; }

	void add_carve(const Vector3 &p_centre, double p_radius) {
		walker.carve_centres.push_back(p_centre);
		walker.carve_radii.push_back(p_radius);
	}
	// Other bodies it cannot walk through (Walker::body_feet): where one stands, and its radius. Set
	// them again before each step as they move.
	void add_body(const Vector3 &p_feet, double p_radius) {
		walker.body_feet.push_back(p_feet);
		walker.body_radii.push_back(p_radius);
	}
	void clear_bodies() {
		walker.body_feet.clear();
		walker.body_radii.clear();
	}
	void clear_carves() {
		walker.carve_centres.clear();
		walker.carve_radii.clear();
	}
	// Shells: solid between two radii about a centre (inner 0: a ball). Both ends must hold the same.
	void add_shell(const Vector3 &p_centre, double p_inner, double p_outer) {
		walker.shell_centres.push_back(p_centre);
		walker.shell_inner.push_back(MAX(p_inner, 0.0));
		walker.shell_outer.push_back(MAX(p_outer, p_inner));
	}
	void clear_shells() {
		walker.shell_centres.clear();
		walker.shell_inner.clear();
		walker.shell_outer.clear();
	}
	double get_ground_radius(const Vector3 &p_dir) const { return walker.ground_radius(p_dir.normalized()); }

	// Colliders: the physics space to cast rays in (World3D.space), which layers, and bodies to ignore
	// (the player's own). Analytic terrain on or off, for a level made only of colliders.
	void set_physics_space(const RID &p_space) { walker.space = p_space; }
	RID get_physics_space() const { return walker.space; }
	void set_collision_mask(uint32_t p_mask) { walker.collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return walker.collision_mask; }
	void add_exclude(const RID &p_body) { walker.exclude.insert(p_body); }
	void clear_excludes() { walker.exclude.clear(); }
	void set_use_terrain(bool p_use) { walker.use_terrain = p_use; }
	bool get_use_terrain() const { return walker.use_terrain; }
	// Down: toward the gravity centre (a planet), or along uniform_down when it is not zero (a level).
	void set_gravity_centre(const Vector3 &p_centre) { walker.gravity_centre = p_centre; }
	Vector3 get_gravity_centre() const { return walker.gravity_centre; }
	void set_uniform_down(const Vector3 &p_down) { walker.uniform_down = p_down; }
	Vector3 get_uniform_down() const { return walker.uniform_down; }
	Vector3 get_up_at(const Vector3 &p_point) const { return walker.up_at(p_point); }
	// The first surface the walker sees from one point to another, or null.
	Variant cast_ray(const Vector3 &p_from, const Vector3 &p_to) const {
		Vector3 on;
		if (walker.cast(p_from, p_to, on)) {
			return on;
		}
		return Variant();
	}
	// Whether a point is inside the solid the walker moves on.
	bool is_inside(const Vector3 &p_point) const { return walker.inside(p_point); }

	// Stands the character on the ground at a point.
	void place(const Vector3 &p_point) { state = walker.place(state, p_point); }
	// One tick. move is (right, forward) in [-1, 1]; yaw is the heading in radians from get_reference();
	// buttons are PlanetNet::WalkerButton bits: 1 jump, 2 sprint, 4 let go, 8 dodge, 16 light attack,
	// 32 heavy, 64 block (held), 128 lock on (held: face yaw). Inputs are rounded to float32 as the
	// network rounds them.
	void step(const Vector2 &p_move, float p_yaw, int p_buttons, double p_dt);
	// Abilities (docs/COMBAT_DESIGN.md): the slots held (bit k: slot k) and where an aimed one goes, an
	// offset from the character (zero: straight ahead). Held until changed, like buttons; rounded to
	// float32 as the network rounds them.
	void set_ability_input(int p_abilities, const Vector3 &p_aim) {
		ability_input = uint8_t(p_abilities & 63);
		aim_input = Vector3(float(p_aim.x), float(p_aim.y), float(p_aim.z));
	}
	// {poise, focus, loadout: [move ids], cooldowns: [ticks], aim}.
	Dictionary get_combat() const { return PlanetNet::combat_of(state); }
	int get_focus_max() const { return int(walker.focus_max); }
	void set_focus(int p_focus) { state.focus = uint16_t(CLAMP(p_focus, 0, int(walker.focus_max))); }
	// The abilities in the slots, by move id (0: empty). Offline; online the server's.
	void set_loadout(const PackedInt32Array &p_moves) {
		for (int k = 0; k < PlanetNet::ABILITY_SLOTS; ++k) {
			state.loadout[k] = k < p_moves.size() ? uint8_t(CLAMP(p_moves[k], 0, 255)) : 0;
		}
	}
	void clear_cooldowns() {
		for (int k = 0; k < PlanetNet::ABILITY_SLOTS; ++k) {
			state.cooldown[k] = 0;
		}
	}

	Vector3 get_position() const { return state.position; }
	Vector3 get_velocity() const { return state.velocity; }
	Quaternion get_rotation() const { return state.rotation; }
	Vector3 get_reference() const { return PlanetNet::Walker::transport(state.reference, walker.up_at(state.position)); }
	// 0 on the ground, 1 in the air, 2 climbing.
	int get_mode() const { return int(state.mode); }
	// Climbing: the outward normal of the face held.
	Vector3 get_normal() const { return state.normal; }
	// Stamina in the walker's units (0..get_stamina_max()), and whether it is spent out and refilling.
	int get_stamina() const { return int(state.stamina); }
	// Sets the stamina (clamped to the bar); a full bar also ends exhaustion. Offline only: online the
	// server owns it.
	void set_stamina(int p_stamina) {
		state.stamina = uint16_t(CLAMP(p_stamina, 0, int(walker.stamina_max)));
		if (state.stamina == walker.stamina_max) {
			state.flags &= ~PlanetNet::FLAG_EXHAUSTED;
		}
	}
	int get_stamina_max() const { return int(walker.stamina_max); }
	void set_stamina_max(int p_max) { walker.stamina_max = uint16_t(CLAMP(p_max, 1, 65535)); }
	bool is_exhausted() const { return (state.flags & PlanetNet::FLAG_EXHAUSTED) != 0; }
	// PlanetNet::WalkerAction (0 none, 1 dodge), how many ticks into it, and whether hits pass through.
	int get_action() const { return int(state.action); }
	int get_action_ticks() const { return int(state.action_ticks); }
	bool is_invulnerable() const { return walker.invulnerable(state); }
	// Combat offline, by the server's rules (planet_net_walker.h; docs/COMBAT.md): whether this tick's
	// swing can hit, whether it reaches a body at a point, whether the guard faces a blow from a point,
	// and a blow taken (knocked back along push, on the guard or not).
	bool is_hitting() const { return walker.hitting(state); }
	bool swing_reaches(const Vector3 &p_feet, double p_radius) const { return walker.swing_reaches(state, p_feet, p_radius); }
	bool guards(const Vector3 &p_from) const { return walker.guards(state, p_from); }
	void recoil(const Vector3 &p_push, bool p_blocked) { state = walker.recoil(state, p_push, p_blocked ? PlanetNet::ACTION_BLOCKED : PlanetNet::ACTION_STAGGER); }
	// A hit landing as the server lands one: knocked back along push into the reaction move `p_move`.
	void react(const Vector3 &p_push, int p_move) { state = walker.recoil(state, p_push, uint8_t(p_move)); }
	// A blow by the move `p_attack` landing as the server lands one -- through poise, armour and fragile
	// casts (Walker::take_hit) -- pushed along `p_away` (a direction). Returns the reaction move it caused,
	// 0 when poise took it.
	int take_hit(int p_attack, const Vector3 &p_away) {
		uint8_t reaction = PlanetNet::ACTION_NONE;
		const Vector3 up = walker.up_at(state.position);
		Vector3 away = p_away - up * p_away.dot(up);
		away = away.length_squared() > 1e-12 ? away.normalized() : Vector3();
		state = walker.take_hit(state, PlanetNet::moves().get(uint8_t(CLAMP(p_attack, 0, 255))), away, reaction);
		return int(reaction);
	}
	int get_poise() const { return int(state.poise); }
	int get_poise_max() const { return int(walker.poise_max); }

	// The move table (planet_net_moves.h), for whoever draws or shows the moves: a move by id as a
	// Dictionary of its fields, how many ids there are, and the table's hash.
	static Dictionary get_move_info(int p_id);
	static int get_move_count() { return PlanetNet::ACTION_COUNT; }
	static int64_t get_move_table_hash() { return int64_t(PlanetNet::moves().hash); }
};
