/**************************************************************************/
/*  planet_math.h                                                         */
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

// PlanetDetMath for scripts. GDScript's sin, cos, tan and the rest are the platform's C runtime, which
// rounds differently on Windows and Linux; anything a script places that the network server must place
// identically -- rocks, cliffs, the lattice mapping -- uses these instead (planet-demo
// docs/DETERMINISM.md). Measured before this existed: GDScript's tan differed between the two in one
// call in twenty, and moved one rock cell in fifteen by a few ULPs.

#include "core/math/basis.h"
#include "core/object/object.h"

class PlanetMath : public Object {
	GDCLASS(PlanetMath, Object);

protected:
	static void _bind_methods();

public:
	static double sin(double p_x);
	static double cos(double p_x);
	static double tan(double p_x);
	static double atan(double p_x);
	static double atan2(double p_y, double p_x);
	static double acos(double p_x);
	static double exp(double p_x);
	static double log(double p_x);
	static double pow(double p_x, double p_y);
	// Basis(axis, angle) -- the same formula, with these sin and cos. basis.rotated(axis, angle) is
	// axis_angle(axis, angle) * basis.
	static Basis axis_angle(const Vector3 &p_axis, double p_angle);
};
