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

#include "core/math/math_funcs.h"
#include "core/math/planet_lattice.h"
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

constexpr uint16_t PROTOCOL_VERSION = 7;

enum MessageType : uint8_t {
	MSG_HELLO = 1, // client -> server: u16 version, u16 n, n bytes of player key (UTF-8; empty: anonymous)
	MSG_WELCOME, // server -> client: u16 version, u16 server id, u64 your entity, f64 region radius,
	//              u32 regions per face edge, f64 walk speed
	MSG_PING, // client -> server: f64 client clock (s)
	MSG_PONG, // server -> client: f64 client clock echoed, f64 server time (ticks)
	MSG_INPUT, // client -> server: u32 newest snapshot seq, u32 ack bits, u8 n,
	//            n x (u32 tick the input is for, f32 move x, f32 move y, f32 yaw)
	MSG_WORLD_REQUEST, // client -> server, world channel: f64 x3 point, u16 n, n bytes
	MSG_WORLD_CHANGE, // server -> client, world channel: u64 id, f64 x3 point, u16 n, n bytes
	MSG_SNAPSHOT, // server -> client: u32 seq, u32 tick, u32 your last applied input tick (0: none),
	//               [if nonzero: f64 x3 your position, f64 x3 velocity, f64 x3 walk reference],
	//               u16 n, n x record,
	//               u16 m, m x u16 removed handle
};

// Ids carry the server that created them in the top 16 bits, so they stay unique across servers.
constexpr int ENTITY_SERVER_SHIFT = 48;

// A delta may only refer to a state this many ticks old; older baselines are resent in full. The
// client keeps at least this much history per entity.
constexpr uint32_t MAX_BASELINE_AGE = 60;

// What one entity looks like to scripts.
struct EntityState {
	uint64_t id = 0;
	uint16_t type = 0;
	// The frame the position is in. 0 is the planet's body-fixed frame; ships will be frames later.
	uint64_t frame = 0;
	Vector3 position;
	Quaternion rotation;
	Vector3 velocity;
};

// ---------------------------------------------------------------- regions

// Regions are cells of a coarse cube-sphere grid: `resolution` cells along each face edge. A key is
// face (3 bits) | i (14 bits) | j (14 bits). Everything here goes through PlanetLattice, which is
// bit-identical on every machine, so a key and its centre mean the same thing to server and client.
struct Regions {
	double radius = 150000.0;
	uint32_t resolution = 1;

	static constexpr uint32_t MAX_RESOLUTION = (1u << 14) - 1;

	void configure(double p_radius, double p_region_size) {
		radius = p_radius;
		const double edge = radius * Math::PI * 0.5;
		const double cells = Math::round(edge / MAX(p_region_size, 1.0));
		resolution = uint32_t(CLAMP(cells, 1.0, double(MAX_RESOLUTION)));
	}

	uint32_t key_of(const Vector3 &p_position) const {
		int face = 0;
		double s = 0.0;
		double t = 0.0;
		PlanetLattice::direction_to_face(p_position.normalized(), face, s, t);
		const int i = CLAMP(int(Math::floor((s + 1.0) * 0.5 * resolution)), 0, int(resolution) - 1);
		const int j = CLAMP(int(Math::floor((t + 1.0) * 0.5 * resolution)), 0, int(resolution) - 1);
		return (uint32_t(face) << 28) | (uint32_t(i) << 14) | uint32_t(j);
	}

	Vector3 centre(uint32_t p_key) const {
		const int face = int(p_key >> 28);
		const uint32_t i = (p_key >> 14) & MAX_RESOLUTION;
		const uint32_t j = p_key & MAX_RESOLUTION;
		const double s = PlanetLattice::face_warp((2.0 * (double(i) + 0.5)) / double(resolution) - 1.0);
		const double t = PlanetLattice::face_warp((2.0 * (double(j) + 0.5)) / double(resolution) - 1.0);
		return PlanetLattice::cube_point(face, s, t).normalized() * radius;
	}

	// Approximate edge length of a region in metres, for sizing searches.
	double cell_size() const { return radius * Math::PI * 0.5 / double(resolution); }
};

// ---------------------------------------------------------------- compact encodings

// Rotation in 32 bits: drop the largest component (recoverable from unit length) and store the other
// three in 10 bits each over [-1/sqrt2, 1/sqrt2]. About 0.1 degrees.
inline uint32_t pack_rotation(Quaternion p_q) {
	p_q.normalize();
	const float c[4] = { float(p_q.x), float(p_q.y), float(p_q.z), float(p_q.w) };
	int largest = 0;
	for (int k = 1; k < 4; ++k) {
		if (Math::abs(c[k]) > Math::abs(c[largest])) {
			largest = k;
		}
	}
	const float sign = c[largest] < 0.f ? -1.f : 1.f;
	uint32_t packed = uint32_t(largest) << 30;
	int shift = 20;
	for (int k = 0; k < 4; ++k) {
		if (k == largest) {
			continue;
		}
		const float v = CLAMP(c[k] * sign * float(Math::SQRT2), -1.f, 1.f);
		packed |= uint32_t(int(Math::round(v * 511.f)) + 511) << shift;
		shift -= 10;
	}
	return packed;
}

inline Quaternion unpack_rotation(uint32_t p_packed) {
	const int largest = int(p_packed >> 30);
	float c[4];
	float sum = 0.f;
	int shift = 20;
	for (int k = 0; k < 4; ++k) {
		if (k == largest) {
			continue;
		}
		const float v = (float(int((p_packed >> shift) & 1023u) - 511) / 511.f) / float(Math::SQRT2);
		c[k] = v;
		sum += v * v;
		shift -= 10;
	}
	c[largest] = Math::sqrt(MAX(0.f, 1.f - sum));
	return Quaternion(c[0], c[1], c[2], c[3]);
}

// An entity as it goes on the wire: position as a region and a float offset from its centre, rotation
// packed, velocity in half floats. The server keeps the last acknowledged Wire per client as the
// baseline for deltas, and the client keeps the same Wire, so both sides compare identical integers.
struct Wire {
	uint16_t type = 0;
	uint64_t frame = 0;
	uint32_t region = 0;
	float offset[3] = { 0.f, 0.f, 0.f };
	uint32_t rotation = 0;
	uint16_t velocity[3] = { 0, 0, 0 };

	static Wire encode(const EntityState &p_state, const Regions &p_regions) {
		Wire w;
		w.type = p_state.type;
		w.frame = p_state.frame;
		w.region = p_regions.key_of(p_state.position);
		const Vector3 d = p_state.position - p_regions.centre(w.region);
		w.offset[0] = float(d.x);
		w.offset[1] = float(d.y);
		w.offset[2] = float(d.z);
		w.rotation = pack_rotation(p_state.rotation);
		w.velocity[0] = Math::make_half_float(float(p_state.velocity.x));
		w.velocity[1] = Math::make_half_float(float(p_state.velocity.y));
		w.velocity[2] = Math::make_half_float(float(p_state.velocity.z));
		return w;
	}

	EntityState decode(uint64_t p_id, const Regions &p_regions) const {
		EntityState s;
		s.id = p_id;
		s.type = type;
		s.frame = frame;
		s.position = p_regions.centre(region) + Vector3(offset[0], offset[1], offset[2]);
		s.rotation = unpack_rotation(rotation);
		s.velocity = Vector3(Math::half_to_float(velocity[0]), Math::half_to_float(velocity[1]), Math::half_to_float(velocity[2]));
		return s;
	}
};

// Record flags. A record without FULL is a delta against the state `age` ticks earlier.
//
// Records name entities by a per-client u16 handle; a FULL record also carries the 64-bit id, which
// binds the handle on the client. The server frees a handle only once the client has acknowledged the
// entity's removal, so a handle never means two things at once. Saves 6 bytes on every delta.
enum RecordFlags : uint8_t {
	RECORD_FULL = 1 << 0,
	RECORD_KIND = 1 << 1, // type and frame
	RECORD_POSITION = 1 << 2,
	RECORD_ROTATION = 1 << 3,
	RECORD_VELOCITY = 1 << 4,
};

// ---------------------------------------------------------------- byte streams

// FNV-1a over a byte range: the checksum that ends every saved world file.
inline uint64_t checksum(const uint8_t *p_bytes, uint32_t p_size) {
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = 0; i < p_size; ++i) {
		h = (h ^ p_bytes[i]) * 1099511628211ull;
	}
	return h;
}

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

	// A snapshot record: the fields of `p_now` that differ from `p_base` (all of them without one).
	// Returns false, writing nothing, when there is a baseline and nothing differs.
	bool record(uint16_t p_handle, uint64_t p_id, const Wire &p_now, const Wire *p_base, uint8_t p_age) {
		uint8_t flags = 0;
		if (p_base == nullptr) {
			flags = RECORD_FULL | RECORD_KIND | RECORD_POSITION | RECORD_ROTATION | RECORD_VELOCITY;
		} else {
			if (p_now.type != p_base->type || p_now.frame != p_base->frame) {
				flags |= RECORD_KIND;
			}
			if (p_now.region != p_base->region || std::memcmp(p_now.offset, p_base->offset, sizeof(p_now.offset)) != 0) {
				flags |= RECORD_POSITION;
			}
			if (p_now.rotation != p_base->rotation) {
				flags |= RECORD_ROTATION;
			}
			if (std::memcmp(p_now.velocity, p_base->velocity, sizeof(p_now.velocity)) != 0) {
				flags |= RECORD_VELOCITY;
			}
			if (flags == 0) {
				return false;
			}
		}
		u16(p_handle);
		u8(flags);
		if (flags & RECORD_FULL) {
			u64(p_id);
		} else {
			u8(p_age);
		}
		if (flags & RECORD_KIND) {
			u16(p_now.type);
			u64(p_now.frame);
		}
		if (flags & RECORD_POSITION) {
			u32(p_now.region);
			f32(p_now.offset[0]);
			f32(p_now.offset[1]);
			f32(p_now.offset[2]);
		}
		if (flags & RECORD_ROTATION) {
			u32(p_now.rotation);
		}
		if (flags & RECORD_VELOCITY) {
			u16(p_now.velocity[0]);
			u16(p_now.velocity[1]);
			u16(p_now.velocity[2]);
		}
		return true;
	}

	const LocalVector<uint8_t> &data() const { return bytes; }
	uint32_t size() const { return bytes.size(); }
	void truncate(uint32_t p_size) { bytes.resize(p_size); }
	void patch_u16(uint32_t p_at, uint16_t p_v) { std::memcpy(bytes.ptr() + p_at, &p_v, sizeof(p_v)); }
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

	// The fields a record carries, applied over `r_wire` (which holds the baseline, or anything for a
	// FULL record). Separate statements throughout: argument evaluation order is unspecified.
	void record_fields(uint8_t p_flags, Wire &r_wire) {
		if (p_flags & RECORD_KIND) {
			r_wire.type = u16();
			r_wire.frame = u64();
		}
		if (p_flags & RECORD_POSITION) {
			r_wire.region = u32();
			r_wire.offset[0] = f32();
			r_wire.offset[1] = f32();
			r_wire.offset[2] = f32();
		}
		if (p_flags & RECORD_ROTATION) {
			r_wire.rotation = u32();
		}
		if (p_flags & RECORD_VELOCITY) {
			r_wire.velocity[0] = u16();
			r_wire.velocity[1] = u16();
			r_wire.velocity[2] = u16();
		}
	}

	bool ok() const { return good; }
};

} // namespace PlanetNet
