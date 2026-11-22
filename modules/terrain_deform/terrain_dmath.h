/**************************************************************************/
/*  terrain_dmath.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                                BERLENGA                                */
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

#include <cmath>
#include <cstdint>

// Deterministic transcendental functions for the terrain generator. The C library's sin, exp, pow and the rest are
// not the same function on every platform (glibc, the MSVC runtime and Apple's libm round differently in the last
// bits), and erosion amplifies a last-bit difference into a different world: the same seed gave other heights on
// Windows than on Linux. These use only + - * /, sqrt, floor and ldexp/frexp, which IEEE 754 defines exactly (and
// the engine builds with contraction off: -ffp-contract=off, MSVC /fp:strict), so every platform computes the
// same bits. Range reduction (Cody and Waite 1980) and short polynomials (Taylor or odd series on a narrow range,
// Muller, "Elementary Functions", 2016), all in double; float overloads round the double result once.
namespace TerrainDMath {

static constexpr double PI_D = 3.14159265358979323846;
static constexpr double LN2_HI = 6.93147180369123816490e-01;
static constexpr double LN2_LO = 1.90821492927058770002e-10;
static constexpr double LN2 = 0.69314718055994530942;
static constexpr double INV_LN2 = 1.44269504088896338700;
// pi / 2 in three parts (the first two with trailing zero bits), for an exact first reduction step.
static constexpr double PIO2_1 = 1.57079632673412561417e+00;
static constexpr double PIO2_2 = 6.07710050650619224932e-11;
static constexpr double PIO2_3 = 2.02226624879595063154e-21;

inline double exp(double x) {
	if (x != x) {
		return x;
	}
	if (x > 709.7) {
		return HUGE_VAL;
	}
	if (x < -745.2) {
		return 0.0;
	}
	const double k = std::floor(x * INV_LN2 + 0.5);
	const double r = (x - k * LN2_HI) - k * LN2_LO; // |r| <= ln2 / 2
	// Taylor to r^14 / 14!: the next term is under 1e-17 relative on |r| <= 0.347.
	double p = 1.0 / 87178291200.0;
	p = p * r + 1.0 / 6227020800.0;
	p = p * r + 1.0 / 479001600.0;
	p = p * r + 1.0 / 39916800.0;
	p = p * r + 1.0 / 3628800.0;
	p = p * r + 1.0 / 362880.0;
	p = p * r + 1.0 / 40320.0;
	p = p * r + 1.0 / 5040.0;
	p = p * r + 1.0 / 720.0;
	p = p * r + 1.0 / 120.0;
	p = p * r + 1.0 / 24.0;
	p = p * r + 1.0 / 6.0;
	p = p * r + 0.5;
	p = p * r + 1.0;
	p = p * r + 1.0;
	return std::ldexp(p, int(k));
}

inline double log(double x) {
	if (x != x || x < 0.0) {
		return NAN;
	}
	if (x == 0.0) {
		return -HUGE_VAL;
	}
	if (x > 1.79e308) {
		return x;
	}
	int e = 0;
	double m = std::frexp(x, &e); // m in [0.5, 1)
	if (m < 0.70710678118654752440) {
		m *= 2.0;
		e -= 1;
	}
	// m in [sqrt(1/2), sqrt(2)): log m = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716.
	const double s = (m - 1.0) / (m + 1.0);
	const double s2 = s * s;
	double p = 1.0 / 25.0;
	p = p * s2 + 1.0 / 23.0;
	p = p * s2 + 1.0 / 21.0;
	p = p * s2 + 1.0 / 19.0;
	p = p * s2 + 1.0 / 17.0;
	p = p * s2 + 1.0 / 15.0;
	p = p * s2 + 1.0 / 13.0;
	p = p * s2 + 1.0 / 11.0;
	p = p * s2 + 1.0 / 9.0;
	p = p * s2 + 1.0 / 7.0;
	p = p * s2 + 1.0 / 5.0;
	p = p * s2 + 1.0 / 3.0;
	p = p * s2 + 1.0;
	const double lm = 2.0 * s * p;
	return (double(e) * LN2_HI + lm) + double(e) * LN2_LO;
}

inline double log2(double x) {
	return log(x) * INV_LN2;
}

inline double log10(double x) {
	return log(x) * 0.43429448190325182765;
}

inline double pow(double x, double y) {
	if (y == 0.0) {
		return 1.0;
	}
	if (x != x || y != y) {
		return NAN;
	}
	if (x == 0.0) {
		return y > 0.0 ? 0.0 : HUGE_VAL;
	}
	if (x == 1.0) {
		return 1.0;
	}
	const double yi = std::floor(y);
	const bool integer = yi == y;
	if (integer && std::fabs(y) <= 64.0) {
		// Repeated squaring: exact as far as the products are, and the same on every platform.
		double b = y < 0.0 ? 1.0 / x : x;
		uint32_t n = uint32_t(std::fabs(y));
		double r = 1.0;
		while (n) {
			if (n & 1u) {
				r *= b;
			}
			b *= b;
			n >>= 1u;
		}
		return r;
	}
	if (x < 0.0) {
		if (!integer) {
			return NAN;
		}
		const double r = exp(y * log(-x));
		return std::fmod(std::fabs(yi), 2.0) == 1.0 ? -r : r;
	}
	return exp(y * log(x));
}

// sin and cos of r on [-pi/4, pi/4].
inline double _sin_k(double r) {
	const double r2 = r * r;
	double p = -1.0 / 1307674368000.0;
	p = p * r2 + 1.0 / 6227020800.0;
	p = p * r2 - 1.0 / 39916800.0;
	p = p * r2 + 1.0 / 362880.0;
	p = p * r2 - 1.0 / 5040.0;
	p = p * r2 + 1.0 / 120.0;
	p = p * r2 - 1.0 / 6.0;
	return r + r * r2 * p;
}

inline double _cos_k(double r) {
	const double r2 = r * r;
	double p = 1.0 / 20922789888000.0;
	p = p * r2 - 1.0 / 87178291200.0;
	p = p * r2 + 1.0 / 479001600.0;
	p = p * r2 - 1.0 / 3628800.0;
	p = p * r2 + 1.0 / 40320.0;
	p = p * r2 - 1.0 / 720.0;
	p = p * r2 + 1.0 / 24.0;
	p = p * r2 - 0.5;
	return 1.0 + r2 * p;
}

// x = q pi / 2 + r, |r| <= pi / 4 (Cody-Waite; exact for |x| up to about 1e9, plenty for angles and noise phases).
inline double _reduce(double x, int &r_q) {
	const double q = std::floor(x * (2.0 / PI_D) + 0.5);
	r_q = int(int64_t(q) & 3);
	return ((x - q * PIO2_1) - q * PIO2_2) - q * PIO2_3;
}

inline double sin(double x) {
	if (!std::isfinite(x)) {
		return NAN;
	}
	int q = 0;
	const double r = _reduce(x, q);
	switch (q) {
		case 0:
			return _sin_k(r);
		case 1:
			return _cos_k(r);
		case 2:
			return -_sin_k(r);
		default:
			return -_cos_k(r);
	}
}

inline double cos(double x) {
	if (!std::isfinite(x)) {
		return NAN;
	}
	int q = 0;
	const double r = _reduce(x, q);
	switch (q) {
		case 0:
			return _cos_k(r);
		case 1:
			return -_sin_k(r);
		case 2:
			return -_cos_k(r);
		default:
			return _sin_k(r);
	}
}

inline double tan(double x) {
	if (!std::isfinite(x)) {
		return NAN;
	}
	int q = 0;
	const double r = _reduce(x, q);
	const double s = _sin_k(r), c = _cos_k(r);
	return (q & 1) ? -c / s : s / c;
}

inline double atan(double x) {
	if (x != x) {
		return x;
	}
	const bool neg = x < 0.0;
	double a = std::fabs(x);
	double off = 0.0;
	if (a > 1.0) {
		a = 1.0 / a;
		off = PI_D * 0.5;
	}
	// Two halvings, atan(a) = 2 atan(a / (1 + sqrt(1 + a^2))): |t| <= tan(pi / 16) = 0.199.
	a = a / (1.0 + std::sqrt(1.0 + a * a));
	a = a / (1.0 + std::sqrt(1.0 + a * a));
	const double a2 = a * a;
	double p = 1.0 / 27.0;
	p = -p * a2 + 1.0 / 25.0;
	p = -p * a2 + 1.0 / 23.0;
	p = -p * a2 + 1.0 / 21.0;
	p = -p * a2 + 1.0 / 19.0;
	p = -p * a2 + 1.0 / 17.0;
	p = -p * a2 + 1.0 / 15.0;
	p = -p * a2 + 1.0 / 13.0;
	p = -p * a2 + 1.0 / 11.0;
	p = -p * a2 + 1.0 / 9.0;
	p = -p * a2 + 1.0 / 7.0;
	p = -p * a2 + 1.0 / 5.0;
	p = -p * a2 + 1.0 / 3.0;
	p = -p * a2 + 1.0;
	double r = 4.0 * a * p;
	if (off != 0.0) {
		r = off - r;
	}
	return neg ? -r : r;
}

inline double atan2(double y, double x) {
	if (x != x || y != y) {
		return NAN;
	}
	if (x == 0.0) {
		if (y == 0.0) {
			return std::signbit(x) ? (std::signbit(y) ? -PI_D : PI_D) : y;
		}
		return y > 0.0 ? PI_D * 0.5 : -PI_D * 0.5;
	}
	const double a = atan(y / x);
	if (x > 0.0) {
		return a;
	}
	return std::signbit(y) ? a - PI_D : a + PI_D;
}

inline double asin(double x) {
	if (x != x || x > 1.0 || x < -1.0) {
		return NAN;
	}
	return atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

inline double acos(double x) {
	if (x != x || x > 1.0 || x < -1.0) {
		return NAN;
	}
	return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x);
}

inline double erf(double x) {
	if (x != x) {
		return x;
	}
	const bool neg = x < 0.0;
	const double a = std::fabs(x);
	double r;
	if (a < 2.5) {
		// The Maclaurin series, a fixed 60 terms (converged past double precision for |x| < 2.5).
		const double a2 = a * a;
		double term = a;
		double sum = a;
		for (int n = 1; n < 60; n++) {
			term *= -a2 / double(n);
			sum += term / double(2 * n + 1);
		}
		r = sum * 1.12837916709551257390; // 2 / sqrt(pi)
	} else if (a < 6.0) {
		// erfc by its continued fraction, evaluated bottom up over a fixed 60 levels.
		double f = 0.0;
		for (int k = 60; k >= 1; k--) {
			f = (0.5 * double(k)) / (a + f);
		}
		r = 1.0 - exp(-a * a) / (1.77245385090551602730 * (a + f)); // sqrt(pi)
	} else {
		r = 1.0;
	}
	return neg ? -r : r;
}

// 1 - erf(x); deterministic like erf (the tail past 6 is 0, which callers here treat as no contribution).
inline double erfc(double x) {
	return 1.0 - erf(x);
}

inline float exp(float x) { return float(exp(double(x))); }
inline float log(float x) { return float(log(double(x))); }
inline float log2(float x) { return float(log2(double(x))); }
inline float log10(float x) { return float(log10(double(x))); }
inline float pow(float x, float y) { return float(pow(double(x), double(y))); }
inline float sin(float x) { return float(sin(double(x))); }
inline float cos(float x) { return float(cos(double(x))); }
inline float tan(float x) { return float(tan(double(x))); }
inline float atan(float x) { return float(atan(double(x))); }
inline float atan2(float y, float x) { return float(atan2(double(y), double(x))); }
inline float asin(float x) { return float(asin(double(x))); }
inline float acos(float x) { return float(acos(double(x))); }
inline float erf(float x) { return float(erf(double(x))); }
inline float erfc(float x) { return float(erfc(double(x))); }

} // namespace TerrainDMath
