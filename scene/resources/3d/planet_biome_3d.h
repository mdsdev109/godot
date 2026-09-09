/**************************************************************************/
/*  planet_biome_3d.h                                                     */
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

#include "core/io/image.h"
#include "core/math/planet_biome.h"
#include "core/io/resource.h"

// One biome, editable in the inspector. Deliberately nothing but numbers -- no behaviour, no
// per-biome code path -- so that adding a biome never adds a branch to the evaluation loop.
class PlanetBiomeProfile3D : public Resource {
	GDCLASS(PlanetBiomeProfile3D, Resource);

	PlanetBiomeProfileData data;

protected:
	static void _bind_methods();

public:
	const PlanetBiomeProfileData &get_data() const { return data; }

	void set_anchor_temperature(real_t p_v);
	real_t get_anchor_temperature() const;
	void set_anchor_moisture(real_t p_v);
	real_t get_anchor_moisture() const;
	void set_base_elevation(real_t p_v);
	real_t get_base_elevation() const;
	void set_amplitude_scale(real_t p_v);
	real_t get_amplitude_scale() const;
	void set_frequency_scale(real_t p_v);
	real_t get_frequency_scale() const;
	void set_roughness(real_t p_v);
	real_t get_roughness() const;
	void set_terrace_strength(real_t p_v);
	real_t get_terrace_strength() const;
	void set_shape_standard(real_t p_v);
	real_t get_shape_standard() const;
	void set_shape_billow(real_t p_v);
	real_t get_shape_billow() const;
	void set_shape_ridged(real_t p_v);
	real_t get_shape_ridged() const;
	void set_ground_material(int p_v);
	int get_ground_material() const;
	void set_cliff_material(int p_v);
	int get_cliff_material() const;

	PlanetBiomeProfile3D() {}
};

// The macro climate map: a cubemap of height, temperature, moisture and weirdness indexed directly
// by surface direction.
//
// Three ways in, one way out. Bake it procedurally, import six images, or bake then export, paint and
// re-import -- everything downstream only ever sees the sampled values, so it cannot tell which route
// produced them.
class PlanetMacroMap3D : public Resource {
	GDCLASS(PlanetMacroMap3D, Resource);

	// Held by shared_ptr and replaced wholesale rather than mutated, because collision shapes on the
	// physics threads hold references to it. Editing produces a new map; running queries keep reading
	// the old one until they finish.
	std::shared_ptr<PlanetMacroMapData> data;
	PlanetMacroBakeParams bake_params;
	int resolution = 1024;
	double bake_radius = 3185500.0;
	// Identity in PlanetMacroMapRegistry, so collision shapes can find this map by value.
	uint64_t registry_id = 0;

	void _republish();

protected:
	static void _bind_methods();

public:
	PlanetMacroMapRef get_data() const { return data; }
	bool is_baked() const { return data != nullptr && data->is_valid(); }

	void set_resolution(int p_resolution);
	int get_resolution() const;
	void set_bake_radius(real_t p_radius);
	real_t get_bake_radius() const;
	void set_seed(int p_seed);
	int get_seed() const;
	void set_continent_period(real_t p_v);
	real_t get_continent_period() const;
	void set_continent_octaves(int p_v);
	int get_continent_octaves() const;
	void set_polar_weight(real_t p_v);
	real_t get_polar_weight() const;
	void set_polar_falloff(real_t p_v);
	real_t get_polar_falloff() const;
	void set_temperature_scale(real_t p_v);
	real_t get_temperature_scale() const;
	void set_moisture_temperature_weight(real_t p_v);
	real_t get_moisture_temperature_weight() const;
	void set_lapse_rate(real_t p_v);
	real_t get_lapse_rate() const;
	void set_moisture_period(real_t p_v);
	real_t get_moisture_period() const;
	void set_moisture_octaves(int p_v);
	int get_moisture_octaves() const;
	void set_temperature_period(real_t p_v);
	real_t get_temperature_period() const;
	void set_temperature_octaves(int p_v);
	int get_temperature_octaves() const;
	void set_climate_gain(real_t p_v);
	real_t get_climate_gain() const;
	void set_normalize_channels(bool p_v);
	bool get_normalize_channels() const;
	void set_weirdness_period(real_t p_v);
	real_t get_weirdness_period() const;
	void set_weirdness_octaves(int p_v);
	int get_weirdness_octaves() const;

	// Fills all six faces from the bake parameters.
	void bake();

	// Manual authoring. Channels map R=height, G=temperature, B=moisture, A=weirdness.
	Error set_face_image(int p_face, const Ref<Image> &p_image);
	Ref<Image> get_face_image(int p_face) const;
	// The same data at FULL float precision, for consumers that cannot afford half-float rounding --
	// notably a GPU that multiplies the climate through a biome frequency scale.
	Ref<Image> get_face_image_float(int p_face) const;

	// Sampling, exposed so tools and tests can read the map without going through a planet.
	Color sample_direction(const Vector3 &p_dir) const;

	// Registry id, packed into shape data so the physics backend can resolve this map.
	int64_t get_registry_id() const { return int64_t(registry_id); }

	PlanetMacroMap3D() {}
	~PlanetMacroMap3D();
};
