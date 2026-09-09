/**************************************************************************/
/*  jolt_planet_shape_3d.cpp                                              */
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

#include "jolt_planet_shape_3d.h"

#include "../misc/jolt_type_conversions.h"
#include "jolt_custom_planet_shape.h"


// Cube-face patches per axis. 6 * 4 * 4 = 96 patches, the design doc starting point.
//
// This number trades against the SubShapeID budget: the compound spends ceil(log2(patch count)) bits
// and the shape gets what is left of 32. At 96 patches that is 7 bits for the compound and 25 for the
// lattice cell hash, which is what GetSubShapeIDBitsRecursive() returns. Raising this without
// lowering that would overflow the ID.
JPH::ShapeRefC JoltPlanetShape3D::_build() const {
	ERR_FAIL_COND_V_MSG(field.base_radius <= 0.0, nullptr, vformat("Failed to build Jolt Physics planet shape with %s. Its base radius must be greater than 0. This shape belongs to %s.", to_string(), _owners_to_string()));
	ERR_FAIL_COND_V_MSG(field.amplitude < 0.0, nullptr, vformat("Failed to build Jolt Physics planet shape with %s. Its amplitude cannot be negative. This shape belongs to %s.", to_string(), _owners_to_string()));
	ERR_FAIL_COND_V_MSG(field.amplitude >= field.base_radius, nullptr, vformat("Failed to build Jolt Physics planet shape with %s. Its amplitude must be less than its base radius. This shape belongs to %s.", to_string(), _owners_to_string()));
	ERR_FAIL_COND_V_MSG(patches_per_axis == 0, nullptr, "PlanetShape3D patch subdivision must be at least 1.");

	JoltCustomPlanetShapeSettings shape_settings;
	shape_settings.field = field;
	shape_settings.lattice = lattice;
	shape_settings.face = face;
	lattice.patch_cell_range(
			patches_per_axis, patch_i, patch_j,
			shape_settings.cell_min_i, shape_settings.cell_min_j,
			shape_settings.cell_max_i, shape_settings.cell_max_j);

	const Vector3 centre_dir = lattice.patch_centre_dir(face, patches_per_axis, patch_i, patch_j);
	shape_settings.patch_origin = centre_dir * field.base_radius;

	// Tight radius bounds for this patch rather than the global ones. This is the min/max bound the
	// design calls a pyramid, evaluated once at build time instead of stored as a tree.
	const Vector3 corner_dir = lattice.vertex_direction(face, shape_settings.cell_min_i, shape_settings.cell_min_j);
	const double half_angle = Math::acos(CLAMP(centre_dir.dot(corner_dir), -1.0, 1.0));
	double patch_min = 0.0;
	double patch_max = 0.0;
	field.get_bounds_for_cone(centre_dir, half_angle, patch_min, patch_max);
	shape_settings.patch_min_radius = float(patch_min);
	shape_settings.patch_max_radius = float(patch_max);

	const JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
	ERR_FAIL_COND_V_MSG(shape_result.HasError(), nullptr, vformat("Failed to build Jolt Physics planet shape with %s. It returned the following error: %s. This shape belongs to %s.", to_string(), to_godot(shape_result.GetError()), _owners_to_string()));

	return shape_result.Get();
}

Variant JoltPlanetShape3D::get_data() const {
	Dictionary data;
	data["base_radius"] = field.base_radius;
	data["coast_enabled"] = field.coast.enabled;
	data["coast_noise_period"] = field.coast.noise_period;
	data["coast_noise_amplitude"] = field.coast.noise_amplitude;
	data["coast_character_period"] = field.coast.character_period;
	data["coast_cliff_height"] = field.coast.cliff_height;
	data["coast_cliff_width"] = field.coast.cliff_width;
	data["coast_seed"] = int64_t(field.coast.seed);

	data["amplitude"] = field.amplitude;
	data["seed"] = int64_t(field.fbm.seed);
	data["period"] = field.fbm.period;
	data["octaves"] = field.fbm.octaves;
	data["lacunarity"] = field.fbm.lacunarity;
	data["gain"] = field.fbm.gain;
	data["face"] = face;
	data["patch_i"] = int64_t(patch_i);
	data["patch_j"] = int64_t(patch_j);
	data["patches_per_axis"] = int64_t(patches_per_axis);
	return data;
}

void JoltPlanetShape3D::set_data(const Variant &p_data) {
	ERR_FAIL_COND(p_data.get_type() != Variant::DICTIONARY);
	const Dictionary data = p_data;

	PlanetFieldData new_field;
	new_field.base_radius = data.get("base_radius", 3185500.0);
	new_field.amplitude = data.get("amplitude", 8000.0);
	new_field.fbm.seed = uint32_t(int64_t(data.get("seed", 0)));
	new_field.fbm.period = float(double(data.get("period", 2443.0)));
	new_field.fbm.octaves = int(data.get("octaves", 13));
	new_field.fbm.lacunarity = float(double(data.get("lacunarity", 2.0)));
	new_field.fbm.gain = float(double(data.get("gain", 0.5)));

	new_field.shape.standard = float(double(data.get("shape_standard", 1.0)));
	new_field.shape.billow = float(double(data.get("shape_billow", 0.0)));
	new_field.shape.ridged = float(double(data.get("shape_ridged", 0.0)));
	new_field.warp.amplitude = float(double(data.get("warp_amplitude", 0.0)));
	new_field.warp.period = float(double(data.get("warp_period", 1.0)));
	new_field.warp.seed = uint32_t(int64_t(data.get("warp_seed", 0)));
	new_field.slope_erosion = data.get("slope_erosion", 0.0);
	new_field.floor_radius = data.get("floor_radius", 0.0);
	new_field.sea_level_radius = data.get("sea_level_radius", 0.0);
	new_field.coast.enabled = data.get("coast_enabled", false);
	new_field.coast.noise_period = MAX(float(double(data.get("coast_noise_period", new_field.coast.noise_period))), 0.01f);
	new_field.coast.noise_amplitude = MAX(float(double(data.get("coast_noise_amplitude", new_field.coast.noise_amplitude))), 0.f);
	new_field.coast.character_period = MAX(float(double(data.get("coast_character_period", new_field.coast.character_period))), 0.01f);
	new_field.coast.cliff_height = MAX(float(double(data.get("coast_cliff_height", new_field.coast.cliff_height))), 0.f);
	new_field.coast.cliff_width = MAX(float(double(data.get("coast_cliff_width", new_field.coast.cliff_width))), 0.01f);
	new_field.coast.seed = uint32_t(int64_t(data.get("coast_seed", 7319)));

	new_field.dig_margin = data.get("dig_margin", 25.0);
	new_field.macro_height_scale = data.get("macro_height_scale", 0.0);
	new_field.mountain_amplitude = data.get("mountain_amplitude", 0.0);
	new_field.mountain_ridge = data.get("mountain_ridge", 0.0);
	new_field.terrace_step = data.get("terrace_step", 0.0);
	new_field.biomes.kernel_radius = float(double(data.get("biome_kernel_radius", 0.55)));
	new_field.macro_map = PlanetMacroMapRegistry::get(uint64_t(int64_t(data.get("macro_map_id", 0))));

	const PackedFloat64Array biome_data = data.get("biomes", PackedFloat64Array());
	new_field.biomes.count = MIN(int(biome_data.size() / 10), PlanetBiomeTable::MAX_BIOMES);
	for (int i = 0; i < new_field.biomes.count; ++i) {
		PlanetBiomeProfileData &e = new_field.biomes.entries[i];
		const double *r = biome_data.ptr() + i * 10;
		e.anchor_temperature = float(r[0]);
		e.anchor_moisture = float(r[1]);
		e.base_elevation = float(r[2]);
		e.amplitude_scale = float(r[3]);
		e.frequency_scale = float(r[4]);
		e.roughness = float(r[5]);
		e.shape.standard = float(r[6]);
		e.shape.billow = float(r[7]);
		e.shape.ridged = float(r[8]);
		e.terrace_strength = float(r[9]);
	}

	const int new_face = int(data.get("face", 0));
	const uint32_t new_pi = uint32_t(int64_t(data.get("patch_i", 0)));
	const uint32_t new_pj = uint32_t(int64_t(data.get("patch_j", 0)));
	const uint32_t new_per_axis = uint32_t(int64_t(data.get("patches_per_axis", 1)));

	// Cells sized to the voxel edge. Rounding up to the next power of two would give 0.149 m cells for
	// a 0.25 m target here, which is 1.68x finer per axis and so 2.8x more field evaluations for
	// nothing -- detail below the voxel edge is discarded by the voxel layer anyway. Round to a
	// multiple of the patch subdivision instead, which is all patch_cell_range needs to divide evenly.
	constexpr double VOXEL_EDGE_M = 0.25;
	const double face_edge_arc = Math::PI * new_field.base_radius / 2.0;
	const double wanted = face_edge_arc / VOXEL_EDGE_M;
	const uint32_t per_axis = MAX(uint32_t(1), new_per_axis);
	uint64_t res64 = uint64_t(Math::ceil(wanted / double(per_axis))) * uint64_t(per_axis);
	res64 = CLAMP(res64, uint64_t(per_axis), uint64_t(1u << 30));
	uint32_t res = uint32_t(res64);
	PlanetLattice new_lattice;
	new_lattice.resolution = res;

	const bool unchanged = planet_field_equal(new_field, field) && new_face == face && new_pi == patch_i && new_pj == patch_j && new_per_axis == patches_per_axis && new_lattice.resolution == lattice.resolution;
	if (unchanged) {
		return;
	}

	field = new_field;
	lattice = new_lattice;
	face = new_face;
	patch_i = new_pi;
	patch_j = new_pj;
	patches_per_axis = new_per_axis;

	destroy();
}

AABB JoltPlanetShape3D::get_aabb() const {
	const real_t r = field.get_max_radius();
	const Vector3 half_extents(r, r, r);
	return AABB(-half_extents, half_extents * 2.0f);
}

String JoltPlanetShape3D::to_string() const {
	return vformat("{base_radius=%f, amplitude=%f, octaves=%d}", field.base_radius, field.amplitude, field.fbm.octaves);
}
