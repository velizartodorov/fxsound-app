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
 *   (H(z) = gain*(1-2z^-1+z^-2)/(1-a1*z^-1-a0*z^-2), the same difference
 *   equation as filtRun2ndHighPass()) because the RBJ numerator (b0, b1, b2) =
 *   gain*(1, -2, 1) is already in that 1:-2:1 ratio. The coefficients are kept
 *   in double precision and run by filtBrickwallRunSection() below.
 */
void PT_DECLSPEC filtBrickwallDesignHighPass(double r_cutoff_hz, double r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	double w0 = MTH_TWO_PI * r_cutoff_hz / r_sample_rate_hz;
	double cos_w0 = cos(w0);
	double alpha = sin(w0) / (2.0 * FILT_BRICKWALL_Q);
	double a0_rbj = 1.0 + alpha;

	cp_coeffs->gain = (1.0 + cos_w0) / (2.0 * a0_rbj);
	cp_coeffs->a1 = (2.0 * cos_w0) / a0_rbj;
	cp_coeffs->a0 = (alpha - 1.0) / a0_rbj;
}

/*
 * FUNCTION: filtBrickwallDesignLowPass()
 * DESCRIPTION:
 *   Designs one 2nd-order Butterworth low-pass section. See filtBrickwallDesignHighPass().
 */
void PT_DECLSPEC filtBrickwallDesignLowPass(double r_cutoff_hz, double r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	double w0 = MTH_TWO_PI * r_cutoff_hz / r_sample_rate_hz;
	double cos_w0 = cos(w0);
	double alpha = sin(w0) / (2.0 * FILT_BRICKWALL_Q);
	double a0_rbj = 1.0 + alpha;

	cp_coeffs->gain = (1.0 - cos_w0) / (2.0 * a0_rbj);
	cp_coeffs->a1 = (2.0 * cos_w0) / a0_rbj;
	cp_coeffs->a0 = (alpha - 1.0) / a0_rbj;
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
		sp_state->hp_sections[i].in_minus1 = 0.0;
		sp_state->hp_sections[i].in_minus2 = 0.0;
		sp_state->hp_sections[i].out_minus1 = 0.0;
		sp_state->hp_sections[i].out_minus2 = 0.0;

		sp_state->lp_sections[i].in_minus1 = 0.0;
		sp_state->lp_sections[i].in_minus2 = 0.0;
		sp_state->lp_sections[i].out_minus1 = 0.0;
		sp_state->lp_sections[i].out_minus2 = 0.0;
	}
}

/*
 * FUNCTION: filtBrickwallRunSection()  [internal]
 * DESCRIPTION:
 *   Runs one sample through one 2nd-order section, direct form I, in double
 *   precision: y[n] = a1*y[n-1] + a0*y[n-2] + gain*(x[n] + d_mid*x[n-1] + x[n-2]),
 *   with d_mid = -2 for a high-pass and +2 for a low-pass section (the same
 *   difference equation as filtRun2ndHighPass()/LowPass() in FiltRun.cpp, which
 *   run in realtype). Outputs this close to zero are flushed to exactly zero, so
 *   history decaying in digital silence never lingers in the slow denormal range
 *   (1e-30 is about -600dBFS, far below anything audible).
 */
static inline double filtBrickwallRunSection(double d_in, double d_mid, const FiltBrickwallBiquadCoeffs *cp_coeffs,
	FiltBrickwallBiquadState *sp_section)
{
	double d_out = cp_coeffs->a1 * sp_section->out_minus1 + cp_coeffs->a0 * sp_section->out_minus2 +
		cp_coeffs->gain * (d_in + d_mid * sp_section->in_minus1 + sp_section->in_minus2);

	if (d_out < 1e-30 && d_out > -1e-30)
	{
		d_out = 0.0;
	}

	sp_section->in_minus2 = sp_section->in_minus1;
	sp_section->in_minus1 = d_in;
	sp_section->out_minus2 = sp_section->out_minus1;
	sp_section->out_minus1 = d_out;

	return d_out;
}

/*
 * FUNCTION: filtBrickwallProcessSample()
 * DESCRIPTION:
 *   Runs one sample through i_num_hp_sections identical cascaded high-pass
 *   sections followed by i_num_lp_sections identical cascaded low-pass sections,
 *   in double precision (see FiltBrickwallBiquadCoeffs). Real-time safe: no
 *   allocation, no locking.
 */
realtype PT_DECLSPEC filtBrickwallProcessSample(realtype r_input, int i_num_hp_sections, int i_num_lp_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs,
	FiltBrickwallChannelState *sp_state)
{
	double d_sample = (double)r_input;
	int i;

	for (i = 0; i < i_num_hp_sections; i++)
	{
		d_sample = filtBrickwallRunSection(d_sample, -2.0, cp_hp_coeffs, &(sp_state->hp_sections[i]));
	}

	for (i = 0; i < i_num_lp_sections; i++)
	{
		d_sample = filtBrickwallRunSection(d_sample, 2.0, cp_lp_coeffs, &(sp_state->lp_sections[i]));
	}

	return (realtype)d_sample;
}

/*
 * FUNCTION: filtBrickwallCalcBandResponseDb()  [internal]
 * DESCRIPTION:
 *   Evaluates one band's (high-pass or low-pass) cascaded magnitude response, in
 *   dB, at r_freq_hz, directly in double precision. For one section,
 *   H(z) = gain*(1 +/- 2z^-1 + z^-2) / (1 - a1*z^-1 - a0*z^-2) (see
 *   filtBrickwallRunSection()). The numerator is gain*(1 -/+ z^-1)^2, whose power
 *   at w is gain^2*16*sin^4(w/2) (high-pass) or gain^2*16*cos^4(w/2) (low-pass) -
 *   written that way to avoid the cancellation 1 - 2cos(w) + cos(2w) suffers at
 *   the very low normalized frequencies of a 5-20Hz cutoff. Shared by
 *   filtBrickwallCalcResponseDb() and filtBrickwallCalibrateCascadeCutoff().
 */
static double filtBrickwallCalcBandResponseDb(double r_freq_hz, double r_sample_rate_hz, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_coeffs, int i_high_pass_flag)
{
	double w = MTH_TWO_PI * r_freq_hz / r_sample_rate_hz;
	double half = i_high_pass_flag ? sin(w / 2.0) : cos(w / 2.0);
	double half_sq = half * half;
	double num_power = 16.0 * half_sq * half_sq * cp_coeffs->gain * cp_coeffs->gain;
	double den_re = 1.0 - cp_coeffs->a1 * cos(w) - cp_coeffs->a0 * cos(2.0 * w);
	double den_im = cp_coeffs->a1 * sin(w) + cp_coeffs->a0 * sin(2.0 * w);
	double den_power = den_re * den_re + den_im * den_im;
	double power;
	double total_power;

	if (i_num_sections <= 0)
	{
		return 0.0;
	}

	power = (den_power > 0.0) ? num_power / den_power : 0.0;
	total_power = pow(power, i_num_sections);
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
double PT_DECLSPEC filtBrickwallCalcResponseDb(double r_freq_hz, double r_sample_rate_hz, int i_num_hp_sections, int i_num_lp_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs)
{
	double hp_db = filtBrickwallCalcBandResponseDb(r_freq_hz, r_sample_rate_hz, i_num_hp_sections, cp_hp_coeffs, 1);
	double lp_db = filtBrickwallCalcBandResponseDb(r_freq_hz, r_sample_rate_hz, i_num_lp_sections, cp_lp_coeffs, 0);

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
double PT_DECLSPEC filtBrickwallCalibrateCascadeCutoff(double r_target_cutoff_hz, double r_sample_rate_hz, int i_num_sections, int i_high_pass_flag)
{
	double lo, hi, nyquist;
	int iteration;

	nyquist = r_sample_rate_hz / 2.0;

	if (i_high_pass_flag)
	{
		lo = 0.01;
		hi = r_target_cutoff_hz;
	}
	else
	{
		lo = r_target_cutoff_hz;
		hi = nyquist * 0.999;
	}

	for (iteration = 0; iteration < 60; iteration++)
	{
		FiltBrickwallBiquadCoeffs coeffs;
		double mid = (lo + hi) / 2.0;
		double response_db;

		if (i_high_pass_flag)
		{
			filtBrickwallDesignHighPass(mid, r_sample_rate_hz, &coeffs);
		}
		else
		{
			filtBrickwallDesignLowPass(mid, r_sample_rate_hz, &coeffs);
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
