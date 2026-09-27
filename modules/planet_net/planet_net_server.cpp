/**************************************************************************/
/*  planet_net_server.cpp                                                 */
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

#include "planet_net_server.h"

#include "core/math/planet_det_math.h"
#include "core/object/class_db.h"

using namespace PlanetNet;

Error PlanetNetServer::listen(int p_port, int p_max_clients) {
	stop();
	return link.listen(p_port, p_max_clients);
}

void PlanetNetServer::stop() {
	link.close();
	clients.clear();
	entities.clear();
	tick = 0;
	accumulator = 0.0;
}

void PlanetNetServer::set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss) {
	link.conditions.latency_ms = p_latency_ms;
	link.conditions.jitter_ms = p_jitter_ms;
	link.conditions.loss = p_loss;
}

void PlanetNetServer::set_server_id(int p_id) {
	ERR_FAIL_COND_MSG(p_id < 1 || p_id > 0xffff, "Server id must be in [1, 65535].");
	server_id = uint16_t(p_id);
}

Vector3 PlanetNetServer::_surface_point(const Vector3 &p_dir) const {
	const Vector3 dir = p_dir.normalized();
	return dir * (field.is_valid() ? double(field->eval_radius(dir)) : planet_radius);
}

int64_t PlanetNetServer::create_entity(int p_type, const Vector3 &p_position) {
	Entity e;
	e.state.id = (uint64_t(server_id) << ENTITY_SERVER_SHIFT) | next_entity++;
	e.state.type = uint16_t(p_type);
	e.state.position = p_position;
	for (int i = 0; i < HISTORY_TICKS; ++i) {
		e.history[i] = p_position;
		e.history_tick[i] = tick;
	}
	const uint64_t id = e.state.id;
	entities.insert(id, e);
	return int64_t(id);
}

void PlanetNetServer::destroy_entity(int64_t p_id) {
	entities.erase(uint64_t(p_id));
}

PackedInt64Array PlanetNetServer::get_entity_ids() const {
	PackedInt64Array ids;
	for (const KeyValue<uint64_t, Entity> &E : entities) {
		ids.push_back(int64_t(E.key));
	}
	return ids;
}

Vector3 PlanetNetServer::get_entity_position(int64_t p_id) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	return e != nullptr ? e->state.position : Vector3();
}

void PlanetNetServer::set_entity_position(int64_t p_id, const Vector3 &p_position) {
	Entity *e = entities.getptr(uint64_t(p_id));
	ERR_FAIL_NULL(e);
	e->state.position = p_position;
}

Vector3 PlanetNetServer::get_entity_position_at(int64_t p_id, double p_tick) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	if (e == nullptr) {
		return Vector3();
	}
	const double t0 = Math::floor(p_tick);
	if (t0 < 0.0 || t0 + 1.0 > double(tick) || double(tick) - t0 >= HISTORY_TICKS) {
		return e->state.position;
	}
	const uint32_t a = uint32_t(t0);
	const uint32_t b = a + 1;
	const int ia = int(a % HISTORY_TICKS);
	const int ib = int(b % HISTORY_TICKS);
	if (e->history_tick[ia] != a || e->history_tick[ib] != b) {
		return e->state.position;
	}
	return e->history[ia].lerp(e->history[ib], p_tick - t0);
}

int PlanetNetServer::get_entity_owner(int64_t p_id) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	return e != nullptr ? e->owner_peer : 0;
}

int64_t PlanetNetServer::get_client_entity(int p_peer) const {
	const Client *c = clients.getptr(p_peer);
	return c != nullptr ? int64_t(c->entity) : 0;
}

PackedInt32Array PlanetNetServer::get_client_peers() const {
	PackedInt32Array peers;
	for (const KeyValue<int, Client> &E : clients) {
		peers.push_back(E.key);
	}
	return peers;
}

void PlanetNetServer::_record_history(Entity &r_entity) {
	const int i = int(tick % HISTORY_TICKS);
	r_entity.history[i] = r_entity.state.position;
	r_entity.history_tick[i] = tick;
}

void PlanetNetServer::poll(double p_delta) {
	if (!link.is_open()) {
		return;
	}
	LocalVector<PlanetNetLink::Event> events;
	link.poll(events);
	for (PlanetNetLink::Event &ev : events) {
		switch (ev.kind) {
			case PlanetNetLink::EVENT_CONNECTED:
				clients.insert(ev.peer, Client());
				break;
			case PlanetNetLink::EVENT_DISCONNECTED: {
				const Client *c = clients.getptr(ev.peer);
				if (c != nullptr) {
					if (c->entity != 0) {
						entities.erase(c->entity);
					}
					clients.erase(ev.peer);
					emit_signal(SNAME("client_disconnected"), ev.peer);
				}
			} break;
			case PlanetNetLink::EVENT_RECEIVED:
				_handle(ev.peer, ev.data);
				break;
		}
	}

	// A long hitch runs at most a quarter second of ticks rather than spiralling.
	accumulator = MIN(accumulator + p_delta, 0.25);
	const double dt = 1.0 / TICK_HZ;
	while (accumulator >= dt) {
		accumulator -= dt;
		_step();
	}
}

void PlanetNetServer::_handle(int p_peer, const LocalVector<uint8_t> &p_data) {
	Client *client = clients.getptr(p_peer);
	if (client == nullptr || p_data.is_empty()) {
		return;
	}
	Reader r(p_data.ptr(), p_data.size());
	const uint8_t type = r.u8();
	switch (type) {
		case MSG_HELLO: {
			const uint16_t version = r.u16();
			if (!r.ok() || version != PROTOCOL_VERSION) {
				link.disconnect_peer(p_peer);
				return;
			}
			if (client->welcomed) {
				return;
			}
			// Players start near the +Y pole, spread out so they do not stack.
			const double spread = 20.0 / planet_radius;
			const Vector3 dir = Vector3(spread * double(p_peer % 8), 1.0, spread * double(p_peer / 8)).normalized();
			const int64_t id = create_entity(ENTITY_TYPE_PLAYER, _surface_point(dir));
			entities[uint64_t(id)].owner_peer = p_peer;
			client->entity = uint64_t(id);
			client->welcomed = true;

			Writer w;
			w.u8(MSG_WELCOME);
			w.u16(PROTOCOL_VERSION);
			w.u16(server_id);
			w.u64(uint64_t(id));
			w.f64(get_time_ticks());
			link.send(p_peer, CHANNEL_CONTROL, w.data(), true);
			emit_signal(SNAME("client_connected"), p_peer, id);
		} break;
		case MSG_PING: {
			const double client_clock = r.f64();
			if (!r.ok()) {
				return;
			}
			Writer w;
			w.u8(MSG_PONG);
			w.f64(client_clock);
			w.f64(get_time_ticks());
			link.send(p_peer, CHANNEL_STATE, w.data(), false);
		} break;
		case MSG_INPUT: {
			const uint8_t n = r.u8();
			// Redundant copies of recent input; only the newest one matters until milestone 3 applies
			// them tick by tick.
			for (uint8_t i = 0; i < n; ++i) {
				const uint32_t input_tick = r.u32();
				const float mx = r.f32();
				const float my = r.f32();
				const float yaw = r.f32();
				if (r.ok() && input_tick >= client->input_tick) {
					client->input_tick = input_tick;
					client->move = Vector2(CLAMP(mx, -1.f, 1.f), CLAMP(my, -1.f, 1.f));
					client->yaw = yaw;
				}
			}
		} break;
		default:
			break;
	}
}

void PlanetNetServer::_step() {
	++tick;
	const double dt = 1.0 / TICK_HZ;
	for (KeyValue<int, Client> &C : clients) {
		Entity *e = C.value.entity != 0 ? entities.getptr(C.value.entity) : nullptr;
		if (e == nullptr) {
			continue;
		}
		// Walk on the surface: yaw is measured from local north, move is (right, forward) in [-1, 1].
		const Vector3 up = e->state.position.normalized();
		Vector3 north = Vector3(0, 1, 0) - up * up.y;
		if (north.length_squared() < 1e-12) {
			north = Vector3(0, 0, -1) - up * up.z;
		}
		north.normalize();
		const Vector3 east = north.cross(up);
		const Vector3 forward = north * PlanetDetMath::cos(double(C.value.yaw)) + east * PlanetDetMath::sin(double(C.value.yaw));
		const Vector3 right = forward.cross(up);
		Vector3 velocity = (forward * double(C.value.move.y) + right * double(C.value.move.x)) * walk_speed;
		if (velocity.length() > walk_speed) {
			velocity = velocity.normalized() * walk_speed;
		}
		const Vector3 moved = _surface_point(e->state.position + velocity * dt);
		e->state.velocity = (moved - e->state.position) / dt;
		e->state.position = moved;
		e->state.rotation = Basis(right, up, -forward).get_rotation_quaternion();
	}
	for (KeyValue<uint64_t, Entity> &E : entities) {
		_record_history(E.value);
	}
	// SNAPSHOT_HZ out of TICK_HZ ticks carry a snapshot, spread evenly.
	if ((uint64_t(tick) * SNAPSHOT_HZ) / TICK_HZ != (uint64_t(tick - 1) * SNAPSHOT_HZ) / TICK_HZ) {
		_send_snapshot();
	}
}

void PlanetNetServer::_send_snapshot() {
	Writer w;
	w.u8(MSG_SNAPSHOT);
	w.u32(tick);
	w.u16(uint16_t(MIN(entities.size(), uint32_t(0xffff))));
	uint32_t written = 0;
	for (const KeyValue<uint64_t, Entity> &E : entities) {
		if (written++ >= 0xffff) {
			break;
		}
		w.entity(E.value.state);
	}
	for (const KeyValue<int, Client> &C : clients) {
		if (C.value.welcomed) {
			link.send(C.key, CHANNEL_STATE, w.data(), false);
		}
	}
}

Dictionary PlanetNetServer::get_stats() const {
	Dictionary d;
	d["tick"] = tick;
	d["clients"] = clients.size();
	d["entities"] = entities.size();
	d["bytes_sent"] = link.get_bytes_sent();
	d["bytes_received"] = link.get_bytes_received();
	return d;
}

void PlanetNetServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("listen", "port", "max_clients"), &PlanetNetServer::listen, DEFVAL(64));
	ClassDB::bind_method(D_METHOD("stop"), &PlanetNetServer::stop);
	ClassDB::bind_method(D_METHOD("is_listening"), &PlanetNetServer::is_listening);
	ClassDB::bind_method(D_METHOD("get_port"), &PlanetNetServer::get_port);
	ClassDB::bind_method(D_METHOD("poll", "delta"), &PlanetNetServer::poll);
	ClassDB::bind_method(D_METHOD("set_link_conditions", "latency_ms", "jitter_ms", "loss"), &PlanetNetServer::set_link_conditions);

	ClassDB::bind_method(D_METHOD("set_server_id", "id"), &PlanetNetServer::set_server_id);
	ClassDB::bind_method(D_METHOD("get_server_id"), &PlanetNetServer::get_server_id);
	ClassDB::bind_method(D_METHOD("set_planet_radius", "radius"), &PlanetNetServer::set_planet_radius);
	ClassDB::bind_method(D_METHOD("get_planet_radius"), &PlanetNetServer::get_planet_radius);
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetNetServer::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetNetServer::get_field);
	ClassDB::bind_method(D_METHOD("set_walk_speed", "speed"), &PlanetNetServer::set_walk_speed);
	ClassDB::bind_method(D_METHOD("get_walk_speed"), &PlanetNetServer::get_walk_speed);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "server_id", PROPERTY_HINT_RANGE, "1,65535"), "set_server_id", "get_server_id");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "walk_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_walk_speed", "get_walk_speed");

	ClassDB::bind_method(D_METHOD("get_tick"), &PlanetNetServer::get_tick);
	ClassDB::bind_method(D_METHOD("get_time_ticks"), &PlanetNetServer::get_time_ticks);
	ClassDB::bind_method(D_METHOD("create_entity", "type", "position"), &PlanetNetServer::create_entity);
	ClassDB::bind_method(D_METHOD("destroy_entity", "id"), &PlanetNetServer::destroy_entity);
	ClassDB::bind_method(D_METHOD("has_entity", "id"), &PlanetNetServer::has_entity);
	ClassDB::bind_method(D_METHOD("get_entity_ids"), &PlanetNetServer::get_entity_ids);
	ClassDB::bind_method(D_METHOD("get_entity_position", "id"), &PlanetNetServer::get_entity_position);
	ClassDB::bind_method(D_METHOD("set_entity_position", "id", "position"), &PlanetNetServer::set_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_position_at", "id", "tick"), &PlanetNetServer::get_entity_position_at);
	ClassDB::bind_method(D_METHOD("get_entity_owner", "id"), &PlanetNetServer::get_entity_owner);
	ClassDB::bind_method(D_METHOD("get_client_entity", "peer"), &PlanetNetServer::get_client_entity);
	ClassDB::bind_method(D_METHOD("get_client_peers"), &PlanetNetServer::get_client_peers);
	ClassDB::bind_method(D_METHOD("get_stats"), &PlanetNetServer::get_stats);

	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "peer"), PropertyInfo(Variant::INT, "entity_id")));
	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "peer")));

	BIND_CONSTANT(ENTITY_TYPE_PLAYER);
	BIND_CONSTANT(HISTORY_TICKS);
}
