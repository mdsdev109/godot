/**************************************************************************/
/*  planet_net_server.h                                                   */
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

// The authoritative side: owns the ENet host, runs the fixed tick, simulates what clients control, and
// sends each client a snapshot of what is near it. Headless; a script creates it and calls poll().
//
// See planet-demo's docs/NETWORKING.md. Each client that says hello gets a player entity walked on the
// planet by its input. Entities are indexed by region; each client is sent only the entities in regions
// within its interest radius, most relevant first, up to a byte budget, as deltas against the last
// state it acknowledged.

#include "planet_net_link.h"
#include "planet_net_protocol.h"
#include "planet_net_walker.h"

#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"
#include "scene/resources/3d/planet_field_3d.h"

class PlanetNetServer : public RefCounted {
	GDCLASS(PlanetNetServer, RefCounted);

public:
	// Entity type the server gives each connected client.
	static constexpr int ENTITY_TYPE_PLAYER = 1;
	// Ticks of transform history kept per entity, for lag compensation and for the tests.
	static constexpr int HISTORY_TICKS = 64;
	// Snapshots remembered per client, to apply acknowledgements to.
	static constexpr int SENT_RECORDS = 64;

private:
	// What one client has been told about one entity.
	struct View {
		bool has_baseline = false; // the client acknowledged a state we can delta against
		uint32_t baseline_tick = 0;
		PlanetNet::Wire baseline;
		double priority = 0.0;
		bool removing = false; // a removal is being sent until acknowledged
		// Bumped whenever the entity leaves the client's interest. An acknowledgement only counts for
		// the epoch its record was sent in: after a removal the client has dropped the old states.
		uint32_t epoch = 0;
		uint16_t handle = 0;
	};

	struct SentRecord {
		uint32_t seq = 0;
		uint32_t tick = 0;
		bool valid = false;
		LocalVector<uint64_t> ids;
		LocalVector<PlanetNet::Wire> wires;
		LocalVector<uint32_t> epochs;
		LocalVector<uint64_t> removals;
		LocalVector<uint16_t> removal_handles;
		LocalVector<uint32_t> removal_epochs;
	};

	struct Client {
		uint64_t entity = 0;
		bool welcomed = false;
		// Inputs by the tick they are for. The step for tick T applies inputs[T], or repeats the last
		// input when it has not arrived; a late input is then useless and the client will be corrected.
		HashMap<uint32_t, PlanetNet::WalkerInput> inputs;
		PlanetNet::WalkerInput last_input;
		bool has_input = false;
		uint32_t last_applied_tick = 0;
		uint64_t inputs_applied = 0;
		uint64_t inputs_missed = 0;
		// What was applied at each recent tick, and the resulting position, for diagnosing prediction.
		uint32_t applied_tick[HISTORY_TICKS] = {};
		PlanetNet::WalkerInput applied_input[HISTORY_TICKS];
		bool applied_arrived[HISTORY_TICKS] = {};
		Vector3 applied_position[HISTORY_TICKS];
		HashMap<uint64_t, View> views;
		SentRecord sent[SENT_RECORDS];
		uint32_t last_acked_seq = 0;
		uint64_t bytes_sent = 0;
		uint32_t in_interest = 0;
		LocalVector<uint16_t> free_handles;
		uint32_t next_handle = 1;
	};

	struct Entity {
		PlanetNet::EntityState state;
		int owner_peer = 0;
		uint32_t region = 0;
		bool indexed = false;
		Vector3 history[HISTORY_TICKS];
		uint32_t history_tick[HISTORY_TICKS] = {};
	};

	PlanetNetLink link;
	HashMap<int, Client> clients;
	HashMap<uint64_t, Entity> entities;
	HashMap<uint32_t, LocalVector<uint64_t>> region_index;
	PlanetNet::Regions regions;
	uint16_t server_id = 1;
	uint64_t next_entity = 1;
	uint32_t tick = 0;
	uint32_t snapshot_seq = 0;
	double accumulator = 0.0;
	double planet_radius = 150000.0;
	PlanetNet::Walker walker;
	double region_size = 1000.0;
	double interest_radius = 3000.0;
	int client_budget = 32768; // bytes per second per client
	bool delta_enabled = true;
	void _configure_regions();
	void _index(uint64_t p_id, Entity &r_entity);
	void _unindex(uint64_t p_id, Entity &r_entity);
	void _handle(int p_peer, const LocalVector<uint8_t> &p_data);
	void _acknowledge(Client &r_client, uint32_t p_seq);
	void _step();
	void _interest_regions(const Vector3 &p_centre, HashSet<uint32_t> &r_regions) const;
	void _send_snapshot(int p_peer, Client &r_client);
	void _record_history(Entity &r_entity);

protected:
	static void _bind_methods();

public:
	Error listen(int p_port, int p_max_clients = 64);
	void stop();
	bool is_listening() const { return link.is_open(); }
	int get_port() const { return link.get_local_port(); }
	void poll(double p_delta);

	void set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss);

	void set_server_id(int p_id);
	int get_server_id() const { return server_id; }
	void set_planet_radius(double p_radius);
	double get_planet_radius() const { return planet_radius; }
	void set_region_size(double p_size);
	double get_region_size() const { return region_size; }
	void set_interest_radius(double p_radius) { interest_radius = p_radius; }
	double get_interest_radius() const { return interest_radius; }
	void set_client_budget(int p_bytes_per_second) { client_budget = MAX(p_bytes_per_second, 1024); }
	int get_client_budget() const { return client_budget; }
	void set_delta_enabled(bool p_enabled) { delta_enabled = p_enabled; }
	bool is_delta_enabled() const { return delta_enabled; }
	// The terrain players walk on. Clients must be given the same field to predict exactly.
	void set_field(const Ref<PlanetField3D> &p_field) { walker.field = p_field; }
	Ref<PlanetField3D> get_field() const { return walker.field; }
	void set_walk_speed(double p_speed) { walker.speed = p_speed; }
	double get_walk_speed() const { return walker.speed; }

	int get_tick() const { return int(tick); }
	// The server's clock in ticks, including the fraction of the tick in progress.
	double get_time_ticks() const { return double(tick) + accumulator * PlanetNet::TICK_HZ; }

	int64_t create_entity(int p_type, const Vector3 &p_position);
	void destroy_entity(int64_t p_id);
	bool has_entity(int64_t p_id) const { return entities.has(uint64_t(p_id)); }
	PackedInt64Array get_entity_ids() const;
	Vector3 get_entity_position(int64_t p_id) const;
	void set_entity_position(int64_t p_id, const Vector3 &p_position);
	void set_entity_state(int64_t p_id, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_velocity);
	// Where the entity was at a (fractional) past tick, from the history ring. Returns the current
	// position when the tick is outside what is kept.
	Vector3 get_entity_position_at(int64_t p_id, double p_tick) const;
	int get_entity_owner(int64_t p_id) const;
	int get_region_of(const Vector3 &p_position) const { return int(regions.key_of(p_position)); }
	int64_t get_client_entity(int p_peer) const;
	PackedInt32Array get_client_peers() const;
	// Entities the client is currently told about (not counting ones being removed).
	PackedInt64Array get_client_view(int p_peer) const;

	Dictionary get_stats() const;
	Dictionary get_client_stats(int p_peer) const;
	// The input the server applied to the client's player at a recent tick, and whether it arrived in
	// time or was repeated from the tick before: {tick, move, yaw, arrived, position}, or {} if too old.
	Dictionary get_client_applied(int p_peer, int p_tick) const;

	~PlanetNetServer() { stop(); }
};
