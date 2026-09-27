/**************************************************************************/
/*  planet_det_math.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the       */
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

// Transcendental functions that return the same bits on every machine.
//
// The planet field is meant to be reproducible anywhere: a Linux server and a Windows client must
// agree on the height of the ground to the last bit, or rollback and server-side simulation drift.
// Basic IEEE-754 arithmetic (+ - * / sqrt) is exactly specified and gives that for free. The C
// library's sin, atan2, pow and exp are not specified to the last bit, and in practice differ: over
// the field's own input ranges mingw, the MSVC UCRT and glibc disagree with each other on up to 4.6%
// of results, and the UCRT even disagrees with itself depending on whether the CPU has FMA3. So the
// field calls these instead.
//
// Everything here is plain double arithmetic, comparisons and exact integer/bit conversions, with
// fdlibm's published kernels. It is not correctly rounded -- a few ulp at worst, far below anything
// the terrain can show -- only identical everywhere. That holds as long as the build does not fuse
// multiply-adds (every platform's detect.py sets -ffp-contract=off; MSVC's /fp:precise does not fuse)
// and is not built with fast-math.
//
// Not bit-identical to the GLSL twin: GPU sin/atan/pow are hardware approximations and cannot be.
// The shader only has to look the same; the CPU has to be exact.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace PlanetDetMath {

constexpr double PI = 3.14159265358979311600e+00;
constexpr double PI_LO = 1.22464679914735317720e-16;
constexpr double PIO2 = 1.57079632679489655800e+00;
constexpr double PIO2_LO = 6.12323399573676603587e-17;

// Round to the nearest integer, ties to even, without a libm call or a mode-dependent conversion.
// Adding and subtracting 1.5 * 2^52 pushes the fraction bits off the end; exact for |x| < 2^51. No
// compiler may fold the pair away without fast-math, which would break far more than this.
inline double round_nearest(const double p_x) {
	constexpr double SHIFT = 6755399441055744.0;
	return (p_x + SHIFT) - SHIFT;
}

// 2^k for k in the normal exponent range.
inline double pow2i(const int64_t p_k) {
	const uint64_t bits = uint64_t(p_k + 1023) << 52;
	double r;
	std::memcpy(&r, &bits, sizeof(r));
	return r;
}

// sin and cos of |x| <= pi/4 (fdlibm __kernel_sin / __kernel_cos, tail term dropped).
inline double kernel_sin(const double x) {
	constexpr double S1 = -1.66666666666666324348e-01;
	constexpr double S2 = 8.33333333332248946124e-03;
	constexpr double S3 = -1.98412698298579493134e-04;
	constexpr double S4 = 2.75573137070700676789e-06;
	constexpr double S5 = -2.50507602534068634195e-08;
	constexpr double S6 = 1.58969099521155010221e-10;
	const double z = x * x;
	const double v = z * x;
	const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
	return x + v * (S1 + z * r);
}

inline double kernel_cos(const double x) {
	constexpr double C1 = 4.16666666666666019037e-02;
	constexpr double C2 = -1.38888888888741095749e-03;
	constexpr double C3 = 2.48015872894767294178e-05;
	constexpr double C4 = -2.75573143513906633035e-07;
	constexpr double C5 = 2.08757232129817482790e-09;
	constexpr double C6 = -1.13596475577881948265e-11;
	const double z = x * x;
	const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
	const double hz = 0.5 * z;
	const double w = 1.0 - hz;
	return w + (((1.0 - w) - hz) + z * r);
}

// Reduces x to r in [-pi/4, pi/4] with x = r + q * pi/2, returning q mod 4. Cody-Waite with pi/2 split
// so q * PIO2_1 is exact; accurate to well under an ulp of r for |x| up to about 1e6, which is far
// beyond any angle the field forms.
inline int reduce_pio2(const double p_x, double &r_r) {
	constexpr double INV_PIO2 = 6.36619772367581382433e-01;
	constexpr double PIO2_1 = 1.57079632673412561417e+00;
	constexpr double PIO2_1T = 6.07710050650619224932e-11;
	const double q = round_nearest(p_x * INV_PIO2);
	r_r = (p_x - q * PIO2_1) - q * PIO2_1T;
	return int(int64_t(q) & 3);
}

inline double sin(const double p_x) {
	double r;
	switch (reduce_pio2(p_x, r)) {
		case 0:
			return kernel_sin(r);
		case 1:
			return kernel_cos(r);
		case 2:
			return -kernel_sin(r);
		default:
			return -kernel_cos(r);
	}
}

inline double cos(const double p_x) {
	double r;
	switch (reduce_pio2(p_x, r)) {
		case 0:
			return kernel_cos(r);
		case 1:
			return -kernel_sin(r);
		case 2:
			return -kernel_cos(r);
		default:
			return kernel_sin(r);
	}
}

inline double tan(const double p_x) {
	double r;
	const int q = reduce_pio2(p_x, r);
	const double s = kernel_sin(r);
	const double c = kernel_cos(r);
	return (q & 1) ? -c / s : s / c;
}

// fdlibm s_atan.c.
inline double atan(const double p_x) {
	constexpr double ATANHI[] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01, 1.57079632679489655800e+00 };
	constexpr double ATANLO[] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17, 6.12323399573676603587e-17 };
	constexpr double AT[] = {
		3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
		-1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
		6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
		-3.65315727442169155270e-02, 1.62858201153657823623e-02
	};
	if (p_x != p_x) {
		return p_x;
	}
	const bool negative = p_x < 0.0;
	double x = negative ? -p_x : p_x;
	if (x >= 7.37869762948382064640e+19) { // 2^66: atan is pi/2 to double precision.
		const double big = ATANHI[3] + ATANLO[3];
		return negative ? -big : big;
	}
	int id;
	if (x < 0.4375) {
		id = -1;
	} else if (x < 1.1875) {
		if (x < 0.6875) {
			id = 0;
			x = (2.0 * x - 1.0) / (2.0 + x);
		} else {
			id = 1;
			x = (x - 1.0) / (x + 1.0);
		}
	} else if (x < 2.4375) {
		id = 2;
		x = (x - 1.5) / (1.0 + 1.5 * x);
	} else {
		id = 3;
		x = -1.0 / x;
	}
	const double z = x * x;
	const double w = z * z;
	const double s1 = z * (AT[0] + w * (AT[2] + w * (AT[4] + w * (AT[6] + w * (AT[8] + w * AT[10])))));
	const double s2 = w * (AT[1] + w * (AT[3] + w * (AT[5] + w * (AT[7] + w * AT[9]))));
	double result;
	if (id < 0) {
		result = x - x * (s1 + s2);
	} else {
		result = ATANHI[id] - ((x * (s1 + s2) - ATANLO[id]) - x);
	}
	return negative ? -result : result;
}

inline double atan2(const double p_y, const double p_x) {
	if (p_x != p_x || p_y != p_y) {
		return p_x + p_y;
	}
	if (p_y == 0.0) {
		// Sign of zero is ignored: the field only uses the angle, and -0 and +0 must not split it.
		return p_x < 0.0 ? PI : 0.0;
	}
	if (p_x == 0.0) {
		return p_y > 0.0 ? PIO2 : -PIO2;
	}
	const double ay = p_y < 0.0 ? -p_y : p_y;
	const double ax = p_x < 0.0 ? -p_x : p_x;
	// atan of the smaller ratio, then reflect: keeps the argument in [0, 1], where atan is best.
	double a;
	if (ay <= ax) {
		a = atan(ay / ax);
	} else {
		a = PIO2 - (atan(ax / ay) - PIO2_LO);
	}
	if (p_x < 0.0) {
		a = PI - (a - PI_LO);
	}
	return p_y < 0.0 ? -a : a;
}

// sqrt is exactly rounded by IEEE-754, so this is as reproducible as atan2. (1 - x)(1 + x) rather than
// 1 - x*x keeps the precision near |x| = 1, where the collider's small cone angles live.
inline double acos(const double p_x) {
	if (!(p_x >= -1.0 && p_x <= 1.0)) {
		return (p_x - p_x) / 0.0;
	}
	return atan2(std::sqrt((1.0 - p_x) * (1.0 + p_x)), p_x);
}

// fdlibm e_exp.c, without its special handling of overflow beyond what the field can reach.
inline double exp(const double p_x) {
	constexpr double LN2_HI = 6.93147180369123816490e-01;
	constexpr double LN2_LO = 1.90821492927058770002e-10;
	constexpr double INV_LN2 = 1.44269504088896338700e+00;
	constexpr double P1 = 1.66666666666666019037e-01;
	constexpr double P2 = -2.77777777770155933842e-03;
	constexpr double P3 = 6.61375632143793436117e-05;
	constexpr double P4 = -1.65339022054652515390e-06;
	constexpr double P5 = 4.13813679705723846039e-08;
	if (p_x != p_x) {
		return p_x;
	}
	if (p_x > 709.0) {
		return 1.0e308 * 10.0;
	}
	if (p_x < -708.0) {
		return 0.0;
	}
	const double k = round_nearest(p_x * INV_LN2);
	const double hi = p_x - k * LN2_HI;
	const double lo = k * LN2_LO;
	const double r = hi - lo;
	const double t = r * r;
	const double c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
	const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
	return y * pow2i(int64_t(k));
}

// fdlibm e_log.c, for positive normal x. Returns -inf for 0 and NaN for negatives.
inline double log(const double p_x) {
	constexpr double LN2_HI = 6.93147180369123816490e-01;
	constexpr double LN2_LO = 1.90821492927058770002e-10;
	constexpr double LG1 = 6.666666666666735130e-01;
	constexpr double LG2 = 3.999999999940941908e-01;
	constexpr double LG3 = 2.857142874366239149e-01;
	constexpr double LG4 = 2.222219843214978396e-01;
	constexpr double LG5 = 1.818357216161805012e-01;
	constexpr double LG6 = 1.531383769920937332e-01;
	constexpr double LG7 = 1.479819860511658591e-01;
	if (!(p_x > 0.0)) {
		return p_x == 0.0 ? -1.0e308 * 10.0 : (p_x - p_x) / 0.0;
	}
	double x = p_x;
	int64_t k = 0;
	if (x < 2.2250738585072014e-308) { // Subnormal: scale into the normal range first.
		x *= 18014398509481984.0; // 2^54
		k = -54;
	}
	uint64_t bits;
	std::memcpy(&bits, &x, sizeof(bits));
	k += int64_t((bits >> 52) & 0x7ff) - 1023;
	bits &= 0x000fffffffffffffull;
	// Mantissa into [sqrt(2)/2, sqrt(2)).
	const uint64_t i = (bits + 0x95f6400000000ull) & 0x0010000000000000ull;
	bits |= i ^ 0x3ff0000000000000ull;
	k += int64_t(i >> 52);
	double m;
	std::memcpy(&m, &bits, sizeof(m));
	const double f = m - 1.0;
	const double dk = double(k);
	const double s = f / (2.0 + f);
	const double z = s * s;
	const double w = z * z;
	const double t1 = w * (LG2 + w * (LG4 + w * LG6));
	const double t2 = z * (LG1 + w * (LG3 + w * (LG5 + w * LG7)));
	const double r = t2 + t1;
	const double hfsq = 0.5 * f * f;
	return dk * LN2_HI - ((hfsq - (s * (hfsq + r) + dk * LN2_LO)) - f);
}

// x^y for x >= 0, which is every use in the field. A negative base returns NaN.
inline double pow(const double p_x, const double p_y) {
	if (p_y == 0.0) {
		return 1.0;
	}
	if (p_x == 1.0) {
		return 1.0;
	}
	if (p_x == 0.0) {
		return p_y > 0.0 ? 0.0 : 1.0e308 * 10.0;
	}
	return exp(p_y * log(p_x));
}

} // namespace PlanetDetMath
