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
// sends snapshots. Headless; a script creates it and calls poll() every frame.
//
// Milestone 1 of planet-demo's docs/NETWORKING.md: each client that says hello gets a player entity,
// walked on the planet by its input, and every client receives every entity in full at SNAPSHOT_HZ.
// Regions, interest and delta snapshots are milestone 2.

#include "planet_net_link.h"
#include "planet_net_protocol.h"

#include "core/object/ref_counted.h"
#include "scene/resources/3d/planet_field_3d.h"

class PlanetNetServer : public RefCounted {
	GDCLASS(PlanetNetServer, RefCounted);

public:
	// Entity type the server gives each connected client.
	static constexpr int ENTITY_TYPE_PLAYER = 1;
	// Ticks of transform history kept per entity, for lag compensation and for the tests.
	static constexpr int HISTORY_TICKS = 64;

private:
	struct Client {
		uint64_t entity = 0;
		bool welcomed = false;
		uint32_t input_tick = 0;
		Vector2 move;
		float yaw = 0.f;
	};

	struct Entity {
		PlanetNet::EntityState state;
		int owner_peer = 0;
		Vector3 history[HISTORY_TICKS];
		uint32_t history_tick[HISTORY_TICKS] = {};
	};

	PlanetNetLink link;
	HashMap<int, Client> clients;
	HashMap<uint64_t, Entity> entities;
	uint16_t server_id = 1;
	uint64_t next_entity = 1;
	uint32_t tick = 0;
	double accumulator = 0.0;
	double planet_radius = 150000.0;
	double walk_speed = 5.0;
	Ref<PlanetField3D> field;

	Vector3 _surface_point(const Vector3 &p_dir) const;
	void _handle(int p_peer, const LocalVector<uint8_t> &p_data);
	void _step();
	void _send_snapshot();
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
	void set_planet_radius(double p_radius) { planet_radius = p_radius; }
	double get_planet_radius() const { return planet_radius; }
	void set_field(const Ref<PlanetField3D> &p_field) { field = p_field; }
	Ref<PlanetField3D> get_field() const { return field; }
	void set_walk_speed(double p_speed) { walk_speed = p_speed; }
	double get_walk_speed() const { return walk_speed; }

	int get_tick() const { return int(tick); }
	// The server's clock in ticks, including the fraction of the tick in progress.
	double get_time_ticks() const { return double(tick) + accumulator * PlanetNet::TICK_HZ; }

	int64_t create_entity(int p_type, const Vector3 &p_position);
	void destroy_entity(int64_t p_id);
	bool has_entity(int64_t p_id) const { return entities.has(uint64_t(p_id)); }
	PackedInt64Array get_entity_ids() const;
	Vector3 get_entity_position(int64_t p_id) const;
	void set_entity_position(int64_t p_id, const Vector3 &p_position);
	// Where the entity was at a (fractional) past tick, from the history ring. Returns the current
	// position when the tick is outside what is kept.
	Vector3 get_entity_position_at(int64_t p_id, double p_tick) const;
	int get_entity_owner(int64_t p_id) const;
	int64_t get_client_entity(int p_peer) const;
	PackedInt32Array get_client_peers() const;

	Dictionary get_stats() const;

	~PlanetNetServer() { stop(); }
};
