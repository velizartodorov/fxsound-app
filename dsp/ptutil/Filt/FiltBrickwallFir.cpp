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
 * FUNCTION: filtBrickwallFirDesign()
 * DESCRIPTION:
 *   Windowed-sinc linear-phase FIR bandpass design. The ideal (infinite,
 *   brickwall) bandpass impulse response is the difference of two ideal
 *   lowpass impulse responses (one at r_lp_cutoff_hz, one at r_hp_cutoff_hz);
 *   truncating to i_num_taps and applying a Blackman window gives a realizable
 *   filter with ~74dB stopband rejection and a transition width inversely
 *   proportional to i_num_taps (verified numerically: at fs=44100 and the
 *   default 20ms-latency tap count, the low-pass edge at 20kHz is effectively
 *   a true brickwall - rejection exceeds 110dB one semitone above the cutoff -
 *   while the high-pass edge at 20Hz is necessarily much gentler, since a
 *   comparably narrow transition there would need tens of thousands more taps
 *   and proportionally more latency; see the design spec).
 *
 *   i_num_taps is chosen from r_target_latency_ms (one-way group delay of a
 *   linear-phase FIR is exactly (N-1)/(2*fs)) and forced odd, since a Type-I
 *   linear-phase FIR needs an odd tap count for its center tap - and this
 *   design's group delay - to land on an exact integer sample.
 */
void PT_DECLSPEC filtBrickwallFirDesign(realtype r_hp_cutoff_hz, realtype r_lp_cutoff_hz, realtype r_sample_rate_hz,
	realtype r_target_latency_ms, realtype *cp_coeffs_out, int *ip_num_taps_out, int i_max_taps)
{
	double target_latency_sec = (double)r_target_latency_ms / 1000.0;
	int num_taps = (int)(2.0 * target_latency_sec * (double)r_sample_rate_hz) + 1;
	double m_max, center, wc_lp, wc_hp;
	int n;

	if ((num_taps % 2) == 0)
	{
		num_taps += 1;
	}
	if (i_max_taps > 0 && (i_max_taps % 2) == 0)
	{
		i_max_taps -= 1;
	}
	if (num_taps > i_max_taps)
	{
		num_taps = i_max_taps;
	}
	if (num_taps < 1)
	{
		num_taps = 1;
	}

	m_max = (double)(num_taps - 1);
	center = m_max / 2.0;
	wc_lp = MTH_TWO_PI * (double)r_lp_cutoff_hz / (double)r_sample_rate_hz;
	wc_hp = MTH_TWO_PI * (double)r_hp_cutoff_hz / (double)r_sample_rate_hz;

	for (n = 0; n < num_taps; n++)
	{
		double m = (double)n - center;
		double lp = (wc_lp / MTH_PI) * filtBrickwallFirSinc(wc_lp * m / MTH_PI);
		double hp_as_lp = (wc_hp / MTH_PI) * filtBrickwallFirSinc(wc_hp * m / MTH_PI);
		double ideal = lp - hp_as_lp;
		double window;

		if (num_taps > 1)
		{
			window = 0.42 - 0.5 * cos(MTH_TWO_PI * (double)n / m_max) + 0.08 * cos(2.0 * MTH_TWO_PI * (double)n / m_max);
		}
		else
		{
			window = 1.0;
		}

		cp_coeffs_out[n] = (realtype)(ideal * window);
	}

	*ip_num_taps_out = num_taps;
}

/*
 * FUNCTION: filtBrickwallFirGetLatencyMs()
 * DESCRIPTION:
 *   One-way group delay of an i_num_taps-tap linear-phase (symmetric) FIR.
 */
double PT_DECLSPEC filtBrickwallFirGetLatencyMs(int i_num_taps, realtype r_sample_rate_hz)
{
	if (i_num_taps <= 1 || r_sample_rate_hz <= (realtype)0.0)
	{
		return 0.0;
	}

	return ((double)(i_num_taps - 1) / (2.0 * (double)r_sample_rate_hz)) * 1000.0;
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

/*
 * FUNCTION: filtBrickwallFirResetChannelState()
 */
void PT_DECLSPEC filtBrickwallFirResetChannelState(FiltBrickwallFirChannelState *sp_state)
{
	int i;

	for (i = 0; i < 2 * FILT_BRICKWALL_FIR_MAX_TAPS; i++)
	{
		sp_state->delay_line[i] = (realtype)0.0;
	}
	sp_state->write_index = 0;
}

/*
 * FUNCTION: filtBrickwallFirProcessSample()
 * DESCRIPTION:
 *   Direct time-domain convolution using a double-length circular delay line
 *   (standard real-time FIR technique): writing each new sample to both
 *   write_index and write_index+FILT_BRICKWALL_FIR_MAX_TAPS lets the
 *   convolution read a contiguous i_num_taps-length slice with no per-tap
 *   modulo/wraparound check in the inner loop. write_index counts DOWN (wraps
 *   via +FILT_BRICKWALL_FIR_MAX_TAPS) specifically so that after the write,
 *   tap_start[k] == x[n-k] for k = 0..i_num_taps-1, matching the convolution
 *   sum y[n] = sum_k h[k]*x[n-k] directly with no index inversion.
 */
realtype PT_DECLSPEC filtBrickwallFirProcessSample(realtype r_input, const realtype *cp_coeffs, int i_num_taps,
	FiltBrickwallFirChannelState *sp_state, realtype *sp_delayed_dry_out)
{
	realtype *tap_start;
	realtype sum;
	int k;

	sp_state->write_index = (sp_state->write_index - 1 + FILT_BRICKWALL_FIR_MAX_TAPS) % FILT_BRICKWALL_FIR_MAX_TAPS;
	sp_state->delay_line[sp_state->write_index] = r_input;
	sp_state->delay_line[sp_state->write_index + FILT_BRICKWALL_FIR_MAX_TAPS] = r_input;

	tap_start = &(sp_state->delay_line[sp_state->write_index]);

	sum = (realtype)0.0;
	for (k = 0; k < i_num_taps; k++)
	{
		sum += cp_coeffs[k] * tap_start[k];
	}

	if (sp_delayed_dry_out != NULL)
	{
		*sp_delayed_dry_out = tap_start[i_num_taps / 2];
	}

	return sum;
}
