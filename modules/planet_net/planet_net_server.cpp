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
	_configure_regions();
	return link.listen(p_port, p_max_clients);
}

void PlanetNetServer::stop() {
	link.close();
	clients.clear();
	entities.clear();
	region_index.clear();
	tick = 0;
	snapshot_seq = 0;
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

void PlanetNetServer::set_planet_radius(double p_radius) {
	planet_radius = p_radius;
	walker.radius = p_radius;
	_configure_regions();
}

void PlanetNetServer::set_region_size(double p_size) {
	region_size = p_size;
	_configure_regions();
}

void PlanetNetServer::_configure_regions() {
	ERR_FAIL_COND_MSG(!clients.is_empty(), "Regions cannot change while clients are connected: they have been told the layout.");
	regions.configure(planet_radius, region_size);
	region_index.clear();
	for (KeyValue<uint64_t, Entity> &E : entities) {
		E.value.indexed = false;
		_index(E.key, E.value);
	}
}

void PlanetNetServer::_index(uint64_t p_id, Entity &r_entity) {
	r_entity.region = regions.key_of(r_entity.state.position);
	region_index[r_entity.region].push_back(p_id);
	r_entity.indexed = true;
}

void PlanetNetServer::_unindex(uint64_t p_id, Entity &r_entity) {
	if (!r_entity.indexed) {
		return;
	}
	LocalVector<uint64_t> *ids = region_index.getptr(r_entity.region);
	if (ids != nullptr) {
		const int64_t at = ids->find(p_id);
		if (at >= 0) {
			ids->remove_at_unordered(uint32_t(at));
		}
		if (ids->is_empty()) {
			region_index.erase(r_entity.region);
		}
	}
	r_entity.indexed = false;
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
	_index(id, entities[id]);
	return int64_t(id);
}

void PlanetNetServer::destroy_entity(int64_t p_id) {
	Entity *e = entities.getptr(uint64_t(p_id));
	if (e == nullptr) {
		return;
	}
	_unindex(uint64_t(p_id), *e);
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

void PlanetNetServer::set_entity_state(int64_t p_id, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_velocity) {
	Entity *e = entities.getptr(uint64_t(p_id));
	ERR_FAIL_NULL(e);
	e->state.position = p_position;
	e->state.rotation = p_rotation;
	e->state.velocity = p_velocity;
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

PackedInt64Array PlanetNetServer::get_client_view(int p_peer) const {
	PackedInt64Array ids;
	const Client *c = clients.getptr(p_peer);
	if (c != nullptr) {
		for (const KeyValue<uint64_t, View> &V : c->views) {
			if (!V.value.removing) {
				ids.push_back(int64_t(V.key));
			}
		}
	}
	return ids;
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
						destroy_entity(int64_t(c->entity));
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
			const int64_t id = create_entity(ENTITY_TYPE_PLAYER, walker.surface_point(dir));
			entities[uint64_t(id)].owner_peer = p_peer;
			client->entity = uint64_t(id);
			client->welcomed = true;

			Writer w;
			w.u8(MSG_WELCOME);
			w.u16(PROTOCOL_VERSION);
			w.u16(server_id);
			w.u64(uint64_t(id));
			w.f64(regions.radius);
			w.u32(regions.resolution);
			w.f64(walker.speed);
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
			const uint32_t newest = r.u32();
			const uint32_t bits = r.u32();
			const uint8_t n = r.u8();
			if (!r.ok()) {
				return;
			}
			if (newest != 0) {
				_acknowledge(*client, newest);
				for (uint32_t k = 0; k < 32; ++k) {
					if ((bits & (1u << k)) && newest > k + 1) {
						_acknowledge(*client, newest - 1 - k);
					}
				}
			}
			// Each packet repeats the last few inputs, so one lost packet costs nothing. Inputs for ticks
			// already simulated are too late to matter.
			for (uint8_t i = 0; i < n; ++i) {
				const uint32_t input_tick = r.u32();
				WalkerInput in;
				in.move.x = r.f32();
				in.move.y = r.f32();
				in.yaw = r.f32();
				if (r.ok() && input_tick > tick && input_tick <= tick + 4 * TICK_HZ) {
					client->inputs[input_tick] = in;
				}
			}
		} break;
		default:
			break;
	}
}

void PlanetNetServer::_acknowledge(Client &r_client, uint32_t p_seq) {
	SentRecord &rec = r_client.sent[p_seq % SENT_RECORDS];
	if (!rec.valid || rec.seq != p_seq) {
		return;
	}
	for (uint32_t k = 0; k < rec.ids.size(); ++k) {
		View *view = r_client.views.getptr(rec.ids[k]);
		if (view != nullptr && !view->removing && view->epoch == rec.epochs[k] &&
				(!view->has_baseline || rec.tick > view->baseline_tick)) {
			view->has_baseline = true;
			view->baseline_tick = rec.tick;
			view->baseline = rec.wires[k];
		}
	}
	for (uint32_t k = 0; k < rec.removals.size(); ++k) {
		View *view = r_client.views.getptr(rec.removals[k]);
		if (view != nullptr && view->removing && view->epoch == rec.removal_epochs[k]) {
			r_client.free_handles.push_back(view->handle);
			r_client.views.erase(rec.removals[k]);
		}
	}
	r_client.last_acked_seq = MAX(r_client.last_acked_seq, p_seq);
	rec.valid = false;
}

void PlanetNetServer::_step() {
	++tick;
	const double dt = 1.0 / TICK_HZ;
	for (KeyValue<int, Client> &C : clients) {
		Client &client = C.value;
		Entity *e = client.entity != 0 ? entities.getptr(client.entity) : nullptr;
		if (e == nullptr) {
			continue;
		}
		const WalkerInput *arrived = client.inputs.getptr(tick);
		if (arrived != nullptr) {
			client.last_input = *arrived;
			client.has_input = true;
			++client.inputs_applied;
		} else if (client.has_input) {
			++client.inputs_missed;
		}
		client.inputs.erase(tick);
		WalkerState s;
		s.position = e->state.position;
		s.velocity = e->state.velocity;
		s.rotation = e->state.rotation;
		s = walker.step(s, client.last_input, dt);
		e->state.position = s.position;
		e->state.velocity = s.velocity;
		e->state.rotation = s.rotation;
		// Reported even before any input arrives, so the client starts predicting from the exact state.
		client.last_applied_tick = tick;
		const int slot = int(tick % HISTORY_TICKS);
		client.applied_tick[slot] = tick;
		client.applied_input[slot] = client.last_input;
		client.applied_arrived[slot] = arrived != nullptr;
		client.applied_position[slot] = s.position;
	}
	for (KeyValue<uint64_t, Entity> &E : entities) {
		_record_history(E.value);
		const uint32_t key = regions.key_of(E.value.state.position);
		if (!E.value.indexed || key != E.value.region) {
			_unindex(E.key, E.value);
			_index(E.key, E.value);
		}
	}
	// SNAPSHOT_HZ out of TICK_HZ ticks carry a snapshot, spread evenly.
	if ((uint64_t(tick) * SNAPSHOT_HZ) / TICK_HZ != (uint64_t(tick - 1) * SNAPSHOT_HZ) / TICK_HZ) {
		++snapshot_seq;
		for (KeyValue<int, Client> &C : clients) {
			if (C.value.welcomed) {
				_send_snapshot(C.key, C.value);
			}
		}
	}
}

void PlanetNetServer::_interest_regions(const Vector3 &p_centre, HashSet<uint32_t> &r_regions) const {
	// Sample the tangent disk at half a region's spacing and collect the regions the samples land
	// in. Works across cube faces with no special cases, because every sample is simply mapped.
	const Vector3 up = p_centre.normalized();
	Vector3 north = Vector3(0, 1, 0) - up * up.y;
	if (north.length_squared() < 1e-12) {
		north = Vector3(0, 0, -1) - up * up.z;
	}
	north.normalize();
	const Vector3 east = north.cross(up);
	const double step = regions.cell_size() * 0.5;
	const double reach = interest_radius + regions.cell_size();
	const int n = int(Math::ceil(reach / step));
	const double limit = (reach / step) * (reach / step) + 1.0;
	for (int a = -n; a <= n; ++a) {
		for (int b = -n; b <= n; ++b) {
			if (double(a * a + b * b) > limit) {
				continue;
			}
			r_regions.insert(regions.key_of(p_centre + (east * double(a) + north * double(b)) * step));
		}
	}
}

void PlanetNetServer::_send_snapshot(int p_peer, Client &r_client) {
	const Entity *self = r_client.entity != 0 ? entities.getptr(r_client.entity) : nullptr;
	if (self == nullptr) {
		return;
	}
	const Vector3 centre = self->state.position;

	HashSet<uint32_t> interest;
	_interest_regions(centre, interest);

	struct Candidate {
		uint64_t id = 0;
		double priority = 0.0;
		bool operator<(const Candidate &p_other) const { return priority > p_other.priority; }
	};
	LocalVector<Candidate> candidates;
	HashSet<uint64_t> wanted;
	for (const uint32_t key : interest) {
		const LocalVector<uint64_t> *ids = region_index.getptr(key);
		if (ids == nullptr) {
			continue;
		}
		for (const uint64_t id : *ids) {
			const Entity &e = entities[id];
			wanted.insert(id);
			View *view = r_client.views.getptr(id);
			if (view == nullptr) {
				uint16_t handle = 0;
				if (!r_client.free_handles.is_empty()) {
					handle = r_client.free_handles[r_client.free_handles.size() - 1];
					r_client.free_handles.resize(r_client.free_handles.size() - 1);
				} else if (r_client.next_handle <= 0xffff) {
					handle = uint16_t(r_client.next_handle++);
				} else {
					continue; // 65535 entities in one client's view: the rest wait for a free handle
				}
				view = &r_client.views.insert(id, View())->value;
				view->handle = handle;
				view->priority = 1000.0; // new to this client: soon
			} else if (view->removing) {
				view->removing = false;
				view->has_baseline = false;
				view->priority += 1000.0;
			}
			// Everything gains priority every snapshot, near things much faster: the weight falls with
			// the square of distance past 100 m, so on a tight budget what is close stays smooth, and the
			// floor means nothing far starves. The client's own player always goes first.
			const double distance = centre.distance_to(e.state.position);
			const double nearness = 400.0 / (100.0 + distance);
			view->priority += 0.3 + nearness * nearness;
			if (id == r_client.entity) {
				view->priority += 1e9;
			}
			Candidate c;
			c.id = id;
			c.priority = view->priority;
			candidates.push_back(c);
		}
	}
	r_client.in_interest = candidates.size();
	candidates.sort();

	SentRecord &rec = r_client.sent[snapshot_seq % SENT_RECORDS];
	rec.seq = snapshot_seq;
	rec.tick = tick;
	rec.valid = true;
	rec.ids.clear();
	rec.wires.clear();
	rec.epochs.clear();
	rec.removals.clear();
	rec.removal_handles.clear();
	rec.removal_epochs.clear();

	// Whatever left interest, or no longer exists, is removed until the client acknowledges it.
	for (KeyValue<uint64_t, View> &V : r_client.views) {
		if (!wanted.has(V.key) && !V.value.removing) {
			V.value.removing = true;
			V.value.has_baseline = false;
			++V.value.epoch;
		}
		if (V.value.removing && rec.removals.size() < 512) {
			rec.removals.push_back(V.key);
			rec.removal_handles.push_back(V.value.handle);
			rec.removal_epochs.push_back(V.value.epoch);
		}
	}

	Writer w;
	w.u8(MSG_SNAPSHOT);
	w.u32(snapshot_seq);
	w.u32(tick);
	// The client's own player, exactly: this is what its prediction for that input tick must equal.
	w.u32(r_client.last_applied_tick);
	if (r_client.last_applied_tick != 0) {
		w.f64(double(self->state.position.x));
		w.f64(double(self->state.position.y));
		w.f64(double(self->state.position.z));
		w.f64(double(self->state.velocity.x));
		w.f64(double(self->state.velocity.y));
		w.f64(double(self->state.velocity.z));
	}
	const uint32_t count_at = w.size();
	w.u16(0);
	const uint32_t removal_bytes = 2 + 2 * rec.removals.size();
	const uint32_t budget = uint32_t(client_budget / SNAPSHOT_HZ);
	uint16_t written = 0;
	for (const Candidate &c : candidates) {
		View &view = r_client.views[c.id];
		const Wire now = Wire::encode(entities[c.id].state, regions);
		const bool usable = delta_enabled && view.has_baseline && tick - view.baseline_tick <= MAX_BASELINE_AGE;
		const uint32_t before = w.size();
		if (!w.record(view.handle, c.id, now, usable ? &view.baseline : nullptr, uint8_t(usable ? tick - view.baseline_tick : 0))) {
			view.priority = 0.0; // unchanged since the client's baseline: nothing to send
			continue;
		}
		if (w.size() + removal_bytes > budget && written > 0) {
			w.truncate(before);
			break;
		}
		view.priority = 0.0;
		rec.ids.push_back(c.id);
		rec.wires.push_back(now);
		rec.epochs.push_back(view.epoch);
		++written;
		if (written == 0xffff) {
			break;
		}
	}
	w.patch_u16(count_at, written);
	w.u16(uint16_t(rec.removal_handles.size()));
	for (const uint16_t handle : rec.removal_handles) {
		w.u16(handle);
	}
	r_client.bytes_sent += w.size();
	link.send(p_peer, CHANNEL_STATE, w.data(), false);
}

Dictionary PlanetNetServer::get_stats() const {
	Dictionary d;
	d["tick"] = tick;
	d["clients"] = clients.size();
	d["entities"] = entities.size();
	d["regions_occupied"] = region_index.size();
	d["regions_per_face_edge"] = regions.resolution;
	d["region_size_m"] = regions.cell_size();
	d["bytes_sent"] = link.get_bytes_sent();
	d["bytes_received"] = link.get_bytes_received();
	return d;
}

Dictionary PlanetNetServer::get_client_stats(int p_peer) const {
	Dictionary d;
	const Client *c = clients.getptr(p_peer);
	if (c == nullptr) {
		return d;
	}
	d["snapshot_bytes_sent"] = c->bytes_sent;
	d["in_interest"] = c->in_interest;
	d["views"] = c->views.size();
	d["last_acked_seq"] = c->last_acked_seq;
	d["inputs_applied"] = c->inputs_applied;
	d["inputs_missed"] = c->inputs_missed;
	return d;
}

Dictionary PlanetNetServer::get_client_applied(int p_peer, int p_tick) const {
	Dictionary d;
	const Client *c = clients.getptr(p_peer);
	if (c == nullptr || p_tick < 0) {
		return d;
	}
	const int slot = p_tick % HISTORY_TICKS;
	if (c->applied_tick[slot] != uint32_t(p_tick)) {
		return d;
	}
	d["tick"] = p_tick;
	d["move"] = c->applied_input[slot].move;
	d["yaw"] = c->applied_input[slot].yaw;
	d["arrived"] = c->applied_arrived[slot];
	d["position"] = c->applied_position[slot];
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
	ClassDB::bind_method(D_METHOD("set_region_size", "size"), &PlanetNetServer::set_region_size);
	ClassDB::bind_method(D_METHOD("get_region_size"), &PlanetNetServer::get_region_size);
	ClassDB::bind_method(D_METHOD("set_interest_radius", "radius"), &PlanetNetServer::set_interest_radius);
	ClassDB::bind_method(D_METHOD("get_interest_radius"), &PlanetNetServer::get_interest_radius);
	ClassDB::bind_method(D_METHOD("set_client_budget", "bytes_per_second"), &PlanetNetServer::set_client_budget);
	ClassDB::bind_method(D_METHOD("get_client_budget"), &PlanetNetServer::get_client_budget);
	ClassDB::bind_method(D_METHOD("set_delta_enabled", "enabled"), &PlanetNetServer::set_delta_enabled);
	ClassDB::bind_method(D_METHOD("is_delta_enabled"), &PlanetNetServer::is_delta_enabled);
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetNetServer::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetNetServer::get_field);
	ClassDB::bind_method(D_METHOD("set_walk_speed", "speed"), &PlanetNetServer::set_walk_speed);
	ClassDB::bind_method(D_METHOD("get_walk_speed"), &PlanetNetServer::get_walk_speed);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "server_id", PROPERTY_HINT_RANGE, "1,65535"), "set_server_id", "get_server_id");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "region_size", PROPERTY_HINT_NONE, "suffix:m"), "set_region_size", "get_region_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interest_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_interest_radius", "get_interest_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "client_budget", PROPERTY_HINT_NONE, "suffix:B/s"), "set_client_budget", "get_client_budget");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "delta_enabled"), "set_delta_enabled", "is_delta_enabled");
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
	ClassDB::bind_method(D_METHOD("set_entity_state", "id", "position", "rotation", "velocity"), &PlanetNetServer::set_entity_state);
	ClassDB::bind_method(D_METHOD("get_entity_position_at", "id", "tick"), &PlanetNetServer::get_entity_position_at);
	ClassDB::bind_method(D_METHOD("get_entity_owner", "id"), &PlanetNetServer::get_entity_owner);
	ClassDB::bind_method(D_METHOD("get_region_of", "position"), &PlanetNetServer::get_region_of);
	ClassDB::bind_method(D_METHOD("get_client_entity", "peer"), &PlanetNetServer::get_client_entity);
	ClassDB::bind_method(D_METHOD("get_client_peers"), &PlanetNetServer::get_client_peers);
	ClassDB::bind_method(D_METHOD("get_client_view", "peer"), &PlanetNetServer::get_client_view);
	ClassDB::bind_method(D_METHOD("get_stats"), &PlanetNetServer::get_stats);
	ClassDB::bind_method(D_METHOD("get_client_stats", "peer"), &PlanetNetServer::get_client_stats);
	ClassDB::bind_method(D_METHOD("get_client_applied", "peer", "tick"), &PlanetNetServer::get_client_applied);

	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "peer"), PropertyInfo(Variant::INT, "entity_id")));
	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "peer")));

	BIND_CONSTANT(ENTITY_TYPE_PLAYER);
	BIND_CONSTANT(HISTORY_TICKS);
}
