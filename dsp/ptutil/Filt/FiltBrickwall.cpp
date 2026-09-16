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

/*
 * FUNCTION: filtBrickwallDesignHighPass()
 * DESCRIPTION:
 *   Designs one 2nd-order Butterworth high-pass section at r_cutoff_hz, reusing
 *   the existing fixed-Q Butterworth design formula (filtDesign2ndButHighPass,
 *   see Fil12But.cpp). The frequency-to-omega mapping matches the one already
 *   used elsewhere in this codebase (e.g. Play32Butter.c):
 *   r_omega = 2*PI*cutoff/sample_rate, with no bilinear pre-warping. This is
 *   accurate away from Nyquist; see the design spec for the known limitation
 *   near Nyquist at 44.1/48kHz for the 20kHz low-pass band.
 */
void PT_DECLSPEC filtBrickwallDesignHighPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	realtype r_omega = (realtype)MTH_TWO_PI * r_cutoff_hz / r_sample_rate_hz;

	filtDesign2ndButHighPass(r_omega, &(cp_coeffs->gain), &(cp_coeffs->a1), &(cp_coeffs->a0));
}

/*
 * FUNCTION: filtBrickwallDesignLowPass()
 * DESCRIPTION:
 *   Designs one 2nd-order Butterworth low-pass section. See filtBrickwallDesignHighPass().
 */
void PT_DECLSPEC filtBrickwallDesignLowPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs)
{
	realtype r_omega = (realtype)MTH_TWO_PI * r_cutoff_hz / r_sample_rate_hz;

	filtDesign2ndButLowPass(r_omega, &(cp_coeffs->gain), &(cp_coeffs->a1), &(cp_coeffs->a0));
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
 * FUNCTION: filtBrickwallCalcResponseDb()
 * DESCRIPTION:
 *   Evaluates the combined magnitude response (in dB) of the full cascade at
 *   r_freq_hz, for verification/testing. Reuses the existing
 *   filtPolyCalcBiquadPowerResponse() (see Filtpoly.cpp), which expects
 *   numerator/denominator coefficients in "b0*z^2+b1*z+b2 / a0*z^2+a1*z+a2" form
 *   and a normalized frequency (cycles/sample). filtRun2ndLowPass()/HighPass()
 *   implement y[n] = a1*y[n-1] + a0*y[n-2] + gain*(x[n] +/- 2x[n-1] + x[n-2]),
 *   i.e. H(z) = gain*(z^2 +/- 2z + 1) / (z^2 - a1*z - a0) after multiplying
 *   through by z^2 - hence the sign flips on a1/a0 below.
 */
double PT_DECLSPEC filtBrickwallCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs)
{
	realtype r_norm_freq = r_freq_hz / r_sample_rate_hz;
	realtype hp_power, lp_power;
	double total_power;

	hp_power = filtPolyCalcBiquadPowerResponse(
		cp_hp_coeffs->gain, (realtype)-2.0 * cp_hp_coeffs->gain, cp_hp_coeffs->gain,
		(realtype)1.0, -(cp_hp_coeffs->a1), -(cp_hp_coeffs->a0),
		r_norm_freq);

	lp_power = filtPolyCalcBiquadPowerResponse(
		cp_lp_coeffs->gain, (realtype)2.0 * cp_lp_coeffs->gain, cp_lp_coeffs->gain,
		(realtype)1.0, -(cp_lp_coeffs->a1), -(cp_lp_coeffs->a0),
		r_norm_freq);

	total_power = pow((double)hp_power, i_num_sections) * pow((double)lp_power, i_num_sections);
	if (total_power < 1e-18)
	{
		total_power = 1e-18;
	}

	return 10.0 * log10(total_power);
}
