/**************************************************************************/
/*  planet_navigator.cpp                                                  */
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

#include "planet_navigator.h"

#include "core/object/class_db.h"
#include "core/templates/hash_set.h"

namespace {

// Node ids: face (3 bits) | i (29 bits) | j (29 bits).
constexpr int NODE_SHIFT = 29;
constexpr uint64_t NODE_MASK = (uint64_t(1) << NODE_SHIFT) - 1;

inline uint64_t make_node(int p_face, uint32_t p_i, uint32_t p_j) {
	return (uint64_t(p_face) << (2 * NODE_SHIFT)) | (uint64_t(p_i) << NODE_SHIFT) | uint64_t(p_j);
}

struct Open {
	double f = 0.0;
	uint64_t node = 0;
	// Lowest f first; ties by node id, so equal-cost paths come out the same everywhere.
	bool before(const Open &p_other) const {
		return f < p_other.f || (f == p_other.f && node < p_other.node);
	}
};

void heap_push(LocalVector<Open> &r_heap, const Open &p_item) {
	r_heap.push_back(p_item);
	uint32_t i = r_heap.size() - 1;
	while (i > 0) {
		const uint32_t parent = (i - 1) / 2;
		if (!r_heap[i].before(r_heap[parent])) {
			break;
		}
		SWAP(r_heap[i], r_heap[parent]);
		i = parent;
	}
}

Open heap_pop(LocalVector<Open> &r_heap) {
	const Open top = r_heap[0];
	r_heap[0] = r_heap[r_heap.size() - 1];
	r_heap.resize(r_heap.size() - 1);
	uint32_t i = 0;
	while (true) {
		const uint32_t l = 2 * i + 1;
		const uint32_t r = l + 1;
		uint32_t best = i;
		if (l < r_heap.size() && r_heap[l].before(r_heap[best])) {
			best = l;
		}
		if (r < r_heap.size() && r_heap[r].before(r_heap[best])) {
			best = r;
		}
		if (best == i) {
			break;
		}
		SWAP(r_heap[i], r_heap[best]);
		i = best;
	}
	return top;
}

} // namespace

void PlanetNavigator::_configure() {
	const double edge = ground.radius * Math::PI * 0.5;
	resolution = uint32_t(CLAMP(Math::round(edge / MAX(cell_size, 0.01)), 1.0, double(NODE_MASK)));
	_clear_tiles();
}

void PlanetNavigator::_clear_tiles() {
	for (KeyValue<uint64_t, Tile *> &T : tiles) {
		memdelete(T.value);
	}
	tiles.clear();
}

void PlanetNavigator::set_field(const Ref<PlanetField3D> &p_field) {
	ground.field = p_field;
	if (p_field.is_valid()) {
		ground.radius = p_field->get_base_radius();
	}
	_configure();
}

void PlanetNavigator::set_planet_radius(double p_radius) {
	ground.radius = p_radius;
	_configure();
}

void PlanetNavigator::set_cell_size(double p_size) {
	cell_size = p_size;
	_configure();
}

void PlanetNavigator::add_carve(const Vector3 &p_centre, double p_radius) {
	ground.carve_centres.push_back(p_centre);
	ground.carve_radii.push_back(p_radius);
	// Carves are rare; dropping every cached height is simpler than finding the tiles one touches.
	_clear_tiles();
}

void PlanetNavigator::clear_carves() {
	ground.carve_centres.clear();
	ground.carve_radii.clear();
	_clear_tiles();
}

uint64_t PlanetNavigator::_node_of(const Vector3 &p_dir) const {
	int face = 0;
	double s = 0.0;
	double t = 0.0;
	PlanetLattice::direction_to_face(p_dir.normalized(), face, s, t);
	const int res = int(resolution);
	const int i = CLAMP(int(Math::floor((s + 1.0) * 0.5 * res)), 0, res - 1);
	const int j = CLAMP(int(Math::floor((t + 1.0) * 0.5 * res)), 0, res - 1);
	return make_node(face, uint32_t(i), uint32_t(j));
}

Vector3 PlanetNavigator::_node_dir(uint64_t p_node) const {
	const int face = int(p_node >> (2 * NODE_SHIFT));
	const double i = double((p_node >> NODE_SHIFT) & NODE_MASK);
	const double j = double(p_node & NODE_MASK);
	const double s = PlanetLattice::face_warp((2.0 * (i + 0.5)) / double(resolution) - 1.0);
	const double t = PlanetLattice::face_warp((2.0 * (j + 0.5)) / double(resolution) - 1.0);
	return PlanetLattice::cube_point(face, s, t).normalized();
}

double PlanetNavigator::_height(uint64_t p_node) {
	const int face = int(p_node >> (2 * NODE_SHIFT));
	const uint32_t i = uint32_t((p_node >> NODE_SHIFT) & NODE_MASK);
	const uint32_t j = uint32_t(p_node & NODE_MASK);
	const uint64_t key = make_node(face, i / TILE, j / TILE);
	Tile **found = tiles.getptr(key);
	Tile *tile = found != nullptr ? *found : nullptr;
	if (tile == nullptr) {
		if (tiles.size() >= 512) {
			_clear_tiles();
		}
		tile = memnew(Tile);
		for (int k = 0; k < TILE * TILE; ++k) {
			tile->filled[k] = false;
		}
		tiles.insert(key, tile);
	}
	const int k = int(j % TILE) * TILE + int(i % TILE);
	if (!tile->filled[k]) {
		tile->height[k] = _column_height(_node_dir(p_node));
		tile->filled[k] = true;
	}
	return tile->height[k];
}

// How far above the ground a column is searched for colliders standing in it: tall enough for a cliff's
// top, so that a cell on a cliff is the cliff and the slope rule keeps walkers off it.
static constexpr double COLUMN_CLEARANCE_M = 30.0;
// A step is refused when something stands between its two cells at this height.
static constexpr double KNEE_M = 0.5;

double PlanetNavigator::_column_height(const Vector3 &p_dir) const {
	const double base = ground.ground_radius(p_dir);
	if (!ground.space.is_valid()) {
		return base;
	}
	Vector3 top;
	if (ground.physics_cast(p_dir * (base + COLUMN_CLEARANCE_M), p_dir * (base - 0.5), top)) {
		return MAX(base, top.length());
	}
	return base;
}

bool PlanetNavigator::_blocked(const Vector3 &p_here, const Vector3 &p_there) const {
	if (!ground.space.is_valid()) {
		return false;
	}
	Vector3 hit;
	return ground.physics_cast(p_here + p_here.normalized() * KNEE_M, p_there + p_there.normalized() * KNEE_M, hit);
}

bool PlanetNavigator::_node_walkable(double p_height) const {
	if (ground.field.is_valid() && ground.field->has_water()) {
		return p_height > ground.field->get_sea_level_radius() + 0.1;
	}
	return true;
}

int PlanetNavigator::_neighbours(uint64_t p_node, uint64_t r_out[8]) const {
	const int face = int(p_node >> (2 * NODE_SHIFT));
	const int64_t i = int64_t((p_node >> NODE_SHIFT) & NODE_MASK);
	const int64_t j = int64_t(p_node & NODE_MASK);
	const int64_t res = int64_t(resolution);
	int n = 0;
	for (int dj = -1; dj <= 1; ++dj) {
		for (int di = -1; di <= 1; ++di) {
			if (di == 0 && dj == 0) {
				continue;
			}
			const int64_t ni = i + di;
			const int64_t nj = j + dj;
			if (ni >= 0 && ni < res && nj >= 0 && nj < res) {
				r_out[n++] = make_node(face, uint32_t(ni), uint32_t(nj));
			} else {
				// Off the face: the cube point one cell further on lies past this face's square, and its
				// direction belongs to the neighbouring face.
				const double s = PlanetLattice::face_warp((2.0 * (double(ni) + 0.5)) / double(res) - 1.0);
				const double t = PlanetLattice::face_warp((2.0 * (double(nj) + 0.5)) / double(res) - 1.0);
				const uint64_t other = _node_of(PlanetLattice::cube_point(face, s, t));
				if (other != p_node) {
					r_out[n++] = other;
				}
			}
		}
	}
	return n;
}

Vector3 PlanetNavigator::ground_point(const Vector3 &p_point) {
	const uint64_t node = _node_of(p_point);
	return _node_dir(node) * _height(node);
}

bool PlanetNavigator::is_walkable_at(const Vector3 &p_point) {
	return _node_walkable(_height(_node_of(p_point)));
}

PackedVector3Array PlanetNavigator::find_path(const Vector3 &p_from, const Vector3 &p_to) {
	PackedVector3Array path;
	last_expansions = 0;
	const uint64_t start = _node_of(p_from);
	const uint64_t goal = _node_of(p_to);
	const double h_start = _height(start);
	const double h_goal = _height(goal);
	if (!_node_walkable(h_start) || !_node_walkable(h_goal)) {
		return path;
	}
	const Vector3 goal_point = _node_dir(goal) * h_goal;
	const double max_slope = PlanetDetMath::tan(Math::deg_to_rad(max_slope_degrees));

	HashMap<uint64_t, double> g;
	HashMap<uint64_t, uint64_t> came_from;
	HashSet<uint64_t> closed;
	LocalVector<Open> open;
	g.insert(start, 0.0);
	Open first;
	first.node = start;
	first.f = (_node_dir(start) * h_start).distance_to(goal_point);
	heap_push(open, first);

	bool found = false;
	while (!open.is_empty() && last_expansions < max_expansions) {
		const Open current = heap_pop(open);
		if (closed.has(current.node)) {
			continue;
		}
		if (current.node == goal) {
			found = true;
			break;
		}
		closed.insert(current.node);
		++last_expansions;
		const double h_here = _height(current.node);
		const Vector3 here = _node_dir(current.node) * h_here;
		const double g_here = g[current.node];
		uint64_t next[8];
		const int n = _neighbours(current.node, next);
		for (int k = 0; k < n; ++k) {
			if (closed.has(next[k])) {
				continue;
			}
			const double h_there = _height(next[k]);
			if (!_node_walkable(h_there)) {
				continue;
			}
			const Vector3 there = _node_dir(next[k]) * h_there;
			// Slope over the ground distance, measured sideways between the two columns.
			const Vector3 dir_here = _node_dir(current.node);
			const Vector3 dir_there = _node_dir(next[k]);
			const double run = dir_here.distance_to(dir_there) * 0.5 * (h_here + h_there);
			const double rise = Math::abs(h_there - h_here);
			if (run <= 0.0 || rise > run * max_slope || _blocked(here, there)) {
				continue;
			}
			const double cost = here.distance_to(there) * (1.0 + slope_weight * rise / run);
			const double g_new = g_here + cost;
			const double *g_old = g.getptr(next[k]);
			if (g_old != nullptr && *g_old <= g_new) {
				continue;
			}
			g[next[k]] = g_new;
			came_from[next[k]] = current.node;
			Open item;
			item.node = next[k];
			item.f = g_new + there.distance_to(goal_point);
			heap_push(open, item);
		}
	}
	if (!found) {
		return path;
	}
	LocalVector<uint64_t> nodes;
	uint64_t at = goal;
	nodes.push_back(at);
	while (at != start) {
		at = came_from[at];
		nodes.push_back(at);
	}
	for (int k = int(nodes.size()) - 1; k >= 0; --k) {
		path.push_back(_node_dir(nodes[k]) * _height(nodes[k]));
	}
	return path;
}

void PlanetNavigator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetNavigator::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetNavigator::get_field);
	ClassDB::bind_method(D_METHOD("set_planet_radius", "radius"), &PlanetNavigator::set_planet_radius);
	ClassDB::bind_method(D_METHOD("get_planet_radius"), &PlanetNavigator::get_planet_radius);
	ClassDB::bind_method(D_METHOD("set_cell_size", "size"), &PlanetNavigator::set_cell_size);
	ClassDB::bind_method(D_METHOD("get_cell_size"), &PlanetNavigator::get_cell_size);
	ClassDB::bind_method(D_METHOD("set_max_slope_degrees", "degrees"), &PlanetNavigator::set_max_slope_degrees);
	ClassDB::bind_method(D_METHOD("get_max_slope_degrees"), &PlanetNavigator::get_max_slope_degrees);
	ClassDB::bind_method(D_METHOD("set_slope_weight", "weight"), &PlanetNavigator::set_slope_weight);
	ClassDB::bind_method(D_METHOD("get_slope_weight"), &PlanetNavigator::get_slope_weight);
	ClassDB::bind_method(D_METHOD("set_max_expansions", "count"), &PlanetNavigator::set_max_expansions);
	ClassDB::bind_method(D_METHOD("get_max_expansions"), &PlanetNavigator::get_max_expansions);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.25,20,0.01,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope_degrees", PROPERTY_HINT_RANGE, "1,89,0.1"), "set_max_slope_degrees", "get_max_slope_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_weight"), "set_slope_weight", "get_slope_weight");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_expansions"), "set_max_expansions", "get_max_expansions");

	ClassDB::bind_method(D_METHOD("add_carve", "centre", "radius"), &PlanetNavigator::add_carve);
	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &PlanetNavigator::set_physics_space);
	ClassDB::bind_method(D_METHOD("get_physics_space"), &PlanetNavigator::get_physics_space);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PlanetNavigator::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PlanetNavigator::get_collision_mask);
	ClassDB::bind_method(D_METHOD("clear_cache"), &PlanetNavigator::clear_cache);
	ClassDB::bind_method(D_METHOD("ground_at", "point"), &PlanetNavigator::ground_at);
	ClassDB::bind_method(D_METHOD("clear_carves"), &PlanetNavigator::clear_carves);
	ClassDB::bind_method(D_METHOD("find_path", "from", "to"), &PlanetNavigator::find_path);
	ClassDB::bind_method(D_METHOD("ground_point", "point"), &PlanetNavigator::ground_point);
	ClassDB::bind_method(D_METHOD("is_walkable_at", "point"), &PlanetNavigator::is_walkable_at);
	ClassDB::bind_method(D_METHOD("get_last_expansions"), &PlanetNavigator::get_last_expansions);
	ClassDB::bind_method(D_METHOD("get_cached_tiles"), &PlanetNavigator::get_cached_tiles);
}
