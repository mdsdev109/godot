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

class PlanetNetClient : public RefCounted {
	GDCLASS(PlanetNetClient, RefCounted);

public:
	// States kept per entity. Must cover MAX_BASELINE_AGE ticks at the fastest update rate.
	static constexpr int ENTITY_HISTORY = 64;
	static constexpr int INPUT_REDUNDANCY = 4;
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
	};

	PlanetNetLink link;
	int server_peer = 0;
	bool link_up = false;
	bool welcomed = false;
	uint64_t local_entity = 0;
	uint16_t server_id = 0;
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
	bool teleport_restart = false;
	uint32_t last_confirmed_tick = 0;
	Vector3 smoothing; // visual offset left by the last corrections, decaying to zero
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
	void _advance_prediction(double p_delta);
	void _confirm(uint32_t p_tick, const PlanetNet::WalkerState &p_state);
	bool _sample(uint64_t p_id, PlanetNet::EntityState &r_state) const;

protected:
	static void _bind_methods();

public:
	Error connect_to_server(const String &p_address, int p_port);
	void disconnect_from_server();
	// True once the server has welcomed us and assigned our entity.
	bool is_server_connected() const { return welcomed; }
	void poll(double p_delta);

	void set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss);

	// move is (right, forward) in [-1, 1]; yaw is the heading in radians from the walker's reference
	// (get_predicted_reference); buttons are PlanetNet::WalkerButton bits: 1 jump, 2 sprint, 4 let go.
	void set_input(const Vector2 &p_move, float p_yaw, int p_buttons = 0);
	// Asks the server to put our player on the ground at a point (PlanetNetServer::allow_teleport).
	// The prediction moves there at once.
	void request_teleport(const Vector3 &p_point);
	// How our player is drawn to everyone else (EntityState::look): sent now if connected, and again on
	// every welcome, so a reconnect keeps it. 0 sends nothing.
	void set_look(int64_t p_look);
	int64_t get_look() const { return int64_t(look); }

	int64_t get_local_entity_id() const { return int64_t(local_entity); }
	double get_server_time_estimate() const { return server_time; }
	double get_render_time() const { return server_time - interpolation_delay_ticks; }
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
	Quaternion get_predicted_rotation() const { return predicted.rotation; }
	Vector3 get_predicted_velocity() const { return predicted.velocity; }
	// PlanetNet::WalkerMode: 0 on the ground, 1 in the air, 2 climbing, 3 gliding.
	int get_predicted_mode() const { return int(predicted.mode); }
	// Stamina in the walker's units (0..stamina_max), and whether it is spent out and refilling.
	int get_predicted_stamina() const { return int(predicted.stamina); }
	bool is_predicted_exhausted() const { return (predicted.flags & PlanetNet::FLAG_EXHAUSTED) != 0; }
	// PlanetNet::WalkerAction (0 none, 1 dodge) and how many ticks into it.
	int get_predicted_action() const { return int(predicted.action); }
	int get_predicted_action_ticks() const { return int(predicted.action_ticks); }
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

	~PlanetNetClient() { link.close(); }
};
