# Brickwall Filter (20Hz - 20kHz) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a user-toggleable bandwidth-limiting filter (20Hz high-pass, 20kHz low-pass) to FxSound's real-time audio path, controllable from Settings > Audio with three steepness presets and an A/B "hear what's removed" preview.

**Architecture:** A new low-level cascaded-biquad filter module in `dsp/ptutil/`, wired into the existing `DfxDsp` pimpl engine as the last processing step in `processAudio` (after the existing `dfxpUniversalModifySamples` call), exposed through `FxController`, and controlled from a new control group in `FxSettingsDialog`'s `AudioSettingsPane`.

**Tech Stack:** C++ (Visual Studio 2022, Windows SDK), JUCE 6.1.6, this repo's existing legacy DSP primitives (`dsp/ptutil/Filt/`).

**Spec:** `docs/superpowers/specs/2026-09-16-brickwall-filter-design.md`

## Global Constraints

- No allocation, locking, or blocking calls inside `DfxDspPrivate::processAudio` or anything it calls (real-time audio thread) — CLAUDE.md requirement for `dsp/`.
- Reuse existing DSP primitives (`filtDesign2ndButLowPass`/`HighPass`, `filtRun2ndLowPass`/`HighPass`, `filtPolyCalcBiquadPowerResponse`) rather than writing new low-level filter math from scratch.
- Cascade of identical fixed-Q (~0.707) Butterworth sections, NOT a per-stage-Q true higher-order Butterworth, and NOT a linear-phase FIR (see spec's "Filter type" section for why).
- Filter defaults to off; steepness defaults to Standard (4 sections/band, ~48 dB/octave) when turned on; preview is session-only and never persisted.
- All user-facing strings use the existing `TRANS(...)` localization macro.
- Changes under `dsp/` are higher-risk per CLAUDE.md and must be flagged explicitly for human review before merge (Task 5 covers this).

---

### Task 1: Cascaded-biquad filter math module

**Files:**
- Create: `dsp/ptutil/include/FiltBrickwall.h`
- Create: `dsp/ptutil/Filt/FiltBrickwall.cpp`
- Modify: `dsp/DfxDsp.vcxproj`
- Modify: `dsp/DfxDsp.vcxproj.filters`

**Interfaces:**
- Produces (used by Task 2):
  - `struct FiltBrickwallBiquadCoeffs { realtype gain; realtype a1; realtype a0; };`
  - `struct FiltBrickwallBiquadState { realtype in_minus1; realtype in_minus2; realtype out_minus1; realtype out_minus2; };`
  - `struct FiltBrickwallChannelState { FiltBrickwallBiquadState hp_sections[FILT_BRICKWALL_MAX_SECTIONS]; FiltBrickwallBiquadState lp_sections[FILT_BRICKWALL_MAX_SECTIONS]; };`
  - `#define FILT_BRICKWALL_MAX_CHANNELS 8`
  - `#define FILT_BRICKWALL_MAX_SECTIONS 8`
  - `void filtBrickwallDesignHighPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs);`
  - `void filtBrickwallDesignLowPass(realtype r_cutoff_hz, realtype r_sample_rate_hz, FiltBrickwallBiquadCoeffs *cp_coeffs);`
  - `void filtBrickwallResetChannelState(FiltBrickwallChannelState *sp_state);`
  - `realtype filtBrickwallProcessSample(realtype r_input, int i_num_sections, const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs, FiltBrickwallChannelState *sp_state);`
  - `double filtBrickwallCalcResponseDb(realtype r_freq_hz, realtype r_sample_rate_hz, int i_num_sections, const FiltBrickwallBiquadCoeffs *cp_hp_coeffs, const FiltBrickwallBiquadCoeffs *cp_lp_coeffs);`

This module is deliberately independent of `DfxDsp` — it knows nothing about steepness presets or the app's enums. Task 2 maps its own `DfxDsp::BrickwallSteepness` to a plain `int i_num_sections` before calling into this module.

- [ ] **Step 1: Create the header**

Create `dsp/ptutil/include/FiltBrickwall.h`:

```cpp
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
```

- [ ] **Step 2: Create the implementation**

Create `dsp/ptutil/Filt/FiltBrickwall.cpp`:

```cpp
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
```

- [ ] **Step 3: Register the new files in the DfxDsp project**

In `dsp/DfxDsp.vcxproj`, find this line:

```xml
    <ClInclude Include="ptutil\include\filt.h" />
```

and add immediately after it:

```xml
    <ClInclude Include="ptutil\include\filt.h" />
    <ClInclude Include="ptutil\include\FiltBrickwall.h" />
```

In the same file, find:

```xml
    <ClCompile Include="ptutil\Filt\Fil12But.cpp" />
```

and add immediately after it:

```xml
    <ClCompile Include="ptutil\Filt\Fil12But.cpp" />
    <ClCompile Include="ptutil\Filt\FiltBrickwall.cpp" />
```

In `dsp/DfxDsp.vcxproj.filters`, find:

```xml
    <ClInclude Include="ptutil\include\filt.h">
      <Filter>Header Files\ptutil</Filter>
    </ClInclude>
```

and add immediately after it:

```xml
    <ClInclude Include="ptutil\include\FiltBrickwall.h">
      <Filter>Header Files\ptutil</Filter>
    </ClInclude>
```

In the same file, find:

```xml
    <ClCompile Include="ptutil\Filt\Fil12But.cpp">
      <Filter>Source Files\ptutil\Filt</Filter>
    </ClCompile>
```

and add immediately after it:

```xml
    <ClCompile Include="ptutil\Filt\FiltBrickwall.cpp">
      <Filter>Source Files\ptutil\Filt</Filter>
    </ClCompile>
```

- [ ] **Step 4: Build to verify**

This codebase has no automated unit test framework under `dsp/` (verified: no test project references either new file). The verification for this pure-library task is a clean build; runtime correctness is checked in Task 2, where this module is actually exercised.

Run: `msbuild dsp\DfxDsp.vcxproj /p:Configuration=Debug /p:Platform=x64`
Expected: `Build succeeded. 0 Warning(s) 0 Error(s)` (or build it via Visual Studio: open `fxsound/Project/FxSound.sln`, right-click the `DfxDsp` project, Build).

- [ ] **Step 5: Commit**

```bash
git add dsp/ptutil/include/FiltBrickwall.h dsp/ptutil/Filt/FiltBrickwall.cpp dsp/DfxDsp.vcxproj dsp/DfxDsp.vcxproj.filters
git commit -m "feat(dsp): add cascaded-biquad brickwall filter math module"
```

---

### Task 2: DfxDsp engine integration

**Files:**
- Modify: `dsp/include/DfxDsp.h`
- Modify: `dsp/DfxDsp.cpp`
- Modify: `dsp/u_DfxDsp.h`
- Modify: `dsp/DfxDspPrivate.cpp`

**Interfaces:**
- Consumes (from Task 1): `FiltBrickwallBiquadCoeffs`, `FiltBrickwallChannelState`, `FILT_BRICKWALL_MAX_CHANNELS`, `filtBrickwallDesignHighPass`, `filtBrickwallDesignLowPass`, `filtBrickwallResetChannelState`, `filtBrickwallProcessSample`, `filtBrickwallCalcResponseDb`.
- Produces (used by Task 3):
  - `enum DfxDsp::BrickwallSteepness { Gentle = 0, Standard = 1, Steep = 2 };`
  - `void DfxDsp::brickwallFilterOn(bool on);`
  - `bool DfxDsp::isBrickwallFilterOn();`
  - `void DfxDsp::setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness);`
  - `DfxDsp::BrickwallSteepness DfxDsp::getBrickwallFilterSteepness();`
  - `void DfxDsp::brickwallFilterPreviewOn(bool on);`
  - `bool DfxDsp::isBrickwallFilterPreviewOn();`

This is one task (not split further) because `DfxDsp.h`/`DfxDsp.cpp` (the public pimpl wrapper) and `u_DfxDsp.h`/`DfxDspPrivate.cpp` (the implementation) only compile together — there is no independently buildable midpoint between declaring these methods and implementing them.

- [ ] **Step 1: Add the public API**

In `dsp/include/DfxDsp.h`, find:

```cpp
	enum Effect { Fidelity = 0, Ambience = 1, Surround = 2, DynamicBoost = 3, Bass = 4, NumEffects = 5 };
```

and change it to:

```cpp
	enum Effect { Fidelity = 0, Ambience = 1, Surround = 2, DynamicBoost = 3, Bass = 4, NumEffects = 5 };
	enum BrickwallSteepness { Gentle = 0, Standard = 1, Steep = 2 };
```

In the same file, find:

```cpp
	void eqOn(bool on);
	int getNumEqBands();
```

and change it to:

```cpp
	void eqOn(bool on);
	int getNumEqBands();
	void brickwallFilterOn(bool on);
	bool isBrickwallFilterOn();
	void setBrickwallFilterSteepness(BrickwallSteepness steepness);
	BrickwallSteepness getBrickwallFilterSteepness();
	void brickwallFilterPreviewOn(bool on);
	bool isBrickwallFilterPreviewOn();
```

- [ ] **Step 2: Forward the public API to the pimpl**

In `dsp/DfxDsp.cpp`, find:

```cpp
int DfxDsp::getNumEqBands()
{
	return data_->getNumEqBands();
}

DfxPreset DfxDsp::getPresetInfo(std::wstring preset_file_full_path)
```

and change it to:

```cpp
int DfxDsp::getNumEqBands()
{
	return data_->getNumEqBands();
}

void DfxDsp::brickwallFilterOn(bool on)
{
	data_->brickwallFilterOn(on);
}

bool DfxDsp::isBrickwallFilterOn()
{
	return data_->isBrickwallFilterOn();
}

void DfxDsp::setBrickwallFilterSteepness(BrickwallSteepness steepness)
{
	data_->setBrickwallFilterSteepness(steepness);
}

DfxDsp::BrickwallSteepness DfxDsp::getBrickwallFilterSteepness()
{
	return data_->getBrickwallFilterSteepness();
}

void DfxDsp::brickwallFilterPreviewOn(bool on)
{
	data_->brickwallFilterPreviewOn(on);
}

bool DfxDsp::isBrickwallFilterPreviewOn()
{
	return data_->isBrickwallFilterPreviewOn();
}

DfxPreset DfxDsp::getPresetInfo(std::wstring preset_file_full_path)
```

- [ ] **Step 3: Add state and method declarations to the pimpl class**

In `dsp/u_DfxDsp.h`, find:

```cpp
#include <string>
#include "AudioPassthru.h"
#include "codedefs.h"
#include "DfxDsp.h"
#include "pt_defs.h"
#include "slout.h"
```

and change it to:

```cpp
#include <string>
#include "AudioPassthru.h"
#include "codedefs.h"
#include "DfxDsp.h"
#include "pt_defs.h"
#include "slout.h"
#include "FiltBrickwall.h"
```

In the same file, find:

```cpp
	void eqOn(bool on);
	int getNumEqBands();
```

and change it to:

```cpp
	void eqOn(bool on);
	int getNumEqBands();
	void brickwallFilterOn(bool on);
	bool isBrickwallFilterOn();
	void setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness);
	DfxDsp::BrickwallSteepness getBrickwallFilterSteepness();
	void brickwallFilterPreviewOn(bool on);
	bool isBrickwallFilterPreviewOn();
```

In the same file, find:

```cpp
	// DfxDspEq.cpp
	int eqSetProcessingOn(int i_storage_type, int i_on);
	int eqGetProcessingOn(int i_storage_type, int *ip_on);
```

and change it to:

```cpp
	// DfxDspEq.cpp
	int eqSetProcessingOn(int i_storage_type, int i_on);
	int eqGetProcessingOn(int i_storage_type, int *ip_on);

	// Brickwall filter (dsp/ptutil/include/FiltBrickwall.h)
	void updateBrickwallFilterCoefficients();
	void resetBrickwallFilterState();
	void applyBrickwallFilter(float *audio_buffer, int num_sample_sets);
```

In the same file, find:

```cpp
	int eq_processing_on_;
};
```

and change it to:

```cpp
	int eq_processing_on_;

	bool brickwall_filter_on_ = false;
	bool brickwall_filter_preview_on_ = false;
	DfxDsp::BrickwallSteepness brickwall_filter_steepness_ = DfxDsp::BrickwallSteepness::Standard;
	int brickwall_cached_sample_rate_ = 44100;
	int brickwall_cached_num_channels_ = 2;
	FiltBrickwallBiquadCoeffs brickwall_hp_coeffs_;
	FiltBrickwallBiquadCoeffs brickwall_lp_coeffs_;
	FiltBrickwallChannelState brickwall_channel_states_[FILT_BRICKWALL_MAX_CHANNELS];
};
```

- [ ] **Step 4: Implement the filter integration in DfxDspPrivate.cpp**

In `dsp/DfxDspPrivate.cpp`, find:

```cpp
#include "u_DfxDsp.h"
#include "codedefs.h"
#include "DfxSdk.h"
#include "dfxp.h"
#include "prelst.h"
#include "file.h"
#include "qnt.h"
#include <string>
```

and change it to:

```cpp
#include "u_DfxDsp.h"
#include "codedefs.h"
#include "DfxSdk.h"
#include "dfxp.h"
#include "prelst.h"
#include "file.h"
#include "qnt.h"
#include <string>
#include <math.h>
#include <cassert>
```

In the same file, find:

```cpp
#define MIDI_MIN_VALUE          0
#define MIDI_MAX_VALUE          127
```

and change it to:

```cpp
#define MIDI_MIN_VALUE          0
#define MIDI_MAX_VALUE          127

#define DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ  20.0
#define DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ   20000.0

/*
 * FUNCTION: brickwallNumSectionsForSteepness()
 * DESCRIPTION:
 *   Maps the app-level steepness preset to a cascaded section count. See the
 *   design spec for why these are cascades of identical fixed-Q sections
 *   rather than a true per-stage-Q higher-order Butterworth.
 */
static int brickwallNumSectionsForSteepness(DfxDsp::BrickwallSteepness steepness)
{
	switch (steepness)
	{
	case DfxDsp::BrickwallSteepness::Gentle:
		return 1;
	case DfxDsp::BrickwallSteepness::Steep:
		return 8;
	case DfxDsp::BrickwallSteepness::Standard:
	default:
		return 4;
	}
}

/*
 * FUNCTION: debugVerifyBrickwallFilterResponse()
 * DESCRIPTION:
 *   Debug-only runtime self-check of the brickwall filter's frequency response,
 *   in place of a unit test (this codebase has no test framework under dsp/).
 *   Asserts (fails loudly in Debug builds) that:
 *     - the passband (1kHz) is essentially untouched by the filter,
 *     - content an octave below the 20Hz cutoff is measurably attenuated,
 *     - the response rolls off approaching Nyquist (rather than asserting an
 *       exact dB value there, since the exact -3dB point can shift near
 *       Nyquist at 44.1/48kHz - see the design spec's Nyquist-proximity note).
 */
static void debugVerifyBrickwallFilterResponse()
{
	const double sample_rates[] = { 44100.0, 48000.0, 96000.0 };
	const DfxDsp::BrickwallSteepness steepness_values[] = {
		DfxDsp::BrickwallSteepness::Gentle,
		DfxDsp::BrickwallSteepness::Standard,
		DfxDsp::BrickwallSteepness::Steep
	};
	size_t rate_index, steepness_index;

	for (rate_index = 0; rate_index < sizeof(sample_rates) / sizeof(sample_rates[0]); rate_index++)
	{
		double sample_rate = sample_rates[rate_index];
		FiltBrickwallBiquadCoeffs hp_coeffs;
		FiltBrickwallBiquadCoeffs lp_coeffs;

		filtBrickwallDesignHighPass((realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, (realtype)sample_rate, &hp_coeffs);
		filtBrickwallDesignLowPass((realtype)DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ, (realtype)sample_rate, &lp_coeffs);

		for (steepness_index = 0; steepness_index < sizeof(steepness_values) / sizeof(steepness_values[0]); steepness_index++)
		{
			int num_sections = brickwallNumSectionsForSteepness(steepness_values[steepness_index]);

			double passband_db = filtBrickwallCalcResponseDb((realtype)1000.0, (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);
			double sub_audible_db = filtBrickwallCalcResponseDb((realtype)10.0, (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);
			double near_nyquist_db = filtBrickwallCalcResponseDb((realtype)(sample_rate * 0.49), (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);

			assert(passband_db > -0.5);
			assert(sub_audible_db < -6.0);
			assert(near_nyquist_db < passband_db);
		}
	}
}
```

In the same file, find:

```cpp
	//return(OKAY);
}


DfxDspPrivate::~DfxDspPrivate()
```

and change it to:

```cpp
	brickwall_cached_sample_rate_ = 44100;
	brickwall_cached_num_channels_ = 2;
	updateBrickwallFilterCoefficients();
	resetBrickwallFilterState();

#ifdef _DEBUG
	debugVerifyBrickwallFilterResponse();
#endif

	//return(OKAY);
}


DfxDspPrivate::~DfxDspPrivate()
```

In the same file, find:

```cpp
int DfxDspPrivate::processAudio(short int *si_input_samples, short int *si_output_samples, int i_num_sample_sets, int i_check_for_duplicate_buffers)
{
	processTimer();
	// Apply DFX processing here using data and format vars above. Format will always be 32 bit floating point.
	if (dfxpUniversalModifySamples(dfxp_handle_, si_input_samples, si_output_samples, i_num_sample_sets, i_check_for_duplicate_buffers) != OKAY)
		return(NOT_OKAY);

	return OKAY;
}

int DfxDspPrivate::setSignalFormat(int i_bps, int i_nch, int i_srate, int i_valid_bits)
{
	if (dfxpUniversalSetSignalFormat(dfxp_handle_, i_bps, i_nch, i_srate, i_valid_bits) != OKAY)
		return(NOT_OKAY);

	return OKAY;
}
```

and change it to:

```cpp
int DfxDspPrivate::processAudio(short int *si_input_samples, short int *si_output_samples, int i_num_sample_sets, int i_check_for_duplicate_buffers)
{
	processTimer();
	// Apply DFX processing here using data and format vars above. Format will always be 32 bit floating point.
	if (dfxpUniversalModifySamples(dfxp_handle_, si_input_samples, si_output_samples, i_num_sample_sets, i_check_for_duplicate_buffers) != OKAY)
		return(NOT_OKAY);

	applyBrickwallFilter(reinterpret_cast<float*>(si_output_samples), i_num_sample_sets);

	return OKAY;
}

int DfxDspPrivate::setSignalFormat(int i_bps, int i_nch, int i_srate, int i_valid_bits)
{
	if (dfxpUniversalSetSignalFormat(dfxp_handle_, i_bps, i_nch, i_srate, i_valid_bits) != OKAY)
		return(NOT_OKAY);

	if (i_srate != brickwall_cached_sample_rate_ || i_nch != brickwall_cached_num_channels_)
	{
		brickwall_cached_sample_rate_ = i_srate;
		brickwall_cached_num_channels_ = i_nch;
		updateBrickwallFilterCoefficients();
		resetBrickwallFilterState();
	}

	return OKAY;
}

void DfxDspPrivate::updateBrickwallFilterCoefficients()
{
	filtBrickwallDesignHighPass((realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, (realtype)brickwall_cached_sample_rate_, &brickwall_hp_coeffs_);
	filtBrickwallDesignLowPass((realtype)DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ, (realtype)brickwall_cached_sample_rate_, &brickwall_lp_coeffs_);
}

void DfxDspPrivate::resetBrickwallFilterState()
{
	int channel;

	for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
	{
		filtBrickwallResetChannelState(&brickwall_channel_states_[channel]);
	}
}

void DfxDspPrivate::applyBrickwallFilter(float *audio_buffer, int num_sample_sets)
{
	int num_sections;
	int sample_index;
	int channel;

	if (!brickwall_filter_on_)
	{
		return;
	}

	num_sections = brickwallNumSectionsForSteepness(brickwall_filter_steepness_);

	for (sample_index = 0; sample_index < num_sample_sets; sample_index++)
	{
		for (channel = 0; channel < brickwall_cached_num_channels_ && channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
		{
			int buffer_index = sample_index * brickwall_cached_num_channels_ + channel;
			realtype pre_filter_sample = (realtype)audio_buffer[buffer_index];

			realtype filtered_sample = filtBrickwallProcessSample(
				pre_filter_sample,
				num_sections,
				&brickwall_hp_coeffs_,
				&brickwall_lp_coeffs_,
				&brickwall_channel_states_[channel]);

			if (brickwall_filter_preview_on_)
			{
				audio_buffer[buffer_index] = (float)(pre_filter_sample - filtered_sample);
			}
			else
			{
				audio_buffer[buffer_index] = (float)filtered_sample;
			}
		}
	}
}

void DfxDspPrivate::brickwallFilterOn(bool on)
{
	brickwall_filter_on_ = on;
	if (on)
	{
		updateBrickwallFilterCoefficients();
		resetBrickwallFilterState();
	}
	else
	{
		brickwall_filter_preview_on_ = false;
	}
}

bool DfxDspPrivate::isBrickwallFilterOn()
{
	return brickwall_filter_on_;
}

void DfxDspPrivate::setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness)
{
	brickwall_filter_steepness_ = steepness;
	resetBrickwallFilterState();
}

DfxDsp::BrickwallSteepness DfxDspPrivate::getBrickwallFilterSteepness()
{
	return brickwall_filter_steepness_;
}

void DfxDspPrivate::brickwallFilterPreviewOn(bool on)
{
	brickwall_filter_preview_on_ = on && brickwall_filter_on_;
}

bool DfxDspPrivate::isBrickwallFilterPreviewOn()
{
	return brickwall_filter_preview_on_;
}
```

- [ ] **Step 5: Build and run to verify**

Run: `msbuild fxsound\Project\FxSound.sln /p:Configuration=Debug /p:Platform=x64` (or build/run `FxSound_App` from Visual Studio with F5 — requires the FxSound virtual audio driver already installed per CLAUDE.md).
Expected: build succeeds, and the app starts without any assertion dialog/crash from `debugVerifyBrickwallFilterResponse()`. If an assertion fires, it means either the frequency-to-omega mapping or the `filtPolyCalcBiquadPowerResponse` coefficient conversion in Task 1 has a sign/convention error — re-derive from the transfer function comment in `filtBrickwallCalcResponseDb()` before adjusting thresholds.

- [ ] **Step 6: Commit**

```bash
git add dsp/include/DfxDsp.h dsp/DfxDsp.cpp dsp/u_DfxDsp.h dsp/DfxDspPrivate.cpp
git commit -m "feat(dsp): wire brickwall filter into DfxDsp engine with debug self-check"
```

---

### Task 3: FxController wiring and persistence

**Files:**
- Modify: `fxsound/Source/GUI/FxController.h`
- Modify: `fxsound/Source/GUI/FxController.cpp`

**Interfaces:**
- Consumes (from Task 2): `DfxDsp::BrickwallSteepness`, `dfx_dsp_.brickwallFilterOn(bool)`, `dfx_dsp_.isBrickwallFilterOn()`, `dfx_dsp_.setBrickwallFilterSteepness(...)`, `dfx_dsp_.getBrickwallFilterSteepness()`, `dfx_dsp_.brickwallFilterPreviewOn(bool)`, `dfx_dsp_.isBrickwallFilterPreviewOn()`.
- Produces (used by Task 4):
  - `bool FxController::isBrickwallFilterOn();`
  - `void FxController::setBrickwallFilterOn(bool on);`
  - `DfxDsp::BrickwallSteepness FxController::getBrickwallFilterSteepness();`
  - `void FxController::setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness);`
  - `bool FxController::isBrickwallFilterPreviewOn();`
  - `void FxController::setBrickwallFilterPreviewOn(bool on);`
- Persists: `brickwall_filter_on` (bool, default false), `brickwall_filter_steepness` (int, default `DfxDsp::BrickwallSteepness::Standard`) via the existing `Settings` class. Preview is never persisted.

- [ ] **Step 1: Declare the new methods**

In `fxsound/Source/GUI/FxController.h`, find:

```cpp
	bool isAlwaysOnTop();
	void setAlwaysOnTop(bool always_on_top);
```

and change it to:

```cpp
	bool isAlwaysOnTop();
	void setAlwaysOnTop(bool always_on_top);

	bool isBrickwallFilterOn();
	void setBrickwallFilterOn(bool on);
	DfxDsp::BrickwallSteepness getBrickwallFilterSteepness();
	void setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness);
	bool isBrickwallFilterPreviewOn();
	void setBrickwallFilterPreviewOn(bool on);
```

- [ ] **Step 2: Implement the methods, following the `setAlwaysOnTop`/`isAlwaysOnTop` pattern**

In `fxsound/Source/GUI/FxController.cpp`, find:

```cpp
bool FxController::isAlwaysOnTop()
{
	return always_on_top_;
}

void FxController::setAlwaysOnTop(bool always_on_top)
{
	always_on_top_ = always_on_top;
	settings_.setBool("always_on_top", always_on_top);
	main_window_->setAlwaysOnTop(always_on_top);
}
```

and change it to:

```cpp
bool FxController::isAlwaysOnTop()
{
	return always_on_top_;
}

void FxController::setAlwaysOnTop(bool always_on_top)
{
	always_on_top_ = always_on_top;
	settings_.setBool("always_on_top", always_on_top);
	main_window_->setAlwaysOnTop(always_on_top);
}

bool FxController::isBrickwallFilterOn()
{
	return dfx_dsp_.isBrickwallFilterOn();
}

void FxController::setBrickwallFilterOn(bool on)
{
	dfx_dsp_.brickwallFilterOn(on);
	settings_.setBool("brickwall_filter_on", on);

	if (!on)
	{
		dfx_dsp_.brickwallFilterPreviewOn(false);
	}
}

DfxDsp::BrickwallSteepness FxController::getBrickwallFilterSteepness()
{
	return dfx_dsp_.getBrickwallFilterSteepness();
}

void FxController::setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness)
{
	dfx_dsp_.setBrickwallFilterSteepness(steepness);
	settings_.setInt("brickwall_filter_steepness", static_cast<int>(steepness));
}

bool FxController::isBrickwallFilterPreviewOn()
{
	return dfx_dsp_.isBrickwallFilterPreviewOn();
}

void FxController::setBrickwallFilterPreviewOn(bool on)
{
	dfx_dsp_.brickwallFilterPreviewOn(on);
}
```

- [ ] **Step 3: Load persisted values at startup**

In `fxsound/Source/GUI/FxController.cpp`, inside `FxController::init(...)`, find:

```cpp
		setPowerState(settings_.getBool("power"));

		initPresets();
```

and change it to:

```cpp
		setBrickwallFilterSteepness(static_cast<DfxDsp::BrickwallSteepness>(settings_.getInt("brickwall_filter_steepness", static_cast<int>(DfxDsp::BrickwallSteepness::Standard))));
		setBrickwallFilterOn(settings_.getBool("brickwall_filter_on"));

		setPowerState(settings_.getBool("power"));

		initPresets();
```

- [ ] **Step 4: Force preview off when FxSound is powered off**

In `fxsound/Source/GUI/FxController.cpp`, inside `FxController::powerOn(bool on)`, find:

```cpp
	else
	{
		dfx_dsp_.powerOn(false);

		if (isTimerRunning())
		{
			stopTimer();
		}

		audio_passthru_->restoreDefaultPlaybackDevice();
	}
```

and change it to:

```cpp
	else
	{
		dfx_dsp_.powerOn(false);
		dfx_dsp_.brickwallFilterPreviewOn(false);

		if (isTimerRunning())
		{
			stopTimer();
		}

		audio_passthru_->restoreDefaultPlaybackDevice();
	}
```

- [ ] **Step 5: Build to verify**

Run: `msbuild fxsound\Project\FxSound.sln /p:Configuration=Debug /p:Platform=x64`
Expected: `Build succeeded. 0 Error(s)`.

- [ ] **Step 6: Commit**

```bash
git add fxsound/Source/GUI/FxController.h fxsound/Source/GUI/FxController.cpp
git commit -m "feat(gui): add FxController API and persistence for brickwall filter"
```

---

### Task 4: Settings dialog UI

**Files:**
- Modify: `fxsound/Source/GUI/FxSettingsDialog.h`
- Modify: `fxsound/Source/GUI/FxSettingsDialog.cpp`

**Interfaces:**
- Consumes (from Task 3): `FxController::isBrickwallFilterOn()`, `setBrickwallFilterOn(bool)`, `getBrickwallFilterSteepness()`, `setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness)`, `isBrickwallFilterPreviewOn()`, `setBrickwallFilterPreviewOn(bool)`.
- No new interfaces produced — this is the top of the stack.

- [ ] **Step 1: Grow the settings window and add new members**

In `fxsound/Source/GUI/FxSettingsDialog.h`, find:

```cpp
	class SettingsComponent : public Component, public Button::Listener
	{
	public:
        static constexpr int WIDTH = 600;
        static constexpr int HEIGHT = 510;
```

and change it to:

```cpp
	class SettingsComponent : public Component, public Button::Listener
	{
	public:
        static constexpr int WIDTH = 600;
        static constexpr int HEIGHT = 660;
```

In the same file, find:

```cpp
	class AudioSettingsPane : public SettingsPane
	{
	public:
		AudioSettingsPane();
		~AudioSettingsPane();

		void resized() override;
		void paint(Graphics& g) override;

	private:
		static constexpr int GROUP_MARGIN = 10;
		static constexpr int ENDPOINT_Y = 50;
		static constexpr int LABEL_WIDTH = 220;
		static constexpr int OUTPUT_PREFERENCE_HEIGHT = 260;
		static constexpr int LABEL_HEIGHT = 14;
		static constexpr int TOGGLE_BUTTON_HEIGHT = 30;
		static constexpr int RESET_PRESETS_BUTTON_WIDTH = 220;
		static constexpr int BUTTON_HEIGHT = 24;
		static constexpr int MAX_BUTTON_WIDTH = 315;

		void setText();
		void resizeResetButton(int x, int y);

		void visibilityChanged() override;
		void mouseEnter(const MouseEvent& mouse_event) override;
		void mouseExit(const MouseEvent& mouse_event) override;

		Label output_preference_title_;
		FxOutputPreference output_preference_;
		ToggleButton prioritize_new_output_toggle_;

		TextButton reset_presets_button_;

		juce::Rectangle<float> output_preference_bounds_;
	};
```

and change it to:

```cpp
	class AudioSettingsPane : public SettingsPane
	{
	public:
		AudioSettingsPane();
		~AudioSettingsPane();

		void resized() override;
		void paint(Graphics& g) override;

	private:
		static constexpr int GROUP_MARGIN = 10;
		static constexpr int ENDPOINT_Y = 50;
		static constexpr int LABEL_WIDTH = 220;
		static constexpr int OUTPUT_PREFERENCE_HEIGHT = 260;
		static constexpr int LABEL_HEIGHT = 14;
		static constexpr int TOGGLE_BUTTON_HEIGHT = 30;
		static constexpr int RESET_PRESETS_BUTTON_WIDTH = 220;
		static constexpr int BUTTON_HEIGHT = 24;
		static constexpr int MAX_BUTTON_WIDTH = 315;
		static constexpr int BRICKWALL_STEEPNESS_RADIO_GROUP_ID = 1;

		void setText();
		void resizeResetButton(int x, int y);
		void updateBrickwallControlsEnabled();

		void visibilityChanged() override;
		void mouseEnter(const MouseEvent& mouse_event) override;
		void mouseExit(const MouseEvent& mouse_event) override;

		Label output_preference_title_;
		FxOutputPreference output_preference_;
		ToggleButton prioritize_new_output_toggle_;

		Label brickwall_filter_title_;
		ToggleButton brickwall_filter_toggle_;
		ToggleButton brickwall_gentle_toggle_;
		ToggleButton brickwall_standard_toggle_;
		ToggleButton brickwall_steep_toggle_;
		TextButton brickwall_preview_button_;

		TextButton reset_presets_button_;

		juce::Rectangle<float> output_preference_bounds_;
	};
```

- [ ] **Step 2: Construct and wire the new controls**

In `fxsound/Source/GUI/FxSettingsDialog.cpp`, find:

```cpp
FxSettingsDialog::AudioSettingsPane::AudioSettingsPane() :
	SettingsPane("Audio"),
	prioritize_new_output_toggle_(TRANS("Prioritize new output devices")),
	reset_presets_button_(TRANS("Reset presets to factory defaults"))
{
```

and change it to:

```cpp
FxSettingsDialog::AudioSettingsPane::AudioSettingsPane() :
	SettingsPane("Audio"),
	prioritize_new_output_toggle_(TRANS("Prioritize new output devices")),
	brickwall_filter_toggle_(TRANS("Brickwall Filter (20Hz - 20kHz)")),
	brickwall_gentle_toggle_(TRANS("Gentle (~12 dB/octave)")),
	brickwall_standard_toggle_(TRANS("Standard (~48 dB/octave)")),
	brickwall_steep_toggle_(TRANS("Steep (~96 dB/octave)")),
	brickwall_preview_button_(TRANS("Hear What's Removed")),
	reset_presets_button_(TRANS("Reset presets to factory defaults"))
{
```

In the same file, find:

```cpp
	prioritize_new_output_toggle_.setToggleState(FxController::getInstance().isNewOutputPrioritized(), NotificationType::dontSendNotification);
	prioritize_new_output_toggle_.onClick = [this]() { FxController::getInstance().setNewOutputPrioritized(prioritize_new_output_toggle_.getToggleState()); };

	auto preset_modified = false;
```

and change it to:

```cpp
	prioritize_new_output_toggle_.setToggleState(FxController::getInstance().isNewOutputPrioritized(), NotificationType::dontSendNotification);
	prioritize_new_output_toggle_.onClick = [this]() { FxController::getInstance().setNewOutputPrioritized(prioritize_new_output_toggle_.getToggleState()); };

	brickwall_filter_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_title_.setJustificationType(Justification::centredLeft);

	brickwall_filter_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	brickwall_filter_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_toggle_.setWantsKeyboardFocus(true);
	brickwall_filter_toggle_.setToggleState(FxController::getInstance().isBrickwallFilterOn(), NotificationType::dontSendNotification);
	brickwall_filter_toggle_.onClick = [this]() {
		FxController::getInstance().setBrickwallFilterOn(brickwall_filter_toggle_.getToggleState());
		updateBrickwallControlsEnabled();
		};

	brickwall_gentle_toggle_.setTooltip(TRANS("Gentlest rolloff. Lowest CPU use, but lets more content below 20Hz and above 20kHz through."));
	brickwall_standard_toggle_.setTooltip(TRANS("Balanced rolloff and CPU use. Recommended for most listening."));
	brickwall_steep_toggle_.setTooltip(TRANS("Steepest rolloff and closest to a true brickwall, at the cost of more CPU use and more phase distortion right at the edges of the band."));

	for (auto* steepness_toggle : { &brickwall_gentle_toggle_, &brickwall_standard_toggle_, &brickwall_steep_toggle_ })
	{
		steepness_toggle->setMouseCursor(MouseCursor::PointingHandCursor);
		steepness_toggle->setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
		steepness_toggle->setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
		steepness_toggle->setWantsKeyboardFocus(true);
		steepness_toggle->setRadioGroupId(BRICKWALL_STEEPNESS_RADIO_GROUP_ID);
	}

	auto current_steepness = FxController::getInstance().getBrickwallFilterSteepness();
	brickwall_gentle_toggle_.setToggleState(current_steepness == DfxDsp::BrickwallSteepness::Gentle, NotificationType::dontSendNotification);
	brickwall_standard_toggle_.setToggleState(current_steepness == DfxDsp::BrickwallSteepness::Standard, NotificationType::dontSendNotification);
	brickwall_steep_toggle_.setToggleState(current_steepness == DfxDsp::BrickwallSteepness::Steep, NotificationType::dontSendNotification);

	brickwall_gentle_toggle_.onClick = [this]() { FxController::getInstance().setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness::Gentle); };
	brickwall_standard_toggle_.onClick = [this]() { FxController::getInstance().setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness::Standard); };
	brickwall_steep_toggle_.onClick = [this]() { FxController::getInstance().setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness::Steep); };

	brickwall_preview_button_.setClickingTogglesState(true);
	brickwall_preview_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	brickwall_preview_button_.setTooltip(TRANS("Plays back only the content the filter is removing, so you can hear what it affects."));
	brickwall_preview_button_.onClick = [this]() {
		auto& controller = FxController::getInstance();
		bool preview_on = brickwall_preview_button_.getToggleState();
		controller.setBrickwallFilterPreviewOn(preview_on);
		brickwall_preview_button_.setButtonText(preview_on ? TRANS("Stop Previewing") : TRANS("Hear What's Removed"));
		};

	updateBrickwallControlsEnabled();

	auto preset_modified = false;
```

In the same file, find:

```cpp
	setText();

	addAndMakeVisible(&output_preference_title_);
	addAndMakeVisible(&output_preference_);
	addAndMakeVisible(&prioritize_new_output_toggle_);
	addAndMakeVisible(&reset_presets_button_);
}
```

and change it to:

```cpp
	setText();

	addAndMakeVisible(&output_preference_title_);
	addAndMakeVisible(&output_preference_);
	addAndMakeVisible(&prioritize_new_output_toggle_);
	addAndMakeVisible(&brickwall_filter_title_);
	addAndMakeVisible(&brickwall_filter_toggle_);
	addAndMakeVisible(&brickwall_gentle_toggle_);
	addAndMakeVisible(&brickwall_standard_toggle_);
	addAndMakeVisible(&brickwall_steep_toggle_);
	addAndMakeVisible(&brickwall_preview_button_);
	addAndMakeVisible(&reset_presets_button_);
}

void FxSettingsDialog::AudioSettingsPane::updateBrickwallControlsEnabled()
{
	bool filter_on = brickwall_filter_toggle_.getToggleState();

	brickwall_gentle_toggle_.setEnabled(filter_on);
	brickwall_standard_toggle_.setEnabled(filter_on);
	brickwall_steep_toggle_.setEnabled(filter_on);
	brickwall_preview_button_.setEnabled(filter_on);

	if (!filter_on && brickwall_preview_button_.getToggleState())
	{
		brickwall_preview_button_.setToggleState(false, NotificationType::dontSendNotification);
		brickwall_preview_button_.setButtonText(TRANS("Hear What's Removed"));
	}
}
```

- [ ] **Step 3: Position the new controls**

In `fxsound/Source/GUI/FxSettingsDialog.cpp`, find:

```cpp
void FxSettingsDialog::AudioSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	output_preference_title_.setBounds(X_MARGIN, ENDPOINT_Y, LABEL_WIDTH, LABEL_HEIGHT);
	int y = output_preference_title_.getBottom() + 10;
	auto width = getWidth() - ((X_MARGIN + 5) * 2);
	output_preference_.setBounds(X_MARGIN, y, width, OUTPUT_PREFERENCE_HEIGHT);

    y = output_preference_.getBottom() + 10;
    prioritize_new_output_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	auto group_x = output_preference_title_.getX() - GROUP_MARGIN;
	auto group_y = output_preference_title_.getY() - GROUP_MARGIN;
	auto group_width = output_preference_.getRight() - group_x + GROUP_MARGIN;
	auto group_height = prioritize_new_output_toggle_.getBottom() - group_y + GROUP_MARGIN;
	output_preference_bounds_ = juce::Rectangle<float>(group_x, group_y, group_width, group_height);

	y = prioritize_new_output_toggle_.getBottom() + 30;
	resizeResetButton(X_MARGIN, y);
}
```

and change it to:

```cpp
void FxSettingsDialog::AudioSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	output_preference_title_.setBounds(X_MARGIN, ENDPOINT_Y, LABEL_WIDTH, LABEL_HEIGHT);
	int y = output_preference_title_.getBottom() + 10;
	auto width = getWidth() - ((X_MARGIN + 5) * 2);
	output_preference_.setBounds(X_MARGIN, y, width, OUTPUT_PREFERENCE_HEIGHT);

    y = output_preference_.getBottom() + 10;
    prioritize_new_output_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	auto group_x = output_preference_title_.getX() - GROUP_MARGIN;
	auto group_y = output_preference_title_.getY() - GROUP_MARGIN;
	auto group_width = output_preference_.getRight() - group_x + GROUP_MARGIN;
	auto group_height = prioritize_new_output_toggle_.getBottom() - group_y + GROUP_MARGIN;
	output_preference_bounds_ = juce::Rectangle<float>(group_x, group_y, group_width, group_height);

	y = prioritize_new_output_toggle_.getBottom() + 30;
	brickwall_filter_title_.setBounds(X_MARGIN, y, LABEL_WIDTH, LABEL_HEIGHT);

	y = brickwall_filter_title_.getBottom() + 10;
	brickwall_filter_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	y = brickwall_filter_toggle_.getBottom() + 5;
	auto steepness_indent = X_MARGIN + 20;
	auto steepness_width = width - 20;
	brickwall_gentle_toggle_.setBounds(steepness_indent, y, steepness_width, TOGGLE_BUTTON_HEIGHT);

	y = brickwall_gentle_toggle_.getBottom() + 5;
	brickwall_standard_toggle_.setBounds(steepness_indent, y, steepness_width, TOGGLE_BUTTON_HEIGHT);

	y = brickwall_standard_toggle_.getBottom() + 5;
	brickwall_steep_toggle_.setBounds(steepness_indent, y, steepness_width, TOGGLE_BUTTON_HEIGHT);

	y = brickwall_steep_toggle_.getBottom() + 10;
	brickwall_preview_button_.setBounds(steepness_indent, y, RESET_PRESETS_BUTTON_WIDTH, BUTTON_HEIGHT);

	y = brickwall_preview_button_.getBottom() + 30;
	resizeResetButton(X_MARGIN, y);
}
```

- [ ] **Step 4: Refresh text in `setText()` (locale-safe, matches existing pattern)**

In `fxsound/Source/GUI/FxSettingsDialog.cpp`, find:

```cpp
void FxSettingsDialog::AudioSettingsPane::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

	output_preference_title_.setFont(theme.getNormalFont());
	output_preference_title_.setText(TRANS("Output Device Preference"), NotificationType::dontSendNotification);

	prioritize_new_output_toggle_.setButtonText(TRANS("Prioritize new output devices"));

	reset_presets_button_.setButtonText(TRANS("Reset presets to factory defaults"));
	resizeResetButton(reset_presets_button_.getX(), reset_presets_button_.getY());
}
```

and change it to:

```cpp
void FxSettingsDialog::AudioSettingsPane::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

	output_preference_title_.setFont(theme.getNormalFont());
	output_preference_title_.setText(TRANS("Output Device Preference"), NotificationType::dontSendNotification);

	prioritize_new_output_toggle_.setButtonText(TRANS("Prioritize new output devices"));

	brickwall_filter_title_.setFont(theme.getNormalFont());
	brickwall_filter_title_.setText(TRANS("Bandwidth Filter"), NotificationType::dontSendNotification);
	brickwall_filter_toggle_.setButtonText(TRANS("Brickwall Filter (20Hz - 20kHz)"));
	brickwall_gentle_toggle_.setButtonText(TRANS("Gentle (~12 dB/octave)"));
	brickwall_standard_toggle_.setButtonText(TRANS("Standard (~48 dB/octave)"));
	brickwall_steep_toggle_.setButtonText(TRANS("Steep (~96 dB/octave)"));

	reset_presets_button_.setButtonText(TRANS("Reset presets to factory defaults"));
	resizeResetButton(reset_presets_button_.getX(), reset_presets_button_.getY());
}
```

- [ ] **Step 5: Auto-revert preview when the Settings dialog closes**

In `fxsound/Source/GUI/FxSettingsDialog.cpp`, find:

```cpp
void FxSettingsDialog::closeButtonPressed()
{
	exitModalState(0);
	removeFromDesktop();
}
```

and change it to:

```cpp
void FxSettingsDialog::closeButtonPressed()
{
	FxController::getInstance().setBrickwallFilterPreviewOn(false);

	exitModalState(0);
	removeFromDesktop();
}
```

- [ ] **Step 6: Build to verify**

Run: `msbuild fxsound\Project\FxSound.sln /p:Configuration=Debug /p:Platform=x64`
Expected: `Build succeeded. 0 Error(s)`.

- [ ] **Step 7: Commit**

```bash
git add fxsound/Source/GUI/FxSettingsDialog.h fxsound/Source/GUI/FxSettingsDialog.cpp
git commit -m "feat(gui): add brickwall filter controls to Settings > Audio"
```

---

### Task 5: End-to-end manual verification

**Files:** none (verification only).

- [ ] **Step 1: Run the app**

Run FxSound_App in the Visual Studio debugger (Debug, x64) — requires the FxSound virtual audio driver already installed via the normal installer, per CLAUDE.md. Confirm it starts with no assertion dialog (this exercises `debugVerifyBrickwallFilterResponse()` from Task 2).

- [ ] **Step 2: Verify the Settings UI**

Open the main window menu > Settings > Audio tab. Confirm:
- The "Brickwall Filter (20Hz - 20kHz)" toggle, three steepness options, and "Hear What's Removed" button are all visible with no clipping or overlap with the existing output-device/reset-presets controls. If anything overlaps, adjust the Y-offsets in `resized()` and/or `SettingsComponent::HEIGHT` (Task 4) and rebuild.
- The three steepness options and preview button are disabled (greyed out) while the master toggle is off, and become enabled when it's turned on.
- Hovering each steepness option shows its tooltip text.

- [ ] **Step 3: Verify audio behavior**

With music/audio playing through FxSound:
- Turn the filter on — audio should keep playing normally (steepness defaults to Standard).
- Switch between Gentle/Standard/Steep — audio should keep playing (a brief discontinuity from the state reset is expected and acceptable).
- Click "Hear What's Removed" — output should become very quiet, containing only removed sub-20Hz/ultra-20kHz content. Click it again — normal playback should resume and the button label should revert to "Hear What's Removed".

- [ ] **Step 4: Verify auto-revert and persistence**

- With preview active, turn off FxSound's main power button — reopen Settings and confirm the preview button shows "Hear What's Removed" (i.e. it auto-reverted).
- With preview active, close the Settings dialog, then reopen it — confirm preview is off.
- With the filter on and steepness set to Steep, fully quit and restart FxSound — confirm the filter is still on and steepness is still Steep after restart, and that preview is off by default.

- [ ] **Step 5: Flag for human review**

Per this repo's CLAUDE.md, changes under `dsp/` are higher-risk. When opening the PR, explicitly call out in the description that `dsp/include/DfxDsp.h`, `dsp/DfxDsp.cpp`, `dsp/u_DfxDsp.h`, `dsp/DfxDspPrivate.cpp`, and the new `dsp/ptutil/**/FiltBrickwall.*` files touch the real-time audio path and warrant careful review of real-time-safety (no allocation/locking/blocking in `processAudio`) before merge. Do not merge it yourself.
