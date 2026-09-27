// Coastal terrain shaping shared by scalar and batched planet field evaluation.
// Copyright (c) 2014-present Godot Engine contributors. SPDX-License-Identifier: MIT
#pragma once

#include "core/math/planet_det_math.h"
#include "core/math/planet_hash_noise.h"
#include "core/math/vector3.h"

struct PlanetCoastParams {
	bool enabled = false;
	float noise_period = 32768.0f;
	float noise_amplitude = 220.0f;
	float character_period = 131072.0f;
	float cliff_height = 160.0f;
	float cliff_width = 4.0f;
	uint32_t seed = 7319u;

	static float smooth(float a, float b, float x) {
		float t = CLAMP((x - a) / (b - a), 0.f, 1.f);
		return t * t * (3.f - 2.f * t);
	}

	// The two outline bands and the independent geology field are always evaluated at fixed
	// wavelengths, regardless of the biome or render LOD. Coordinates retain the CPU's doubles.
	Vector3 sample(const Vector3 &p, float macro_above_sea) const {
		using namespace PlanetHashNoise;
		const double inv = 1.0 / double(noise_period);
		const float broad = perlin_3d_precise(p.x * inv, p.y * inv, p.z * inv, seed);
		const float bays = perlin_3d_precise(p.x * inv * 32.0, p.y * inv * 32.0, p.z * inv * 32.0, seed + 1u);
		const double ci = 1.0 / double(character_period);
		const float geology = perlin_3d_precise(p.x * ci, p.y * ci, p.z * ci, seed + 2u);
		const float control = macro_above_sea + noise_amplitude * (0.96f * broad + 0.04f * bays);
		const float character = smooth(-0.3f, 0.3f, geology);
		return Vector3(control, character, profile(control, character));
	}

	float profile(float control, float character) const {
		const float beach = 1.f - smooth(0.2f, 0.55f, character);
		const float cliff = smooth(0.48f, 0.8f, character);
		const float shoulder = 0.75f * control * float(PlanetDetMath::exp(double(-std::abs(control) / 35.f)));
		const float wall = control / std::sqrt(control * control + cliff_width * cliff_width);
		return control - beach * shoulder + cliff * cliff_height * wall;
	}

	// An identity region preserves inland detail. The soft limit has a continuous derivative
	// and bounds biome relief to 85% of the coastal profile, preserving its land/ocean sign.
	static float add_relief(float profile_height, float relief) {
		const float budget = 0.85f * std::abs(profile_height);
		if (budget < 1e-6f) {
			return profile_height;
		}
		const float r = std::abs(relief);
		// Biome elevation returns gradually inland. Without this envelope its large signed
		// bias consumes almost the whole relief budget and flattens a beach into the water plane.
		const float influence = smooth(0.f, 150.f, std::abs(profile_height));
		if (r <= 0.75f * budget) {
			return profile_height + relief * influence;
		}
		const float q = 4.f * r / budget - 2.f;
		const float limited = budget * (1.f - 0.25f / q);
		return profile_height + (relief < 0.f ? -limited : limited) * influence;
	}

	bool operator==(const PlanetCoastParams &b) const {
		return enabled == b.enabled && noise_period == b.noise_period && noise_amplitude == b.noise_amplitude &&
				character_period == b.character_period && cliff_height == b.cliff_height && cliff_width == b.cliff_width && seed == b.seed;
	}
};
