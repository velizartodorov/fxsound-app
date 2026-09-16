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
#ifndef _FILT_BRICKWALL_H_
#define _FILT_BRICKWALL_H_

#include "codedefs.h"

/* Maximum audio channels and cascaded 2nd-order sections per band this module supports. */
#define FILT_BRICKWALL_MAX_CHANNELS  8
#define FILT_BRICKWALL_MAX_SECTIONS  8

/* Coefficients for one 2nd-order Butterworth section, as produced by
 * filtDesign2ndButLowPass()/filtDesign2ndButHighPass() (see Fil12But.cpp). */
struct FiltBrickwallBiquadCoeffs {
	realtype gain;
	realtype a1;
	realtype a0;
};

/* Running state (sample history) for one 2nd-order section. */
struct FiltBrickwallBiquadState {
	realtype in_minus1;
	realtype in_minus2;
	realtype out_minus1;
	realtype out_minus2;
};

/* Per-channel state for a full high-pass + low-pass cascade. All sections in a
 * band share the same FiltBrickwallBiquadCoeffs (identical stages) - only their
 * history differs, so only the state (not the coefficients) is arrayed per section. */
struct FiltBrickwallChannelState {
	FiltBrickwallBiquadState hp_sections[FILT_BRICKWALL_MAX_SECTIONS];
	FiltBrickwallBiquadState lp_sections[FILT_BRICKWALL_MAX_SECTIONS];
};

void PT_DECLSPEC filtBrickwallDesignHighPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs);
void PT_DECLSPEC filtBrickwallDesignLowPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs);
void PT_DECLSPEC filtBrickwallResetChannelState(FiltBrickwallChannelState *sp_state);
realtype PT_DECLSPEC filtBrickwallProcessSample(realtype r_input, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs,
	FiltBrickwallChannelState *sp_state);
double PT_DECLSPEC filtBrickwallCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, int i_num_sections,
	const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs);

#endif
