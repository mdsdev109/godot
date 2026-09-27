/**************************************************************************/
/*  planet_net_client.cpp                                                 */
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

#include "planet_net_client.h"

#include "core/object/class_db.h"

using namespace PlanetNet;

Error PlanetNetClient::connect_to_server(const String &p_address, int p_port) {
	disconnect_from_server();
	server_peer = link.connect_to(p_address, p_port);
	if (server_peer == 0) {
		return ERR_CANT_CONNECT;
	}
	return OK;
}

void PlanetNetClient::disconnect_from_server() {
	const bool was = welcomed;
	link.close();
	server_peer = 0;
	link_up = false;
	welcomed = false;
	local_entity = 0;
	clock = 0.0;
	server_time = 0.0;
	clock_valid = false;
	clock_samples.clear();
	next_clock_sample = 0;
	ping_timer = 0.0;
	snapshots.clear();
	recent_inputs.clear();
	last_input_tick = 0;
	if (was) {
		emit_signal(SNAME("disconnected"));
	}
}

void PlanetNetClient::set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss) {
	link.conditions.latency_ms = p_latency_ms;
	link.conditions.jitter_ms = p_jitter_ms;
	link.conditions.loss = p_loss;
}

void PlanetNetClient::set_input(const Vector2 &p_move, float p_yaw) {
	input_move = p_move;
	input_yaw = p_yaw;
}

void PlanetNetClient::poll(double p_delta) {
	if (!link.is_open()) {
		return;
	}
	clock += p_delta;

	LocalVector<PlanetNetLink::Event> events;
	link.poll(events);
	for (PlanetNetLink::Event &ev : events) {
		switch (ev.kind) {
			case PlanetNetLink::EVENT_CONNECTED: {
				link_up = true;
				Writer w;
				w.u8(MSG_HELLO);
				w.u16(PROTOCOL_VERSION);
				link.send(server_peer, CHANNEL_CONTROL, w.data(), true);
				_send_ping();
			} break;
			case PlanetNetLink::EVENT_DISCONNECTED:
				disconnect_from_server();
				return;
			case PlanetNetLink::EVENT_RECEIVED:
				_handle(ev.data);
				break;
		}
	}
	if (!link_up) {
		return;
	}

	_update_clock(p_delta);
	ping_timer += p_delta;
	// Fast while the clock is settling, then a steady trickle to follow drift and route changes.
	if (ping_timer >= (clock_samples.size() < CLOCK_SAMPLES / 2 ? 0.05 : 0.25)) {
		ping_timer = 0.0;
		_send_ping();
	}
	if (welcomed && clock_valid) {
		_send_input();
	}
}

void PlanetNetClient::_send_ping() {
	Writer w;
	w.u8(MSG_PING);
	w.f64(clock);
	link.send(server_peer, CHANNEL_STATE, w.data(), false);
}

void PlanetNetClient::_on_pong(double p_sent_clock, double p_server_ticks) {
	const double rtt = clock - p_sent_clock;
	if (rtt < 0.0) {
		return;
	}
	last_rtt = rtt;
	// The server read its clock about half a round trip ago. The offset maps local seconds to server
	// ticks; the sample with the shortest round trip has the least room for asymmetric delay, so it is
	// the one trusted.
	ClockSample s;
	s.rtt = rtt;
	s.offset = (p_server_ticks + 0.5 * rtt * TICK_HZ) - clock * TICK_HZ;
	if (clock_samples.size() < CLOCK_SAMPLES) {
		clock_samples.push_back(s);
	} else {
		clock_samples[next_clock_sample] = s;
		next_clock_sample = (next_clock_sample + 1) % CLOCK_SAMPLES;
	}
}

void PlanetNetClient::_update_clock(double p_delta) {
	server_time += p_delta * TICK_HZ;
	if (clock_samples.is_empty()) {
		return;
	}
	const ClockSample *best = &clock_samples[0];
	for (const ClockSample &s : clock_samples) {
		if (s.rtt < best->rtt) {
			best = &s;
		}
	}
	const double target = clock * TICK_HZ + best->offset;
	const double error = target - server_time;
	if (!clock_valid || Math::abs(error) > TICK_HZ) {
		// First estimate, or more than a second out: jump rather than crawl.
		server_time = target;
		clock_valid = true;
	} else {
		// Otherwise steer, so render time never jumps backwards visibly.
		server_time += error * MIN(1.0, p_delta * 2.0);
	}
}

void PlanetNetClient::_send_input() {
	const uint32_t now_tick = uint32_t(MAX(server_time, 0.0));
	if (now_tick == last_input_tick && !recent_inputs.is_empty()) {
		return;
	}
	last_input_tick = now_tick;
	InputCommand cmd;
	cmd.tick = now_tick;
	cmd.move = input_move;
	cmd.yaw = input_yaw;
	if (recent_inputs.size() >= INPUT_REDUNDANCY) {
		recent_inputs.remove_at(0);
	}
	recent_inputs.push_back(cmd);

	Writer w;
	w.u8(MSG_INPUT);
	w.u8(uint8_t(recent_inputs.size()));
	for (const InputCommand &c : recent_inputs) {
		w.u32(c.tick);
		w.f32(c.move.x);
		w.f32(c.move.y);
		w.f32(c.yaw);
	}
	link.send(server_peer, CHANNEL_STATE, w.data(), false);
}

void PlanetNetClient::_handle(const LocalVector<uint8_t> &p_data) {
	if (p_data.is_empty()) {
		return;
	}
	Reader r(p_data.ptr(), p_data.size());
	const uint8_t type = r.u8();
	switch (type) {
		case MSG_WELCOME: {
			const uint16_t version = r.u16();
			const uint16_t sid = r.u16();
			const uint64_t entity = r.u64();
			r.f64(); // the server's clock; the ping exchange measures it properly
			if (!r.ok() || version != PROTOCOL_VERSION) {
				disconnect_from_server();
				return;
			}
			server_id = sid;
			local_entity = entity;
			welcomed = true;
			emit_signal(SNAME("connected"), int64_t(entity));
		} break;
		case MSG_PONG: {
			const double sent = r.f64();
			const double server_ticks = r.f64();
			if (r.ok()) {
				_on_pong(sent, server_ticks);
			}
		} break;
		case MSG_SNAPSHOT: {
			Snapshot snap;
			snap.tick = r.u32();
			const uint16_t n = r.u16();
			snap.entities.resize(n);
			for (uint16_t i = 0; i < n; ++i) {
				snap.entities[i] = r.entity();
			}
			if (!r.ok()) {
				return;
			}
			// Unreliable delivery can reorder or repeat; keep the buffer sorted and unique by tick.
			uint32_t at = snapshots.size();
			while (at > 0 && snapshots[at - 1].tick > snap.tick) {
				--at;
			}
			if (at > 0 && snapshots[at - 1].tick == snap.tick) {
				return;
			}
			snapshots.insert(at, snap);
			while (snapshots.size() > SNAPSHOT_BUFFER) {
				snapshots.remove_at(0);
			}
		} break;
		default:
			break;
	}
}

bool PlanetNetClient::_find(const Snapshot &p_snapshot, uint64_t p_id, EntityState &r_state) const {
	for (const EntityState &e : p_snapshot.entities) {
		if (e.id == p_id) {
			r_state = e;
			return true;
		}
	}
	return false;
}

bool PlanetNetClient::_sample(uint64_t p_id, EntityState &r_state) const {
	if (snapshots.is_empty()) {
		return false;
	}
	const double t = get_render_time();
	// The pair of snapshots around the render time that both contain the entity.
	for (int i = int(snapshots.size()) - 2; i >= 0; --i) {
		const Snapshot &a = snapshots[i];
		const Snapshot &b = snapshots[i + 1];
		if (double(a.tick) <= t && t <= double(b.tick)) {
			EntityState ea;
			EntityState eb;
			if (_find(a, p_id, ea) && _find(b, p_id, eb)) {
				const double f = (t - double(a.tick)) / double(b.tick - a.tick);
				r_state = eb;
				r_state.position = ea.position.lerp(eb.position, f);
				r_state.rotation = ea.rotation.slerp(eb.rotation, f);
				return true;
			}
			break;
		}
	}
	// Outside the buffer, or the entity is new: newest state that has it, extrapolated by its velocity
	// for at most a quarter of a second past that snapshot, then held.
	for (int i = int(snapshots.size()) - 1; i >= 0; --i) {
		if (_find(snapshots[i], p_id, r_state)) {
			const double ahead = CLAMP((t - double(snapshots[i].tick)) / TICK_HZ, 0.0, 0.25);
			r_state.position += r_state.velocity * ahead;
			return true;
		}
	}
	return false;
}

PackedInt64Array PlanetNetClient::get_entity_ids() const {
	PackedInt64Array ids;
	if (!snapshots.is_empty()) {
		for (const EntityState &e : snapshots[snapshots.size() - 1].entities) {
			ids.push_back(int64_t(e.id));
		}
	}
	return ids;
}

bool PlanetNetClient::has_entity(int64_t p_id) const {
	EntityState e;
	return !snapshots.is_empty() && _find(snapshots[snapshots.size() - 1], uint64_t(p_id), e);
}

int PlanetNetClient::get_entity_type(int64_t p_id) const {
	EntityState e;
	return (!snapshots.is_empty() && _find(snapshots[snapshots.size() - 1], uint64_t(p_id), e)) ? int(e.type) : 0;
}

Vector3 PlanetNetClient::get_entity_position(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? e.position : Vector3();
}

Quaternion PlanetNetClient::get_entity_rotation(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? e.rotation : Quaternion();
}

Dictionary PlanetNetClient::get_stats() const {
	Dictionary d;
	d["connected"] = welcomed;
	d["rtt_ms"] = get_rtt_ms();
	d["server_time"] = server_time;
	d["snapshots"] = snapshots.size();
	d["newest_snapshot_tick"] = snapshots.is_empty() ? -1 : int64_t(snapshots[snapshots.size() - 1].tick);
	d["bytes_sent"] = link.get_bytes_sent();
	d["bytes_received"] = link.get_bytes_received();
	return d;
}

void PlanetNetClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("connect_to_server", "address", "port"), &PlanetNetClient::connect_to_server);
	ClassDB::bind_method(D_METHOD("disconnect_from_server"), &PlanetNetClient::disconnect_from_server);
	ClassDB::bind_method(D_METHOD("is_server_connected"), &PlanetNetClient::is_server_connected);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &PlanetNetClient::poll);
	ClassDB::bind_method(D_METHOD("set_link_conditions", "latency_ms", "jitter_ms", "loss"), &PlanetNetClient::set_link_conditions);
	ClassDB::bind_method(D_METHOD("set_input", "move", "yaw"), &PlanetNetClient::set_input);
	ClassDB::bind_method(D_METHOD("get_local_entity_id"), &PlanetNetClient::get_local_entity_id);
	ClassDB::bind_method(D_METHOD("get_server_time_estimate"), &PlanetNetClient::get_server_time_estimate);
	ClassDB::bind_method(D_METHOD("get_render_time"), &PlanetNetClient::get_render_time);
	ClassDB::bind_method(D_METHOD("get_rtt_ms"), &PlanetNetClient::get_rtt_ms);
	ClassDB::bind_method(D_METHOD("set_interpolation_delay_ms", "ms"), &PlanetNetClient::set_interpolation_delay_ms);
	ClassDB::bind_method(D_METHOD("get_interpolation_delay_ms"), &PlanetNetClient::get_interpolation_delay_ms);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interpolation_delay_ms", PROPERTY_HINT_RANGE, "0,500,1,suffix:ms"), "set_interpolation_delay_ms", "get_interpolation_delay_ms");
	ClassDB::bind_method(D_METHOD("get_entity_ids"), &PlanetNetClient::get_entity_ids);
	ClassDB::bind_method(D_METHOD("has_entity", "id"), &PlanetNetClient::has_entity);
	ClassDB::bind_method(D_METHOD("get_entity_type", "id"), &PlanetNetClient::get_entity_type);
	ClassDB::bind_method(D_METHOD("get_entity_position", "id"), &PlanetNetClient::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_rotation", "id"), &PlanetNetClient::get_entity_rotation);
	ClassDB::bind_method(D_METHOD("get_stats"), &PlanetNetClient::get_stats);

	ADD_SIGNAL(MethodInfo("connected", PropertyInfo(Variant::INT, "entity_id")));
	ADD_SIGNAL(MethodInfo("disconnected"));
}
