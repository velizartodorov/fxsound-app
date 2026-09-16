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
#ifndef _FILT_BRICKWALL_FIR_H_
#define _FILT_BRICKWALL_FIR_H_

#include "codedefs.h"

/* Stopband attenuation, in dB, that filtBrickwallFirDesignKaiser() designs for. */
#define FILT_BRICKWALL_FIR_KAISER_ATTEN_DB  100.0

/*
 * Tap count for a linear-phase FIR whose total latency, including an FFT
 * convolver's i_block_size-sample partition delay, is r_latency_ms:
 * N = 2 * (round(latency_ms * fs / 1000) - block_size) + 1. Always odd (Type-I
 * linear phase), at least 1.
 */
int PT_DECLSPEC filtBrickwallFirNumTaps(realtype r_latency_ms, realtype r_sample_rate_hz, int i_block_size);

/*
 * Windowed-sinc linear-phase high-pass at r_hp_cutoff_hz with a Kaiser window
 * for FILT_BRICKWALL_FIR_KAISER_ATTEN_DB of stopband attenuation.
 * r_hp_cutoff_hz = 0 gives a pure delay (no filtering). i_num_taps must be odd
 * (see filtBrickwallFirNumTaps()). Writes i_num_taps coefficients.
 * Control/designer thread only.
 */
void PT_DECLSPEC filtBrickwallFirDesignKaiser(realtype r_hp_cutoff_hz, realtype r_sample_rate_hz,
	int i_num_taps, realtype *rp_coeffs_out);

/*
 * Evaluates the magnitude response (in dB) of the designed FIR at r_freq_hz, by
 * direct DTFT summation over its coefficients. For verification/testing only -
 * O(i_num_taps) per call, never call this from the real-time audio path.
 */
double PT_DECLSPEC filtBrickwallFirCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, const realtype *cp_coeffs, int i_num_taps);

#endif
