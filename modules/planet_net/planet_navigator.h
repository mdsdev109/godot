/**************************************************************************/
/*  planet_navigator.h                                                    */
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

// Path finding for NPCs (planet-demo docs/NAVIGATION.md), on a planet or on a flat level alike.
//
// The ground is found the way the walker finds it -- by its own ray casts, against the analytic planet
// and the colliders on its layers -- so where the navigator thinks a body can stand and where the walker
// can stand are the same. No meshes and no Godot navigation server: a headless server builds it from the
// field and the physics space it already has.
//
// Cells: on a planet, cells of the cube-sphere lattice at about cell_size metres, a step off one cube
// face landing on the next; on a flat level (uniform_down set), a square grid in the plane through
// flat_origin. Each cell's column holds up to MAX_FLOORS floors: surfaces facing up, with standing room
// (agent_height) above them and not inside a collider -- the ground, the top of a rock, a slab and the
// floor beneath it, a bridge and the ground under it. A node is a cell's floor. Edges join a floor to the
// floors of the eight neighbouring cells within the agent's slope, unless something stands between them
// at knee height. A* with deterministic tie-breaking finds the path; smooth_path() then skips waypoints
// a body can walk between in a straight line.

#include "planet_net_walker.h"

#include "core/math/planet_lattice.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"

class PlanetNavigator : public RefCounted {
	GDCLASS(PlanetNavigator, RefCounted);

public:
	static constexpr int TILE = 32;
	static constexpr int MAX_FLOORS = 4;

private:
	struct Tile {
		uint8_t count[TILE * TILE];
		bool filled[TILE * TILE];
		double height[TILE * TILE * MAX_FLOORS]; // along the column's up, highest first
	};

	// A column: where a cell's (or any point's) vertical line runs. Heights are along `up`: on a planet
	// distances from the centre, on a flat level from flat_origin.
	struct Column {
		Vector3 base; // the point at height 0
		Vector3 up;
		Vector3 at(double p_h) const { return base + up * p_h; }
	};

	PlanetNet::Walker ground;
	double cell_size = 1.5;
	uint32_t resolution = 1;
	double max_slope_degrees = 35.0;
	double slope_weight = 2.0;
	double agent_height = 1.8;
	double step_height = 0.5; // smooth_path: how far the ground may sit off a straight line's
	int max_expansions = 40000;
	int last_expansions = 0;
	// A flat level: the grid's plane through flat_origin, perpendicular to -uniform_down, searched from
	// flat_top above it to flat_bottom below.
	Vector3 flat_origin;
	Vector3 flat_u;
	Vector3 flat_v;
	double flat_top = 40.0;
	double flat_bottom = -10.0;
	HashMap<uint64_t, Tile *> tiles;

	bool _flat() const { return ground.uniform_down.length_squared() > 0.0; }
	void _configure();
	void _clear_tiles();
	uint64_t _cell_of(const Vector3 &p_point) const;
	Column _column(uint64_t p_cell) const;
	Column _column_through(const Vector3 &p_point) const;
	double _height_on(const Column &p_column, const Vector3 &p_point) const;
	int _floors(const Column &p_column, double r_heights[MAX_FLOORS]) const;
	bool _standable(const Column &p_column, double p_h) const;
	int _cell_floors(uint64_t p_cell, const double **r_heights);
	int _nearest_floor(uint64_t p_cell, const Vector3 &p_point);
	Vector3 _node_point(uint64_t p_node);
	bool _blocked(const Vector3 &p_here, const Vector3 &p_there, const Vector3 &p_up) const;
	bool _node_walkable(const Vector3 &p_point) const;
	int _neighbours(uint64_t p_cell, uint64_t r_out[8]) const;
	bool _straight(const Vector3 &p_a, const Vector3 &p_b);

protected:
	static void _bind_methods();

public:
	void set_field(const Ref<PlanetField3D> &p_field);
	Ref<PlanetField3D> get_field() const { return ground.field; }
	void set_planet_radius(double p_radius);
	double get_planet_radius() const { return ground.radius; }
	void set_cell_size(double p_size);
	double get_cell_size() const { return cell_size; }
	void set_max_slope_degrees(double p_degrees) { max_slope_degrees = p_degrees; }
	double get_max_slope_degrees() const { return max_slope_degrees; }
	void set_slope_weight(double p_weight) { slope_weight = p_weight; }
	double get_slope_weight() const { return slope_weight; }
	void set_max_expansions(int p_count) { max_expansions = MAX(p_count, 1); }
	int get_max_expansions() const { return max_expansions; }
	void set_agent_height(double p_height) {
		agent_height = MAX(p_height, 0.1);
		_clear_tiles();
	}
	double get_agent_height() const { return agent_height; }
	void set_step_height(double p_height) { step_height = MAX(p_height, 0.0); }
	double get_step_height() const { return step_height; }

	// A flat level rather than a planet: down along `p_down` (zero: a planet again), the grid's plane
	// through `flat_origin`, searched from flat_top above it to flat_bottom below. The analytic ground is
	// left out on a flat level (use_terrain); its colliders are its ground.
	void set_uniform_down(const Vector3 &p_down);
	Vector3 get_uniform_down() const { return ground.uniform_down; }
	void set_flat_origin(const Vector3 &p_origin) {
		flat_origin = p_origin;
		_clear_tiles();
	}
	Vector3 get_flat_origin() const { return flat_origin; }
	void set_flat_top(double p_top) {
		flat_top = p_top;
		_clear_tiles();
	}
	double get_flat_top() const { return flat_top; }
	void set_flat_bottom(double p_bottom) {
		flat_bottom = p_bottom;
		_clear_tiles();
	}
	double get_flat_bottom() const { return flat_bottom; }
	void set_use_terrain(bool p_use) {
		ground.use_terrain = p_use;
		_clear_tiles();
	}
	bool get_use_terrain() const { return ground.use_terrain; }

	// Dug holes, planet space (see PlanetNetServer::add_carve). Cached floors are dropped, so paths
	// change at once.
	void add_carve(const Vector3 &p_centre, double p_radius);
	void clear_carves();

	// Colliders: with a physics space set, a column's floors include whatever stands in it (a rock's top,
	// a cliff, a slab and the floor under it) and a step is refused when something stands between the two
	// cells at knee height. The space's colliders change as they stream; clear_cache() after they do.
	void set_physics_space(const RID &p_space) {
		ground.space = p_space;
		_clear_tiles();
	}
	RID get_physics_space() const { return ground.space; }
	void set_collision_mask(uint32_t p_mask) {
		ground.collision_mask = p_mask;
		_clear_tiles();
	}
	uint32_t get_collision_mask() const { return ground.collision_mask; }
	void clear_cache() { _clear_tiles(); }
	// The ground under (or over) a point, exactly there rather than at its cell's centre -- the floor of
	// its own column nearest it: what to stand a body on.
	Vector3 ground_at(const Vector3 &p_point) const;
	// Every floor of the cell containing a point, highest first, as points at the cell's centre.
	PackedVector3Array get_floors(const Vector3 &p_point);

	// Floor points from the floor nearest p_from (in its cell) to the floor nearest p_to, both included,
	// or empty when there is no walkable path within max_expansions.
	PackedVector3Array find_path(const Vector3 &p_from, const Vector3 &p_to);
	// A path with every waypoint dropped that a body can walk past in a straight line: the ground along
	// the line within step_height of it, nothing in the way at knee height.
	PackedVector3Array smooth_path(const PackedVector3Array &p_path);
	// Whether a body can walk straight from one point to the other (what smooth_path asks).
	bool is_straight_walkable(const Vector3 &p_from, const Vector3 &p_to) { return _straight(p_from, p_to); }
	// The floor point of the cell containing a point nearest it, and whether that cell can be stood on.
	Vector3 ground_point(const Vector3 &p_point);
	bool is_walkable_at(const Vector3 &p_point);
	int get_last_expansions() const { return last_expansions; }
	int get_cached_tiles() const { return int(tiles.size()); }

	PlanetNavigator() { _configure(); }
	~PlanetNavigator() { _clear_tiles(); }
};
