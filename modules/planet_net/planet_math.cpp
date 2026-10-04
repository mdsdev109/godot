/**************************************************************************/
/*  planet_math.cpp                                                       */
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

#include "planet_math.h"

#include "core/math/planet_det_math.h"
#include "core/object/class_db.h"

double PlanetMath::sin(double p_x) { return PlanetDetMath::sin(p_x); }
double PlanetMath::cos(double p_x) { return PlanetDetMath::cos(p_x); }
double PlanetMath::tan(double p_x) { return PlanetDetMath::tan(p_x); }
double PlanetMath::atan(double p_x) { return PlanetDetMath::atan(p_x); }
double PlanetMath::atan2(double p_y, double p_x) { return PlanetDetMath::atan2(p_y, p_x); }
double PlanetMath::acos(double p_x) { return PlanetDetMath::acos(p_x); }
double PlanetMath::exp(double p_x) { return PlanetDetMath::exp(p_x); }
double PlanetMath::log(double p_x) { return PlanetDetMath::log(p_x); }
double PlanetMath::pow(double p_x, double p_y) { return PlanetDetMath::pow(p_x, p_y); }

Basis PlanetMath::axis_angle(const Vector3 &p_axis, double p_angle) {
	// Basis::set_axis_angle, line for line, but for the sine and cosine.
	Basis b;
	const Vector3 axis_sq(p_axis.x * p_axis.x, p_axis.y * p_axis.y, p_axis.z * p_axis.z);
	const double cosine = PlanetDetMath::cos(p_angle);
	b.rows[0][0] = axis_sq.x + cosine * (1.0 - axis_sq.x);
	b.rows[1][1] = axis_sq.y + cosine * (1.0 - axis_sq.y);
	b.rows[2][2] = axis_sq.z + cosine * (1.0 - axis_sq.z);
	const double sine = PlanetDetMath::sin(p_angle);
	const double t = 1.0 - cosine;
	double xyzt = p_axis.x * p_axis.y * t;
	double zyxs = p_axis.z * sine;
	b.rows[0][1] = xyzt - zyxs;
	b.rows[1][0] = xyzt + zyxs;
	xyzt = p_axis.x * p_axis.z * t;
	zyxs = p_axis.y * sine;
	b.rows[0][2] = xyzt + zyxs;
	b.rows[2][0] = xyzt - zyxs;
	xyzt = p_axis.y * p_axis.z * t;
	zyxs = p_axis.x * sine;
	b.rows[1][2] = xyzt - zyxs;
	b.rows[2][1] = xyzt + zyxs;
	return b;
}

void PlanetMath::_bind_methods() {
	ClassDB::bind_static_method("PlanetMath", D_METHOD("sin", "x"), &PlanetMath::sin);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("cos", "x"), &PlanetMath::cos);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("tan", "x"), &PlanetMath::tan);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("atan", "x"), &PlanetMath::atan);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("atan2", "y", "x"), &PlanetMath::atan2);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("acos", "x"), &PlanetMath::acos);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("exp", "x"), &PlanetMath::exp);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("log", "x"), &PlanetMath::log);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("pow", "x", "y"), &PlanetMath::pow);
	ClassDB::bind_static_method("PlanetMath", D_METHOD("axis_angle", "axis", "angle"), &PlanetMath::axis_angle);
}
