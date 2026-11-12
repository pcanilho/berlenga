/**************************************************************************/
/*  render_raytracing_volumes.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2026-present Paulo Canilho and Berlenga contributors.    */
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

#include "render_raytracing_volumes.h"

RTSparseVolume RTVolumes::volume;
bool RTVolumes::present = false;
RTDenseVolumes RTVolumes::dense;
Mutex RTVolumes::dense_mutex;

void RTVolumes::set_volume(const RTSparseVolume &p_volume) {
	volume = p_volume;
	present = p_volume.density.is_valid() && p_volume.table.is_valid();
}

void RTVolumes::clear() {
	present = false;
	volume = RTSparseVolume();
}

bool RTVolumes::get_volume(RTSparseVolume &r_volume) {
	if (!present) {
		return false;
	}
	r_volume = volume;
	return true;
}

void RTVolumes::set_dense(const RTDenseVolumes &p_dense) {
	MutexLock lock(dense_mutex);
	dense = p_dense;
}

bool RTVolumes::get_dense(RTDenseVolumes &r_dense) {
	MutexLock lock(dense_mutex);
	if (!dense.texture.is_valid() || dense.boxes.is_empty()) {
		return false;
	}
	r_dense = dense;
	return true;
}
