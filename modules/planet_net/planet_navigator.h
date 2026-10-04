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

// Path finding over the planet's ground, for NPCs (planet-demo docs/NAVIGATION.md).
//
// Nodes are cells of the cube-sphere lattice at about cell_size metres; each has one height, the
// walker's ground along its centre (f lowered by any carve). Edges are the eight neighbours, found by
// stepping in lattice space and mapping back through a direction, so a step off a cube face lands on
// the next face. A node is walkable above sea level; an edge is walkable up to the agent's slope. A*
// with deterministic tie-breaking finds the path. No meshes: a headless server builds it from f.

#include "planet_net_walker.h"

#include "core/math/planet_lattice.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"

class PlanetNavigator : public RefCounted {
	GDCLASS(PlanetNavigator, RefCounted);

public:
	static constexpr int TILE = 32;

private:
	struct Tile {
		double height[TILE * TILE];
		bool filled[TILE * TILE];
	};

	PlanetNet::Walker ground;
	double cell_size = 1.5;
	uint32_t resolution = 1;
	double max_slope_degrees = 35.0;
	double slope_weight = 2.0;
	int max_expansions = 40000;
	int last_expansions = 0;
	HashMap<uint64_t, Tile *> tiles;

	void _configure();
	void _clear_tiles();
	uint64_t _node_of(const Vector3 &p_dir) const;
	Vector3 _node_dir(uint64_t p_node) const;
	double _height(uint64_t p_node);
	double _column_height(const Vector3 &p_dir) const;
	bool _blocked(const Vector3 &p_here, const Vector3 &p_there) const;
	bool _node_walkable(double p_height) const;
	int _neighbours(uint64_t p_node, uint64_t r_out[8]) const;

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

	// Dug holes, planet space (see PlanetNetServer::add_carve). Cached heights are dropped, so paths
	// change at once.
	void add_carve(const Vector3 &p_centre, double p_radius);
	void clear_carves();

	// Colliders: with a physics space set, a cell stands on the top of whatever is in its column (a rock,
	// a cliff) and a step is refused when something stands between the two cells at knee height. The
	// space's colliders change as they stream; clear_cache() after they do.
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
	// The ground under a point, exactly there rather than at its cell's centre: what to stand a body on.
	Vector3 ground_at(const Vector3 &p_point) const {
		const Vector3 dir = p_point.normalized();
		return dir * _column_height(dir);
	}

	// Ground points from the cell containing p_from to the cell containing p_to, both included, or
	// empty when there is no walkable path within max_expansions.
	PackedVector3Array find_path(const Vector3 &p_from, const Vector3 &p_to);
	// The ground point of the cell containing a point, and whether that cell can be stood on.
	Vector3 ground_point(const Vector3 &p_point);
	bool is_walkable_at(const Vector3 &p_point);
	int get_last_expansions() const { return last_expansions; }
	int get_cached_tiles() const { return int(tiles.size()); }

	PlanetNavigator() { _configure(); }
	~PlanetNavigator() { _clear_tiles(); }
};
