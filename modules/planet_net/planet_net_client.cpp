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
	move_table_refused = false;
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
	tracked.clear();
	handles.clear();
	newest_seq = 0;
	ack_bits = 0;
	last_ack_tick = 0;
	predicting = false;
	predicted_tick = 0;
	frames.clear();
	last_confirmed_tick = 0;
	smoothing = Vector3();
	teleports_requested = 0;
	teleport_restart = false;
	if (was) {
		emit_signal(SNAME("disconnected"));
	}
}

void PlanetNetClient::set_link_conditions(double p_latency_ms, double p_jitter_ms, double p_loss) {
	link.conditions.latency_ms = p_latency_ms;
	link.conditions.jitter_ms = p_jitter_ms;
	link.conditions.loss = p_loss;
}

void PlanetNetClient::send_world_change(const Vector3 &p_point, const PackedByteArray &p_data) {
	ERR_FAIL_COND_MSG(p_data.size() > 0xffff, "A world change is at most 65535 bytes.");
	if (!welcomed) {
		return;
	}
	Writer w;
	w.u8(MSG_WORLD_REQUEST);
	w.f64(double(p_point.x));
	w.f64(double(p_point.y));
	w.f64(double(p_point.z));
	w.u16(uint16_t(p_data.size()));
	for (int i = 0; i < p_data.size(); ++i) {
		w.u8(p_data[i]);
	}
	link.send(server_peer, CHANNEL_WORLD, w.data(), true);
}

void PlanetNetClient::set_input(const Vector2 &p_move, float p_yaw, int p_buttons) {
	// Predict with exactly what the server will receive. Inputs travel as float32 and a double build's
	// Vector2 holds doubles, so an unrounded input predicts a different step on every tick it matters.
	input.move = Vector2(float(p_move.x), float(p_move.y));
	input.yaw = p_yaw;
	input.buttons = uint16_t(p_buttons);
}

void PlanetNetClient::set_ability_input(int p_abilities, const Vector3 &p_aim) {
	// As the server receives them: the slots a byte, the aim three float32s.
	input.abilities = uint8_t(p_abilities & 63);
	input.aim = Vector3(float(p_aim.x), float(p_aim.y), float(p_aim.z));
}

void PlanetNetClient::set_look(int64_t p_look) {
	look = uint64_t(p_look);
	if (welcomed) {
		_send_look();
	}
}

void PlanetNetClient::_send_look() {
	if (look == 0) {
		return;
	}
	Writer w;
	w.u8(MSG_LOOK);
	w.u64(look);
	link.send(server_peer, CHANNEL_CONTROL, w.data(), true);
}

void PlanetNetClient::send_command(const String &p_line) {
	if (!welcomed || p_line.strip_edges().is_empty()) {
		return;
	}
	Writer w;
	w.u8(MSG_COMMAND);
	w.text(p_line);
	link.send(server_peer, CHANNEL_CONTROL, w.data(), true);
}

void PlanetNetClient::request_teleport(const Vector3 &p_point) {
	if (!welcomed) {
		return;
	}
	Writer w;
	w.u8(MSG_TELEPORT);
	w.f64(double(p_point.x));
	w.f64(double(p_point.y));
	w.f64(double(p_point.z));
	link.send(server_peer, CHANNEL_CONTROL, w.data(), true);
	++teleports_requested;
	teleport_restart = true;
	// Go there now; the server's word, when it comes, will agree to within a tick of walking.
	if (predicting) {
		predicted = walker.place(predicted, p_point);
		frames.clear();
		smoothing = Vector3();
	}
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
				const CharString key = player_key.utf8();
				const int length = MIN(key.length(), 1024);
				w.u16(uint16_t(length));
				for (int i = 0; i < length; ++i) {
					w.u8(uint8_t(key[i]));
				}
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
		_gather_bodies();
		_advance_prediction(p_delta);
		_send_input();
	}
}

// The solid bodies for prediction (Walker::body_feet): every other entity of a solid type, where its
// newest state has it. Not where the server will have it at the predicted tick, which nobody here
// knows: touching another body, a prediction can be corrected.
void PlanetNetClient::_gather_bodies() {
	walker.body_feet.clear();
	walker.body_radii.clear();
	if (solid_types.is_empty()) {
		return;
	}
	for (const KeyValue<uint64_t, Tracked> &E : tracked) {
		if (E.key == local_entity || E.value.samples.is_empty()) {
			continue;
		}
		const EntityState &st = E.value.samples[E.value.samples.size() - 1].state;
		const double *r = solid_types.getptr(int(st.type));
		if (r != nullptr && st.position.distance_squared_to(predicted.position) < 100.0) {
			walker.body_feet.push_back(st.position);
			walker.body_radii.push_back(*r);
		}
	}
}

void PlanetNetClient::_advance_prediction(double p_delta) {
	// Corrections are shown as an offset that fades, not as a jump.
	smoothing *= Math::exp(-p_delta / 0.1);
	if (!predicting) {
		return;
	}
	// Run ahead of the server by half a round trip plus a margin, so this tick's input is there
	// before the server needs it.
	const double target = server_time + 0.5 * last_rtt * TICK_HZ + prediction_lead_ticks;
	int steps = 0;
	while (double(predicted_tick) + 1.0 <= target && steps < 8) {
		++predicted_tick;
		++steps;
		predicted = walker.step(predicted, input, 1.0 / TICK_HZ);
		Frame f;
		f.tick = predicted_tick;
		f.input = input;
		f.state = predicted;
		f.view = uint32_t(MAX(get_render_time(), 0.0));
		frames.push_back(f);
	}
	while (frames.size() > PREDICTION_FRAMES) {
		frames.remove_at(0);
	}
}

void PlanetNetClient::_confirm(uint32_t p_tick, const WalkerState &p_state) {
	const Vector3 &p_position = p_state.position;
	if (!predicting) {
		// The first word from the server about our own player: predict from there.
		predicting = true;
		predicted_tick = p_tick;
		predicted = p_state;
		frames.clear();
		last_confirmed_tick = p_tick;
		return;
	}
	if (p_tick <= last_confirmed_tick) {
		return;
	}
	last_confirmed_tick = p_tick;
	int at = -1;
	for (int i = int(frames.size()) - 1; i >= 0; --i) {
		if (frames[i].tick == p_tick) {
			at = i;
			break;
		}
	}
	if (at < 0) {
		// The server is past our prediction, or it fell out of the history: start again from its word.
		if (p_tick >= predicted_tick || frames.is_empty() || p_tick < frames[0].tick) {
			++prediction_resets;
			smoothing += predicted.position - p_position;
			predicted_tick = p_tick;
			predicted = p_state;
			frames.clear();
		}
		return;
	}
	++prediction_compared;
	const Vector3 mine = frames[at].state.position;
	if (same_walk(frames[at].state, p_state)) {
		++prediction_exact;
		return;
	}
	// Mispredicted, almost always because the server did not have our input for some tick in time.
	// Take its state and replay everything we have predicted since.
	++prediction_corrections;
	prediction_worst = MAX(prediction_worst, double(mine.distance_to(p_position)));
	const Vector3 before = predicted.position;
	WalkerState s = p_state;
	s.rotation = frames[at].state.rotation;
	frames[at].state = s;
	for (uint32_t i = uint32_t(at) + 1; i < frames.size(); ++i) {
		s = walker.step(s, frames[i].input, 1.0 / TICK_HZ);
		frames[i].state = s;
	}
	predicted = s;
	smoothing += before - predicted.position;
}

Vector3 PlanetNetClient::get_predicted_position_at(int p_tick) const {
	for (int i = int(frames.size()) - 1; i >= 0; --i) {
		if (frames[i].tick == uint32_t(p_tick)) {
			return frames[i].state.position;
		}
	}
	return Vector3();
}

Dictionary PlanetNetClient::get_predicted_frame(int p_tick) const {
	Dictionary d;
	for (int i = int(frames.size()) - 1; i >= 0; --i) {
		if (frames[i].tick == uint32_t(p_tick)) {
			d["tick"] = p_tick;
			d["move"] = frames[i].input.move;
			d["yaw"] = frames[i].input.yaw;
			d["buttons"] = frames[i].input.buttons;
			d["mode"] = frames[i].state.mode;
			d["position"] = frames[i].state.position;
			d["velocity"] = frames[i].state.velocity;
			d["stamina"] = frames[i].state.stamina;
			d["action"] = frames[i].state.action;
			break;
		}
	}
	return d;
}

void PlanetNetClient::reset_prediction_stats() {
	prediction_compared = 0;
	prediction_exact = 0;
	prediction_corrections = 0;
	prediction_resets = 0;
	prediction_worst = 0.0;
}

Dictionary PlanetNetClient::get_prediction_stats() const {
	Dictionary d;
	d["predicting"] = predicting;
	d["predicted_tick"] = predicted_tick;
	d["compared"] = prediction_compared;
	d["exact"] = prediction_exact;
	d["corrections"] = prediction_corrections;
	d["resets"] = prediction_resets;
	d["worst_correction_m"] = prediction_worst;
	d["smoothing_m"] = smoothing.length();
	return d;
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
	// Once per tick: the newest predicted inputs (repeated, so a lost packet costs nothing) and the
	// snapshot acknowledgements. Before prediction starts there are no inputs, only acknowledgements.
	const uint32_t now_tick = predicting ? predicted_tick : uint32_t(MAX(server_time, 0.0));
	if (now_tick == last_ack_tick) {
		return;
	}
	last_ack_tick = now_tick;
	const uint32_t n = predicting ? MIN(frames.size(), uint32_t(INPUT_REDUNDANCY)) : 0;
	Writer w;
	w.u8(MSG_INPUT);
	w.u32(newest_seq);
	w.u32(ack_bits);
	w.u8(uint8_t(n));
	for (uint32_t i = frames.size() - n; i < frames.size(); ++i) {
		w.u32(frames[i].tick);
		w.f32(frames[i].input.move.x);
		w.f32(frames[i].input.move.y);
		w.f32(frames[i].input.yaw);
		w.u16(frames[i].input.buttons);
		w.u8(frames[i].input.abilities);
		w.f32(float(frames[i].input.aim.x));
		w.f32(float(frames[i].input.aim.y));
		w.f32(float(frames[i].input.aim.z));
		w.u32(frames[i].view);
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
			const double region_radius = r.f64();
			const uint32_t region_resolution = r.u32();
			const double walk_speed = r.f64();
			const uint64_t move_hash = r.u64();
			if (!r.ok() || version != PROTOCOL_VERSION) {
				disconnect_from_server();
				return;
			}
			const uint64_t own_hash = move_hash_override != 0 ? move_hash_override : moves().hash;
			if (move_hash != own_hash) {
				// Another move table: every move would be mispredicted. Leave, and say why.
				ERR_PRINT(vformat("PlanetNetClient: the server's move table (%s) is not this client's (%s); leaving.",
						String::num_uint64(move_hash, 16), String::num_uint64(own_hash, 16)));
				move_table_refused = true;
				disconnect_from_server();
				return;
			}
			server_id = sid;
			local_entity = entity;
			regions.radius = region_radius;
			regions.resolution = region_resolution;
			walker.radius = region_radius;
			walker.speed = walk_speed;
			welcomed = true;
			_send_look();
			emit_signal(SNAME("connected"), int64_t(entity));
		} break;
		case MSG_HIT: {
			Dictionary hit;
			hit["attacker"] = int64_t(r.u64());
			hit["target"] = int64_t(r.u64());
			hit["result"] = int(r.u8());
			hit["attack"] = int(r.u8());
			hit["damage"] = int(r.u16());
			hit["health"] = int(r.u16());
			Vector3 point;
			point.x = r.f64();
			point.y = r.f64();
			point.z = r.f64();
			hit["point"] = point;
			hit["reaction"] = int(r.u8());
			if (r.ok()) {
				emit_signal(SNAME("hit"), hit);
			}
		} break;
		case MSG_MESSAGE: {
			const uint8_t channel = r.u8();
			const String text = r.text();
			if (r.ok()) {
				emit_signal(SNAME("message"), int(channel), text);
			}
		} break;
		case MSG_HEALTH: {
			const uint16_t h = r.u16();
			const uint16_t m = r.u16();
			if (r.ok()) {
				health = h;
				health_max = m;
			}
		} break;
		case MSG_CAST: {
			Dictionary cast;
			cast["caster"] = int64_t(r.u64());
			cast["move"] = int(r.u8());
			Vector3 aim;
			aim.x = r.f64();
			aim.y = r.f64();
			aim.z = r.f64();
			cast["aim"] = aim;
			cast["tick"] = int(r.u32());
			if (r.ok()) {
				emit_signal(SNAME("cast"), cast);
			}
		} break;
		case MSG_WORLD_CHANGE: {
			const uint64_t id = r.u64();
			Vector3 point;
			point.x = r.f64();
			point.y = r.f64();
			point.z = r.f64();
			const uint16_t n = r.u16();
			PackedByteArray data;
			data.resize(n);
			for (uint16_t i = 0; i < n; ++i) {
				data.write[i] = r.u8();
			}
			if (r.ok()) {
				emit_signal(SNAME("world_change"), int64_t(id), point, data);
			}
		} break;
		case MSG_PONG: {
			const double sent = r.f64();
			const double server_ticks = r.f64();
			if (r.ok()) {
				_on_pong(sent, server_ticks);
			}
		} break;
		case MSG_SNAPSHOT:
			if (welcomed) {
				_on_snapshot(r);
			}
			break;
		default:
			break;
	}
}

void PlanetNetClient::_mark_received(uint32_t p_seq) {
	if (p_seq > newest_seq) {
		const uint32_t shift = p_seq - newest_seq;
		if (newest_seq == 0 || shift > 32) {
			ack_bits = 0;
		} else {
			ack_bits = (shift == 32 ? 0u : ack_bits << shift) | (1u << (shift - 1));
		}
		newest_seq = p_seq;
	} else if (p_seq < newest_seq && newest_seq - p_seq <= 32) {
		ack_bits |= 1u << (newest_seq - p_seq - 1);
	}
}

void PlanetNetClient::_on_snapshot(Reader &r_reader) {
	const uint32_t seq = r_reader.u32();
	const uint32_t tick = r_reader.u32();
	const uint32_t applied = r_reader.u32();
	if (applied != 0) {
		WalkerState state;
		state.position.x = r_reader.f64();
		state.position.y = r_reader.f64();
		state.position.z = r_reader.f64();
		state.velocity.x = r_reader.f64();
		state.velocity.y = r_reader.f64();
		state.velocity.z = r_reader.f64();
		read_walk(r_reader, state);
		const uint8_t teleports = r_reader.u8();
		if (r_reader.ok() && teleports == teleports_requested) {
			if (teleport_restart) {
				// The first word from after our teleport: predict afresh from it.
				teleport_restart = false;
				if (predicting) {
					smoothing += predicted.position - state.position;
				}
				predicting = false;
			}
			_confirm(applied, state);
		}
	}
	const uint16_t n = r_reader.u16();
	if (!r_reader.ok()) {
		return;
	}
	bool complete = true;
	for (uint16_t i = 0; i < n; ++i) {
		const uint16_t handle = r_reader.u16();
		const uint8_t flags = r_reader.u8();
		uint64_t id = 0;
		if (flags & RECORD_FULL) {
			id = r_reader.u64();
			handles[handle] = id;
		} else {
			const uint64_t *bound = handles.getptr(handle);
			id = bound != nullptr ? *bound : 0;
		}
		Wire wire;
		const Sample *base = nullptr;
		Tracked *t = id != 0 ? tracked.getptr(id) : nullptr;
		if (!(flags & RECORD_FULL)) {
			const uint8_t age = r_reader.u8();
			if (t != nullptr) {
				for (const Sample &s : t->samples) {
					if (s.tick == tick - age) {
						base = &s;
						break;
					}
				}
			}
			if (base != nullptr) {
				wire = base->wire;
			}
		}
		r_reader.record_fields(flags, wire);
		if (!r_reader.ok()) {
			return;
		}
		++records_received;
		if (!(flags & RECORD_FULL) && base == nullptr) {
			// The baseline is gone. Skip the record and do not acknowledge the snapshot, so the
			// server never treats a state we do not have as our baseline.
			++records_undecodable;
			complete = false;
			continue;
		}
		if (t == nullptr) {
			t = &tracked.insert(id, Tracked())->value;
		}
		Sample s;
		s.tick = tick;
		s.wire = wire;
		s.state = wire.decode(id, regions);
		// In tick order; an equal tick replaces (a resend of the same state).
		uint32_t at = t->samples.size();
		while (at > 0 && t->samples[at - 1].tick > tick) {
			--at;
		}
		if (at > 0 && t->samples[at - 1].tick == tick) {
			t->samples[at - 1] = s;
		} else {
			t->samples.insert(at, s);
		}
		while (t->samples.size() > ENTITY_HISTORY) {
			t->samples.remove_at(0);
		}
	}
	const uint16_t m = r_reader.u16();
	for (uint16_t i = 0; i < m; ++i) {
		const uint16_t handle = r_reader.u16();
		if (!r_reader.ok()) {
			return;
		}
		const uint64_t *bound = handles.getptr(handle);
		if (bound != nullptr) {
			tracked.erase(*bound);
			handles.erase(handle);
		}
	}
	if (r_reader.ok() && complete) {
		_mark_received(seq);
	}
}

bool PlanetNetClient::_sample(uint64_t p_id, EntityState &r_state) const {
	const Tracked *t = tracked.getptr(p_id);
	if (t == nullptr || t->samples.is_empty()) {
		return false;
	}
	const double time = get_render_time();
	const LocalVector<Sample> &s = t->samples;
	for (int i = int(s.size()) - 2; i >= 0; --i) {
		if (double(s[i].tick) <= time && time <= double(s[i + 1].tick)) {
			const double f = (time - double(s[i].tick)) / double(s[i + 1].tick - s[i].tick);
			r_state = s[i + 1].state;
			r_state.position = s[i].state.position.lerp(s[i + 1].state.position, f);
			r_state.rotation = s[i].state.rotation.slerp(s[i + 1].state.rotation, f);
			return true;
		}
	}
	// Before the first state: hold it. After the last: extrapolate by velocity for at most half a
	// second, then hold. Entities the server has not changed are simply not resent, so "after the last"
	// is normal for anything standing still; for moving ones it means a lost update on a tight budget,
	// where dead reckoning errs far less than stopping (measured: 0.65 m worst at a quarter second).
	if (time < double(s[0].tick)) {
		r_state = s[0].state;
		return true;
	}
	r_state = s[s.size() - 1].state;
	const double ahead = CLAMP((time - double(s[s.size() - 1].tick)) / TICK_HZ, 0.0, 0.5);
	r_state.position += r_state.velocity * ahead;
	return true;
}

PackedInt64Array PlanetNetClient::get_entity_ids() const {
	PackedInt64Array ids;
	for (const KeyValue<uint64_t, Tracked> &E : tracked) {
		ids.push_back(int64_t(E.key));
	}
	return ids;
}

int PlanetNetClient::get_entity_type(int64_t p_id) const {
	const Tracked *t = tracked.getptr(uint64_t(p_id));
	return (t != nullptr && !t->samples.is_empty()) ? int(t->samples[t->samples.size() - 1].state.type) : 0;
}

int PlanetNetClient::get_entity_update_tick(int64_t p_id) const {
	const Tracked *t = tracked.getptr(uint64_t(p_id));
	return (t != nullptr && !t->samples.is_empty()) ? int(t->samples[t->samples.size() - 1].tick) : -1;
}

Vector3 PlanetNetClient::get_entity_position(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? e.position : Vector3();
}

Quaternion PlanetNetClient::get_entity_rotation(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? e.rotation : Quaternion();
}

Vector3 PlanetNetClient::get_entity_velocity(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? e.velocity : Vector3();
}

int PlanetNetClient::get_entity_pose(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? int(e.pose) : 0;
}

int64_t PlanetNetClient::get_entity_look(int64_t p_id) const {
	EntityState e;
	return _sample(uint64_t(p_id), e) ? int64_t(e.look) : 0;
}

Dictionary PlanetNetClient::get_stats() const {
	Dictionary d;
	d["connected"] = welcomed;
	d["rtt_ms"] = get_rtt_ms();
	d["server_time"] = server_time;
	d["entities"] = tracked.size();
	d["newest_snapshot_seq"] = newest_seq;
	d["records_received"] = records_received;
	d["records_undecodable"] = records_undecodable;
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
	ClassDB::bind_method(D_METHOD("set_input", "move", "yaw", "buttons"), &PlanetNetClient::set_input, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &PlanetNetClient::set_physics_space);
	ClassDB::bind_method(D_METHOD("get_physics_space"), &PlanetNetClient::get_physics_space);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PlanetNetClient::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PlanetNetClient::get_collision_mask);
	ClassDB::bind_method(D_METHOD("add_exclude", "body"), &PlanetNetClient::add_exclude);
	ClassDB::bind_method(D_METHOD("clear_excludes"), &PlanetNetClient::clear_excludes);
	ClassDB::bind_method(D_METHOD("request_teleport", "point"), &PlanetNetClient::request_teleport);
	ClassDB::bind_method(D_METHOD("send_command", "line"), &PlanetNetClient::send_command);
	ADD_SIGNAL(MethodInfo("message", PropertyInfo(Variant::INT, "channel"), PropertyInfo(Variant::STRING, "text")));
	ClassDB::bind_method(D_METHOD("set_look", "look"), &PlanetNetClient::set_look);
	ClassDB::bind_method(D_METHOD("set_solid_type", "type", "radius"), &PlanetNetClient::set_solid_type);
	ClassDB::bind_method(D_METHOD("get_solid_types"), &PlanetNetClient::get_solid_types);
	ClassDB::bind_method(D_METHOD("get_look"), &PlanetNetClient::get_look);
	ClassDB::bind_method(D_METHOD("get_entity_look", "id"), &PlanetNetClient::get_entity_look);
	ClassDB::bind_method(D_METHOD("get_predicted_velocity"), &PlanetNetClient::get_predicted_velocity);
	ClassDB::bind_method(D_METHOD("get_predicted_mode"), &PlanetNetClient::get_predicted_mode);
	ClassDB::bind_method(D_METHOD("is_predicted_crouching"), &PlanetNetClient::is_predicted_crouching);
	ClassDB::bind_method(D_METHOD("get_predicted_stamina"), &PlanetNetClient::get_predicted_stamina);
	ClassDB::bind_method(D_METHOD("is_predicted_exhausted"), &PlanetNetClient::is_predicted_exhausted);
	ClassDB::bind_method(D_METHOD("get_predicted_action"), &PlanetNetClient::get_predicted_action);
	ClassDB::bind_method(D_METHOD("get_predicted_action_ticks"), &PlanetNetClient::get_predicted_action_ticks);
	ClassDB::bind_method(D_METHOD("get_predicted_poise"), &PlanetNetClient::get_predicted_poise);
	ClassDB::bind_method(D_METHOD("get_health"), &PlanetNetClient::get_health);
	ClassDB::bind_method(D_METHOD("get_health_max"), &PlanetNetClient::get_health_max);
	ClassDB::bind_method(D_METHOD("set_ability_input", "abilities", "aim"), &PlanetNetClient::set_ability_input);
	ClassDB::bind_method(D_METHOD("get_predicted_combat"), &PlanetNetClient::get_predicted_combat);
	ClassDB::bind_method(D_METHOD("get_predicted_normal"), &PlanetNetClient::get_predicted_normal);
	ClassDB::bind_method(D_METHOD("get_local_entity_id"), &PlanetNetClient::get_local_entity_id);
	ClassDB::bind_method(D_METHOD("get_server_time_estimate"), &PlanetNetClient::get_server_time_estimate);
	ClassDB::bind_method(D_METHOD("get_render_time"), &PlanetNetClient::get_render_time);
	ClassDB::bind_method(D_METHOD("get_rtt_ms"), &PlanetNetClient::get_rtt_ms);
	ClassDB::bind_method(D_METHOD("set_interpolation_delay_ms", "ms"), &PlanetNetClient::set_interpolation_delay_ms);
	ClassDB::bind_method(D_METHOD("get_interpolation_delay_ms"), &PlanetNetClient::get_interpolation_delay_ms);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interpolation_delay_ms", PROPERTY_HINT_RANGE, "0,500,1,suffix:ms"), "set_interpolation_delay_ms", "get_interpolation_delay_ms");
	ClassDB::bind_method(D_METHOD("add_carve", "centre", "radius"), &PlanetNetClient::add_carve);
	ClassDB::bind_method(D_METHOD("clear_carves"), &PlanetNetClient::clear_carves);
	ClassDB::bind_method(D_METHOD("add_shell", "centre", "inner_radius", "outer_radius"), &PlanetNetClient::add_shell);
	ClassDB::bind_method(D_METHOD("clear_shells"), &PlanetNetClient::clear_shells);
	ClassDB::bind_method(D_METHOD("set_player_key", "key"), &PlanetNetClient::set_player_key);
	ClassDB::bind_method(D_METHOD("get_player_key"), &PlanetNetClient::get_player_key);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "player_key"), "set_player_key", "get_player_key");
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetNetClient::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetNetClient::get_field);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::RID, "physics_space"), "set_physics_space", "get_physics_space");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ClassDB::bind_method(D_METHOD("set_prediction_lead_ticks", "ticks"), &PlanetNetClient::set_prediction_lead_ticks);
	ClassDB::bind_method(D_METHOD("get_prediction_lead_ticks"), &PlanetNetClient::get_prediction_lead_ticks);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "prediction_lead_ticks", PROPERTY_HINT_RANGE, "-10,10,0.1"), "set_prediction_lead_ticks", "get_prediction_lead_ticks");
	ClassDB::bind_method(D_METHOD("is_predicting"), &PlanetNetClient::is_predicting);
	ClassDB::bind_method(D_METHOD("get_predicted_tick"), &PlanetNetClient::get_predicted_tick);
	ClassDB::bind_method(D_METHOD("get_predicted_position"), &PlanetNetClient::get_predicted_position);
	ClassDB::bind_method(D_METHOD("get_predicted_rotation"), &PlanetNetClient::get_predicted_rotation);
	ClassDB::bind_method(D_METHOD("get_predicted_reference"), &PlanetNetClient::get_predicted_reference);
	ClassDB::bind_method(D_METHOD("get_predicted_position_at", "tick"), &PlanetNetClient::get_predicted_position_at);
	ClassDB::bind_method(D_METHOD("get_prediction_stats"), &PlanetNetClient::get_prediction_stats);
	ClassDB::bind_method(D_METHOD("get_predicted_frame", "tick"), &PlanetNetClient::get_predicted_frame);
	ClassDB::bind_method(D_METHOD("reset_prediction_stats"), &PlanetNetClient::reset_prediction_stats);
	ClassDB::bind_method(D_METHOD("get_entity_ids"), &PlanetNetClient::get_entity_ids);
	ClassDB::bind_method(D_METHOD("has_entity", "id"), &PlanetNetClient::has_entity);
	ClassDB::bind_method(D_METHOD("get_entity_type", "id"), &PlanetNetClient::get_entity_type);
	ClassDB::bind_method(D_METHOD("get_entity_update_tick", "id"), &PlanetNetClient::get_entity_update_tick);
	ClassDB::bind_method(D_METHOD("get_entity_position", "id"), &PlanetNetClient::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_rotation", "id"), &PlanetNetClient::get_entity_rotation);
	ClassDB::bind_method(D_METHOD("get_entity_velocity", "id"), &PlanetNetClient::get_entity_velocity);
	ClassDB::bind_method(D_METHOD("get_entity_pose", "id"), &PlanetNetClient::get_entity_pose);
	ClassDB::bind_method(D_METHOD("was_move_table_refused"), &PlanetNetClient::was_move_table_refused);
	ClassDB::bind_method(D_METHOD("set_move_hash_override", "hash"), &PlanetNetClient::set_move_hash_override);
	ClassDB::bind_static_method("PlanetNetClient", D_METHOD("get_move_table_hash"), &PlanetNetClient::get_move_table_hash);
	ClassDB::bind_method(D_METHOD("get_stats"), &PlanetNetClient::get_stats);

	ADD_SIGNAL(MethodInfo("connected", PropertyInfo(Variant::INT, "entity_id")));
	ADD_SIGNAL(MethodInfo("world_change", PropertyInfo(Variant::INT, "id"), PropertyInfo(Variant::VECTOR3, "point"),
			PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data")));
	ClassDB::bind_method(D_METHOD("send_world_change", "point", "data"), &PlanetNetClient::send_world_change);
	ADD_SIGNAL(MethodInfo("disconnected"));
	// A swing the server judged, involving something this client sees: {attacker, target, result
	// (PlanetNet::HitResult: 1 struck, 2 blocked, 3 guard broken, 4 dodged), attack, damage, health, point}.
	ADD_SIGNAL(MethodInfo("hit", PropertyInfo(Variant::DICTIONARY, "hit")));
	ADD_SIGNAL(MethodInfo("cast", PropertyInfo(Variant::DICTIONARY, "cast")));
}
