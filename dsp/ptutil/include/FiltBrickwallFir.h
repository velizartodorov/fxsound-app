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

/* Maximum audio channels and FIR taps this module supports. Must stay odd
 * (a Type-I linear-phase FIR needs an odd tap count for an exact-integer-sample
 * center/group-delay) - callers that clamp to this bound must preserve that. */
#define FILT_BRICKWALL_FIR_MAX_CHANNELS  8
#define FILT_BRICKWALL_FIR_MAX_TAPS      24001

/* Per-channel state: a double-length delay line (see FiltBrickwallFir.cpp for
 * why) plus a write cursor. Real-time safe to read/write once allocated - no
 * allocation happens after construction. */
struct FiltBrickwallFirChannelState {
	realtype delay_line[2 * FILT_BRICKWALL_FIR_MAX_TAPS];
	int write_index;
};

/*
 * Designs a linear-phase FIR bandpass (high-pass at r_hp_cutoff_hz combined
 * with low-pass at r_lp_cutoff_hz) via windowed-sinc design (Blackman window),
 * targeting r_target_latency_ms of one-way group delay. The actual tap count
 * (and therefore actual latency, always <= the target) is written to
 * *ip_num_taps_out; call filtBrickwallFirGetLatencyMs() with that value to get
 * the actual latency. cp_coeffs_out must have room for i_max_taps entries
 * (FILT_BRICKWALL_FIR_MAX_TAPS is the module's own bound; pass that constant
 * unless a caller has a smaller buffer). Control-thread only.
 */
void PT_DECLSPEC filtBrickwallFirDesign(realtype r_hp_cutoff_hz, realtype r_lp_cutoff_hz, realtype r_sample_rate_hz,
	realtype r_target_latency_ms, realtype *cp_coeffs_out, int *ip_num_taps_out, int i_max_taps);

/* One-way group delay, in milliseconds, of an i_num_taps-tap linear-phase FIR at r_sample_rate_hz. */
double PT_DECLSPEC filtBrickwallFirGetLatencyMs(int i_num_taps, realtype r_sample_rate_hz);

/*
 * Evaluates the magnitude response (in dB) of the designed FIR at r_freq_hz, by
 * direct DTFT summation over its coefficients. For verification/testing only -
 * O(i_num_taps) per call, never call this from the real-time audio path.
 */
double PT_DECLSPEC filtBrickwallFirCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, const realtype *cp_coeffs, int i_num_taps);

/* Clears one channel's delay-line history. Must be called before first use and
 * whenever the coefficients (tap count or sample rate) change. */
void PT_DECLSPEC filtBrickwallFirResetChannelState(FiltBrickwallFirChannelState *sp_state);

/*
 * Convolves one sample through the FIR. Real-time safe: fixed-size array
 * indexing only, no allocation, no locking. If sp_delayed_dry_out is non-NULL,
 * it receives the input sample delayed by the filter's own group delay
 * (extracted from the same delay line, not a separate buffer) - needed to
 * compute a phase-correct "what did the filter remove" residual, since unlike
 * the near-zero-latency IIR cascade, this filter's output is not time-aligned
 * with its input.
 */
realtype PT_DECLSPEC filtBrickwallFirProcessSample(realtype r_input, const realtype *cp_coeffs, int i_num_taps,
	FiltBrickwallFirChannelState *sp_state, realtype *sp_delayed_dry_out);

#endif
