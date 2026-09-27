/**************************************************************************/
/*  planet_cliff_skin_3d.h                                                */
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

#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "scene/resources/3d/planet_field_3d.h"

// The continuous rock face over every steep wall of a region: the native builder behind the demo's
// cliff_skin.gd, which keeps the tuning and passes it in as a Dictionary. Stateless apart from that
// tuning, so one instance may build regions from several threads at once.
class PlanetCliffSkin3D : public RefCounted {
	GDCLASS(PlanetCliffSkin3D, RefCounted);

public:
	struct Params {
		double step_m = 2.5;
		double blur_m = 15.0;
		double wall_slope_start = 0.75;
		double wall_slope_full = 1.1;
		double apron_slope_start = 0.35;
		double apron_slope_full = 0.0;
		double offset_m = 5.0;
		double ridge_m = 1.2;
		double ridge_period_m = 9.0;
		double ridge_stretch = 2.5;
		double buttress_m = 9.0;
		double buttress_period_m = 26.0;
		double buttress_stretch = 3.0;
		double strata_m = 1.2;
		double strata_period_m = 8.0;
		double facet_m = 0.8;
		double facet_period_m = 3.5;
		double clearance_m = 0.6;
		double lip_width_m = 14.0;
		double lip_height_m = 6.0;
		double lip_out_m = 10.0;
		double lip_lump_period_m = 7.0;
		double lump_m = 6.5;
		double lump_period_m = 14.0;
		bool vertical_face = true;
		double foot_search_m = 40.0;
		double lip_pull_in_m = 20.0;
		double lip_push_out_m = 20.0;
		double lip_shift_period_m = 20.0;
		double face_noise_m = 5.0;
		double face_noise_period_m = 20.0;
		double face_spacing_m = 1.5;
		int face_max_split = 16;
		double reference_wall_m = 85.0;
		double reference_slope = 3.0;
		double slope_scale_min = 0.5;
		double scale_min = 0.1;
		double scale_max = 1.0;
		double vertical_offset_m = 1.5;
		double max_reach_m = 60.0;
		double drama_min = 0.35;
		double drama_max = 1.8;
		double drama_period_m = 180.0;
		double jut_period_m = 38.0;
		double max_overhang_m = 24.0;
		double foot_sink_m = 4.0;
		int smooth_r = 1;
		double grass_blend_m = 3.5;
		double edge_sink_m = 2.0;
		int64_t seed_value = 7319;
	};

protected:
	static void _bind_methods();

public:
	static Params params_from_dictionary(const Dictionary &p_params);

	// Builds the face for one region; see cliff_skin.gd for the method. Vertices and collision faces
	// are relative to p_anchor. Returns {vertices, normals, indices, faces, wall_cells, mask, colors}.
	Dictionary build(const Ref<PlanetField3D> &p_field, double p_planet_radius, int p_lattice_res, double p_cell_arc,
			int p_face, int p_ci, int p_cj, int p_region_cells, const Vector3 &p_anchor, const Dictionary &p_params) const;
};
