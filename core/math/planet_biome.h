/**************************************************************************/
/*  planet_biome.h                                                        */
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

#include <memory>
#include <vector>

// Climate maps and biome blending for planetary terrain.
//
// Two tiers: a low-frequency MACRO layer that says what the climate is at a direction, and a biome
// table that turns climate into the parameters the high-frequency noise runs with. The macro layer is
// a table rather than live noise because continents vary over thousands of kilometres and evaluating
// several octaves of fBm per sample to learn that is most of a field evaluation for information that
// barely changes.
//
// The whole thing stays a pure function of direction: a piecewise-bilinear table indexed by `dir` is
// still f(dir), which is what lets the collider generate triangles on demand.

// One sample of the macro layer.
struct PlanetMacroSample {
	float height = 0.f; // Continental baseline, [0, 1].
	float temperature = 0.f; // [0, 1], 0 cold.
	float moisture = 0.f; // [0, 1], 0 dry.
	float weirdness = 0.f; // [0, 1]. Reserved: stored and interpolated, not yet used for selection.
};

// Parameters used to bake a macro map procedurally. Authored maps ignore these entirely.
struct PlanetMacroBakeParams {
	uint32_t seed = 1337u;
	// Wavelength of the largest continental feature, in metres on the surface.
	float continent_period = 6000000.f;
	int continent_octaves = 4;
	// Temperature is built in three parts, mixed and then scaled.
	//
	//   polar map   1 - |y|^polar_falloff        warm at the equator, cold at the poles
	//   noise map   an independent fBm field     regional variation with no north-south order
	//   blend       polar_weight between them
	//   then        minus the lapse rate times elevation, normalized, times temperature_scale
	//
	// Splitting them matters because they do different jobs: the weight decides how ORDERLY climate
	// is, the falloff decides how LARGE the cold caps are, and the scale shifts the whole planet warm
	// or cold. Trying to get all three out of one weight is why the ice caps could not be shrunk.

	// How strongly the polar map drives temperature versus the noise map. 1 is pure latitude banding.
	float polar_weight = 0.6f;

	// Shape of the pole-to-equator falloff, as an exponent on |y|.
	//
	// At 1.0 the falloff is linear in sin(latitude), which spreads cold evenly and puts about a fifth
	// of the planet below 0.2 temperature -- roughly twice Earth's polar fraction. Above 1 the warm
	// band holds further towards the poles and then drops away sharply, which is both what Earth does
	// and the only control here that actually changes how BIG the ice caps are.
	float polar_falloff = 1.7f;

	// Multiplies the normalized temperature. Below 1 cools the whole planet, above 1 warms it.
	// Applied after normalization, because normalization would otherwise undo it exactly.
	float temperature_scale = 1.0f;

	// How strongly temperature drives moisture. Warm air holds far more water vapour -- about 7% more
	// per degree -- so leaving the two channels independent gives frozen rainforests and is the single
	// least physical thing about the climate model. 0 keeps them independent, 1 scales moisture by
	// temperature outright.
	//
	// Applied after both channels are normalized, then moisture is renormalized: that keeps the full
	// moisture range in use, so the coupling changes WHERE the wet places are without shrinking how
	// wet they get.
	float moisture_temperature_weight = 0.35f;
	// Temperature lost per unit of normalized elevation, the atmospheric lapse rate. Mountains are
	// cold even at the equator, which is most of what makes climate read as planetary.
	float lapse_rate = 0.5f;
	float moisture_period = 3000000.f;
	int moisture_octaves = 3;

	// Temperature's own noise field. It used to borrow the continental period and run a hardcoded two
	// octaves, which is why temperature came out far smoother than every other channel and could not
	// be tuned at all -- the only knob that touched it was the continent size.
	float temperature_period = 2550000.f;
	int temperature_octaves = 5;

	// fBm gain shared by all four climate channels: how much amplitude survives into the fine octaves.
	// Raising it is the difference between broad smooth regions and climate that varies locally.
	// One control rather than four, because the channels want to agree about how detailed the world is.
	float climate_gain = 0.62f;
	float weirdness_period = 4000000.f;
	int weirdness_octaves = 2;
	// Stretch each channel to fill [0, 1] after baking.
	//
	// Normalized fBm is a sum of octaves, so its values cluster hard around the middle -- measured at
	// 0.28..0.81 for moisture on a default bake. Biome anchors placed near 0 and 1 then sit out in the
	// tails and are effectively unreachable: four of the nine default biomes never appeared anywhere on
	// land. Stretching costs one pass over the texels and makes the whole climate square usable.
	bool normalize_channels = true;
};

// A cube-face texture set indexed by direction, with standard cubemap face selection and mapping.
//
// Cubemap semantics rather than the engine's own equi-angular lattice parameterization: the input is
// already a direction, GPUs filter cubemaps across face seams in hardware, and six square images are
// the format every painting tool already speaks. The cost is that texel density varies about 5x from
// face centre to corner, which is irrelevant for a field whose features are hundreds of kilometres
// across.
//
// Immutable once baked or loaded, which is what makes it safe to share by pointer across the physics
// threads instead of copying 50 MB into every collision shape.
struct PlanetMacroMapData {
	static constexpr int FACE_COUNT = 6;

	int resolution = 0; // Texels per axis per face. Power of two.
	// Face-major, then row-major within a face. One PlanetMacroSample per texel.
	std::vector<PlanetMacroSample> texels;

	bool is_valid() const {
		return resolution > 0 && texels.size() == size_t(FACE_COUNT) * size_t(resolution) * size_t(resolution);
	}

	void allocate(const int p_resolution) {
		resolution = p_resolution;
		texels.assign(size_t(FACE_COUNT) * size_t(p_resolution) * size_t(p_resolution), PlanetMacroSample());
	}

	const PlanetMacroSample &at(const int p_face, const int p_x, const int p_y) const {
		const int x = p_x < 0 ? 0 : (p_x >= resolution ? resolution - 1 : p_x);
		const int y = p_y < 0 ? 0 : (p_y >= resolution ? resolution - 1 : p_y);
		return texels[(size_t(p_face) * size_t(resolution) + size_t(y)) * size_t(resolution) + size_t(x)];
	}

	PlanetMacroSample &at_mut(const int p_face, const int p_x, const int p_y) {
		return texels[(size_t(p_face) * size_t(resolution) + size_t(p_y)) * size_t(resolution) + size_t(p_x)];
	}

	// Standard cubemap face selection. Face order matches OpenGL/Vulkan: +X, -X, +Y, -Y, +Z, -Z, so
	// the same data uploads to a hardware cubemap without reshuffling.
	static void direction_to_face_uv(const Vector3 &p_dir, int &r_face, double &r_u, double &r_v) {
		const double ax = Math::abs(p_dir.x);
		const double ay = Math::abs(p_dir.y);
		const double az = Math::abs(p_dir.z);
		double ma = 0.0;
		double sc = 0.0;
		double tc = 0.0;
		if (ax >= ay && ax >= az) {
			ma = ax;
			if (p_dir.x > 0.0) {
				r_face = 0;
				sc = -p_dir.z;
				tc = -p_dir.y;
			} else {
				r_face = 1;
				sc = p_dir.z;
				tc = -p_dir.y;
			}
		} else if (ay >= az) {
			ma = ay;
			if (p_dir.y > 0.0) {
				r_face = 2;
				sc = p_dir.x;
				tc = p_dir.z;
			} else {
				r_face = 3;
				sc = p_dir.x;
				tc = -p_dir.z;
			}
		} else {
			ma = az;
			if (p_dir.z > 0.0) {
				r_face = 4;
				sc = p_dir.x;
				tc = -p_dir.y;
			} else {
				r_face = 5;
				sc = -p_dir.x;
				tc = -p_dir.y;
			}
		}
		if (ma <= 0.0) {
			ma = 1.0;
		}
		r_u = 0.5 * (sc / ma + 1.0);
		r_v = 0.5 * (tc / ma + 1.0);
	}

	// Bilinear sample. Edge texels clamp rather than reaching into the neighbouring face: adjacent
	// faces agree exactly on their shared edge because the bake evaluates one continuous function of
	// direction, so the result stays continuous across a seam. Only the derivative flattens, within
	// half a texel -- kilometres on a macro map, and invisible under the micro noise.
	PlanetMacroSample sample(const Vector3 &p_dir, float p_height_origin = 0.f) const {
		PlanetMacroSample out;
		if (!is_valid()) {
			return out;
		}
		int face = 0;
		double u = 0.0;
		double v = 0.0;
		direction_to_face_uv(p_dir, face, u, v);

		// EDGE-INCLUSIVE grid: texel 0 sits exactly on u = 0 and texel resolution-1 exactly on u = 1,
		// rather than half a texel inside each. See the bake for why -- the two have to agree.
		const double fx = u * double(resolution - 1);
		const double fy = v * double(resolution - 1);
		const int x0 = int(Math::floor(fx));
		const int y0 = int(Math::floor(fy));
		const float tx = float(fx - double(x0));
		const float ty = float(fy - double(y0));

		const PlanetMacroSample &s00 = at(face, x0, y0);
		const PlanetMacroSample &s10 = at(face, x0 + 1, y0);
		const PlanetMacroSample &s01 = at(face, x0, y0 + 1);
		const PlanetMacroSample &s11 = at(face, x0 + 1, y0 + 1);

		// Subtract a height reference before interpolation when resolving a coastline. Rounding
		// an absolute climate height first loses millimetres which a steep cliff can amplify.
		out.height = PlanetHashNoise::lerp_f(PlanetHashNoise::lerp_f(s00.height - p_height_origin, s10.height - p_height_origin, tx),
				PlanetHashNoise::lerp_f(s01.height - p_height_origin, s11.height - p_height_origin, tx), ty);
		out.temperature = PlanetHashNoise::lerp_f(PlanetHashNoise::lerp_f(s00.temperature, s10.temperature, tx),
				PlanetHashNoise::lerp_f(s01.temperature, s11.temperature, tx), ty);
		out.moisture = PlanetHashNoise::lerp_f(PlanetHashNoise::lerp_f(s00.moisture, s10.moisture, tx),
				PlanetHashNoise::lerp_f(s01.moisture, s11.moisture, tx), ty);
		out.weirdness = PlanetHashNoise::lerp_f(PlanetHashNoise::lerp_f(s00.weirdness, s10.weirdness, tx),
				PlanetHashNoise::lerp_f(s01.weirdness, s11.weirdness, tx), ty);
		return out;
	}

	// Direction of a texel centre, used by the bake and by tests.
	static Vector3 face_uv_to_direction(const int p_face, const double p_u, const double p_v) {
		const double sc = p_u * 2.0 - 1.0;
		const double tc = p_v * 2.0 - 1.0;
		Vector3 d;
		switch (p_face) {
			case 0:
				d = Vector3(1.0, -tc, -sc);
				break;
			case 1:
				d = Vector3(-1.0, -tc, sc);
				break;
			case 2:
				d = Vector3(sc, 1.0, tc);
				break;
			case 3:
				d = Vector3(sc, -1.0, -tc);
				break;
			case 4:
				d = Vector3(sc, -tc, 1.0);
				break;
			default:
				d = Vector3(-sc, -tc, -1.0);
				break;
		}
		return d.normalized();
	}

	// Fills every face from noise. Temperature combines latitude banding with noise and then loses
	// height to the lapse rate, so climate follows the planet rather than looking like a third
	// independent noise field.
	void bake(const PlanetMacroBakeParams &p_params, const double p_planet_radius) {
		if (!is_valid()) {
			return;
		}
		PlanetHashNoise::FbmParams h_fbm;
		h_fbm.seed = p_params.seed;
		h_fbm.period = p_params.continent_period;
		h_fbm.octaves = p_params.continent_octaves;
		h_fbm.gain = p_params.climate_gain;

		PlanetHashNoise::FbmParams m_fbm;
		m_fbm.seed = p_params.seed + 101u;
		m_fbm.period = p_params.moisture_period;
		m_fbm.octaves = p_params.moisture_octaves;
		m_fbm.gain = p_params.climate_gain;

		PlanetHashNoise::FbmParams t_fbm;
		t_fbm.seed = p_params.seed + 202u;
		t_fbm.period = p_params.temperature_period;
		t_fbm.octaves = p_params.temperature_octaves;
		t_fbm.gain = p_params.climate_gain;

		PlanetHashNoise::FbmParams w_fbm;
		w_fbm.seed = p_params.seed + 303u;
		w_fbm.period = p_params.weirdness_period;
		w_fbm.octaves = p_params.weirdness_octaves;
		w_fbm.gain = p_params.climate_gain;

		for (int face = 0; face < FACE_COUNT; ++face) {
			for (int y = 0; y < resolution; ++y) {
				for (int x = 0; x < resolution; ++x) {
					// TEXEL CENTRES ON THE FACE EDGES, not half a texel inside them.
					//
					// This baked at (x + 0.5)/resolution, which puts the outermost texel of every face
					// half a texel INSIDE it. Two faces meeting at a cube edge then store their edge
					// values at directions half a texel apart -- some five kilometres here -- and the
					// sampler, which clamps at the edge, hands back each face's own. The result is a
					// step in the climate right along every cube seam. Continuous BAKE, discontinuous
					// SAMPLE: the function really is one function of direction, but the grid it lands
					// on does not line up across a seam.
					//
					// Harmless while the macro height scale was 1274 m. At macro_height_fraction 0.075
					// it is 239 km, and half a texel of climate gradient becomes a 347 m cliff down
					// twelve edges of the planet -- in the COLLIDER as much as the render, because both
					// read this map.
					//
					// With the edges included, the shared texel is the same direction on both faces, so
					// the two bakes write the same value and clamping returns it. The seam closes by
					// construction rather than by a cross-face lookup that the CPU and the shader would
					// each have to implement and keep in step.
					const double u = double(x) / double(resolution - 1);
					const double v = double(y) / double(resolution - 1);
					const Vector3 d = face_uv_to_direction(face, u, v);
					const double px = d.x * p_planet_radius;
					const double py = d.y * p_planet_radius;
					const double pz = d.z * p_planet_radius;

					PlanetMacroSample s;
					s.height = 0.5f * (PlanetHashNoise::fbm_3d_precise(px, py, pz, h_fbm) + 1.f);
					s.moisture = 0.5f * (PlanetHashNoise::fbm_3d_precise(px, py, pz, m_fbm) + 1.f);
					s.weirdness = 0.5f * (PlanetHashNoise::fbm_3d_precise(px, py, pz, w_fbm) + 1.f);

					// Polar map: 1 at the equator, 0 at the poles, shaped by the falloff exponent.
					const float polar = 1.f - float(Math::pow(Math::abs(d.y), double(p_params.polar_falloff)));
					const float t_noise = 0.5f * (PlanetHashNoise::fbm_3d_precise(px, py, pz, t_fbm) + 1.f);
					float temp = p_params.polar_weight * polar + (1.f - p_params.polar_weight) * t_noise;
					// Elevation still cools, and still does so before normalization, so the lapse rate
					// competes with the blend rather than being flattened by it.
					temp -= p_params.lapse_rate * s.height;
					s.temperature = temp;

					at_mut(face, x, y) = s;
				}
			}
		}

		if (p_params.normalize_channels) {
			normalize();
		} else {
			// Normalization would have clamped for us. Without it the blend can leave [0, 1] once the
			// lapse rate has been subtracted.
			for (PlanetMacroSample &s : texels) {
				s.temperature = s.temperature < 0.f ? 0.f : (s.temperature > 1.f ? 1.f : s.temperature);
			}
		}

		// AFTER normalization, never before: normalizing rescales a channel to fill [0, 1], which would
		// erase this scale exactly and leave the control doing nothing.
		if (p_params.temperature_scale != 1.f) {
			for (PlanetMacroSample &s : texels) {
				const float t = s.temperature * p_params.temperature_scale;
				s.temperature = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
			}
		}

		// Couple moisture to temperature, once both are settled in [0, 1].
		if (p_params.moisture_temperature_weight > 0.f) {
			const float w = p_params.moisture_temperature_weight;
			for (PlanetMacroSample &s : texels) {
				s.moisture *= (1.f - w) + w * s.temperature;
			}
			// Renormalize moisture alone, so the coupling redistributes wetness rather than draining
			// it. Without this a high weight simply makes the whole planet drier and the wet biomes
			// stop being reachable at all.
			if (p_params.normalize_channels) {
				float lo = 1e30f;
				float hi = -1e30f;
				for (const PlanetMacroSample &s : texels) {
					lo = s.moisture < lo ? s.moisture : lo;
					hi = s.moisture > hi ? s.moisture : hi;
				}
				const float span = hi - lo;
				if (span > 1e-4f) {
					const float inv = 1.f / span;
					for (PlanetMacroSample &s : texels) {
						s.moisture = (s.moisture - lo) * inv;
					}
				}
			}
		}
	}

	// Rescales every channel so its observed range fills [0, 1]. Applied to the whole map at once, not
	// per face, or the six faces would each get a different mapping and the seams would jump.
	void normalize() {
		if (texels.empty()) {
			return;
		}
		float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f };
		float hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
		for (const PlanetMacroSample &s : texels) {
			const float v[4] = { s.height, s.temperature, s.moisture, s.weirdness };
			for (int k = 0; k < 4; ++k) {
				lo[k] = v[k] < lo[k] ? v[k] : lo[k];
				hi[k] = v[k] > hi[k] ? v[k] : hi[k];
			}
		}
		float scale[4];
		for (int k = 0; k < 4; ++k) {
			const float span = hi[k] - lo[k];
			// A genuinely flat channel is left alone rather than amplified into noise.
			scale[k] = span > 1e-4f ? (1.f / span) : 0.f;
		}
		for (PlanetMacroSample &s : texels) {
			if (scale[0] > 0.f) {
				s.height = (s.height - lo[0]) * scale[0];
			}
			if (scale[1] > 0.f) {
				s.temperature = (s.temperature - lo[1]) * scale[1];
			}
			if (scale[2] > 0.f) {
				s.moisture = (s.moisture - lo[2]) * scale[2];
			}
			if (scale[3] > 0.f) {
				s.weirdness = (s.weirdness - lo[3]) * scale[3];
			}
		}
	}
	// Steepest the bilinear height channel can be, per metre of surface travel, per unit of the
	// channel. Worst case is a full 0-to-1 swing between adjacent texels, so it is set by the texel
	// spacing on the ground: one cube face spans a quarter turn, hence (pi/2 * radius) / resolution.
	double max_height_gradient_per_metre(const double p_planet_radius) const {
		if (!is_valid() || p_planet_radius <= 0.0) {
			return 0.0;
		}
		const double texel_m = (Math::PI * 0.5 * p_planet_radius) / double(resolution);
		return texel_m > 0.0 ? (1.0 / texel_m) : 0.0;
	}
};

using PlanetMacroMapRef = std::shared_ptr<const PlanetMacroMapData>;

// A macro map cannot cross the physics-server boundary the way the rest of the field does: shape data
// travels as a Variant, and a Variant cannot carry a shared_ptr to fifty megabytes. Copying it per
// patch is out of the question -- there are dozens of live patches.
//
// So the map is registered here once and referred to by id. The registry holds a strong reference, so
// the data outlives every shape that resolves it, and shapes take their own shared_ptr copy on
// resolve. Guarded by a mutex because Jolt resolves ids from the physics threads.
class PlanetMacroMapRegistry {
public:
	static uint64_t register_map(const PlanetMacroMapRef &p_map);
	static void unregister_map(uint64_t p_id);
	static PlanetMacroMapRef get(uint64_t p_id);
};

// One biome, as pure data. No per-biome code path exists anywhere: biomes differ only by these
// numbers, which is what keeps the evaluation loop a fixed length regardless of where you stand.
struct PlanetBiomeProfileData {
	// Position in climate space. Selection uses temperature and moisture only; elevation is applied
	// as a modifier instead of a third axis, which would triple the table for little gain.
	float anchor_temperature = 0.5f;
	float anchor_moisture = 0.5f;

	float base_elevation = 0.f; // Offset, as a fraction of the planet's amplitude.
	float amplitude_scale = 1.f; // Multiplies the planet's amplitude.
	float frequency_scale = 1.f; // Multiplies the base frequency (divides the period).
	float roughness = 0.5f; // fBm gain.
	float terrace_strength = 0.f; // 0 is untouched terrain, 1 is fully shaped into global terraces.
	float plateau_strength = 0.f; // 0 has no plateaus; 1 raises each plateau level by the full height.
	float landform_strength = 0.f; // Scales the major landforms (ranges, massifs, volcanoes, mesas).
	float hill_strength = 0.f; // Scales the minor landforms (hills, buttes, knolls).
	PlanetHashNoise::ShapeMix shape;

	// Which surface material this biome wears, on flat ground and on cliffs.
	//
	// Indices into the renderer's material palette, not colours: the biome table says WHAT a place is
	// made of and the renderer decides how that looks, so the same table drives a splat map, a
	// footstep sound, or a spawn rule without any of them knowing about the others. Two integers,
	// which keeps the promise the file opens with -- a biome is nothing but numbers.
	int ground_material = 4;
	int cliff_material = 5;
};

// The blended result: what the noise loop actually runs with at one point.
struct PlanetBlendedParams {
	float base_elevation = 0.f;
	float amplitude_scale = 1.f;
	float frequency_scale = 1.f;
	float roughness = 0.5f;
	float terrace_strength = 0.f;
	float plateau_strength = 0.f;
	float landform_strength = 0.f;
	float hill_strength = 0.f;
	PlanetHashNoise::ShapeMix shape;
};

struct PlanetBiomeTable {
	static constexpr int MAX_BIOMES = 16;

	PlanetBiomeProfileData entries[MAX_BIOMES];
	int count = 0;
	// Support radius of the blend kernel in climate space. Must be wide enough that every point is
	// reached by at least one anchor, or the fallback below has to carry it.
	float kernel_radius = 0.55f;

	bool is_empty() const { return count <= 0; }

	// Smooth, finite-support, and evaluated against EVERY biome rather than the nearest few. A
	// nearest-N scheme is discontinuous: the moment the N-th nearest anchor changes, the blended
	// parameters jump and the surface steps. With a dozen anchors the extra distance evaluations are
	// nothing next to one octave of noise.
	static float kernel(const float p_dist_sq, const float p_radius) {
		const float r2 = p_radius * p_radius;
		if (p_dist_sq >= r2) {
			return 0.f;
		}
		const float t = 1.f - p_dist_sq / r2;
		return t * t; // C1 at the support boundary, so parameters ease in rather than switch on.
	}

	void blend(const float p_temperature, const float p_moisture, PlanetBlendedParams &r_out) const {
		r_out = PlanetBlendedParams();
		if (count <= 0) {
			return;
		}

		float weights[MAX_BIOMES];
		float total = 0.f;
		for (int i = 0; i < count; ++i) {
			const float dt = p_temperature - entries[i].anchor_temperature;
			const float dm = p_moisture - entries[i].anchor_moisture;
			weights[i] = kernel(dt * dt + dm * dm, kernel_radius);
			total += weights[i];
		}

		// Nothing in range: fall back to the single nearest anchor so the field stays defined
		// everywhere. Reachable only if the kernel radius is set smaller than the anchor spacing.
		if (total <= 0.f) {
			int best = 0;
			float best_d = 1e30f;
			for (int i = 0; i < count; ++i) {
				const float dt = p_temperature - entries[i].anchor_temperature;
				const float dm = p_moisture - entries[i].anchor_moisture;
				const float d = dt * dt + dm * dm;
				if (d < best_d) {
					best_d = d;
					best = i;
				}
			}
			weights[best] = 1.f;
			total = 1.f;
		}

		const float inv = 1.f / total;
		float base_elevation = 0.f;
		float amplitude_scale = 0.f;
		float frequency_scale = 0.f;
		float roughness = 0.f;
		float terrace_strength = 0.f;
		float plateau_strength = 0.f;
		float landform_strength = 0.f;
		float hill_strength = 0.f;
		float w_std = 0.f;
		float w_bil = 0.f;
		float w_rid = 0.f;
		for (int i = 0; i < count; ++i) {
			const float w = weights[i] * inv;
			base_elevation += entries[i].base_elevation * w;
			amplitude_scale += entries[i].amplitude_scale * w;
			// Normalize after summing. Summing individually normalized weights does not preserve
			// a constant 1.0 in float32; that error rescales planet-centred noise coordinates and
			// creates centimetre-scale height jumps even when every biome uses global frequency.
			frequency_scale += entries[i].frequency_scale * weights[i];
			roughness += entries[i].roughness * w;
			terrace_strength += entries[i].terrace_strength * w;
			plateau_strength += entries[i].plateau_strength * w;
			landform_strength += entries[i].landform_strength * w;
			hill_strength += entries[i].hill_strength * w;
			w_std += entries[i].shape.standard * w;
			w_bil += entries[i].shape.billow * w;
			w_rid += entries[i].shape.ridged * w;
		}
		r_out.base_elevation = base_elevation;
		r_out.amplitude_scale = amplitude_scale;
		r_out.frequency_scale = frequency_scale / total;
		r_out.roughness = roughness;
		r_out.terrace_strength = terrace_strength;
		r_out.plateau_strength = plateau_strength;
		r_out.landform_strength = landform_strength;
		r_out.hill_strength = hill_strength;
		r_out.shape.standard = w_std;
		r_out.shape.billow = w_bil;
		r_out.shape.ridged = w_rid;
	}

	float max_plateau_strength() const {
		float result = 0.f;
		for (int i = 0; i < count; ++i) {
			result = result > entries[i].plateau_strength ? result : entries[i].plateau_strength;
		}
		return result;
	}

	float max_landform_strength() const {
		float result = 0.f;
		for (int i = 0; i < count; ++i) {
			result = result > entries[i].landform_strength ? result : entries[i].landform_strength;
		}
		return result;
	}

	float max_hill_strength() const {
		float result = 0.f;
		for (int i = 0; i < count; ++i) {
			result = result > entries[i].hill_strength ? result : entries[i].hill_strength;
		}
		return result;
	}

	float max_terrace_strength() const {
		float result = 0.f;
		for (int i = 0; i < count; ++i) {
			result = result > entries[i].terrace_strength ? result : entries[i].terrace_strength;
		}
		return result;
	}

	// Worst-case parameter values over the whole table. The global radius bounds and the Lipschitz
	// constant have to hold everywhere, and a blend can never exceed the extremes of what it blends.
	void extremes(float &r_max_amplitude, float &r_max_frequency, float &r_max_roughness,
			float &r_min_base_elev, float &r_max_base_elev) const {
		r_max_amplitude = 0.f;
		r_max_frequency = 0.f;
		r_max_roughness = 0.f;
		r_min_base_elev = 0.f;
		r_max_base_elev = 0.f;
		if (count <= 0) {
			r_max_amplitude = 1.f;
			r_max_frequency = 1.f;
			r_max_roughness = 0.5f;
			return;
		}
		r_min_base_elev = entries[0].base_elevation;
		r_max_base_elev = entries[0].base_elevation;
		for (int i = 0; i < count; ++i) {
			const PlanetBiomeProfileData &e = entries[i];
			r_max_amplitude = r_max_amplitude > e.amplitude_scale ? r_max_amplitude : e.amplitude_scale;
			r_max_frequency = r_max_frequency > e.frequency_scale ? r_max_frequency : e.frequency_scale;
			r_max_roughness = r_max_roughness > e.roughness ? r_max_roughness : e.roughness;
			r_min_base_elev = r_min_base_elev < e.base_elevation ? r_min_base_elev : e.base_elevation;
			r_max_base_elev = r_max_base_elev > e.base_elevation ? r_max_base_elev : e.base_elevation;
		}
	}

	// Widest shape range across the table, for exact global bounds.
	void shape_extremes(float &r_min, float &r_max) const {
		r_min = 0.f;
		r_max = 0.f;
		if (count <= 0) {
			PlanetHashNoise::ShapeMix().bounds(r_min, r_max);
			return;
		}
		bool first = true;
		for (int i = 0; i < count; ++i) {
			float lo = 0.f;
			float hi = 0.f;
			entries[i].shape.bounds(lo, hi);
			if (first) {
				r_min = lo;
				r_max = hi;
				first = false;
			} else {
				r_min = r_min < lo ? r_min : lo;
				r_max = r_max > hi ? r_max : hi;
			}
		}
	}

	// The nine-biome default: a 3x3 grid over temperature and moisture. Mountains are not a biome
	// here -- elevation drives amplitude and ridging as a modifier, so the table stays small enough
	// to actually tune.
	static PlanetBiomeTable make_default() {
		PlanetBiomeTable t;
		struct Def {
			float temp;
			float moist;
			float base;
			float amp;
			float freq;
			float rough;
			float s;
			float b;
			float r;
			int ground;
			int cliff;
		};
		// Material indices are the renderer's palette order:
		// 0 grass, 1 dry grass, 2 sand, 3 gravel, 4 rock, 5 cliff, 6 snow, 7 ice.
		// temp, moist, base_elev, amp, freq, rough, standard, billow, ridged, ground, cliff
		static const Def defs[9] = {
			{ 0.15f, 0.15f, -0.05f, 0.35f, 0.8f, 0.45f, 0.9f, 0.1f, 0.0f, 3, 5 }, // polar desert
			{ 0.15f, 0.50f, 0.00f, 0.55f, 1.0f, 0.50f, 0.8f, 0.1f, 0.1f, 3, 4 }, // tundra
			{ 0.15f, 0.85f, 0.10f, 0.80f, 1.1f, 0.55f, 0.4f, 0.5f, 0.1f, 7, 5 }, // glacial domes
			{ 0.50f, 0.15f, -0.02f, 0.45f, 0.9f, 0.48f, 0.85f, 0.15f, 0.0f, 1, 4 }, // steppe
			{ 0.50f, 0.50f, 0.00f, 0.60f, 1.0f, 0.50f, 0.7f, 0.1f, 0.2f, 0, 4 }, // plains
			{ 0.50f, 0.85f, 0.08f, 0.95f, 1.2f, 0.55f, 0.3f, 0.1f, 0.6f, 0, 5 }, // forested hills
			{ 0.85f, 0.15f, -0.04f, 0.70f, 0.6f, 0.60f, 0.2f, 0.8f, 0.0f, 2, 2 }, // dunes
			{ 0.85f, 0.50f, 0.00f, 0.50f, 1.0f, 0.50f, 0.8f, 0.1f, 0.1f, 1, 4 }, // savanna
			{ 0.85f, 0.85f, 0.12f, 1.00f, 1.3f, 0.58f, 0.2f, 0.1f, 0.7f, 0, 5 }, // jungle highlands
		};
		t.count = 9;
		for (int i = 0; i < 9; ++i) {
			t.entries[i].anchor_temperature = defs[i].temp;
			t.entries[i].anchor_moisture = defs[i].moist;
			t.entries[i].base_elevation = defs[i].base;
			t.entries[i].amplitude_scale = defs[i].amp;
			t.entries[i].frequency_scale = defs[i].freq;
			t.entries[i].roughness = defs[i].rough;
			t.entries[i].shape.standard = defs[i].s;
			t.entries[i].shape.billow = defs[i].b;
			t.entries[i].shape.ridged = defs[i].r;
			t.entries[i].ground_material = defs[i].ground;
			t.entries[i].cliff_material = defs[i].cliff;
		}
		return t;
	}
};
