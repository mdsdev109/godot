/**************************************************************************/
/*  planet_biome.cpp                                                      */
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

#include "core/math/planet_biome.h"

#include <mutex>
#include <unordered_map>

namespace {
std::mutex &registry_mutex() {
	static std::mutex m;
	return m;
}

std::unordered_map<uint64_t, PlanetMacroMapRef> &registry_map() {
	static std::unordered_map<uint64_t, PlanetMacroMapRef> m;
	return m;
}

uint64_t &next_id() {
	static uint64_t id = 1;
	return id;
}
} // namespace

uint64_t PlanetMacroMapRegistry::register_map(const PlanetMacroMapRef &p_map) {
	if (p_map == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(registry_mutex());
	const uint64_t id = next_id()++;
	registry_map()[id] = p_map;
	return id;
}

void PlanetMacroMapRegistry::unregister_map(const uint64_t p_id) {
	if (p_id == 0) {
		return;
	}
	std::lock_guard<std::mutex> guard(registry_mutex());
	registry_map().erase(p_id);
}

PlanetMacroMapRef PlanetMacroMapRegistry::get(const uint64_t p_id) {
	if (p_id == 0) {
		return PlanetMacroMapRef();
	}
	std::lock_guard<std::mutex> guard(registry_mutex());
	const auto it = registry_map().find(p_id);
	return it == registry_map().end() ? PlanetMacroMapRef() : it->second;
}
