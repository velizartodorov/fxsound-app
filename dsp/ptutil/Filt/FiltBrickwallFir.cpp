/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <math.h>

#include "codedefs.h"
#include "mth.h"
#include "FiltBrickwallFir.h"

/*
 * FUNCTION: filtBrickwallFirSinc()  [internal]
 * DESCRIPTION:
 *   Normalized sinc: sin(pi*x)/(pi*x), with sinc(0) = 1.
 */
static double filtBrickwallFirSinc(double x)
{
	if (fabs(x) < 1e-12)
	{
		return 1.0;
	}

	return sin(MTH_PI * x) / (MTH_PI * x);
}

/*
 * FUNCTION: filtBrickwallFirBesselI0()  [internal]
 * DESCRIPTION:
 *   Zeroth-order modified Bessel function of the first kind, by its power
 *   series (converges quickly for the beta values used here, ~10).
 */
static double filtBrickwallFirBesselI0(double x)
{
	double sum = 1.0;
	double term = 1.0;
	double half_x = x / 2.0;
	int k;

	for (k = 1; k < 100; k++)
	{
		term *= (half_x / (double)k) * (half_x / (double)k);
		sum += term;
		if (term < sum * 1e-12)
		{
			break;
		}
	}

	return sum;
}

/*
 * FUNCTION: filtBrickwallFirNumTaps()
 */
int PT_DECLSPEC filtBrickwallFirNumTaps(realtype r_latency_ms, realtype r_sample_rate_hz, int i_block_size)
{
	int latency_samples = (int)floor((double)r_latency_ms * (double)r_sample_rate_hz / 1000.0 + 0.5);
	int num_taps = 2 * (latency_samples - i_block_size) + 1;

	if (num_taps < 1)
	{
		num_taps = 1;
	}

	return num_taps;
}

/*
 * FUNCTION: filtBrickwallFirDesignKaiser()
 * DESCRIPTION:
 *   Ideal high-pass impulse response (a unit impulse at the centre tap minus
 *   the ideal low-pass impulse response at r_hp_cutoff_hz), truncated to
 *   i_num_taps and shaped by a Kaiser window. The window is exactly 1 at the
 *   centre tap, so the impulse is kept intact and only the low-pass part is
 *   shaped. Kaiser trades transition width against stopband depth
 *   explicitly: beta = 0.1102 * (A - 8.7) for A dB of attenuation, and the
 *   transition width is roughly (A - 8) * fs / (14.36 * N) Hz.
 */
void PT_DECLSPEC filtBrickwallFirDesignKaiser(realtype r_hp_cutoff_hz, realtype r_sample_rate_hz,
	int i_num_taps, realtype *rp_coeffs_out)
{
	double beta = 0.1102 * (FILT_BRICKWALL_FIR_KAISER_ATTEN_DB - 8.7);
	double i0_beta = filtBrickwallFirBesselI0(beta);
	double m_max = (double)(i_num_taps - 1);
	double center = m_max / 2.0;
	int center_tap = (i_num_taps - 1) / 2;
	double wc_hp = MTH_TWO_PI * (double)r_hp_cutoff_hz / (double)r_sample_rate_hz;
	int n;

	for (n = 0; n < i_num_taps; n++)
	{
		double m = (double)n - center;
		double impulse = (n == center_tap) ? 1.0 : 0.0;
		double hp_as_lp = (wc_hp / MTH_PI) * filtBrickwallFirSinc(wc_hp * m / MTH_PI);
		double window = 1.0;

		if (i_num_taps > 1)
		{
			double r = 2.0 * (double)n / m_max - 1.0;
			double inside = 1.0 - r * r;

			if (inside < 0.0)
			{
				inside = 0.0;
			}
			window = filtBrickwallFirBesselI0(beta * sqrt(inside)) / i0_beta;
		}

		rp_coeffs_out[n] = (realtype)((impulse - hp_as_lp) * window);
	}
}

/*
 * FUNCTION: filtBrickwallFirCalcResponseDb()
 * DESCRIPTION:
 *   Direct DTFT evaluation: H(e^jw) = sum_n h[n]*e^(-jwn), w = 2*pi*f/fs.
 *   For verification/testing only.
 */
double PT_DECLSPEC filtBrickwallFirCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, const realtype *cp_coeffs, int i_num_taps)
{
	double w = MTH_TWO_PI * (double)r_freq_hz / (double)r_sample_rate_hz;
	double real = 0.0;
	double imag = 0.0;
	double magnitude;
	int n;

	for (n = 0; n < i_num_taps; n++)
	{
		real += (double)cp_coeffs[n] * cos(w * (double)n);
		imag -= (double)cp_coeffs[n] * sin(w * (double)n);
	}

	magnitude = sqrt(real * real + imag * imag);
	if (magnitude < 1e-9)
	{
		magnitude = 1e-9;
	}

	return 20.0 * log10(magnitude);
}
