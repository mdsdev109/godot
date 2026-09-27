/**************************************************************************/
/*  planet_net_link.h                                                     */
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

// One ENet host plus a network-conditions simulator in front of it.
//
// ENet has no way to simulate a bad connection, and every test of this layer runs over localhost, where
// packets arrive instantly and never get lost. So outgoing packets can be held back by a latency with
// jitter, and unreliable ones dropped at a loss rate, before ENet sees them. Reliable packets are never
// dropped -- ENet would not know to resend them -- but a "lost" one is delayed by a round trip, which is
// what a retransmit costs, and reliable packets stay in order. Only the sender delays, so the latency
// set on each end is that end's one-way latency.

#include "modules/enet/enet_connection.h"
#include "modules/enet/enet_packet_peer.h"

#include "core/math/random_pcg.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

class PlanetNetLink {
public:
	struct Conditions {
		double latency_ms = 0.0;
		double jitter_ms = 0.0;
		double loss = 0.0;
	};

	enum EventKind {
		EVENT_CONNECTED,
		EVENT_DISCONNECTED,
		EVENT_RECEIVED,
	};

	struct Event {
		EventKind kind = EVENT_RECEIVED;
		int peer = 0;
		int channel = 0;
		LocalVector<uint8_t> data;
	};

	Conditions conditions;

	// Server side.
	Error listen(int p_port, int p_max_peers);
	// Client side. Returns the id the server will be known by once EVENT_CONNECTED arrives.
	int connect_to(const String &p_address, int p_port);
	void close();
	void disconnect_peer(int p_peer);

	void send(int p_peer, int p_channel, const LocalVector<uint8_t> &p_data, bool p_reliable);
	// Releases held packets that are due, services ENet, and appends what arrived.
	void poll(LocalVector<Event> &r_events);

	bool is_open() const { return host.is_valid(); }
	int get_local_port() const;
	uint64_t get_bytes_sent() const { return bytes_sent; }
	uint64_t get_bytes_received() const { return bytes_received; }

	~PlanetNetLink() { close(); }

private:
	struct Held {
		uint64_t due_usec = 0;
		int peer = 0;
		int channel = 0;
		bool reliable = false;
		LocalVector<uint8_t> data;
	};

	Ref<ENetConnection> host;
	HashMap<int, Ref<ENetPacketPeer>> peers;
	HashMap<ENetPacketPeer *, int> peer_ids;
	HashMap<int, uint64_t> last_reliable_due;
	LocalVector<Held> held;
	RandomPCG rng = RandomPCG(0x5eed);
	int next_peer_id = 1;
	uint64_t bytes_sent = 0;
	uint64_t bytes_received = 0;

	int _id_for(const Ref<ENetPacketPeer> &p_peer);
	void _send_now(int p_peer, int p_channel, const uint8_t *p_data, uint32_t p_size, bool p_reliable);
};
