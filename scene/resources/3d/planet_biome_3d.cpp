/**************************************************************************/
/*  planet_biome_3d.cpp                                                   */
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

#include "planet_biome_3d.h"

#include "core/object/class_db.h"
#include "core/variant/variant_utility.h"

// ---------------------------------------------------------------- PlanetBiomeProfile3D

#define PB_SETGET(name, member, lo, hi)                                          \
	void PlanetBiomeProfile3D::set_##name(real_t p_v) {                          \
		data.member = CLAMP(float(p_v), float(lo), float(hi));                   \
		emit_changed();                                                          \
	}                                                                            \
	real_t PlanetBiomeProfile3D::get_##name() const { return data.member; }

PB_SETGET(anchor_temperature, anchor_temperature, 0.0, 1.0)
PB_SETGET(anchor_moisture, anchor_moisture, 0.0, 1.0)
PB_SETGET(base_elevation, base_elevation, -4.0, 4.0)
PB_SETGET(amplitude_scale, amplitude_scale, 0.0, 16.0)
PB_SETGET(frequency_scale, frequency_scale, 0.001, 64.0)
PB_SETGET(roughness, roughness, 0.0, 1.0)
PB_SETGET(terrace_strength, terrace_strength, 0.0, 1.0)
PB_SETGET(plateau_strength, plateau_strength, 0.0, 1.0)
PB_SETGET(landform_strength, landform_strength, 0.0, 2.0)
PB_SETGET(hill_strength, hill_strength, 0.0, 2.0)
PB_SETGET(shape_standard, shape.standard, 0.0, 1.0)
PB_SETGET(shape_billow, shape.billow, 0.0, 1.0)
PB_SETGET(shape_ridged, shape.ridged, 0.0, 1.0)

#undef PB_SETGET

// Integers, so they do not go through the float clamp macro above. Clamped to the palette anyway:
// an index the renderer has no material for would read a texture layer that is not there.
void PlanetBiomeProfile3D::set_ground_material(int p_v) {
	data.ground_material = CLAMP(p_v, 0, 15);
	emit_changed();
}
int PlanetBiomeProfile3D::get_ground_material() const { return data.ground_material; }

void PlanetBiomeProfile3D::set_cliff_material(int p_v) {
	data.cliff_material = CLAMP(p_v, 0, 15);
	emit_changed();
}
int PlanetBiomeProfile3D::get_cliff_material() const { return data.cliff_material; }

void PlanetBiomeProfile3D::_bind_methods() {
#define PB_BIND(name)                                                                             \
	ClassDB::bind_method(D_METHOD("set_" #name, "value"), &PlanetBiomeProfile3D::set_##name);      \
	ClassDB::bind_method(D_METHOD("get_" #name), &PlanetBiomeProfile3D::get_##name);

	PB_BIND(anchor_temperature)
	PB_BIND(anchor_moisture)
	PB_BIND(base_elevation)
	PB_BIND(amplitude_scale)
	PB_BIND(frequency_scale)
	PB_BIND(roughness)
	PB_BIND(terrace_strength)
	PB_BIND(plateau_strength)
	PB_BIND(landform_strength)
	PB_BIND(hill_strength)
	PB_BIND(shape_standard)
	PB_BIND(shape_billow)
	PB_BIND(shape_ridged)
	PB_BIND(ground_material)
	PB_BIND(cliff_material)
#undef PB_BIND

	ADD_GROUP("Climate Anchor", "anchor_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "anchor_temperature", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_anchor_temperature", "get_anchor_temperature");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "anchor_moisture", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_anchor_moisture", "get_anchor_moisture");

	ADD_GROUP("Terrain", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_elevation", PROPERTY_HINT_RANGE, "-4,4,0.001"), "set_base_elevation", "get_base_elevation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "amplitude_scale", PROPERTY_HINT_RANGE, "0,16,0.001"), "set_amplitude_scale", "get_amplitude_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "frequency_scale", PROPERTY_HINT_RANGE, "0.001,64,0.001"), "set_frequency_scale", "get_frequency_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_roughness", "get_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "terrace_strength", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_terrace_strength", "get_terrace_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "plateau_strength", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_plateau_strength", "get_plateau_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "landform_strength", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_landform_strength", "get_landform_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hill_strength", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_hill_strength", "get_hill_strength");

	ADD_GROUP("Shape Mix", "shape_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_standard", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_standard", "get_shape_standard");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_billow", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_billow", "get_shape_billow");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shape_ridged", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_shape_ridged", "get_shape_ridged");

	ADD_GROUP("Surface", "");
	const char *mat_hint = "Grass,Dry Grass,Sand,Gravel,Rock,Cliff,Snow,Ice";
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ground_material", PROPERTY_HINT_ENUM, mat_hint), "set_ground_material", "get_ground_material");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cliff_material", PROPERTY_HINT_ENUM, mat_hint), "set_cliff_material", "get_cliff_material");
}

// ---------------------------------------------------------------- PlanetMacroMap3D

void PlanetMacroMap3D::set_resolution(int p_resolution) {
	ERR_FAIL_COND_MSG(p_resolution < 4 || p_resolution > 8192, "PlanetMacroMap3D resolution must be between 4 and 8192.");
	ERR_FAIL_COND_MSG((p_resolution & (p_resolution - 1)) != 0, "PlanetMacroMap3D resolution must be a power of two.");
	resolution = p_resolution;
	// The existing map is now the wrong size. Dropped rather than resampled: a resize is a re-bake or
	// a re-import, and silently stretching authored data would be worse than making it explicit.
	data.reset();
	_republish();
	emit_changed();
}
int PlanetMacroMap3D::get_resolution() const { return resolution; }

void PlanetMacroMap3D::set_bake_radius(real_t p_radius) {
	ERR_FAIL_COND_MSG(p_radius <= 0, "PlanetMacroMap3D bake_radius must be positive.");
	bake_radius = p_radius;
	emit_changed();
}
real_t PlanetMacroMap3D::get_bake_radius() const { return bake_radius; }

#define PM_SETGET(name, member, type, lo, hi)                                   \
	void PlanetMacroMap3D::set_##name(type p_v) {                               \
		bake_params.member = CLAMP(p_v, type(lo), type(hi));                    \
		emit_changed();                                                         \
	}                                                                           \
	type PlanetMacroMap3D::get_##name() const { return type(bake_params.member); }

PM_SETGET(continent_period, continent_period, real_t, 1.0, 1e9)
PM_SETGET(continent_octaves, continent_octaves, int, 1, 12)
PM_SETGET(polar_weight, polar_weight, real_t, 0.0, 1.0)
PM_SETGET(polar_falloff, polar_falloff, real_t, 0.05, 16.0)
PM_SETGET(temperature_scale, temperature_scale, real_t, 0.0, 4.0)
PM_SETGET(moisture_temperature_weight, moisture_temperature_weight, real_t, 0.0, 1.0)
PM_SETGET(lapse_rate, lapse_rate, real_t, 0.0, 2.0)
PM_SETGET(moisture_period, moisture_period, real_t, 1.0, 1e9)
PM_SETGET(moisture_octaves, moisture_octaves, int, 1, 12)
PM_SETGET(temperature_period, temperature_period, real_t, 1.0, 1e9)
PM_SETGET(temperature_octaves, temperature_octaves, int, 1, 12)
PM_SETGET(climate_gain, climate_gain, real_t, 0.05, 0.95)
PM_SETGET(weirdness_period, weirdness_period, real_t, 1.0, 1e9)
PM_SETGET(weirdness_octaves, weirdness_octaves, int, 1, 12)

#undef PM_SETGET

void PlanetMacroMap3D::set_normalize_channels(bool p_v) {
	bake_params.normalize_channels = p_v;
	emit_changed();
}
bool PlanetMacroMap3D::get_normalize_channels() const { return bake_params.normalize_channels; }

void PlanetMacroMap3D::set_seed(int p_seed) {
	bake_params.seed = uint32_t(p_seed);
	emit_changed();
}
int PlanetMacroMap3D::get_seed() const { return int(bake_params.seed); }

PlanetMacroMap3D::~PlanetMacroMap3D() {
	PlanetMacroMapRegistry::unregister_map(registry_id);
}

// Replaces the registered map with the current data. Called after anything that produces new texels,
// so a shape resolving the id always gets what the inspector is showing.
void PlanetMacroMap3D::_republish() {
	PlanetMacroMapRegistry::unregister_map(registry_id);
	registry_id = data != nullptr ? PlanetMacroMapRegistry::register_map(data) : 0;
}

void PlanetMacroMap3D::bake() {
	std::shared_ptr<PlanetMacroMapData> fresh = std::make_shared<PlanetMacroMapData>();
	fresh->allocate(resolution);
	fresh->bake(bake_params, bake_radius);
	data = fresh;
	_republish();
	emit_changed();
}

Error PlanetMacroMap3D::set_face_image(int p_face, const Ref<Image> &p_image) {
	ERR_FAIL_INDEX_V(p_face, PlanetMacroMapData::FACE_COUNT, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V_MSG(p_image.is_null(), ERR_INVALID_PARAMETER, "PlanetMacroMap3D face image is null.");
	ERR_FAIL_COND_V_MSG(p_image->get_width() != resolution || p_image->get_height() != resolution,
			ERR_INVALID_PARAMETER,
			vformat("PlanetMacroMap3D face image must be %dx%d to match the map resolution.", resolution, resolution));

	if (data == nullptr || !data->is_valid()) {
		std::shared_ptr<PlanetMacroMapData> fresh = std::make_shared<PlanetMacroMapData>();
		fresh->allocate(resolution);
		data = fresh;
	}

	for (int y = 0; y < resolution; ++y) {
		for (int x = 0; x < resolution; ++x) {
			const Color c = p_image->get_pixel(x, y);
			PlanetMacroSample &s = data->at_mut(p_face, x, y);
			s.height = float(c.r);
			s.temperature = float(c.g);
			s.moisture = float(c.b);
			s.weirdness = float(c.a);
		}
	}
	_republish();
	emit_changed();
	return OK;
}

Ref<Image> PlanetMacroMap3D::get_face_image(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, PlanetMacroMapData::FACE_COUNT, Ref<Image>());
	ERR_FAIL_COND_V_MSG(!is_baked(), Ref<Image>(), "PlanetMacroMap3D has no data; bake() or set_face_image() first.");

	// Half-float rather than 8-bit: 256 levels of continental height is about 78 m per step over a
	// 20 km range, and bilinear interpolation turns that into visible macro-scale faceting rather
	// than hiding it. Round-trips through export and re-import without losing authored detail.
	Ref<Image> img = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAH);
	for (int y = 0; y < resolution; ++y) {
		for (int x = 0; x < resolution; ++x) {
			const PlanetMacroSample &s = data->at(p_face, x, y);
			img->set_pixel(x, y, Color(s.height, s.temperature, s.moisture, s.weirdness));
		}
	}
	return img;
}

Ref<Image> PlanetMacroMap3D::get_face_image_float(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, PlanetMacroMapData::FACE_COUNT, Ref<Image>());
	ERR_FAIL_COND_V_MSG(!is_baked(), Ref<Image>(), "PlanetMacroMap3D has no data; bake() or set_face_image() first.");

	// Full 32-bit float, unlike get_face_image(). Half-float is right for authoring round-trips and
	// wrong for a renderer: its 5e-4 quantisation of the climate is multiplied by the biome frequency
	// scale into a noise lattice coordinate of about 1300 units, which moves the terrain by most of a
	// cell and shows up as the GPU surface disagreeing with the collider by metres.
	Ref<Image> img = Image::create_empty(resolution, resolution, false, Image::FORMAT_RGBAF);
	for (int y = 0; y < resolution; ++y) {
		for (int x = 0; x < resolution; ++x) {
			const PlanetMacroSample &s = data->at(p_face, x, y);
			img->set_pixel(x, y, Color(s.height, s.temperature, s.moisture, s.weirdness));
		}
	}
	return img;
}

Color PlanetMacroMap3D::sample_direction(const Vector3 &p_dir) const {
	if (!is_baked()) {
		return Color(0, 0, 0, 0);
	}
	Vector3 d = p_dir;
	if (d.length_squared() <= 0) {
		return Color(0, 0, 0, 0);
	}
	d.normalize();
	const PlanetMacroSample s = data->sample(d);
	return Color(s.height, s.temperature, s.moisture, s.weirdness);
}

void PlanetMacroMap3D::_bind_methods() {
#define PM_BIND(name)                                                                        \
	ClassDB::bind_method(D_METHOD("set_" #name, "value"), &PlanetMacroMap3D::set_##name);     \
	ClassDB::bind_method(D_METHOD("get_" #name), &PlanetMacroMap3D::get_##name);

	PM_BIND(resolution)
	PM_BIND(bake_radius)
	PM_BIND(seed)
	PM_BIND(continent_period)
	PM_BIND(continent_octaves)
	PM_BIND(polar_weight)
	PM_BIND(polar_falloff)
	PM_BIND(temperature_scale)
	PM_BIND(moisture_temperature_weight)
	PM_BIND(lapse_rate)
	PM_BIND(moisture_period)
	PM_BIND(moisture_octaves)
	PM_BIND(temperature_period)
	PM_BIND(temperature_octaves)
	PM_BIND(climate_gain)
	PM_BIND(normalize_channels)
	PM_BIND(weirdness_period)
	PM_BIND(weirdness_octaves)
#undef PM_BIND

	ClassDB::bind_method(D_METHOD("bake"), &PlanetMacroMap3D::bake);
	ClassDB::bind_method(D_METHOD("get_registry_id"), &PlanetMacroMap3D::get_registry_id);
	ClassDB::bind_method(D_METHOD("is_baked"), &PlanetMacroMap3D::is_baked);
	ClassDB::bind_method(D_METHOD("set_face_image", "face", "image"), &PlanetMacroMap3D::set_face_image);
	ClassDB::bind_method(D_METHOD("get_face_image", "face"), &PlanetMacroMap3D::get_face_image);
	ClassDB::bind_method(D_METHOD("get_face_image_float", "face"), &PlanetMacroMap3D::get_face_image_float);
	ClassDB::bind_method(D_METHOD("sample_direction", "direction"), &PlanetMacroMap3D::sample_direction);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "resolution", PROPERTY_HINT_ENUM, "64:64,128:128,256:256,512:512,1024:1024,2048:2048,4096:4096"), "set_resolution", "get_resolution");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bake_radius", PROPERTY_HINT_RANGE, "1,100000000,0.001,or_greater,suffix:m"), "set_bake_radius", "get_bake_radius");

	ADD_GROUP("Procedural Bake", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "continent_period", PROPERTY_HINT_RANGE, "1,100000000,1,or_greater,suffix:m"), "set_continent_period", "get_continent_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "continent_octaves", PROPERTY_HINT_RANGE, "1,12,1"), "set_continent_octaves", "get_continent_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature_period", PROPERTY_HINT_RANGE, "1,100000000,1,or_greater,suffix:m"), "set_temperature_period", "get_temperature_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "temperature_octaves", PROPERTY_HINT_RANGE, "1,12,1"), "set_temperature_octaves", "get_temperature_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "climate_gain", PROPERTY_HINT_RANGE, "0.05,0.95,0.001"), "set_climate_gain", "get_climate_gain");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "polar_weight", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_polar_weight", "get_polar_weight");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "polar_falloff", PROPERTY_HINT_RANGE, "0.05,16,0.01"), "set_polar_falloff", "get_polar_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "temperature_scale", PROPERTY_HINT_RANGE, "0,4,0.001"), "set_temperature_scale", "get_temperature_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moisture_temperature_weight", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_moisture_temperature_weight", "get_moisture_temperature_weight");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lapse_rate", PROPERTY_HINT_RANGE, "0,2,0.001"), "set_lapse_rate", "get_lapse_rate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "moisture_period", PROPERTY_HINT_RANGE, "1,100000000,1,or_greater,suffix:m"), "set_moisture_period", "get_moisture_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "moisture_octaves", PROPERTY_HINT_RANGE, "1,12,1"), "set_moisture_octaves", "get_moisture_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "weirdness_period", PROPERTY_HINT_RANGE, "1,100000000,1,or_greater,suffix:m"), "set_weirdness_period", "get_weirdness_period");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "weirdness_octaves", PROPERTY_HINT_RANGE, "1,12,1"), "set_weirdness_octaves", "get_weirdness_octaves");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "normalize_channels"), "set_normalize_channels", "get_normalize_channels");
}
