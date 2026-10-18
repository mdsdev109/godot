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

// Node ids: floor (2 bits) | face (3 bits) | i (29 bits) | j (29 bits). On a flat level the face is
// FLAT_FACE and i, j are offset by FLAT_OFFSET so negative cells fit.
constexpr int NODE_SHIFT = 29;
constexpr uint64_t NODE_MASK = (uint64_t(1) << NODE_SHIFT) - 1;
constexpr int FLOOR_SHIFT = 61;
constexpr uint64_t CELL_MASK = (uint64_t(1) << FLOOR_SHIFT) - 1;
constexpr int FLAT_FACE = 7;
constexpr int64_t FLAT_OFFSET = int64_t(1) << 28;

inline uint64_t make_node(int p_face, uint32_t p_i, uint32_t p_j) {
	return (uint64_t(p_face) << (2 * NODE_SHIFT)) | (uint64_t(p_i) << NODE_SHIFT) | uint64_t(p_j);
}
inline int node_face(uint64_t p_node) { return int((p_node >> (2 * NODE_SHIFT)) & 7); }
inline uint32_t node_i(uint64_t p_node) { return uint32_t((p_node >> NODE_SHIFT) & NODE_MASK); }
inline uint32_t node_j(uint64_t p_node) { return uint32_t(p_node & NODE_MASK); }
inline int node_floor(uint64_t p_node) { return int(p_node >> FLOOR_SHIFT); }
inline uint64_t with_floor(uint64_t p_cell, int p_floor) { return (p_cell & CELL_MASK) | (uint64_t(p_floor) << FLOOR_SHIFT); }

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

// How far above the ground a planet's column is searched for colliders standing in it: tall enough for a
// cliff's top, so that a cell on a cliff is the cliff and the slope rule keeps walkers off it.
static constexpr double COLUMN_CLEARANCE_M = 30.0;
// A step is refused when something stands between its two cells at this height.
static constexpr double KNEE_M = 0.5;

void PlanetNavigator::_configure() {
	const double edge = ground.radius * Math::PI * 0.5;
	resolution = uint32_t(CLAMP(Math::round(edge / MAX(cell_size, 0.01)), 1.0, double(NODE_MASK)));
	if (_flat()) {
		const Vector3 up = -ground.uniform_down.normalized();
		flat_u = PlanetNet::Walker::local_north(up);
		flat_v = flat_u.cross(up).normalized();
	}
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

void PlanetNavigator::set_uniform_down(const Vector3 &p_down) {
	ground.uniform_down = p_down;
	if (_flat()) {
		ground.use_terrain = false;
	}
	_configure();
}

void PlanetNavigator::add_carve(const Vector3 &p_centre, double p_radius) {
	ground.carve_centres.push_back(p_centre);
	ground.carve_radii.push_back(p_radius);
	// Carves are rare; dropping every cached floor is simpler than finding the tiles one touches.
	_clear_tiles();
}

void PlanetNavigator::clear_carves() {
	ground.carve_centres.clear();
	ground.carve_radii.clear();
	_clear_tiles();
}

uint64_t PlanetNavigator::_cell_of(const Vector3 &p_point) const {
	if (_flat()) {
		const Vector3 d = p_point - flat_origin;
		const int64_t i = int64_t(Math::floor(d.dot(flat_u) / cell_size)) + FLAT_OFFSET;
		const int64_t j = int64_t(Math::floor(d.dot(flat_v) / cell_size)) + FLAT_OFFSET;
		return make_node(FLAT_FACE, uint32_t(CLAMP(i, int64_t(0), int64_t(NODE_MASK))), uint32_t(CLAMP(j, int64_t(0), int64_t(NODE_MASK))));
	}
	int face = 0;
	double s = 0.0;
	double t = 0.0;
	PlanetLattice::direction_to_face(p_point.normalized(), face, s, t);
	const int res = int(resolution);
	const int i = CLAMP(int(Math::floor((s + 1.0) * 0.5 * res)), 0, res - 1);
	const int j = CLAMP(int(Math::floor((t + 1.0) * 0.5 * res)), 0, res - 1);
	return make_node(face, uint32_t(i), uint32_t(j));
}

PlanetNavigator::Column PlanetNavigator::_column(uint64_t p_cell) const {
	Column c;
	if (_flat()) {
		const double i = double(int64_t(node_i(p_cell)) - FLAT_OFFSET) + 0.5;
		const double j = double(int64_t(node_j(p_cell)) - FLAT_OFFSET) + 0.5;
		c.up = -ground.uniform_down.normalized();
		c.base = flat_origin + flat_u * (i * cell_size) + flat_v * (j * cell_size);
		return c;
	}
	const double i = double(node_i(p_cell));
	const double j = double(node_j(p_cell));
	const double s = PlanetLattice::face_warp((2.0 * (i + 0.5)) / double(resolution) - 1.0);
	const double t = PlanetLattice::face_warp((2.0 * (j + 0.5)) / double(resolution) - 1.0);
	c.up = PlanetLattice::cube_point(node_face(p_cell), s, t).normalized();
	return c;
}

PlanetNavigator::Column PlanetNavigator::_column_through(const Vector3 &p_point) const {
	Column c;
	if (_flat()) {
		c.up = -ground.uniform_down.normalized();
		c.base = p_point - c.up * (p_point - flat_origin).dot(c.up);
		return c;
	}
	c.up = p_point.normalized();
	return c;
}

double PlanetNavigator::_height_on(const Column &p_column, const Vector3 &p_point) const {
	return (p_point - p_column.base).dot(p_column.up);
}

RID PlanetNavigator::_body() {
	if (!body_shape.is_valid() && agent_radius > 0.0 && PhysicsServer3D::get_singleton() != nullptr) {
		body_shape = PhysicsServer3D::get_singleton()->capsule_shape_create();
		Dictionary d;
		d["radius"] = agent_radius;
		d["height"] = MAX(agent_height - KNEE_M, agent_radius * 2.0 + 0.01);
		PhysicsServer3D::get_singleton()->shape_set_data(body_shape, d);
	}
	return body_shape;
}

// The body's capsule standing on a floor at `p_at`, upright along `p_up`: from knee height to the head.
static Transform3D body_transform(const Vector3 &p_at, const Vector3 &p_up, double p_knee, double p_height) {
	const Vector3 x = PlanetNet::Walker::local_north(p_up);
	const Basis basis(x, p_up, x.cross(p_up));
	return Transform3D(basis.orthonormalized(), p_at + p_up * (p_knee + 0.5 * (p_height - p_knee)));
}

bool PlanetNavigator::_standable(const Column &p_column, double p_h) const {
	if (!ground.space.is_valid() || PhysicsServer3D::get_singleton() == nullptr) {
		return true;
	}
	// Standing room: nothing over it within the agent's height.
	Vector3 hit;
	if (ground.physics_cast(p_column.at(p_h + 0.05), p_column.at(p_h + agent_height), hit)) {
		return false;
	}
	// Not inside a solid: a ray from inside a collider does not see it, so ask whether the body's middle
	// is in one.
	PhysicsDirectSpaceState3D *state = PhysicsServer3D::get_singleton()->space_get_direct_state(ground.space);
	if (state == nullptr) {
		return true;
	}
	PhysicsServer3DTypes::PointParameters params;
	params.position = p_column.at(p_h + agent_height * 0.5);
	params.exclude = ground.exclude;
	params.collision_mask = ground.collision_mask;
	PhysicsServer3DTypes::ShapeResult result;
	if (state->intersect_point(params, &result, 1) != 0) {
		return false;
	}
	// Room round it: the body's capsule, knee to head, touching nothing.
	if (agent_radius > 0.0) {
		const RID shape = const_cast<PlanetNavigator *>(this)->_body();
		if (shape.is_valid()) {
			PhysicsServer3DTypes::ShapeParameters sp;
			sp.shape_rid = shape;
			sp.transform = body_transform(p_column.at(p_h), p_column.up, KNEE_M, agent_height);
			sp.exclude = ground.exclude;
			sp.collision_mask = ground.collision_mask;
			PhysicsServer3DTypes::ShapeResult hit;
			if (state->intersect_shape(sp, &hit, 1) != 0) {
				return false;
			}
		}
	}
	return true;
}

int PlanetNavigator::_floors(const Column &p_column, double r_heights[MAX_FLOORS]) const {
	const bool flat = _flat();
	const double base = flat ? 0.0 : ground.ground_radius(p_column.up);
	const double top = flat ? flat_top : base + COLUMN_CLEARANCE_M;
	const double bottom = flat ? flat_bottom : base - 0.5;
	double found[8];
	int n = 0;
	if (ground.space.is_valid()) {
		// Down the column, surface after surface: a ray from inside a collider passes out of it, so the
		// next cast from just under a top finds what lies beneath -- a slab's top, then the floor under it.
		double from = top;
		for (int k = 0; k < 8 && from > bottom; ++k) {
			Vector3 hit;
			Vector3 normal;
			if (!ground.physics_cast(p_column.at(from), p_column.at(bottom), hit, &normal)) {
				break;
			}
			const double h = _height_on(p_column, hit);
			// Facing up as a floor does; not under the planet's ground (solid there); room to stand.
			// A trimesh's back face just under its top reports a normal facing the ray as well, and is
			// left out by the standing room it lacks.
			if (normal.dot(p_column.up) >= PlanetNet::Walker::COS_WALK && (flat || h >= base - 0.05) && _standable(p_column, h)) {
				found[n++] = h;
			}
			from = h - 0.05;
		}
	}
	if (!flat) {
		// The planet's own ground, unless a collider's floor is it, or covers it, or it is under one.
		bool covered = false;
		for (int k = 0; k < n; ++k) {
			covered = covered || Math::abs(found[k] - base) < 0.1;
		}
		if (!covered && n < 8 && _standable(p_column, base)) {
			found[n++] = base;
		}
	}
	// Highest first; the top MAX_FLOORS.
	for (int a = 1; a < n; ++a) {
		for (int b = a; b > 0 && found[b] > found[b - 1]; --b) {
			SWAP(found[b], found[b - 1]);
		}
	}
	const int count = MIN(n, MAX_FLOORS);
	for (int k = 0; k < count; ++k) {
		r_heights[k] = found[k];
	}
	return count;
}

int PlanetNavigator::_cell_floors(uint64_t p_cell, const double **r_heights) {
	const uint64_t cell = p_cell & CELL_MASK;
	const uint64_t key = make_node(node_face(cell), node_i(cell) / TILE, node_j(cell) / TILE);
	Tile **got = tiles.getptr(key);
	Tile *tile = got != nullptr ? *got : nullptr;
	if (tile == nullptr) {
		if (tiles.size() >= 512) {
			_clear_tiles();
		}
		tile = memnew(Tile);
		for (int k = 0; k < TILE * TILE; ++k) {
			tile->filled[k] = false;
			tile->count[k] = 0;
		}
		tiles.insert(key, tile);
	}
	const int k = int(node_j(cell) % TILE) * TILE + int(node_i(cell) % TILE);
	if (!tile->filled[k]) {
		tile->count[k] = uint8_t(_floors(_column(cell), &tile->height[k * MAX_FLOORS]));
		tile->filled[k] = true;
	}
	*r_heights = &tile->height[k * MAX_FLOORS];
	return tile->count[k];
}

int PlanetNavigator::_nearest_floor(uint64_t p_cell, const Vector3 &p_point) {
	const double *h = nullptr;
	const int n = _cell_floors(p_cell, &h);
	const double at = _height_on(_column(p_cell), p_point);
	int best = -1;
	double best_d = 0.0;
	for (int k = 0; k < n; ++k) {
		const double d = Math::abs(h[k] - at);
		if (best < 0 || d < best_d) {
			best = k;
			best_d = d;
		}
	}
	return best;
}

Vector3 PlanetNavigator::_node_point(uint64_t p_node) {
	const double *h = nullptr;
	const int n = _cell_floors(p_node, &h);
	const int f = MIN(node_floor(p_node), MAX(n - 1, 0));
	return _column(p_node).at(n > 0 ? h[f] : 0.0);
}

bool PlanetNavigator::_blocked(const Vector3 &p_here, const Vector3 &p_there, const Vector3 &p_up) const {
	if (!ground.space.is_valid()) {
		return false;
	}
	Vector3 hit;
	if (_flat()) {
		return ground.physics_cast(p_here + p_up * KNEE_M, p_there + p_up * KNEE_M, hit);
	}
	return ground.physics_cast(p_here + p_here.normalized() * KNEE_M, p_there + p_there.normalized() * KNEE_M, hit);
}

bool PlanetNavigator::_node_walkable(const Vector3 &p_point) const {
	if (!_flat() && ground.field.is_valid() && ground.field->has_water()) {
		return p_point.length() > ground.field->get_sea_level_radius() + 0.1;
	}
	return true;
}

int PlanetNavigator::_neighbours(uint64_t p_cell, uint64_t r_out[8]) const {
	const uint64_t cell = p_cell & CELL_MASK;
	const int face = node_face(cell);
	const int64_t i = int64_t(node_i(cell));
	const int64_t j = int64_t(node_j(cell));
	int n = 0;
	if (face == FLAT_FACE) {
		for (int dj = -1; dj <= 1; ++dj) {
			for (int di = -1; di <= 1; ++di) {
				if (di != 0 || dj != 0) {
					r_out[n++] = make_node(FLAT_FACE, uint32_t(i + di), uint32_t(j + dj));
				}
			}
		}
		return n;
	}
	const int64_t res = int64_t(resolution);
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
				const uint64_t other = _cell_of(PlanetLattice::cube_point(face, s, t));
				if (other != cell) {
					r_out[n++] = other;
				}
			}
		}
	}
	return n;
}

Vector3 PlanetNavigator::ground_at(const Vector3 &p_point) const {
	const Column c = _column_through(p_point);
	double h[MAX_FLOORS];
	const int n = _floors(c, h);
	if (n == 0) {
		return _flat() ? p_point : p_point.normalized() * ground.ground_radius(p_point.normalized());
	}
	const double at = _height_on(c, p_point);
	int best = 0;
	for (int k = 1; k < n; ++k) {
		if (Math::abs(h[k] - at) < Math::abs(h[best] - at)) {
			best = k;
		}
	}
	return c.at(h[best]);
}

PackedVector3Array PlanetNavigator::get_floors(const Vector3 &p_point) {
	PackedVector3Array out;
	const uint64_t cell = _cell_of(p_point);
	const double *h = nullptr;
	const int n = _cell_floors(cell, &h);
	const Column c = _column(cell);
	for (int k = 0; k < n; ++k) {
		out.push_back(c.at(h[k]));
	}
	return out;
}

Vector3 PlanetNavigator::ground_point(const Vector3 &p_point) {
	const uint64_t cell = _cell_of(p_point);
	const int f = _nearest_floor(cell, p_point);
	return f >= 0 ? _node_point(with_floor(cell, f)) : p_point;
}

bool PlanetNavigator::is_walkable_at(const Vector3 &p_point) {
	const uint64_t cell = _cell_of(p_point);
	const int f = _nearest_floor(cell, p_point);
	return f >= 0 && _node_walkable(_node_point(with_floor(cell, f)));
}

PackedVector3Array PlanetNavigator::find_path(const Vector3 &p_from, const Vector3 &p_to) {
	PackedVector3Array path;
	last_expansions = 0;
	const uint64_t start_cell = _cell_of(p_from);
	const uint64_t goal_cell = _cell_of(p_to);
	const int start_floor = _nearest_floor(start_cell, p_from);
	const int goal_floor = _nearest_floor(goal_cell, p_to);
	if (start_floor < 0 || goal_floor < 0) {
		return path;
	}
	const uint64_t start = with_floor(start_cell, start_floor);
	const uint64_t goal = with_floor(goal_cell, goal_floor);
	const Vector3 start_point = _node_point(start);
	const Vector3 goal_point = _node_point(goal);
	if (!_node_walkable(start_point) || !_node_walkable(goal_point)) {
		return path;
	}
	const double max_slope = PlanetDetMath::tan(Math::deg_to_rad(max_slope_degrees));
	const bool flat = _flat();

	HashMap<uint64_t, double> g;
	HashMap<uint64_t, uint64_t> came_from;
	HashSet<uint64_t> closed;
	LocalVector<Open> open;
	g.insert(start, 0.0);
	Open first;
	first.node = start;
	first.f = start_point.distance_to(goal_point);
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
		const Column col_here = _column(current.node);
		const Vector3 here = _node_point(current.node);
		const double h_here = _height_on(col_here, here);
		const double g_here = g[current.node];
		uint64_t next_cells[8];
		const int n = _neighbours(current.node, next_cells);
		for (int k = 0; k < n; ++k) {
			const double *heights = nullptr;
			const int floors = _cell_floors(next_cells[k], &heights);
			const Column col_there = _column(next_cells[k]);
			for (int f = 0; f < floors; ++f) {
				const uint64_t next = with_floor(next_cells[k], f);
				if (closed.has(next)) {
					continue;
				}
				const Vector3 there = col_there.at(heights[f]);
				if (!_node_walkable(there)) {
					continue;
				}
				// Slope over the ground distance, measured sideways between the two columns.
				const double h_there = heights[f];
				const double run = flat ? col_here.base.distance_to(col_there.base)
										: col_here.up.distance_to(col_there.up) * 0.5 * (h_here + h_there);
				const double rise = Math::abs(h_there - h_here);
				if (run <= 0.0 || rise > run * max_slope || _blocked(here, there, col_here.up)) {
					continue;
				}
				const double cost = here.distance_to(there) * (1.0 + slope_weight * rise / run);
				const double g_new = g_here + cost;
				const double *g_old = g.getptr(next);
				if (g_old != nullptr && *g_old <= g_new) {
					continue;
				}
				g[next] = g_new;
				came_from[next] = current.node;
				Open item;
				item.node = next;
				item.f = g_new + there.distance_to(goal_point);
				heap_push(open, item);
			}
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
		path.push_back(_node_point(nodes[k]));
	}
	return path;
}

bool PlanetNavigator::_straight(const Vector3 &p_a, const Vector3 &p_b) {
	const bool flat = _flat();
	const Vector3 up_a = _column_through(p_a).up;
	if (_blocked(p_a, p_b, up_a)) {
		return false;
	}
	// The body swept along the line: with room round it, a leg that passes a wall's corner keeps clear.
	if (agent_radius > 0.0 && ground.space.is_valid() && PhysicsServer3D::get_singleton() != nullptr) {
		PhysicsDirectSpaceState3D *state = PhysicsServer3D::get_singleton()->space_get_direct_state(ground.space);
		const RID shape = _body();
		if (state != nullptr && shape.is_valid()) {
			PhysicsServer3DTypes::ShapeParameters sp;
			sp.shape_rid = shape;
			const Vector3 lift = up_a * 0.05; // off the floor it stands on, which a step down would meet
			sp.transform = body_transform(p_a + lift, up_a, KNEE_M, agent_height);
			sp.motion = p_b - p_a;
			sp.exclude = ground.exclude;
			sp.collision_mask = ground.collision_mask;
			real_t safe = 1.0;
			real_t unsafe = 1.0;
			if (state->cast_motion(sp, safe, unsafe) && safe < 1.0) {
				return false;
			}
		}
	}
	// Along the line in steps of half a cell, following the ground from floor to floor: each step within
	// the agent's slope of the last, and the end on the floor b is on.
	const double max_slope = PlanetDetMath::tan(Math::deg_to_rad(max_slope_degrees));
	const Column ca = _column_through(p_a);
	const Column cb = _column_through(p_b);
	const double ha = _height_on(ca, p_a);
	const double hb = _height_on(cb, p_b);
	Vector3 flat_d = p_b - p_a;
	flat_d -= up_a * flat_d.dot(up_a);
	const double dist = flat_d.length();
	const double piece = cell_size * 0.5;
	const int steps = MAX(1, int(Math::ceil(dist / piece)));
	double prev = ha;
	for (int s = 1; s <= steps; ++s) {
		const double t = double(s) / double(steps);
		const Vector3 p = flat ? p_a.lerp(p_b, t) : p_a.normalized().lerp(p_b.normalized(), t).normalized() * Math::lerp(p_a.length(), p_b.length(), t);
		const Column c = _column_through(p);
		double h[MAX_FLOORS];
		const int n = _floors(c, h);
		int best = -1;
		for (int k = 0; k < n; ++k) {
			if (best < 0 || Math::abs(h[k] - prev) < Math::abs(h[best] - prev)) {
				best = k;
			}
		}
		if (best < 0 || Math::abs(h[best] - prev) > MAX(step_height, (dist / steps) * max_slope) || !_node_walkable(c.at(h[best]))) {
			return false;
		}
		prev = h[best];
	}
	return Math::abs(prev - hb) <= step_height + 0.05;
}

PackedVector3Array PlanetNavigator::smooth_path(const PackedVector3Array &p_path) {
	if (p_path.size() <= 2) {
		return p_path;
	}
	PackedVector3Array out;
	out.push_back(p_path[0]);
	int at = 0;
	const int n = p_path.size();
	while (at < n - 1) {
		// The furthest waypoint walkable to in a straight line (the next one always is).
		int reach = at + 1;
		for (int k = n - 1; k > at + 1; --k) {
			if (_straight(p_path[at], p_path[k])) {
				reach = k;
				break;
			}
		}
		out.push_back(p_path[reach]);
		at = reach;
	}
	return out;
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
	ClassDB::bind_method(D_METHOD("set_agent_height", "height"), &PlanetNavigator::set_agent_height);
	ClassDB::bind_method(D_METHOD("get_agent_height"), &PlanetNavigator::get_agent_height);
	ClassDB::bind_method(D_METHOD("set_step_height", "height"), &PlanetNavigator::set_step_height);
	ClassDB::bind_method(D_METHOD("get_step_height"), &PlanetNavigator::get_step_height);
	ClassDB::bind_method(D_METHOD("set_agent_radius", "radius"), &PlanetNavigator::set_agent_radius);
	ClassDB::bind_method(D_METHOD("get_agent_radius"), &PlanetNavigator::get_agent_radius);
	ClassDB::bind_method(D_METHOD("set_uniform_down", "down"), &PlanetNavigator::set_uniform_down);
	ClassDB::bind_method(D_METHOD("get_uniform_down"), &PlanetNavigator::get_uniform_down);
	ClassDB::bind_method(D_METHOD("set_flat_origin", "origin"), &PlanetNavigator::set_flat_origin);
	ClassDB::bind_method(D_METHOD("get_flat_origin"), &PlanetNavigator::get_flat_origin);
	ClassDB::bind_method(D_METHOD("set_flat_top", "top"), &PlanetNavigator::set_flat_top);
	ClassDB::bind_method(D_METHOD("get_flat_top"), &PlanetNavigator::get_flat_top);
	ClassDB::bind_method(D_METHOD("set_flat_bottom", "bottom"), &PlanetNavigator::set_flat_bottom);
	ClassDB::bind_method(D_METHOD("get_flat_bottom"), &PlanetNavigator::get_flat_bottom);
	ClassDB::bind_method(D_METHOD("set_use_terrain", "use"), &PlanetNavigator::set_use_terrain);
	ClassDB::bind_method(D_METHOD("get_use_terrain"), &PlanetNavigator::get_use_terrain);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cell_size", PROPERTY_HINT_RANGE, "0.25,20,0.01,suffix:m"), "set_cell_size", "get_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_slope_degrees", PROPERTY_HINT_RANGE, "1,89,0.1"), "set_max_slope_degrees", "get_max_slope_degrees");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "slope_weight"), "set_slope_weight", "get_slope_weight");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_expansions"), "set_max_expansions", "get_max_expansions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_height", PROPERTY_HINT_NONE, "suffix:m"), "set_agent_height", "get_agent_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "step_height", PROPERTY_HINT_NONE, "suffix:m"), "set_step_height", "get_step_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_agent_radius", "get_agent_radius");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uniform_down"), "set_uniform_down", "get_uniform_down");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "flat_origin"), "set_flat_origin", "get_flat_origin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flat_top", PROPERTY_HINT_NONE, "suffix:m"), "set_flat_top", "get_flat_top");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "flat_bottom", PROPERTY_HINT_NONE, "suffix:m"), "set_flat_bottom", "get_flat_bottom");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_terrain"), "set_use_terrain", "get_use_terrain");

	ClassDB::bind_method(D_METHOD("add_carve", "centre", "radius"), &PlanetNavigator::add_carve);
	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &PlanetNavigator::set_physics_space);
	ClassDB::bind_method(D_METHOD("get_physics_space"), &PlanetNavigator::get_physics_space);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PlanetNavigator::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PlanetNavigator::get_collision_mask);
	ClassDB::bind_method(D_METHOD("clear_cache"), &PlanetNavigator::clear_cache);
	ClassDB::bind_method(D_METHOD("ground_at", "point"), &PlanetNavigator::ground_at);
	ClassDB::bind_method(D_METHOD("get_floors", "point"), &PlanetNavigator::get_floors);
	ClassDB::bind_method(D_METHOD("clear_carves"), &PlanetNavigator::clear_carves);
	ClassDB::bind_method(D_METHOD("find_path", "from", "to"), &PlanetNavigator::find_path);
	ClassDB::bind_method(D_METHOD("smooth_path", "path"), &PlanetNavigator::smooth_path);
	ClassDB::bind_method(D_METHOD("is_straight_walkable", "from", "to"), &PlanetNavigator::is_straight_walkable);
	ClassDB::bind_method(D_METHOD("ground_point", "point"), &PlanetNavigator::ground_point);
	ClassDB::bind_method(D_METHOD("in_sight", "from", "to", "slack"), &PlanetNavigator::in_sight, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("can_cast_from", "feet", "point"), &PlanetNavigator::can_cast_from);
	ClassDB::bind_method(D_METHOD("is_walkable_at", "point"), &PlanetNavigator::is_walkable_at);
	ClassDB::bind_method(D_METHOD("get_last_expansions"), &PlanetNavigator::get_last_expansions);
	ClassDB::bind_method(D_METHOD("get_cached_tiles"), &PlanetNavigator::get_cached_tiles);
}
