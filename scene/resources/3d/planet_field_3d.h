/**************************************************************************/
/*  planet_field_3d.h                                                     */
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

#include "core/io/resource.h"
#include "core/variant/typed_array.h"
#include "core/math/planet_field.h"
#include "scene/resources/3d/planet_biome_3d.h"

// Resource wrapper around PlanetFieldData. The data itself lives in core/math/planet_field.h so that
// the Jolt collision shape can use it on the physics thread without a physics backend depending on
// scene/. Consumers take a copy of get_data(); the copy is immutable and therefore thread-safe.
class PlanetField3D : public Resource {
	GDCLASS(PlanetField3D, Resource);

	PlanetFieldData data;

	// Kept as Refs as well as inside `data` so the inspector has something to edit and so a re-bake or
	// a biome tweak can be picked up: both emit `changed`, which refreshes the plain-data copy the
	// collider actually reads.
	Ref<PlanetMacroMap3D> macro_map;
	TypedArray<PlanetBiomeProfile3D> biome_profiles;

	void _rebuild_macro_map();
	void _rebuild_biomes();

protected:
	static void _bind_methods();

public:
	const PlanetFieldData &get_data() const { return data; }

	void set_base_radius(real_t p_radius);
	real_t get_base_radius() const;
	void set_amplitude(real_t p_amplitude);
	real_t get_amplitude() const;
	void set_seed(int p_seed);
	int get_seed() const;
	void set_period(real_t p_period);
	real_t get_period() const;
	void set_octaves(int p_octaves);
	int get_octaves() const;
	void set_lacunarity(real_t p_lacunarity);
	real_t get_lacunarity() const;
	void set_gain(real_t p_gain);
	real_t get_gain() const;

	void set_macro_map(const Ref<PlanetMacroMap3D> &p_map);
	Ref<PlanetMacroMap3D> get_macro_map() const;
	void set_biome_profiles(const TypedArray<PlanetBiomeProfile3D> &p_profiles);
	TypedArray<PlanetBiomeProfile3D> get_biome_profiles() const;
	void set_macro_height_scale(real_t p_v);
	real_t get_macro_height_scale() const;
	void set_mountain_amplitude(real_t p_v);
	real_t get_mountain_amplitude() const;
	void set_mountain_ridge(real_t p_v);
	real_t get_mountain_ridge() const;
	void set_terrace_step(real_t p_v);
	real_t get_terrace_step() const;
	void set_biome_kernel_radius(real_t p_v);
	real_t get_biome_kernel_radius() const;

	// Fills the biome list with the built-in nine-biome climate grid, as editable resources. A
	// starting point to tune rather than a hidden default, so what the planet does is always visible
	// in the inspector.
	void use_default_biomes();
	bool has_biomes() const { return data.use_biomes(); }
	Vector3 get_blended_params(const Vector3 &p_unit_dir) const;
	// The biome blend at a point in CLIMATE space rather than on the sphere, so it can be baked
	// into a lookup table for the GPU. Returns [base_elevation, amplitude_scale, frequency_scale,
	// roughness, standard, billow, ridged, terrace_strength].
	PackedFloat32Array resolve_biome_at(real_t p_temperature, real_t p_moisture) const;

	void set_shape_standard(real_t p_value);
	real_t get_shape_standard() const;
	void set_shape_billow(real_t p_value);
	real_t get_shape_billow() const;
	void set_shape_ridged(real_t p_value);
	real_t get_shape_ridged() const;
	void set_warp_amplitude(real_t p_value);
	real_t get_warp_amplitude() const;
	void set_warp_period(real_t p_value);
	real_t get_warp_period() const;
	void set_warp_seed(int p_value);
	int get_warp_seed() const;
	void set_slope_erosion(real_t p_value);
	real_t get_slope_erosion() const;
	void set_floor_radius(real_t p_value);
	real_t get_floor_radius() const;
	void set_sea_level_radius(real_t p_value);
	real_t get_sea_level_radius() const;

	real_t eval_radius(const Vector3 &p_unit_dir) const { return data.eval_radius(p_unit_dir); }

	// Raw fbm at a 3D position, the quantity the GLSL twin computes. Exposed so the shader can be
	// checked against the CPU path sample for sample rather than by eye.
	real_t eval_noise_at(const Vector3 &p_pos) const {
		return real_t(PlanetHashNoise::fbm_3d(float(p_pos.x), float(p_pos.y), float(p_pos.z), data.fbm));
	}

	// The GLSL twin of core/math/planet_hash_noise.h, for the renderer and for verification. This is
	// the same text the voxel graph shader generator emits, so there is exactly one GPU implementation
	// of the field rather than one per consumer.
	static String get_noise_glsl();
	bool has_water() const { return data.has_water(); }
	real_t sea_depth(const Vector3 &p_unit_dir) const { return data.sea_depth(p_unit_dir); }
	bool is_submerged(const Vector3 &p_unit_dir) const { return data.is_submerged(p_unit_dir); }
	bool is_point_underwater(const Vector3 &p_point) const { return data.is_point_underwater(p_point); }
	real_t dig_floor_radius(const Vector3 &p_unit_dir) const { return data.dig_floor_radius(p_unit_dir); }
	bool can_dig(const Vector3 &p_unit_dir) const { return data.can_dig(p_unit_dir); }
	real_t distance_to_coast(const Vector3 &p_unit_dir, real_t p_max_search) const { return data.distance_to_coast(p_unit_dir, p_max_search); }
	void set_dig_margin(real_t p_v);
	real_t get_dig_margin() const;
	real_t get_min_radius() const { return data.get_min_radius(); }
	real_t get_max_radius() const { return data.get_max_radius(); }


	Vector3 sample_coast(const Vector3 &p_unit_dir) const { return data.sample_coast(p_unit_dir); }
	void set_coast_enabled(bool p_value);
	bool get_coast_enabled() const;
	void set_coast_noise_period(real_t p_value);
	real_t get_coast_noise_period() const;
	void set_coast_noise_amplitude(real_t p_value);
	real_t get_coast_noise_amplitude() const;
	void set_coast_character_period(real_t p_value);
	real_t get_coast_character_period() const;
	void set_coast_cliff_height(real_t p_value);
	real_t get_coast_cliff_height() const;
	void set_coast_cliff_width(real_t p_value);
	real_t get_coast_cliff_width() const;
	void set_coast_seed(int p_value);
	int get_coast_seed() const;

	PlanetField3D() {}
};
