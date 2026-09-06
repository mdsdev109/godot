/**************************************************************************/
/*  jolt_custom_planet_shape.cpp                                          */
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

#include "jolt_custom_planet_shape.h"

#include <chrono>

#include <Jolt/Geometry/RayTriangle.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/CastConvexVsTriangles.h>
#include <Jolt/Physics/Collision/CollideConvexVsTriangles.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>

#ifdef JPH_DEBUG_RENDERER
#include <Jolt/Renderer/DebugRenderer.h>
#endif

namespace {

// Accumulates elapsed time into a counter on scope exit, so early returns are measured too.
struct ScopedNanoTimer {
	std::atomic<uint64_t> &counter;
	std::chrono::steady_clock::time_point start;

	explicit ScopedNanoTimer(std::atomic<uint64_t> &p_counter) :
			counter(p_counter), start(std::chrono::steady_clock::now()) {}

	~ScopedNanoTimer() {
		counter.fetch_add(
				uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
				std::memory_order_relaxed);
	}
};

JPH::Shape *construct_planet() {
	return new JoltCustomPlanetShape();
}

// Convex shape (1) against the planet patch (2).
//
// This mirrors HeightFieldShape::sCollideConvexVsHeightField: build a CollideConvexVsTriangles
// visitor, work out which lattice cells the query box can touch, tessellate just those, and feed the
// triangles in. The triangles are discarded when this returns.
void collide_convex_vs_planet(
		const JPH::Shape *p_shape1,
		const JPH::Shape *p_shape2,
		JPH::Vec3Arg p_scale1,
		JPH::Vec3Arg p_scale2,
		JPH::Mat44Arg p_center_of_mass_transform1,
		JPH::Mat44Arg p_center_of_mass_transform2,
		const JPH::SubShapeIDCreator &p_sub_shape_id_creator1,
		const JPH::SubShapeIDCreator &p_sub_shape_id_creator2,
		const JPH::CollideShapeSettings &p_collide_shape_settings,
		JPH::CollideShapeCollector &p_collector,
		const JPH::ShapeFilter &p_shape_filter) {
	ERR_FAIL_COND(p_shape1->GetType() != JPH::EShapeType::Convex);
	ERR_FAIL_COND(p_shape2->GetSubType() != JoltCustomShapeSubType::PLANET);

	const JPH::ConvexShape *shape1 = static_cast<const JPH::ConvexShape *>(p_shape1);
	const JoltCustomPlanetShape *shape2 = static_cast<const JoltCustomPlanetShape *>(p_shape2);

	// Exposes the query box in the planet's local space, which the base class computes anyway.
	struct Visitor : public JPH::CollideConvexVsTriangles {
		using JPH::CollideConvexVsTriangles::CollideConvexVsTriangles;
		const JPH::AABox &get_bounds_of_1_in_space_of_2() const { return mBoundsOf1InSpaceOf2; }
		bool should_abort() const { return mCollector.ShouldEarlyOut(); }
	};

	Visitor visitor(
			shape1,
			p_scale1,
			p_scale2,
			p_center_of_mass_transform1,
			p_center_of_mass_transform2,
			p_sub_shape_id_creator1.GetID(),
			p_collide_shape_settings,
			p_collector);

	// Non-uniform scale would mean the lattice no longer maps to a sphere and every bound here stops
	// holding. Refuse loudly rather than producing silently wrong contacts.
	if (!p_scale2.IsClose(JPH::Vec3::sReplicate(p_scale2.GetX()), 1.0e-6f)) {
		return;
	}

	const JPH::AABox &query_box = visitor.get_bounds_of_1_in_space_of_2();

	PlanetCollisionStats::add(PlanetCollisionStats::collide_queries, 1);
	// Scoped so the early-out path is timed too; a reject that is not counted would flatter the
	// measurement exactly where the design depends on rejects being cheap.
	const ScopedNanoTimer profile_timer(PlanetCollisionStats::nanoseconds);

	// Radial reject before anything else. Most bodies near a planet are well above or well below the
	// ground, and this discards them for a few dozen float operations.
	if (!shape2->box_straddles_surface(query_box)) {
		PlanetCollisionStats::add(PlanetCollisionStats::rejected_queries, 1);
		return;
	}

	const JPH::SubShapeIDCreator sub_shape_id_creator2 = p_sub_shape_id_creator2;
	const JPH::uint id_bits = shape2->GetSubShapeIDBitsRecursive();

	// Each shape owns cells on exactly one face, so there is no face loop. A body straddling a face
	// seam is still covered, because the neighbouring face has its own patch body.
	uint32_t min_i = 0;
	uint32_t min_j = 0;
	uint32_t max_i = 0;
	uint32_t max_j = 0;
	if (!shape2->get_cell_range_for_box(query_box, shape2->face, min_i, min_j, max_i, max_j)) {
		PlanetCollisionStats::add(PlanetCollisionStats::rejected_queries, 1);
		return;
	}

	shape2->tessellate_cells(
			shape2->face, min_i, min_j, max_i, max_j,
			[&](JPH::Vec3Arg p_v0, JPH::Vec3Arg p_v1, JPH::Vec3Arg p_v2, uint8_t p_active_edges, uint32_t p_cell_id) {
				if (visitor.should_abort()) {
					return;
				}
				const JPH::SubShapeID sub_shape_id = sub_shape_id_creator2.PushID(p_cell_id, id_bits).GetID();
				visitor.Collide(p_v0, p_v1, p_v2, p_active_edges, sub_shape_id);
			});
}

// Sweeping a convex shape against the planet. Same structure as the collide path: find the cells the
// swept volume can touch, tessellate them, feed the triangles in, discard them.
void cast_convex_vs_planet(const JPH::ShapeCast &p_shape_cast, const JPH::ShapeCastSettings &p_shape_cast_settings, const JPH::Shape *p_shape, JPH::Vec3Arg p_scale, const JPH::ShapeFilter &p_shape_filter, JPH::Mat44Arg p_center_of_mass_transform2, const JPH::SubShapeIDCreator &p_sub_shape_id_creator1, const JPH::SubShapeIDCreator &p_sub_shape_id_creator2, JPH::CastShapeCollector &p_collector) {
	ERR_FAIL_COND(p_shape->GetSubType() != JoltCustomShapeSubType::PLANET);
	const JoltCustomPlanetShape *shape = static_cast<const JoltCustomPlanetShape *>(p_shape);

	PlanetCollisionStats::add(PlanetCollisionStats::cast_queries, 1);

	JPH::CastConvexVsTriangles caster(p_shape_cast, p_shape_cast_settings, p_scale, p_center_of_mass_transform2, p_sub_shape_id_creator1, p_collector);

	// Swept bounds: the cast shape bounds at the start, unioned with the same box at the end.
	JPH::AABox swept = p_shape_cast.mShapeWorldBounds;
	swept.Encapsulate(JPH::AABox(
			p_shape_cast.mShapeWorldBounds.mMin + p_shape_cast.mDirection,
			p_shape_cast.mShapeWorldBounds.mMax + p_shape_cast.mDirection));

	if (!shape->box_straddles_surface(swept)) {
		return;
	}

	uint32_t min_i = 0;
	uint32_t min_j = 0;
	uint32_t max_i = 0;
	uint32_t max_j = 0;
	if (!shape->get_cell_range_for_box(swept, shape->face, min_i, min_j, max_i, max_j)) {
		return;
	}

	const JPH::SubShapeIDCreator sub_shape_id_creator2 = p_sub_shape_id_creator2;
	const JPH::uint id_bits = shape->GetSubShapeIDBitsRecursive();

	shape->tessellate_cells(
			shape->face, min_i, min_j, max_i, max_j,
			[&](JPH::Vec3Arg p_v0, JPH::Vec3Arg p_v1, JPH::Vec3Arg p_v2, uint8_t p_active_edges, uint32_t p_cell_id) {
				if (p_collector.ShouldEarlyOut()) {
					return;
				}
				caster.Cast(p_v0, p_v1, p_v2, p_active_edges, sub_shape_id_creator2.PushID(p_cell_id, id_bits).GetID());
			});
}

// Planet vs planet: both are static, so Jolt never asks. Registered anyway so the table has no hole
// if that assumption ever changes.
void cast_noop(const JPH::ShapeCast &p_shape_cast, const JPH::ShapeCastSettings &p_shape_cast_settings, const JPH::Shape *p_shape, JPH::Vec3Arg p_scale, const JPH::ShapeFilter &p_shape_filter, JPH::Mat44Arg p_center_of_mass_transform2, const JPH::SubShapeIDCreator &p_sub_shape_id_creator1, const JPH::SubShapeIDCreator &p_sub_shape_id_creator2, JPH::CastShapeCollector &p_collector) {
}

void collide_noop(const JPH::Shape *p_shape1, const JPH::Shape *p_shape2, JPH::Vec3Arg p_scale1, JPH::Vec3Arg p_scale2, JPH::Mat44Arg p_center_of_mass_transform1, JPH::Mat44Arg p_center_of_mass_transform2, const JPH::SubShapeIDCreator &p_sub_shape_id_creator1, const JPH::SubShapeIDCreator &p_sub_shape_id_creator2, const JPH::CollideShapeSettings &p_collide_shape_settings, JPH::CollideShapeCollector &p_collector, const JPH::ShapeFilter &p_shape_filter) {
}

} // namespace

JPH::ShapeSettings::ShapeResult JoltCustomPlanetShapeSettings::Create() const {
	if (mCachedResult.IsEmpty()) {
		new JoltCustomPlanetShape(*this, mCachedResult);
	}

	return mCachedResult;
}

// The query box arrives in this shape local space, which is offset by patch_origin. Everything below
// reasons about directions from the planet centre, so shift it back first.
JPH::AABox JoltCustomPlanetShape::to_planet_space(const JPH::AABox &p_box) const {
	const JPH::Vec3 offset(float(patch_origin.x), float(patch_origin.y), float(patch_origin.z));
	return JPH::AABox(p_box.mMin + offset, p_box.mMax + offset);
}

bool JoltCustomPlanetShape::box_straddles_surface(const JPH::AABox &p_box) const {
	const JPH::AABox box = to_planet_space(p_box);
	// Closest and furthest distance from the planet centre to the box.
	const JPH::Vec3 closest = JPH::Vec3::sMax(box.mMin, JPH::Vec3::sMin(JPH::Vec3::sZero(), box.mMax));
	const float near_dist = closest.Length();
	const float far_dist = JPH::Vec3::sMax(box.mMin.Abs(), box.mMax.Abs()).Length();
	return far_dist >= patch_min_radius && near_dist <= patch_max_radius;
}

JPH::AABox JoltCustomPlanetShape::GetLocalBounds() const {
	// Corners, edge midpoints and centre of the patch, at both radius extremes, relative to
	// patch_origin. The subtraction is in double for the same reason the tessellator does it that way.
	JPH::Vec3 lo = JPH::Vec3::sReplicate(FLT_MAX);
	JPH::Vec3 hi = JPH::Vec3::sReplicate(-FLT_MAX);

	const uint32_t is[3] = { cell_min_i, uint32_t((uint64_t(cell_min_i) + uint64_t(cell_max_i)) / 2), cell_max_i };
	const uint32_t js[3] = { cell_min_j, uint32_t((uint64_t(cell_min_j) + uint64_t(cell_max_j)) / 2), cell_max_j };

	for (int a = 0; a < 3; ++a) {
		for (int b = 0; b < 3; ++b) {
			const Vector3 d = lattice.vertex_direction(face, is[a], js[b]);
			for (int e = 0; e < 2; ++e) {
				const double r = (e == 0) ? double(patch_min_radius) : double(patch_max_radius);
				const JPH::Vec3 pt(
						float(d.x * r - patch_origin.x),
						float(d.y * r - patch_origin.y),
						float(d.z * r - patch_origin.z));
				lo = JPH::Vec3::sMin(lo, pt);
				hi = JPH::Vec3::sMax(hi, pt);
			}
		}
	}

	// The arc bulges out of the hull of the sampled points; pad by the sagitta between two adjacent
	// samples, which is the largest such bulge.
	const Vector3 c0 = lattice.vertex_direction(face, is[0], js[0]);
	const Vector3 c1 = lattice.vertex_direction(face, is[1], js[1]);
	const double half_angle = 0.5 * Math::acos(CLAMP(c0.dot(c1), -1.0, 1.0));
	const float sagitta = float(double(patch_max_radius) * (1.0 - Math::cos(half_angle)));
	const JPH::Vec3 pad = JPH::Vec3::sReplicate(sagitta + 1.0f);

	return JPH::AABox(lo - pad, hi + pad);
}

bool JoltCustomPlanetShape::get_cell_range_for_box(
		const JPH::AABox &p_box,
		int p_face,
		uint32_t &r_min_i,
		uint32_t &r_min_j,
		uint32_t &r_max_i,
		uint32_t &r_max_j) const {
	const JPH::AABox planet_box = to_planet_space(p_box);
	const JPH::Vec3 box_min = planet_box.mMin;
	const JPH::Vec3 box_max = planet_box.mMax;
	const JPH::Vec3 closest = JPH::Vec3::sMax(box_min, JPH::Vec3::sMin(JPH::Vec3::sZero(), box_max));
	const float near_dist = closest.Length();

	// Direction extent of the box. Every corner is mapped to its cube face; corners landing on a
	// different face than this patch are ignored, but the resulting range is then padded by the box's
	// angular size so a query straddling a face seam still picks up this patch's cells.
	double s_lo = INFINITY;
	double s_hi = -INFINITY;
	double t_lo = INFINITY;
	double t_hi = -INFINITY;
	bool any_on_face = false;
	bool all_on_face = true;

	for (int c = 0; c < 8; ++c) {
		const Vector3 corner(
				(c & 1) ? double(box_max.GetX()) : double(box_min.GetX()),
				(c & 2) ? double(box_max.GetY()) : double(box_min.GetY()),
				(c & 4) ? double(box_max.GetZ()) : double(box_min.GetZ()));
		if (corner.length_squared() <= 0.0) {
			// The box contains the planet centre; no meaningful direction range.
			return false;
		}
		int corner_face = 0;
		double s = 0.0;
		double t = 0.0;
		PlanetLattice::direction_to_face(corner, corner_face, s, t);
		if (corner_face != p_face) {
			all_on_face = false;
			continue;
		}
		any_on_face = true;
		s_lo = MIN(s_lo, s);
		s_hi = MAX(s_hi, s);
		t_lo = MIN(t_lo, t);
		t_hi = MAX(t_hi, t);
	}

	if (!any_on_face) {
		return false;
	}

	// The corner directions already bound the box exactly. Central projection maps a convex box to the
	// convex hull of its projected corners, and (s, t) on a cube face is exactly that projection, so no
	// conversion from angle to (s, t) is needed here.
	//
	// An earlier version padded by the box angular radius on top of this, which sounds harmless but
	// inflated a 1 m body from about 10 cells to 3,335 -- a 200x increase in field evaluations and the
	// single reason this path missed its budget. The only slack actually required is for a cell whose
	// triangle overlaps the box while the vertex that indexes it sits just outside, plus rounding.
	double pad = 2.0 * lattice.cell_size_st();

	// Exception: if part of the box projects onto a different cube face, the corners on this face do
	// not bound it any more, so fall back to the conservative angular pad for that rare case.
	if (!all_on_face && near_dist > 0.0) {
		pad += 2.0 * (double((box_max - box_min).Length()) / near_dist);
	}

	s_lo -= pad;
	s_hi += pad;
	t_lo -= pad;
	t_hi += pad;

	// (s, t) in [-1, 1] maps to vertex index in [0, resolution].
	const double res = double(lattice.resolution);
	const double fi_lo = Math::floor((s_lo + 1.0) * 0.5 * res);
	const double fi_hi = Math::ceil((s_hi + 1.0) * 0.5 * res);
	const double fj_lo = Math::floor((t_lo + 1.0) * 0.5 * res);
	const double fj_hi = Math::ceil((t_hi + 1.0) * 0.5 * res);

	const double lo_i = MAX(fi_lo, double(cell_min_i));
	const double hi_i = MIN(fi_hi, double(cell_max_i));
	const double lo_j = MAX(fj_lo, double(cell_min_j));
	const double hi_j = MIN(fj_hi, double(cell_max_j));

	if (hi_i <= lo_i || hi_j <= lo_j) {
		return false;
	}

	r_min_i = uint32_t(lo_i);
	r_min_j = uint32_t(lo_j);
	r_max_i = uint32_t(hi_i);
	r_max_j = uint32_t(hi_j);
	return true;
}

void JoltCustomPlanetShape::CollidePoint(JPH::Vec3Arg p_point, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::CollidePointCollector &p_collector, const JPH::ShapeFilter &p_shape_filter) const {
	// A point is inside the planet when its distance from the centre is below f in that direction.
	// One field evaluation, no tessellation.
	const JPH::Vec3 planet_point = p_point + JPH::Vec3(float(patch_origin.x), float(patch_origin.y), float(patch_origin.z));
	const float len = planet_point.Length();
	if (len <= 0.0f) {
		return;
	}
	if (len < patch_min_radius) {
		// Below the patch floor, so certainly inside, without evaluating f.
	} else if (len > patch_max_radius) {
		return;
	} else {
		const Vector3 dir(double(planet_point.GetX()) / double(len), double(planet_point.GetY()) / double(len), double(planet_point.GetZ()) / double(len));
		if (double(len) >= field.eval_radius(dir)) {
			return;
		}
	}

	JPH::CollidePointResult result;
	result.mBodyID = JPH::TransformedShape::sGetBodyID(p_collector.GetContext());
	result.mSubShapeID2 = p_sub_shape_id_creator.GetID();
	p_collector.AddHit(result);
}

bool JoltCustomPlanetShape::clip_ray_to_shell(JPH::Vec3Arg p_origin, JPH::Vec3Arg p_direction, float &r_t_min, float &r_t_max) const {
	// Intersect with the sphere at patch_max_radius. Anything that can hit the surface must pass
	// through it, so this both rejects and bounds the march.
	const double ox = double(p_origin.GetX());
	const double oy = double(p_origin.GetY());
	const double oz = double(p_origin.GetZ());
	const double dx = double(p_direction.GetX());
	const double dy = double(p_direction.GetY());
	const double dz = double(p_direction.GetZ());

	const double a = dx * dx + dy * dy + dz * dz;
	if (a <= 0.0) {
		return false;
	}
	const double b = 2.0 * (ox * dx + oy * dy + oz * dz);
	const double r = double(patch_max_radius);
	const double c = ox * ox + oy * oy + oz * oz - r * r;

	const double disc = b * b - 4.0 * a * c;
	if (disc < 0.0) {
		return false;
	}
	const double sqrt_disc = Math::sqrt(disc);
	double t0 = (-b - sqrt_disc) / (2.0 * a);
	double t1 = (-b + sqrt_disc) / (2.0 * a);
	if (t0 > t1) {
		const double tmp = t0;
		t0 = t1;
		t1 = tmp;
	}

	// Jolt rays carry their length in the direction, so the useful range is always [0, 1].
	t0 = MAX(t0, 0.0);
	t1 = MIN(t1, 1.0);
	if (t1 < t0) {
		return false;
	}

	r_t_min = float(t0);
	r_t_max = float(t1);
	return true;
}

bool JoltCustomPlanetShape::CastRay(const JPH::RayCast &p_ray, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::RayCastResult &p_hit) const {
	PlanetCollisionStats::add(PlanetCollisionStats::ray_queries, 1);
	const JPH::Vec3 patch_offset(float(patch_origin.x), float(patch_origin.y), float(patch_origin.z));
	const JPH::Vec3 origin = p_ray.mOrigin + patch_offset;

	const JPH::uint id_bits = GetSubShapeIDBitsRecursive();
	bool hit = false;

	march_ray_cells(origin, p_ray.mDirection, [&](uint32_t p_min_i, uint32_t p_min_j, uint32_t p_max_i, uint32_t p_max_j) {
		tessellate_cells(face, p_min_i, p_min_j, p_max_i, p_max_j,
				[&](JPH::Vec3Arg p_v0, JPH::Vec3Arg p_v1, JPH::Vec3Arg p_v2, uint8_t p_active_edges, uint32_t p_cell_id) {
					// Triangles are patch-local, so cast the patch-local ray against them.
					const float fraction = JPH::RayTriangle(p_ray.mOrigin, p_ray.mDirection, p_v0, p_v1, p_v2);
					if (fraction < p_hit.mFraction) {
						p_hit.mFraction = fraction;
						p_hit.mSubShapeID2 = p_sub_shape_id_creator.PushID(p_cell_id, id_bits).GetID();
						hit = true;
					}
				});
	});

	return hit;
}

void JoltCustomPlanetShape::CastRay(const JPH::RayCast &p_ray, const JPH::RayCastSettings &p_ray_cast_settings, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::CastRayCollector &p_collector, const JPH::ShapeFilter &p_shape_filter) const {
	const JPH::Vec3 patch_offset(float(patch_origin.x), float(patch_origin.y), float(patch_origin.z));
	const JPH::Vec3 origin = p_ray.mOrigin + patch_offset;

	const JPH::uint id_bits = GetSubShapeIDBitsRecursive();
	const bool cull_back = p_ray_cast_settings.mBackFaceModeTriangles == JPH::EBackFaceMode::IgnoreBackFaces;

	march_ray_cells(origin, p_ray.mDirection, [&](uint32_t p_min_i, uint32_t p_min_j, uint32_t p_max_i, uint32_t p_max_j) {
		if (p_collector.ShouldEarlyOut()) {
			return;
		}
		tessellate_cells(face, p_min_i, p_min_j, p_max_i, p_max_j,
				[&](JPH::Vec3Arg p_v0, JPH::Vec3Arg p_v1, JPH::Vec3Arg p_v2, uint8_t p_active_edges, uint32_t p_cell_id) {
					if (p_collector.ShouldEarlyOut()) {
						return;
					}
					const float fraction = JPH::RayTriangle(p_ray.mOrigin, p_ray.mDirection, p_v0, p_v1, p_v2);
					if (fraction >= p_collector.GetEarlyOutFraction()) {
						return;
					}
					if (cull_back) {
						// Triangles are wound outward, so a ray arriving from outside meets a front face.
						const JPH::Vec3 normal = (p_v1 - p_v0).Cross(p_v2 - p_v0);
						if (normal.Dot(p_ray.mDirection) > 0.0f) {
							return;
						}
					}
					JPH::RayCastResult result;
					// The body ID comes from the collector context; a default-constructed one makes the
					// caller look up a null body.
					result.mBodyID = JPH::TransformedShape::sGetBodyID(p_collector.GetContext());
					result.mFraction = fraction;
					result.mSubShapeID2 = p_sub_shape_id_creator.PushID(p_cell_id, id_bits).GetID();
					p_collector.AddHit(result);
				});
	});
}

void JoltCustomPlanetShape::register_type() {
	JPH::ShapeFunctions &shape_functions = JPH::ShapeFunctions::sGet(JoltCustomShapeSubType::PLANET);

	shape_functions.mConstruct = construct_planet;
	shape_functions.mColor = JPH::Color::sGreen;

	// Only the convex subtypes. The compound and decorator entries for User3 were already filled in by
	// Jolt's own StaticCompoundShape::sRegister and decorator registration, which run inside
	// RegisterTypes() across sAllSubShapeTypes. Those handlers unwrap a compound or a
	// RotatedTranslated/Scaled shape and re-dispatch its convex leaves against us, which is exactly
	// what we want; overwriting them with our own functions would silently stop compound and
	// transformed bodies from ever reaching the planet.
	for (const JPH::EShapeSubType sub_type : JPH::sConvexSubShapeTypes) {
		JPH::CollisionDispatch::sRegisterCollideShape(sub_type, JoltCustomShapeSubType::PLANET, collide_convex_vs_planet);
		JPH::CollisionDispatch::sRegisterCollideShape(JoltCustomShapeSubType::PLANET, sub_type, JPH::CollisionDispatch::sReversedCollideShape);
		JPH::CollisionDispatch::sRegisterCastShape(sub_type, JoltCustomShapeSubType::PLANET, cast_convex_vs_planet);
		JPH::CollisionDispatch::sRegisterCastShape(JoltCustomShapeSubType::PLANET, sub_type, JPH::CollisionDispatch::sReversedCastShape);
	}

	// Planet vs planet never happens: both are static, and Jolt never collides static against static.
	// Registering it anyway means the assumption changing does not leave a hole in the table.
	JPH::CollisionDispatch::sRegisterCollideShape(JoltCustomShapeSubType::PLANET, JoltCustomShapeSubType::PLANET, collide_noop);
	JPH::CollisionDispatch::sRegisterCastShape(JoltCustomShapeSubType::PLANET, JoltCustomShapeSubType::PLANET, cast_noop);
}

#ifdef JPH_DEBUG_RENDERER

void JoltCustomPlanetShape::Draw(JPH::DebugRenderer *p_renderer, JPH::RMat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, JPH::ColorArg p_color, bool p_use_material_colors, bool p_draw_wireframe) const {
	constexpr int STEPS = 8;
	const uint64_t span_i = uint64_t(cell_max_i) - uint64_t(cell_min_i);
	const uint64_t span_j = uint64_t(cell_max_j) - uint64_t(cell_min_j);

	for (int j = 0; j <= STEPS; ++j) {
		for (int i = 0; i < STEPS; ++i) {
			const uint32_t i0 = cell_min_i + uint32_t((span_i * uint64_t(i)) / STEPS);
			const uint32_t i1 = cell_min_i + uint32_t((span_i * uint64_t(i + 1)) / STEPS);
			const uint32_t jj = cell_min_j + uint32_t((span_j * uint64_t(j)) / STEPS);

			const Vector3 da = lattice.vertex_direction(face, i0, jj);
			const Vector3 db = lattice.vertex_direction(face, i1, jj);
			const double ra = field.eval_radius(da);
			const double rb = field.eval_radius(db);

			p_renderer->DrawLine(
					p_center_of_mass_transform * JPH::Vec3(float(da.x * ra - patch_origin.x), float(da.y * ra - patch_origin.y), float(da.z * ra - patch_origin.z)),
					p_center_of_mass_transform * JPH::Vec3(float(db.x * rb - patch_origin.x), float(db.y * rb - patch_origin.y), float(db.z * rb - patch_origin.z)),
					p_color);
		}
	}
}

#endif
