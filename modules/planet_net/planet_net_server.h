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
	// The most a walker handed here (or woken from a store) is stepped on to catch up to now: ten seconds.
	// A handover between servers is a fraction of one; a record from the store can be hours old, and
	// stepping every tick since was 13 s of one poll on a world unserved for six hours.
	static constexpr uint32_t CARRY_MAX_TICKS = 10 * PlanetNet::TICK_HZ;
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

	// A change to the world itself -- a dig, later a placed block -- as an opaque payload at a point. The
	// network layer does not know what it means: it journals it per region, saves it with the region,
	// and delivers it to every client whose interest covers that region, including clients that
	// arrive later. Applying one must be the same on every machine and repeatable without harm.
	struct WorldChange {
		uint64_t id = 0;
		Vector3 point;
		PackedByteArray data;
	};

	// A player as saved: who they are, which entity is theirs, and where they were when last seen.
	struct PlayerRecord {
		uint64_t entity = 0;
		PlanetNet::EntityState state;
		bool online = false;
	};

	struct Client {
		uint64_t entity = 0;
		bool welcomed = false;
		String player_key;
		// Inputs by the tick they are for. The step for tick T applies inputs[T], or repeats the last
		// input when it has not arrived; a late input is then useless and the client will be corrected.
		HashMap<uint32_t, PlanetNet::WalkerInput> inputs;
		// For each input, the tick its player was drawing others at when choosing it, and the one for the
		// tick being applied: where its swings are judged against (lag compensation).
		HashMap<uint32_t, uint32_t> input_views;
		uint32_t view_tick = 0;
		PlanetNet::WalkerInput last_input;
		bool has_input = false;
		// Handed here from another zone (a transformed import): its inputs before `turned_until` were chosen
		// in that zone's frame, and are turned by `turn` when applied (0: not told yet -- all of them).
		bool turned = false;
		Basis turn;
		uint32_t turned_until = 0;
		// Teleports applied, mod 256; snapshots carry it so the client can tell word from before one.
		uint8_t teleports = 0;
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
		// Regions whose world-change journal this client has been sent; new changes there follow.
		HashSet<uint32_t> journal_regions;
		// Connected ahead of a switch from another server (docs/SERVER_MESH.md): welcomed with no entity,
		// its inputs kept, until its player is handed here (import_entities) and bound to it.
		bool standby = false;
	};

	struct Entity {
		PlanetNet::EntityState state;
		int owner_peer = 0;
		uint32_t region = 0;
		bool indexed = false;
		// Saved with its region. Players are not: they are saved under their key and exist in the world
		// only while connected.
		bool persistent = true;
		// Players only: the walker's state beyond EntityState. Its position, velocity and rotation are not
		// kept here but in `state`, which is what replicates: read and write it through walker_state()
		// and store_walker_state(), never field by field -- new walker state then cannot be forgotten.
		PlanetNet::WalkerState walk;
		// A player imported from another zone's frame, by this carry, until its client is bound (_bind).
		bool arrival_carried = false;
		Transform3D arrival_carry;
		// Combat (docs/COMBAT.md). A target has health_max > 0: players (when player_hits) and what
		// set_entity_health makes one, such as a training dummy. Health refills after a rest.
		int32_t health = 0;
		int32_t health_max = 0;
		uint32_t hurt_tick = 0;
		// Ticks left of a hit reaction, for a target with no walker (drawn from its pose).
		uint8_t react = 0;
		// The swing in progress (by the tick it started) and what it has already struck: one blow each.
		uint32_t swing_start = 0;
		LocalVector<uint64_t> swing_hits;
		uint32_t cast_start = UINT32_MAX; // the aimed move last announced (MSG_CAST), by its start tick
		// Not a player but a walker all the same, stepped from `npc_input` (set_entity_input): an enemy.
		bool npc = false;
		PlanetNet::WalkerInput npc_input;
		// Its side: blows land only across sides (players are TEAM_PLAYERS, enemies TEAM_ENEMIES unless set
		// otherwise; 0, as the dummy, is on none and struck by all). Player against player is player_hits'.
		uint8_t team = 0;
		// An NPC's awareness of the players, its brain's (set_entity_awareness): 0 unaware, 1 suspicious,
		// 2 alerted. Drawn by every client (the pose's bits 13-14); presentation only -- a blow's damage is
		// positional (Walker::from_behind), not by what the target noticed.
		uint8_t awareness = 0;
		Vector3 history[HISTORY_TICKS];
		uint32_t history_tick[HISTORY_TICKS] = {};
		// Another server's, shown here (docs/SERVER_MESH.md): never stepped, struck or saved -- its owner
		// sends its state (import_entities with mirror), and a blow on it is the owner's to judge
		// (mirror_struck).
		bool mirror = false;
		// A player handed here from another server, or brought back by the world above, waiting for its
		// client: the first to arrive with this key (standby or not) is bound to it.
		String await_key;
		// The tick its walker state is from, when it was handed here: stepped on to the present when bound.
		uint32_t walk_tick = 0;
	};

	static bool has_walker(const Entity &p_entity) { return p_entity.state.type == ENTITY_TYPE_PLAYER || p_entity.npc; }
	static PlanetNet::WalkerState walker_state(const Entity &p_entity) {
		PlanetNet::WalkerState s = p_entity.walk;
		s.position = p_entity.state.position;
		s.velocity = p_entity.state.velocity;
		s.rotation = p_entity.state.rotation;
		return s;
	}
	static void store_walker_state(Entity &r_entity, const PlanetNet::WalkerState &p_state) {
		r_entity.walk = p_state;
		r_entity.state.position = p_state.position;
		r_entity.state.velocity = p_state.velocity;
		r_entity.state.rotation = p_state.rotation;
		r_entity.state.pose = PlanetNet::pose_of(p_state) | uint16_t(r_entity.awareness << PlanetNet::POSE_AWARE_SHIFT);
	}

	PlanetNetLink link;
	HashMap<int, Client> clients;
	HashMap<uint64_t, Entity> entities;
	bool allow_teleport = true;
	bool player_hits = false;
	HashMap<int, double> solid_types;
	int32_t player_health = 100;
	uint64_t hits_resolved = 0;
	HashMap<uint32_t, LocalVector<uint64_t>> region_index;
	PlanetNet::Regions regions;
	uint16_t server_id = 1;
	uint64_t next_entity = 1;
	uint32_t tick = 0;
	uint32_t snapshot_seq = 0;
	double accumulator = 0.0;
	// The shared clock (set_epoch): ticks counted from this wall-clock moment, the same on every server.
	// Negative: none, the tick counts poll()'s deltas as before.
	double epoch = -1.0;
	uint64_t space = 0;
	double planet_radius = 150000.0;
	PlanetNet::Walker walker;
	Vector3 spawn_direction = Vector3(0, 1, 0);
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
	void _resolve_swings();
	void _strike(uint64_t p_attacker, Entity &r_attacker, uint64_t p_target, Entity &r_target, const Vector3 &p_at);
	void _send_cast(uint64_t p_caster, const Entity &p_entity, uint32_t p_start);
	void _send_health(const Entity &p_entity);
	void _send_welcome(int p_peer, uint64_t p_entity, const Transform3D *p_carry = nullptr);
	static PlanetNet::WalkerInput _input_for(const Client &p_client, uint32_t p_tick, const PlanetNet::WalkerInput &p_input);
	// Binds a client to a player entity handed here, stepping it from when it left its old server to now
	// on the inputs the client has already sent here.
	void _bind(int p_peer, Client &r_client, uint64_t p_id);
	uint64_t _awaiting(const String &p_key) const;
	void _write_entity(PlanetNet::Writer &w, uint64_t p_id, const Entity &p_entity) const;

	// Persistence: see save_world().
	String world_path;
	double autosave_interval = 0.0;
	double autosave_timer = 0.0;
	HashMap<String, PlayerRecord> players;
	HashMap<uint32_t, LocalVector<WorldChange>> journal;
	uint64_t next_change = 1;
	void _send_change(int p_peer, const WorldChange &p_change);
	HashSet<uint32_t> saved_regions; // regions that have a file on disk
	Dictionary last_load_report;
	uint64_t _create_with_id(uint64_t p_id, int p_type, const Vector3 &p_position);
	void _release_client(Client &r_client);
	Error _write_file(const String &p_name, PlanetNet::Writer &r_writer) const;
	Error _read_file(const String &p_name, uint32_t p_magic, LocalVector<uint8_t> &r_bytes) const;

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
	// Holes players walk into (planet space, metres). The game adds one per committed dig; clients must
	// be given the same, or they mispredict over holes.
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
	int get_carve_count() const { return int(walker.carve_centres.size()); }
	double get_ground_radius(const Vector3 &p_dir) const { return walker.ground_radius(p_dir.normalized()); }
	// Whether clients may ask to be put anywhere (MSG_TELEPORT). A development tool; on for now.
	void set_allow_teleport(bool p_allow) { allow_teleport = p_allow; }
	bool get_allow_teleport() const { return allow_teleport; }
	// Colliders the walker stands on and climbs, besides the analytic ground (planet_net_walker.h): a
	// physics space, which layers of it, and bodies to ignore. Server and clients must see the same
	// colliders on these layers, or predictions miss wherever they differ.
	void set_physics_space(const RID &p_space) { walker.space = p_space; }
	RID get_physics_space() const { return walker.space; }
	void set_collision_mask(uint32_t p_mask) { walker.collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return walker.collision_mask; }
	void add_exclude(const RID &p_body) { walker.exclude.insert(p_body); }
	void clear_excludes() { walker.exclude.clear(); }
	void set_walk_speed(double p_speed) { walker.speed = p_speed; }
	double get_walk_speed() const { return walker.speed; }
	// Where new players appear (a direction from the planet's centre); they are spread a few metres
	// apart around it so they do not stack.
	void set_spawn_direction(const Vector3 &p_direction);
	Vector3 get_spawn_direction() const { return spawn_direction; }

	int get_tick() const { return int(tick); }
	// The server's clock in ticks, including the fraction of the tick in progress.
	double get_time_ticks() const { return double(tick) + accumulator * PlanetNet::TICK_HZ; }

	// The shared clock (docs/SERVER_MESH.md): from now, the tick is the time since this wall-clock moment
	// (Unix seconds) and poll() steps to it, so every server with the same epoch is on the same tick and a
	// client switching between them keeps its clock. Set before clients connect.
	void set_epoch(double p_unix_seconds);
	// The shared clock corrected (a sync with the universe's master clock): the epoch moved, the tick kept --
	// poll() then waits or catches up to it, never stepping a tick twice.
	void slew_epoch(double p_unix_seconds);
	double get_epoch() const { return epoch; }

	// Handing entities between servers (docs/SERVER_MESH.md). Exported whole -- state, walker, combat,
	// an enemy's input, a player's key -- with the tick they are from; imported with their ids. Imported
	// as mirrors, they are another server's, shown to clients here and updated by further imports; as
	// owned, they are simulated here from now on (a mirror of the same id becomes owned). A player
	// imported owned waits for its client (await_key), and is bound to it when it arrives -- or at once,
	// if it is already here on standby -- stepped to the present on the inputs it has sent.
	PackedByteArray export_entities(const PackedInt64Array &p_ids) const;
	// p_transform takes the exporter's frame to this server's: identity for a server of the same planet, a
	// seam's for one of another zone (a mirror seen across a gate's ring -- the game's docs/SERVER_MESH.md).
	PackedInt64Array import_entities(const PackedByteArray &p_bytes, bool p_mirror, const Transform3D &p_transform = Transform3D());
	bool is_entity_mirror(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr && e->mirror;
	}
	// Whether the entity is a player waiting for its client (import_entities).
	bool is_entity_awaiting(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr && !e->await_key.is_empty();
	}
	// The client's player leaves this server without the client leaving: its entity is no longer the
	// client's (export it first, then destroy it). The connection stays until the client closes it.
	void detach_client(int p_peer);
	// Tells a client to switch to another server (MSG_TRANSFER): it connects there on standby at once.
	void send_transfer(int p_peer, const String &p_address, int p_port);
	bool is_client_standby(int p_peer) const {
		const Client *c = clients.getptr(p_peer);
		return c != nullptr && c->standby;
	}
	// The inputs a client has sent that are kept (for the ticks to come, or a standby's of the last 2 s): a standby
	// client is ready to be handed its player once it has sent some, or the replay (_bind) has none to step on.
	int get_client_input_count(int p_peer) const {
		const Client *c = clients.getptr(p_peer);
		return c != nullptr ? int(c->inputs.size()) : 0;
	}
	String get_client_key(int p_peer) const {
		const Client *c = clients.getptr(p_peer);
		return c != nullptr ? c->player_key : String();
	}
	// A blow another server applied, told to the clients here that see either end (MSG_HIT): the
	// attacker's server, when its player struck a mirror of something the other owns. `hit` is that
	// server's `hit` signal's dictionary.
	void relay_hit(const Dictionary &p_hit);
	// A zone (docs/SERVER_MESH.md, portals): a space with no planet under it, only colliders, down one
	// way. Its clients must be set the same.
	// Which space this server runs: 0 the planet, else a zone's id. Its clients are told on welcome.
	void set_space(int64_t p_space) { space = uint64_t(p_space); }
	int64_t get_space() const { return int64_t(space); }
	void set_use_terrain(bool p_use) { walker.use_terrain = p_use; }
	bool get_use_terrain() const { return walker.use_terrain; }
	void set_uniform_down(const Vector3 &p_down) { walker.uniform_down = p_down; }
	Vector3 get_uniform_down() const { return walker.uniform_down; }
	// A zone's gravity, m/s^2 (0: none -- a level with no floor, docs LEVELS.md); told to its clients with its down.
	void set_gravity(double p_gravity) { walker.gravity = MAX(p_gravity, 0.0); }
	double get_gravity() const { return walker.gravity; }
	// A blow judged elsewhere (the attacker's server, against its mirror of a target owned here): applied
	// as if struck here, the attacker's state as mirrored here (docs/SERVER_MESH.md, fighting across
	// servers).
	void strike(int64_t p_attacker, int64_t p_target, const Vector3 &p_at);

	// A DM's flight (FLAG_NOCLIP; docs/SERVER_MESH.md, DM mode): through everything, never struck, nobody's
	// body; its client predicts it from the next snapshot on.
	// A player coming from another zone's servers (planet-demo docs/SYSTEM_WORLD.md, "Everything is a zone"):
	// made here, at `p_position`, waiting for its client's key, so that the client's switch binds to it as a
	// handoff's does. A DM's (noclip) stays one.
	int64_t expect_player(const String &p_key, const Vector3 &p_position, bool p_noclip);
	void set_entity_noclip(int64_t p_id, bool p_on) {
		Entity *e = entities.getptr(uint64_t(p_id));
		ERR_FAIL_NULL(e);
		PlanetNet::WalkerState s = walker_state(*e);
		s.flags = p_on ? (s.flags | PlanetNet::FLAG_NOCLIP) : (s.flags & ~PlanetNet::FLAG_NOCLIP);
		store_walker_state(*e, s);
	}
	bool is_entity_noclip(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr && (e->walk.flags & PlanetNet::FLAG_NOCLIP);
	}
	// One region at a time (docs/SERVER_MESH.md: a cell is a region): its persistent entities and its
	// journal written to its file -- and, unloading, dropped from this server, when it goes to another
	// owner or to sleep. Loading reads them back, as owned. Players are never in a region's file.
	Error save_regions(const PackedInt32Array &p_keys, bool p_unload);
	PackedInt64Array load_regions(const PackedInt32Array &p_keys);
	// A world change committed by another server, near enough to matter here: journalled and sent on as
	// if committed here (its id is kept, so it arrives once however many ways it comes).
	void add_world_change(int64_t p_id, const Vector3 &p_point, const PackedByteArray &p_data);
	// The regions holding the entities this server owns (not mirrors), and the entities in one region.
	PackedInt64Array get_entities_in_region(int p_key) const;

	int64_t create_entity(int p_type, const Vector3 &p_position);
	// An enemy (docs/COMBAT.md, "The bandit"): a walker like a player's, stepped every tick from what
	// set_entity_input last set -- a script's brain on the host -- judged by the same rules, its moves
	// against players, players' against it. Not saved. At 0 health it is down for good (ACTION_DEATH)
	// and `entity_died` says so; what follows is the script's.
	int64_t create_npc(int p_type, const Vector3 &p_position);
	void set_entity_input(int64_t p_id, const Vector2 &p_move, float p_yaw, int p_buttons, int p_abilities, const Vector3 &p_aim);
	static constexpr int TEAM_PLAYERS = 1;
	static constexpr int TEAM_ENEMIES = 2;
	void set_entity_team(int64_t p_id, int p_team) {
		Entity *e = entities.getptr(uint64_t(p_id));
		ERR_FAIL_NULL(e);
		e->team = uint8_t(CLAMP(p_team, 0, 255));
	}
	int get_entity_team(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->team) : 0;
	}
	// Console commands (MSG_COMMAND, MSG_MESSAGE): a client's typed line arrives as `command_received(peer,
	// entity_id, line)`, raw -- the scripts parse and route it (planet-demo net/commands/) -- and what
	// it says back, or anything else for a player to read, goes out by channel (MessageChannel: info,
	// error, chat, system).
	void send_message(int p_peer, int p_channel, const String &p_text);
	// Bulk data to one client, for its scripts (MSG_DATA): at most MAX_DATA bytes, reliable, ordered.
	void send_data(int p_peer, int p_kind, const PackedByteArray &p_bytes);
	void broadcast_message(int p_channel, const String &p_text);
	// The peer a player's entity is (0: not a player, or gone).
	int get_entity_peer(int64_t p_id) const;
	enum { MESSAGE_INFO = PlanetNet::MESSAGE_INFO, MESSAGE_ERROR = PlanetNet::MESSAGE_ERROR, MESSAGE_CHAT = PlanetNet::MESSAGE_CHAT, MESSAGE_SYSTEM = PlanetNet::MESSAGE_SYSTEM };
	void set_entity_awareness(int64_t p_id, int p_level) {
		Entity *e = entities.getptr(uint64_t(p_id));
		ERR_FAIL_NULL(e);
		e->awareness = uint8_t(CLAMP(p_level, 0, 3));
		if (has_walker(*e)) {
			e->state.pose = uint16_t((e->state.pose & ~(3u << PlanetNet::POSE_AWARE_SHIFT)) | (e->awareness << PlanetNet::POSE_AWARE_SHIFT));
		}
	}
	int get_entity_awareness(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->awareness) : 0;
	}
	// A walker's state for scripts: {position, velocity, rotation, up, reference, mode, action,
	// action_ticks, stamina, poise, focus, loadout, cooldowns, aim, health, health_max, dead, no_climb}.
	Dictionary get_entity_walker(int64_t p_id) const;
	void destroy_entity(int64_t p_id);
	bool has_entity(int64_t p_id) const { return entities.has(uint64_t(p_id)); }
	PackedInt64Array get_entity_ids() const;
	Vector3 get_entity_position(int64_t p_id) const;
	// A player's walker mode (PlanetNet::WalkerMode: 0 ground, 1 air, 2 climbing, 3 gliding); 0 for
	// anything else.
	int get_entity_walk_mode(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->walk.mode) : 0;
	}
	// A player's stamina, in the walker's units (0..stamina_max).
	int get_entity_stamina(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->walk.stamina) : 0;
	}
	// Combat (docs/COMBAT.md). A target: an entity given health, which swings in their window strike
	// (players are targets of each other only with player_hits). The server judges every swing against
	// where the targets were on the attacker's screen (lag compensation, at most max_rewind_ticks back) and
	// tells the clients that see either what it did (MSG_HIT), and scripts through `hit`.
	// How far back a swing may be judged: the clients' interpolation delay (100 ms, 3 ticks) and about
	// 170 ms of an attacker's own latency. Past that the attacker must lead its target: a body that rolled
	// out of reach was struck where it had stood half a second before (docs/COMBAT_DESIGN.md section 7).
	// Whether the target is dodging or guarding is read now, whatever the rewind.
	static constexpr int MAX_REWIND_TICKS = 8;
	int max_rewind_ticks = MAX_REWIND_TICKS;
	void set_max_rewind_ticks(int p_ticks) { max_rewind_ticks = CLAMP(p_ticks, 0, HISTORY_TICKS - 1); }
	int get_max_rewind_ticks() const { return max_rewind_ticks; }
	static constexpr double TARGET_RADIUS = 0.35;
	// What a hit does -- damage, poise damage, push, what the guard pays -- is the move's
	// (planet_net_moves.h). A guard that cannot pay breaks, the body pushed back this hard.
	static constexpr double GUARD_BREAK_PUSH = 4.0;
	static constexpr int HEALTH_REST_TICKS = 5 * PlanetNet::TICK_HZ;
	void set_entity_health(int64_t p_id, int p_health_max);
	int get_entity_health(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->health) : 0;
	}
	int get_entity_health_max(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->health_max) : 0;
	}
	void set_player_hits(bool p_on) { player_hits = p_on; }
	bool get_player_hits() const { return player_hits; }
	// A player's action now (PlanetNet::WalkerAction) and ticks into it.
	int get_entity_action(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->walk.action) : 0;
	}
	// A player's poise (Walker::take_hit).
	int get_entity_poise(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? int(e->walk.poise) : 0;
	}
	// A player's combat state: {poise, focus, loadout, cooldowns, aim} (docs/COMBAT_DESIGN.md).
	Dictionary get_entity_combat(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr ? PlanetNet::combat_of(e->walk) : Dictionary();
	}
	// A player's abilities, by move id per slot (0: empty). Its client predicts with them from the next
	// snapshot on.
	void set_entity_loadout(int64_t p_id, const PackedInt32Array &p_moves) {
		Entity *e = entities.getptr(uint64_t(p_id));
		ERR_FAIL_NULL(e);
		for (int k = 0; k < PlanetNet::ABILITY_SLOTS; ++k) {
			e->walk.loadout[k] = k < p_moves.size() ? uint8_t(CLAMP(p_moves[k], 0, 255)) : 0;
		}
	}

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

	// Whether hits pass through the entity this tick (a player mid-dodge).
	bool is_entity_invulnerable(int64_t p_id) const {
		const Entity *e = entities.getptr(uint64_t(p_id));
		return e != nullptr && walker.invulnerable(e->walk);
	}
	void set_entity_position(int64_t p_id, const Vector3 &p_position);
	void set_entity_state(int64_t p_id, const Vector3 &p_position, const Quaternion &p_rotation, const Vector3 &p_velocity);
	// Where the entity was at a (fractional) past tick, from the history ring. Returns the current
	// position when the tick is outside what is kept.
	Vector3 get_entity_position_at(int64_t p_id, double p_tick) const;
	int get_entity_owner(int64_t p_id) const;
	int get_region_of(const Vector3 &p_position) const { return int(regions.key_of(p_position)); }
	Vector3 get_region_centre(int p_key) const { return regions.centre(uint32_t(p_key)); }
	// Volume mode (PlanetNet::Regions): every zone that is not a planet's surface. 0 is the surface's.
	void set_volume_cell(double p_size) { regions.volume_cell = MAX(p_size, 0.0); }
	double get_volume_cell() const { return regions.volume_cell; }
	int64_t get_client_entity(int p_peer) const;
	PackedInt32Array get_client_peers() const;
	// Entities the client is currently told about (not counting ones being removed).
	PackedInt64Array get_client_view(int p_peer) const;

	Dictionary get_stats() const;
	Dictionary get_client_stats(int p_peer) const;

	// The world on disk: one file per occupied region holding its persistent entities, plus the
	// players table and a header. Written atomically (temporary file, then rename); a region that
	// has emptied loses its file. Load before listening, into an empty world.
	void set_world_path(const String &p_path) { world_path = p_path; }
	String get_world_path() const { return world_path; }
	// Seconds between automatic saves while polling; 0 disables them.
	void set_autosave_interval(double p_seconds) { autosave_interval = MAX(p_seconds, 0.0); }
	double get_autosave_interval() const { return autosave_interval; }
	Error save_world();
	Error load_world();
	// What the last load found: regions, entities, players, and any files it had to skip.
	Dictionary get_last_load_report() const { return last_load_report; }
	void set_entity_persistent(int64_t p_id, bool p_persistent);
	bool is_entity_persistent(int64_t p_id) const;
	PackedStringArray get_player_keys() const;
	int64_t get_player_entity(const String &p_key) const;
	Vector3 get_player_position(const String &p_key) const;

	// World changes (see WorldChange). A client's request arrives as world_change_requested; the script
	// decides, and commit_world_change() journals it and sends it on. Returns the change's id.
	int64_t commit_world_change(const Vector3 &p_point, const PackedByteArray &p_data);
	// All committed changes, oldest first: [{id, point, data}]. For a server rebuilding its own copy
	// of the world after load_world().
	Array get_world_changes() const;
	int get_world_change_count() const;
	// The input the server applied to the client's player at a recent tick, and whether it arrived in
	// time or was repeated from the tick before: {tick, move, yaw, arrived, position}, or {} if too old.
	Dictionary get_client_applied(int p_peer, int p_tick) const;

	~PlanetNetServer() { stop(); }
};
