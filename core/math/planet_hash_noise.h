#ifndef ZN_PLANET_HASH_NOISE_H
#define ZN_PLANET_HASH_NOISE_H

#include <cmath>
#include <cstdint>

// Integer-hash gradient noise, written to be evaluated identically on the CPU and in GLSL.
//
// Lives in core/ rather than in the voxel module because three separate consumers need the exact same
// `f`: the analytic planet collision shape (physics thread), the voxel generator, and the renderer.
// A copy per consumer is precisely the failure this file exists to prevent. Note this does mean the
// voxel module can no longer build as a standalone GDExtension without a shim; that trade is
// deliberate, since this fork builds it as an engine module.
//
// Why this exists rather than using FastNoiseLite or FastNoise2. Measured through the voxel graph in
// an optimized build (`--run_planet_field_bench`), 13 octaves:
//
//     Godot FastNoiseLite   13.25 ns/octave/sample   has a GLSL path, but a separate port
//     ZN_FastNoiseLite      12.97                    same
//     FastNoise2 (AVX512)    1.44                    no GLSL path at all
//     this                   5.93                    exact GLSL twin
//
// FastNoise2 is four times faster and would be the obvious pick on speed alone, but it has no
// shader_gen_func, so it cannot produce the GPU side of the field at all. It is also not
// deterministic across CPUs: FastSIMD specializes FMulAdd_f32 to a real FMA on AVX2/AVX512 and
// leaves it as (a*b)+c elsewhere, so its output depends on the machine it runs on.
//
// This implementation is within the design's 10 ns/octave budget and uses only uint32
// multiply/xor/shift plus float operations whose semantics are pinned in both C++ and GLSL.
//
// Conventions:
//  - `perlin_3d` returns a value in [-1, 1]. The bound is exact, not empirical: Perlin gradient noise
//    in N dimensions is bounded by sqrt(N)/2, so the 3D result is scaled by 2/sqrt(3).
//  - `fbm_3d` divides by the sum of octave amplitudes, so it is also in [-1, 1] for any octave count.
//    That gives range analysis a tight, provable interval instead of a conservative guess, which
//    matters because the min/max bound pyramid is what makes broadphase culling work.

// Floating-point contraction must be off, or this whole file's guarantee is void.
//
// Measured: with contraction on (clang's default), `-march=native` produced different results from
// `-msse2` / `-mavx2` for the same inputs, because AVX512 codegen fused a multiply-add into a single
// instruction with one rounding instead of two. That means terrain would differ between two players'
// machines depending on their CPU, quite apart from any CPU/GPU mismatch. With this pragma, -msse2,
// -msse4.2, -mavx2 and -march=native all produce bit-identical output.
//
// Note this is a file-scope pragma; it is restored to DEFAULT at the bottom of this header so it does
// not silently apply to whatever includes it.
#pragma STDC FP_CONTRACT OFF

// perlin_3d is large enough (eight hashed corners) that clang declines to inline it, which leaves a
// call in the sample loop and blocks vectorization entirely. Measured: forcing these inline is worth
// several times the runtime, so it is not a micro-optimization.
#if defined(__GNUC__) || defined(__clang__)
#define ZN_PLANET_NOISE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define ZN_PLANET_NOISE_INLINE __forceinline
#else
#define ZN_PLANET_NOISE_INLINE inline
#endif

namespace PlanetHashNoise {

// "lowbias32" finalizer. Chosen because it is short, has good avalanche behaviour, and uses only
// operations with identical semantics in GLSL.
ZN_PLANET_NOISE_INLINE uint32_t hash_u32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// Per-axis mixing constants. Coordinates are folded in as `coord * CONST` and summed, so the
// eight cell corners can share work: corner (xi+1) is just (xi * CONST) + CONST. Unsigned
// arithmetic is modular in both C++ and GLSL, so that identity is exact, not an approximation.
constexpr uint32_t HASH_CONST_X = 0x27d4eb2du;
constexpr uint32_t HASH_CONST_Y = 0x85ebca6bu;
constexpr uint32_t HASH_CONST_Z = 0xc2b2ae35u;

// Note the int32 -> uint32 conversions are two's-complement reinterpretations in both C++ and GLSL,
// so negative coordinates hash the same on both sides.
ZN_PLANET_NOISE_INLINE uint32_t hash_coords(const int32_t xi, const int32_t yi, const int32_t zi, const uint32_t seed) {
	uint32_t h = seed;
	h += uint32_t(xi) * HASH_CONST_X;
	h += uint32_t(yi) * HASH_CONST_Y;
	h += uint32_t(zi) * HASH_CONST_Z;
	return hash_u32(h);
}

// Branchless floor. `std::floor` does not lower to an instruction on every target we build for
// (measured: on llvm-mingw it stayed a libm call, which blocked vectorization of the sample loop
// outright and cost roughly 2x). Valid for |x| < 2^31, which the field never approaches: the
// highest-frequency octave of a 13-octave stack at planet scale peaks around 5.3e6.
ZN_PLANET_NOISE_INLINE float floor_fast(const float x) {
	const float t = float(int32_t(x));
	return t - float(t > x);
}

// Perlin's improved-noise gradient selection: picks one of the 12 cube-edge directions using the low
// bits of the hash. Written with selects rather than a table or a switch so it stays branchless and
// the sample loop can vectorize.
ZN_PLANET_NOISE_INLINE float grad_3d(const uint32_t hash, const float x, const float y, const float z) {
	const uint32_t h = hash & 15u;
	const float u = (h < 8u) ? x : y;
	// Bitwise `|` rather than `||`: no short-circuit means no branch to defeat vectorization.
	const float v = (h < 4u) ? y : (((h == 12u) | (h == 14u)) ? x : z);
	return ((h & 1u) == 0u ? u : -u) + ((h & 2u) == 0u ? v : -v);
}

// Quintic fade. C2-continuous, which matters because a C1 fade produces visible creases once the
// surface is used for collision normals.
ZN_PLANET_NOISE_INLINE float fade(const float t) {
	return t * t * t * (t * (t * 6.f - 15.f) + 10.f);
}

ZN_PLANET_NOISE_INLINE float lerp_f(const float a, const float b, const float t) {
	return a + t * (b - a);
}

// 2 / sqrt(3). Perlin 3D noise is bounded by sqrt(3)/2, so this normalizes the range to exactly
// [-1, 1].
constexpr float PERLIN_3D_NORMALIZATION = 1.1547005383792515f;

ZN_PLANET_NOISE_INLINE float perlin_3d(const float x, const float y, const float z, const uint32_t seed) {
	const float fx = floor_fast(x);
	const float fy = floor_fast(y);
	const float fz = floor_fast(z);

	const int32_t xi = int32_t(fx);
	const int32_t yi = int32_t(fy);
	const int32_t zi = int32_t(fz);

	const float dx = x - fx;
	const float dy = y - fy;
	const float dz = z - fz;

	const float u = fade(dx);
	const float v = fade(dy);
	const float w = fade(dz);

	const float dx1 = dx - 1.f;
	const float dy1 = dy - 1.f;
	const float dz1 = dz - 1.f;

	// Three multiplies for the whole cell instead of three per corner. Exactly equal to calling
	// hash_coords() eight times, because unsigned multiply distributes over addition mod 2^32.
	const uint32_t hx0 = uint32_t(xi) * HASH_CONST_X + seed;
	const uint32_t hy0 = uint32_t(yi) * HASH_CONST_Y;
	const uint32_t hz0 = uint32_t(zi) * HASH_CONST_Z;
	const uint32_t hx1 = hx0 + HASH_CONST_X;
	const uint32_t hy1 = hy0 + HASH_CONST_Y;
	const uint32_t hz1 = hz0 + HASH_CONST_Z;

	const float n000 = grad_3d(hash_u32(hx0 + hy0 + hz0), dx, dy, dz);
	const float n100 = grad_3d(hash_u32(hx1 + hy0 + hz0), dx1, dy, dz);
	const float n010 = grad_3d(hash_u32(hx0 + hy1 + hz0), dx, dy1, dz);
	const float n110 = grad_3d(hash_u32(hx1 + hy1 + hz0), dx1, dy1, dz);
	const float n001 = grad_3d(hash_u32(hx0 + hy0 + hz1), dx, dy, dz1);
	const float n101 = grad_3d(hash_u32(hx1 + hy0 + hz1), dx1, dy, dz1);
	const float n011 = grad_3d(hash_u32(hx0 + hy1 + hz1), dx, dy1, dz1);
	const float n111 = grad_3d(hash_u32(hx1 + hy1 + hz1), dx1, dy1, dz1);

	const float x00 = lerp_f(n000, n100, u);
	const float x10 = lerp_f(n010, n110, u);
	const float x01 = lerp_f(n001, n101, u);
	const float x11 = lerp_f(n011, n111, u);

	const float y0 = lerp_f(x00, x10, v);
	const float y1 = lerp_f(x01, x11, v);

	return lerp_f(y0, y1, w) * PERLIN_3D_NORMALIZATION;
}

// Double-input Perlin. Identical hashing, gradients and interpolation to perlin_3d; the only change
// is that the lattice cell is found by flooring in DOUBLE, so the cell index is exact and the
// in-cell offset keeps full float precision no matter how large the coordinate is.
//
// This exists because float32 cannot carry a planet. At 3,185,500 m a float32 ULP is 0.25 m, so
// sampling the noise at `dir * base_radius` in float32 snaps every sample onto a 0.25 m grid: the
// surface comes out as visible terraces with multi-metre plateaus rather than terrain. Measured
// before this change: 40 consecutive lattice samples produced 4 distinct heights, all exact multiples
// of 0.25 m, with flat runs 3.75 m long.
//
// Only the floor is done in double. Everything downstream is the same float32 arithmetic as the
// single-precision path, so the two agree exactly whenever the float32 path has not already lost the
// coordinate.
ZN_PLANET_NOISE_INLINE float perlin_3d_precise(const double x, const double y, const double z, const uint32_t seed) {
	const double fx = std::floor(x);
	const double fy = std::floor(y);
	const double fz = std::floor(z);

	const int32_t xi = int32_t(fx);
	const int32_t yi = int32_t(fy);
	const int32_t zi = int32_t(fz);

	const float dx = float(x - fx);
	const float dy = float(y - fy);
	const float dz = float(z - fz);

	const float u = fade(dx);
	const float v = fade(dy);
	const float w = fade(dz);

	const float dx1 = dx - 1.f;
	const float dy1 = dy - 1.f;
	const float dz1 = dz - 1.f;

	const uint32_t hx0 = uint32_t(xi) * HASH_CONST_X + seed;
	const uint32_t hy0 = uint32_t(yi) * HASH_CONST_Y;
	const uint32_t hz0 = uint32_t(zi) * HASH_CONST_Z;
	const uint32_t hx1 = hx0 + HASH_CONST_X;
	const uint32_t hy1 = hy0 + HASH_CONST_Y;
	const uint32_t hz1 = hz0 + HASH_CONST_Z;

	const float n000 = grad_3d(hash_u32(hx0 + hy0 + hz0), dx, dy, dz);
	const float n100 = grad_3d(hash_u32(hx1 + hy0 + hz0), dx1, dy, dz);
	const float n010 = grad_3d(hash_u32(hx0 + hy1 + hz0), dx, dy1, dz);
	const float n110 = grad_3d(hash_u32(hx1 + hy1 + hz0), dx1, dy1, dz);
	const float n001 = grad_3d(hash_u32(hx0 + hy0 + hz1), dx, dy, dz1);
	const float n101 = grad_3d(hash_u32(hx1 + hy0 + hz1), dx1, dy, dz1);
	const float n011 = grad_3d(hash_u32(hx0 + hy1 + hz1), dx, dy1, dz1);
	const float n111 = grad_3d(hash_u32(hx1 + hy1 + hz1), dx1, dy1, dz1);

	const float x00 = lerp_f(n000, n100, u);
	const float x10 = lerp_f(n010, n110, u);
	const float x01 = lerp_f(n001, n101, u);
	const float x11 = lerp_f(n011, n111, u);

	const float y0 = lerp_f(x00, x10, v);
	const float y1 = lerp_f(x01, x11, v);

	return lerp_f(y0, y1, w) * PERLIN_3D_NORMALIZATION;
}

struct FbmParams {
	uint32_t seed = 0;
	// Wavelength of the first octave, in world units.
	float period = 1.f;
	int octaves = 1;
	float lacunarity = 2.f;
	float gain = 0.5f;
};

// Sum of the octave amplitudes. Used to normalize, and by range analysis.
inline float fbm_amplitude_sum(const int octaves, const float gain) {
	float amp = 1.f;
	float sum = 0.f;
	for (int i = 0; i < octaves; ++i) {
		sum += amp;
		amp *= gain;
	}
	return sum;
}

inline float fbm_3d(const float x, const float y, const float z, const FbmParams &p) {
	float freq = 1.f / p.period;
	float amp = 1.f;
	float sum = 0.f;
	float norm = 0.f;
	for (int i = 0; i < p.octaves; ++i) {
		sum += amp * perlin_3d(x * freq, y * freq, z * freq, p.seed + uint32_t(i));
		norm += amp;
		freq *= p.lacunarity;
		amp *= p.gain;
	}
	// Multiply by the reciprocal rather than dividing, matching fbm_3d_series exactly. `sum / norm`
	// rounds once and `sum * (1 / norm)` rounds twice, so the two forms disagree in the last bit for
	// roughly a fifth of inputs. That was a real defect caught by the Phase 2 check: the scalar and
	// batched paths are both the definition of `f`, and a field with two definitions produces a step
	// at every chunk boundary.
	if (norm <= 0.f) {
		return 0.f;
	}
	const float inv_norm = 1.f / norm;
	return sum * inv_norm;
}

inline float fbm_3d_precise(const double x, const double y, const double z, const FbmParams &p) {
	double freq = 1.0 / double(p.period);
	float amp = 1.f;
	float sum = 0.f;
	float norm = 0.f;
	for (int i = 0; i < p.octaves; ++i) {
		sum += amp * perlin_3d_precise(x * freq, y * freq, z * freq, p.seed + uint32_t(i));
		norm += amp;
		freq *= double(p.lacunarity);
		amp *= p.gain;
	}
	if (norm <= 0.f) {
		return 0.f;
	}
	const float inv_norm = 1.f / norm;
	return sum * inv_norm;
}

// Batched double-input form. Same octave-outer / sample-inner structure as the float version so the
// inner loop still vectorizes; only the per-axis floor runs in double.
inline void fbm_3d_series_precise(
		const double *__restrict px,
		const double *__restrict py,
		const double *__restrict pz,
		float *__restrict po,
		const unsigned int count,
		const FbmParams &p) {
	for (unsigned int i = 0; i < count; ++i) {
		po[i] = 0.f;
	}

	double freq = 1.0 / double(p.period);
	float amp = 1.f;
	float norm = 0.f;

	for (int oct = 0; oct < p.octaves; ++oct) {
		const uint32_t seed = p.seed + uint32_t(oct);
		for (unsigned int i = 0; i < count; ++i) {
			po[i] += amp * perlin_3d_precise(px[i] * freq, py[i] * freq, pz[i] * freq, seed);
		}
		norm += amp;
		freq *= double(p.lacunarity);
		amp *= p.gain;
	}

	if (norm > 0.f) {
		const float inv_norm = 1.f / norm;
		for (unsigned int i = 0; i < count; ++i) {
			po[i] *= inv_norm;
		}
	}
}

// Batched evaluation. The octave loop is outermost and the sample loop innermost so the inner loop is
// branchless, uniform, and vectorizable across samples. Buffers are small enough to stay in cache
// across the repeated passes.
// Raw restrict-qualified pointers rather than any span type: a bounds check per element per octave
// blocks vectorization outright (measured ~2.8x), and restrict tells the compiler the output does not
// alias the inputs.
inline void fbm_3d_series(
		const float *__restrict px,
		const float *__restrict py,
		const float *__restrict pz,
		float *__restrict po,
		const unsigned int count,
		const FbmParams &p
) {

	for (unsigned int i = 0; i < count; ++i) {
		po[i] = 0.f;
	}

	float freq = 1.f / p.period;
	float amp = 1.f;
	float norm = 0.f;

	for (int oct = 0; oct < p.octaves; ++oct) {
		const uint32_t seed = p.seed + uint32_t(oct);
		for (unsigned int i = 0; i < count; ++i) {
			po[i] += amp * perlin_3d(px[i] * freq, py[i] * freq, pz[i] * freq, seed);
		}
		norm += amp;
		freq *= p.lacunarity;
		amp *= p.gain;
	}

	if (norm > 0.f) {
		const float inv_norm = 1.f / norm;
		for (unsigned int i = 0; i < count; ++i) {
			po[i] *= inv_norm;
		}
	}
}


// ---------------------------------------------------------------- shaping and derivatives

// d/dt of the quintic fade. Needed by the analytic Perlin derivative below.
ZN_PLANET_NOISE_INLINE float fade_deriv(const float t) {
	return 30.f * t * t * (t * (t - 2.f) + 1.f);
}

// grad_3d, but also reporting WHICH gradient vector was selected. Perlin's improved-noise set puts
// +-1 on exactly two of the three axes, so the vector is recovered by asking which axis each of the
// two terms belongs to. The returned dot product is bit-identical to grad_3d(): `su * u` with su
// exactly +-1.0f is the same value as the original's conditional negation.
ZN_PLANET_NOISE_INLINE float grad_3d_vec(const uint32_t hash, const float x, const float y, const float z,
		float &r_gx, float &r_gy, float &r_gz) {
	const uint32_t h = hash & 15u;
	const bool h_lt8 = h < 8u;
	const bool h_lt4 = h < 4u;
	const bool h_xz = ((h == 12u) | (h == 14u));

	const float u = h_lt8 ? x : y;
	const float v = h_lt4 ? y : (h_xz ? x : z);
	const float su = ((h & 1u) == 0u) ? 1.f : -1.f;
	const float sv = ((h & 2u) == 0u) ? 1.f : -1.f;

	r_gx = 0.f;
	r_gy = 0.f;
	r_gz = 0.f;
	if (h_lt8) {
		r_gx += su;
	} else {
		r_gy += su;
	}
	if (h_lt4) {
		r_gy += sv;
	} else if (h_xz) {
		r_gx += sv;
	} else {
		r_gz += sv;
	}
	return su * u + sv * v;
}

// Perlin with its analytic gradient. The value is computed by the identical sequence of operations
// as perlin_3d_precise() and is bit-identical to it; the derivative is extra work layered on top.
//
// d/dx has two parts: the corner gradients interpolated by the same trilinear weights, plus the
// change carried by the fade weights themselves. Dropping the second term is a common shortcut and
// it is wrong -- it gives a gradient that does not integrate back to the value, which shows up as
// slope weighting fighting the surface it is supposed to follow.
ZN_PLANET_NOISE_INLINE float perlin_3d_precise_deriv(const double x, const double y, const double z,
		const uint32_t seed, float &r_dx, float &r_dy, float &r_dz) {
	const double fx = std::floor(x);
	const double fy = std::floor(y);
	const double fz = std::floor(z);

	const int32_t xi = int32_t(fx);
	const int32_t yi = int32_t(fy);
	const int32_t zi = int32_t(fz);

	const float dx = float(x - fx);
	const float dy = float(y - fy);
	const float dz = float(z - fz);

	const float u = fade(dx);
	const float v = fade(dy);
	const float w = fade(dz);

	const float dx1 = dx - 1.f;
	const float dy1 = dy - 1.f;
	const float dz1 = dz - 1.f;

	const uint32_t hx0 = uint32_t(xi) * HASH_CONST_X + seed;
	const uint32_t hy0 = uint32_t(yi) * HASH_CONST_Y;
	const uint32_t hz0 = uint32_t(zi) * HASH_CONST_Z;
	const uint32_t hx1 = hx0 + HASH_CONST_X;
	const uint32_t hy1 = hy0 + HASH_CONST_Y;
	const uint32_t hz1 = hz0 + HASH_CONST_Z;

	float gx[8];
	float gy[8];
	float gz[8];
	const float n000 = grad_3d_vec(hash_u32(hx0 + hy0 + hz0), dx, dy, dz, gx[0], gy[0], gz[0]);
	const float n100 = grad_3d_vec(hash_u32(hx1 + hy0 + hz0), dx1, dy, dz, gx[1], gy[1], gz[1]);
	const float n010 = grad_3d_vec(hash_u32(hx0 + hy1 + hz0), dx, dy1, dz, gx[2], gy[2], gz[2]);
	const float n110 = grad_3d_vec(hash_u32(hx1 + hy1 + hz0), dx1, dy1, dz, gx[3], gy[3], gz[3]);
	const float n001 = grad_3d_vec(hash_u32(hx0 + hy0 + hz1), dx, dy, dz1, gx[4], gy[4], gz[4]);
	const float n101 = grad_3d_vec(hash_u32(hx1 + hy0 + hz1), dx1, dy, dz1, gx[5], gy[5], gz[5]);
	const float n011 = grad_3d_vec(hash_u32(hx0 + hy1 + hz1), dx, dy1, dz1, gx[6], gy[6], gz[6]);
	const float n111 = grad_3d_vec(hash_u32(hx1 + hy1 + hz1), dx1, dy1, dz1, gx[7], gy[7], gz[7]);

	const float x00 = lerp_f(n000, n100, u);
	const float x10 = lerp_f(n010, n110, u);
	const float x01 = lerp_f(n001, n101, u);
	const float x11 = lerp_f(n011, n111, u);

	const float y0 = lerp_f(x00, x10, v);
	const float y1 = lerp_f(x01, x11, v);

	// Trilinear interpolation of the corner gradients.
	const float igx = lerp_f(lerp_f(lerp_f(gx[0], gx[1], u), lerp_f(gx[2], gx[3], u), v),
			lerp_f(lerp_f(gx[4], gx[5], u), lerp_f(gx[6], gx[7], u), v), w);
	const float igy = lerp_f(lerp_f(lerp_f(gy[0], gy[1], u), lerp_f(gy[2], gy[3], u), v),
			lerp_f(lerp_f(gy[4], gy[5], u), lerp_f(gy[6], gy[7], u), v), w);
	const float igz = lerp_f(lerp_f(lerp_f(gz[0], gz[1], u), lerp_f(gz[2], gz[3], u), v),
			lerp_f(lerp_f(gz[4], gz[5], u), lerp_f(gz[6], gz[7], u), v), w);

	// Change carried by the fade weights.
	const float du = lerp_f(lerp_f(n100 - n000, n110 - n010, v), lerp_f(n101 - n001, n111 - n011, v), w);
	const float dv = lerp_f(lerp_f(n010 - n000, n110 - n100, u), lerp_f(n011 - n001, n111 - n101, u), w);
	const float dw = lerp_f(lerp_f(n001 - n000, n101 - n100, u), lerp_f(n011 - n010, n111 - n110, u), v);

	r_dx = (igx + du * fade_deriv(dx)) * PERLIN_3D_NORMALIZATION;
	r_dy = (igy + dv * fade_deriv(dy)) * PERLIN_3D_NORMALIZATION;
	r_dz = (igz + dw * fade_deriv(dz)) * PERLIN_3D_NORMALIZATION;

	return lerp_f(y0, y1, w) * PERLIN_3D_NORMALIZATION;
}

// How each octave's raw signal is reshaped. Three independent weights summing to 1, NOT a single
// morph parameter sliding 0 -> 1 -> 2. A 1-D morph from standard to ridged has to pass through
// billow, so every plains-to-mountains transition would grow a band of billow terrain nobody asked
// for.
struct ShapeMix {
	float standard = 1.f;
	float billow = 0.f;
	float ridged = 0.f;

	// Exact output range of the shaped signal. shape_signal() is piecewise linear in n over [-1, 0]
	// and [0, 1], so its extremes can only occur at n = -1, 0 or 1.
	void bounds(float &r_min, float &r_max) const {
		const float at_neg = -standard + billow;
		const float at_zero = ridged;
		const float at_pos = standard + billow;
		r_min = at_neg < at_zero ? at_neg : at_zero;
		r_min = r_min < at_pos ? r_min : at_pos;
		r_max = at_neg > at_zero ? at_neg : at_zero;
		r_max = r_max > at_pos ? r_max : at_pos;
	}
};

// Low-frequency displacement of the sample position, to break up the lattice-aligned look of raw
// fBm. Costs three extra noise evaluations per sample, and it raises the effective gradient of the
// whole field -- see PlanetFieldData::get_lipschitz_per_metre, which must account for it.
struct WarpParams {
	float amplitude = 0.f; // World units of displacement. Zero disables the warp entirely.
	float period = 1.f;
	uint32_t seed = 0x9e3779b9u;
};

ZN_PLANET_NOISE_INLINE float shape_signal(const float n, const ShapeMix &sh, float &r_dn) {
	const float an = (n < 0.f) ? -n : n;
	r_dn = sh.standard + ((n < 0.f) ? -1.f : 1.f) * (sh.billow - sh.ridged);
	return sh.standard * n + sh.billow * an + sh.ridged * (1.f - an);
}

ZN_PLANET_NOISE_INLINE void apply_warp(double &x, double &y, double &z, const WarpParams &wp) {
	if (wp.amplitude == 0.f) {
		return;
	}
	const double wf = 1.0 / double(wp.period);
	const double sx = x * wf;
	const double sy = y * wf;
	const double sz = z * wf;
	const float ox = perlin_3d_precise(sx, sy, sz, wp.seed);
	const float oy = perlin_3d_precise(sx, sy, sz, wp.seed + 1u);
	const float oz = perlin_3d_precise(sx, sy, sz, wp.seed + 2u);
	x += double(wp.amplitude) * double(ox);
	y += double(wp.amplitude) * double(oy);
	z += double(wp.amplitude) * double(oz);
}

// fBm with shaping and slope-based octave weighting.
//
// `p_slope_scale` converts the accumulated noise gradient into a real surface slope (height per
// metre), which is what makes `p_slope_erosion` a dimensionless knob rather than one needing retuning
// whenever amplitude changes. Weighting each octave by 1/(1 + k*|slope|^2) keeps flat ground flat and
// concentrates detail on slopes -- the cheap stand-in for erosion, and the biggest single step from
// "bumpy" towards "eroded".
//
// With ShapeMix{1,0,0}, zero warp and zero erosion this is bit-identical to fbm_3d_precise(): the
// shaping collapses to `n + 0*|n| + 0*(1-|n|)`, the weight is exactly 1.0f, and the accumulation and
// normalization happen in the same order and precision.
inline float fbm_3d_shaped_precise(double x, double y, double z, const FbmParams &p,
		const ShapeMix &sh, const WarpParams &wp, const float p_slope_erosion, const float p_slope_scale) {
	apply_warp(x, y, z, wp);

	double freq = 1.0 / double(p.period);
	float amp = 1.f;
	float sum = 0.f;
	float norm = 0.f;
	float gx = 0.f;
	float gy = 0.f;
	float gz = 0.f;

	for (int i = 0; i < p.octaves; ++i) {
		float dx = 0.f;
		float dy = 0.f;
		float dz = 0.f;
		const float n = perlin_3d_precise_deriv(x * freq, y * freq, z * freq, p.seed + uint32_t(i), dx, dy, dz);

		float dshape = 0.f;
		const float shaped = shape_signal(n, sh, dshape);

		// Weight from the gradient of the octaves ALREADY accumulated, never including this one. That
		// ordering is the whole trick: octave 0 keeps weight exactly 1, and each octave is damped only
		// by structure coarser than itself. Folding the current octave in instead makes the weight vary
		// at the FINEST scale while multiplying the LARGEST amplitude term, so the weight becomes the
		// high-frequency signal -- measured at 150x the Lipschitz bound.
		float weight = 1.f;
		if (p_slope_erosion > 0.f) {
			weight = 1.f / (1.f + p_slope_erosion * (gx * gx + gy * gy + gz * gz));
		}

		sum += amp * (weight * shaped);
		// Normalized by the UNWEIGHTED amplitude sum on purpose. Dividing by sum(amp*weight) makes the
		// denominator vary from point to point, and the gradient of that ratio can far exceed the
		// gradient of either part -- measured at 7x the Lipschitz bound, which would have meant missed
		// collisions. With a fixed denominator the weights can only ever attenuate.
		norm += amp;

		if (p_slope_erosion > 0.f) {
			// Deliberately NOT scaled by dshape. shape_signal's slope jumps at n == 0 whenever billow
			// and ridged differ, because |n| is not differentiable there. Feeding that into the weight
			// makes the weight itself discontinuous, and a discontinuous weight on an O(1) octave is a
			// step in the surface -- it measured 18000 m of slope per metre, 75x over the bound, and
			// only when shaping and erosion were enabled together. The raw noise gradient is
			// continuous (quintic fade makes Perlin C2) and is an upper bound on the shaped one, since
			// |dshape| <= 1.
			const float fr = float(freq) * p_slope_scale * amp;
			gx += dx * fr;
			gy += dy * fr;
			gz += dz * fr;
		}

		freq *= double(p.lacunarity);
		amp *= p.gain;
	}

	if (norm <= 0.f) {
		return 0.f;
	}
	const float inv_norm = 1.f / norm;
	return sum * inv_norm;
}

// Batched form. Deliberately point-outer rather than octave-outer: the slope weighting carries a
// per-point gradient accumulator across octaves, and this ordering makes each point follow exactly
// the same arithmetic path as fbm_3d_shaped_precise(), so the scalar and batched definitions of `f`
// agree bit for bit rather than approximately. Both are the definition of the surface -- the renderer
// uses one and the collider uses the other.
inline void fbm_3d_shaped_series_precise(
		const double *__restrict px,
		const double *__restrict py,
		const double *__restrict pz,
		float *__restrict po,
		const unsigned int count,
		const FbmParams &p,
		const ShapeMix &sh,
		const WarpParams &wp,
		const float p_slope_erosion,
		const float p_slope_scale) {
	for (unsigned int i = 0; i < count; ++i) {
		po[i] = fbm_3d_shaped_precise(px[i], py[i], pz[i], p, sh, wp, p_slope_erosion, p_slope_scale);
	}
}

} // namespace PlanetHashNoise

// Restore the default so including this header does not change float codegen for the rest of the
// translation unit.
#pragma STDC FP_CONTRACT DEFAULT

#endif // ZN_PLANET_HASH_NOISE_H
