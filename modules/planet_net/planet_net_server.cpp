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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

using namespace PlanetNet;

Error PlanetNetServer::listen(int p_port, int p_max_clients) {
	ERR_FAIL_COND_V_MSG(link.is_open(), ERR_ALREADY_IN_USE, "Already listening; stop() first.");
	_configure_regions();
	return link.listen(p_port, p_max_clients);
}

// Closes the host. The world stays: connected players are recorded under their keys and leave it,
// everything else remains, so a script can save_world() after stopping.
void PlanetNetServer::stop() {
	for (KeyValue<int, Client> &C : clients) {
		_release_client(C.value);
	}
	clients.clear();
	link.close();
	tick = 0;
	snapshot_seq = 0;
	accumulator = 0.0;
}

void PlanetNetServer::_release_client(Client &r_client) {
	if (r_client.entity == 0) {
		return;
	}
	const Entity *e = entities.getptr(r_client.entity);
	if (!r_client.player_key.is_empty()) {
		PlayerRecord *rec = players.getptr(r_client.player_key);
		if (rec != nullptr) {
			if (e != nullptr) {
				rec->state = e->state;
			}
			rec->online = false;
		}
	}
	destroy_entity(int64_t(r_client.entity));
	r_client.entity = 0;
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

void PlanetNetServer::set_spawn_direction(const Vector3 &p_direction) {
	ERR_FAIL_COND_MSG(p_direction.length_squared() < 1e-12, "Spawn direction must not be zero.");
	spawn_direction = p_direction.normalized();
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
	return int64_t(_create_with_id((uint64_t(server_id) << ENTITY_SERVER_SHIFT) | next_entity++, p_type, p_position));
}

int64_t PlanetNetServer::expect_player(const String &p_key, const Vector3 &p_position, bool p_noclip) {
	ERR_FAIL_COND_V(p_key.is_empty(), 0);
	const uint64_t already = _awaiting(p_key);
	if (already != 0) {
		destroy_entity(int64_t(already));
	}
	const int64_t id = create_entity(ENTITY_TYPE_PLAYER, p_position);
	Entity *e = entities.getptr(uint64_t(id));
	ERR_FAIL_NULL_V(e, 0);
	e->await_key = p_key;
	e->persistent = false;
	e->team = TEAM_PLAYERS;
	e->health_max = player_health;
	e->health = player_health;
	if (p_noclip) {
		set_entity_noclip(id, true);
	}
	return id;
}

int64_t PlanetNetServer::create_npc(int p_type, const Vector3 &p_position) {
	const int64_t id = create_entity(p_type, p_position);
	Entity *e = entities.getptr(uint64_t(id));
	ERR_FAIL_NULL_V(e, 0);
	e->npc = true;
	e->persistent = false;
	e->team = TEAM_ENEMIES;
	WalkerState s = walker.place(walker_state(*e), p_position);
	s.flags |= FLAG_NO_CLIMB; // an NPC does not climb (docs/COMBAT.md, "The bandit")
	store_walker_state(*e, s);
	return id;
}

void PlanetNetServer::set_entity_input(int64_t p_id, const Vector2 &p_move, float p_yaw, int p_buttons, int p_abilities, const Vector3 &p_aim) {
	Entity *e = entities.getptr(uint64_t(p_id));
	ERR_FAIL_NULL(e);
	e->npc_input.move = Vector2(float(p_move.x), float(p_move.y));
	e->npc_input.yaw = p_yaw;
	e->npc_input.buttons = uint16_t(p_buttons);
	e->npc_input.abilities = uint8_t(p_abilities & 63);
	e->npc_input.aim = p_aim;
}

Dictionary PlanetNetServer::get_entity_walker(int64_t p_id) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	if (e == nullptr || !has_walker(*e)) {
		return Dictionary();
	}
	const WalkerState s = walker_state(*e);
	Dictionary d = combat_of(s);
	d["position"] = s.position;
	d["velocity"] = s.velocity;
	d["rotation"] = s.rotation;
	d["up"] = walker.up_at(s.position);
	d["reference"] = Walker::transport(s.reference, walker.up_at(s.position));
	d["mode"] = int(s.mode);
	d["action"] = int(s.action);
	d["action_ticks"] = int(s.action_ticks);
	d["stamina"] = int(s.stamina);
	d["health"] = int(e->health);
	d["health_max"] = int(e->health_max);
	d["dead"] = s.action == ACTION_DEATH;
	d["no_climb"] = (s.flags & FLAG_NO_CLIMB) != 0;
	d["noclip"] = (s.flags & FLAG_NOCLIP) != 0;
	d["crouch"] = (s.flags & FLAG_CROUCH) != 0;
	return d;
}

uint64_t PlanetNetServer::_create_with_id(uint64_t p_id, int p_type, const Vector3 &p_position) {
	ERR_FAIL_COND_V_MSG(entities.has(p_id), 0, "Entity id already in use.");
	if ((p_id >> ENTITY_SERVER_SHIFT) == server_id) {
		next_entity = MAX(next_entity, (p_id & ((uint64_t(1) << ENTITY_SERVER_SHIFT) - 1)) + 1);
	}
	Entity e;
	e.state.id = p_id;
	e.state.type = uint16_t(p_type);
	e.state.position = p_position;
	for (int i = 0; i < HISTORY_TICKS; ++i) {
		e.history[i] = p_position;
		e.history_tick[i] = tick;
	}
	const uint64_t id = e.state.id;
	entities.insert(id, e);
	_index(id, entities[id]);
	return id;
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

void PlanetNetServer::set_entity_health(int64_t p_id, int p_health_max) {
	Entity *e = entities.getptr(uint64_t(p_id));
	ERR_FAIL_NULL(e);
	e->health_max = MAX(p_health_max, 0);
	e->health = e->health_max;
}

void PlanetNetServer::_resolve_swings() {
	for (KeyValue<uint64_t, Entity> &T : entities) {
		Entity &t = T.value;
		// Reactions run out; health comes back after a rest.
		if (t.react > 0 && --t.react == 0) {
			t.state.pose = 0;
		}
		// An NPC keeps its wounds, as offline play's bandits do: healing every 5 s without a blow, a bandit
		// fought online was back to full whenever the fight paused.
		if (t.health_max > 0 && t.health < t.health_max && tick - t.hurt_tick > uint32_t(HEALTH_REST_TICKS) && !t.npc && !t.mirror) {
			t.health = t.health_max;
			_send_health(t);
		}
	}
	// Every walker's move: the players', judged where each saw its targets; the enemies', which see the
	// server's present, judged there.
	LocalVector<uint64_t> attackers;
	LocalVector<uint32_t> views;
	for (KeyValue<int, Client> &C : clients) {
		if (C.value.entity != 0 && entities.has(C.value.entity)) {
			attackers.push_back(C.value.entity);
			views.push_back(C.value.view_tick != 0 ? C.value.view_tick : tick);
		}
	}
	for (const KeyValue<uint64_t, Entity> &E : entities) {
		if (E.value.npc && !E.value.mirror) {
			attackers.push_back(E.key);
			views.push_back(tick);
		}
	}
	for (uint32_t k = 0; k < attackers.size(); ++k) {
		const uint64_t attacker = attackers[k];
		Entity *a = entities.getptr(attacker);
		if (a == nullptr) {
			continue;
		}
		const Move &m = moves().get(a->walk.action);
		const uint32_t start = tick - a->walk.action_ticks;
		if (m.shape == SHAPE_AREA && a->cast_start != start) {
			// An aimed move begun: everyone who sees the caster is told where it lands, to draw it.
			a->cast_start = start;
			_send_cast(attacker, *a, start);
		}
		if (!walker.hitting(a->walk)) {
			continue;
		}
		if (a->swing_start != start) {
			a->swing_start = start;
			a->swing_hits.clear();
		}
		// Where the others were on this player's screen when it swung: its view, at most max_rewind_ticks back.
		const uint32_t view = views[k];
		const uint32_t most = uint32_t(max_rewind_ticks);
		const double at = double(MAX(view, tick > most ? tick - most : 0u));
		const WalkerState swing = walker_state(*a);
		for (KeyValue<uint64_t, Entity> &T : entities) {
			Entity &t = T.value;
			if (T.key == attacker || t.health_max <= 0) {
				continue;
			}
			// Players strike each other only with player_hits; otherwise blows land only across sides.
			const bool both_players = t.state.type == ENTITY_TYPE_PLAYER && a->state.type == ENTITY_TYPE_PLAYER;
			if (both_players ? !player_hits : (a->team != 0 && a->team == t.team)) {
				continue;
			}
			if (a->swing_hits.has(T.key)) {
				continue;
			}
			// Far off now is far off then too (the rewind is under a second): skip the history.
			const Vector3 centre = m.shape == SHAPE_AREA ? a->walk.aim : a->state.position;
			const double near = MAX(m.reach, m.radius) + 6.0;
			if (t.state.position.distance_squared_to(centre) > near * near) {
				continue;
			}
			const Vector3 then = get_entity_position_at(int64_t(T.key), at);
			if (!walker.swing_reaches(swing, then, TARGET_RADIUS)) {
				continue;
			}
			a->swing_hits.push_back(T.key);
			if (t.mirror) {
				// Another server's: its owner judges the blow, told by the scripts over the mesh.
				emit_signal(SNAME("mirror_struck"), int64_t(attacker), int64_t(T.key), then);
				continue;
			}
			_strike(attacker, *a, T.key, t, then);
		}
	}
}

void PlanetNetServer::send_message(int p_peer, int p_channel, const String &p_text) {
	const Client *c = clients.getptr(p_peer);
	if (c == nullptr || !c->welcomed) {
		return;
	}
	Writer w;
	w.u8(MSG_MESSAGE);
	w.u8(uint8_t(CLAMP(p_channel, 0, 255)));
	w.text(p_text);
	link.send(p_peer, CHANNEL_CONTROL, w.data(), true);
}

void PlanetNetServer::send_data(int p_peer, int p_kind, const PackedByteArray &p_bytes) {
	const Client *c = clients.getptr(p_peer);
	ERR_FAIL_COND_MSG(uint32_t(p_bytes.size()) > MAX_DATA, "PlanetNetServer.send_data: more than MAX_DATA bytes.");
	if (c == nullptr || !c->welcomed) {
		return;
	}
	Writer w;
	w.u8(MSG_DATA);
	w.u8(uint8_t(CLAMP(p_kind, 0, 255)));
	w.u32(uint32_t(p_bytes.size()));
	w.raw(p_bytes.ptr(), uint32_t(p_bytes.size()));
	link.send(p_peer, CHANNEL_WORLD, w.data(), true);
}

void PlanetNetServer::broadcast_message(int p_channel, const String &p_text) {
	for (KeyValue<int, Client> &C : clients) {
		send_message(C.key, p_channel, p_text);
	}
}

int PlanetNetServer::get_entity_peer(int64_t p_id) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	return e != nullptr ? int(e->owner_peer) : 0;
}

void PlanetNetServer::_send_health(const Entity &p_entity) {
	// To the player it is, only: its own health, when it changes (not in every snapshot, where four bytes
	// more starved a budget-limited client's farthest entities).
	if (p_entity.owner_peer == 0 || p_entity.state.type != ENTITY_TYPE_PLAYER) {
		return;
	}
	Writer w;
	w.u8(MSG_HEALTH);
	w.u16(uint16_t(MAX(0, p_entity.health)));
	w.u16(uint16_t(MAX(0, p_entity.health_max)));
	link.send(p_entity.owner_peer, CHANNEL_CONTROL, w.data(), true);
}

void PlanetNetServer::_send_cast(uint64_t p_caster, const Entity &p_entity, uint32_t p_start) {
	Writer w;
	w.u8(MSG_CAST);
	w.u64(p_caster);
	w.u8(p_entity.walk.action);
	w.f64(p_entity.walk.aim.x);
	w.f64(p_entity.walk.aim.y);
	w.f64(p_entity.walk.aim.z);
	w.u32(p_start);
	for (KeyValue<int, Client> &C : clients) {
		const Client &c = C.value;
		const View *seen = c.views.getptr(p_caster);
		if (c.welcomed && (c.entity == p_caster || (seen != nullptr && !seen->removing))) {
			link.send(C.key, CHANNEL_CONTROL, w.data(), true);
		}
	}
	Dictionary cast;
	cast["caster"] = int64_t(p_caster);
	cast["move"] = int(p_entity.walk.action);
	cast["aim"] = p_entity.walk.aim;
	cast["tick"] = int(p_start);
	emit_signal(SNAME("cast"), cast);
}

void PlanetNetServer::_strike(uint64_t p_attacker, Entity &r_attacker, uint64_t p_target, Entity &r_target, const Vector3 &p_at) {
	const uint8_t attack = r_attacker.walk.action;
	const Move &m = moves().get(attack);
	const bool walks = has_walker(r_target);
	uint8_t result = HIT_STRUCK;
	uint8_t reaction = ACTION_NONE;
	int damage = 0;
	if (walks) {
		// The one rule for a blow on a walker (Walker::strike), offline play's too.
		WalkerState ts = walker_state(r_target);
		result = walker.strike(walker_state(r_attacker), ts, reaction, damage, GUARD_BREAK_PUSH);
		store_walker_state(r_target, ts);

	} else {
		// A target without a walker (the dummy) has no poise and no guard: every blow lands its reaction.
		damage = m.damage;
		reaction = Walker::reaction_move(m.reaction);
		if (reaction != ACTION_NONE) {
			r_target.react = moves().get(reaction).ticks;
			r_target.state.pose = uint16_t(reaction << 4);
		}
	}
	if (damage > 0) {
		r_target.health = MAX(0, r_target.health - damage);
		r_target.hurt_tick = tick;
		_send_health(r_target);
		// An enemy's health gone: it is down for good (a player's floors at 0, and refills).
		if (r_target.npc && r_target.health == 0 && r_target.walk.action != ACTION_DEATH) {
			WalkerState ds = walker_state(r_target);
			ds.action = ACTION_DEATH;
			ds.action_ticks = 0;
			store_walker_state(r_target, ds);
			emit_signal(SNAME("entity_died"), int64_t(p_target));
		}
	}
	++hits_resolved;
	Writer w;
	w.u8(MSG_HIT);
	w.u64(p_attacker);
	w.u64(p_target);
	w.u8(result);
	w.u8(attack);
	w.u16(uint16_t(damage));
	w.u16(uint16_t(MAX(0, r_target.health)));
	w.f64(p_at.x);
	w.f64(p_at.y);
	w.f64(p_at.z);
	w.u8(reaction);
	// To each client that sees either end, and to both.
	for (KeyValue<int, Client> &C : clients) {
		const Client &c = C.value;
		if (!c.welcomed) {
			continue;
		}
		const View *seen_target = c.views.getptr(p_target);
		const View *seen_attacker = c.views.getptr(p_attacker);
		if (c.entity == p_attacker || c.entity == p_target || (seen_target != nullptr && !seen_target->removing) ||
				(seen_attacker != nullptr && !seen_attacker->removing)) {
			link.send(C.key, CHANNEL_CONTROL, w.data(), true);
		}
	}
	Dictionary hit;
	hit["attacker"] = int64_t(p_attacker);
	hit["target"] = int64_t(p_target);
	hit["result"] = int(result);
	hit["attack"] = int(attack);
	hit["damage"] = damage;
	hit["health"] = int(r_target.health);
	hit["point"] = p_at;
	hit["reaction"] = int(reaction);
	hit["tick"] = int(tick);
	emit_signal(SNAME("hit"), hit);
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
				Client *c = clients.getptr(ev.peer);
				if (c != nullptr) {
					_release_client(*c);
					clients.erase(ev.peer);
					emit_signal(SNAME("client_disconnected"), ev.peer);
				}
			} break;
			case PlanetNetLink::EVENT_RECEIVED:
				_handle(ev.peer, ev.data);
				break;
		}
	}

	if (autosave_interval > 0.0 && !world_path.is_empty()) {
		autosave_timer += p_delta;
		if (autosave_timer >= autosave_interval) {
			autosave_timer = 0.0;
			save_world();
		}
	}

	const double dt = 1.0 / TICK_HZ;
	if (epoch >= 0.0) {
		// The shared clock: step to the tick the wall clock says, however poll() is called. A long hitch
		// runs at most a quarter second of ticks and skips the rest, so every server stays on the same tick.
		const double now = (OS::get_singleton()->get_unix_time() - epoch) * TICK_HZ;
		const uint32_t target = uint32_t(MAX(Math::floor(now), 0.0));
		if (target > tick + uint32_t(TICK_HZ / 4)) {
			tick = target - uint32_t(TICK_HZ / 4);
		}
		while (tick < target) {
			_step();
		}
		accumulator = CLAMP(now - double(tick), 0.0, 1.0) * dt;
		return;
	}
	// A long hitch runs at most a quarter second of ticks rather than spiralling.
	accumulator = MIN(accumulator + p_delta, 0.25);
	while (accumulator >= dt) {
		accumulator -= dt;
		_step();
	}
}

void PlanetNetServer::set_epoch(double p_unix_seconds) {
	epoch = p_unix_seconds;
	if (epoch >= 0.0) {
		const double now = (OS::get_singleton()->get_unix_time() - epoch) * TICK_HZ;
		tick = uint32_t(MAX(Math::floor(now), 0.0));
		accumulator = 0.0;
		for (KeyValue<uint64_t, Entity> &E : entities) {
			for (int i = 0; i < HISTORY_TICKS; ++i) {
				E.value.history_tick[i] = tick;
			}
		}
	}
}

void PlanetNetServer::slew_epoch(double p_unix_seconds) {
	if (epoch < 0.0) {
		set_epoch(p_unix_seconds);
		return;
	}
	epoch = p_unix_seconds;
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
			const uint8_t flags = r.u8();
			const uint16_t key_length = r.u16();
			CharString key_utf8;
			key_utf8.resize_uninitialized(key_length + 1);
			for (uint16_t i = 0; i < key_length; ++i) {
				key_utf8.ptrw()[i] = char(r.u8());
			}
			key_utf8.ptrw()[key_length] = 0;
			if (!r.ok() || version != PROTOCOL_VERSION) {
				link.disconnect_peer(p_peer);
				return;
			}
			if (client->welcomed) {
				return;
			}
			const String key = String::utf8(key_utf8.get_data(), key_length);
			client->player_key = key;
			// A player handed here from another server, or brought back by the world above: this is it.
			const uint64_t waiting = key.is_empty() ? 0 : _awaiting(key);
			if (waiting != 0) {
				_bind(p_peer, *client, waiting);
				return;
			}
			if ((flags & HELLO_STANDBY) != 0) {
				// Ahead of a switch: welcomed with no entity; its inputs are kept until its player arrives.
				client->standby = true;
				client->welcomed = true;
				_send_welcome(p_peer, 0);
				return;
			}
			PlayerRecord *known = key.is_empty() ? nullptr : players.getptr(key);
			if (known != nullptr && known->online) {
				// The same player twice at once: refuse the newcomer rather than split one identity.
				link.disconnect_peer(p_peer);
				return;
			}
			int64_t id = 0;
			if (known != nullptr) {
				// Back where they left, as the same entity.
				id = int64_t(_create_with_id(known->entity, ENTITY_TYPE_PLAYER, known->state.position));
				if (id == 0) {
					link.disconnect_peer(p_peer);
					return;
				}
				Entity &e = entities[uint64_t(id)];
				e.state.rotation = known->state.rotation;
				e.state.velocity = known->state.velocity;
			} else {
				// New players start at the spawn point, spread out so they do not stack.
				const Vector3 up = spawn_direction;
				const Vector3 east = PlanetNet::Walker::local_north(up).cross(up);
				const Vector3 north = up.cross(east);
				const double spread = 3.0 / planet_radius;
				const Vector3 dir = (up + east * (spread * double(p_peer % 8)) + north * (spread * double(p_peer / 8))).normalized();
				id = create_entity(ENTITY_TYPE_PLAYER, walker.surface_point(dir));
				if (!key.is_empty()) {
					PlayerRecord rec;
					rec.entity = uint64_t(id);
					rec.state = entities[uint64_t(id)].state;
					players.insert(key, rec);
				}
			}
			if (!key.is_empty()) {
				players[key].online = true;
			}
			Entity &player = entities[uint64_t(id)];
			player.owner_peer = p_peer;
			player.persistent = false;
			player.health_max = player_health;
			player.health = player_health;
			player.team = TEAM_PLAYERS;
			client->entity = uint64_t(id);
			client->welcomed = true;
			_send_welcome(p_peer, uint64_t(id));
			_send_health(player);
			emit_signal(SNAME("client_connected"), p_peer, id);
		} break;
		case MSG_WORLD_REQUEST: {
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
			if (r.ok() && client->welcomed) {
				emit_signal(SNAME("world_change_requested"), p_peer, point, data);
			}
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
		case MSG_TELEPORT: {
			Vector3 point;
			point.x = r.f64();
			point.y = r.f64();
			point.z = r.f64();
			Entity *e = client->entity != 0 ? entities.getptr(client->entity) : nullptr;
			if (!r.ok() || e == nullptr) {
				return;
			}
			// Counted even when refused: the client waits for the count before believing snapshots again,
			// and then resumes from wherever this leaves the player.
			++client->teleports;
			if (!allow_teleport || point.length_squared() < 1.0) {
				return;
			}
			// The scripts first: what stands there (content, colliders) can be made solid before the walker is
			// placed on it.
			emit_signal(SNAME("teleport_requested"), int64_t(client->entity), point);
			e = entities.getptr(client->entity);
			if (e == nullptr) {
				return;
			}
			store_walker_state(*e, walker.place(walker_state(*e), point));
		} break;
		case MSG_COMMAND: {
			// A console line, raw: the scripts parse, route and permit it (command_received).
			const String line = r.text();
			if (!r.ok() || !client->welcomed || line.strip_edges().is_empty()) {
				return;
			}
			emit_signal(SNAME("command_received"), p_peer, int64_t(client->entity), line);
		} break;
		case MSG_LOOK: {
			const uint64_t look = r.u64();
			Entity *e = client->entity != 0 ? entities.getptr(client->entity) : nullptr;
			if (!r.ok() || e == nullptr) {
				return;
			}
			e->state.look = look;
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
				in.buttons = r.u16();
				in.abilities = uint8_t(r.u8() & 63);
				in.aim.x = r.f32();
				in.aim.y = r.f32();
				in.aim.z = r.f32();
				const uint32_t view = r.u32();
				if (r.ok() && input_tick > tick && input_tick <= tick + 4 * TICK_HZ) {
					client->inputs[input_tick] = in;
					client->input_views[input_tick] = view;
				}
			}
		} break;
		default:
			break;
	}
}

void PlanetNetServer::_send_welcome(int p_peer, uint64_t p_entity) {
	Writer w;
	w.u8(MSG_WELCOME);
	w.u16(PROTOCOL_VERSION);
	w.u16(server_id);
	w.u64(p_entity);
	w.f64(regions.radius);
	w.u32(regions.resolution);
	w.f64(regions.volume_cell);
	w.f64(walker.speed);
	w.u64(moves().hash);
	w.u64(space);
	w.u8(walker.use_terrain ? 1 : 0);
	w.f64(walker.uniform_down.x);
	w.f64(walker.uniform_down.y);
	w.f64(walker.uniform_down.z);
	link.send(p_peer, CHANNEL_CONTROL, w.data(), true);
}

uint64_t PlanetNetServer::_awaiting(const String &p_key) const {
	for (const KeyValue<uint64_t, Entity> &E : entities) {
		if (!E.value.await_key.is_empty() && E.value.await_key == p_key) {
			return E.key;
		}
	}
	return 0;
}

void PlanetNetServer::_bind(int p_peer, Client &r_client, uint64_t p_id) {
	Entity *e = entities.getptr(p_id);
	ERR_FAIL_NULL(e);
	e->await_key = String();
	e->owner_peer = p_peer;
	e->mirror = false;
	e->persistent = false;
	e->team = TEAM_PLAYERS;
	if (e->health_max <= 0) {
		e->health_max = player_health;
		e->health = player_health;
	}
	r_client.entity = p_id;
	r_client.welcomed = true;
	r_client.standby = false;
	if (!r_client.player_key.is_empty()) {
		PlayerRecord rec;
		rec.entity = p_id;
		rec.state = e->state;
		rec.online = true;
		players[r_client.player_key] = rec;
	}
	// From the tick it left its old server to now, on the inputs the client sent here meanwhile: the
	// same steps its old server and its prediction took, so the switch costs no correction.
	const double dt = 1.0 / TICK_HZ;
	if (e->walk_tick != 0 && e->walk_tick < tick) {
		WalkerState st = walker_state(*e);
		for (uint32_t k = e->walk_tick + 1; k <= tick; ++k) {
			const WalkerInput *in = r_client.inputs.getptr(k);
			if (in != nullptr) {
				r_client.last_input = *in;
				r_client.has_input = true;
			}
			const uint32_t *view = r_client.input_views.getptr(k);
			if (view != nullptr) {
				r_client.view_tick = *view;
			}
			r_client.inputs.erase(k);
			r_client.input_views.erase(k);
			st = walker.step(st, r_client.last_input, dt);
		}
		store_walker_state(*e, st);
	}
	e->walk_tick = 0;
	r_client.last_applied_tick = tick;
	// No past here to rewind into: where it is now.
	for (int k = 0; k < HISTORY_TICKS; ++k) {
		e->history[k] = e->state.position;
		e->history_tick[k] = tick;
	}
	_send_welcome(p_peer, p_id);
	_send_health(*e);
	emit_signal(SNAME("client_connected"), p_peer, int64_t(p_id));
}

void PlanetNetServer::detach_client(int p_peer) {
	Client *c = clients.getptr(p_peer);
	if (c == nullptr || c->entity == 0) {
		return;
	}
	Entity *e = entities.getptr(c->entity);
	if (e != nullptr) {
		e->owner_peer = 0;
	}
	if (!c->player_key.is_empty()) {
		PlayerRecord *rec = players.getptr(c->player_key);
		if (rec != nullptr) {
			if (e != nullptr) {
				rec->state = e->state;
			}
			rec->online = false;
		}
	}
	c->entity = 0;
	c->standby = true; // its inputs are no longer for anything here, but kept, should it come back
}

void PlanetNetServer::send_transfer(int p_peer, const String &p_address, int p_port) {
	if (!clients.has(p_peer)) {
		return;
	}
	Writer w;
	w.u8(MSG_TRANSFER);
	w.text(p_address);
	w.u16(uint16_t(CLAMP(p_port, 0, 65535)));
	link.send(p_peer, CHANNEL_CONTROL, w.data(), true);
}

void PlanetNetServer::relay_hit(const Dictionary &p_hit) {
	const uint64_t attacker = uint64_t(int64_t(p_hit.get("attacker", 0)));
	const uint64_t target = uint64_t(int64_t(p_hit.get("target", 0)));
	const Vector3 at = p_hit.get("point", Vector3());
	Writer w;
	w.u8(MSG_HIT);
	w.u64(attacker);
	w.u64(target);
	w.u8(uint8_t(int(p_hit.get("result", 0))));
	w.u8(uint8_t(int(p_hit.get("attack", 0))));
	w.u16(uint16_t(CLAMP(int(p_hit.get("damage", 0)), 0, 65535)));
	w.u16(uint16_t(CLAMP(int(p_hit.get("health", 0)), 0, 65535)));
	w.f64(at.x);
	w.f64(at.y);
	w.f64(at.z);
	w.u8(uint8_t(int(p_hit.get("reaction", 0))));
	for (KeyValue<int, Client> &C : clients) {
		const Client &c = C.value;
		if (!c.welcomed) {
			continue;
		}
		const View *seen_target = c.views.getptr(target);
		const View *seen_attacker = c.views.getptr(attacker);
		if (c.entity == attacker || c.entity == target || (seen_target != nullptr && !seen_target->removing) ||
				(seen_attacker != nullptr && !seen_attacker->removing)) {
			link.send(C.key, CHANNEL_CONTROL, w.data(), true);
		}
	}
}

void PlanetNetServer::strike(int64_t p_attacker, int64_t p_target, const Vector3 &p_at) {
	Entity *a = entities.getptr(uint64_t(p_attacker));
	Entity *t = entities.getptr(uint64_t(p_target));
	if (a == nullptr || t == nullptr || t->mirror || t->health_max <= 0) {
		return;
	}
	_strike(uint64_t(p_attacker), *a, uint64_t(p_target), *t, p_at);
}

namespace {
constexpr uint32_t MAGIC_EXPORT = 0x58454e50; // "PNEX"
enum ExportFlags : uint8_t {
	EXPORT_WALKER = 1,
	EXPORT_NPC = 2,
	EXPORT_PERSISTENT = 4,
	EXPORT_PLAYER = 8,
};
} // namespace

void PlanetNetServer::_write_entity(Writer &w, uint64_t p_id, const Entity &p_entity) const {
	const EntityState &s = p_entity.state;
	w.u64(p_id);
	w.u16(s.type);
	w.u64(s.frame);
	w.f64(double(s.position.x));
	w.f64(double(s.position.y));
	w.f64(double(s.position.z));
	w.f64(double(s.rotation.x));
	w.f64(double(s.rotation.y));
	w.f64(double(s.rotation.z));
	w.f64(double(s.rotation.w));
	w.f64(double(s.velocity.x));
	w.f64(double(s.velocity.y));
	w.f64(double(s.velocity.z));
	w.u16(s.pose);
	w.u64(s.look);
	const bool player = s.type == ENTITY_TYPE_PLAYER;
	const uint8_t flags = (has_walker(p_entity) ? EXPORT_WALKER : 0) | (p_entity.npc ? EXPORT_NPC : 0) |
			(p_entity.persistent ? EXPORT_PERSISTENT : 0) | (player ? EXPORT_PLAYER : 0);
	w.u8(flags);
	w.u8(p_entity.team);
	w.u32(uint32_t(p_entity.health));
	w.u32(uint32_t(p_entity.health_max));
	w.u8(p_entity.awareness);
	if (flags & EXPORT_WALKER) {
		write_walk(w, p_entity.walk);
	}
	if (flags & EXPORT_NPC) {
		const WalkerInput &in = p_entity.npc_input;
		w.f32(in.move.x);
		w.f32(in.move.y);
		w.f32(in.yaw);
		w.u16(in.buttons);
		w.u8(in.abilities);
		w.f32(float(in.aim.x));
		w.f32(float(in.aim.y));
		w.f32(float(in.aim.z));
	}
	if (flags & EXPORT_PLAYER) {
		String key = p_entity.await_key;
		if (key.is_empty()) {
			const Client *c = clients.getptr(p_entity.owner_peer);
			if (c != nullptr) {
				key = c->player_key;
			}
		}
		w.text(key);
	}
}

PackedByteArray PlanetNetServer::export_entities(const PackedInt64Array &p_ids) const {
	Writer w;
	w.u32(MAGIC_EXPORT);
	w.u32(tick);
	const uint32_t count_at = w.size();
	w.u32(0);
	uint32_t n = 0;
	for (int i = 0; i < p_ids.size(); ++i) {
		const Entity *e = entities.getptr(uint64_t(p_ids[i]));
		if (e != nullptr) {
			_write_entity(w, uint64_t(p_ids[i]), *e);
			++n;
		}
	}
	// The count, in place.
	PackedByteArray out;
	out.resize(w.size());
	memcpy(out.ptrw(), w.data().ptr(), w.size());
	memcpy(out.ptrw() + count_at, &n, 4);
	return out;
}

PackedInt64Array PlanetNetServer::import_entities(const PackedByteArray &p_bytes, bool p_mirror) {
	PackedInt64Array out;
	Reader r(p_bytes.ptr(), uint32_t(p_bytes.size()));
	if (r.u32() != MAGIC_EXPORT) {
		ERR_FAIL_V_MSG(out, "Not an entity export.");
	}
	const uint32_t from_tick = r.u32();
	const uint32_t n = r.u32();
	const double dt = 1.0 / TICK_HZ;
	for (uint32_t i = 0; i < n && r.ok(); ++i) {
		EntityState s;
		s.id = r.u64();
		s.type = r.u16();
		s.frame = r.u64();
		s.position.x = r.f64();
		s.position.y = r.f64();
		s.position.z = r.f64();
		s.rotation.x = r.f64();
		s.rotation.y = r.f64();
		s.rotation.z = r.f64();
		s.rotation.w = r.f64();
		s.velocity.x = r.f64();
		s.velocity.y = r.f64();
		s.velocity.z = r.f64();
		s.pose = r.u16();
		s.look = r.u64();
		const uint8_t flags = r.u8();
		const uint8_t team = r.u8();
		const int32_t health = int32_t(r.u32());
		const int32_t health_max = int32_t(r.u32());
		const uint8_t awareness = r.u8();
		WalkerState walk;
		if (flags & EXPORT_WALKER) {
			read_walk(r, walk);
		}
		WalkerInput npc_in;
		if (flags & EXPORT_NPC) {
			npc_in.move.x = r.f32();
			npc_in.move.y = r.f32();
			npc_in.yaw = r.f32();
			npc_in.buttons = r.u16();
			npc_in.abilities = r.u8();
			npc_in.aim.x = r.f32();
			npc_in.aim.y = r.f32();
			npc_in.aim.z = r.f32();
		}
		String key;
		if (flags & EXPORT_PLAYER) {
			key = r.text();
		}
		if (!r.ok()) {
			break;
		}
		Entity *e = entities.getptr(s.id);
		if (e != nullptr && !e->mirror) {
			continue; // ours already: another server's word does not move it
		}
		if (e == nullptr) {
			if (_create_with_id(s.id, s.type, s.position) == 0) {
				continue;
			}
			e = entities.getptr(s.id);
			e->mirror = true; // until it is made ours below
		}
		e->state = s;
		e->walk = walk;
		e->walk.position = s.position;
		e->walk.velocity = s.velocity;
		e->walk.rotation = s.rotation;
		e->team = team;
		e->health = health;
		e->health_max = health_max;
		e->awareness = awareness;
		e->npc_input = npc_in;
		if (p_mirror) {
			e->mirror = true;
			e->npc = false;
			e->persistent = false;
			e->owner_peer = 0;
		} else {
			e->mirror = false;
			e->npc = (flags & EXPORT_NPC) != 0;
			e->persistent = (flags & EXPORT_PERSISTENT) != 0;
			e->walk_tick = from_tick;
			if (e->npc && from_tick < tick) {
				// Stepped on to now, on the input its brain last gave it.
				WalkerState st = walker_state(*e);
				for (uint32_t k = from_tick; k < tick; ++k) {
					st = walker.step(st, e->npc_input, dt);
				}
				store_walker_state(*e, st);
				e->walk_tick = 0;
			}
			if (flags & EXPORT_PLAYER) {
				e->await_key = key;
				e->owner_peer = 0;
				// Its client may be here already, on standby.
				for (KeyValue<int, Client> &C : clients) {
					if (C.value.entity == 0 && !key.is_empty() && C.value.player_key == key) {
						_bind(C.key, C.value, s.id);
						break;
					}
				}
			}
		}
		out.push_back(int64_t(s.id));
	}
	return out;
}

PackedInt64Array PlanetNetServer::get_entities_in_region(int p_key) const {
	PackedInt64Array out;
	const LocalVector<uint64_t> *ids = region_index.getptr(uint32_t(p_key));
	if (ids != nullptr) {
		for (const uint64_t id : *ids) {
			out.push_back(int64_t(id));
		}
	}
	return out;
}

void PlanetNetServer::add_world_change(int64_t p_id, const Vector3 &p_point, const PackedByteArray &p_data) {
	const uint32_t key = regions.key_of(p_point);
	LocalVector<WorldChange> &changes = journal[key];
	for (const WorldChange &c : changes) {
		if (c.id == uint64_t(p_id)) {
			return;
		}
	}
	WorldChange change;
	change.id = uint64_t(p_id);
	change.point = p_point;
	change.data = p_data;
	changes.push_back(change);
	for (KeyValue<int, Client> &C : clients) {
		if (C.value.welcomed && C.value.journal_regions.has(key)) {
			_send_change(C.key, change);
		}
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
	// The solid bodies as they stand before anyone moves this tick; each walker steps against the others.
	LocalVector<uint64_t> solid_ids;
	LocalVector<Vector3> solid_feet;
	LocalVector<double> solid_radii;
	if (!solid_types.is_empty()) {
		for (const KeyValue<uint64_t, Entity> &E : entities) {
			const double *r = solid_types.getptr(int(E.value.state.type));
			if (r != nullptr && !(E.value.walk.flags & FLAG_NOCLIP)) {
				solid_ids.push_back(E.key);
				// Where the clients have it: through the wire's rounding, so a prediction against a body
				// that stands still is exact.
				solid_feet.push_back(Wire::encode(E.value.state, regions).decode(E.key, regions).position);
				solid_radii.push_back(*r);
			}
		}
	}
	for (KeyValue<int, Client> &C : clients) {
		Client &client = C.value;
		Entity *e = client.entity != 0 ? entities.getptr(client.entity) : nullptr;
		if (e == nullptr) {
			// On standby: its inputs kept a couple of seconds back, for when its player arrives.
			if (tick > uint32_t(2 * TICK_HZ)) {
				client.inputs.erase(tick - uint32_t(2 * TICK_HZ));
				client.input_views.erase(tick - uint32_t(2 * TICK_HZ));
			}
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
		const uint32_t *view = client.input_views.getptr(tick);
		if (view != nullptr) {
			client.view_tick = *view;
		} else if (client.view_tick != 0) {
			++client.view_tick; // a repeated input: the screen moved on a tick too
		}
		client.input_views.erase(tick);
		walker.body_feet.clear();
		walker.body_radii.clear();
		for (uint32_t k = 0; k < solid_ids.size(); ++k) {
			if (solid_ids[k] != client.entity && solid_feet[k].distance_squared_to(e->state.position) < 100.0) {
				walker.body_feet.push_back(solid_feet[k]);
				walker.body_radii.push_back(solid_radii[k]);
			}
		}
		const WalkerState s = walker.step(walker_state(*e), client.last_input, dt);
		store_walker_state(*e, s);
		// Reported even before any input arrives, so the client starts predicting from the exact state.
		client.last_applied_tick = tick;
		const int slot = int(tick % HISTORY_TICKS);
		client.applied_tick[slot] = tick;
		client.applied_input[slot] = client.last_input;
		client.applied_arrived[slot] = arrived != nullptr;
		client.applied_position[slot] = s.position;
	}
	// Enemies: walkers stepped from what their brains set (set_entity_input).
	for (KeyValue<uint64_t, Entity> &E : entities) {
		Entity &e = E.value;
		if (!e.npc || e.mirror) {
			continue;
		}
		walker.body_feet.clear();
		walker.body_radii.clear();
		for (uint32_t k = 0; k < solid_ids.size(); ++k) {
			if (solid_ids[k] != E.key && solid_feet[k].distance_squared_to(e.state.position) < 100.0) {
				walker.body_feet.push_back(solid_feet[k]);
				walker.body_radii.push_back(solid_radii[k]);
			}
		}
		store_walker_state(e, walker.step(walker_state(e), e.npc_input, dt));
	}
	walker.body_feet.clear();
	walker.body_radii.clear();
	_resolve_swings();
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
	if (regions.is_volume()) {
		// A zone in volume mode: the ball round the centre, sampled the same way.
		const double step = regions.cell_size() * 0.5;
		const double reach = interest_radius + regions.cell_size();
		const int n = int(Math::ceil(reach / step));
		const double limit = (reach / step) * (reach / step) + 1.0;
		for (int a = -n; a <= n; ++a) {
			for (int b = -n; b <= n; ++b) {
				for (int c = -n; c <= n; ++c) {
					if (double(a * a + b * b + c * c) <= limit) {
						r_regions.insert(regions.key_of(p_centre + Vector3(double(a), double(b), double(c)) * step));
					}
				}
			}
		}
		return;
	}
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

	// Regions newly in interest bring their whole world-change journal with them, reliably and in order.
	for (const uint32_t key : interest) {
		if (r_client.journal_regions.has(key)) {
			continue;
		}
		r_client.journal_regions.insert(key);
		const LocalVector<WorldChange> *changes = journal.getptr(key);
		if (changes != nullptr) {
			for (const WorldChange &change : *changes) {
				_send_change(p_peer, change);
			}
		}
	}

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
		write_walk(w, self->walk);
		w.u8(r_client.teleports);
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
	d["hits_resolved"] = hits_resolved;
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
	// The tick its newest input says it was drawing others at, and how far behind the server that is now:
	// what a swing of its would be rewound by (at most max_rewind_ticks).
	d["view_tick"] = int64_t(c->view_tick);
	d["view_behind"] = c->view_tick != 0 ? int64_t(tick) - int64_t(c->view_tick) : int64_t(0);
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
	d["buttons"] = c->applied_input[slot].buttons;
	d["arrived"] = c->applied_arrived[slot];
	d["position"] = c->applied_position[slot];
	return d;
}

// ---------------------------------------------------------------- world changes

void PlanetNetServer::_send_change(int p_peer, const WorldChange &p_change) {
	Writer w;
	w.u8(MSG_WORLD_CHANGE);
	w.u64(p_change.id);
	w.f64(double(p_change.point.x));
	w.f64(double(p_change.point.y));
	w.f64(double(p_change.point.z));
	w.u16(uint16_t(p_change.data.size()));
	for (int i = 0; i < p_change.data.size(); ++i) {
		w.u8(p_change.data[i]);
	}
	link.send(p_peer, CHANNEL_WORLD, w.data(), true);
}

int64_t PlanetNetServer::commit_world_change(const Vector3 &p_point, const PackedByteArray &p_data) {
	ERR_FAIL_COND_V_MSG(p_data.size() > 0xffff, 0, "A world change is at most 65535 bytes.");
	WorldChange change;
	// Carrying this server's id, as entity ids do, so changes from several servers never collide.
	change.id = (uint64_t(server_id) << ENTITY_SERVER_SHIFT) | next_change++;
	change.point = p_point;
	change.data = p_data;
	const uint32_t key = regions.key_of(p_point);
	journal[key].push_back(change);
	for (KeyValue<int, Client> &C : clients) {
		if (C.value.welcomed && C.value.journal_regions.has(key)) {
			_send_change(C.key, change);
		}
	}
	return int64_t(change.id);
}

Array PlanetNetServer::get_world_changes() const {
	LocalVector<const WorldChange *> all;
	for (const KeyValue<uint32_t, LocalVector<WorldChange>> &R : journal) {
		for (const WorldChange &c : R.value) {
			all.push_back(&c);
		}
	}
	// By id, which is commit order.
	for (uint32_t i = 1; i < all.size(); ++i) {
		const WorldChange *v = all[i];
		uint32_t j = i;
		while (j > 0 && all[j - 1]->id > v->id) {
			all[j] = all[j - 1];
			--j;
		}
		all[j] = v;
	}
	Array out;
	for (const WorldChange *c : all) {
		Dictionary d;
		d["id"] = int64_t(c->id);
		d["point"] = c->point;
		d["data"] = c->data;
		out.push_back(d);
	}
	return out;
}

int PlanetNetServer::get_world_change_count() const {
	int n = 0;
	for (const KeyValue<uint32_t, LocalVector<WorldChange>> &R : journal) {
		n += int(R.value.size());
	}
	return n;
}

// ---------------------------------------------------------------- persistence

namespace {

constexpr uint32_t MAGIC_WORLD = 0x44574e50; // "PNWD"
constexpr uint32_t MAGIC_REGION = 0x47524e50; // "PNRG"
constexpr uint32_t MAGIC_PLAYERS = 0x4c504e50; // "PNPL"
// 2: region files end with the region's world-change journal.
constexpr uint16_t FILE_FORMAT = 2;

void write_state(PlanetNet::Writer &w, const PlanetNet::EntityState &s) {
	w.f64(double(s.position.x));
	w.f64(double(s.position.y));
	w.f64(double(s.position.z));
	w.f32(float(s.rotation.x));
	w.f32(float(s.rotation.y));
	w.f32(float(s.rotation.z));
	w.f32(float(s.rotation.w));
	w.f32(float(s.velocity.x));
	w.f32(float(s.velocity.y));
	w.f32(float(s.velocity.z));
}

void read_state(PlanetNet::Reader &r, PlanetNet::EntityState &s) {
	s.position.x = r.f64();
	s.position.y = r.f64();
	s.position.z = r.f64();
	s.rotation.x = r.f32();
	s.rotation.y = r.f32();
	s.rotation.z = r.f32();
	s.rotation.w = r.f32();
	s.velocity.x = r.f32();
	s.velocity.y = r.f32();
	s.velocity.z = r.f32();
}

String region_file(uint32_t p_key) {
	return "region_" + String::num_uint64(p_key, 16).lpad(8, "0") + ".bin";
}

} // namespace

Error PlanetNetServer::_write_file(const String &p_name, Writer &r_writer) const {
	r_writer.u64(checksum(r_writer.data().ptr(), r_writer.size()));
	const String path = world_path.path_join(p_name);
	const String temp = path + ".tmp";
	{
		Ref<FileAccess> f = FileAccess::open(temp, FileAccess::WRITE);
		if (f.is_null()) {
			return ERR_FILE_CANT_WRITE;
		}
		f->store_buffer(r_writer.data().ptr(), r_writer.size());
		if (f->get_error() != OK) {
			return ERR_FILE_CANT_WRITE;
		}
	}
	if (FileAccess::exists(path)) {
		DirAccess::remove_absolute(path);
	}
	return DirAccess::rename_absolute(temp, path);
}

Error PlanetNetServer::_read_file(const String &p_name, uint32_t p_magic, LocalVector<uint8_t> &r_bytes) const {
	const String path = world_path.path_join(p_name);
	if (!FileAccess::exists(path)) {
		return ERR_FILE_NOT_FOUND;
	}
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(path);
	if (bytes.size() < 4 + 2 + 8) {
		return ERR_FILE_CORRUPT;
	}
	const uint32_t body = uint32_t(bytes.size()) - 8;
	uint64_t stored = 0;
	memcpy(&stored, bytes.ptr() + body, 8);
	uint32_t magic = 0;
	memcpy(&magic, bytes.ptr(), 4);
	uint16_t format = 0;
	memcpy(&format, bytes.ptr() + 4, 2);
	if (magic != p_magic || format != FILE_FORMAT || stored != checksum(bytes.ptr(), body)) {
		return ERR_FILE_CORRUPT;
	}
	r_bytes.resize(body - 6);
	memcpy(r_bytes.ptr(), bytes.ptr() + 6, body - 6);
	return OK;
}

Error PlanetNetServer::save_world() {
	ERR_FAIL_COND_V_MSG(world_path.is_empty(), ERR_UNCONFIGURED, "Set world_path before saving.");
	Error err = DirAccess::make_dir_recursive_absolute(world_path);
	if (err != OK && err != ERR_ALREADY_EXISTS) {
		return err;
	}
	err = OK;

	HashMap<uint32_t, LocalVector<uint64_t>> by_region;
	for (const KeyValue<uint64_t, Entity> &E : entities) {
		if (E.value.persistent) {
			by_region[regions.key_of(E.value.state.position)].push_back(E.key);
		}
	}
	// A region with changes but no entities still needs its file.
	for (const KeyValue<uint32_t, LocalVector<WorldChange>> &J : journal) {
		if (!by_region.has(J.key)) {
			by_region.insert(J.key, LocalVector<uint64_t>());
		}
	}
	for (const KeyValue<uint32_t, LocalVector<uint64_t>> &R : by_region) {
		Writer w;
		w.u32(MAGIC_REGION);
		w.u16(FILE_FORMAT);
		w.u32(R.key);
		w.u32(R.value.size());
		for (const uint64_t id : R.value) {
			const Entity &e = entities[id];
			w.u64(id);
			w.u16(e.state.type);
			w.u64(e.state.frame);
			write_state(w, e.state);
		}
		const LocalVector<WorldChange> *changes = journal.getptr(R.key);
		w.u32(changes != nullptr ? changes->size() : 0);
		if (changes != nullptr) {
			for (const WorldChange &c : *changes) {
				w.u64(c.id);
				w.f64(double(c.point.x));
				w.f64(double(c.point.y));
				w.f64(double(c.point.z));
				w.u16(uint16_t(c.data.size()));
				for (int i = 0; i < c.data.size(); ++i) {
					w.u8(c.data[i]);
				}
			}
		}
		const Error e = _write_file(region_file(R.key), w);
		err = err == OK ? e : err;
	}
	// Regions that emptied since the last save lose their files, or their entities would come back.
	for (const uint32_t key : saved_regions) {
		if (!by_region.has(key)) {
			DirAccess::remove_absolute(world_path.path_join(region_file(key)));
		}
	}
	saved_regions.clear();
	for (const KeyValue<uint32_t, LocalVector<uint64_t>> &R : by_region) {
		saved_regions.insert(R.key);
	}

	Writer p;
	p.u32(MAGIC_PLAYERS);
	p.u16(FILE_FORMAT);
	p.u32(players.size());
	for (const KeyValue<String, PlayerRecord> &P : players) {
		const CharString key = P.key.utf8();
		p.u16(uint16_t(key.length()));
		for (int i = 0; i < key.length(); ++i) {
			p.u8(uint8_t(key[i]));
		}
		p.u64(P.value.entity);
		// Connected players are saved where they are now, not where they logged in.
		const Entity *live = P.value.online ? entities.getptr(P.value.entity) : nullptr;
		write_state(p, live != nullptr ? live->state : P.value.state);
	}
	const Error pe = _write_file("players.bin", p);
	err = err == OK ? pe : err;

	Writer h;
	h.u32(MAGIC_WORLD);
	h.u16(FILE_FORMAT);
	h.u16(server_id);
	h.u64(next_entity);
	const Error he = _write_file("world.bin", h);
	return err == OK ? he : err;
}

Error PlanetNetServer::load_world() {
	ERR_FAIL_COND_V_MSG(world_path.is_empty(), ERR_UNCONFIGURED, "Set world_path before loading.");
	ERR_FAIL_COND_V_MSG(!clients.is_empty(), ERR_BUSY, "Load the world before clients connect.");
	entities.clear();
	region_index.clear();
	players.clear();
	saved_regions.clear();
	journal.clear();
	regions.configure(planet_radius, region_size);
	int change_count = 0;

	int region_count = 0;
	int entity_count = 0;
	PackedStringArray skipped;
	Error result = OK;

	LocalVector<uint8_t> bytes;
	if (_read_file("world.bin", MAGIC_WORLD, bytes) == OK) {
		Reader r(bytes.ptr(), bytes.size());
		r.u16(); // the id of the server that saved it; ids keep their own
		const uint64_t saved_next = r.u64();
		if (r.ok()) {
			next_entity = MAX(next_entity, saved_next);
		}
	}

	if (DirAccess::dir_exists_absolute(world_path)) {
		for (const String &name : DirAccess::get_files_at(world_path)) {
			if (!name.begins_with("region_") || !name.ends_with(".bin")) {
				continue;
			}
			const uint32_t key = uint32_t(name.trim_prefix("region_").trim_suffix(".bin").hex_to_int());
			// Known on disk either way, so the next save rewrites or removes it.
			saved_regions.insert(key);
			Error e = _read_file(name, MAGIC_REGION, bytes);
			if (e == OK) {
				Reader r(bytes.ptr(), bytes.size());
				r.u32(); // region key, also in the name
				const uint32_t n = r.u32();
				LocalVector<EntityState> loaded;
				for (uint32_t i = 0; i < n && r.ok(); ++i) {
					EntityState s;
					s.id = r.u64();
					s.type = r.u16();
					s.frame = r.u64();
					read_state(r, s);
					loaded.push_back(s);
				}
				const uint32_t nc = r.u32();
				LocalVector<WorldChange> changes;
				for (uint32_t i = 0; i < nc && r.ok(); ++i) {
					WorldChange c;
					c.id = r.u64();
					c.point.x = r.f64();
					c.point.y = r.f64();
					c.point.z = r.f64();
					const uint16_t len = r.u16();
					c.data.resize(len);
					for (uint16_t k = 0; k < len; ++k) {
						c.data.write[k] = r.u8();
					}
					changes.push_back(c);
				}
				if (!r.ok()) {
					e = ERR_FILE_CORRUPT;
				} else {
					for (const EntityState &s : loaded) {
						if (_create_with_id(s.id, s.type, s.position) != 0) {
							Entity &ent = entities[s.id];
							ent.state = s;
							++entity_count;
						}
					}
					for (const WorldChange &c : changes) {
						journal[regions.key_of(c.point)].push_back(c);
						if ((c.id >> ENTITY_SERVER_SHIFT) == server_id || (c.id >> ENTITY_SERVER_SHIFT) == 0) {
							next_change = MAX(next_change, (c.id & ((uint64_t(1) << ENTITY_SERVER_SHIFT) - 1)) + 1);
						}
						++change_count;
					}
					++region_count;
				}
			}
			if (e != OK) {
				skipped.push_back(name);
				result = ERR_FILE_CORRUPT;
			}
		}
	}

	int player_count = 0;
	const Error pe = _read_file("players.bin", MAGIC_PLAYERS, bytes);
	if (pe == OK) {
		Reader r(bytes.ptr(), bytes.size());
		const uint32_t n = r.u32();
		for (uint32_t i = 0; i < n && r.ok(); ++i) {
			const uint16_t len = r.u16();
			CharString key_utf8;
			key_utf8.resize_uninitialized(len + 1);
			for (uint16_t k = 0; k < len; ++k) {
				key_utf8.ptrw()[k] = char(r.u8());
			}
			key_utf8.ptrw()[len] = 0;
			PlayerRecord rec;
			rec.entity = r.u64();
			read_state(r, rec.state);
			rec.state.id = rec.entity;
			rec.state.type = ENTITY_TYPE_PLAYER;
			if (r.ok()) {
				players.insert(String::utf8(key_utf8.get_data(), len), rec);
				++player_count;
				if ((rec.entity >> ENTITY_SERVER_SHIFT) == server_id) {
					next_entity = MAX(next_entity, (rec.entity & ((uint64_t(1) << ENTITY_SERVER_SHIFT) - 1)) + 1);
				}
			}
		}
		if (!r.ok()) {
			skipped.push_back("players.bin");
			result = ERR_FILE_CORRUPT;
		}
	} else if (pe != ERR_FILE_NOT_FOUND) {
		skipped.push_back("players.bin");
		result = ERR_FILE_CORRUPT;
	}

	last_load_report = Dictionary();
	last_load_report["regions"] = region_count;
	last_load_report["entities"] = entity_count;
	last_load_report["players"] = player_count;
	last_load_report["world_changes"] = change_count;
	last_load_report["skipped"] = skipped;
	return result;
}

Error PlanetNetServer::save_regions(const PackedInt32Array &p_keys, bool p_unload) {
	ERR_FAIL_COND_V_MSG(world_path.is_empty(), ERR_UNCONFIGURED, "Set world_path before saving.");
	Error err = DirAccess::make_dir_recursive_absolute(world_path);
	if (err != OK && err != ERR_ALREADY_EXISTS) {
		return err;
	}
	err = OK;
	for (int i = 0; i < p_keys.size(); ++i) {
		const uint32_t key = uint32_t(p_keys[i]);
		LocalVector<uint64_t> ids;
		const LocalVector<uint64_t> *in_region = region_index.getptr(key);
		if (in_region != nullptr) {
			for (const uint64_t id : *in_region) {
				const Entity &e = entities[id];
				if (e.persistent && !e.mirror && e.state.type != ENTITY_TYPE_PLAYER) {
					ids.push_back(id);
				}
			}
		}
		const LocalVector<WorldChange> *changes = journal.getptr(key);
		if (ids.is_empty() && (changes == nullptr || changes->is_empty())) {
			if (saved_regions.has(key) || FileAccess::exists(world_path.path_join(region_file(key)))) {
				DirAccess::remove_absolute(world_path.path_join(region_file(key)));
			}
			saved_regions.erase(key);
		} else {
			Writer w;
			w.u32(MAGIC_REGION);
			w.u16(FILE_FORMAT);
			w.u32(key);
			w.u32(ids.size());
			for (const uint64_t id : ids) {
				const Entity &e = entities[id];
				w.u64(id);
				w.u16(e.state.type);
				w.u64(e.state.frame);
				write_state(w, e.state);
			}
			w.u32(changes != nullptr ? changes->size() : 0);
			if (changes != nullptr) {
				for (const WorldChange &c : *changes) {
					w.u64(c.id);
					w.f64(double(c.point.x));
					w.f64(double(c.point.y));
					w.f64(double(c.point.z));
					w.u16(uint16_t(c.data.size()));
					for (int k = 0; k < c.data.size(); ++k) {
						w.u8(c.data[k]);
					}
				}
			}
			const Error e = _write_file(region_file(key), w);
			err = err == OK ? e : err;
			saved_regions.insert(key);
		}
		if (p_unload) {
			for (const uint64_t id : ids) {
				destroy_entity(int64_t(id));
			}
			journal.erase(key);
			saved_regions.erase(key);
			for (KeyValue<int, Client> &C : clients) {
				C.value.journal_regions.erase(key);
			}
		}
	}
	return err;
}

PackedInt64Array PlanetNetServer::load_regions(const PackedInt32Array &p_keys) {
	PackedInt64Array out;
	LocalVector<uint8_t> bytes;
	for (int i = 0; i < p_keys.size(); ++i) {
		const uint32_t key = uint32_t(p_keys[i]);
		if (_read_file(region_file(key), MAGIC_REGION, bytes) != OK) {
			continue;
		}
		saved_regions.insert(key);
		Reader r(bytes.ptr(), bytes.size());
		r.u32();
		const uint32_t n = r.u32();
		for (uint32_t k = 0; k < n && r.ok(); ++k) {
			EntityState st;
			st.id = r.u64();
			st.type = r.u16();
			st.frame = r.u64();
			read_state(r, st);
			if (!r.ok()) {
				break;
			}
			Entity *e = entities.getptr(st.id);
			if (e != nullptr && !e->mirror) {
				continue;
			}
			if (e == nullptr) {
				if (_create_with_id(st.id, st.type, st.position) == 0) {
					continue;
				}
				e = entities.getptr(st.id);
			}
			e->state = st;
			e->mirror = false;
			e->persistent = true;
			out.push_back(int64_t(st.id));
		}
		const uint32_t nc = r.u32();
		for (uint32_t k = 0; k < nc && r.ok(); ++k) {
			WorldChange c;
			c.id = r.u64();
			c.point.x = r.f64();
			c.point.y = r.f64();
			c.point.z = r.f64();
			const uint16_t len = r.u16();
			c.data.resize(len);
			for (uint16_t b = 0; b < len; ++b) {
				c.data.write[b] = r.u8();
			}
			if (r.ok()) {
				add_world_change(int64_t(c.id), c.point, c.data);
			}
		}
	}
	return out;
}

void PlanetNetServer::set_entity_persistent(int64_t p_id, bool p_persistent) {
	Entity *e = entities.getptr(uint64_t(p_id));
	ERR_FAIL_NULL(e);
	e->persistent = p_persistent;
}

bool PlanetNetServer::is_entity_persistent(int64_t p_id) const {
	const Entity *e = entities.getptr(uint64_t(p_id));
	return e != nullptr && e->persistent;
}

PackedStringArray PlanetNetServer::get_player_keys() const {
	PackedStringArray keys;
	for (const KeyValue<String, PlayerRecord> &P : players) {
		keys.push_back(P.key);
	}
	return keys;
}

int64_t PlanetNetServer::get_player_entity(const String &p_key) const {
	const PlayerRecord *rec = players.getptr(p_key);
	return rec != nullptr ? int64_t(rec->entity) : 0;
}

Vector3 PlanetNetServer::get_player_position(const String &p_key) const {
	const PlayerRecord *rec = players.getptr(p_key);
	if (rec == nullptr) {
		return Vector3();
	}
	const Entity *live = rec->online ? entities.getptr(rec->entity) : nullptr;
	return live != nullptr ? live->state.position : rec->state.position;
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
	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &PlanetNetServer::set_physics_space);
	ClassDB::bind_method(D_METHOD("get_physics_space"), &PlanetNetServer::get_physics_space);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PlanetNetServer::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PlanetNetServer::get_collision_mask);
	ClassDB::bind_method(D_METHOD("add_exclude", "body"), &PlanetNetServer::add_exclude);
	ClassDB::bind_method(D_METHOD("clear_excludes"), &PlanetNetServer::clear_excludes);
	ClassDB::bind_method(D_METHOD("set_walk_speed", "speed"), &PlanetNetServer::set_walk_speed);
	ClassDB::bind_method(D_METHOD("get_walk_speed"), &PlanetNetServer::get_walk_speed);
	ClassDB::bind_method(D_METHOD("set_allow_teleport", "allow"), &PlanetNetServer::set_allow_teleport);
	ClassDB::bind_method(D_METHOD("get_allow_teleport"), &PlanetNetServer::get_allow_teleport);
	ClassDB::bind_method(D_METHOD("get_entity_walk_mode", "id"), &PlanetNetServer::get_entity_walk_mode);
	ClassDB::bind_method(D_METHOD("get_entity_stamina", "id"), &PlanetNetServer::get_entity_stamina);
	ClassDB::bind_method(D_METHOD("is_entity_invulnerable", "id"), &PlanetNetServer::is_entity_invulnerable);
	ClassDB::bind_method(D_METHOD("add_carve", "centre", "radius"), &PlanetNetServer::add_carve);
	ClassDB::bind_method(D_METHOD("clear_carves"), &PlanetNetServer::clear_carves);
	ClassDB::bind_method(D_METHOD("add_shell", "centre", "inner_radius", "outer_radius"), &PlanetNetServer::add_shell);
	ClassDB::bind_method(D_METHOD("clear_shells"), &PlanetNetServer::clear_shells);
	ClassDB::bind_method(D_METHOD("get_carve_count"), &PlanetNetServer::get_carve_count);
	ClassDB::bind_method(D_METHOD("get_ground_radius", "direction"), &PlanetNetServer::get_ground_radius);
	ClassDB::bind_method(D_METHOD("set_spawn_direction", "direction"), &PlanetNetServer::set_spawn_direction);
	ClassDB::bind_method(D_METHOD("get_spawn_direction"), &PlanetNetServer::get_spawn_direction);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "spawn_direction"), "set_spawn_direction", "get_spawn_direction");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "server_id", PROPERTY_HINT_RANGE, "1,65535"), "set_server_id", "get_server_id");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "region_size", PROPERTY_HINT_NONE, "suffix:m"), "set_region_size", "get_region_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interest_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_interest_radius", "get_interest_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "client_budget", PROPERTY_HINT_NONE, "suffix:B/s"), "set_client_budget", "get_client_budget");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "delta_enabled"), "set_delta_enabled", "is_delta_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::RID, "physics_space"), "set_physics_space", "get_physics_space");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "walk_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_walk_speed", "get_walk_speed");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "allow_teleport"), "set_allow_teleport", "get_allow_teleport");

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
	ClassDB::bind_method(D_METHOD("get_region_centre", "key"), &PlanetNetServer::get_region_centre);
	ClassDB::bind_method(D_METHOD("set_volume_cell", "size"), &PlanetNetServer::set_volume_cell);
	ClassDB::bind_method(D_METHOD("expect_player", "key", "position", "noclip"), &PlanetNetServer::expect_player);
	ClassDB::bind_method(D_METHOD("get_volume_cell"), &PlanetNetServer::get_volume_cell);
	ClassDB::bind_method(D_METHOD("get_client_entity", "peer"), &PlanetNetServer::get_client_entity);
	ClassDB::bind_method(D_METHOD("get_client_peers"), &PlanetNetServer::get_client_peers);
	ClassDB::bind_method(D_METHOD("get_client_view", "peer"), &PlanetNetServer::get_client_view);
	ClassDB::bind_method(D_METHOD("get_stats"), &PlanetNetServer::get_stats);
	ClassDB::bind_method(D_METHOD("get_client_stats", "peer"), &PlanetNetServer::get_client_stats);
	ClassDB::bind_method(D_METHOD("get_client_applied", "peer", "tick"), &PlanetNetServer::get_client_applied);

	ClassDB::bind_method(D_METHOD("set_world_path", "path"), &PlanetNetServer::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &PlanetNetServer::get_world_path);
	ClassDB::bind_method(D_METHOD("set_autosave_interval", "seconds"), &PlanetNetServer::set_autosave_interval);
	ClassDB::bind_method(D_METHOD("get_autosave_interval"), &PlanetNetServer::get_autosave_interval);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "world_path", PROPERTY_HINT_DIR), "set_world_path", "get_world_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "autosave_interval", PROPERTY_HINT_RANGE, "0,3600,0.1,suffix:s"), "set_autosave_interval", "get_autosave_interval");
	ClassDB::bind_method(D_METHOD("save_world"), &PlanetNetServer::save_world);
	ClassDB::bind_method(D_METHOD("load_world"), &PlanetNetServer::load_world);
	ClassDB::bind_method(D_METHOD("get_last_load_report"), &PlanetNetServer::get_last_load_report);
	ClassDB::bind_method(D_METHOD("set_entity_persistent", "id", "persistent"), &PlanetNetServer::set_entity_persistent);
	ClassDB::bind_method(D_METHOD("is_entity_persistent", "id"), &PlanetNetServer::is_entity_persistent);
	ClassDB::bind_method(D_METHOD("get_player_keys"), &PlanetNetServer::get_player_keys);
	ClassDB::bind_method(D_METHOD("get_player_entity", "key"), &PlanetNetServer::get_player_entity);
	ClassDB::bind_method(D_METHOD("get_player_position", "key"), &PlanetNetServer::get_player_position);

	ADD_SIGNAL(MethodInfo("client_connected", PropertyInfo(Variant::INT, "peer"), PropertyInfo(Variant::INT, "entity_id")));
	ADD_SIGNAL(MethodInfo("hit", PropertyInfo(Variant::DICTIONARY, "hit")));
	ADD_SIGNAL(MethodInfo("cast", PropertyInfo(Variant::DICTIONARY, "cast")));
	ADD_SIGNAL(MethodInfo("entity_died", PropertyInfo(Variant::INT, "id")));
	ClassDB::bind_method(D_METHOD("set_entity_health", "id", "health_max"), &PlanetNetServer::set_entity_health);
	ClassDB::bind_method(D_METHOD("set_solid_type", "type", "radius"), &PlanetNetServer::set_solid_type);
	ClassDB::bind_method(D_METHOD("get_solid_types"), &PlanetNetServer::get_solid_types);
	ClassDB::bind_method(D_METHOD("get_entity_health", "id"), &PlanetNetServer::get_entity_health);
	ClassDB::bind_method(D_METHOD("get_entity_health_max", "id"), &PlanetNetServer::get_entity_health_max);
	ClassDB::bind_method(D_METHOD("get_entity_action", "id"), &PlanetNetServer::get_entity_action);
	ClassDB::bind_method(D_METHOD("get_entity_poise", "id"), &PlanetNetServer::get_entity_poise);
	ClassDB::bind_method(D_METHOD("create_npc", "type", "position"), &PlanetNetServer::create_npc);
	ClassDB::bind_method(D_METHOD("set_entity_input", "id", "move", "yaw", "buttons", "abilities", "aim"), &PlanetNetServer::set_entity_input, DEFVAL(0), DEFVAL(Vector3()));
	ClassDB::bind_method(D_METHOD("get_entity_walker", "id"), &PlanetNetServer::get_entity_walker);
	ClassDB::bind_method(D_METHOD("set_entity_team", "id", "team"), &PlanetNetServer::set_entity_team);
	ClassDB::bind_method(D_METHOD("send_message", "peer", "channel", "text"), &PlanetNetServer::send_message);
	ClassDB::bind_method(D_METHOD("send_data", "peer", "kind", "bytes"), &PlanetNetServer::send_data);
	ClassDB::bind_method(D_METHOD("broadcast_message", "channel", "text"), &PlanetNetServer::broadcast_message);
	ClassDB::bind_method(D_METHOD("get_entity_peer", "id"), &PlanetNetServer::get_entity_peer);
	ClassDB::bind_method(D_METHOD("set_epoch", "unix_seconds"), &PlanetNetServer::set_epoch);
	ClassDB::bind_method(D_METHOD("get_epoch"), &PlanetNetServer::get_epoch);
	ClassDB::bind_method(D_METHOD("slew_epoch", "unix_seconds"), &PlanetNetServer::slew_epoch);
	ClassDB::bind_method(D_METHOD("export_entities", "ids"), &PlanetNetServer::export_entities);
	ClassDB::bind_method(D_METHOD("import_entities", "bytes", "mirror"), &PlanetNetServer::import_entities);
	ClassDB::bind_method(D_METHOD("is_entity_mirror", "id"), &PlanetNetServer::is_entity_mirror);
	ClassDB::bind_method(D_METHOD("is_entity_awaiting", "id"), &PlanetNetServer::is_entity_awaiting);
	ClassDB::bind_method(D_METHOD("detach_client", "peer"), &PlanetNetServer::detach_client);
	ClassDB::bind_method(D_METHOD("send_transfer", "peer", "address", "port"), &PlanetNetServer::send_transfer);
	ClassDB::bind_method(D_METHOD("is_client_standby", "peer"), &PlanetNetServer::is_client_standby);
	ClassDB::bind_method(D_METHOD("get_client_key", "peer"), &PlanetNetServer::get_client_key);
	ClassDB::bind_method(D_METHOD("strike", "attacker", "target", "at"), &PlanetNetServer::strike);
	ClassDB::bind_method(D_METHOD("relay_hit", "hit"), &PlanetNetServer::relay_hit);
	ClassDB::bind_method(D_METHOD("set_entity_noclip", "id", "on"), &PlanetNetServer::set_entity_noclip);
	ClassDB::bind_method(D_METHOD("is_entity_noclip", "id"), &PlanetNetServer::is_entity_noclip);
	ADD_SIGNAL(MethodInfo("teleport_requested", PropertyInfo(Variant::INT, "entity_id"), PropertyInfo(Variant::VECTOR3, "point")));
	ClassDB::bind_method(D_METHOD("set_space", "space"), &PlanetNetServer::set_space);
	ClassDB::bind_method(D_METHOD("get_space"), &PlanetNetServer::get_space);
	ClassDB::bind_method(D_METHOD("set_use_terrain", "use"), &PlanetNetServer::set_use_terrain);
	ClassDB::bind_method(D_METHOD("get_use_terrain"), &PlanetNetServer::get_use_terrain);
	ClassDB::bind_method(D_METHOD("set_uniform_down", "down"), &PlanetNetServer::set_uniform_down);
	ClassDB::bind_method(D_METHOD("get_uniform_down"), &PlanetNetServer::get_uniform_down);
	ClassDB::bind_method(D_METHOD("save_regions", "keys", "unload"), &PlanetNetServer::save_regions);
	ClassDB::bind_method(D_METHOD("load_regions", "keys"), &PlanetNetServer::load_regions);
	ClassDB::bind_method(D_METHOD("add_world_change", "id", "point", "data"), &PlanetNetServer::add_world_change);
	ClassDB::bind_method(D_METHOD("get_entities_in_region", "key"), &PlanetNetServer::get_entities_in_region);
	ADD_SIGNAL(MethodInfo("mirror_struck", PropertyInfo(Variant::INT, "attacker"), PropertyInfo(Variant::INT, "target"), PropertyInfo(Variant::VECTOR3, "at")));
	ADD_SIGNAL(MethodInfo("command_received", PropertyInfo(Variant::INT, "peer"), PropertyInfo(Variant::INT, "entity_id"), PropertyInfo(Variant::STRING, "line")));
	BIND_CONSTANT(MESSAGE_INFO);
	BIND_CONSTANT(MESSAGE_ERROR);
	BIND_CONSTANT(MESSAGE_CHAT);
	BIND_CONSTANT(MESSAGE_SYSTEM);
	ClassDB::bind_method(D_METHOD("set_entity_awareness", "id", "level"), &PlanetNetServer::set_entity_awareness);
	ClassDB::bind_method(D_METHOD("get_entity_awareness", "id"), &PlanetNetServer::get_entity_awareness);
	ClassDB::bind_method(D_METHOD("get_entity_team", "id"), &PlanetNetServer::get_entity_team);
	BIND_CONSTANT(TEAM_PLAYERS);
	BIND_CONSTANT(TEAM_ENEMIES);
	ClassDB::bind_method(D_METHOD("set_max_rewind_ticks", "ticks"), &PlanetNetServer::set_max_rewind_ticks);
	ClassDB::bind_method(D_METHOD("get_max_rewind_ticks"), &PlanetNetServer::get_max_rewind_ticks);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_rewind_ticks"), "set_max_rewind_ticks", "get_max_rewind_ticks");
	ClassDB::bind_method(D_METHOD("get_entity_combat", "id"), &PlanetNetServer::get_entity_combat);
	ClassDB::bind_method(D_METHOD("set_entity_loadout", "id", "moves"), &PlanetNetServer::set_entity_loadout);
	ClassDB::bind_method(D_METHOD("set_player_hits", "on"), &PlanetNetServer::set_player_hits);
	ClassDB::bind_method(D_METHOD("get_player_hits"), &PlanetNetServer::get_player_hits);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "player_hits"), "set_player_hits", "get_player_hits");
	ADD_SIGNAL(MethodInfo("client_disconnected", PropertyInfo(Variant::INT, "peer")));
	ADD_SIGNAL(MethodInfo("world_change_requested", PropertyInfo(Variant::INT, "peer"), PropertyInfo(Variant::VECTOR3, "point"),
			PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data")));
	ClassDB::bind_method(D_METHOD("commit_world_change", "point", "data"), &PlanetNetServer::commit_world_change);
	ClassDB::bind_method(D_METHOD("get_world_changes"), &PlanetNetServer::get_world_changes);
	ClassDB::bind_method(D_METHOD("get_world_change_count"), &PlanetNetServer::get_world_change_count);

	BIND_CONSTANT(ENTITY_TYPE_PLAYER);
	BIND_CONSTANT(HISTORY_TICKS);
}
