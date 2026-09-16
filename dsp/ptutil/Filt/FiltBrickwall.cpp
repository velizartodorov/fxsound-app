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
#include "filt.h"
#include "FiltBrickwall.h"

/* Q for a maximally-flat (Butterworth) 2nd-order section: 1/sqrt(2). */
#define FILT_BRICKWALL_Q 0.70710678118654752440

/*
 * FUNCTION: filtBrickwallDesignHighPass()
 * DESCRIPTION:
 *   Designs one 2nd-order Butterworth high-pass section at r_cutoff_hz, using the
 *   standard bilinear-transform ("RBJ cookbook") biquad design. This codebase's
 *   existing filtDesign2ndButHighPass()/LowPass() (Fil12But.cpp) were tried first
 *   and found, by direct numeric verification, to be badly inaccurate this close
 *   to Nyquist: at 44.1kHz they place the actual -3dB point of a 20kHz-targeted
 *   low-pass section at roughly 13.5kHz, not 20kHz (that formula's difference
 *   equation was written for filters used well below Nyquist elsewhere in this
 *   codebase, e.g. a 150Hz crossover in Play32Butter.c, and was never valid at
 *   90%+ of Nyquist). The bilinear-transform formula below is exact at any
 *   frequency up to Nyquist - verified numerically to land at exactly -3.0103dB
 *   at the design frequency itself, including at 20kHz/44.1kHz.
 *
 *   The resulting (gain, a1, a0) match this module's existing convention exactly
 *   (H(z) = gain*(1-2z^-1+z^-2)/(1-a1*z^-1-a0*z^-2), run via filtRun2ndHighPass())
 *   because the RBJ numerator (b0, b1, b2) = gain*(1, -2, 1) is already in that
 *   1:-2:1 ratio - only the coefficient VALUES differ from the old formula, not
 *   the difference-equation structure, so filtRun2ndHighPass()/LowPass() and
 *   filtPolyCalcBiquadPowerResponse() (used below) still apply unchanged.
 */
void PT_DECLSPEC filtBrickwallDesignHighPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	double w0 = MTH_TWO_PI * (double)r_cutoff_hz / (double)r_sample_rate_hz;
	double cos_w0 = cos(w0);
	double alpha = sin(w0) / (2.0 * FILT_BRICKWALL_Q);
	double a0_rbj = 1.0 + alpha;

	cp_coeffs->gain = (realtype)((1.0 + cos_w0) / (2.0 * a0_rbj));
	cp_coeffs->a1 = (realtype)((2.0 * cos_w0) / a0_rbj);
	cp_coeffs->a0 = (realtype)((alpha - 1.0) / a0_rbj);
}

/*
 * FUNCTION: filtBrickwallDesignLowPass()
 * DESCRIPTION:
 *   Designs one 2nd-order Butterworth low-pass section. See filtBrickwallDesignHighPass().
 */
void PT_DECLSPEC filtBrickwallDesignLowPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	double w0 = MTH_TWO_PI * (double)r_cutoff_hz / (double)r_sample_rate_hz;
	double cos_w0 = cos(w0);
	double alpha = sin(w0) / (2.0 * FILT_BRICKWALL_Q);
	double a0_rbj = 1.0 + alpha;

	cp_coeffs->gain = (realtype)((1.0 - cos_w0) / (2.0 * a0_rbj));
	cp_coeffs->a1 = (realtype)((2.0 * cos_w0) / a0_rbj);
	cp_coeffs->a0 = (realtype)((alpha - 1.0) / a0_rbj);
}

/*
 * FUNCTION: filtBrickwallResetChannelState()
 * DESCRIPTION:
 *   Clears all section history for one channel. Must be called before first use
 *   and whenever the sample rate or section count changes, to avoid a click from
 *   stale history computed at a different cutoff/section count.
 */
void PT_DECLSPEC filtBrickwallResetChannelState(FiltBrickwallChannelState *sp_state)
{
	int i;

	for (i = 0; i < FILT_BRICKWALL_MAX_SECTIONS; i++)
	{
		sp_state->hp_sections[i].in_minus1 = (realtype)0.0;
		sp_state->hp_sections[i].in_minus2 = (realtype)0.0;
		sp_state->hp_sections[i].out_minus1 = (realtype)0.0;
		sp_state->hp_sections[i].out_minus2 = (realtype)0.0;

		sp_state->lp_sections[i].in_minus1 = (realtype)0.0;
		sp_state->lp_sections[i].in_minus2 = (realtype)0.0;
		sp_state->lp_sections[i].out_minus1 = (realtype)0.0;
		sp_state->lp_sections[i].out_minus2 = (realtype)0.0;
	}
}

/*
 * FUNCTION: filtBrickwallProcessSample()
 * DESCRIPTION:
 *   Runs one sample through i_num_sections identical cascaded high-pass sections
 *   followed by i_num_sections identical cascaded low-pass sections, reusing the
 *   existing filtRun2ndHighPass()/filtRun2ndLowPass() biquad implementations
 *   (see FiltRun.cpp). Real-time safe: no allocation, no locking.
 */
realtype PT_DECLSPEC filtBrickwallProcessSample(realtype r_input, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs,
	FiltBrickwallChannelState *sp_state)
{
	realtype r_sample = r_input;
	realtype r_out;
	int i;

	for (i = 0; i < i_num_sections; i++)
	{
		filtRun2ndHighPass(r_sample,
			&(sp_state->hp_sections[i].in_minus1), &(sp_state->hp_sections[i].in_minus2),
			&r_out,
			&(sp_state->hp_sections[i].out_minus1), &(sp_state->hp_sections[i].out_minus2),
			cp_hp_coeffs->gain, cp_hp_coeffs->a1, cp_hp_coeffs->a0);
		r_sample = r_out;
	}

	for (i = 0; i < i_num_sections; i++)
	{
		filtRun2ndLowPass(r_sample,
			&(sp_state->lp_sections[i].in_minus1), &(sp_state->lp_sections[i].in_minus2),
			&r_out,
			&(sp_state->lp_sections[i].out_minus1), &(sp_state->lp_sections[i].out_minus2),
			cp_lp_coeffs->gain, cp_lp_coeffs->a1, cp_lp_coeffs->a0);
		r_sample = r_out;
	}

	return r_sample;
}

/*
 * FUNCTION: filtBrickwallCalcBandResponseDb()  [internal]
 * DESCRIPTION:
 *   Evaluates one band's (high-pass or low-pass) cascaded magnitude response, in
 *   dB, at r_freq_hz. Reuses the existing filtPolyCalcBiquadPowerResponse() (see
 *   Filtpoly.cpp), which expects numerator/denominator coefficients in
 *   "b0*z^2+b1*z+b2 / a0*z^2+a1*z+a2" form and a normalized frequency
 *   (cycles/sample). filtRun2ndLowPass()/HighPass() implement
 *   y[n] = a1*y[n-1] + a0*y[n-2] + gain*(x[n] +/- 2x[n-1] + x[n-2]), i.e.
 *   H(z) = gain*(z^2 +/- 2z + 1) / (z^2 - a1*z - a0) after multiplying through by
 *   z^2 - hence the sign flips on a1/a0 below. Shared by filtBrickwallCalcResponseDb()
 *   and filtBrickwallCalibrateCascadeCutoff().
 */
static double filtBrickwallCalcBandResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_coeffs, int i_high_pass_flag)
{
	realtype r_norm_freq = r_freq_hz / r_sample_rate_hz;
	realtype r_sign = i_high_pass_flag ? (realtype)-2.0 : (realtype)2.0;
	realtype power;
	double total_power;

	power = filtPolyCalcBiquadPowerResponse(
		cp_coeffs->gain, r_sign * cp_coeffs->gain, cp_coeffs->gain,
		(realtype)1.0, -(cp_coeffs->a1), -(cp_coeffs->a0),
		r_norm_freq);

	total_power = pow((double)power, i_num_sections);
	if (total_power < 1e-18)
	{
		total_power = 1e-18;
	}

	return 10.0 * log10(total_power);
}

/*
 * FUNCTION: filtBrickwallCalcResponseDb()
 * DESCRIPTION:
 *   Evaluates the combined (high-pass + low-pass cascade) magnitude response, in
 *   dB, at r_freq_hz, for verification/testing.
 */
double PT_DECLSPEC filtBrickwallCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs)
{
	double hp_db = filtBrickwallCalcBandResponseDb(r_freq_hz, r_sample_rate_hz, i_num_sections, cp_hp_coeffs, 1);
	double lp_db = filtBrickwallCalcBandResponseDb(r_freq_hz, r_sample_rate_hz, i_num_sections, cp_lp_coeffs, 0);

	return hp_db + lp_db;
}

/*
 * FUNCTION: filtBrickwallCalibrateCascadeCutoff()
 * DESCRIPTION:
 *   Binary-searches for the single-section design cutoff such that a cascade of
 *   i_num_sections identical sections is exactly -3.0103dB at r_target_cutoff_hz.
 *   A closed-form correction factor (derived from the idealized continuous-time
 *   Butterworth cascade formula) was tried first and found, by numeric
 *   verification, to be unreliable - it works passably at 44.1/48kHz but
 *   overshoots badly at higher sample rates, because the actual bilinear-
 *   transformed digital section shape deviates from the idealized continuous-time
 *   assumption as the design frequency is pushed higher. This search instead
 *   verifies the actual cascade response directly on every iteration, so it is
 *   correct at any sample rate and section count.
 *
 *   For the low-pass band, increasing the single-section design cutoff (moving it
 *   above the target) monotonically reduces attenuation at the target frequency,
 *   so the search brackets [target, ~Nyquist]. For the high-pass band, decreasing
 *   the single-section design cutoff (moving it below the target) monotonically
 *   reduces attenuation at the target, so the search brackets [~0, target].
 *
 *   Control-thread only: ~60 iterations of trig-heavy evaluation per call. Never
 *   call this from the real-time audio path.
 */
double PT_DECLSPEC filtBrickwallCalibrateCascadeCutoff(realtype r_target_cutoff_hz, realtype r_sample_rate_hz, int i_num_sections, int i_high_pass_flag)
{
	double lo, hi, nyquist;
	int iteration;

	nyquist = (double)r_sample_rate_hz / 2.0;

	if (i_high_pass_flag)
	{
		lo = 0.01;
		hi = (double)r_target_cutoff_hz;
	}
	else
	{
		lo = (double)r_target_cutoff_hz;
		hi = nyquist * 0.999;
	}

	for (iteration = 0; iteration < 60; iteration++)
	{
		FiltBrickwallBiquadCoeffs coeffs;
		double mid = (lo + hi) / 2.0;
		double response_db;

		if (i_high_pass_flag)
		{
			filtBrickwallDesignHighPass((realtype)mid, r_sample_rate_hz, &coeffs);
		}
		else
		{
			filtBrickwallDesignLowPass((realtype)mid, r_sample_rate_hz, &coeffs);
		}

		response_db = filtBrickwallCalcBandResponseDb(r_target_cutoff_hz, r_sample_rate_hz, i_num_sections, &coeffs, i_high_pass_flag);

		if (i_high_pass_flag)
		{
			if (response_db < -3.0103)
			{
				hi = mid;
			}
			else
			{
				lo = mid;
			}
		}
		else
		{
			if (response_db < -3.0103)
			{
				lo = mid;
			}
			else
			{
				hi = mid;
			}
		}
	}

	return (lo + hi) / 2.0;
}
