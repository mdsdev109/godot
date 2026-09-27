/**************************************************************************/
/*  planet_shape_3d.cpp                                                   */
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

#include "planet_shape_3d.h"

#include "core/math/planet_lattice.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/physics_3d/physics_server_3d.h"

Vector<Vector3> PlanetShape3D::get_debug_mesh_lines() const {
	// Two rings: the min and max radius bounds. Drawing the actual surface would mean evaluating the
	// field, which the editor should not do at planet scale for a gizmo.
	Vector<Vector3> points;

	if (field.is_null()) {
		return points;
	}

	const real_t radii[2] = { field->get_min_radius(), field->get_max_radius() };
	for (const real_t r : radii) {
		for (int i = 0; i < 360; i++) {
			const float ra = Math::deg_to_rad((float)i);
			const float rb = Math::deg_to_rad((float)i + 1);
			const Point2 a = Vector2(Math::sin(ra), Math::cos(ra)) * r;
			const Point2 b = Vector2(Math::sin(rb), Math::cos(rb)) * r;

			points.push_back(Vector3(a.x, 0, a.y));
			points.push_back(Vector3(b.x, 0, b.y));
			points.push_back(Vector3(0, a.x, a.y));
			points.push_back(Vector3(0, b.x, b.y));
			points.push_back(Vector3(a.x, a.y, 0));
			points.push_back(Vector3(b.x, b.y, 0));
		}
	}

	return points;
}

Ref<ArrayMesh> PlanetShape3D::get_debug_arraymesh_faces(const Color &p_modulate) const {
	if (field.is_null()) {
		return Ref<ArrayMesh>();
	}
	const real_t base_radius = field->get_base_radius();

	Array sphere_array;
	sphere_array.resize(RSE::ARRAY_MAX);
	SphereMesh::create_mesh_array(sphere_array, base_radius, base_radius * 2, 32);

	Vector<Color> colors;
	const PackedVector3Array &verts = sphere_array[RSE::ARRAY_VERTEX];
	const int32_t verts_size = verts.size();
	for (int i = 0; i < verts_size; i++) {
		colors.append(p_modulate);
	}

	Ref<ArrayMesh> sphere_mesh = memnew(ArrayMesh);
	sphere_array[RSE::ARRAY_COLOR] = colors;
	sphere_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, sphere_array);
	return sphere_mesh;
}

real_t PlanetShape3D::get_enclosing_radius() const {
	// Patch-local: the shape is a patch centred on get_patch_origin(), not the whole planet.
	return field.is_valid() ? (field->get_max_radius() - field->get_min_radius()) : 0.0;
}

void PlanetShape3D::set_exclusions(const PackedFloat64Array &p_exclusions) {
	exclusions = p_exclusions;
	_update_shape();
	emit_changed();
}

void PlanetShape3D::_update_shape() {
	if (field.is_null()) {
		// No field yet. Leave whatever the server has rather than pushing a degenerate shape.
		return;
	}

	// The whole field crosses to the server, not just its bounds. The backend needs to evaluate f
	// itself inside the narrow phase, and it holds a by-value copy so nothing can mutate the field
	// out from under a running query.
	const PlanetFieldData &fd = field->get_data();
	Dictionary data;
	data["base_radius"] = fd.base_radius;
	data["amplitude"] = fd.amplitude;
	data["seed"] = int64_t(fd.fbm.seed);
	data["period"] = fd.fbm.period;
	data["octaves"] = fd.fbm.octaves;
	data["lacunarity"] = fd.fbm.lacunarity;
	data["gain"] = fd.fbm.gain;
	// Everything the field needs to evaluate f, not just the fBm parameters. Anything omitted here is
	// silently absent from the COLLIDER while still present in the renderer, which is the worst kind
	// of divergence: the ground you see stops being the ground you stand on.
	data["shape_standard"] = fd.shape.standard;
	data["shape_billow"] = fd.shape.billow;
	data["shape_ridged"] = fd.shape.ridged;
	data["warp_amplitude"] = fd.warp.amplitude;
	data["warp_period"] = fd.warp.period;
	data["warp_seed"] = int64_t(fd.warp.seed);
	data["slope_erosion"] = fd.slope_erosion;
	data["floor_radius"] = fd.floor_radius;
	data["sea_level_radius"] = fd.sea_level_radius;
	data["coast_enabled"] = fd.coast.enabled;
	data["coast_noise_period"] = fd.coast.noise_period;
	data["coast_noise_amplitude"] = fd.coast.noise_amplitude;
	data["coast_character_period"] = fd.coast.character_period;
	data["coast_cliff_height"] = fd.coast.cliff_height;
	data["coast_cliff_width"] = fd.coast.cliff_width;
	data["coast_seed"] = int64_t(fd.coast.seed);

	data["dig_margin"] = fd.dig_margin;
	data["macro_height_scale"] = fd.macro_height_scale;
	data["mountain_amplitude"] = fd.mountain_amplitude;
	data["mountain_ridge"] = fd.mountain_ridge;
	data["terrace_step"] = fd.terrace_step;
	data["terrace_riser"] = fd.terrace_riser;
	data["plateau_height"] = fd.plateau_height;
	data["plateau_period"] = fd.plateau_period;
	data["plateau_levels"] = fd.plateau_levels;
	data["plateau_edge"] = fd.plateau_edge;
	data["plateau_spread"] = fd.plateau_spread;
	data["plateau_seed"] = int64_t(fd.plateau_seed);
	data["landform_period"] = fd.landforms.major.period;
	data["landform_height"] = fd.landforms.major.height;
	data["landform_tier_step"] = fd.landforms.major.tier_step;
	data["landform_density"] = fd.landforms.major.density;
	data["landform_seed"] = int64_t(fd.landforms.major.seed);
	data["hill_period"] = fd.landforms.minor.period;
	data["hill_height"] = fd.landforms.minor.height;
	data["hill_tier_step"] = fd.landforms.minor.tier_step;
	data["hill_density"] = fd.landforms.minor.density;
	data["hill_seed"] = int64_t(fd.landforms.minor.seed);
	data["landform_warp"] = fd.landforms.warp;
	data["landform_riser"] = fd.landforms.riser;
	data["biome_kernel_radius"] = fd.biomes.kernel_radius;

	// The map itself travels by registry id; a Variant cannot carry the map, and copying it per patch
	// would be tens of megabytes each.
	Ref<PlanetMacroMap3D> map = field->get_macro_map();
	data["macro_map_id"] = map.is_valid() ? map->get_registry_id() : int64_t(0);

	// Thirteen floats per biome, flat, because a Variant array of dictionaries would allocate on every
	// shape update and there are dozens of patches.
	PackedFloat64Array biome_data;
	biome_data.resize(fd.biomes.count * 13);
	for (int i = 0; i < fd.biomes.count; ++i) {
		const PlanetBiomeProfileData &e = fd.biomes.entries[i];
		double *w = biome_data.ptrw() + i * 13;
		w[0] = e.anchor_temperature;
		w[1] = e.anchor_moisture;
		w[2] = e.base_elevation;
		w[3] = e.amplitude_scale;
		w[4] = e.frequency_scale;
		w[5] = e.roughness;
		w[6] = e.shape.standard;
		w[7] = e.shape.billow;
		w[8] = e.shape.ridged;
		w[9] = e.terrace_strength;
		w[10] = e.plateau_strength;
		w[11] = e.landform_strength;
		w[12] = e.hill_strength;
	}
	data["biomes"] = biome_data;

	data["face"] = face;
	data["patch_i"] = patch_i;
	data["patch_j"] = patch_j;
	data["patches_per_axis"] = patches_per_axis;
	data["exclusions"] = exclusions;
	PhysicsServer3D::get_singleton()->shape_set_data(get_shape(), data);
	Shape3D::_update_shape();
}

void PlanetShape3D::_field_changed() {
	_update_shape();
	emit_changed();
}

void PlanetShape3D::set_field(const Ref<PlanetField3D> &p_field) {
	if (field == p_field) {
		return;
	}
	// Editing the field's properties has to rebuild the shape, or its cached radius bounds go stale
	// and stop containing f.
	if (field.is_valid()) {
		field->disconnect_changed(callable_mp(this, &PlanetShape3D::_field_changed));
	}
	field = p_field;
	if (field.is_valid()) {
		field->connect_changed(callable_mp(this, &PlanetShape3D::_field_changed));
	}
	_update_shape();
	emit_changed();
}

Ref<PlanetField3D> PlanetShape3D::get_field() const {
	return field;
}


static PlanetLattice make_lattice(double p_base_radius, int p_patches_per_axis) {
	// Must match JoltPlanetShape3D::set_data exactly, or the origin reported to the scene will not be
	// the origin the shape tessellates around.
	constexpr double VOXEL_EDGE_M = 0.25;
	const double face_edge_arc = Math::PI * p_base_radius / 2.0;
	const double wanted = face_edge_arc / VOXEL_EDGE_M;
	const uint32_t per_axis = MAX(uint32_t(1), uint32_t(p_patches_per_axis));
	uint64_t res64 = uint64_t(Math::ceil(wanted / double(per_axis))) * uint64_t(per_axis);
	res64 = CLAMP(res64, uint64_t(per_axis), uint64_t(1u << 30));
	uint32_t res = uint32_t(res64);
	PlanetLattice lattice;
	lattice.resolution = res;
	return lattice;
}

int PlanetShape3D::get_lattice_resolution() const {
	if (field.is_null()) {
		return 0;
	}
	return int(make_lattice(field->get_base_radius(), patches_per_axis).resolution);
}

Vector3 PlanetShape3D::get_patch_origin() const {
	if (field.is_null()) {
		return Vector3();
	}
	const double base_radius = field->get_base_radius();
	const PlanetLattice lattice = make_lattice(base_radius, patches_per_axis);
	const Vector3 dir = lattice.patch_centre_dir(face, uint32_t(patches_per_axis), uint32_t(patch_i), uint32_t(patch_j));
	return dir * base_radius;
}

void PlanetShape3D::set_face(int p_face) {
	ERR_FAIL_INDEX(p_face, 6);
	face = p_face;
	_update_shape();
	emit_changed();
}

int PlanetShape3D::get_face() const {
	return face;
}

void PlanetShape3D::set_patch_i(int p_i) {
	ERR_FAIL_COND(p_i < 0);
	patch_i = p_i;
	_update_shape();
	emit_changed();
}

int PlanetShape3D::get_patch_i() const {
	return patch_i;
}

void PlanetShape3D::set_patch_j(int p_j) {
	ERR_FAIL_COND(p_j < 0);
	patch_j = p_j;
	_update_shape();
	emit_changed();
}

int PlanetShape3D::get_patch_j() const {
	return patch_j;
}

void PlanetShape3D::set_patches_per_axis(int p_n) {
	ERR_FAIL_COND_MSG(p_n < 1, "PlanetShape3D patch subdivision must be at least 1.");
	patches_per_axis = p_n;
	_update_shape();
	emit_changed();
}

int PlanetShape3D::get_patches_per_axis() const {
	return patches_per_axis;
}

Dictionary PlanetShape3D::get_collision_stats() {
	Dictionary d;
	d["collide_queries"] = int64_t(PlanetCollisionStats::collide_queries.load(std::memory_order_relaxed));
	d["ray_queries"] = int64_t(PlanetCollisionStats::ray_queries.load(std::memory_order_relaxed));
	d["cast_queries"] = int64_t(PlanetCollisionStats::cast_queries.load(std::memory_order_relaxed));
	d["rejected_queries"] = int64_t(PlanetCollisionStats::rejected_queries.load(std::memory_order_relaxed));
	d["cells_tessellated"] = int64_t(PlanetCollisionStats::cells_tessellated.load(std::memory_order_relaxed));
	d["field_samples"] = int64_t(PlanetCollisionStats::field_samples.load(std::memory_order_relaxed));
	d["nanoseconds"] = int64_t(PlanetCollisionStats::nanoseconds.load(std::memory_order_relaxed));
	return d;
}

void PlanetShape3D::reset_collision_stats() {
	PlanetCollisionStats::reset();
}

void PlanetShape3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_exclusions", "exclusions"), &PlanetShape3D::set_exclusions);
	ClassDB::bind_method(D_METHOD("get_exclusions"), &PlanetShape3D::get_exclusions);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT64_ARRAY, "exclusions"), "set_exclusions", "get_exclusions");
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetShape3D::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetShape3D::get_field);

	ClassDB::bind_method(D_METHOD("set_face", "face"), &PlanetShape3D::set_face);
	ClassDB::bind_method(D_METHOD("get_face"), &PlanetShape3D::get_face);
	ClassDB::bind_method(D_METHOD("set_patch_i", "i"), &PlanetShape3D::set_patch_i);
	ClassDB::bind_method(D_METHOD("get_patch_i"), &PlanetShape3D::get_patch_i);
	ClassDB::bind_method(D_METHOD("set_patch_j", "j"), &PlanetShape3D::set_patch_j);
	ClassDB::bind_method(D_METHOD("get_patch_j"), &PlanetShape3D::get_patch_j);
	ClassDB::bind_method(D_METHOD("set_patches_per_axis", "n"), &PlanetShape3D::set_patches_per_axis);
	ClassDB::bind_method(D_METHOD("get_patches_per_axis"), &PlanetShape3D::get_patches_per_axis);
	ClassDB::bind_method(D_METHOD("get_patch_origin"), &PlanetShape3D::get_patch_origin);
	ClassDB::bind_method(D_METHOD("get_lattice_resolution"), &PlanetShape3D::get_lattice_resolution);
	ClassDB::bind_static_method("PlanetShape3D", D_METHOD("get_collision_stats"), &PlanetShape3D::get_collision_stats);
	ClassDB::bind_static_method("PlanetShape3D", D_METHOD("reset_collision_stats"), &PlanetShape3D::reset_collision_stats);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "face", PROPERTY_HINT_RANGE, "0,5,1"), "set_face", "get_face");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patch_i"), "set_patch_i", "get_patch_i");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patch_j"), "set_patch_j", "get_patch_j");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "patches_per_axis", PROPERTY_HINT_RANGE, "1,1024,1"), "set_patches_per_axis", "get_patches_per_axis");
}

PlanetShape3D::PlanetShape3D() :
		Shape3D(PhysicsServer3D::get_singleton()->shape_create(PS3DE::SHAPE_PLANET)) {
	Ref<PlanetField3D> default_field;
	default_field.instantiate();
	set_field(default_field);
}
