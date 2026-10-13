/**************************************************************************/
/*  planet_character.cpp                                                  */
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

#include "planet_character.h"
#include "planet_net_protocol.h"

#include "core/object/class_db.h"

void PlanetCharacter::step(const Vector2 &p_move, float p_yaw, int p_buttons, double p_dt) {
	PlanetNet::WalkerInput in;
	in.move = Vector2(float(p_move.x), float(p_move.y));
	in.yaw = p_yaw;
	in.buttons = uint8_t(p_buttons);
	state = walker.step(state, in, p_dt);
}

void PlanetCharacter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_field", "field"), &PlanetCharacter::set_field);
	ClassDB::bind_method(D_METHOD("get_field"), &PlanetCharacter::get_field);
	ClassDB::bind_method(D_METHOD("set_planet_radius", "radius"), &PlanetCharacter::set_planet_radius);
	ClassDB::bind_method(D_METHOD("get_planet_radius"), &PlanetCharacter::get_planet_radius);
	ClassDB::bind_method(D_METHOD("set_walk_speed", "speed"), &PlanetCharacter::set_walk_speed);
	ClassDB::bind_method(D_METHOD("get_walk_speed"), &PlanetCharacter::get_walk_speed);
	ClassDB::bind_method(D_METHOD("set_sprint_speed", "speed"), &PlanetCharacter::set_sprint_speed);
	ClassDB::bind_method(D_METHOD("get_sprint_speed"), &PlanetCharacter::get_sprint_speed);
	ClassDB::bind_method(D_METHOD("set_climb_speed", "speed"), &PlanetCharacter::set_climb_speed);
	ClassDB::bind_method(D_METHOD("get_climb_speed"), &PlanetCharacter::get_climb_speed);
	ClassDB::bind_method(D_METHOD("set_jump_speed", "speed"), &PlanetCharacter::set_jump_speed);
	ClassDB::bind_method(D_METHOD("get_jump_speed"), &PlanetCharacter::get_jump_speed);
	ClassDB::bind_method(D_METHOD("set_gravity", "gravity"), &PlanetCharacter::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &PlanetCharacter::get_gravity);
	ClassDB::bind_method(D_METHOD("set_can_climb", "can"), &PlanetCharacter::set_can_climb);
	ClassDB::bind_method(D_METHOD("get_can_climb"), &PlanetCharacter::get_can_climb);
	ClassDB::bind_method(D_METHOD("add_carve", "centre", "radius"), &PlanetCharacter::add_carve);
	ClassDB::bind_method(D_METHOD("clear_carves"), &PlanetCharacter::clear_carves);
	ClassDB::bind_method(D_METHOD("get_ground_radius", "direction"), &PlanetCharacter::get_ground_radius);
	ClassDB::bind_method(D_METHOD("is_inside", "point"), &PlanetCharacter::is_inside);
	ClassDB::bind_method(D_METHOD("set_physics_space", "space"), &PlanetCharacter::set_physics_space);
	ClassDB::bind_method(D_METHOD("get_physics_space"), &PlanetCharacter::get_physics_space);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &PlanetCharacter::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &PlanetCharacter::get_collision_mask);
	ClassDB::bind_method(D_METHOD("add_exclude", "body"), &PlanetCharacter::add_exclude);
	ClassDB::bind_method(D_METHOD("clear_excludes"), &PlanetCharacter::clear_excludes);
	ClassDB::bind_method(D_METHOD("set_use_terrain", "use"), &PlanetCharacter::set_use_terrain);
	ClassDB::bind_method(D_METHOD("get_use_terrain"), &PlanetCharacter::get_use_terrain);
	ClassDB::bind_method(D_METHOD("set_gravity_centre", "centre"), &PlanetCharacter::set_gravity_centre);
	ClassDB::bind_method(D_METHOD("get_gravity_centre"), &PlanetCharacter::get_gravity_centre);
	ClassDB::bind_method(D_METHOD("set_uniform_down", "down"), &PlanetCharacter::set_uniform_down);
	ClassDB::bind_method(D_METHOD("get_uniform_down"), &PlanetCharacter::get_uniform_down);
	ClassDB::bind_method(D_METHOD("get_up_at", "point"), &PlanetCharacter::get_up_at);
	ClassDB::bind_method(D_METHOD("cast_ray", "from", "to"), &PlanetCharacter::cast_ray);
	ClassDB::bind_method(D_METHOD("add_shell", "centre", "inner_radius", "outer_radius"), &PlanetCharacter::add_shell);
	ClassDB::bind_method(D_METHOD("clear_shells"), &PlanetCharacter::clear_shells);
	ClassDB::bind_method(D_METHOD("place", "point"), &PlanetCharacter::place);
	ClassDB::bind_method(D_METHOD("step", "move", "yaw", "buttons", "delta"), &PlanetCharacter::step, DEFVAL(1.0 / PlanetNet::TICK_HZ));
	ClassDB::bind_method(D_METHOD("get_position"), &PlanetCharacter::get_position);
	ClassDB::bind_method(D_METHOD("get_velocity"), &PlanetCharacter::get_velocity);
	ClassDB::bind_method(D_METHOD("get_rotation"), &PlanetCharacter::get_rotation);
	ClassDB::bind_method(D_METHOD("get_reference"), &PlanetCharacter::get_reference);
	ClassDB::bind_method(D_METHOD("get_mode"), &PlanetCharacter::get_mode);
	ClassDB::bind_method(D_METHOD("get_normal"), &PlanetCharacter::get_normal);
	ClassDB::bind_method(D_METHOD("get_stamina"), &PlanetCharacter::get_stamina);
	ClassDB::bind_method(D_METHOD("set_stamina", "stamina"), &PlanetCharacter::set_stamina);
	ClassDB::bind_method(D_METHOD("get_stamina_max"), &PlanetCharacter::get_stamina_max);
	ClassDB::bind_method(D_METHOD("set_stamina_max", "max"), &PlanetCharacter::set_stamina_max);
	ClassDB::bind_method(D_METHOD("is_exhausted"), &PlanetCharacter::is_exhausted);
	ClassDB::bind_method(D_METHOD("get_action"), &PlanetCharacter::get_action);
	ClassDB::bind_method(D_METHOD("get_action_ticks"), &PlanetCharacter::get_action_ticks);
	ClassDB::bind_method(D_METHOD("is_invulnerable"), &PlanetCharacter::is_invulnerable);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "field", PROPERTY_HINT_RESOURCE_TYPE, "PlanetField3D"), "set_field", "get_field");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planet_radius", PROPERTY_HINT_NONE, "suffix:m"), "set_planet_radius", "get_planet_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "walk_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_walk_speed", "get_walk_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sprint_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_sprint_speed", "get_sprint_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "climb_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_climb_speed", "get_climb_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "jump_speed", PROPERTY_HINT_NONE, "suffix:m/s"), "set_jump_speed", "get_jump_speed");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "can_climb"), "set_can_climb", "get_can_climb");
	ADD_PROPERTY(PropertyInfo(Variant::RID, "physics_space"), "set_physics_space", "get_physics_space");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_terrain"), "set_use_terrain", "get_use_terrain");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "gravity_centre"), "set_gravity_centre", "get_gravity_centre");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uniform_down"), "set_uniform_down", "get_uniform_down");
}
