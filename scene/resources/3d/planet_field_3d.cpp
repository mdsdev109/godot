/**************************************************************************/
/*  planet_field_3d.cpp                                                   */
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

#include "core/object/callable_mp.h"
#include "planet_field_3d.h"

#include "core/math/planet_hash_noise_shader.h"
#include "core/object/class_db.h"

void PlanetField3D::set_base_radius(real_t p_radius) {
	ERR_FAIL_COND_MSG(p_radius <= 0, "PlanetField3D base radius must be greater than zero.");
	data.base_radius = p_radius;
	emit_changed();
}

real_t PlanetField3D::get_base_radius() const {
	return data.base_radius;
}

void PlanetField3D::set_amplitude(real_t p_amplitude) {
	ERR_FAIL_COND_MSG(p_amplitude < 0, "PlanetField3D amplitude cannot be negative.");
	ERR_FAIL_COND_MSG(p_amplitude >= data.base_radius, "PlanetField3D amplitude must be less than the base radius, otherwise the surface can reach or pass through the planet centre.");
	data.amplitude = p_amplitude;
	emit_changed();
}

real_t PlanetField3D::get_amplitude() const {
	return data.amplitude;
}

void PlanetField3D::set_seed(int p_seed) {
	data.fbm.seed = uint32_t(p_seed);
	emit_changed();
}

int PlanetField3D::get_seed() const {
	return int(data.fbm.seed);
}

void PlanetField3D::set_period(real_t p_period) {
	ERR_FAIL_COND_MSG(p_period <= 0, "PlanetField3D period must be greater than zero.");
	data.fbm.period = float(p_period);
	emit_changed();
}

real_t PlanetField3D::get_period() const {
	return data.fbm.period;
}

void PlanetField3D::set_octaves(int p_octaves) {
	ERR_FAIL_COND_MSG(p_octaves < 1 || p_octaves > 32, "PlanetField3D octaves must be between 1 and 32.");
	data.fbm.octaves = p_octaves;
	emit_changed();
}

int PlanetField3D::get_octaves() const {
	return data.fbm.octaves;
}

void PlanetField3D::set_lacunarity(real_t p_lacunarity) {
	ERR_FAIL_COND_MSG(p_lacunarity < 1.0, "PlanetField3D lacunarity must be at least 1.");
	data.fbm.lacunarity = float(p_lacunarity);
	emit_changed();
}

real_t PlanetField3D::get_lacunarity() const {
	return data.fbm.lacunarity;
}

void PlanetField3D::set_gain(real_t p_gain) {
	ERR_FAIL_COND_MSG(p_gain < 0.0 || p_gain > 1.0, "PlanetField3D gain must be between 0 and 1.");
	data.fbm.gain = float(p_gain);
	emit_changed();
}

real_t PlanetField3D::get_gain() const {
	return data.fbm.gain;
}

String PlanetField3D::get_noise_glsl() {
	return String::utf8(g_planet_hash_noise_shader);
}

void PlanetField3D::set_shape_standard(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D shape_standard cannot be negative.");
	data.shape.standard = p_value;
	emit_changed();
}
real_t PlanetField3D::get_shape_standard() const {
	return data.shape.standard;
}

void PlanetField3D::set_shape_billow(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D shape_billow cannot be negative.");
	data.shape.billow = p_value;
	emit_changed();
}
real_t PlanetField3D::get_shape_billow() const {
	return data.shape.billow;
}

void PlanetField3D::set_shape_ridged(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D shape_ridged cannot be negative.");
	data.shape.ridged = p_value;
	emit_changed();
}
real_t PlanetField3D::get_shape_ridged() const {
	return data.shape.ridged;
}

void PlanetField3D::set_warp_amplitude(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D warp_amplitude cannot be negative.");
	data.warp.amplitude = p_value;
	emit_changed();
}
real_t PlanetField3D::get_warp_amplitude() const {
	return data.warp.amplitude;
}

void PlanetField3D::set_warp_period(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value <= 0, "PlanetField3D warp_period must be greater than zero.");
	data.warp.period = p_value;
	emit_changed();
}
real_t PlanetField3D::get_warp_period() const {
	return data.warp.period;
}

void PlanetField3D::set_warp_seed(int p_value) {
	data.warp.seed = uint32_t(p_value);
	emit_changed();
}
int PlanetField3D::get_warp_seed() const {
	return int(data.warp.seed);
}

void PlanetField3D::set_slope_erosion(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D slope_erosion cannot be negative.");
	data.slope_erosion = p_value;
	emit_changed();
}
real_t PlanetField3D::get_slope_erosion() const {
	return data.slope_erosion;
}

void PlanetField3D::set_floor_radius(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0, "PlanetField3D floor_radius cannot be negative.");
	data.floor_radius = p_value;
	emit_changed();
}
real_t PlanetField3D::get_floor_radius() const {
	return data.floor_radius;
}

void PlanetField3D::set_sea_level_radius(real_t p_value) {
	data.sea_level_radius = p_value;
	emit_changed();
}
real_t PlanetField3D::get_sea_level_radius() const {
	return data.sea_level_radius;
}


void PlanetField3D::_rebuild_macro_map() {
	data.macro_map = macro_map.is_valid() ? macro_map->get_data() : PlanetMacroMapRef();
	emit_changed();
}

void PlanetField3D::_rebuild_biomes() {
	data.biomes.count = 0;
	const int n = MIN(biome_profiles.size(), PlanetBiomeTable::MAX_BIOMES);
	for (int i = 0; i < n; ++i) {
		Ref<PlanetBiomeProfile3D> profile = biome_profiles[i];
		if (profile.is_null()) {
			continue;
		}
		data.biomes.entries[data.biomes.count] = profile->get_data();
		++data.biomes.count;
	}
	emit_changed();
}

void PlanetField3D::set_macro_map(const Ref<PlanetMacroMap3D> &p_map) {
	if (macro_map == p_map) {
		return;
	}
	if (macro_map.is_valid() && macro_map->is_connected("changed", callable_mp(this, &PlanetField3D::_rebuild_macro_map))) {
		macro_map->disconnect("changed", callable_mp(this, &PlanetField3D::_rebuild_macro_map));
	}
	macro_map = p_map;
	if (macro_map.is_valid()) {
		macro_map->connect("changed", callable_mp(this, &PlanetField3D::_rebuild_macro_map));
	}
	_rebuild_macro_map();
}
Ref<PlanetMacroMap3D> PlanetField3D::get_macro_map() const {
	return macro_map;
}

void PlanetField3D::set_biome_profiles(const TypedArray<PlanetBiomeProfile3D> &p_profiles) {
	for (int i = 0; i < biome_profiles.size(); ++i) {
		Ref<PlanetBiomeProfile3D> old_profile = biome_profiles[i];
		if (old_profile.is_valid() && old_profile->is_connected("changed", callable_mp(this, &PlanetField3D::_rebuild_biomes))) {
			old_profile->disconnect("changed", callable_mp(this, &PlanetField3D::_rebuild_biomes));
		}
	}
	biome_profiles = p_profiles;
	for (int i = 0; i < biome_profiles.size(); ++i) {
		Ref<PlanetBiomeProfile3D> profile = biome_profiles[i];
		if (profile.is_valid() && !profile->is_connected("changed", callable_mp(this, &PlanetField3D::_rebuild_biomes))) {
			profile->connect("changed", callable_mp(this, &PlanetField3D::_rebuild_biomes));
		}
	}
	_rebuild_biomes();
}
TypedArray<PlanetBiomeProfile3D> PlanetField3D::get_biome_profiles() const {
	return biome_profiles;
}

void PlanetField3D::set_macro_height_scale(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v < 0, "PlanetField3D macro_height_scale cannot be negative.");
	data.macro_height_scale = p_v;
	emit_changed();
}
real_t PlanetField3D::get_macro_height_scale() const { return data.macro_height_scale; }

void PlanetField3D::set_mountain_amplitude(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v < 0, "PlanetField3D mountain_amplitude cannot be negative.");
	data.mountain_amplitude = p_v;
	emit_changed();
}
real_t PlanetField3D::get_mountain_amplitude() const { return data.mountain_amplitude; }

void PlanetField3D::set_mountain_ridge(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v < 0, "PlanetField3D mountain_ridge cannot be negative.");
	data.mountain_ridge = p_v;
	emit_changed();
}
real_t PlanetField3D::get_mountain_ridge() const { return data.mountain_ridge; }

void PlanetField3D::set_terrace_step(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v < 0, "PlanetField3D terrace_step cannot be negative.");
	data.terrace_step = p_v;
	emit_changed();
}
real_t PlanetField3D::get_terrace_step() const { return data.terrace_step; }

void PlanetField3D::set_biome_kernel_radius(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v <= 0, "PlanetField3D biome_kernel_radius must be greater than zero.");
	data.biomes.kernel_radius = float(p_v);
	emit_changed();
}
real_t PlanetField3D::get_biome_kernel_radius() const { return data.biomes.kernel_radius; }

void PlanetField3D::use_default_biomes() {
	const PlanetBiomeTable defaults = PlanetBiomeTable::make_default();
	TypedArray<PlanetBiomeProfile3D> list;
	for (int i = 0; i < defaults.count; ++i) {
		Ref<PlanetBiomeProfile3D> profile;
		profile.instantiate();
		const PlanetBiomeProfileData &e = defaults.entries[i];
		profile->set_anchor_temperature(e.anchor_temperature);
		profile->set_anchor_moisture(e.anchor_moisture);
		profile->set_base_elevation(e.base_elevation);
		profile->set_amplitude_scale(e.amplitude_scale);
		profile->set_frequency_scale(e.frequency_scale);
		profile->set_roughness(e.roughness);
		profile->set_terrace_strength(e.terrace_strength);
		profile->set_shape_standard(e.shape.standard);
		profile->set_shape_billow(e.shape.billow);
		profile->set_shape_ridged(e.shape.ridged);
		// Field by field, which means every member added to PlanetBiomeProfileData has to be added
		// here too or it silently keeps the resource's own default. Adding the two material indices
		// and forgetting this line made every biome on the planet claim to be bare rock.
		profile->set_ground_material(e.ground_material);
		profile->set_cliff_material(e.cliff_material);
		list.push_back(profile);
	}
	set_biome_profiles(list);
}

// Blended amplitude scale, frequency scale and ridged weight at a direction. Exposed for tuning and
// for the demo's HUD -- being able to see which biome you are standing in is most of what makes the
// table tunable at all.
Vector3 PlanetField3D::get_blended_params(const Vector3 &p_unit_dir) const {
	PlanetHashNoise::FbmParams f;
	PlanetHashNoise::ShapeMix sh;
	double amp = 0.0;
	double off = 0.0;
	Vector3 d = p_unit_dir;
	if (d.length_squared() <= 0) {
		return Vector3();
	}
	d.normalize();
	float terrace_strength = 0.f;
	data.resolve_params(d, f, sh, amp, off, terrace_strength);
	const double amp_scale = data.amplitude > 0.0 ? amp / data.amplitude : 0.0;
	const double freq_scale = f.period > 0.f ? double(data.fbm.period) / double(f.period) : 0.0;
	return Vector3(amp_scale, freq_scale, sh.ridged);
}

PackedFloat32Array PlanetField3D::resolve_biome_at(real_t p_temperature, real_t p_moisture) const {
	PlanetBlendedParams b;
	PackedFloat32Array out;
	out.resize(8);
	if (!data.use_biomes()) {
		// No table: the master parameters, unscaled, so the caller needs no special case.
		out.set(0, 0.f);
		out.set(1, 1.f);
		out.set(2, 1.f);
		out.set(3, data.fbm.gain);
		out.set(4, data.shape.standard);
		out.set(5, data.shape.billow);
		out.set(6, data.shape.ridged);
		out.set(7, 0.f);
		return out;
	}
	data.biomes.blend(float(p_temperature), float(p_moisture), b);
	out.set(0, b.base_elevation);
	out.set(1, b.amplitude_scale);
	out.set(2, b.frequency_scale);
	out.set(3, b.roughness);
	out.set(4, b.shape.standard);
	out.set(5, b.shape.billow);
	out.set(6, b.shape.ridged);
	out.set(7, b.terrace_strength);
	return out;
}

void PlanetField3D::set_dig_margin(real_t p_v) {
	ERR_FAIL_COND_MSG(p_v < 0, "PlanetField3D dig_margin cannot be negative.");
	data.dig_margin = p_v;
	emit_changed();
}
real_t PlanetField3D::get_dig_margin() const { return data.dig_margin; }

void PlanetField3D::set_coast_enabled(bool p_value) {
	data.coast.enabled = p_value;
	emit_changed();
}
bool PlanetField3D::get_coast_enabled() const { return data.coast.enabled; }

void PlanetField3D::set_coast_noise_period(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value <= 0 || !Math::is_finite(p_value), "Invalid coast noise_period.");
	data.coast.noise_period = p_value;
	emit_changed();
}
real_t PlanetField3D::get_coast_noise_period() const { return data.coast.noise_period; }

void PlanetField3D::set_coast_noise_amplitude(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0 || !Math::is_finite(p_value), "Invalid coast noise_amplitude.");
	data.coast.noise_amplitude = p_value;
	emit_changed();
}
real_t PlanetField3D::get_coast_noise_amplitude() const { return data.coast.noise_amplitude; }

void PlanetField3D::set_coast_character_period(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value <= 0 || !Math::is_finite(p_value), "Invalid coast character_period.");
	data.coast.character_period = p_value;
	emit_changed();
}
real_t PlanetField3D::get_coast_character_period() const { return data.coast.character_period; }

void PlanetField3D::set_coast_cliff_height(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value < 0 || !Math::is_finite(p_value), "Invalid coast cliff_height.");
	data.coast.cliff_height = p_value;
	emit_changed();
}
real_t PlanetField3D::get_coast_cliff_height() const { return data.coast.cliff_height; }

void PlanetField3D::set_coast_cliff_width(real_t p_value) {
	ERR_FAIL_COND_MSG(p_value <= 0 || !Math::is_finite(p_value), "Invalid coast cliff_width.");
	data.coast.cliff_width = p_value;
	emit_changed();
}
real_t PlanetField3D::get_coast_cliff_width() const { return data.coast.cliff_width; }

void PlanetField3D::set_coast_seed(int p_value) {
	data.coast.seed = p_value;
	emit_changed();
}
int PlanetField3D::get_coast_seed() const { return data.coast.seed; }

void PlanetField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_coast_enabled", "value"), &PlanetField3D::set_coast_enabled);
	ClassDB::bind_method(D_METHOD("get_coast_enabled"), &PlanetField3D::get_coast_enabled);
	ClassDB::bind_method(D_METHOD("set_coast_noise_period", "value"), &PlanetField3D::set_coast_noise_period);
	ClassDB::bind_method(D_METHOD("get_coast_noise_period"), &PlanetField3D::get_coast_noise_period);
	ClassDB::bind_method(D_METHOD("set_coast_noise_amplitude", "value"), &PlanetField3D::set_coast_noise_amplitude);
	ClassDB::bind_method(D_METHOD("get_coast_noise_amplitude"), &PlanetField3D::get_coast_noise_amplitude);
	ClassDB::bind_method(D_METHOD("set_coast_character_period", "value"), &PlanetField3D::set_coast_character_period);
	ClassDB::bind_method(D_METHOD("get_coast_character_period"), &PlanetField3D::get_coast_character_period);
	ClassDB::bind_method(D_METHOD("set_coast_cliff_height", "value"), &PlanetField3D::set_coast_cliff_height);
	ClassDB::bind_method(D_METHOD("get_coast_cliff_height"), &PlanetField3D::get_coast_cliff_height);
	ClassDB::bind_method(D_METHOD("set_coast_cliff_width", "value"), &PlanetField3D::set_coast_cliff_width);
	ClassDB::bind_method(D_METHOD("get_coast_cliff_width"), &PlanetField3D::get_coast_cliff_width);
	ClassDB::bind_method(D_METHOD("set_coast_seed", "value"), &PlanetField3D::set_coast_seed);
	ClassDB::bind_method(D_METHOD("get_coast_seed"), &PlanetField3D::get_coast_seed);
	ClassDB::bind_method(D_METHOD("sample_coast", "unit_dir"), &PlanetField3D::sample_coast);

	ClassDB::bind_method(D_METHOD("set_base_radius", "radius"), &PlanetField3D::set_base_radius);
	ClassDB::bind_method(D_METHOD("get_base_radius"), &PlanetField3D::get_base_radius);
	ClassDB::bind_method(D_METHOD("set_amplitude", "amplitude"), &PlanetField3D::set_amplitude);
	ClassDB::bind_method(D_METHOD("get_amplitude"), &PlanetField3D::get_amplitude);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &PlanetField3D::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &PlanetField3D::get_seed);
	ClassDB::bind_method(D_METHOD("set_period", "period"), &PlanetField3D::set_period);
	ClassDB::bind_method(D_METHOD("get_period"), &PlanetField3D::get_period);
	ClassDB::bind_method(D_METHOD("set_octaves", "octaves"), &PlanetField3D::set_octaves);
	ClassDB::bind_method(D_METHOD("get_octaves"), &PlanetField3D::get_octaves);
	ClassDB::bind_method(D_METHOD("set_lacunarity", "lacunarity"), &PlanetField3D::set_lacunarity);
	ClassDB::bind_method(D_METHOD("get_lacunarity"), &PlanetField3D::get_lacunarity);
	ClassDB::bind_method(D_METHOD("set_gain", "gain"), &PlanetField3D::set_gain);
	ClassDB::bind_method(D_METHOD("get_gain"), &PlanetField3D::get_gain);
	ClassDB::bind_method(D_METHOD("set_shape_standard", "value"), &PlanetField3D::set_shape_standard);
	ClassDB::bind_method(D_METHOD("get_shape_standard"), &PlanetField3D::get_shape_standard);
	ClassDB::bind_method(D_METHOD("set_shape_billow", "value"), &PlanetField3D::set_shape_billow);
	ClassDB::bind_method(D_METHOD("get_shape_billow"), &PlanetField3D::get_shape_billow);
	ClassDB::bind_method(D_METHOD("set_shape_ridged", "value"), &PlanetField3D::set_shape_ridged);
	ClassDB::bind_method(D_METHOD("get_shape_ridged"), &PlanetField3D::get_shape_ridged);
	ClassDB::bind_method(D_METHOD("set_warp_amplitude", "value"), &PlanetField3D::set_warp_amplitude);
	ClassDB::bind_method(D_METHOD("get_warp_amplitude"), &PlanetField3D::get_warp_amplitude);
	ClassDB::bind_method(D_METHOD("set_warp_period", "value"), &PlanetField3D::set_warp_period);
	ClassDB::bind_method(D_METHOD("get_warp_period"), &PlanetField3D::get_warp_period);
	ClassDB::bind_method(D_METHOD("set_warp_seed", "value"), &PlanetField3D::set_warp_seed);
	ClassDB::bind_method(D_METHOD("get_warp_seed"), &PlanetField3D::get_warp_seed);
	ClassDB::bind_method(D_METHOD("set_slope_erosion", "value"), &PlanetField3D::set_slope_erosion);
	ClassDB::bind_method(D_METHOD("get_slope_erosion"), &PlanetField3D::get_slope_erosion);
	ClassDB::bind_method(D_METHOD("set_floor_radius", "value"), &PlanetField3D::set_floor_radius);
	ClassDB::bind_method(D_METHOD("get_floor_radius"), &PlanetField3D::get_floor_radius);
	ClassDB::bind_method(D_METHOD("set_sea_level_radius", "value"), &PlanetField3D::set_sea_level_radius);
	ClassDB::bind_method(D_METHOD("get_sea_level_radius"), &PlanetField3D::get_sea_level_radius);

	ClassDB::bind_method(D_METHOD("eval_radius", "unit_dir"), &PlanetField3D::eval_radius);
	ClassDB::bind_method(D_METHOD("eval_noise_at", "position"), &PlanetField3D::eval_noise_at);
	ClassDB::bind_static_method("PlanetField3D", D_METHOD("get_noise_glsl"), &PlanetField3D::get_noise_glsl);
	ClassDB::bind_method(D_METHOD("set_macro_map", "map"), &PlanetField3D::set_macro_map);
	ClassDB::bind_method(D_METHOD("get_macro_map"), &PlanetField3D::get_macro_map);
	ClassDB::bind_method(D_METHOD("set_biome_profiles", "profiles"), &PlanetField3D::set_biome_profiles);
	ClassDB::bind_method(D_METHOD("get_biome_profiles"), &PlanetField3D::get_biome_profiles);
	ClassDB::bind_method(D_METHOD("set_macro_height_scale", "value"), &PlanetField3D::set_macro_height_scale);
	ClassDB::bind_method(D_METHOD("get_macro_height_scale"), &PlanetField3D::get_macro_height_scale);
	ClassDB::bind_method(D_METHOD("set_mountain_amplitude", "value"), &PlanetField3D::set_mountain_amplitude);
	ClassDB::bind_method(D_METHOD("get_mountain_amplitude"), &PlanetField3D::get_mountain_amplitude);
	ClassDB::bind_method(D_METHOD("set_mountain_ridge", "value"), &PlanetField3D::set_mountain_ridge);
	ClassDB::bind_method(D_METHOD("get_mountain_ridge"), &PlanetField3D::get_mountain_ridge);
	ClassDB::bind_method(D_METHOD("set_terrace_step", "value"), &PlanetField3D::set_terrace_step);
	ClassDB::bind_method(D_METHOD("get_terrace_step"), &PlanetField3D::get_terrace_step);
	ClassDB::bind_method(D_METHOD("set_biome_kernel_radius", "value"), &PlanetField3D::set_biome_kernel_radius);
	ClassDB::bind_method(D_METHOD("get_biome_kernel_radius"), &PlanetField3D::get_biome_kernel_radius);
	ClassDB::bind_method(D_METHOD("use_default_biomes"), &PlanetField3D::use_default_biomes);
	ClassDB::bind_method(D_METHOD("has_biomes"), &PlanetField3D::has_biomes);
	ClassDB::bind_method(D_METHOD("get_blended_params", "unit_dir"), &PlanetField3D::get_blended_params);
	ClassDB::bind_method(D_METHOD("resolve_biome_at", "temperature", "moisture"), &PlanetField3D::resolve_biome_at);
	ClassDB::bind_method(D_METHOD("has_water"), &PlanetField3D::has_water);
	ClassDB::bind_method(D_METHOD("sea_depth", "unit_dir"), &PlanetField3D::sea_depth);
	ClassDB::bind_method(D_METHOD("is_submerged", "unit_dir"), &PlanetField3D::is_submerged);
	ClassDB::bind_method(D_METHOD("is_point_underwater", "point"), &PlanetField3D::is_point_underwater);
	ClassDB::bind_method(D_METHOD("dig_floor_radius", "unit_dir"), &PlanetField3D::dig_floor_radius);
	ClassDB::bind_method(D_METHOD("can_dig", "unit_dir"), &PlanetField3D::can_dig);
	ClassDB::bind_method(D_METHOD("distance_to_coast", "unit_dir", "max_search"), &PlanetField3D::distance_to_coast);
	ClassDB::bind_method(D_METHOD("set_dig_margin", "value"), &PlanetField3D::set_dig_margin);
	ClassDB::bind_method(D_METHOD("get_dig_margin"), &PlanetField3D::get_dig_margin);
	ClassDB::bind_method(D_METHOD("get_min_radius"), &PlanetField3D::get_min_radius);
	ClassDB::bind_method(D_METHOD("get_max_radius"), &PlanetField3D::get_max_radius);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_radius", PROPERTY_HINT_RANGE, "0.001,10000000,0.001,or_greater,suffix:m"), "set_base_radius", "get_base_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "amplitude", PROPERTY_HINT_RANGE, "0,100000,0.001,or_greater,suffix:m"), "set_amplitude", "get_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "period", PROPERTY_HINT_RANGE, "0.001,1000000,0.001,or_greater,suffix:m"), "set_period", "get_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "octaves", PROPERTY_HINT_RANGE, "1,32,1"), "set_octaves", "get_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lacunarity", PROPERTY_HINT_RANGE, "1,8,0.001"), "set_lacunarity", "get_lacunarity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_gain", "get_gain");

	ADD_GROUP("Shaping", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_standard", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_standard", "get_shape_standard");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_billow", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_billow", "get_shape_billow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_ridged", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_ridged", "get_shape_ridged");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_erosion", PROPERTY_HINT_RANGE, "0,8,0.001,or_greater"), "set_slope_erosion", "get_slope_erosion");

	ADD_GROUP("Domain Warp", "warp_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "warp_amplitude", PROPERTY_HINT_RANGE, "0,10000,0.001,or_greater,suffix:m"), "set_warp_amplitude", "get_warp_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "warp_period", PROPERTY_HINT_RANGE, "0.001,1000000,0.001,or_greater,suffix:m"), "set_warp_period", "get_warp_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "warp_seed"), "set_warp_seed", "get_warp_seed");


	ADD_GROUP("Biomes", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "macro_map", PROPERTY_HINT_RESOURCE_TYPE, "PlanetMacroMap3D"), "set_macro_map", "get_macro_map");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "biome_profiles", PROPERTY_HINT_TYPE_STRING,
						 vformat("%d/%d:PlanetBiomeProfile3D", Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE)),
			"set_biome_profiles", "get_biome_profiles");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "biome_kernel_radius", PROPERTY_HINT_RANGE, "0.01,2,0.001"), "set_biome_kernel_radius", "get_biome_kernel_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "macro_height_scale", PROPERTY_HINT_RANGE, "0,100000,0.001,or_greater,suffix:m"), "set_macro_height_scale", "get_macro_height_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mountain_amplitude", PROPERTY_HINT_RANGE, "0,8,0.001"), "set_mountain_amplitude", "get_mountain_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mountain_ridge", PROPERTY_HINT_RANGE, "0,4,0.001"), "set_mountain_ridge", "get_mountain_ridge");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrace_step", PROPERTY_HINT_RANGE, "0,1000,0.1,or_greater,suffix:m"), "set_terrace_step", "get_terrace_step");


	ADD_GROUP("Coasts", "coast_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "coast_enabled"), "set_coast_enabled", "get_coast_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_noise_period", PROPERTY_HINT_RANGE, "1,1000000,1,or_greater,suffix:m"), "set_coast_noise_period", "get_coast_noise_period");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_noise_amplitude", PROPERTY_HINT_RANGE, "0,10000,1,or_greater,suffix:m"), "set_coast_noise_amplitude", "get_coast_noise_amplitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_character_period", PROPERTY_HINT_RANGE, "1,1000000,1,or_greater,suffix:m"), "set_coast_character_period", "get_coast_character_period");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_cliff_height", PROPERTY_HINT_RANGE, "0,1000,1,or_greater,suffix:m"), "set_coast_cliff_height", "get_coast_cliff_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coast_cliff_width", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater"), "set_coast_cliff_width", "get_coast_cliff_width");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "coast_seed"), "set_coast_seed", "get_coast_seed");

	ADD_GROUP("Surface Levels", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "floor_radius", PROPERTY_HINT_RANGE, "0,10000000,0.001,or_greater,suffix:m"), "set_floor_radius", "get_floor_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dig_margin", PROPERTY_HINT_RANGE, "0,10000,0.5,or_greater,suffix:m along surface"), "set_dig_margin", "get_dig_margin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sea_level_radius", PROPERTY_HINT_RANGE, "0,10000000,0.001,or_greater,suffix:m"), "set_sea_level_radius", "get_sea_level_radius");

}
