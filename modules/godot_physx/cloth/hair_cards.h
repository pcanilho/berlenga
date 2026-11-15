/**************************************************************************/
/*  hair_cards.h                                                          */
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

#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

// The clumps far hair is drawn by (docs/ideas/hair.md, stage 3, thick strands): the strands
// clustered into clumps (k-means, Wang et al. 2009's shape distance), each clump's mean strand with
// rotation-minimising frames (as K. Wu et al.'s DiffHairCard, 2025) and its cross-section: two
// axes and the strands' spread along each.
class HairCardBake : public RefCounted {
	GDSOFTCLASS(HairCardBake, RefCounted);

public:
	int card_count = 0;
	int samples = 0; // points per card
	PackedVector3Array points; // groom space, samples per card: the card's mean strand
	PackedInt32Array strand_counts; // per card: the strands in its clump
	PackedInt32Array strand_card; // per groom strand: its card
	// Per card point, the clump's cross-section: two axes (0: along the strands' widest spread,
	// 1: across that; unit, groom space) and the strands' half spread along each (m).
	PackedVector3Array axis[2];
	PackedFloat32Array half_width[2];

	// p_points / p_counts: the groom. p_samples: points per card (2 or more).
	static Ref<HairCardBake> bake(const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_cards, int p_samples, float p_root_width, float p_tip_width);
	// Shared between PhysXHair3D nodes on the same groom with the same settings (a crowd).
	static Ref<HairCardBake> get_cached(uint64_t p_key, const PackedVector3Array &p_points, const PackedInt32Array &p_counts, int p_cards, int p_samples, float p_root_width, float p_tip_width);
	static void clear_cache();
};
