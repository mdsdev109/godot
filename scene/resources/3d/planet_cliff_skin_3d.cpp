/**************************************************************************/
/*  planet_cliff_skin_3d.cpp                                              */
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

#include "planet_cliff_skin_3d.h"

#include "core/math/planet_lattice.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_map.h"

// A straight port of the demo's cliff_skin.gd, whose comments explain the method. It is not required
// to match the GDScript bit for bit; nothing else evaluates the face.

namespace {

typedef PlanetCliffSkin3D::Params Params;
typedef LocalVector<double> Grid;

// Runs p_fn(i) for i in [0, p_count) across the worker pool and waits. The region builder already
// runs on its own thread, so this is where the region's cost gets spread over the cores.
template <typename F>
void parallel_for(int p_count, const F &p_fn) {
	if (p_count <= 0) {
		return;
	}
	struct Ctx {
		const F *fn;
	} ctx{ &p_fn };
	WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_native_group_task(
			[](void *p_ud, uint32_t p_i) { (*static_cast<Ctx *>(p_ud)->fn)(int(p_i)); },
			&ctx, p_count, -1, true, String("PlanetCliffSkin3D"));
	WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
}

inline double smoothstep(double p_from, double p_to, double p_x) {
	return Math::smoothstep(p_from, p_to, p_x);
}

inline double lerp(double p_a, double p_b, double p_t) {
	return p_a + (p_b - p_a) * p_t;
}

struct Noise {
	int64_t seed;

	double hash01(int64_t p_x, int64_t p_y, int64_t p_z, int64_t p_salt) const {
		uint64_t h = 2166136261u;
		const int64_t values[5] = { p_x, p_y, p_z, p_salt, seed };
		for (int v = 0; v < 5; v++) {
			const uint64_t value = uint64_t(values[v]);
			for (int shift = 0; shift < 32; shift += 8) {
				h = ((h ^ ((value >> shift) & 0xFF)) * 16777619u) & 0xFFFFFFFFu;
			}
		}
		h ^= h >> 13;
		h = (h * 0x5bd1e995u) & 0xFFFFFFFFu;
		h ^= h >> 15;
		return double(h & 0xFFFFFF) / 16777216.0;
	}

	// Smooth value noise in 0..1.
	double value(const Vector3 &p_p, int64_t p_salt) const {
		const double cx = Math::floor(p_p.x);
		const double cy = Math::floor(p_p.y);
		const double cz = Math::floor(p_p.z);
		double fx = p_p.x - cx;
		double fy = p_p.y - cy;
		double fz = p_p.z - cz;
		fx = fx * fx * (3.0 - 2.0 * fx);
		fy = fy * fy * (3.0 - 2.0 * fy);
		fz = fz * fz * (3.0 - 2.0 * fz);
		const int64_t ix = int64_t(int32_t(cx));
		const int64_t iy = int64_t(int32_t(cy));
		const int64_t iz = int64_t(int32_t(cz));
		double v = 0.0;
		for (int dz = 0; dz < 2; dz++) {
			for (int dy = 0; dy < 2; dy++) {
				for (int dx = 0; dx < 2; dx++) {
					const double w = (dx == 1 ? fx : 1.0 - fx) * (dy == 1 ? fy : 1.0 - fy) * (dz == 1 ? fz : 1.0 - fz);
					v += w * hash01(ix + dx, iy + dy, iz + dz, p_salt);
				}
			}
		}
		return v;
	}

	// Noise in -1..1 with wavelength p_period * p_scale, by cross-fading fixed octaves; see
	// _band_noise in cliff_skin.gd for why the period cannot simply be scaled.
	double band(const Vector3 &p_q, double p_period, double p_scale, int64_t p_seed, double p_spacing) const {
		const double finest = MAX(0.0, Math::log(p_period / (3.0 * p_spacing)) / Math::log(2.0));
		const double level = CLAMP(-Math::log(MAX(p_scale, 1e-3)) / Math::log(2.0), -1.0, finest);
		const double l0 = Math::floor(level);
		const double f = level - l0;
		const double n0 = value(p_q * Math::pow(2.0, l0) / p_period, p_seed + 101 * int64_t(l0)) - 0.5;
		if (f <= 0.0) {
			return n0 * 2.0;
		}
		const double n1 = value(p_q * Math::pow(2.0, l0 + 1.0) / p_period, p_seed + 101 * int64_t(l0 + 1.0)) - 0.5;
		return 2.0 * (n0 * (1.0 - f) + n1 * f) / Math::sqrt((1.0 - f) * (1.0 - f) + f * f);
	}
};

// Box blur by running sums; windows truncated at the array edge.
Grid box_blur(const Grid &p_src, int n, int r) {
	Grid tmp;
	Grid out;
	tmp.resize(n * n);
	out.resize(n * n);
	for (int b = 0; b < n; b++) {
		const int row = b * n;
		double sum = 0.0;
		for (int t = 0; t <= MIN(r, n - 1); t++) {
			sum += p_src[row + t];
		}
		for (int a = 0; a < n; a++) {
			const int lo = a - r;
			const int hi = a + r;
			tmp[row + a] = sum / double(MIN(hi, n - 1) - MAX(lo, 0) + 1);
			if (hi + 1 < n) {
				sum += p_src[row + hi + 1];
			}
			if (lo >= 0) {
				sum -= p_src[row + lo];
			}
		}
	}
	for (int a = 0; a < n; a++) {
		double sum = 0.0;
		for (int t = 0; t <= MIN(r, n - 1); t++) {
			sum += tmp[t * n + a];
		}
		for (int b = 0; b < n; b++) {
			const int lo = b - r;
			const int hi = b + r;
			out[b * n + a] = sum / double(MIN(hi, n - 1) - MAX(lo, 0) + 1);
			if (hi + 1 < n) {
				sum += tmp[(hi + 1) * n + a];
			}
			if (lo >= 0) {
				sum -= tmp[lo * n + a];
			}
		}
	}
	return out;
}

// Running min or max over a (2r+1) window along a strided line, van Herk / Gil-Werman.
void line_extreme(const double *p_src, double *p_dst, int p_stride, int n, int r, bool p_max,
		LocalVector<double> &fwd, LocalVector<double> &bwd) {
	const int w = 2 * r + 1;
	fwd.resize(n);
	bwd.resize(n);
	for (int i = 0; i < n; i++) {
		double v = p_src[i * p_stride];
		if (i % w != 0) {
			v = p_max ? MAX(v, fwd[i - 1]) : MIN(v, fwd[i - 1]);
		}
		fwd[i] = v;
	}
	for (int i = n - 1; i >= 0; i--) {
		double v = p_src[i * p_stride];
		if (i < n - 1 && (i + 1) % w != 0) {
			v = p_max ? MAX(v, bwd[i + 1]) : MIN(v, bwd[i + 1]);
		}
		bwd[i] = v;
	}
	for (int i = 0; i < n; i++) {
		const int lo = MAX(i - r, 0);
		const int hi = MIN(i + r, n - 1);
		p_dst[i * p_stride] = p_max ? MAX(bwd[lo], fwd[hi]) : MIN(bwd[lo], fwd[hi]);
	}
}

Grid extreme2d(const Grid &p_src, int n, int r, bool p_max) {
	Grid tmp;
	Grid out;
	tmp.resize(n * n);
	out.resize(n * n);
	LocalVector<double> fwd;
	LocalVector<double> bwd;
	for (int b = 0; b < n; b++) {
		line_extreme(p_src.ptr() + b * n, tmp.ptr() + b * n, 1, n, r, p_max, fwd, bwd);
	}
	for (int a = 0; a < n; a++) {
		line_extreme(tmp.ptr() + a, out.ptr() + a, n, n, r, p_max, fwd, bwd);
	}
	return out;
}

// Everything the face tessellation reads, and the face point / normal at a continuous lattice
// coordinate.
struct FaceCtx {
	const Params *prm;
	const Noise *noise;
	int n;
	const LocalVector<Vector3> *pos;
	const LocalVector<Vector3> *outs;
	const Grid *mask;
	const Grid *blend;
	const Grid *scale;

	Vector3 noised(const Vector3 &p, const Vector3 &p_out, double p_mask, double p_scale) const {
		if (!prm->vertical_face || p_mask <= 0.0) {
			return p;
		}
		const double face_noise = prm->face_noise_m * p_scale *
				(noise->band(p, prm->face_noise_period_m, p_scale, 15, prm->face_spacing_m) +
						0.5 * noise->band(p, prm->face_noise_period_m / 2.5, p_scale, 16, prm->face_spacing_m));
		return p + p_out * (p_mask * face_noise);
	}

	// Position; the mask and grass blend are written if asked for.
	Vector3 point(double p_u, double p_v, double *r_mask = nullptr, double *r_blend = nullptr) const {
		const int a = CLAMP(int(Math::floor(p_u)), 0, n - 2);
		const int b = CLAMP(int(Math::floor(p_v)), 0, n - 2);
		const double fu = p_u - double(a);
		const double fv = p_v - double(b);
		const int k00 = b * n + a;
		const int k10 = k00 + 1;
		const int k01 = k00 + n;
		const int k11 = k01 + 1;
		const double w00 = (1.0 - fu) * (1.0 - fv);
		const double w10 = fu * (1.0 - fv);
		const double w01 = (1.0 - fu) * fv;
		const double w11 = fu * fv;
		const Vector3 p = (*pos)[k00] * w00 + (*pos)[k10] * w10 + (*pos)[k01] * w01 + (*pos)[k11] * w11;
		const Vector3 o = (*outs)[k00] * w00 + (*outs)[k10] * w10 + (*outs)[k01] * w01 + (*outs)[k11] * w11;
		const double mk = (*mask)[k00] * w00 + (*mask)[k10] * w10 + (*mask)[k01] * w01 + (*mask)[k11] * w11;
		const double sc = (*scale)[k00] * w00 + (*scale)[k10] * w10 + (*scale)[k01] * w01 + (*scale)[k11] * w11;
		if (r_mask) {
			*r_mask = mk;
		}
		if (r_blend) {
			*r_blend = (*blend)[k00] * w00 + (*blend)[k10] * w10 + (*blend)[k01] * w01 + (*blend)[k11] * w11;
		}
		return noised(p, o, mk, sc);
	}

	// Normal of the noised face itself, so it is the same whichever quad or region emits the point.
	Vector3 normal(double p_u, double p_v, double p_orient, const Vector3 &p_fallback) const {
		const double h = 0.1;
		const Vector3 du = point(p_u + h, p_v) - point(p_u - h, p_v);
		const Vector3 dv = point(p_u, p_v + h) - point(p_u, p_v - h);
		const Vector3 nrm = du.cross(dv);
		if (nrm.length_squared() < 1e-12) {
			return p_fallback;
		}
		return nrm.normalized() * p_orient;
	}
};

// One emitted vertex before it is evaluated: on a lattice edge (ka to kb at t; a corner has ka == kb)
// or inside a quad at lattice coordinate (u, v).
struct VertexSpec {
	int ka = -1;
	int kb = -1;
	double t = 0.0;
	double u = 0.0;
	double v = 0.0;
	int fallback = 0;
};

} // namespace

PlanetCliffSkin3D::Params PlanetCliffSkin3D::params_from_dictionary(const Dictionary &p_params) {
	Params p;
#define PCS_READ(m_name, m_type)                           \
	if (p_params.has(#m_name)) {                           \
		p.m_name = m_type(p_params[#m_name]);              \
	}
	PCS_READ(step_m, double)
	PCS_READ(blur_m, double)
	PCS_READ(wall_slope_start, double)
	PCS_READ(wall_slope_full, double)
	PCS_READ(apron_slope_start, double)
	PCS_READ(apron_slope_full, double)
	PCS_READ(offset_m, double)
	PCS_READ(ridge_m, double)
	PCS_READ(ridge_period_m, double)
	PCS_READ(ridge_stretch, double)
	PCS_READ(buttress_m, double)
	PCS_READ(buttress_period_m, double)
	PCS_READ(buttress_stretch, double)
	PCS_READ(strata_m, double)
	PCS_READ(strata_period_m, double)
	PCS_READ(facet_m, double)
	PCS_READ(facet_period_m, double)
	PCS_READ(clearance_m, double)
	PCS_READ(lip_width_m, double)
	PCS_READ(lip_height_m, double)
	PCS_READ(lip_out_m, double)
	PCS_READ(lip_lump_period_m, double)
	PCS_READ(lump_m, double)
	PCS_READ(lump_period_m, double)
	PCS_READ(vertical_face, bool)
	PCS_READ(foot_search_m, double)
	PCS_READ(lip_pull_in_m, double)
	PCS_READ(lip_push_out_m, double)
	PCS_READ(lip_shift_period_m, double)
	PCS_READ(face_noise_m, double)
	PCS_READ(face_noise_period_m, double)
	PCS_READ(face_spacing_m, double)
	PCS_READ(face_max_split, int)
	PCS_READ(reference_wall_m, double)
	PCS_READ(reference_slope, double)
	PCS_READ(slope_scale_min, double)
	PCS_READ(scale_min, double)
	PCS_READ(scale_max, double)
	PCS_READ(vertical_offset_m, double)
	PCS_READ(max_reach_m, double)
	PCS_READ(drama_min, double)
	PCS_READ(drama_max, double)
	PCS_READ(drama_period_m, double)
	PCS_READ(jut_period_m, double)
	PCS_READ(max_overhang_m, double)
	PCS_READ(foot_sink_m, double)
	PCS_READ(smooth_r, int)
	PCS_READ(grass_blend_m, double)
	PCS_READ(edge_sink_m, double)
	PCS_READ(seed_value, int64_t)
#undef PCS_READ
	return p;
}

Dictionary PlanetCliffSkin3D::build(const Ref<PlanetField3D> &p_field, double p_planet_radius, int p_lattice_res,
		double p_cell_arc, int p_face, int p_ci, int p_cj, int p_region_cells, const Vector3 &p_anchor,
		const Dictionary &p_params) const {
	ERR_FAIL_COND_V(p_field.is_null(), Dictionary());
	const Params prm = params_from_dictionary(p_params);
	const PlanetFieldData &field = p_field->get_data();
	const Noise noise{ prm.seed_value };
	const double R = p_planet_radius;

	const int n_core = MAX(1, int(Math::round(double(p_region_cells) * p_cell_arc / prm.step_m)));
	const double sc = double(p_region_cells) / double(n_core);
	const int blur = MAX(1, int(Math::round(prm.blur_m / prm.step_m * 0.5)));
	const int lip_r = MAX(1, int(Math::round(prm.lip_width_m / prm.step_m)));
	const int foot_r = MAX(1, int(Math::round(prm.foot_search_m / prm.step_m)));
	// One more foot_r than before: the apron dilates the wall mask by it before everything else reads it.
	const int halo = 2 * blur + lip_r + 5 + 3 + prm.smooth_r + 1 + 2 * foot_r;
	const int n = n_core + 1 + 2 * halo;
	const int nn = n * n;
	const int gi0 = p_ci * n_core - halo;
	const int gj0 = p_cj * n_core - halo;

	auto empty = [&]() {
		Dictionary d;
		d["vertices"] = PackedVector3Array();
		d["normals"] = PackedVector3Array();
		d["indices"] = PackedInt32Array();
		d["faces"] = PackedVector3Array();
		d["wall_cells"] = 0;
		d["mask"] = PackedFloat32Array();
		d["colors"] = PackedColorArray();
		return d;
	};

	// 1. Directions and the finished surface's height, a row per task.
	LocalVector<Vector3> dirs;
	Grid h;
	dirs.resize(nn);
	h.resize(nn);
	const double inv_res = 1.0 / double(p_lattice_res);
	parallel_for(n, [&](int b) {
		for (int a = 0; a < n; a++) {
			const double u = double(gi0 + a) * sc;
			const double v = double(gj0 + b) * sc;
			const double s = PlanetLattice::face_warp(2.0 * u * inv_res - 1.0);
			const double t = PlanetLattice::face_warp(2.0 * v * inv_res - 1.0);
			const Vector3 d = PlanetLattice::cube_point(p_face, s, t).normalized();
			dirs[b * n + a] = d;
			h[b * n + a] = field.eval_radius(d) - R;
		}
	});
	const Grid hb = box_blur(h, n, blur);

	// 2. The wall mask, from the blurred surface's slope.
	Grid mask;
	LocalVector<Vector3> grads;
	mask.resize(nn);
	grads.resize(nn);
	for (int i = 0; i < nn; i++) {
		mask[i] = 0.0;
		grads[i] = Vector3();
	}
	bool any_wall = false;
	for (int b = 1; b < n - 1; b++) {
		for (int a = 1; a < n - 1; a++) {
			const int k = b * n + a;
			const Vector3 di = (dirs[k + 1] - dirs[k - 1]) * (0.5 * R);
			const Vector3 dj = (dirs[k + n] - dirs[k - n]) * (0.5 * R);
			const double gaa = di.dot(di);
			const double gab = di.dot(dj);
			const double gbb = dj.dot(dj);
			const double det = gaa * gbb - gab * gab;
			const double ga = (hb[k + 1] - hb[k - 1]) * 0.5;
			const double gb = (hb[k + n] - hb[k - n]) * 0.5;
			const double x = (ga * gbb - gb * gab) / det;
			const double y = (gb * gaa - ga * gab) / det;
			Vector3 grad = di * x + dj * y;
			grad -= dirs[k] * grad.dot(dirs[k]);
			grads[k] = grad;
			mask[k] = smoothstep(prm.wall_slope_start, prm.wall_slope_full, grad.length());
			any_wall = any_wall || mask[k] > 0.02;
		}
	}
	if (!any_wall) {
		return empty();
	}

	// 3. Filters: inner wall, lip extent, smoothed gradient, foot and rim heights, wall slope, scale.
	// The apron: slope that is still steep, though not wall, within the foot search of a wall joins it.
	// Without it the face covered only the steepest band of a short or shallow slope, stood vertical
	// over the ground far below, and pulled back in where the band ended partway down -- a brim over
	// an undercut, like the rim of a hat. With it the face runs down to where the slope really flattens.
	// Only BELOW the wall: the apron carries the face down to its foot, never up over the flank above
	// it, which on a mountain would wrap the whole slope in rock.
	if (prm.apron_slope_full > prm.apron_slope_start) {
		const Grid near_wall = extreme2d(mask, n, foot_r, true);
		Grid wall_h;
		wall_h.resize(nn);
		for (int i = 0; i < nn; i++) {
			wall_h[i] = mask[i] > 0.5 ? h[i] : 1e30;
		}
		const Grid wall_low = extreme2d(wall_h, n, foot_r, false);
		for (int i = 0; i < nn; i++) {
			const double below = 1.0 - smoothstep(-1.0, 2.0, h[i] - wall_low[i]);
			const double apron = smoothstep(prm.apron_slope_start, prm.apron_slope_full, grads[i].length())
					* smoothstep(0.3, 0.7, near_wall[i]) * below;
			mask[i] = MAX(mask[i], apron);
		}
	}
	const Grid inner = extreme2d(mask, n, blur, false);
	const Grid ext = box_blur(extreme2d(mask, n, lip_r, true), n, 2);
	LocalVector<Vector3> gsmooth;
	{
		Grid xs, ys, zs;
		xs.resize(nn);
		ys.resize(nn);
		zs.resize(nn);
		for (int i = 0; i < nn; i++) {
			xs[i] = grads[i].x;
			ys[i] = grads[i].y;
			zs[i] = grads[i].z;
		}
		xs = box_blur(xs, n, 3);
		ys = box_blur(ys, n, 3);
		zs = box_blur(zs, n, 3);
		gsmooth.resize(nn);
		for (int i = 0; i < nn; i++) {
			gsmooth[i] = Vector3(xs[i], ys[i], zs[i]);
		}
	}
	// The foot the face stands over is the lowest point of the WALL nearby, not the lowest ground. On a
	// short or shallow slope the wall mask covers only its steepest band; standing the face over ground
	// far below that band pushed the lip out over the rest of the slope while the face's own bottom edge
	// tucked back in where the band ended -- a brim over an undercut, like the rim of a hat. Over the
	// wall's own foot the face is vertical down to where it really ends, and the slope below stays
	// terrain. Where no wall is near, the lowest ground stands in, as before.
	const Grid ground_low = extreme2d(h, n, foot_r, false);
	Grid wall_h;
	wall_h.resize(nn);
	for (int i = 0; i < nn; i++) {
		wall_h[i] = mask[i] > 0.5 ? h[i] : 1e30;
	}
	const Grid wall_low = extreme2d(wall_h, n, foot_r, false);
	Grid hmin;
	hmin.resize(nn);
	for (int i = 0; i < nn; i++) {
		hmin[i] = wall_low[i] < 1e29 ? MAX(wall_low[i], ground_low[i]) : ground_low[i];
	}
	const Grid hmax = extreme2d(h, n, foot_r, true);
	Grid gslope;
	gslope.resize(nn);
	for (int i = 0; i < nn; i++) {
		gslope[i] = gsmooth[i].length();
	}
	const Grid wall_slope = extreme2d(gslope, n, lip_r, true);
	Grid scale;
	scale.resize(nn);
	for (int i = 0; i < nn; i++) {
		const double by_height = (hmax[i] - hmin[i]) / prm.reference_wall_m;
		const double by_slope = lerp(prm.slope_scale_min, 1.0, smoothstep(prm.wall_slope_full, prm.reference_slope, wall_slope[i]));
		scale[i] = CLAMP(by_height * by_slope, prm.scale_min, prm.scale_max);
	}
	scale = box_blur(scale, n, blur);

	// 4. Displacement: push out, lift, sink.
	LocalVector<Vector3> outs;
	Grid push, lift, sink, base;
	outs.resize(nn);
	push.resize(nn);
	lift.resize(nn);
	sink.resize(nn);
	base.resize(nn);
	for (int i = 0; i < nn; i++) {
		outs[i] = Vector3();
		push[i] = 0.0;
		lift[i] = 0.0;
		sink[i] = 0.0;
		base[i] = 0.0;
	}
	parallel_for(n - 2, [&](int row) {
		const int b = row + 1;
		for (int a = 1; a < n - 1; a++) {
			const int k = b * n + a;
			const double m = mask[k];
			const Vector3 g = gsmooth[k];
			const double gl = g.length();
			outs[k] = gl > 1e-4 ? -g / gl : Vector3();
			base[k] = lerp(h[k], MAX(h[k], hb[k]), inner[k]) + prm.clearance_m * m;
			const double edge = CLAMP(ext[k] - m, 0.0, 1.0);
			const double cover = MAX(m, edge);
			if (cover <= 0.0) {
				sink[k] = prm.edge_sink_m;
				continue;
			}
			const double upper = smoothstep(-1.0, 2.0, h[k] - hb[k]);
			const double lip = edge * upper;
			const double foot = edge * (1.0 - upper);
			Vector3 q(double(gi0 + a) * prm.step_m, double(gj0 + b) * prm.step_m, h[k]);
			const double sk = prm.vertical_face ? scale[k] : 1.0;
			const double at_rim = smoothstep(0.2, 0.9, lip);
			sink[k] = prm.foot_sink_m * smoothstep(0.0, 0.6, foot) + prm.edge_sink_m * (1.0 - cover);
			if (prm.vertical_face) {
				lift[k] = prm.lip_height_m * sk * smoothstep(0.0, 0.4, lip);
				const double reach = CLAMP((h[k] - hmin[k]) / MAX(wall_slope[k], 0.35), 0.0, prm.max_reach_m);
				const double lip_shift = sk * (0.5 * (prm.lip_push_out_m - prm.lip_pull_in_m) + 0.5 * (prm.lip_push_out_m + prm.lip_pull_in_m) * noise.band(Vector3(q.x, q.y, 0.0), prm.lip_shift_period_m, sk, 14, prm.face_spacing_m));
				const double up_frac = CLAMP((h[k] - hmin[k]) / MAX(hmax[k] - hmin[k], 1.0), 0.0, 1.0);
				push[k] = MAX(m, at_rim) * reach + lip_shift * MAX(m * up_frac, at_rim) + prm.vertical_offset_m * sk * cover;
				continue;
			}
			// The older sculpted face, kept for vertical_face off.
			const double lumps = 0.55 + 0.9 * noise.value(q / prm.lip_lump_period_m, 9);
			const double drama = lerp(prm.drama_min, prm.drama_max, noise.value(Vector3(q.x, q.y, 0.0) / prm.drama_period_m, 11));
			double jut = noise.value(Vector3(q.x, q.y, 0.0) / prm.jut_period_m, 12);
			jut = jut * jut * 2.2 + 0.1;
			lift[k] = prm.lip_height_m * smoothstep(0.0, 0.4, lip) * lumps * (0.6 + 0.4 * drama);
			q.z = base[k];
			const double carve = smoothstep(0.0, 0.6, inner[k]);
			const double bumps = prm.lump_m * (noise.value(q / prm.lump_period_m, 10) * 2.0 - 1.0) + prm.lump_m * 0.5 * (noise.value(q / (prm.lump_period_m * 0.4), 13) * 2.0 - 1.0);
			const double bt = noise.value(Vector3(q.x / prm.buttress_period_m, q.y / prm.buttress_period_m, q.z / (prm.buttress_period_m * prm.buttress_stretch)), 6);
			const double buttress = bt * bt;
			const double r1 = noise.value(Vector3(q.x / prm.ridge_period_m, q.y / prm.ridge_period_m, q.z / (prm.ridge_period_m * prm.ridge_stretch)), 3);
			const double ridge = 1.0 - Math::abs(2.0 * r1 - 1.0);
			const double r2 = noise.value(Vector3(q.x / (prm.ridge_period_m * 0.45), q.y / (prm.ridge_period_m * 0.45), q.z / (prm.ridge_period_m * prm.ridge_stretch * 0.45)), 4);
			const double ridges = CLAMP(ridge * ridge * 0.75 + (1.0 - Math::abs(2.0 * r2 - 1.0)) * 0.35, 0.0, 1.0);
			const double wander = noise.value(Vector3(q.x / 30.0, q.y / 30.0, 0.5), 7);
			const double sf = Math::fposmod(q.z / prm.strata_period_m + wander * 1.7, 1.0);
			const double tooth = smoothstep(0.0, 0.8, sf) * (1.0 - smoothstep(0.85, 1.0, sf));
			const double presence = smoothstep(0.35, 0.65, noise.value(Vector3(q.x / 18.0, q.y / 18.0, q.z / 40.0), 8));
			const double strata = tooth * presence;
			const double sculpt = prm.offset_m + prm.buttress_m * buttress * drama + bumps + carve * (prm.ridge_m * ridges + prm.strata_m * strata + prm.facet_m * (noise.value(q / prm.facet_period_m, 5) - 0.5) * 2.0);
			const double foot_carry = foot * lerp(0.3, 1.0, jut * 0.5);
			const double overhang = MIN(prm.lip_out_m * lumps * jut * drama, prm.max_overhang_m) * smoothstep(0.3, 0.95, lip);
			push[k] = MAX(MAX(m, at_rim), foot_carry) * MAX(sculpt, 0.5) + overhang;
		}
	});
	push = box_blur(push, n, prm.smooth_r);
	lift = box_blur(lift, n, prm.smooth_r);
	sink = box_blur(sink, n, prm.smooth_r);

	// 5. Smooth face positions, and the grass blend against the terrain under each.
	LocalVector<Vector3> pos;
	Grid ground_blend;
	pos.resize(nn);
	ground_blend.resize(nn);
	for (int i = 0; i < nn; i++) {
		pos[i] = Vector3();
		ground_blend[i] = 0.0;
	}
	parallel_for(n - 2, [&](int row) {
		const int b = row + 1;
		for (int a = 1; a < n - 1; a++) {
			const int k = b * n + a;
			const double above = base[k] + lift[k] - sink[k];
			pos[k] = dirs[k] * (R + above) + outs[k] * push[k];
			if (ext[k] > 0.02) {
				const Vector3 p = pos[k];
				ground_blend[k] = 1.0 - smoothstep(0.0, prm.grass_blend_m, p.length() - field.eval_radius(p.normalized()));
			}
		}
	});

	const int mid = (n / 2) * n + n / 2;
	const double orient = (dirs[mid + 1] - dirs[mid - 1]).cross(dirs[mid + n] - dirs[mid - n]).dot(dirs[mid]) > 0.0 ? 1.0 : -1.0;

	// 6. Tessellation by edge length. Topology first, sequentially: which vertices exist, shared
	// between quads by key, and the triangles. Then every vertex is evaluated in parallel.
	FaceCtx ctx{ &prm, &noise, n, &pos, &outs, &mask, &ground_blend, &scale };
	auto edge_split = [&](int ka, int kb) {
		return CLAMP(int(Math::ceil(pos[ka].distance_to(pos[kb]) / prm.face_spacing_m)), 1, prm.face_max_split);
	};
	HashMap<uint64_t, int> index_of;
	LocalVector<VertexSpec> specs;
	LocalVector<int> indices;
	LocalVector<int> grid;
	int wall_cells = 0;
	for (int b = halo; b < halo + n_core; b++) {
		for (int a = halo; a < halo + n_core; a++) {
			const int k00 = b * n + a;
			const int k10 = k00 + 1;
			const int k01 = k00 + n;
			const int k11 = k01 + 1;
			if (MAX(MAX(ext[k00], ext[k10]), MAX(ext[k01], ext[k11])) <= 0.02) {
				continue;
			}
			wall_cells++;
			const int eu0 = edge_split(k00, k10);
			const int eu1 = edge_split(k01, k11);
			const int ev0 = edge_split(k00, k01);
			const int ev1 = edge_split(k10, k11);
			const int nu = MAX(eu0, eu1);
			const int nv = MAX(ev0, ev1);
			grid.resize((nu + 1) * (nv + 1));
			for (int j = 0; j <= nv; j++) {
				for (int i = 0; i <= nu; i++) {
					VertexSpec spec;
					spec.fallback = k00;
					uint64_t key;
					int ka = -1;
					int kb = -1;
					int idx = 0;
					int e = 1;
					if (j == 0 || j == nv) {
						ka = j == 0 ? k00 : k01;
						kb = ka + 1;
						e = j == 0 ? eu0 : eu1;
						idx = int(Math::round(double(i * e) / double(nu)));
					} else if (i == 0 || i == nu) {
						ka = i == 0 ? k00 : k10;
						kb = ka + n;
						e = i == 0 ? ev0 : ev1;
						idx = int(Math::round(double(j * e) / double(nv)));
					}
					if (ka < 0) {
						key = (uint64_t(2) << 62) | (uint64_t(k00) << 24) | (uint64_t(i) << 12) | uint64_t(j);
						spec.u = double(a) + double(i) / double(nu);
						spec.v = double(b) + double(j) / double(nv);
					} else if (idx == 0 || idx == e) {
						const int kc = idx == 0 ? ka : kb;
						key = uint64_t(kc);
						spec.ka = kc;
						spec.kb = kc;
					} else {
						key = (uint64_t(1) << 62) | (uint64_t(ka) << 36) | (uint64_t(kb) << 12) | uint64_t(idx);
						spec.ka = ka;
						spec.kb = kb;
						spec.t = double(idx) / double(e);
					}
					int *found = index_of.getptr(key);
					int vi;
					if (found) {
						vi = *found;
					} else {
						vi = int(specs.size());
						index_of.insert(key, vi);
						specs.push_back(spec);
					}
					grid[j * (nu + 1) + i] = vi;
				}
			}
			for (int j = 0; j < nv; j++) {
				for (int i = 0; i < nu; i++) {
					const int s00 = j * (nu + 1) + i;
					const int quad[4] = { grid[s00], grid[s00 + 1], grid[s00 + nu + 2], grid[s00 + nu + 1] };
					const int tris[2][3] = { { quad[0], quad[1], quad[2] }, { quad[0], quad[2], quad[3] } };
					for (int ti = 0; ti < 2; ti++) {
						const int *tri = tris[ti];
						if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) {
							continue;
						}
						indices.push_back(tri[0]);
						if (orient > 0.0) {
							indices.push_back(tri[2]);
							indices.push_back(tri[1]);
						} else {
							indices.push_back(tri[1]);
							indices.push_back(tri[2]);
						}
					}
				}
			}
		}
	}

	const int vcount = int(specs.size());
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedFloat32Array vertex_mask;
	PackedColorArray colors;
	vertices.resize(vcount);
	normals.resize(vcount);
	vertex_mask.resize(vcount);
	colors.resize(vcount);
	Vector3 *vw = vertices.ptrw();
	Vector3 *nw = normals.ptrw();
	float *mw = vertex_mask.ptrw();
	Color *cw = colors.ptrw();
	const int chunk = 256;
	parallel_for((vcount + chunk - 1) / chunk, [&](int c) {
		const int end = MIN(vcount, (c + 1) * chunk);
		for (int vi = c * chunk; vi < end; vi++) {
			const VertexSpec &s = specs[vi];
			Vector3 p;
			double mk;
			double bl;
			double u;
			double v;
			if (s.ka < 0) {
				u = s.u;
				v = s.v;
				p = ctx.point(u, v, &mk, &bl);
			} else {
				// Along a lattice edge, straight between its ends: the same arithmetic from either side.
				const double t = s.t;
				const Vector3 pe = pos[s.ka].lerp(pos[s.kb], t);
				mk = lerp(mask[s.ka], mask[s.kb], t);
				bl = lerp(ground_blend[s.ka], ground_blend[s.kb], t);
				p = ctx.noised(pe, outs[s.ka].lerp(outs[s.kb], t), mk, lerp(scale[s.ka], scale[s.kb], t));
				u = lerp(double(s.ka % n), double(s.kb % n), t);
				v = lerp(double(s.ka / n), double(s.kb / n), t);
			}
			vw[vi] = p - p_anchor;
			mw[vi] = float(mk);
			cw[vi] = Color(float(bl), 0.0f, 0.0f, 1.0f);
			nw[vi] = ctx.normal(u, v, orient, dirs[s.fallback]);
		}
	});

	PackedInt32Array out_indices;
	PackedVector3Array faces;
	out_indices.resize(indices.size());
	faces.resize(indices.size());
	int32_t *iw = out_indices.ptrw();
	Vector3 *fw = faces.ptrw();
	for (uint32_t i = 0; i < indices.size(); i++) {
		iw[i] = indices[i];
		fw[i] = vw[indices[i]];
	}

	Dictionary d;
	d["vertices"] = vertices;
	d["normals"] = normals;
	d["indices"] = out_indices;
	d["faces"] = faces;
	d["wall_cells"] = wall_cells;
	d["mask"] = vertex_mask;
	d["colors"] = colors;
	return d;
}

void PlanetCliffSkin3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build", "field", "planet_radius", "lattice_res", "cell_arc", "face", "ci", "cj", "region_cells", "anchor", "params"),
			&PlanetCliffSkin3D::build);
}
