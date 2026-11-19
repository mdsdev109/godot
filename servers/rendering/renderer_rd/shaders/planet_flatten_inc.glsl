// Planet flattening: a small planet drawn as if it were a much larger one around the camera, so its horizon is far
// away, relaxing back to the true sphere as `amount` goes to 0 (planet-demo's docs/FLATTENED_GROUND.md). Only where
// things are drawn changes; they stay where they are for everything else.
//
// Every point keeps its bearing from the camera's foot on the planet, its distance round the surface from that foot
// (s) and its height above the radius (h), and is drawn at that distance and height on a sphere of radius
// `ground_radius` touching the true one under the camera. Everything happens in the vertical plane through the camera
// and the point. Between the ground picture and the true one the point turns about the camera, in that plane, by the
// angle between them as the camera sees them, and its distance from the camera blends geometrically -- so climbing
// moves what is seen smoothly, and nothing sinks past where it ends up (the clamp).
//
// Works in any frame with the centre and eye given in it: the pass's view space here, so the camera is near the
// origin and nothing planet-sized forms near it. Kept apart from the scene shaders so a check can compile it alone
// (planet-demo's tests/planet_flatten_checks.gd), and mirrored on the CPU there in double -- keep the two the same.

float planet_flatten_sinc(float x) {
	if (abs(x) < 0.05) {
		float x2 = x * x;
		return 1.0 - x2 / 6.0 + x2 * x2 / 120.0;
	}
	return sin(x) / x;
}

vec3 planet_flatten_rotate(vec3 v, vec3 axis, float angle) {
	float c = cos(angle);
	return v * c + cross(axis, v) * sin(angle) + axis * dot(axis, v) * (1.0 - c);
}

// Point `p`, the planet's centre and the eye in one frame; `radius` the true radius, `ground_radius` the one drawn on
// the ground, `amount` 1 the ground picture, 0 the true one. Returns where `p` is drawn; `r_axis` and `r_angle` turn
// its directions (planet_flatten_direction), `r_mix` how much of that turn they take. A point all but straight above or
// below the eye -- within about 1e-5 of the radius of its vertical -- is drawn where it is: its bearing there is float
// noise, and flattening moves it less than a millimetre.
vec3 planet_flatten_point(vec3 p, vec3 centre, vec3 eye, float radius, float ground_radius, float amount, out vec3 r_axis, out float r_angle, out float r_mix) {
	r_axis = vec3(0.0, 1.0, 0.0);
	r_angle = 0.0;
	r_mix = 0.0;
	if (amount <= 0.0) {
		return p;
	}
	vec3 e = eye - centre;
	float eye_r = length(e);
	vec3 F = e / eye_r;
	vec3 q = p - centre;
	float r = length(q);
	vec3 u = q / r;
	vec3 t = u - F * dot(u, F);
	if (length(t) < 0.00001) {
		return p;
	}
	t = normalize(t);
	t = normalize(t - F * dot(t, F));
	float ang = 2.0 * asin(min(length(u - F) * 0.5, 1.0));
	float s = radius * ang;
	float h = r - radius;
	float th = s / ground_radius;
	float hs = 0.5 * th * planet_flatten_sinc(0.5 * th); // sin(th / 2)
	// The ground picture and the true one, from the eye: neither forms a planet-sized number near it.
	vec3 ground = F * ((radius - eye_r) + h * (1.0 - 2.0 * hs * hs) - 2.0 * ground_radius * hs * hs) + t * (s * (1.0 + h / ground_radius) * planet_flatten_sinc(th));
	vec3 truth = p - eye;
	r_axis = normalize(cross(F, t));
	r_angle = th - ang;
	vec2 g2 = vec2(dot(ground, t), dot(ground, F));
	vec2 t2 = vec2(dot(truth, t), dot(truth, F));
	// The signed angle from the ground picture to the true one, measured directly: two atan2s subtracted lose metres
	// for points nearly straight below the eye.
	float between = atan(g2.x * t2.y - g2.y * t2.x, dot(g2, t2));
	bool clamped = between > 0.000001; // the ground picture would be lower than the true one: take the true one
	if (amount >= 1.0 && !clamped) {
		r_mix = 1.0;
		return eye + ground;
	}
	float turn = clamped ? between : (1.0 - amount) * between;
	float stretch = exp((1.0 - amount) * log(max(length(t2), 0.001) / max(length(g2), 0.001)));
	vec2 o = vec2(g2.x * cos(turn) - g2.y * sin(turn), g2.x * sin(turn) + g2.y * cos(turn)) * stretch;
	r_mix = amount;
	return eye + t * o.x + F * o.y;
}

// A direction at the point (a normal, a tangent) as it is drawn there: turned as planet_flatten_point says.
vec3 planet_flatten_direction(vec3 v, vec3 axis, float angle, float mix_amount) {
	if (mix_amount <= 0.0) {
		return v;
	}
	vec3 turned = planet_flatten_rotate(v, axis, angle);
	return mix_amount >= 1.0 ? turned : normalize(mix(v, turned, mix_amount));
}
