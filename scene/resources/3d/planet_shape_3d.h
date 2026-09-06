/**************************************************************************/
/*  planet_shape_3d.h                                                     */
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

#include "scene/resources/3d/planet_field_3d.h"
#include "scene/resources/3d/shape_3d.h"

class ArrayMesh;

// A whole-planet static collision surface defined analytically as r = f(direction), with no stored,
// streamed or chunked geometry. Collision triangles are generated inside the narrow phase, used, and
// discarded, the same way JPH::HeightFieldShape works but on a spherical parameterization.
//
// Jolt Physics only. Other backends error on creation.
//
// Phase 2: the field is wired up and the shape reports real bounds derived from it. Collision itself
// still does nothing -- that is Phase 3.
class PlanetShape3D : public Shape3D {
	GDCLASS(PlanetShape3D, Shape3D);

	// The definition of f. Radius bounds are derived from it rather than authored: an authored bound
	// that does not contain f silently culls real contacts, so there is no reason to let the two
	// drift apart.
	Ref<PlanetField3D> field;

	// Which patch of the planet this shape covers, and how finely the planet is divided.
	//
	// The planet is many static bodies, one per patch, rather than one body. That is a precision
	// requirement, not an optimization: Jolt narrows the body-relative transform to float32, so the
	// distance between the colliding body and the planet body origin sets the achievable accuracy. One
	// planet-centred body puts the whole radius through float32 (0.38 m granularity at 3,185,500 m, and
	// a resting sphere then hovers and jitters by about that). Splitting into patches makes that
	// distance patch-sized instead.
	int face = 0;
	int patch_i = 0;
	int patch_j = 0;
	int patches_per_axis = 1;

	void _field_changed();

protected:
	static void _bind_methods();

	virtual void _update_shape() override;

public:
	void set_field(const Ref<PlanetField3D> &p_field);
	Ref<PlanetField3D> get_field() const;

	void set_face(int p_face);
	int get_face() const;
	void set_patch_i(int p_i);
	int get_patch_i() const;
	void set_patch_j(int p_j);
	int get_patch_j() const;
	void set_patches_per_axis(int p_n);
	int get_patches_per_axis() const;

	// Narrow-phase instrumentation, for profiling against the design budget. Static: the counters are
	// global to the planet collision path, not per shape.
	static Dictionary get_collision_stats();
	static void reset_collision_stats();

	// The lattice resolution the physics backend will actually use. Exposed so callers cannot drift
	// from it: a renderer that computes its own resolution stops sharing vertices with the collider,
	// and feet no longer land on the surface.
	int get_lattice_resolution() const;

	// Where the body holding this shape must be positioned. Anything else shifts the patch off the
	// planet, because the shape emits its geometry relative to exactly this point.
	Vector3 get_patch_origin() const;

	virtual Vector<Vector3> get_debug_mesh_lines() const override;
	virtual Ref<ArrayMesh> get_debug_arraymesh_faces(const Color &p_modulate) const override;
	virtual real_t get_enclosing_radius() const override;

	PlanetShape3D();
};
