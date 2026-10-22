/**************************************************************************/
/*  planet_net_client.h                                                   */
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

// The client side: connects, keeps a synchronised copy of the server's clock, sends input every tick,
// acknowledges snapshots, and shows each entity interpolated between its received states at a fixed
// delay behind the server.
//
// See planet-demo's docs/NETWORKING.md. The server sends only what is near, and only what changed
// since a state this client acknowledged, so entities are tracked individually: each keeps the states
// it arrived with, which are both its interpolation history and the baselines deltas refer to.
//
// The local player is predicted: the client runs the same walker as the server (planet_net_walker.h)
// a little ahead of the server's clock, so its input for a tick arrives before the server simulates
// that tick. Each snapshot carries the server's exact state after the last input it applied; when the
// prediction for that tick differs, the client takes the server's state and replays its stored
// inputs, and the visible jump is smoothed away over a tenth of a second.

#include "planet_net_link.h"
#include "planet_net_protocol.h"
#include "planet_net_walker.h"

#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"

class PlanetNetClient : public RefCounted {
	GDCLASS(PlanetNetClient, RefCounted);

public:
	// States kept per entity. Must cover MAX_BASELINE_AGE ticks at the fastest update rate.
	static constexpr int ENTITY_HISTORY = 64;
	static constexpr int INPUT_REDUNDANCY = 4;
	// Inputs sent to a standby server, at most: every one still held (the prediction's lead and then some).
	static constexpr int STANDBY_INPUTS = 255;
	// Predicted ticks kept for reconciliation: must cover a round trip plus the lead.
	static constexpr int PREDICTION_FRAMES = 128;
	static constexpr int CLOCK_SAMPLES = 16;

private:
	struct Sample {
		uint32_t tick = 0;
		PlanetNet::Wire wire;
		PlanetNet::EntityState state;
	};

	struct Tracked {
		LocalVector<Sample> samples; // oldest first
	};

	struct ClockSample {
		double rtt = 0.0;
		double offset = 0.0;
	};

	struct Frame {
		uint32_t tick = 0;
		PlanetNet::WalkerInput input;
		PlanetNet::WalkerState state; // after applying input
		uint32_t view = 0; // the tick others were being drawn at when the input was chosen
	};
	// The two predicted frames either side of the drawn moment and the place between them (false: none
	// yet, or just reset -- draw the newest).
	bool _display_pair(const Frame *&r_before, const Frame *&r_after, double &r_t) const;
	static constexpr double DISPLAY_LAG_TICKS = 1.5;

	// Two connections: the server this client plays on, and -- ahead of a switch (MSG_TRANSFER) -- the
	// next, on standby (docs/SERVER_MESH.md). A switch swaps them.
	PlanetNetLink links[2];
	int peers[2] = { 0, 0 };
	int primary = 0;
	PlanetNetLink &link() { return links[primary]; }
	const PlanetNetLink &link() const { return links[primary]; }
	PlanetNetLink &standby() { return links[1 - primary]; }
	bool standby_up = false; // connected, hello sent
	bool standby_ready = false; // welcomed with no entity: inputs go there too
	String standby_address;
	int standby_port = 0;
	int switch_count = 0;
	double switched_at = -1.0; // `clock` at the last switch
	HashSet<uint64_t> stale; // entities from before the last switch the new server has not mentioned yet
	void _poll_standby();
	void _promote(uint16_t p_server_id, uint64_t p_entity, const Transform3D *p_carry = nullptr);
	static bool _read_carry(PlanetNet::Reader &r_reader, Transform3D &r_carry);
	// Handed here from another zone (a welcome with a carry): the inputs of ticks before `carry_until` were
	// chosen in the old zone's frame, and are replayed turned by `carry_turn`, as the server applies them.
	bool carry_active = false;
	Basis carry_turn;
	uint32_t carry_until = 0;
	PlanetNet::WalkerInput _input_of(const Frame &p_frame) const;
	void _send_hello(PlanetNetLink &r_link, int p_peer, uint8_t p_flags);
	bool link_up = false;
	bool welcomed = false;
	// The server's move table was not this client's, and it left (planet_net_moves.h). A test sets the
	// hash it claims to have, to see that happen.
	bool move_table_refused = false;
	// The player's health, as the server last said (MSG_HEALTH; not predicted: blows are the server's).
	int health = 0;
	int health_max = 0;
	uint64_t move_hash_override = 0;
	uint64_t local_entity = 0;
	uint16_t server_id = 0;
	// The space the server runs (its welcome): 0 the planet, else a zone, whose walker has no terrain and a
	// uniform down. A change is signalled (space_changed) for the game to show the other place.
	uint64_t space = 0;
	void _read_space(PlanetNet::Reader &r_reader);
	// A word about our player this far from the prediction is a jump, not an error: taken as it is, not
	// smoothed (a zone entered, a teleport by another server's say).
	static constexpr double SNAP_DISTANCE = 25.0;
	PlanetNet::Regions regions;

	// Seconds since connect_to_server(), advanced by poll().
	double clock = 0.0;
	// The estimate of the server's clock in ticks, advanced with the local clock and steered towards
	// the lowest-round-trip sample.
	double server_time = 0.0;
	bool clock_valid = false;
	LocalVector<ClockSample> clock_samples;
	uint32_t next_clock_sample = 0;
	double ping_timer = 0.0;
	double last_rtt = 0.0;
	double smoothed_rtt = 0.0; // the round trip, filtered (a tenth of each new one)
	// How far ahead of the server's clock the prediction runs, moved toward half the smoothed round trip
	// plus the lead at LEAD_SLEW ticks a second; further off than LEAD_JUMP_TICKS, it jumps.
	double lead_offset = 0.0;
	bool lead_valid = false;
	static constexpr double LEAD_SLEW = 1.0;
	static constexpr double LEAD_JUMP_TICKS = 3.0;

	double interpolation_delay_ticks = 3.0;
	HashMap<uint64_t, Tracked> tracked;
	HashMap<uint16_t, uint64_t> handles; // bound by FULL records, released by removals
	// Newest snapshot sequence decoded, and which of the 32 before it were: sent back with input.
	uint32_t newest_seq = 0;
	uint32_t ack_bits = 0;
	uint64_t records_received = 0;
	uint64_t records_undecodable = 0;

	PlanetNet::WalkerInput input;
	uint32_t last_ack_tick = 0;

	PlanetNet::Walker walker;
	String player_key;
	double prediction_lead_ticks = 2.0;
	bool predicting = false;
	uint32_t predicted_tick = 0;
	PlanetNet::WalkerState predicted;
	LocalVector<Frame> frames; // oldest first
	// Teleports asked for, mod 256. Word about our player from before the server applied the latest is
	// ignored; the first from after it restarts prediction (see request_teleport).
	uint8_t teleports_requested = 0;
	uint64_t look = 0;
	void _send_look();
	HashMap<int, double> solid_types;
	void _gather_bodies();
	bool teleport_restart = false;
	uint32_t last_confirmed_tick = 0;
	Vector3 smoothing; // visual offset left by the last corrections, decaying to zero
	double display_target = 0.0; // the prediction's clock at the last poll (ticks, fractional)
	double frame_ahead_ticks = 0.0; // set_frame_ahead
	uint64_t prediction_compared = 0;
	uint64_t prediction_exact = 0;
	uint64_t prediction_corrections = 0;
	uint64_t prediction_resets = 0;
	double prediction_worst = 0.0;

	void _handle(const LocalVector<uint8_t> &p_data);
	void _on_snapshot(PlanetNet::Reader &r_reader);
	void _mark_received(uint32_t p_seq);
	void _on_pong(double p_sent_clock, double p_server_ticks);
	void _update_clock(double p_delta);
	void _send_ping();
	void _send_input();
	void _write_inputs(PlanetNet::Writer &r_writer, uint32_t p_count) const;
	void _advance_prediction(double p_delta);
	void _confirm(uint32_t p_tick, const PlanetNet::WalkerState &p_state);
	bool _sample(uint64_t p_id, PlanetNet::EntityState &r_state) const;

protected:
	static void _bind_methods();

public:
	Error connect_to_server(const String &p_address, int p_port);
	// Connects to the next server on standby, ahead of a switch (docs/SERVER_MESH.md): inputs go to both
	// until the next server says it has the player, and then it is the one. A server's MSG_TRANSFER does
	// this by itself.
	Error prepare_switch(const String &p_address, int p_port);
	bool is_switch_pending() const { return standby_up; }
	int get_switch_count() const { return switch_count; }
	int get_server_id() const { return int(server_id); }
	int64_t get_space() const { return int64_t(space); }
	void disconnect_from_server();
	// True once the server has welcomed us and assigned our entity.
	bool is_server_connected() const { return welcomed; }
	bool was_move_table_refused() const { return move_table_refused; }
	void set_move_hash_override(int64_t p_hash) { move_hash_override = uint64_t(p_hash); }
	// The move table compiled in (planet_net_moves.h), by its hash.
	static int64_t get_move_table_hash() { return int64_t(PlanetNet::moves().hash); }
	void poll(double p_delta);

	void set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss);

	// move is (right, forward) in [-1, 1]; yaw is the heading in radians from the walker's reference
	// (get_predicted_reference); buttons are PlanetNet::WalkerButton bits: 1 jump, 2 sprint, 4 let go.
	void set_input(const Vector2 &p_move, float p_yaw, int p_buttons = 0);
	// The ability slots held (bit k: slot k) and where an aimed one goes, as an offset from the player
	// (zero: straight ahead). Held until changed, like the buttons. docs/COMBAT_DESIGN.md.
	void set_ability_input(int p_abilities, const Vector3 &p_aim);
	// Asks the server to put our player on the ground at a point (PlanetNetServer::allow_teleport).
	// The prediction moves there at once.
	void request_teleport(const Vector3 &p_point);
	// A console line to the server (MSG_COMMAND), raw; what it says back comes as `message(channel, text)`.
	void send_command(const String &p_line);
	// How our player is drawn to everyone else (EntityState::look): sent now if connected, and again on
	// every welcome, so a reconnect keeps it. 0 sends nothing.
	// Bodies the walkers cannot pass through (Walker::body_feet): entities of these types, each type its
	// radius. Server and clients must name the same, or a client predicts walking through what the
	// server stops it at. The game sets them (players, its training dummy); none by default.
	void set_solid_type(int p_type, double p_radius) {
		if (p_radius > 0.0) {
			solid_types[p_type] = p_radius;
		} else {
			solid_types.erase(p_type);
		}
	}
	Dictionary get_solid_types() const {
		Dictionary d;
		for (const KeyValue<int, double> &E : solid_types) {
			d[E.key] = E.value;
		}
		return d;
	}
	void set_look(int64_t p_look);
	int64_t get_look() const { return int64_t(look); }

	int64_t get_local_entity_id() const { return int64_t(local_entity); }
	double get_server_time_estimate() const { return server_time; }
	double get_render_time() const { return server_time - interpolation_delay_ticks + frame_ahead_ticks; }
	// Drawing: how far past the last poll this frame is drawn (seconds; the game's physics interpolation
	// fraction of a step), so what is drawn -- the player between its predicted ticks, others between
	// their states -- is where it is at the frame's moment, every frame, not at the last step's. poll()
	// sets it back to 0: the simulation reads its own clock.
	void set_frame_ahead(double p_seconds) { frame_ahead_ticks = MAX(p_seconds, 0.0) * PlanetNet::TICK_HZ; }
	double get_rtt_ms() const { return last_rtt * 1000.0; }
	void set_interpolation_delay_ms(double p_ms) { interpolation_delay_ticks = p_ms * 0.001 * PlanetNet::TICK_HZ; }
	double get_interpolation_delay_ms() const { return interpolation_delay_ticks * 1000.0 / PlanetNet::TICK_HZ; }

	// Asks the server to make a change to the world (see PlanetNetServer::WorldChange). It arrives back,
	// if the server accepts it, as world_change -- as do everyone else's changes near this client.
	void send_world_change(const Vector3 &p_point, const PackedByteArray &p_data);

	// Who this player is. Reconnecting with the same key restores the same entity where it was left.
	// Empty: anonymous, not saved. Stand-in for accounts, which do not exist yet.
	void set_player_key(const String &p_key) { player_key = p_key; }
	String get_player_key() const { return player_key; }

	// The holes to predict on: see PlanetNetServer::add_carve.
	void add_carve(const Vector3 &p_centre, double p_radius) {
		walker.carve_centres.push_back(p_centre);
		walker.carve_radii.push_back(p_radius);
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

	// Colliders the walker stands on and climbs, besides the analytic ground (planet_net_walker.h): a
	// physics space, which layers of it, and bodies to ignore. Server and clients must see the same
	// colliders on these layers, or predictions miss wherever they differ.
	void set_physics_space(const RID &p_space) { walker.space = p_space; }
	RID get_physics_space() const { return walker.space; }
	void set_collision_mask(uint32_t p_mask) { walker.collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return walker.collision_mask; }
	void add_exclude(const RID &p_body) { walker.exclude.insert(p_body); }
	void clear_excludes() { walker.exclude.clear(); }

	// A zone's walker (PlanetNetServer::set_use_terrain, set_uniform_down): the server's, or every step
	// is mispredicted.
	void set_use_terrain(bool p_use) { walker.use_terrain = p_use; }
	bool get_use_terrain() const { return walker.use_terrain; }
	void set_uniform_down(const Vector3 &p_down) { walker.uniform_down = p_down; }
	Vector3 get_uniform_down() const { return walker.uniform_down; }
	// The zone's gravity, as its server said (PlanetNetServer::set_gravity).
	double get_gravity() const { return walker.gravity; }
	// The terrain to predict on: must be the server's field, or prediction never matches.
	void set_field(const Ref<PlanetField3D> &p_field) { walker.field = p_field; }
	Ref<PlanetField3D> get_field() const { return walker.field; }
	// Extra ticks of input lead over half the round trip, to absorb jitter.
	void set_prediction_lead_ticks(double p_ticks) { prediction_lead_ticks = p_ticks; }
	double get_prediction_lead_ticks() const { return prediction_lead_ticks; }
	bool is_predicting() const { return predicting; }
	int get_predicted_tick() const { return int(predicted_tick); }
	// The local player where it should be drawn: predicted, with corrections smoothed.
	Vector3 get_predicted_position() const { return predicted.position + smoothing; }
	// The world as the predicted player's walker sees it -- the same walker, field, colliders and rules as the
	// server's -- for whoever draws or aims for the player (PlayerRig): the game's questions about where the
	// player is go here, not to a walker of its own (planet-demo docs/ARCHITECTURE.md, "One path").
	Vector3 get_up_at(const Vector3 &p_point) const { return walker.up_at(p_point); }
	double get_ground_radius(const Vector3 &p_dir) const { return walker.ground_radius(p_dir.normalized()); }
	Variant cast_ray(const Vector3 &p_from, const Vector3 &p_to) const {
		Vector3 on;
		if (walker.cast(p_from, p_to, on)) {
			return on;
		}
		return Variant();
	}
	bool in_sight(const Vector3 &p_from, const Vector3 &p_to, double p_slack) const { return walker.sees(p_from, p_to, p_slack); }
	// From the predicted player's feet, as the server judges it from the player's.
	bool can_cast_at(const Vector3 &p_point) const { return walker.can_cast_from(predicted.position, p_point); }
	bool is_inside(const Vector3 &p_point) const { return walker.inside(p_point); }
	// For drawing: between the last two predicted ticks by this frame's place between them (a tick behind
	// the newest). The game's decisions take the newest, get_predicted_position.
	Vector3 get_predicted_display_position() const;
	Quaternion get_predicted_display_rotation() const;
	Quaternion get_predicted_rotation() const { return predicted.rotation; }
	Vector3 get_predicted_velocity() const { return predicted.velocity; }
	// PlanetNet::WalkerMode: 0 on the ground, 1 in the air, 2 climbing, 3 gliding.
	int get_predicted_mode() const { return int(predicted.mode); }
	bool is_predicted_crouching() const { return (predicted.flags & PlanetNet::FLAG_CROUCH) != 0; }
	// Stamina in the walker's units (0..stamina_max), and whether it is spent out and refilling.
	int get_predicted_stamina() const { return int(predicted.stamina); }
	bool is_predicted_exhausted() const { return (predicted.flags & PlanetNet::FLAG_EXHAUSTED) != 0; }
	// PlanetNet::WalkerAction (0 none, 1 dodge) and how many ticks into it.
	int get_predicted_action() const { return int(predicted.action); }
	int get_predicted_action_ticks() const { return int(predicted.action_ticks); }
	int get_predicted_poise() const { return int(predicted.poise); }
	int get_health() const { return health; }
	int get_health_max() const { return health_max; }
	// The predicted player's combat state: {poise, focus, loadout: [move ids], cooldowns: [ticks], aim}.
	Dictionary get_predicted_combat() const { return PlanetNet::combat_of(predicted); }
	// Climbing: the outward normal of the face held.
	Vector3 get_predicted_normal() const { return predicted.normal; }
	// The direction yaw is measured from at the predicted position (WalkerState::reference). A game
	// turns its camera heading into input yaw with this: atan2(heading . (reference x up), heading .
	// reference).
	Vector3 get_predicted_reference() const { return PlanetNet::Walker::transport(predicted.reference, walker.up_at(predicted.position)); }
	// The raw prediction for a tick still in the history, or Vector3() if it is not.
	Vector3 get_predicted_position_at(int p_tick) const;
	Dictionary get_prediction_stats() const;
	void reset_prediction_stats();
	// The input predicted with at a tick still in the history: {tick, move, yaw, position}, or {}.
	Dictionary get_predicted_frame(int p_tick) const;

	// Entities the server has told us about and not yet removed.
	PackedInt64Array get_entity_ids() const;
	bool has_entity(int64_t p_id) const { return tracked.has(uint64_t(p_id)); }
	int get_entity_type(int64_t p_id) const;
	// Server tick of the newest state received for the entity, or -1. Entities that do not change are
	// not resent, so for them this stays old by design.
	int get_entity_update_tick(int64_t p_id) const;
	// Interpolated at get_render_time().
	Vector3 get_entity_position(int64_t p_id) const;
	Quaternion get_entity_rotation(int64_t p_id) const;
	Vector3 get_entity_velocity(int64_t p_id) const;
	// What the entity's body is doing (PlanetNet::pose_of: walk mode | action << 4).
	int get_entity_pose(int64_t p_id) const;
	// The look the entity's player chose (set_look), 0 if none has reached us.
	int64_t get_entity_look(int64_t p_id) const;

	Dictionary get_stats() const;

	~PlanetNetClient() {
		links[0].close();
		links[1].close();
	}
};
