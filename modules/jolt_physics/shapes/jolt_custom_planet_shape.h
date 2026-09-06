/**************************************************************************/
/*  jolt_custom_planet_shape.h                                            */
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

#include "jolt_custom_shape_type.h"

#include "core/math/planet_field.h"
#include "core/math/planet_lattice.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/ActiveEdges.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

// Analytic planet surface, r = f(direction from the planet centre), covering one patch of the
// lattice.
//
// Derives from JPH::Shape, not JPH::ConvexShape: r = f(dir) is star-shaped but not convex, so there
// is no usable support function. Any crater or gap between two peaks breaks convexity, and the
// amplitude ceiling for a convex r = f(dir) is far below anything a player could see.
//
// Collision works the way JPH::HeightFieldShape does -- generate triangles on demand inside the
// narrow phase, hand them to CollideConvexVsTriangles, discard them -- but on a spherical
// parameterization instead of a planar grid. Nothing is cached: caching is what produces the
// boundary-crossing hitches this design exists to avoid.
//
// One instance covers a rectangular range of lattice cells on one cube face, and emits triangles in
// coordinates RELATIVE TO patch_origin rather than to the planet centre.
//
// That relative origin is the whole point. Jolt hands the narrow phase float32 transforms: even in a
// double-precision build, the relative transform between two bodies is computed in double and then
// narrowed, so what matters is the distance between the two body origins. With one planet-centred
// body that distance is the planet radius, whose float32 ULP at 3,185,500 m is 0.38 m, and a resting
// sphere then sits 0.28 m off the surface and jitters by the same amount. Measured across radii, both
// the clearance error and the jitter track 0.75x the ULP at the body origin.
//
// So the planet is built as many static BODIES, one per patch, each positioned at its own patch
// origin in double precision. Sub-shapes of a compound would not do: CompoundShape stores sub-shape
// positions as Float3, and the body-relative transform would still span the planet radius.
//
// Note JPH::StaticCompoundShape is unusable here regardless -- its Node stores child bounds as
// HalfFloat, which saturates above 65504, so at planet scale it reports no collisions at all.
// Measured: identical code collides correctly at radius 30,000 and returns zero contacts at 65,000.
class JoltCustomPlanetShapeSettings final : public JPH::ShapeSettings {
public:
	PlanetFieldData field;
	PlanetLattice lattice;

	int face = 0;
	// Half-open cell range [min, max) on that face.
	uint32_t cell_min_i = 0;
	uint32_t cell_min_j = 0;
	uint32_t cell_max_i = 0;
	uint32_t cell_max_j = 0;

	// Planet-space point that this shape's local origin corresponds to. The owning body must be placed
	// here, in double precision.
	Vector3 patch_origin;

	float patch_min_radius = 0.0f;
	float patch_max_radius = 0.0f;

	virtual JPH::ShapeSettings::ShapeResult Create() const override;
};

class JoltCustomPlanetShape final : public JPH::Shape {
public:
	PlanetFieldData field;
	PlanetLattice lattice;

	int face = 0;
	uint32_t cell_min_i = 0;
	uint32_t cell_min_j = 0;
	uint32_t cell_max_i = 0;
	uint32_t cell_max_j = 0;

	Vector3 patch_origin;

	float patch_min_radius = 0.0f;
	float patch_max_radius = 0.0f;

	static void register_type();

	JoltCustomPlanetShape() :
			JPH::Shape(JPH::EShapeType::User1, JoltCustomShapeSubType::PLANET) {}

	JoltCustomPlanetShape(const JoltCustomPlanetShapeSettings &p_settings, JPH::Shape::ShapeResult &p_result) :
			JPH::Shape(JPH::EShapeType::User1, JoltCustomShapeSubType::PLANET, p_settings, p_result),
			field(p_settings.field),
			lattice(p_settings.lattice),
			face(p_settings.face),
			cell_min_i(p_settings.cell_min_i),
			cell_min_j(p_settings.cell_min_j),
			cell_max_i(p_settings.cell_max_i),
			cell_max_j(p_settings.cell_max_j),
			patch_origin(p_settings.patch_origin),
			patch_min_radius(p_settings.patch_min_radius),
			patch_max_radius(p_settings.patch_max_radius) {
		if (!p_result.HasError()) {
			p_result.Set(this);
		}
	}

	virtual JPH::AABox GetLocalBounds() const override;

	// Largest sphere fitting inside the shape. The patch is a thin curved shell rather than a solid, so
	// there is no meaningful inscribed sphere; report the shell thickness, which is what penetration
	// recovery should be limited to.
	virtual float GetInnerRadius() const override { return 0.5f * (patch_max_radius - patch_min_radius); }

	// Bits this shape spends addressing its own triangles. 28 cannot uniquely index every 0.25 m cell
	// on a planet and does not need to: Jolt keys the manifold cache by (body pair, sub-shape pair)
	// and contacts within one body pair are spatially local, so two cells on opposite sides of the
	// planet aliasing to the same bucket is harmless. 28 rather than all 32 leaves headroom for any
	// decorator Godot wraps the shape in.
	static constexpr JPH::uint SUB_SHAPE_ID_BITS = 28;
	virtual JPH::uint GetSubShapeIDBitsRecursive() const override { return SUB_SHAPE_ID_BITS; }

	virtual const JPH::PhysicsMaterial *GetMaterial(const JPH::SubShapeID &p_sub_shape_id) const override {
		return JPH::PhysicsMaterial::sDefault;
	}

	// Static body: never consulted by the solver.
	virtual JPH::MassProperties GetMassProperties() const override {
		JPH::MassProperties mass_properties;
		mass_properties.mMass = 1.0f;
		mass_properties.mInertia = JPH::Mat44::sIdentity();
		return mass_properties;
	}

	// Computed analytically from the position, never by inverting the SubShapeID -- the ID is a hash
	// of the lattice cell and is deliberately not invertible. The gradient of ||p|| - f(p_hat) is
	// dominated by the radial term at these amplitudes, so the outward radial direction is the right
	// answer for a resting contact; the per-triangle normal that actually drives the solver comes from
	// the contact result's penetration axis, not from here.
	virtual JPH::Vec3 GetSurfaceNormal(const JPH::SubShapeID &p_sub_shape_id, JPH::Vec3Arg p_local_surface_position) const override {
		// Radial in PLANET space, so undo the patch offset before normalizing. Using the patch-local
		// position directly would give a normal pointing away from the patch centre, which is close to
		// tangent to the surface.
		const JPH::Vec3 planet_pos = p_local_surface_position + JPH::Vec3(float(patch_origin.x), float(patch_origin.y), float(patch_origin.z));
		return planet_pos.NormalizedOr(JPH::Vec3::sAxisY());
	}

	virtual void GetSubmergedVolume(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::Plane &p_surface, float &p_total_volume, float &p_submerged_volume, JPH::Vec3 &p_center_of_buoyancy JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg p_base_offset)) const override {
		p_total_volume = 0.0f;
		p_submerged_volume = 0.0f;
		p_center_of_buoyancy = JPH::Vec3::sZero();
	}

#ifdef JPH_DEBUG_RENDERER
	virtual void Draw(JPH::DebugRenderer *p_renderer, JPH::RMat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, JPH::ColorArg p_color, bool p_use_material_colors, bool p_draw_wireframe) const override;
#endif

	// Raycasts march the same lattice cells the collider tessellates and test the same triangles, so a
	// raycast and a contact cannot disagree about where the ground is. The design doc offers an
	// alternative -- root-find on g(t) = |p(t)| - f(p_hat(t)) -- but that would answer against the exact
	// analytic surface while collision answers against its chordal approximation, and the two differ by
	// the facet sagitta. Sharing the triangles is what keeps them consistent.
	virtual bool CastRay(const JPH::RayCast &p_ray, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::RayCastResult &p_hit) const override;

	virtual void CastRay(const JPH::RayCast &p_ray, const JPH::RayCastSettings &p_ray_cast_settings, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::CastRayCollector &p_collector, const JPH::ShapeFilter &p_shape_filter = JPH::ShapeFilter()) const override;

	virtual void CollidePoint(JPH::Vec3Arg p_point, const JPH::SubShapeIDCreator &p_sub_shape_id_creator, JPH::CollidePointCollector &p_collector, const JPH::ShapeFilter &p_shape_filter = JPH::ShapeFilter()) const override;

	virtual void CollideSoftBodyVertices(JPH::Mat44Arg p_center_of_mass_transform, JPH::Vec3Arg p_scale, const JPH::CollideSoftBodyVertexIterator &p_vertices, JPH::uint p_num_vertices, int p_colliding_shape_index) const override {}

	virtual void GetTrianglesStart(JPH::Shape::GetTrianglesContext &p_context, const JPH::AABox &p_box, JPH::Vec3Arg p_position_com, JPH::QuatArg p_rotation, JPH::Vec3Arg p_scale) const override {}

	virtual int GetTrianglesNext(JPH::Shape::GetTrianglesContext &p_context, int p_max_triangles_requested, JPH::Float3 *p_triangle_vertices, const JPH::PhysicsMaterial **p_materials = nullptr) const override { return 0; }

	virtual JPH::Shape::Stats GetStats() const override { return JPH::Shape::Stats(sizeof(*this), 0); }

	virtual float GetVolume() const override {
		const double r = field.base_radius;
		return float((4.0 / 3.0) * Math::PI * r * r * r);
	}

	// Cheap face-independent reject: true if the box straddles the radius band the surface lives in.
	// This is the test that replaces the broadphase patch decomposition, so it runs first and must
	// stay cheap.
	bool box_straddles_surface(const JPH::AABox &p_box) const;

	// Shifts a box from this shape local space into planet-centred space.
	JPH::AABox to_planet_space(const JPH::AABox &p_box) const;

	// Clips a ray (in planet space) to the patch radius shell. Returns false when the ray cannot reach
	// the shell at all, which is the cheap reject for the overwhelmingly common orbital raycast.
	bool clip_ray_to_shell(JPH::Vec3Arg p_origin, JPH::Vec3Arg p_direction, float &r_t_min, float &r_t_max) const;

	// Walks the lattice cells a ray passes through, calling p_callback(min_i, min_j, max_i, max_j) for
	// each small cell range along the way. Returns false if the ray never enters the patch.
	//
	// Stepping is driven by the ray angular sweep rather than its length, so a near-radial ray -- an
	// orbital raycast, the common case -- touches one or two cells no matter how long it is. The cap
	// exists so that a grazing ray cannot walk the whole planet; at a 0.149 m cell that is about 2.4 km
	// of tangential travel, far beyond any gameplay ray, but a longer grazing ray will stop early.
	static constexpr uint32_t MAX_RAY_CELL_STEPS = 16384;
	template <typename TCallback>
	bool march_ray_cells(JPH::Vec3Arg p_origin, JPH::Vec3Arg p_direction, TCallback p_callback) const;

	// Maps a box in this shape's local space to the range of lattice cells on one cube face whose
	// surface could intersect it. Returns false if the box cannot touch that face.
	bool get_cell_range_for_box(
			const JPH::AABox &p_box,
			int p_face,
			uint32_t &r_min_i,
			uint32_t &r_min_j,
			uint32_t &r_max_i,
			uint32_t &r_max_j) const;

	// Emits the triangles for a cell range. `p_callback` takes (v0, v1, v2, sub_shape_id).
	template <typename TCallback>
	void tessellate_cells(
			int p_face,
			uint32_t p_min_i,
			uint32_t p_min_j,
			uint32_t p_max_i,
			uint32_t p_max_j,
			TCallback p_callback) const;
};

// Tessellates a range of lattice cells into triangles.
//
// Works in tiles with a one-cell border. The border exists so that every emitted triangle has its
// neighbours available, which is what active-edge classification needs: an internal edge between two
// nearly-coplanar triangles must be marked inactive, or a sliding body catches on nothing. Getting
// that wrong is the difference between a character walking smoothly across a cell boundary and
// tripping over it, which is exactly the Phase 3 acceptance test.
template <typename TCallback>
void JoltCustomPlanetShape::tessellate_cells(
		int p_face,
		uint32_t p_min_i,
		uint32_t p_min_j,
		uint32_t p_max_i,
		uint32_t p_max_j,
		TCallback p_callback) const {
	// Sized so a typical query -- a metre-scale body on a 0.149 m lattice -- fits in ONE tile. Each
	// tile pays for its own one-cell border, so splitting a 13-cell range into two 8-cell tiles costs
	// noticeably more vertices than covering it in one.
	constexpr uint32_t TILE = 16;
	// Bordered cell grid is TILE+2 per axis, so the vertex grid is TILE+3.
	constexpr uint32_t MAX_VERTS_PER_AXIS = TILE + 3;
	constexpr uint32_t MAX_VERTS = MAX_VERTS_PER_AXIS * MAX_VERTS_PER_AXIS;
	constexpr uint32_t MAX_CELLS_PER_AXIS = TILE + 2;
	constexpr uint32_t MAX_CELLS = MAX_CELLS_PER_AXIS * MAX_CELLS_PER_AXIS;

	// Jolt default: edges shallower than 5 degrees are not active.
	constexpr float ACTIVE_EDGE_COS_THRESHOLD = 0.996195f;

	for (uint32_t tile_j = p_min_j; tile_j < p_max_j; tile_j += TILE) {
		const uint32_t tile_j_end = MIN(tile_j + TILE, p_max_j);
		for (uint32_t tile_i = p_min_i; tile_i < p_max_i; tile_i += TILE) {
			const uint32_t tile_i_end = MIN(tile_i + TILE, p_max_i);

			// Bordered cell range, clamped to the face. Where the border is clipped, at a face edge,
			// the outermost emitted cells treat the missing neighbour as coplanar, which errs towards
			// inactive and therefore towards not snagging.
			const uint32_t bi0 = tile_i > 0 ? tile_i - 1 : 0;
			const uint32_t bj0 = tile_j > 0 ? tile_j - 1 : 0;
			const uint32_t bi1 = MIN(tile_i_end + 1, lattice.resolution);
			const uint32_t bj1 = MIN(tile_j_end + 1, lattice.resolution);

			const uint32_t cells_i = bi1 - bi0;
			const uint32_t cells_j = bj1 - bj0;
			const uint32_t verts_i = cells_i + 1;
			const uint32_t verts_j = cells_j + 1;
			const uint32_t vert_count = verts_i * verts_j;
			if (cells_i == 0 || cells_j == 0 || vert_count > MAX_VERTS) {
				continue;
			}

			Vector3 dirs[MAX_VERTS];
			double scratch_x[MAX_VERTS];
			double scratch_y[MAX_VERTS];
			double scratch_z[MAX_VERTS];
			float noise[MAX_VERTS];
			double radius[MAX_VERTS];

			for (uint32_t j = 0; j < verts_j; ++j) {
				for (uint32_t i = 0; i < verts_i; ++i) {
					dirs[j * verts_i + i] = lattice.vertex_direction(p_face, bi0 + i, bj0 + j);
				}
			}

			field.eval_radius_batch(dirs, scratch_x, scratch_y, scratch_z, noise, radius, vert_count);

			PlanetCollisionStats::add(PlanetCollisionStats::field_samples, vert_count);
			PlanetCollisionStats::add(PlanetCollisionStats::cells_tessellated, uint64_t(tile_i_end - tile_i) * uint64_t(tile_j_end - tile_j));

			// The subtraction of patch_origin happens in double and the narrowing to float happens
			// after it. Doing it the other way round would put the planet radius through float32 and
			// reintroduce the 0.38 m quantization this patch layout exists to remove.
			JPH::Vec3 pos[MAX_VERTS];
			for (uint32_t k = 0; k < vert_count; ++k) {
				const double r = radius[k];
				pos[k] = JPH::Vec3(
						float(dirs[k].x * r - patch_origin.x),
						float(dirs[k].y * r - patch_origin.y),
						float(dirs[k].z * r - patch_origin.z));
			}

			// Two triangles per cell, split along the (i,j)-(i+1,j+1) diagonal. Winding is forced
			// outward per cell rather than from a per-face table: the six cube faces do not all have
			// the same handedness, and a face wound the wrong way is an inside-out surface that bodies
			// fall straight through.
			JPH::Vec3 normal_a[MAX_CELLS];
			JPH::Vec3 normal_b[MAX_CELLS];
			bool flipped[MAX_CELLS];

			for (uint32_t j = 0; j + 1 < verts_j; ++j) {
				for (uint32_t i = 0; i + 1 < verts_i; ++i) {
					const uint32_t k00 = j * verts_i + i;
					const uint32_t k10 = k00 + 1;
					const uint32_t k01 = k00 + verts_i;
					const uint32_t k11 = k01 + 1;
					const uint32_t c = j * cells_i + i;

					// Outward reference must be the true radial direction, not the patch-local position:
					// after subtracting patch_origin, pos is an offset from the patch centre and points
					// nowhere useful.
					const Vector3 &out_dir = dirs[k00];
					const JPH::Vec3 outward(float(out_dir.x), float(out_dir.y), float(out_dir.z));
					const JPH::Vec3 fallback = outward;

					JPH::Vec3 na = (pos[k10] - pos[k00]).Cross(pos[k11] - pos[k00]);
					const bool flip = na.Dot(outward) < 0.0f;
					JPH::Vec3 nb = (pos[k11] - pos[k00]).Cross(pos[k01] - pos[k00]);
					if (flip) {
						na = -na;
						nb = -nb;
					}
					normal_a[c] = na.NormalizedOr(fallback);
					normal_b[c] = nb.NormalizedOr(fallback);
					flipped[c] = flip;
				}
			}

			// Emit only the interior cells; the border exists purely to classify their edges.
			for (uint32_t cj = tile_j; cj < tile_j_end; ++cj) {
				for (uint32_t ci = tile_i; ci < tile_i_end; ++ci) {
					const uint32_t li = ci - bi0;
					const uint32_t lj = cj - bj0;
					const uint32_t c = lj * cells_i + li;

					const uint32_t k00 = lj * verts_i + li;
					const uint32_t k10 = k00 + 1;
					const uint32_t k01 = k00 + verts_i;
					const uint32_t k11 = k01 + 1;

					const JPH::Vec3 na = normal_a[c];
					const JPH::Vec3 nb = normal_b[c];

					// Neighbour normal across each shared edge. Where the border was clipped, reuse
					// this cell's own normal, which classifies that edge as inactive.
					const JPH::Vec3 n_below = (lj > 0) ? normal_b[c - cells_i] : na;
					const JPH::Vec3 n_above = (lj + 2 < verts_j) ? normal_a[c + cells_i] : nb;
					const JPH::Vec3 n_left = (li > 0) ? normal_a[c - 1] : nb;
					const JPH::Vec3 n_right = (li + 2 < verts_i) ? normal_b[c + 1] : na;

					const uint32_t id = PlanetLattice::cell_hash(p_face, ci, cj, SUB_SHAPE_ID_BITS);

					// Vertices and per-edge neighbour normals are carried in matching arrays so that
					// flipping the winding reorders both together. Jolt indexes the active-edge bits
					// by emitted edge (v0-v1, v1-v2, v2-v0), so these must not drift apart.
					{
						uint32_t vi[3] = { k00, k10, k11 };
						JPH::Vec3 nbr[3] = { n_below, n_right, nb };
						if (flipped[c]) {
							const uint32_t tv = vi[1];
							vi[1] = vi[2];
							vi[2] = tv;
							const JPH::Vec3 tn = nbr[0];
							nbr[0] = nbr[2];
							nbr[2] = tn;
						}
						uint8_t active = 0;
						for (int e = 0; e < 3; ++e) {
							const JPH::Vec3 edge = pos[vi[(e + 1) % 3]] - pos[vi[e]];
							if (JPH::ActiveEdges::IsEdgeActive(na, nbr[e], edge.NormalizedOr(JPH::Vec3::sAxisX()), ACTIVE_EDGE_COS_THRESHOLD)) {
								active |= uint8_t(1 << e);
							}
						}
						p_callback(pos[vi[0]], pos[vi[1]], pos[vi[2]], active, id);
					}
					{
						uint32_t vi[3] = { k00, k11, k01 };
						JPH::Vec3 nbr[3] = { na, n_above, n_left };
						if (flipped[c]) {
							const uint32_t tv = vi[1];
							vi[1] = vi[2];
							vi[2] = tv;
							const JPH::Vec3 tn = nbr[0];
							nbr[0] = nbr[2];
							nbr[2] = tn;
						}
						uint8_t active = 0;
						for (int e = 0; e < 3; ++e) {
							const JPH::Vec3 edge = pos[vi[(e + 1) % 3]] - pos[vi[e]];
							if (JPH::ActiveEdges::IsEdgeActive(nb, nbr[e], edge.NormalizedOr(JPH::Vec3::sAxisX()), ACTIVE_EDGE_COS_THRESHOLD)) {
								active |= uint8_t(1 << e);
							}
						}
						p_callback(pos[vi[0]], pos[vi[1]], pos[vi[2]], active, id);
					}
				}
			}
		}
	}
}

// Steps along a ray, converting each sample point to the lattice cell containing it and reporting the
// range spanning consecutive samples so no cell between two samples is skipped.
template <typename TCallback>
bool JoltCustomPlanetShape::march_ray_cells(JPH::Vec3Arg p_origin, JPH::Vec3Arg p_direction, TCallback p_callback) const {
	float t_min = 0.0f;
	float t_max = 0.0f;
	if (!clip_ray_to_shell(p_origin, p_direction, t_min, t_max)) {
		return false;
	}

	const Vector3 origin(double(p_origin.GetX()), double(p_origin.GetY()), double(p_origin.GetZ()));
	const Vector3 dir(double(p_direction.GetX()), double(p_direction.GetY()), double(p_direction.GetZ()));

	const Vector3 p_enter = origin + dir * double(t_min);
	const Vector3 p_exit = origin + dir * double(t_max);
	if (p_enter.length_squared() <= 0.0 || p_exit.length_squared() <= 0.0) {
		return false;
	}

	// How far the direction sweeps decides the step count; the length of the ray does not.
	const double cos_sweep = CLAMP(p_enter.normalized().dot(p_exit.normalized()), -1.0, 1.0);
	const double sweep = Math::acos(cos_sweep);
	const double cell_angle = 2.0 / double(lattice.resolution);

	uint32_t steps = 1;
	if (cell_angle > 0.0) {
		const double wanted = (sweep / cell_angle) * 2.0 + 2.0;
		steps = uint32_t(CLAMP(wanted, 1.0, double(MAX_RAY_CELL_STEPS)));
	}

	bool have_prev = false;
	uint32_t prev_i = 0;
	uint32_t prev_j = 0;
	bool entered = false;

	for (uint32_t step = 0; step <= steps; ++step) {
		const double t = double(t_min) + (double(t_max) - double(t_min)) * (double(step) / double(steps));
		const Vector3 pt = origin + dir * t;
		if (pt.length_squared() <= 0.0) {
			continue;
		}

		int pt_face = 0;
		double s = 0.0;
		double tt = 0.0;
		PlanetLattice::direction_to_face(pt.normalized(), pt_face, s, tt);
		if (pt_face != face) {
			have_prev = false;
			continue;
		}

		const double res_d = double(lattice.resolution);
		const double fi = (s + 1.0) * 0.5 * res_d;
		const double fj = (tt + 1.0) * 0.5 * res_d;
		uint32_t ci = uint32_t(CLAMP(fi, double(cell_min_i), double(cell_max_i) - 1.0));
		uint32_t cj = uint32_t(CLAMP(fj, double(cell_min_j), double(cell_max_j) - 1.0));

		uint32_t lo_i = ci;
		uint32_t hi_i = ci;
		uint32_t lo_j = cj;
		uint32_t hi_j = cj;
		if (have_prev) {
			lo_i = MIN(lo_i, prev_i);
			hi_i = MAX(hi_i, prev_i);
			lo_j = MIN(lo_j, prev_j);
			hi_j = MAX(hi_j, prev_j);
		}

		// One cell of slack on every side. The marched cell comes from truncating a floating-point
		// (s, t), so a ray passing near a cell boundary can be attributed to the neighbour -- and for a
		// near-radial ray the sweep is zero, so there is no marching to cover the mistake and the ray
		// reports no hit at all. That was roughly 5-10% of straight-down rays finding no ground.
		const uint32_t slack = 1;
		const uint32_t elo_i = (lo_i > cell_min_i + slack) ? (lo_i - slack) : cell_min_i;
		const uint32_t elo_j = (lo_j > cell_min_j + slack) ? (lo_j - slack) : cell_min_j;
		const uint32_t ehi_i = MIN(hi_i + 1 + slack, cell_max_i);
		const uint32_t ehi_j = MIN(hi_j + 1 + slack, cell_max_j);

		entered = true;
		p_callback(elo_i, elo_j, ehi_i, ehi_j);

		prev_i = ci;
		prev_j = cj;
		have_prev = true;
	}

	return entered;
}
