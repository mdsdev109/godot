/**************************************************************************/
/*  planet_net_link.cpp                                                   */
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

#include "planet_net_link.h"

#include "planet_net_protocol.h"

#include "core/os/os.h"

#include <enet/enet.h>

Error PlanetNetLink::listen(int p_port, int p_max_peers) {
	close();
	host.instantiate();
	const Error err = host->create_host_bound(IPAddress("*"), p_port, p_max_peers, PlanetNet::CHANNEL_COUNT);
	if (err != OK) {
		host.unref();
	}
	return err;
}

int PlanetNetLink::connect_to(const String &p_address, int p_port) {
	close();
	host.instantiate();
	if (host->create_host(1, PlanetNet::CHANNEL_COUNT) != OK) {
		host.unref();
		return 0;
	}
	const Ref<ENetPacketPeer> peer = host->connect_to_host(p_address, p_port, PlanetNet::CHANNEL_COUNT);
	if (peer.is_null()) {
		host.unref();
		return 0;
	}
	return _id_for(peer);
}

void PlanetNetLink::close() {
	if (host.is_valid()) {
		for (KeyValue<int, Ref<ENetPacketPeer>> &E : peers) {
			if (E.value.is_valid() && E.value->is_active()) {
				E.value->peer_disconnect_now();
			}
		}
		host->flush();
		host->destroy();
		host.unref();
	}
	peers.clear();
	peer_ids.clear();
	last_reliable_due.clear();
	held.clear();
}

void PlanetNetLink::disconnect_peer(int p_peer) {
	const Ref<ENetPacketPeer> *peer = peers.getptr(p_peer);
	if (peer != nullptr && peer->is_valid() && (*peer)->is_active()) {
		(*peer)->peer_disconnect_later();
	}
}

int PlanetNetLink::get_local_port() const {
	return host.is_valid() ? host->get_local_port() : 0;
}

int PlanetNetLink::_id_for(const Ref<ENetPacketPeer> &p_peer) {
	const int *existing = peer_ids.getptr(p_peer.ptr());
	if (existing != nullptr) {
		return *existing;
	}
	const int id = next_peer_id++;
	peer_ids.insert(p_peer.ptr(), id);
	peers.insert(id, p_peer);
	return id;
}

void PlanetNetLink::send(int p_peer, int p_channel, const LocalVector<uint8_t> &p_data, bool p_reliable) {
	if (host.is_null()) {
		return;
	}
	bytes_sent += p_data.size();
	if (conditions.latency_ms <= 0.0 && conditions.jitter_ms <= 0.0 && conditions.loss <= 0.0) {
		_send_now(p_peer, p_channel, p_data.ptr(), p_data.size(), p_reliable);
		return;
	}
	const bool lost = conditions.loss > 0.0 && rng.randf() < conditions.loss;
	if (lost && !p_reliable) {
		return;
	}
	double delay_ms = conditions.latency_ms + conditions.jitter_ms * (2.0 * double(rng.randf()) - 1.0);
	if (lost) {
		delay_ms += 2.0 * conditions.latency_ms; // a retransmit costs a round trip
	}
	uint64_t due = OS::get_singleton()->get_ticks_usec() + uint64_t(MAX(delay_ms, 0.0) * 1000.0);
	if (p_reliable) {
		// Reliable traffic is ordered: jitter may not let a later packet overtake an earlier one.
		uint64_t &last = last_reliable_due[p_peer];
		due = MAX(due, last);
		last = due;
	}
	Held h;
	h.due_usec = due;
	h.peer = p_peer;
	h.channel = p_channel;
	h.reliable = p_reliable;
	h.data = p_data;
	held.push_back(h);
}

void PlanetNetLink::_send_now(int p_peer, int p_channel, const uint8_t *p_data, uint32_t p_size, bool p_reliable) {
	const Ref<ENetPacketPeer> *peer = peers.getptr(p_peer);
	// Only to peers fully connected: one being refused or still handshaking has no channels yet.
	if (peer == nullptr || peer->is_null() || (*peer)->get_state() != ENetPacketPeer::STATE_CONNECTED) {
		return;
	}
	// Unreliable packets bigger than the MTU are fragmented unreliably too, rather than being upgraded
	// to reliable delivery, which ENet does by default and which would stall the stream on loss.
	const uint32_t flags = p_reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	ENetPacket *packet = enet_packet_create(p_data, p_size, flags);
	if ((*peer)->send(uint8_t(p_channel), packet) != 0) {
		enet_packet_destroy(packet);
	}
}

void PlanetNetLink::poll(LocalVector<Event> &r_events) {
	if (host.is_null()) {
		return;
	}
	if (!held.is_empty()) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		uint32_t kept = 0;
		// In submission order, so reliable packets that became due together leave in order.
		for (uint32_t i = 0; i < held.size(); ++i) {
			if (held[i].due_usec <= now) {
				_send_now(held[i].peer, held[i].channel, held[i].data.ptr(), held[i].data.size(), held[i].reliable);
			} else {
				if (kept != i) {
					held[kept] = held[i];
				}
				++kept;
			}
		}
		held.resize(kept);
	}

	while (true) {
		ENetConnection::Event event;
		const ENetConnection::EventType type = host->service(0, event);
		if (type == ENetConnection::EVENT_NONE || type == ENetConnection::EVENT_ERROR) {
			break;
		}
		Event out;
		out.peer = event.peer.is_valid() ? _id_for(event.peer) : 0;
		if (type == ENetConnection::EVENT_CONNECT) {
			out.kind = EVENT_CONNECTED;
		} else if (type == ENetConnection::EVENT_DISCONNECT) {
			out.kind = EVENT_DISCONNECTED;
			if (event.peer.is_valid()) {
				peer_ids.erase(event.peer.ptr());
			}
			peers.erase(out.peer);
			last_reliable_due.erase(out.peer);
		} else {
			out.kind = EVENT_RECEIVED;
			out.channel = event.channel_id;
			if (event.packet != nullptr) {
				out.data.resize(uint32_t(event.packet->dataLength));
				memcpy(out.data.ptr(), event.packet->data, event.packet->dataLength);
				bytes_received += event.packet->dataLength;
				enet_packet_destroy(event.packet);
			}
		}
		r_events.push_back(out);
	}
	host->flush();
}
