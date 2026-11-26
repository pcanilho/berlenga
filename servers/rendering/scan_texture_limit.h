/**************************************************************************/
/*  scan_texture_limit.h                                                  */
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

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "servers/rendering/rendering_device.h"

// Scanned textures that systems load from files at run time (ground, stone, cave, forest floor and bark scans; no
// import step, so no import size limit) are halved to at most rendering/textures/scans/cpu_device_max_size on a CPU
// (software) rendering device. There every texture is system memory and every sample costs CPU time, and a full
// scan set at 2K and up does not fit beside a path traced scene. GPU devices keep every scan at its full size.
// The halving is a 2x2 box (Image::shrink_x2), so what is left equals the full texture's next mip levels.

// The largest edge a scan keeps on this device, 0 for no limit.
inline int scan_texture_max_size() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || rd->get_device_type() != RenderingDeviceEnums::DEVICE_TYPE_CPU) {
		return 0;
	}
	return MAX(int(GLOBAL_GET("rendering/textures/scans/cpu_device_max_size")), 0);
}

// Halves p_image in place until its longer edge is within the limit (before mipmaps are generated).
inline void scan_texture_limit(const Ref<Image> &p_image) {
	const int max_size = scan_texture_max_size();
	if (max_size <= 0 || p_image.is_null() || p_image->is_empty()) {
		return;
	}
	if (p_image->has_mipmaps()) {
		p_image->clear_mipmaps();
	}
	while (MAX(p_image->get_width(), p_image->get_height()) > max_size && MIN(p_image->get_width(), p_image->get_height()) > 1) {
		p_image->shrink_x2();
	}
}
