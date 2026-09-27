/**************************************************************************/
/*  planet_net_protocol.h                                                 */
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

// The wire format shared by PlanetNetServer and PlanetNetClient. See planet-demo's docs/NETWORKING.md.
//
// Everything is little-endian, which every platform the game targets is; values are copied byte for
// byte, so a double written on one machine reads back as the same double on another.

#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

#include <cstdint>
#include <cstring>

namespace PlanetNet {

// Simulation ticks per second on the server, and snapshots per second to each client.
constexpr int TICK_HZ = 30;
constexpr int SNAPSHOT_HZ = 20;

// 0: reliable ordered control (hello, welcome). 1: unreliable sequenced state (snapshots, input, clock).
// 2: reliable ordered world changes, for voxel edits later.
constexpr int CHANNEL_CONTROL = 0;
constexpr int CHANNEL_STATE = 1;
constexpr int CHANNEL_WORLD = 2;
constexpr int CHANNEL_COUNT = 3;

constexpr uint16_t PROTOCOL_VERSION = 1;

enum MessageType : uint8_t {
	MSG_HELLO = 1, // client -> server: u16 version
	MSG_WELCOME, // server -> client: u16 version, u16 server id, u64 your entity, f64 server time (ticks)
	MSG_PING, // client -> server: f64 client clock (s)
	MSG_PONG, // server -> client: f64 client clock echoed, f64 server time (ticks)
	MSG_INPUT, // client -> server: u8 n, n x (u32 tick, f32 move x, f32 move y, f32 yaw)
	MSG_SNAPSHOT, // server -> client: u32 tick, u16 n, n x entity state
};

// Ids carry the server that created them in the top 16 bits, so they stay unique across servers.
constexpr int ENTITY_SERVER_SHIFT = 48;

// What one entity looks like on the wire, and to scripts on the client.
struct EntityState {
	uint64_t id = 0;
	uint16_t type = 0;
	// The frame the position is in. 0 is the planet's body-fixed frame; ships will be frames later.
	uint64_t frame = 0;
	Vector3 position;
	Quaternion rotation;
	Vector3 velocity;
};

class Writer {
	LocalVector<uint8_t> bytes;

	template <typename T>
	void put(const T &p_value) {
		const uint32_t at = bytes.size();
		bytes.resize(at + sizeof(T));
		std::memcpy(bytes.ptr() + at, &p_value, sizeof(T));
	}

public:
	void u8(uint8_t p_v) { put(p_v); }
	void u16(uint16_t p_v) { put(p_v); }
	void u32(uint32_t p_v) { put(p_v); }
	void u64(uint64_t p_v) { put(p_v); }
	void f32(float p_v) { put(p_v); }
	void f64(double p_v) { put(p_v); }
	void vec3d(const Vector3 &p_v) {
		f64(double(p_v.x));
		f64(double(p_v.y));
		f64(double(p_v.z));
	}
	void vec3f(const Vector3 &p_v) {
		f32(float(p_v.x));
		f32(float(p_v.y));
		f32(float(p_v.z));
	}
	void quatf(const Quaternion &p_q) {
		f32(float(p_q.x));
		f32(float(p_q.y));
		f32(float(p_q.z));
		f32(float(p_q.w));
	}
	void entity(const EntityState &p_e) {
		u64(p_e.id);
		u16(p_e.type);
		u64(p_e.frame);
		vec3d(p_e.position);
		quatf(p_e.rotation);
		vec3f(p_e.velocity);
	}

	const LocalVector<uint8_t> &data() const { return bytes; }
	void clear() { bytes.clear(); }
};

// Reads until something runs past the end, after which every read returns zero and ok() is false.
// Callers check ok() once at the end rather than after every field.
class Reader {
	const uint8_t *bytes = nullptr;
	uint32_t size = 0;
	uint32_t at = 0;
	bool good = true;

	template <typename T>
	T get() {
		T v{};
		if (!good || at + sizeof(T) > size) {
			good = false;
			return v;
		}
		std::memcpy(&v, bytes + at, sizeof(T));
		at += sizeof(T);
		return v;
	}

public:
	Reader(const uint8_t *p_bytes, uint32_t p_size) :
			bytes(p_bytes), size(p_size) {}

	uint8_t u8() { return get<uint8_t>(); }
	uint16_t u16() { return get<uint16_t>(); }
	uint32_t u32() { return get<uint32_t>(); }
	uint64_t u64() { return get<uint64_t>(); }
	float f32() { return get<float>(); }
	double f64() { return get<double>(); }
	Vector3 vec3d() {
		const double x = f64();
		const double y = f64();
		const double z = f64();
		return Vector3(x, y, z);
	}
	Vector3 vec3f() {
		const float x = f32();
		const float y = f32();
		const float z = f32();
		return Vector3(x, y, z);
	}
	Quaternion quatf() {
		const float x = f32();
		const float y = f32();
		const float z = f32();
		const float w = f32();
		return Quaternion(x, y, z, w);
	}
	EntityState entity() {
		EntityState e;
		e.id = u64();
		e.type = u16();
		e.frame = u64();
		e.position = vec3d();
		e.rotation = quatf();
		e.velocity = vec3f();
		return e;
	}

	bool ok() const { return good; }
};

} // namespace PlanetNet
