/**************************************************************************/
/*  planet_landform.h                                                     */
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

#include "core/math/planet_hash_noise.h"
#include "core/math/vector3.h"

// Landforms: discrete features placed on a jittered 3D cell grid, rather than more noise.
//
// Noise is statistically the same everywhere, so turning it up makes all ground rougher without ever
// producing a mountain you would name. Landforms are the opposite: each cell of a coarse 3D grid may
// hold ONE feature -- a massif, a range, a volcano, a mesa -- with its own shape, size and height,
// decided by a hash of the cell. A second, finer grid scatters hills, buttes and knolls. This is what
// compresses verticality the way a hand-made map does: a landmark every few kilometres, whatever the
// climate, with the biome only scaling it.
//
// Features are TIERED, not smooth. Height is cut into steps whose risers vary in width around the
// feature, so some faces are sheer (the cliff skin covers them) and others are walkable ramps. On
// mountains the tier boundaries SPIRAL -- the step count is advanced by the angle around the peak, one
// whole step per turn -- so each bench is a ledge that winds up the mountain rather than a ring.
//
// The GLSL twin is landform_layer() in the demo's shaders/planet_field.gdshaderinc. Every constant in
// decode_site(), decode_tiers() and every step of eval_layer() must match it; the shader takes the gradient by finite
// differences, so only the height itself has to agree.

struct PlanetLandformLayer {
	double period = 8000.0; // Cell size, metres. Each cell holds at most one feature.
	double height = 0.0; // Tallest feature, metres. Zero disables the layer.
	double tier_step = 40.0; // Nominal step between tiers, metres.
	float density = 0.7f; // Fraction of cells that hold a feature.
	uint32_t seed = 5101u;
};

struct PlanetLandforms {
	PlanetLandformLayer major; // ranges, massifs, volcanoes, mesas
	PlanetLandformLayer minor; // hills, buttes, knolls
	double warp = 0.3; // Outline warp, as a fraction of each feature's radius.
	double riser = 0.12; // Narrowest riser, as a fraction of a step. Smaller is sheerer.

	bool enabled() const { return major.height > 0.0 || minor.height > 0.0; }
};

namespace PlanetLandform {

enum Type {
	MASSIF = 0,
	RANGE = 1,
	VOLCANO = 2,
	MESA = 3,
	HILL = 4,
	BUTTE = 5,
	KNOLL = 6,
};

// Feature parameters, all derived from one cell hash.
struct Feature {
	bool present = false;
	int type = HILL;
	float jitter[3] = { 0.5f, 0.5f, 0.5f }; // Feature point inside its cell, in cell units.
	float elongation = 1.f; // Stretch along the feature's axis.
	float radius = 0.3f; // In cell units, across the short axis.
	float height = 1.f; // Fraction of the layer height.
	float power = 1.5f; // Profile exponent, for the peaked types.
	float tier_scale = 1.f; // Multiplies the layer's tier step.
	float riser = 0.3f; // Mean riser fraction around the feature.
	float winding = 0.f; // -1, 0 or +1 steps per turn.
	float tier_strength = 1.f;
	float phase = 0.f;
	float tiers = 1.f; // How many tiers the feature's full height is cut into, at most.
	float phase2 = 0.f;
	float lobe_phase[3] = { 0.f, 0.f, 0.f }; // The outline's 2-, 3- and 5-lobe harmonics.
	float shift_phase = 0.f;
	float seg_phase = 0.f;
	float axis[3] = { 1.f, 0.f, 0.f };
};

// Steepest the smooth profile of a peaked feature may be, rise over run, before tiering.
static constexpr double MAX_FLANK = 0.55;

// A salted hash of the cell, and a uniform float from either half of one. Every value a feature
// needs is a 16-bit half of one of nine salted hashes. Integer arithmetic wraps identically in GLSL.
ZN_PLANET_NOISE_INLINE uint32_t salted(const uint32_t p_hash, const uint32_t p_salt) {
	return PlanetHashNoise::hash_u32(p_hash + p_salt * 0x9e3779b9u);
}

ZN_PLANET_NOISE_INLINE float hi16(const uint32_t p_x) {
	return float(p_x >> 16) * (1.f / 65536.f);
}

ZN_PLANET_NOISE_INLINE float lo16(const uint32_t p_x) {
	return float(p_x & 0xffffu) * (1.f / 65536.f);
}

// Stage one: whether the cell holds a feature, where, and its size -- everything needed to know if a
// sample is in its reach. Four hashes. Most cells a sample looks at stop here.
inline bool decode_site(const uint32_t h, const bool p_major, const float p_density, const float p_warp,
		const double p_height, const double p_period, Feature &f) {
	const uint32_t s1 = salted(h, 1u);
	f.present = hi16(s1) < p_density;
	if (!f.present) {
		return false;
	}
	const uint32_t s2 = salted(h, 2u);
	const uint32_t s3 = salted(h, 3u);
	const uint32_t s4 = salted(h, 4u);
	f.jitter[0] = 0.15f + 0.7f * lo16(s1);
	f.jitter[1] = 0.15f + 0.7f * hi16(s2);
	f.jitter[2] = 0.15f + 0.7f * lo16(s2);
	const float t = hi16(s3);
	const float r6 = lo16(s3);
	const float r7 = hi16(s4);
	const float r8 = lo16(s4);
	if (p_major) {
		if (t < 0.36f) {
			f.type = MASSIF;
			f.elongation = 1.f + 0.6f * r6;
			f.radius = 0.18f + 0.13f * r7;
			f.height = 0.65f + 0.35f * r8;
			f.power = 1.6f;
		} else if (t < 0.66f) {
			f.type = RANGE;
			f.elongation = 1.6f + 0.8f * r6;
			f.radius = 0.12f + 0.05f * r7;
			f.height = 0.6f + 0.4f * r8;
			f.power = 1.4f;
		} else if (t < 0.8f) {
			f.type = VOLCANO;
			f.elongation = 1.f + 0.25f * r6;
			f.radius = 0.18f + 0.1f * r7;
			f.height = 0.7f + 0.3f * r8;
			f.power = 1.3f;
			f.tier_strength = 0.8f;
		} else {
			f.type = MESA;
			f.elongation = 1.f + r6;
			f.radius = 0.12f + 0.1f * r7;
			f.height = 0.25f + 0.3f * r8;
		}
	} else {
		if (t < 0.5f) {
			f.type = HILL;
			f.elongation = 1.f + 0.8f * r6;
			f.radius = 0.25f + 0.3f * r7;
			f.height = 0.3f + 0.5f * r8;
			f.tier_strength = 0.f;
		} else if (t < 0.75f) {
			f.type = BUTTE;
			f.elongation = 1.f + 1.2f * r6;
			f.radius = 0.15f + 0.2f * r7;
			f.height = 0.5f + 0.5f * r8;
		} else {
			f.type = KNOLL;
			f.elongation = 1.f + 0.5f * r6;
			f.radius = 0.18f + 0.2f * r7;
			f.height = 0.6f + 0.4f * r8;
			f.power = 1.6f;
		}
	}
	// Everything a feature can reach -- its long axis plus the outline warp -- stays within HALF a cell
	// of its point. That is what lets a sample search only the 2x2x2 cells nearest it; a feature that
	// reached further would be cut off at the edge of that search and leave a crease.
	const float fit = 0.5f / (f.elongation * (1.f + p_warp));
	f.radius = f.radius < fit ? f.radius : fit;
	// The smooth flank never steeper than MAX_FLANK: the tiers make the cliffs, and a flank that is
	// already a wall turns the whole mountain into one cliff face. Widened first, lowered if it cannot
	// widen. Mesa and butte sides are meant to be walls and are left alone.
	if (f.type != MESA && f.type != BUTTE) {
		const double flank = f.type == HILL ? 1.54 : double(f.power);
		const double h_m = p_height * double(f.height);
		double r_m = double(f.radius) * p_period;
		const double need = h_m * flank / MAX_FLANK;
		if (need > r_m) {
			const double widest = double(fit) * p_period;
			r_m = need < widest ? need : widest;
			f.radius = float(r_m / p_period);
			if (h_m * flank / r_m > MAX_FLANK) {
				f.height = float(MAX_FLANK * r_m / (flank * p_height));
			}
		}
	}
	return true;
}

// Stage two: how the feature is tiered and outlined. Seven more hashes, only for features in reach.
inline void decode_tiers(const uint32_t h, const bool p_major, const float p_riser_min, Feature &f) {
	const uint32_t s5 = salted(h, 5u);
	const uint32_t s6 = salted(h, 6u);
	const uint32_t s7 = salted(h, 7u);
	const uint32_t s8 = salted(h, 8u);
	const uint32_t s9 = salted(h, 9u);
	const uint32_t s10 = salted(h, 10u);
	const uint32_t s11 = salted(h, 11u);
	f.tier_scale = 0.7f + 0.7f * hi16(s5);
	const float r10 = lo16(s5);
	f.riser = p_riser_min + (0.5f - p_riser_min) * r10 * r10;
	const bool winds = f.type == MASSIF || f.type == RANGE || f.type == VOLCANO || f.type == KNOLL;
	f.winding = winds ? (hi16(s6) < 0.5f ? 1.f : -1.f) : 0.f;
	// A mountain is cut into a few big bands, not dozens of thin ones; a butte or knoll into one or two.
	f.tiers = p_major ? 2.f + Math::floor(lo16(s6) * 3.f) : 1.f + Math::floor(lo16(s6) * 2.f);
	f.axis[0] = 2.f * hi16(s7) - 1.f;
	f.axis[1] = 2.f * lo16(s7) - 1.f;
	f.axis[2] = 2.f * hi16(s8) - 1.f;
	f.phase = lo16(s8) * 6.2831853f;
	f.phase2 = hi16(s9) * 6.2831853f;
	f.lobe_phase[0] = lo16(s9) * 6.2831853f;
	f.lobe_phase[1] = hi16(s10) * 6.2831853f;
	f.lobe_phase[2] = lo16(s10) * 6.2831853f;
	f.shift_phase = hi16(s11) * 6.2831853f;
	f.seg_phase = lo16(s11) * 6.2831853f;
}

// The tallest a feature's tier can be: the layer's step at its largest scale, or the whole feature cut
// into its fewest tiers.
inline double max_tier(const PlanetLandformLayer &L, const bool p_major) {
	const double by_step = L.tier_step * 1.4;
	const double by_count = L.height / (p_major ? 2.0 : 1.0);
	return by_step > by_count ? by_step : by_count;
}

// The feature's smooth profile in [0, 1] at normalised distance s in [0, 1).
inline double profile(const Feature &f, const double s) {
	switch (f.type) {
		case MESA:
		case BUTTE:
			return 1.0 - Math::smoothstep(0.7, 1.0, s);
		case HILL: {
			const double q = 1.0 - s * s;
			return q * q;
		}
		case VOLCANO:
			return Math::pow(1.0 - s, double(f.power)) - 0.22 * (1.0 - Math::smoothstep(0.0, 0.18, s));
		default:
			return Math::pow(1.0 - s, double(f.power));
	}
}

// Polynomial smooth maximum: overlapping mountains merge into one mass instead of stacking.
inline double smooth_max(const double a, const double b, const double k) {
	const double h = CLAMP(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
	return b * h + a * (1.0 - h) + k * h * (1.0 - h);
}

// The tiered height of one feature, from the sample's offset `d` to its feature point in metres.
//
// The outline is not warped by noise: three angular harmonics per feature push it out into lobes and
// pull it into bays, which costs a few sines rather than three Perlin samples, and the tier boundaries
// and segment breaks wobble on the same kind of harmonics. Every angle term has a whole number of
// cycles per turn, so the angle's wrap at +-pi never shows.
inline double feature_height(const Feature &f, const Vector3 &p_feature_dir, const Vector3 &d, const double p_cell,
		const PlanetLandformLayer &L, const double p_warp, const double p_riser_min) {
	const double R = double(f.radius) * p_cell;
	// Constant per feature, so the frame never switches across it.
	const Vector3 axis(f.axis[0], f.axis[1], f.axis[2]);
	Vector3 ut = axis - p_feature_dir * axis.dot(p_feature_dir);
	if (ut.length_squared() < 1e-4) {
		ut = p_feature_dir.cross(Vector3(0.0, 0.0, 1.0));
		if (ut.length_squared() < 1e-4) {
			ut = p_feature_dir.cross(Vector3(1.0, 0.0, 0.0));
		}
	}
	ut.normalize();
	const Vector3 vt = p_feature_dir.cross(ut);
	const double ae = d.dot(ut) / double(f.elongation);
	const double b = d.dot(vt);
	const double z = d.dot(p_feature_dir);
	// Outside the lobes' widest reach there is nothing to compute; skip the trigonometry.
	const double r = Math::sqrt(ae * ae + b * b + z * z);
	if (r >= R * (1.0 + p_warp)) {
		return 0.0;
	}
	const double theta = Math::atan2(b, ae);
	const double lobes = 1.0 + p_warp * (0.55 * Math::sin(2.0 * theta + double(f.lobe_phase[0]))
			+ 0.3 * Math::sin(3.0 * theta + double(f.lobe_phase[1])) + 0.15 * Math::sin(5.0 * theta + double(f.lobe_phase[2])));
	const double s = r / (R * lobes);
	if (s >= 1.0) {
		return 0.0;
	}
	const double y = L.height * double(f.height) * profile(f, s);
	// The tiers fade out at the rim, where the feature meets the ground, near its centre, and near its
	// axis, where the angle is undefined. Where they have faded out entirely, or the feature has none,
	// the smooth profile is the answer.
	const double rt = Math::sqrt(ae * ae + b * b);
	const double fade = double(f.tier_strength) * Math::smoothstep(0.0, 0.3, s) * (1.0 - Math::smoothstep(0.85, 1.0, s))
			* Math::smoothstep(0.0, 0.3 * R, rt);
	if (fade <= 0.0) {
		return y;
	}

	const double step_by_count = L.height * double(f.height) / double(f.tiers);
	const double step_by_layer = L.tier_step * double(f.tier_scale);
	const double T = step_by_count > step_by_layer ? step_by_count : step_by_layer;
	// The tier boundaries wobble rather than following the smooth contour, and the riser widens and
	// narrows around the feature, so a band is a sheer wall on one side and a ramp on another.
	const double turn = double(f.winding) * theta / Math::TAU + 0.25 * Math::sin(4.0 * theta + 6.0 * s + double(f.shift_phase));
	const double rho = CLAMP(double(f.riser) + 0.3 * Math::sin(3.0 * theta + double(f.phase)), p_riser_min, 0.95);
	const double q = y / T + turn;
	const double level = Math::floor(q);
	const double ph = q - level;
	const double tt = CLAMP((ph - (0.5 - 0.5 * rho)) / rho, 0.0, 1.0);
	const double eased = tt * tt * (3.0 - 2.0 * tt);
	const double tiered = T * (level + eased) - T * turn;
	// Segments: whole sectors of a band fade to a plain slope, so the bands break up instead of
	// ringing the mountain.
	const double seg = Math::smoothstep(0.0, 0.45, 0.6 * Math::sin(2.0 * theta + double(f.phase2))
			+ 0.4 * Math::sin(5.0 * theta + 3.0 * s + double(f.seg_phase)));
	const double k = fade * (0.1 + 0.9 * seg);
	return y + k * (tiered - y);
}

// Whether the unit cube at (x, y, z) straddles the sphere of radius `shell`, all in cell units.
inline bool cell_meets_sphere(const double x, const double y, const double z, const double shell) {
	const double nx = x > 0.0 ? x : (x + 1.0 < 0.0 ? x + 1.0 : 0.0);
	const double ny = y > 0.0 ? y : (y + 1.0 < 0.0 ? y + 1.0 : 0.0);
	const double nz = z > 0.0 ? z : (z + 1.0 < 0.0 ? z + 1.0 : 0.0);
	const double fx = Math::abs(x) > Math::abs(x + 1.0) ? x : x + 1.0;
	const double fy = Math::abs(y) > Math::abs(y + 1.0) ? y : y + 1.0;
	const double fz = Math::abs(z) > Math::abs(z + 1.0) ? z : z + 1.0;
	const double near2 = nx * nx + ny * ny + nz * nz;
	const double far2 = fx * fx + fy * fy + fz * fz;
	const double s2 = shell * shell;
	return near2 <= s2 && s2 <= far2;
}

// One layer's height at a unit direction.
inline double eval_layer(const PlanetLandforms &P, const PlanetLandformLayer &L, const bool p_major,
		const Vector3 &p_unit_dir, const double p_base_radius) {
	if (L.height <= 0.0) {
		return 0.0;
	}
	const Vector3 p = p_unit_dir * p_base_radius;
	const double inv = 1.0 / L.period;
	const double x = p.x * inv;
	const double y = p.y * inv;
	const double z = p.z * inv;
	const double ix = Math::floor(x);
	const double iy = Math::floor(y);
	const double iz = Math::floor(z);
	const double fx = x - ix;
	const double fy = y - iy;
	const double fz = z - iz;
	double acc = 0.0;
	// Only the 2x2x2 cells nearest the sample can hold a feature in reach (see decode_site), and of
	// those only the ones the sphere passes through can own a projected feature point.
	const int bx = fx < 0.5 ? -1 : 0;
	const int by = fy < 0.5 ? -1 : 0;
	const int bz = fz < 0.5 ? -1 : 0;
	const double shell = p_base_radius * inv;
	for (int oz = bz; oz <= bz + 1; ++oz) {
		for (int oy = by; oy <= by + 1; ++oy) {
			for (int ox = bx; ox <= bx + 1; ++ox) {
				const int32_t cx = int32_t(ix) + ox;
				const int32_t cy = int32_t(iy) + oy;
				const int32_t cz = int32_t(iz) + oz;
				if (!cell_meets_sphere(double(cx), double(cy), double(cz), shell)) {
					continue;
				}
				const uint32_t h = PlanetHashNoise::hash_coords(cx, cy, cz, L.seed);
				Feature f;
				if (!decode_site(h, p_major, L.density, float(P.warp), L.height, L.period, f)) {
					continue;
				}
				// The feature point is projected onto the sphere, so every landform stands at full height
				// instead of only the slice of it that a 3D cell's point happens to put through the surface.
				// A cell keeps its feature only if the projected point still lies inside it: each point then
				// has exactly one owner, whichever sample asks, so nothing pops in or out.
				const Vector3 feature_dir = Vector3(double(cx) + double(f.jitter[0]), double(cy) + double(f.jitter[1]),
						double(cz) + double(f.jitter[2]))
												   .normalized();
				const Vector3 on_sphere = feature_dir * (p_base_radius * inv) - Vector3(double(cx), double(cy), double(cz));
				if (on_sphere.x < 0.0 || on_sphere.x >= 1.0 || on_sphere.y < 0.0 || on_sphere.y >= 1.0 ||
						on_sphere.z < 0.0 || on_sphere.z >= 1.0) {
					continue;
				}
				const Vector3 d((fx - double(ox) - on_sphere.x) * L.period,
						(fy - double(oy) - on_sphere.y) * L.period,
						(fz - double(oz) - on_sphere.z) * L.period);
				const double reach = double(f.radius) * L.period * double(f.elongation) * (1.0 + P.warp);
				if (d.length_squared() >= reach * reach) {
					continue;
				}
				decode_tiers(h, p_major, float(P.riser), f);
				const double hf = feature_height(f, feature_dir, d, L.period, L, P.warp, P.riser);
				acc = p_major ? smooth_max(acc, hf, 0.08 * L.height) : acc + hf;
			}
		}
	}
	return acc;
}

// Upper bound on one layer's slope, metres per metre, at full strength. Conservative on purpose: an
// under-estimate is a missed collision. Checked against measurement by the planet field check.
inline double layer_lipschitz(const PlanetLandforms &P, const PlanetLandformLayer &L, const bool p_major) {
	if (L.height <= 0.0) {
		return 0.0;
	}
	const double r_min = 0.12 * L.period;
	const double t_max = max_tier(L, p_major);
	// |grad s| is 1/R, stretched by the lobes: they shrink the radius by up to (1 - warp) and turn at
	// most 2.75 warp per radian (0.55 * 2 + 0.3 * 3 + 0.15 * 5), with 1.5x margin on the radius ratio.
	const double lobe_min = 1.0 - P.warp;
	const double warp_stretch = (1.0 + 4.2 * P.warp / lobe_min) / lobe_min;
	const double profile_slope = 5.0; // the mesa's smoothstep over 0.3 of the radius; the others are less
	const double smooth = L.height * profile_slope / r_min * warp_stretch;
	// Across a riser the cubic multiplies the slope by up to 1.5 / riser. The angle's slope is 1/r, and
	// every angle term only acts where the axis fade has r >= ~0.1 R.
	const double riser_gain = 1.5 / P.riser;
	const double near_axis = 0.1 * r_min;
	const double spiral = t_max * (riser_gain + 1.0) / (Math::TAU * near_axis);
	const double wobble = t_max * 0.9 * riser_gain / near_axis;
	// The boundary wobble: 0.25 * sin(4 theta + 6 s).
	const double shift = t_max * (riser_gain + 1.0) * 0.25 * (4.0 / near_axis + 6.0 * warp_stretch / r_min);
	// The fade's own slope times the largest gap between tiered and smooth height (one step); the
	// segment term adds its angle and radius slopes.
	const double fade = t_max * (1.5 / 0.3 + 1.5 / 0.15 + 1.5 / 0.3) / r_min * warp_stretch
			+ t_max * 0.9 * (1.5 / 0.45) * (1.2 / near_axis + 0.4 * (5.0 / near_axis + 3.0 * warp_stretch / r_min));
	const double one = smooth * riser_gain + spiral + wobble + shift + fade;
	// The major layer takes a smooth maximum, which is 1-Lipschitz in its inputs. The minor layer sums,
	// and a point can sit inside at most a handful of the 27 candidates at once.
	return p_major ? one : one * 8.0;
}

} // namespace PlanetLandform
