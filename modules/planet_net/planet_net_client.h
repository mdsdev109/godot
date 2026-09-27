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
// and shows each entity interpolated between snapshots at a fixed delay behind the server.
//
// Milestone 1 of planet-demo's docs/NETWORKING.md. The local player is interpolated like everything
// else for now; prediction and reconciliation are milestone 3.

#include "planet_net_link.h"
#include "planet_net_protocol.h"

#include "core/object/ref_counted.h"

class PlanetNetClient : public RefCounted {
	GDCLASS(PlanetNetClient, RefCounted);

public:
	static constexpr int SNAPSHOT_BUFFER = 32;
	static constexpr int INPUT_REDUNDANCY = 4;
	static constexpr int CLOCK_SAMPLES = 16;

private:
	struct Snapshot {
		uint32_t tick = 0;
		LocalVector<PlanetNet::EntityState> entities;
	};

	struct ClockSample {
		double rtt = 0.0;
		double offset = 0.0;
	};

	struct InputCommand {
		uint32_t tick = 0;
		Vector2 move;
		float yaw = 0.f;
	};

	PlanetNetLink link;
	int server_peer = 0;
	bool link_up = false;
	bool welcomed = false;
	uint64_t local_entity = 0;
	uint16_t server_id = 0;

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
	LocalVector<Snapshot> snapshots; // oldest first
	Vector2 input_move;
	float input_yaw = 0.f;
	uint32_t last_input_tick = 0;
	LocalVector<InputCommand> recent_inputs;

	void _handle(const LocalVector<uint8_t> &p_data);
	void _on_pong(double p_sent_clock, double p_server_ticks);
	void _update_clock(double p_delta);
	void _send_ping();
	void _send_input();
	bool _find(const Snapshot &p_snapshot, uint64_t p_id, PlanetNet::EntityState &r_state) const;
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

	// move is (right, forward) in [-1, 1]; yaw is the heading in radians from local north.
	void set_input(const Vector2 &p_move, float p_yaw);

	int64_t get_local_entity_id() const { return int64_t(local_entity); }
	double get_server_time_estimate() const { return server_time; }
	double get_render_time() const { return server_time - interpolation_delay_ticks; }
	double get_rtt_ms() const { return last_rtt * 1000.0; }
	void set_interpolation_delay_ms(double p_ms) { interpolation_delay_ticks = p_ms * 0.001 * PlanetNet::TICK_HZ; }
	double get_interpolation_delay_ms() const { return interpolation_delay_ticks * 1000.0 / PlanetNet::TICK_HZ; }

	// Entities in the newest snapshot.
	PackedInt64Array get_entity_ids() const;
	bool has_entity(int64_t p_id) const;
	int get_entity_type(int64_t p_id) const;
	// Interpolated at get_render_time().
	Vector3 get_entity_position(int64_t p_id) const;
	Quaternion get_entity_rotation(int64_t p_id) const;

	Dictionary get_stats() const;

	~PlanetNetClient() { link.close(); }
};
