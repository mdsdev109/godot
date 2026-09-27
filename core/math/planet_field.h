/**************************************************************************/
/*  planet_field.h                                                        */
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

#include "core/math/planet_det_math.h"
#include "core/math/math_funcs.h"
#include "core/math/planet_biome.h"
#include "core/math/planet_coast.h"
#include "core/math/planet_hash_noise.h"
#include "core/math/planet_landform.h"
#include "core/math/vector3.h"

#include <atomic>

// The definition of `f`: a planet's surface radius as a function of direction from its centre.
//
//     f(dir) = base_radius + amplitude * fbm(dir * base_radius)
//
// Plain data with no Object machinery, and it lives in core/ rather than scene/ for a specific
// reason: the Jolt collision shape needs it on the physics thread, and a physics backend must not
// depend on scene/. PlanetField3D in scene/resources/3d/ is a thin Resource wrapper around this, and
// the shape holds a copy by value -- which also makes it trivially thread-safe, since a copy cannot
// be mutated out from under a running query.
//
// The noise is sampled in 3D at the surface point rather than as a 2D field over some spherical
// parameterization. 3D noise on a sphere is inherently seamless, so there are no cube-face or
// meridian seams to blend across.
// Instrumentation for the planet collision path.
//
// Lives in core/ so that both the Jolt backend (which increments it) and the scene layer (which
// reports it to script) can reach it without either depending on the other. Counters are relaxed
// atomics because Jolt runs the narrow phase across several threads; relaxed is sufficient since
// nothing here orders other memory.
//
// The cost that matters is field samples: at the measured 6.75 ns per octave per sample, a 13-octave
// field is about 88 ns per sample, and the design budgets 1 ms/frame for 100 active bodies.
struct PlanetCollisionStats {
	static inline std::atomic<uint64_t> collide_queries{ 0 };
	static inline std::atomic<uint64_t> ray_queries{ 0 };
	static inline std::atomic<uint64_t> cast_queries{ 0 };
	static inline std::atomic<uint64_t> rejected_queries{ 0 };
	static inline std::atomic<uint64_t> cells_tessellated{ 0 };
	static inline std::atomic<uint64_t> field_samples{ 0 };
	static inline std::atomic<uint64_t> nanoseconds{ 0 };

	static void reset() {
		collide_queries.store(0, std::memory_order_relaxed);
		ray_queries.store(0, std::memory_order_relaxed);
		cast_queries.store(0, std::memory_order_relaxed);
		rejected_queries.store(0, std::memory_order_relaxed);
		cells_tessellated.store(0, std::memory_order_relaxed);
		field_samples.store(0, std::memory_order_relaxed);
		nanoseconds.store(0, std::memory_order_relaxed);
	}

	static void add(std::atomic<uint64_t> &p_counter, uint64_t p_value) {
		p_counter.fetch_add(p_value, std::memory_order_relaxed);
	}
};

// Every member that changes the surface. A shape only rebuilds when this says something moved, so a
// field left out here is a field whose edits never reach the collider.
struct PlanetFieldData;
bool planet_field_equal(const PlanetFieldData &a, const PlanetFieldData &b);

struct PlanetFieldData {
	double base_radius = 3185500.0;
	// Peak displacement either side of base_radius. fbm() is normalized to [-1, 1], so this is an
	// exact bound on the displacement, not a nominal figure.
	double amplitude = 8000.0;

	// Defaults must be stated here, not left to FbmParams. Its own defaults are period 1.0 and a
	// single octave, which at planet scale means a one-metre wavelength sampled on a 0.19 m lattice:
	// the surface aliases into noise swinging the full amplitude between adjacent vertices, which
	// reads as spurious collisions tens of metres above the ground rather than as bad-looking terrain.
	//
	// 2443 m is the ground sample distance of a 2048^2/face cubemap at this radius, and 13 octaves
	// from there reaches the 0.25 m voxel edge. Detail finer than the voxel edge must not be here: the
	// voxel layer reconstructs the surface piecewise-linearly at that spacing, so anything below it is
	// kept on the analytic side and discarded on the voxel side, which is a trippable step exactly
	// where players dig.
	PlanetHashNoise::FbmParams fbm = { 0u, 2443.0f, 13, 2.0f, 0.5f };

	// How each octave is reshaped. Defaults to pure standard noise, which makes the shaped evaluation
	// bit-identical to the old plain-fBm one -- so turning these knobs is the only thing that can
	// change the surface, not merely switching to the new code path.
	PlanetHashNoise::ShapeMix shape;

	// Low-frequency distortion of the sample position. Zero amplitude disables it.
	PlanetHashNoise::WarpParams warp;

	// Slope-based octave attenuation: 0 is classic fBm, larger values push detail off flat ground and
	// onto slopes. Dimensionless, because the gradient is scaled into real height-per-metre first.
	double slope_erosion = 0.0;

	// Bedrock. The surface is clamped to at least this radius, so terrain can never dig below it.
	// Zero disables the clamp.
	double floor_radius = 0.0;

	// Water surface radius. Water exists only where this is above the floor -- a planet whose sea
	// level is at or below its bedrock is simply dry, with no separate flag to keep in sync.
	double sea_level_radius = 0.0;
	PlanetCoastParams coast;

	bool use_coast() const { return coast.enabled && has_water() && macro_height_scale > 0.0 && use_biomes(); }

	Vector3 sample_coast(const Vector3 &p_unit_dir) const {
		if (!use_coast()) {
			return Vector3();
		}
		const float sea_height = float(0.5 + (sea_level_radius - base_radius) / macro_height_scale);
		const float control = macro_map->sample(p_unit_dir, sea_height).height * float(macro_height_scale);
		return coast.sample(p_unit_dir * base_radius, control);
	}

	double coast_offset(const Vector3 &p_unit_dir, double p_offset) const {
		if (!use_coast() || macro_height_scale <= 0.0) {
			return p_offset;
		}
		const PlanetMacroSample macro = macro_map->sample(p_unit_dir);
		const Vector3 c = sample_coast(p_unit_dir);
		const double macro_term = macro_height_scale * (double(macro.height) - 0.5);
		return sea_level_radius - base_radius + double(PlanetCoastParams::add_relief(float(c.z), float(p_offset - macro_term)));
	}

	double coast_displacement_bound() const {
		// The beach term is at most 0.75*35/e metres; relief limiting only reduces displacement.
		return use_coast() ? double(coast.noise_amplitude + coast.cliff_height) + 10.0 : 0.0;
	}

	bool has_water() const { return sea_level_radius > floor_radius; }

	// How much dry ground must lie between an excavation and the sea, measured ALONG THE SURFACE in
	// metres. The safety margin behind can_dig().
	double dig_margin = 25.0;

	// Rays used when searching for the coast. Each one is sphere-traced, so this is angular coverage
	// rather than step count: the gap between adjacent rays at the far end of a search of range D is
	// D * TAU / RAYS, and the certified ball around each sample closes most of it.
	static constexpr int COAST_SEARCH_RAYS = 16;

	// How far below sea level the ORIGINAL surface lies here. Positive means sea bed, negative means
	// dry land. Deliberately a property of the field rather than of whatever the terrain has been
	// dug into: a hole inland does not become ocean because someone dug below sea level, and the
	// water surface is masked by this, not by the excavated geometry.
	double sea_depth(const Vector3 &p_unit_dir) const {
		return sea_level_radius - eval_radius(p_unit_dir);
	}

	bool is_submerged(const Vector3 &p_unit_dir) const {
		return has_water() && sea_depth(p_unit_dir) > 0.0;
	}

	// Is a point inside the sea? Used for underwater effects, buoyancy and camera state.
	bool is_point_underwater(const Vector3 &p_point) const {
		if (!has_water()) {
			return false;
		}
		return p_point.length() < sea_level_radius;
	}

	// How far you can walk from here before the ground drops below sea level, in metres of surface
	// travel, saturating at p_max_search.
	//
	// A HORIZONTAL question, not a vertical one. "Am I high enough above the water?" is the wrong rule
	// for a dig permit: standing two metres above sea level in the middle of a continent is perfectly
	// safe, while standing two hundred metres up on a sea cliff is not -- the cliff has the whole ocean
	// a few paces away and one bad tunnel from flooding. What decides safety is the width of the rock
	// between you and the water, so that is what this measures.
	//
	// Sphere-traced rather than sampled on a grid. The field's Lipschitz bound L says the surface
	// cannot change height by more than L metres per metre of travel, so ground standing h above sea
	// level certifies a coast-free ball of radius h/L around itself. Marching by that radius makes the
	// step collapse only as the coast is approached, and inland ground -- where h/L already covers the
	// whole search range -- costs ONE field evaluation and no marching at all. That early-out is what
	// makes this affordable to ask per dig, per frame: the expensive case only happens at the beach.
	//
	// Conservative in the two ways that matter. It never reports a coast that is not there, and the
	// saturating return means "at least this far", never "exactly this far".
	double distance_to_coast(const Vector3 &p_unit_dir, double p_max_search) const {
		if (p_max_search <= 0.0) {
			return 0.0;
		}
		if (!has_water()) {
			return p_max_search;
		}
		const double h0 = eval_radius(p_unit_dir) - sea_level_radius;
		if (h0 <= 0.0) {
			// Already standing on sea bed. The coast is not nearby, it is underfoot.
			return 0.0;
		}
		const double lip = get_lipschitz_per_metre();
		if (lip <= 0.0) {
			// A surface that cannot change height cannot reach the sea from dry ground.
			return p_max_search;
		}
		const double certified = h0 / lip;
		if (certified >= p_max_search) {
			return p_max_search;
		}

		// A basis for walking away from here along the sphere.
		Vector3 u = p_unit_dir.cross(Vector3(0, 0, 1));
		if (u.length_squared() < 1e-12) {
			u = p_unit_dir.cross(Vector3(0, 1, 0));
		}
		u.normalize();
		const Vector3 v = p_unit_dir.cross(u).normalized();

		// A floor on the step so a ray grazing the waterline cannot march in ever-shrinking increments.
		// It costs accuracy at the coast itself, in the direction of reporting the coast slightly nearer
		// than it is, which is the safe direction.
		const double min_step = MAX(p_max_search * 0.02, 0.05);
		double nearest = p_max_search;

		for (int k = 0; k < COAST_SEARCH_RAYS; ++k) {
			const double az = (Math::TAU * double(k)) / double(COAST_SEARCH_RAYS);
			const Vector3 t = u * PlanetDetMath::cos(az) + v * PlanetDetMath::sin(az);
			// Start where the centre sample already certifies clear ground, and stop at whatever the
			// rays before this one already found -- later rays get cheaper as the answer tightens.
			double s = certified;
			while (s < nearest) {
				const double a = s / sea_level_radius;
				const Vector3 d = (p_unit_dir * PlanetDetMath::cos(a) + t * PlanetDetMath::sin(a)).normalized();
				const double h = eval_radius(d) - sea_level_radius;
				if (h <= 0.0) {
					nearest = s;
					break;
				}
				s += MAX(h / lip, min_step);
			}
		}
		return nearest;
	}

	// Whether digging is permitted here at all. Refused on the sea bed, and on any ground with the
	// coast closer than the margin -- however high above the water that ground happens to sit.
	bool can_dig(const Vector3 &p_unit_dir) const {
		if (!has_water()) {
			return true;
		}
		if (dig_margin <= 0.0) {
			// No margin asked for: the only rule left is that you cannot dig the sea bed itself.
			return !is_submerged(p_unit_dir);
		}
		return distance_to_coast(p_unit_dir, dig_margin) >= dig_margin;
	}

	// The lowest radius that may be excavated at this direction.
	//
	// Digging into the sea is not something the renderer or the collider can recover from gracefully --
	// the water surface is a mask over the ORIGINAL coastline, so a breach would leave a dry hole under
	// a sea that is still drawn above it. Rather than model flooding, the rule is that the sea cannot be
	// reached: within the margin of a coast nothing may be removed at all, and beyond it the ground is
	// far enough from the water that bedrock is a safe floor.
	//
	// Note this is a POINT query. It is the right answer for the block under the cursor and the wrong
	// one for a tunnel: a shaft begun safely inland and driven sideways passes through directions this
	// was never asked about. Volume edits must test every direction they touch, not just their origin.
	double dig_floor_radius(const Vector3 &p_unit_dir) const {
		if (!can_dig(p_unit_dir)) {
			// Nothing may be removed: the floor is the ground already standing here.
			return eval_radius(p_unit_dir);
		}
		return floor_radius;
	}

	// Climate map and biome table. Shared by pointer, never copied: the map is tens of megabytes and
	// PlanetFieldData is copied by value into every collision shape. It is immutable once baked, which
	// is what makes sharing it across the physics threads safe.
	PlanetMacroMapRef macro_map;
	PlanetBiomeTable biomes;

	// Metres of elevation swing contributed directly by the macro height channel, before any biome
	// noise. This is what makes continents and ocean basins, as opposed to local terrain.
	double macro_height_scale = 0.0;

	// Elevation modifiers, applied on top of the blended biome parameters. Mountains are expressed
	// this way rather than as their own biomes: high ground gets more amplitude and more ridging
	// regardless of climate, which is true of real mountains and keeps the biome table at nine
	// entries instead of twenty-seven.
	double mountain_amplitude = 0.0;
	double mountain_ridge = 0.0;

	// Globally aligned vertical spacing between terrain shelves, in metres. Individual biomes blend
	// their terrace strength, but not the spacing: changing the spacing across climate boundaries
	// would drag every height band sideways in the same way spatially varying noise frequency does.
	// Zero disables the shaping entirely.
	double terrace_step = 0.0;
	// Fraction of each step spent climbing from one shelf to the next; the rest is split between the
	// two benches. Global for the same reason the spacing is: a width that varied with the biome blend
	// would add a term to the slope that the Lipschitz bound below does not account for. Small is a
	// cliff-like riser, large a soft rolling bank.
	double terrace_riser = 0.2;

	// Plateaus: a few large level land masses separated by single tall walls. A low-frequency mask
	// (three octaves, the finer two only wiggling the walls) is split into `plateau_levels` levels;
	// each boundary climbs one `plateau_height` inside the middle `plateau_edge` of a level. Applied
	// AFTER terracing, so a plateau wall is one wall rather than a staircase, and scaled by the biome
	// blend's plateau strength so flat country can opt out. Zero height disables it.
	double plateau_height = 0.0;
	double plateau_period = 8192.0;
	int plateau_levels = 3;
	double plateau_edge = 0.08;
	double plateau_spread = 1.6;
	uint32_t plateau_seed = 4099u;

	// Landforms: discrete mountains, ranges, volcanoes and mesas on one cell grid, and hills, buttes and
	// knolls on a finer one -- see planet_landform.h. Applied after the plateaus, scaled by the biome
	// blend's landform and hill strengths. A layer with zero height is disabled.
	PlanetLandforms landforms;

	bool use_biomes() const { return macro_map != nullptr && macro_map->is_valid() && !biomes.is_empty(); }

	// Resolves the per-point noise parameters from the macro map and biome table. One place, so the
	// scalar and batched evaluators cannot drift apart.
	void resolve_params(const Vector3 &p_unit_dir, PlanetHashNoise::FbmParams &r_fbm,
			PlanetHashNoise::ShapeMix &r_shape, double &r_amplitude, double &r_base_offset,
			float &r_terrace_strength, float &r_plateau_strength, float *r_landform = nullptr) const {
		if (r_landform) {
			r_landform[0] = 0.f;
			r_landform[1] = 0.f;
		}
		r_fbm = fbm;
		r_shape = shape;
		r_amplitude = amplitude;
		r_base_offset = 0.0;
		r_terrace_strength = 0.f;
		r_plateau_strength = 0.f;
		if (!use_biomes()) {
			return;
		}

		const PlanetMacroSample macro = macro_map->sample(p_unit_dir);
		PlanetBlendedParams b;
		biomes.blend(macro.temperature, macro.moisture, b);

		const double h = double(macro.height);
		double amp_scale = double(b.amplitude_scale) * (1.0 + mountain_amplitude * h);

		PlanetHashNoise::ShapeMix mix = b.shape;
		mix.ridged += float(mountain_ridge * h);
		const float sum = mix.standard + mix.billow + mix.ridged;
		if (sum > 0.f) {
			const float inv = 1.f / sum;
			mix.standard *= inv;
			mix.billow *= inv;
			mix.ridged *= inv;
		}

		r_shape = mix;
		r_amplitude = amplitude * amp_scale;
		r_base_offset = amplitude * double(b.base_elevation) + macro_height_scale * (h - 0.5);
		if (b.frequency_scale > 0.f) {
			r_fbm.period = fbm.period / b.frequency_scale;
		}
		r_fbm.gain = b.roughness;
		r_terrace_strength = b.terrace_strength;
		r_plateau_strength = b.plateau_strength;
		if (r_landform) {
			r_landform[0] = b.landform_strength;
			r_landform[1] = b.hill_strength;
		}
	}

	// Height the landforms add at a point, given the blended landform and hill strengths.
	double landform_offset(const Vector3 &p_unit_dir, const float *p_strength) const {
		double result = 0.0;
		if (p_strength[0] > 0.f && landforms.major.height > 0.0) {
			result += double(p_strength[0]) * PlanetLandform::eval_layer(landforms, landforms.major, true, p_unit_dir, base_radius);
		}
		if (p_strength[1] > 0.f && landforms.minor.height > 0.0) {
			result += double(p_strength[1]) * PlanetLandform::eval_layer(landforms, landforms.minor, false, p_unit_dir, base_radius);
		}
		return result;
	}

	// The plateau mask in 0..plateau_levels before shaping, from fixed-wavelength noise. Coordinates keep
	// the CPU's doubles, like the coast's.
	double plateau_phase(const Vector3 &p_unit_dir) const {
		using namespace PlanetHashNoise;
		const Vector3 p = p_unit_dir * base_radius;
		const double inv = 1.0 / plateau_period;
		const float n0 = perlin_3d_precise(p.x * inv, p.y * inv, p.z * inv, plateau_seed);
		const float n1 = perlin_3d_precise(p.x * inv * 4.0, p.y * inv * 4.0, p.z * inv * 4.0, plateau_seed + 1u);
		const float n2 = perlin_3d_precise(p.x * inv * 16.0, p.y * inv * 16.0, p.z * inv * 16.0, plateau_seed + 2u);
		const double n = 0.75 * double(n0) + 0.2 * double(n1) + 0.05 * double(n2);
		return CLAMP((0.5 + 0.5 * plateau_spread * n) * double(plateau_levels), 0.0, double(plateau_levels));
	}

	// Height the plateaus add at a point: whole levels, plus a cubic climb in the middle `plateau_edge`
	// of each, times the blended strength.
	double plateau_offset(const Vector3 &p_unit_dir, const float p_strength) const {
		if (plateau_height <= 0.0 || p_strength <= 0.f || plateau_levels <= 0) {
			return 0.0;
		}
		const double t = plateau_phase(p_unit_dir);
		const double level = Math::floor(t);
		const double phase = t - level;
		const double s = CLAMP((phase - (0.5 - 0.5 * plateau_edge)) / plateau_edge, 0.0, 1.0);
		const double eased = s * s * (3.0 - 2.0 * s);
		return double(p_strength) * plateau_height * (level + eased);
	}

	// Smoothly shelves an already-computed height without sampling another field. The middle
	// `terrace_riser` of each step is a cubic transition and the rest is split between the lower and
	// upper benches. This keeps the surface continuous while producing risers between broad walkable
	// shelves. Levels are planet-global so neighbouring biomes agree on every shelf.
	double apply_terracing(const double p_offset, const float p_strength) const {
		if (terrace_step <= 0.0 || p_strength <= 0.f) {
			return p_offset;
		}
		const double scaled = p_offset / terrace_step;
		const double level = Math::floor(scaled);
		const double phase = scaled - level;
		const double t = CLAMP((phase - (0.5 - 0.5 * terrace_riser)) / terrace_riser, 0.0, 1.0);
		const double eased = t * t * (3.0 - 2.0 * t);
		const double shelved = (level + eased) * terrace_step;
		return Math::lerp(p_offset, shelved, double(CLAMP(p_strength, 0.f, 1.f)));
	}

	// Applies the bedrock clamp. Every path that produces a radius goes through here, so the floor
	// cannot be honoured by the renderer and missed by the collider.
	double apply_floor(const double p_radius) const {
		return p_radius < floor_radius ? floor_radius : p_radius;
	}

	// Exact global bounds. Still exact, still no sampling: the shaped signal is piecewise linear in
	// the raw noise, so ShapeMix::bounds() gives its true range, and the slope weighting can only
	// produce a weighted average of shaped values, which cannot leave that range.
	// Worst case over everything that can vary: the shape mix, the biome amplitude and base
	// elevation, the macro height channel and the mountain modifiers. A blend can never exceed the
	// extremes of what it blends, so taking each extreme independently is conservative and still
	// exact enough to be useful.
	void get_extreme_terms(double &r_max_amp, double &r_min_offset, double &r_max_offset,
			float &r_shape_min, float &r_shape_max) const {
		shape.bounds(r_shape_min, r_shape_max);
		r_max_amp = amplitude;
		r_min_offset = 0.0;
		r_max_offset = 0.0;
		if (!use_biomes()) {
			return;
		}
		float max_amp_scale = 1.f;
		float max_freq = 1.f;
		float max_rough = 0.5f;
		float min_base = 0.f;
		float max_base = 0.f;
		biomes.extremes(max_amp_scale, max_freq, max_rough, min_base, max_base);
		float t_lo = 0.f;
		float t_hi = 0.f;
		biomes.shape_extremes(t_lo, t_hi);
		// The mountain modifier pushes the mix towards pure ridged, whose range is [0, 1], so the
		// union with that covers every mix reachable after renormalization.
		r_shape_min = t_lo < 0.f ? t_lo : 0.f;
		r_shape_max = t_hi > 1.f ? t_hi : 1.f;
		r_max_amp = amplitude * double(max_amp_scale) * (1.0 + mountain_amplitude);
		r_min_offset = amplitude * double(min_base) - macro_height_scale * 0.5;
		r_max_offset = amplitude * double(max_base) + macro_height_scale * 0.5;
		// The shelf differs from its input by at most the width of one bench: just below the riser the
		// input has climbed (1 - riser) / 2 of a step and the shelf has not moved. This was a quarter
		// step when the riser was half the step, and was not updated when it narrowed to a fifth, which
		// left the advertised radius range 0.15 step too tight.
		const double terrace_pad = terrace_step > 0.0
				? terrace_step * 0.5 * (1.0 - terrace_riser) * double(biomes.max_terrace_strength())
				: 0.0;
		r_min_offset -= terrace_pad;
		r_max_offset += terrace_pad;
		// Plateaus only ever raise the ground, by at most every level at full strength.
		if (plateau_height > 0.0 && plateau_levels > 0) {
			r_max_offset += plateau_height * double(plateau_levels) * double(biomes.max_plateau_strength());
		}
		// Landforms raise the ground by at most their height, plus the smooth maximum's overshoot and one
		// tier; the tiers can also dip a step below the smooth profile. The minor layer sums, so allow for
		// overlapping features.
		if (landforms.major.height > 0.0) {
			const double s = double(biomes.max_landform_strength());
			const double tier = PlanetLandform::max_tier(landforms.major, true);
			r_max_offset += s * (landforms.major.height * 1.1 + tier);
			r_min_offset -= s * tier;
		}
		if (landforms.minor.height > 0.0) {
			const double s = double(biomes.max_hill_strength());
			const double tier = PlanetLandform::max_tier(landforms.minor, false);
			r_max_offset += 8.0 * s * (landforms.minor.height + tier);
			r_min_offset -= 8.0 * s * tier;
		}
	}

	double get_min_radius() const {
		double max_amp = 0.0;
		double min_off = 0.0;
		double max_off = 0.0;
		float lo = 0.f;
		float hi = 0.f;
		get_extreme_terms(max_amp, min_off, max_off, lo, hi);
		const double contrib = max_amp * double(lo < 0.f ? lo : 0.f);
		return apply_floor(base_radius + min_off + contrib - coast_displacement_bound());
	}
	double get_max_radius() const {
		double max_amp = 0.0;
		double min_off = 0.0;
		double max_off = 0.0;
		float lo = 0.f;
		float hi = 0.f;
		get_extreme_terms(max_amp, min_off, max_off, lo, hi);
		const double contrib = max_amp * double(hi > 0.f ? hi : 0.f);
		return apply_floor(base_radius + max_off + contrib + coast_displacement_bound());
	}

	// Radius for one unit direction.
	//
	// Every arithmetic step here must match eval_radius_batch exactly, including where the narrowing
	// to float happens. In a double build `double * double -> float` and `float * float` round
	// differently, and at planet scale that gap is a whole float32 ULP, which is 0.25 m. Both are the
	// definition of `f`, so they must agree bit for bit, not approximately.
	double eval_radius(const Vector3 &p_unit_dir) const {
		float terrace_strength = 0.f;
		float plateau_strength = 0.f;
		float landform_strength[2] = { 0.f, 0.f };
		const double unshelved = eval_unshelved_offset(p_unit_dir, terrace_strength, plateau_strength, landform_strength);
		const double offset = apply_terracing(unshelved, terrace_strength) + plateau_offset(p_unit_dir, plateau_strength)
				+ landform_offset(p_unit_dir, landform_strength);
		return apply_floor(base_radius + coast_offset(p_unit_dir, offset));
	}

	double eval_unshelved_offset(const Vector3 &p_unit_dir, float &r_terrace_strength) const {
		float plateau_strength = 0.f;
		return eval_unshelved_offset(p_unit_dir, r_terrace_strength, plateau_strength);
	}

	// The height above the base radius before terracing, coast shaping and the floor, with the
	// terrace strength that applies there. This is what the shelves are cut from, so the level it
	// sits in and how far through that level it is say exactly where every riser is -- which is how
	// cliff geometry finds the edges it belongs on without searching the finished surface for walls.
	double eval_unshelved_offset(const Vector3 &p_unit_dir, float &r_terrace_strength, float &r_plateau_strength,
			float *r_landform = nullptr) const {
		// Both the sample position and the final sum are computed in double. Doing either in float32
		// quantizes the surface to the float32 ULP at this radius -- 0.25 m at 3,185,500 m -- which
		// shows up as terraced terrain with metre-scale plateaus, not as a subtle rounding error.
		PlanetHashNoise::FbmParams local_fbm;
		PlanetHashNoise::ShapeMix local_shape;
		double local_amplitude = 0.0;
		double base_offset = 0.0;
		resolve_params(p_unit_dir, local_fbm, local_shape, local_amplitude, base_offset, r_terrace_strength, r_plateau_strength, r_landform);

		const double n = double(PlanetHashNoise::fbm_3d_shaped_precise(
				p_unit_dir.x * base_radius, p_unit_dir.y * base_radius, p_unit_dir.z * base_radius,
				local_fbm, local_shape, warp, float(slope_erosion), float(local_amplitude)));
		return base_offset + local_amplitude * n;
	}

	// Batched radius evaluation over unit directions supplied as three separate arrays, the layout
	// the vectorized inner loop wants. `r_radius` is used as scratch and then overwritten with the
	// result, so it must have room for p_count floats.
	//
	// KNOWN OPEN ISSUE: the noise is sampled at `dir * base_radius`, a planet-scale coordinate, in
	// float32. A float32 ULP at 3,185,500 m is 0.25 m, so the sample lattice is distorted by up to
	// that much. It is deterministic, and the collider and the voxel generator agree because they
	// evaluate the identical expression -- but they agree by coincidence of arithmetic rather than by
	// construction. Closing that properly means both sides evaluating at shared quantized lattice
	// points.
	// Directions arrive as doubles because the sample position must be formed in double; see
	// eval_radius. `r_radius` receives the radius per direction, also in double.
	void eval_radius_batch(
			const Vector3 *p_dirs,
			double *p_scratch_x,
			double *p_scratch_y,
			double *p_scratch_z,
			float *p_noise,
			double *r_radius,
			uint32_t p_count) const {
		for (uint32_t i = 0; i < p_count; ++i) {
			p_scratch_x[i] = p_dirs[i].x * base_radius;
			p_scratch_y[i] = p_dirs[i].y * base_radius;
			p_scratch_z[i] = p_dirs[i].z * base_radius;
		}

		// Per point rather than one call for the batch: with biomes enabled the noise parameters
		// themselves vary per point. Routing through the identical scalar path is what keeps the
		// batched result bit-identical to eval_radius(), which the collider and renderer depend on.
		for (uint32_t i = 0; i < p_count; ++i) {
			PlanetHashNoise::FbmParams local_fbm;
			PlanetHashNoise::ShapeMix local_shape;
			double local_amplitude = 0.0;
			double base_offset = 0.0;
			float terrace_strength = 0.f;
			float plateau_strength = 0.f;
			float landform_strength[2] = { 0.f, 0.f };
			resolve_params(p_dirs[i], local_fbm, local_shape, local_amplitude, base_offset, terrace_strength, plateau_strength,
					landform_strength);

			p_noise[i] = PlanetHashNoise::fbm_3d_shaped_precise(
					p_scratch_x[i], p_scratch_y[i], p_scratch_z[i],
					local_fbm, local_shape, warp, float(slope_erosion), float(local_amplitude));
			const double offset = apply_terracing(base_offset + local_amplitude * double(p_noise[i]), terrace_strength)
					+ plateau_offset(p_dirs[i], plateau_strength) + landform_offset(p_dirs[i], landform_strength);
			r_radius[i] = apply_floor(base_radius + coast_offset(p_dirs[i], offset));
		}
	}

	// Upper bound on |d(perlin_3d)/dp| for one octave, per lattice unit.
	//
	// NOT a derived constant. Measured by the planet field check over 400k random points using central
	// finite differences: the observed maximum is 3.68, so this carries about 2.7x headroom.
	//
	// The distinction matters. The global bounds above are exact by construction. This one is
	// empirical, and an empirical Lipschitz constant that turns out to be too small produces missed
	// collisions rather than a visual glitch. The check re-validates it on every run; treat a failure
	// there as a correctness bug, not a flaky threshold.
	static constexpr double PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT = 10.0;

	// How much f can change per metre of surface travel, worst case, summed over all octaves.
	double get_lipschitz_per_metre() const {
		// Worst case across the biome table, not the nominal configuration: the bound has to hold
		// everywhere on the planet, and the steepest biome is the one that decides it.
		double worst_amplitude = amplitude;
		double period = double(fbm.period);
		double gain = double(fbm.gain);
		if (use_biomes()) {
			float max_amp_scale = 1.f;
			float max_freq = 1.f;
			float max_rough = 0.5f;
			float min_base = 0.f;
			float max_base = 0.f;
			biomes.extremes(max_amp_scale, max_freq, max_rough, min_base, max_base);
			worst_amplitude = amplitude * double(max_amp_scale) * (1.0 + mountain_amplitude);
			if (max_freq > 0.f) {
				period = double(fbm.period) / double(max_freq);
			}
			gain = double(max_rough);
		}

		double total = 0.0;
		double amp = 1.0;
		double norm = 0.0;
		double freq = 1.0 / period;
		for (int i = 0; i < fbm.octaves; ++i) {
			total += amp * PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT * freq;
			norm += amp;
			freq *= double(fbm.lacunarity);
			amp *= gain;
		}
		if (norm > 0.0) {
			total /= norm;
		}

		// Domain warping displaces the sample position, so the chain rule multiplies the whole field's
		// gradient by (1 + d|warp|/dp). Leaving this out would under-estimate the bound, and an
		// under-estimated Lipschitz constant produces MISSED COLLISIONS, not a cosmetic error -- see
		// the note on PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT above.
		//
		// Shaping needs no factor: standard, billow and ridged are all 1-Lipschitz in the raw signal,
		// and the three weights sum to 1. Slope weighting only attenuates octaves, and the field check
		// re-validates the whole bound empirically with these features on.
		double warp_factor = 1.0;
		if (warp.amplitude != 0.f && warp.period > 0.f) {
			warp_factor += (double(warp.amplitude) * PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT) / double(warp.period);
		}
		double result = worst_amplitude * total * warp_factor;

		// The macro height channel contributes metres of its own, and its gradient has to be in the
		// bound too. It has always been swamped by the micro noise, so leaving it out never showed --
		// until amplitude is turned down to isolate the climate map, at which point the bound collapses
		// to zero while the surface still rises and falls by hundreds of metres. A zero bound means the
		// cone margin is zero, and that means missed collisions rather than a cosmetic error.
		if (macro_map != nullptr && macro_height_scale != 0.0) {
			result += Math::abs(macro_height_scale) * macro_map->max_height_gradient_per_metre(base_radius);
		}
		// Across a riser of width w the cubic has derivative at most 1.5 / w (7.5 at a fifth of a step).
		// Blending it with the original field gives 1 + (1.5 / w - 1) * strength; use the strongest
		// biome for the safety bound.
		if (terrace_step > 0.0 && use_biomes()) {
			result *= 1.0 + (1.5 / terrace_riser - 1.0) * double(biomes.max_terrace_strength());
		}
		// A plateau wall: the cubic climbs one level across `plateau_edge` of the mask's level space, so
		// its slope is height * levels * spread / 2 * (1.5 / edge) times the mask noise's gradient,
		// whose three octaves at 1x, 4x and 16x frequency weigh 0.75, 0.2 and 0.05.
		if (plateau_height > 0.0 && plateau_levels > 0 && use_biomes()) {
			const double mask_gradient = PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT * (0.75 + 0.2 * 4.0 + 0.05 * 16.0) / plateau_period;
			result += double(biomes.max_plateau_strength()) * plateau_height * double(plateau_levels)
					* 0.5 * plateau_spread * (1.5 / plateau_edge) * mask_gradient;
		}
		if (use_biomes()) {
			result += double(biomes.max_landform_strength()) * PlanetLandform::layer_lipschitz(landforms, landforms.major, true);
			result += double(biomes.max_hill_strength()) * PlanetLandform::layer_lipschitz(landforms, landforms.minor, false);
		}
		if (use_coast()) {
			const double outline_slope = double(coast.noise_amplitude) * 2.24 * PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT / double(coast.noise_period);
			const double profile_slope = 2.0 + double(coast.cliff_height) / double(coast.cliff_width);
			const double character_slope = (double(coast.cliff_height) + 10.0) * 32.0 * PERLIN_3D_LIPSCHITZ_PER_LATTICE_UNIT / double(coast.character_period);
			result = 4.0 * ((result + outline_slope) * profile_slope + character_slope);
		}
		return result;
	}

	// Conservative radius bounds over a cone of directions, for broadphase culling and the narrow
	// phase's early-out. Samples the cone and pads by the Lipschitz bound times the worst-case gap
	// between a surface point and the nearest sample, then clamps to the exact global bounds.
	void get_bounds_for_cone(
			const Vector3 &p_cone_axis,
			double p_cone_half_angle,
			double &r_min_radius,
			double &r_max_radius) const {
		const Vector3 axis = p_cone_axis.normalized();
		if (axis.is_zero_approx()) {
			r_min_radius = get_min_radius();
			r_max_radius = get_max_radius();
			return;
		}
		if (p_cone_half_angle <= 0.0) {
			const double r = eval_radius(axis);
			r_min_radius = r;
			r_max_radius = r;
			return;
		}
		// A wide cone covers so much of the sphere that sampling it densely enough is not worth it;
		// the exact global bound is cheaper and not much looser.
		if (p_cone_half_angle >= Math::PI / 8.0) {
			r_min_radius = get_min_radius();
			r_max_radius = get_max_radius();
			return;
		}

		Vector3 u = axis.cross(Vector3(0, 0, 1));
		if (u.length_squared() < 1e-6) {
			u = axis.cross(Vector3(0, 1, 0));
		}
		u.normalize();
		const Vector3 v = axis.cross(u).normalized();

		constexpr int RINGS = 4;
		constexpr int SPOKES = 8;

		const double tan_half = PlanetDetMath::tan(p_cone_half_angle);

		double sampled_min = eval_radius(axis);
		double sampled_max = sampled_min;

		for (int ring = 1; ring <= RINGS; ++ring) {
			const double t = tan_half * (double(ring) / double(RINGS));
			for (int spoke = 0; spoke < SPOKES; ++spoke) {
				const double a = (Math::TAU * double(spoke)) / double(SPOKES);
				const Vector3 dir = (axis + u * (t * PlanetDetMath::cos(a)) + v * (t * PlanetDetMath::sin(a))).normalized();
				const double r = eval_radius(dir);
				sampled_min = MIN(sampled_min, r);
				sampled_max = MAX(sampled_max, r);
			}
		}

		// Worst-case arc distance from a surface point to the nearest sample. The polar grid is
		// coarsest at the outer ring, between adjacent spokes.
		const double cone_arc = p_cone_half_angle * base_radius;
		const double radial_gap = cone_arc / double(RINGS);
		const double tangential_gap = (Math::TAU * cone_arc) / double(SPOKES);
		const double max_gap = 0.5 * Math::sqrt(radial_gap * radial_gap + tangential_gap * tangential_gap);
		const double margin = get_lipschitz_per_metre() * max_gap;

		r_min_radius = MAX(get_min_radius(), sampled_min - margin);
		r_max_radius = MIN(get_max_radius(), sampled_max + margin);
	}
};

inline bool planet_landform_layer_equal(const PlanetLandformLayer &a, const PlanetLandformLayer &b) {
	return a.period == b.period && a.height == b.height && a.tier_step == b.tier_step && a.density == b.density &&
			a.seed == b.seed;
}

inline bool planet_field_equal(const PlanetFieldData &a, const PlanetFieldData &b) {
	if (!(a.coast == b.coast)) {
		return false;
	}
	if (a.base_radius != b.base_radius || a.amplitude != b.amplitude || a.slope_erosion != b.slope_erosion ||
			a.floor_radius != b.floor_radius || a.sea_level_radius != b.sea_level_radius ||
			a.dig_margin != b.dig_margin ||
			a.macro_height_scale != b.macro_height_scale || a.mountain_amplitude != b.mountain_amplitude ||
			a.mountain_ridge != b.mountain_ridge || a.terrace_step != b.terrace_step ||
			a.terrace_riser != b.terrace_riser || a.plateau_height != b.plateau_height ||
			a.plateau_period != b.plateau_period || a.plateau_levels != b.plateau_levels ||
			a.plateau_edge != b.plateau_edge || a.plateau_spread != b.plateau_spread ||
			a.plateau_seed != b.plateau_seed ||
			!planet_landform_layer_equal(a.landforms.major, b.landforms.major) ||
			!planet_landform_layer_equal(a.landforms.minor, b.landforms.minor) ||
			a.landforms.warp != b.landforms.warp || a.landforms.riser != b.landforms.riser) {
		return false;
	}
	if (a.fbm.seed != b.fbm.seed || a.fbm.period != b.fbm.period || a.fbm.octaves != b.fbm.octaves ||
			a.fbm.lacunarity != b.fbm.lacunarity || a.fbm.gain != b.fbm.gain) {
		return false;
	}
	if (a.shape.standard != b.shape.standard || a.shape.billow != b.shape.billow || a.shape.ridged != b.shape.ridged) {
		return false;
	}
	if (a.warp.amplitude != b.warp.amplitude || a.warp.period != b.warp.period || a.warp.seed != b.warp.seed) {
		return false;
	}
	if (a.macro_map != b.macro_map || a.biomes.count != b.biomes.count ||
			a.biomes.kernel_radius != b.biomes.kernel_radius) {
		return false;
	}
	for (int i = 0; i < a.biomes.count; ++i) {
		const PlanetBiomeProfileData &x = a.biomes.entries[i];
		const PlanetBiomeProfileData &y = b.biomes.entries[i];
		if (x.anchor_temperature != y.anchor_temperature || x.anchor_moisture != y.anchor_moisture ||
				x.base_elevation != y.base_elevation || x.amplitude_scale != y.amplitude_scale ||
				x.frequency_scale != y.frequency_scale || x.roughness != y.roughness ||
				x.shape.standard != y.shape.standard || x.shape.billow != y.shape.billow ||
				x.shape.ridged != y.shape.ridged || x.terrace_strength != y.terrace_strength ||
				x.plateau_strength != y.plateau_strength || x.landform_strength != y.landform_strength ||
				x.hill_strength != y.hill_strength) {
			return false;
		}
	}
	return true;
}
