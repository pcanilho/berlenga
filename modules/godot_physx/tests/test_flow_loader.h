/**************************************************************************/
/*  test_flow_loader.h                                                    */
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

#pragma once

#ifdef GODOT_PHYSX_FLOW

#include "../flow/flow_backend.h"

#include "tests/test_macros.h"

namespace TestFlowLoader {

// A build or release without the Flow runtime libraries must fail softly: Flow's own loader closed
// both handles unconditionally, and dlclose(NULL) crashed the engine.
TEST_CASE("[Modules][PhysX] Missing Flow libraries fail softly") {
	char err[512] = {};
	CHECK_FALSE(FlowBackend::acquire_library_named("nvflow_missing.dll", "nvflowext_missing.dll", err, sizeof(err)));
	CHECK(err[0] != '\0');
}

TEST_CASE("[Modules][PhysX] One of two Flow libraries missing fails softly") {
	char err[512] = {};
	// libc loads fine and has none of Flow's entry points.
#ifdef _WIN32
	CHECK_FALSE(FlowBackend::acquire_library_named("kernel32.dll", "nvflowext_missing.dll", err, sizeof(err)));
	CHECK_FALSE(FlowBackend::acquire_library_named("kernel32.dll", "kernel32.dll", err, sizeof(err)));
#else
	CHECK_FALSE(FlowBackend::acquire_library_named("libc.so.6", "libnvflowext_missing.so", err, sizeof(err)));
	CHECK_FALSE(FlowBackend::acquire_library_named("libc.so.6", "libc.so.6", err, sizeof(err)));
#endif
	CHECK(err[0] != '\0');
}

} // namespace TestFlowLoader

#endif // GODOT_PHYSX_FLOW
