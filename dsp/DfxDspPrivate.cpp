/*
FxSound
Copyright (C) 2025  FxSound LLC

Contributors:
	www.theremino.com (2025)
	
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
#include <vector>
#include <chrono>

#include "BinauralSyn.h"
#include "ptutil\dfxp\u_dfxp.h"
#include "com.h"
#include "dfxSharedUtil.h"
#include "GraphicEq.h"
#include "spectrum.h"
#include "SurroundSyn.h"

#define DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE		L"DFX"
#define DFXG_DISPLAYED_DFX_PRODUCT_NAME_WIDE    L"FxSound"
#define DFXP_VENDOR_CODE_UNIVERSAL			  23

#define DFXG_MIN_USER_PRESET_INDEX				 99 /* 0 based number of min user preset (preset number 100) */
#define DFXG_MAX_PRESET_NAME_LENGTH              128
#define DFXG_NO_PROCESSING_PRESET                0
#define DFXG_DEFAULT_PRESET_INDEX                0 /* 0 based, i.e. index of 2 corresponds to preset number 3 */
#define DFXG_FREE_PRESET_MIN_INDEX               0 
#define DFXG_FREE_PRESET_MAX_INDEX               0


#define MIDI_MIN_VALUE          0
#define MIDI_MAX_VALUE          127

#define DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ  20.0
// User-adjustable range for the high-pass (low-frequency) cutoff. 0Hz bypasses
// the high-pass band entirely (see filtBrickwallProcessSample()); the default
// above is what the debug self-checks below verify against.
#define DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MIN_HZ  0.0f
#define DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MAX_HZ  200.0f
// Auto-levelling for the preview ("hear what's removed"), in both modes. What a
// low cut removes is usually far quieter than the music (at a 20Hz cutoff it
// is mostly subsonic rumble), so it is raised towards a comfortable listening
// level - but by an amount that depends on how loud it actually is, since at a
// 100-200Hz cutoff it contains real bass and a fixed boost (the earlier +30dB)
// would be very loud and clip. A soft limiter guarantees it never clips.
#define DFXG_BRICKWALL_PREVIEW_TARGET_RMS   0.1f    // -20dBFS
#define DFXG_BRICKWALL_PREVIEW_MAX_GAIN     100.0f  // +40dB; the gain never goes below 1 (0dB)
#define DFXG_BRICKWALL_PREVIEW_RMS_MS       100.0   // loudness measurement window
#define DFXG_BRICKWALL_PREVIEW_RISE_MS      400.0   // gain rises slowly...
#define DFXG_BRICKWALL_PREVIEW_FALL_MS      10.0    // ...and falls fast, so it never blasts
#define DFXG_BRICKWALL_PREVIEW_LIMIT        0.9f    // soft limiter knee; output never exceeds 1.0

/* One-pole smoothing coefficients for the preview leveller at a sample rate
 * (BrickwallPreviewLevelCoeffs is in u_DfxDsp.h). Not for the per-buffer path:
 * applyBrickwallFilter() caches the result per sample rate. */
static BrickwallPreviewLevelCoeffs brickwallPreviewLevelCoeffs(int sample_rate)
{
	BrickwallPreviewLevelCoeffs coeffs;
	double fs = (sample_rate > 0) ? (double)sample_rate : 44100.0;

	coeffs.rms = (float)(1.0 - exp(-1000.0 / (DFXG_BRICKWALL_PREVIEW_RMS_MS * fs)));
	coeffs.rise = (float)(1.0 - exp(-1000.0 / (DFXG_BRICKWALL_PREVIEW_RISE_MS * fs)));
	coeffs.fall = (float)(1.0 - exp(-1000.0 / (DFXG_BRICKWALL_PREVIEW_FALL_MS * fs)));
	return coeffs;
}

/*
 * FUNCTION: brickwallLevelPreviewFrame()
 * DESCRIPTION:
 *   Auto-levels one interleaved frame of the preview signal in place: a
 *   feed-forward RMS measurement (shared by all channels, so the stereo image
 *   is kept) steers a smoothed gain between 0dB and +40dB towards
 *   DFXG_BRICKWALL_PREVIEW_TARGET_RMS, then a soft limiter keeps every sample
 *   within +/-1.0. Real-time safe (arithmetic only); *fp_mean_square and *fp_gain
 *   are the caller's audio-thread state.
 */
static void brickwallLevelPreviewFrame(float *fp_frame, int i_num_channels, const BrickwallPreviewLevelCoeffs *cp_coeffs,
	float *fp_mean_square, float *fp_gain)
{
	float sum = 0.0f;
	float desired_gain = DFXG_BRICKWALL_PREVIEW_MAX_GAIN;
	int channel;

	if (i_num_channels <= 0)
	{
		return;
	}

	for (channel = 0; channel < i_num_channels; channel++)
	{
		sum += fp_frame[channel] * fp_frame[channel];
	}
	*fp_mean_square += cp_coeffs->rms * (sum / (float)i_num_channels - *fp_mean_square);
	if (*fp_mean_square < 1e-20f)
	{
		*fp_mean_square = 0.0f; // keep the state out of the denormal range in silence
	}

	if (*fp_mean_square > 1e-12f)
	{
		desired_gain = DFXG_BRICKWALL_PREVIEW_TARGET_RMS / sqrtf(*fp_mean_square);
		if (desired_gain > DFXG_BRICKWALL_PREVIEW_MAX_GAIN)
		{
			desired_gain = DFXG_BRICKWALL_PREVIEW_MAX_GAIN;
		}
		else if (desired_gain < 1.0f)
		{
			desired_gain = 1.0f;
		}
	}
	*fp_gain += ((desired_gain > *fp_gain) ? cp_coeffs->rise : cp_coeffs->fall) * (desired_gain - *fp_gain);

	for (channel = 0; channel < i_num_channels; channel++)
	{
		float sample = fp_frame[channel] * *fp_gain;
		float magnitude = fabsf(sample);

		if (magnitude > DFXG_BRICKWALL_PREVIEW_LIMIT)
		{
			float over = (magnitude - DFXG_BRICKWALL_PREVIEW_LIMIT) / (1.0f - DFXG_BRICKWALL_PREVIEW_LIMIT);
			magnitude = DFXG_BRICKWALL_PREVIEW_LIMIT + (1.0f - DFXG_BRICKWALL_PREVIEW_LIMIT) * tanhf(over);
			sample = (sample < 0.0f) ? -magnitude : magnitude;
		}
		fp_frame[channel] = sample;
	}
}

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
	case DfxDsp::BrickwallSteepness::UltraSteep:
		return 11;
	case DfxDsp::BrickwallSteepness::Standard:
	default:
		return 4;
	}
}

/*
 * FUNCTION: brickwallFirLatencyMsForMode() / brickwallFirBlockSizeForMode()
 * DESCRIPTION:
 *   Total latency and FFT partition size for each linear-phase latency mode
 *   (see the design spec's Modes table). Low uses a smaller partition so its
 *   filter stays close to the previous 20ms direct-convolution design's length.
 */
static double brickwallFirLatencyMsForMode(int mode)
{
	switch (mode)
	{
	case DfxDsp::BrickwallLinearPhaseLatency::Medium:
		return 160.0;
	case DfxDsp::BrickwallLinearPhaseLatency::High:
		return 320.0;
	case DfxDsp::BrickwallLinearPhaseLatency::Max:
		return 640.0;
	case DfxDsp::BrickwallLinearPhaseLatency::Low:
	default:
		return 20.0;
	}
}

static int brickwallFirBlockSizeForMode(int mode)
{
	return (mode == DfxDsp::BrickwallLinearPhaseLatency::Low) ? 128 : 512;
}

/*
 * FUNCTION: brickwallDesignIirParams()
 * DESCRIPTION:
 *   Designs a complete Zero Latency (IIR) parameter set. Designer thread (and
 *   debug self-checks) only: the cutoff calibration is a numeric search, far
 *   too slow for the audio thread.
 *
 *   The filter itself is a low cut only: a cascade of identical high-pass
 *   sections, calibrated so the whole cascade is -3dB at the cutoff (cascading
 *   shifts its -3dB point away from a single section's own cutoff, so the
 *   design depends on the section count too). Its low-pass sections instead
 *   produce the preview ("hear what's removed"): a complementary low-pass at
 *   the same cutoff and steepness, also -3dB at the cutoff. Subtracting the
 *   filtered signal from the input can't be used here, because this
 *   minimum-phase cascade shifts the phase of content far above the cutoff
 *   without changing its level, and that phase shift would leak into the
 *   difference (about -16dB at 1kHz for Ultra Steep).
 *
 *   A 0Hz cutoff means "no low-frequency filtering": both bands get 0
 *   sections instead of being designed, since a biquad high-pass at exactly
 *   0Hz is degenerate (see FiltBrickwall.h), and with nothing removed the
 *   preview is silent.
 */
static void brickwallDesignIirParams(int sample_rate, int steepness, float cutoff_hz, unsigned int reset_serial,
	BrickwallIirParams *params)
{
	int num_sections = brickwallNumSectionsForSteepness(static_cast<DfxDsp::BrickwallSteepness>(steepness));
	bool hp_bypassed = cutoff_hz <= 0.0f;

	*params = {};
	if (!hp_bypassed && sample_rate > 0)
	{
		double calibrated_hp_cutoff = filtBrickwallCalibrateCascadeCutoff((double)cutoff_hz, (double)sample_rate, num_sections, 1);
		double calibrated_lp_cutoff = filtBrickwallCalibrateCascadeCutoff((double)cutoff_hz, (double)sample_rate, num_sections, 0);

		filtBrickwallDesignHighPass(calibrated_hp_cutoff, (double)sample_rate, &params->hp_coeffs);
		filtBrickwallDesignLowPass(calibrated_lp_cutoff, (double)sample_rate, &params->lp_coeffs);
		params->num_hp_sections = num_sections;
		params->num_lp_sections = num_sections;
	}
	params->sample_rate = sample_rate;
	params->reset_serial = reset_serial;
}

/*
 * FUNCTION: brickwallIirExchangeInit() / brickwallIirExchangePublish() /
 *           brickwallIirExchangeAdopt()
 * DESCRIPTION:
 *   Lock-free triple buffer of IIR parameter sets (see BrickwallIirExchange in
 *   u_DfxDsp.h). Publish: designer thread only (the single producer); it
 *   fills its back slot, then swaps it into the middle with the dirty bit set
 *   (release). Adopt: audio thread only (the single consumer); if the middle
 *   is dirty it swaps its front slot in for it (acquire), and it always
 *   returns the front slot, which nothing else writes while it is the front;
 *   *bp_took_new (if non-NULL) says whether that is a newly published set.
 *   Atomic loads/exchanges only - real-time safe. Before the first publish the
 *   front set has sample_rate 0, which never matches a real format.
 */
static void brickwallIirExchangeInit(BrickwallIirExchange *exchange)
{
	int slot;

	for (slot = 0; slot < BRICKWALL_IIR_NUM_SLOTS; slot++)
	{
		exchange->slots[slot] = {};
	}
	exchange->front_slot = 0;
	exchange->middle_slot.store(1, std::memory_order_relaxed);
	exchange->back_slot = 2;
}

static void brickwallIirExchangePublish(BrickwallIirExchange *exchange, const BrickwallIirParams *params)
{
	exchange->slots[exchange->back_slot] = *params;
	int previous = exchange->middle_slot.exchange(exchange->back_slot | BRICKWALL_IIR_SLOT_DIRTY, std::memory_order_acq_rel);
	exchange->back_slot = previous & BRICKWALL_IIR_SLOT_MASK;
}

static const BrickwallIirParams *brickwallIirExchangeAdopt(BrickwallIirExchange *exchange, bool *bp_took_new)
{
	bool took_new = false;

	if (exchange->middle_slot.load(std::memory_order_relaxed) & BRICKWALL_IIR_SLOT_DIRTY)
	{
		int previous = exchange->middle_slot.exchange(exchange->front_slot, std::memory_order_acq_rel);
		exchange->front_slot = previous & BRICKWALL_IIR_SLOT_MASK;
		took_new = true;
	}
	if (bp_took_new != NULL)
	{
		*bp_took_new = took_new;
	}

	return &exchange->slots[exchange->front_slot];
}

/*
 * FUNCTION: brickwallIirNeedsReset()
 * DESCRIPTION:
 *   Audio thread: whether the IIR cascade's history can't be carried over to
 *   the parameter set *params. It can't when the set belongs to a newer reset
 *   serial (filter on, power on, format change), when the section counts
 *   changed (steepness change, or a 0Hz cutoff bypass toggled), or when the
 *   IIR path wasn't running (linear phase was on, the filter or power was
 *   off, or no matching set was ready), since its history is then stale. A
 *   cutoff-only change keeps the history. Either way the change is
 *   crossfaded (see brickwallIirPoll()).
 */
static bool brickwallIirNeedsReset(const BrickwallIirParams *params, unsigned int history_serial,
	int history_hp_sections, int history_lp_sections, bool ran_last_buffer)
{
	return !ran_last_buffer ||
		params->reset_serial != history_serial ||
		params->num_hp_sections != history_hp_sections ||
		params->num_lp_sections != history_lp_sections;
}

/*
 * Low cut filter output stage: transitions.
 *
 * Every change of what the filter outputs is ramped, per sample, on the audio
 * thread; nothing here allocates, locks, designs filters or makes system
 * calls, and all of its state (BrickwallAudioState, u_DfxDsp.h) is audio
 * thread only. Ramp lengths come from the cached sample rate
 * (brickwallAudioStateSetRate()). Extra work is done only while a ramp runs.
 *
 * Output source state machine. The source is Dry (filter or power off, or
 * nothing ready yet), IIR (Zero Latency) or FIR (the active Linear Phase
 * engine). Each segment (a run of frames in which no ramp ends) starts by
 * comparing the desired source with the current one (brickwallStepSource()):
 *
 *   - Dry <-> IIR, current source at full gain: crossfade over
 *     BRICKWALL_CROSSFADE_MS, running both (crossfade_from = the old one). If
 *     the old source is wanted again mid-way, the crossfade reverses from
 *     where it is; any other change waits until it has finished.
 *   - Anything involving FIR (Dry/IIR -> FIR, FIR -> Dry/IIR, one engine to
 *     another), or a change while the current source isn't at full gain:
 *     fade the current source out over BRICKWALL_FADE_MS (it keeps running
 *     while it fades), switch while silent, fade the new source in. A FIR
 *     engine's fade-in ramps its input rather than its output: a fresh engine
 *     outputs (nearly) nothing for its latency and would then start abruptly
 *     on whatever the input was doing; ramping the input makes the delayed
 *     onset itself a fade. That fade-in always completes before a fade-out.
 *   - Dry at full gain, nothing wanted and the preview off: the stage is idle
 *     and returns at once, at no cost.
 *
 * Within the IIR source, a new parameter set (cutoff or steepness change) is
 * crossfaded from the current set to the incoming one over
 * BRICKWALL_CROSSFADE_MS, both running; the incoming history starts as a
 * copy of the current one when the section counts and reset serial match
 * (cutoff-only change) and from zero otherwise. No other set is adopted
 * until the crossfade ends, so a cutoff drag follows in ~20ms steps.
 *
 * The preview mixes the normal output with the auto-levelled "what's
 * removed" signal (IIR: the complementary low-pass; FIR: delayed dry minus
 * filtered; Dry: nothing), crossfading over BRICKWALL_CROSSFADE_MS when it is
 * switched on or off.
 *
 * Reset guarantees are unchanged: the IIR history is cleared whenever the
 * IIR source starts after not running, and a FIR engine is only switched to
 * when it carries the current reset serial (it is never fed while it isn't
 * the source, and is retired or invalidated before it could be played again).
 */

/*
 * FUNCTION: brickwallRampWeight()  [internal]
 * DESCRIPTION:
 *   Weight pos/len of a linear ramp; exactly 1 at its end.
 */
static inline float brickwallRampWeight(int pos, int len, float step)
{
	return (pos >= len) ? 1.0f : (float)pos * step;
}

/*
 * FUNCTION: brickwallRescaleRamp()  [internal]
 * DESCRIPTION:
 *   Moves a ramp position to a new ramp length, keeping its proportion (an
 *   ended ramp stays ended).
 */
static int brickwallRescaleRamp(int pos, int old_len, int new_len)
{
	if (old_len <= 0 || pos >= old_len)
	{
		return (pos > 0) ? new_len : 0;
	}

	return (int)(((long long)pos * (long long)new_len) / (long long)old_len);
}

/*
 * FUNCTION: brickwallAudioStateInit()
 * DESCRIPTION:
 *   Initial output stage: Dry at full gain, idle, no IIR parameter set.
 */
static void brickwallAudioStateInit(BrickwallAudioState *state)
{
	int slot, channel;

	state->ramp_rate = -1;
	state->crossfade_frames = 1;
	state->fade_frames = 1;
	state->source = BRICKWALL_SOURCE_DRY;
	state->crossfade_from = -1;
	state->crossfade_pos = 0;
	state->gain_pos = state->fade_frames;
	state->gain_dir = 0;
	state->preview_pos = 0;
	state->preview_was_on = false;
	state->preview_mean_square = 0.0f;
	state->preview_gain = 1.0f;
	state->preview_level_coeffs = {};
	for (slot = 0; slot < 2; slot++)
	{
		state->iir_params[slot] = {};
		for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
		{
			filtBrickwallResetChannelState(&state->iir_states[slot][channel]);
		}
	}
	state->iir_cur = 0;
	state->iir_crossfading = false;
	state->iir_crossfade_pos = 0;
	state->iir_running = false;
}

/*
 * FUNCTION: brickwallAudioStateSetRate()
 * DESCRIPTION:
 *   Recomputes the ramp lengths and the preview leveller's coefficients when
 *   the sample rate changes (cached per rate, like the leveller coefficients
 *   were; the exp() calls only run on a rate change). Ramps in progress keep
 *   their proportion.
 */
static void brickwallAudioStateSetRate(BrickwallAudioState *state, int sample_rate)
{
	double fs;
	int crossfade_frames, fade_frames;

	if (sample_rate == state->ramp_rate)
	{
		return;
	}

	fs = (sample_rate > 0) ? (double)sample_rate : 44100.0;
	crossfade_frames = (int)(fs * BRICKWALL_CROSSFADE_MS / 1000.0 + 0.5);
	fade_frames = (int)(fs * BRICKWALL_FADE_MS / 1000.0 + 0.5);
	if (crossfade_frames < 1)
	{
		crossfade_frames = 1;
	}
	if (fade_frames < 1)
	{
		fade_frames = 1;
	}

	state->crossfade_pos = brickwallRescaleRamp(state->crossfade_pos, state->crossfade_frames, crossfade_frames);
	state->preview_pos = brickwallRescaleRamp(state->preview_pos, state->crossfade_frames, crossfade_frames);
	state->iir_crossfade_pos = brickwallRescaleRamp(state->iir_crossfade_pos, state->crossfade_frames, crossfade_frames);
	state->gain_pos = brickwallRescaleRamp(state->gain_pos, state->fade_frames, fade_frames);
	state->crossfade_frames = crossfade_frames;
	state->fade_frames = fade_frames;

	state->preview_level_coeffs = brickwallPreviewLevelCoeffs(sample_rate);
	state->ramp_rate = sample_rate;
}

/*
 * FUNCTION: brickwallIirCopyParams()  [internal]
 * DESCRIPTION:
 *   Takes the audio thread's own copy of a published parameter set, with the
 *   section counts clamped to FILT_BRICKWALL_MAX_SECTIONS: brickwallNum-
 *   SectionsForSteepness() only returns 1/4/8/11, but filtBrickwallProcess-
 *   Sample() indexes fixed-size arrays without checking, so this is a safety
 *   net.
 */
static void brickwallIirCopyParams(BrickwallIirParams *dst, const BrickwallIirParams *src)
{
	*dst = *src;
	if (dst->num_hp_sections > FILT_BRICKWALL_MAX_SECTIONS)
	{
		dst->num_hp_sections = FILT_BRICKWALL_MAX_SECTIONS;
	}
	else if (dst->num_hp_sections < 0)
	{
		dst->num_hp_sections = 0;
	}
	if (dst->num_lp_sections > FILT_BRICKWALL_MAX_SECTIONS)
	{
		dst->num_lp_sections = FILT_BRICKWALL_MAX_SECTIONS;
	}
	else if (dst->num_lp_sections < 0)
	{
		dst->num_lp_sections = 0;
	}
}

/*
 * FUNCTION: brickwallIirFinishCrossfade()  [internal]
 * DESCRIPTION:
 *   Makes the incoming IIR set (and its history) the current one.
 */
static void brickwallIirFinishCrossfade(BrickwallAudioState *state)
{
	state->iir_cur ^= 1;
	state->iir_crossfading = false;
	state->iir_crossfade_pos = 0;
}

/*
 * FUNCTION: brickwallIirPoll()
 * DESCRIPTION:
 *   Adopts the newest published IIR parameter set, unless a parameter
 *   crossfade is still running (the set then waits in the exchange and the
 *   newest one is taken when it ends). A set for another sample rate or an
 *   older reset serial is stale and ignored (a fresh one always follows).
 *   While the cascade isn't running, the set simply becomes the current one
 *   (its history is cleared when it starts, see brickwallIirStart()). While
 *   it runs, the set becomes the incoming one and a crossfade starts; its
 *   history is a copy of the current one for a cutoff-only change, and empty
 *   otherwise (see brickwallIirNeedsReset()). The copies are plain structs:
 *   no allocation.
 */
static void brickwallIirPoll(BrickwallAudioState *state, BrickwallIirExchange *exchange, int sample_rate, unsigned int reset_serial)
{
	const BrickwallIirParams *published;
	const BrickwallIirParams *current;
	bool took_new = false;
	int next, channel;

	if (state->iir_crossfading)
	{
		return;
	}

	published = brickwallIirExchangeAdopt(exchange, &took_new);
	if (!took_new || published->sample_rate != sample_rate || published->reset_serial != reset_serial)
	{
		return;
	}

	if (!state->iir_running)
	{
		brickwallIirCopyParams(&state->iir_params[state->iir_cur], published);
		return;
	}

	next = state->iir_cur ^ 1;
	current = &state->iir_params[state->iir_cur];
	brickwallIirCopyParams(&state->iir_params[next], published);
	if (brickwallIirNeedsReset(&state->iir_params[next], current->reset_serial, current->num_hp_sections,
		current->num_lp_sections, true))
	{
		for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
		{
			filtBrickwallResetChannelState(&state->iir_states[next][channel]);
		}
	}
	else
	{
		for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
		{
			state->iir_states[next][channel] = state->iir_states[state->iir_cur][channel];
		}
	}
	state->iir_crossfading = true;
	state->iir_crossfade_pos = 0;
}

/*
 * FUNCTION: brickwallIirReady()
 * DESCRIPTION:
 *   Whether the IIR set the cascade is (or will be, once a crossfade ends)
 *   running was designed for this sample rate and reset serial.
 */
static bool brickwallIirReady(const BrickwallAudioState *state, int sample_rate, unsigned int reset_serial)
{
	const BrickwallIirParams *params = &state->iir_params[state->iir_crossfading ? (state->iir_cur ^ 1) : state->iir_cur];

	return params->sample_rate == sample_rate && params->reset_serial == reset_serial;
}

/*
 * FUNCTION: brickwallIirStart() / brickwallIirStop()
 * DESCRIPTION:
 *   The cascade starts with cleared history whenever it starts feeding again
 *   after a gap; when it stops, any parameter crossfade is completed at once
 *   (its history no longer matters).
 */
static void brickwallIirStart(BrickwallAudioState *state)
{
	int channel;

	if (state->iir_running)
	{
		return;
	}
	if (state->iir_crossfading)
	{
		brickwallIirFinishCrossfade(state);
	}
	for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
	{
		filtBrickwallResetChannelState(&state->iir_states[state->iir_cur][channel]);
	}
	state->iir_running = true;
}

static void brickwallIirStop(BrickwallAudioState *state)
{
	if (state->iir_crossfading)
	{
		brickwallIirFinishCrossfade(state);
	}
	state->iir_running = false;
}

/*
 * FUNCTION: brickwallIirRunChannel()  [internal]
 * DESCRIPTION:
 *   Runs one sample of one channel through the current IIR set: the
 *   high-pass (the filter) and the complementary low-pass (the preview, see
 *   brickwallDesignIirParams()) both run on every sample, with separate
 *   histories, so toggling the preview never starts either from stale
 *   history. During a parameter crossfade the incoming set runs too and both
 *   outputs are blended with r_incoming_weight.
 */
static inline void brickwallIirRunChannel(BrickwallAudioState *state, int channel, realtype r_input, float r_incoming_weight,
	float *fp_filtered, float *fp_removed)
{
	const BrickwallIirParams *params = &state->iir_params[state->iir_cur];
	FiltBrickwallChannelState *history = &state->iir_states[state->iir_cur][channel];
	float filtered, removed;

	filtered = (float)filtBrickwallProcessSample(r_input, params->num_hp_sections, 0, &params->hp_coeffs, &params->lp_coeffs, history);
	removed = (params->num_lp_sections > 0) ?
		(float)filtBrickwallProcessSample(r_input, 0, params->num_lp_sections, &params->hp_coeffs, &params->lp_coeffs, history) : 0.0f;

	if (state->iir_crossfading)
	{
		const int next = state->iir_cur ^ 1;
		const BrickwallIirParams *incoming = &state->iir_params[next];
		FiltBrickwallChannelState *incoming_history = &state->iir_states[next][channel];
		float incoming_filtered, incoming_removed;

		incoming_filtered = (float)filtBrickwallProcessSample(r_input, incoming->num_hp_sections, 0,
			&incoming->hp_coeffs, &incoming->lp_coeffs, incoming_history);
		incoming_removed = (incoming->num_lp_sections > 0) ?
			(float)filtBrickwallProcessSample(r_input, 0, incoming->num_lp_sections, &incoming->hp_coeffs, &incoming->lp_coeffs, incoming_history) : 0.0f;
		filtered += r_incoming_weight * (incoming_filtered - filtered);
		removed += r_incoming_weight * (incoming_removed - removed);
	}

	*fp_filtered = filtered;
	*fp_removed = removed;
}

/*
 * FUNCTION: brickwallSettleRamps()  [internal]
 * DESCRIPTION:
 *   Ends every ramp that has reached its end.
 */
static void brickwallSettleRamps(BrickwallAudioState *state)
{
	if (state->crossfade_from >= 0 && state->crossfade_pos >= state->crossfade_frames)
	{
		state->crossfade_from = -1;
		state->crossfade_pos = 0;
	}
	if ((state->gain_dir > 0 && state->gain_pos >= state->fade_frames) || (state->gain_dir < 0 && state->gain_pos <= 0))
	{
		state->gain_dir = 0;
	}
	if (state->iir_crossfading && state->iir_crossfade_pos >= state->crossfade_frames)
	{
		brickwallIirFinishCrossfade(state);
	}
}

/*
 * FUNCTION: brickwallStepSource()
 * DESCRIPTION:
 *   One step of the output source state machine (see the overview above),
 *   at the start of a segment. Returns true if it switched source while
 *   silent: the caller then re-evaluates the desired source before running
 *   (the switch itself can make a pending engine adoptable).
 */
static bool brickwallStepSource(BrickwallAudioState *state, int desired)
{
	brickwallSettleRamps(state);

	if (state->crossfade_from >= 0)
	{
		// Dry <-> IIR crossfade running: reverse it if the old source is wanted
		// again, otherwise let it finish first.
		if (desired == state->crossfade_from)
		{
			state->crossfade_from = state->source;
			state->source = desired;
			state->crossfade_pos = state->crossfade_frames - state->crossfade_pos;
			brickwallSettleRamps(state);
		}
		return false;
	}

	if (state->source == BRICKWALL_SOURCE_FIR && state->gain_dir > 0)
	{
		// A Linear Phase fade-in ramps the engine's input: let it complete.
		return false;
	}

	if (desired == state->source)
	{
		state->gain_dir = (state->gain_pos < state->fade_frames) ? 1 : 0;
		return false;
	}

	if (state->gain_pos >= state->fade_frames &&
		state->source != BRICKWALL_SOURCE_FIR && desired != BRICKWALL_SOURCE_FIR)
	{
		state->crossfade_from = state->source;
		state->source = desired;
		state->crossfade_pos = 0;
		state->gain_dir = 0;
		return false;
	}

	if (state->gain_pos > 0)
	{
		state->gain_dir = -1;
		return false;
	}

	state->source = desired;
	state->gain_dir = 1;
	return true;
}

/*
 * FUNCTION: brickwallRunSegment()
 * DESCRIPTION:
 *   Processes frames in place, up to i_num_frames but stopping where any ramp
 *   ends (so the caller can step the state machine there). Returns the number
 *   of frames processed (at least 1). sp_engine is the engine to run while
 *   the source is FIR (NULL otherwise). The first i_processed_channels
 *   channels of each frame are processed; the rest pass through. Real-time
 *   safe: arithmetic, the IIR cascade and filtPartConvProcessFrame() only.
 */
static int brickwallRunSegment(BrickwallAudioState *state, FiltPartConv *sp_engine, float *fp_buffer, int i_num_frames,
	int i_num_channels, int i_processed_channels, bool b_preview_on)
{
	const float crossfade_step = 1.0f / (float)state->crossfade_frames;
	const float fade_step = 1.0f / (float)state->fade_frames;
	const bool run_iir = state->source == BRICKWALL_SOURCE_IIR || state->crossfade_from == BRICKWALL_SOURCE_IIR;
	const bool run_fir = state->source == BRICKWALL_SOURCE_FIR && sp_engine != NULL;
	const int fir_channels = run_fir ? sp_engine->num_channels : 0;
	const bool crossfading = state->crossfade_from >= 0;
	bool fir_fading_in;
	int preview_dir = 0;
	int segment = i_num_frames;
	int n, channel;

	brickwallSettleRamps(state);
	if (run_iir)
	{
		brickwallIirStart(state);
	}
	else
	{
		brickwallIirStop(state);
	}
	fir_fading_in = run_fir && state->gain_dir > 0;

	if (b_preview_on && state->preview_pos < state->crossfade_frames)
	{
		preview_dir = 1;
	}
	else if (!b_preview_on && state->preview_pos > 0)
	{
		preview_dir = -1;
	}

	// The segment ends where the first ramp ends.
	if (crossfading && state->crossfade_frames - state->crossfade_pos < segment)
	{
		segment = state->crossfade_frames - state->crossfade_pos;
	}
	if (state->gain_dir > 0 && state->fade_frames - state->gain_pos < segment)
	{
		segment = state->fade_frames - state->gain_pos;
	}
	else if (state->gain_dir < 0 && state->gain_pos < segment)
	{
		segment = state->gain_pos;
	}
	if (preview_dir > 0 && state->crossfade_frames - state->preview_pos < segment)
	{
		segment = state->crossfade_frames - state->preview_pos;
	}
	else if (preview_dir < 0 && state->preview_pos < segment)
	{
		segment = state->preview_pos;
	}
	if (state->iir_crossfading && state->crossfade_frames - state->iir_crossfade_pos < segment)
	{
		segment = state->crossfade_frames - state->iir_crossfade_pos;
	}
	if (segment < 1)
	{
		segment = 1; // defensive: settled ramps never leave 0 frames
	}

	for (n = 0; n < segment; n++)
	{
		float *frame = &fp_buffer[(size_t)n * (size_t)i_num_channels];
		float filtered[FILT_BRICKWALL_MAX_CHANNELS];
		float removed[FILT_BRICKWALL_MAX_CHANNELS];
		float fir_frame[FILT_PART_CONV_MAX_CHANNELS];
		float fir_dry[FILT_PART_CONV_MAX_CHANNELS];
		float weights[3] = { 0.0f, 0.0f, 0.0f }; // per BRICKWALL_SOURCE_*
		float current_weight, fir_input_gain = 1.0f;
		float preview_mix, iir_incoming_weight;

		if (crossfading)
		{
			state->crossfade_pos++;
			current_weight = brickwallRampWeight(state->crossfade_pos, state->crossfade_frames, crossfade_step);
			weights[state->crossfade_from] = 1.0f - current_weight;
		}
		else
		{
			state->gain_pos += state->gain_dir;
			current_weight = brickwallRampWeight(state->gain_pos, state->fade_frames, fade_step);
		}
		if (fir_fading_in)
		{
			fir_input_gain = current_weight;
			current_weight = 1.0f;
		}
		weights[state->source] += current_weight;

		state->preview_pos += preview_dir;
		preview_mix = brickwallRampWeight(state->preview_pos, state->crossfade_frames, crossfade_step);
		if (state->iir_crossfading)
		{
			state->iir_crossfade_pos++;
		}
		iir_incoming_weight = brickwallRampWeight(state->iir_crossfade_pos, state->crossfade_frames, crossfade_step);

		if (run_fir)
		{
			for (channel = 0; channel < fir_channels; channel++)
			{
				fir_frame[channel] = (channel < i_processed_channels) ? frame[channel] * fir_input_gain : 0.0f;
			}
			filtPartConvProcessFrame(sp_engine, fir_frame, fir_dry);
		}

		for (channel = 0; channel < i_processed_channels; channel++)
		{
			float input = frame[channel];
			float out_filtered = weights[BRICKWALL_SOURCE_DRY] * input;
			float out_removed = 0.0f; // Dry removes nothing

			if (run_iir)
			{
				float iir_filtered, iir_removed;

				brickwallIirRunChannel(state, channel, (realtype)input, iir_incoming_weight, &iir_filtered, &iir_removed);
				out_filtered += weights[BRICKWALL_SOURCE_IIR] * iir_filtered;
				out_removed += weights[BRICKWALL_SOURCE_IIR] * iir_removed;
			}
			if (channel < fir_channels)
			{
				out_filtered += weights[BRICKWALL_SOURCE_FIR] * fir_frame[channel];
				out_removed += weights[BRICKWALL_SOURCE_FIR] * (fir_dry[channel] - fir_frame[channel]);
			}
			filtered[channel] = out_filtered;
			removed[channel] = out_removed;
		}

		if (state->preview_pos > 0)
		{
			brickwallLevelPreviewFrame(removed, i_processed_channels, &state->preview_level_coeffs,
				&state->preview_mean_square, &state->preview_gain);
			for (channel = 0; channel < i_processed_channels; channel++)
			{
				frame[channel] = filtered[channel] + preview_mix * (removed[channel] - filtered[channel]);
			}
		}
		else
		{
			for (channel = 0; channel < i_processed_channels; channel++)
			{
				frame[channel] = filtered[channel];
			}
		}
	}

	brickwallSettleRamps(state);
	return segment;
}

/*
 * FUNCTION: brickwallProcessBuffer()
 * DESCRIPTION:
 *   Runs the output stage over one interleaved buffer: adopts a new IIR set
 *   if one is waiting, asks desired_source() (which returns a
 *   BRICKWALL_SOURCE_* and may adopt a pending Linear Phase engine - it must
 *   only do so while the current source isn't FIR) what should be playing,
 *   returns at once if the stage is idle, and otherwise processes the buffer
 *   segment by segment, stepping the state machine between segments.
 *   *spp_engine is read when the source is FIR. Restarts the preview's
 *   auto-leveller from 0dB when the preview is switched on from fully off.
 *   Real-time safe.
 */
template <typename DesiredSourceFn>
static void brickwallProcessBuffer(BrickwallAudioState *state, BrickwallIirExchange *exchange, int sample_rate,
	unsigned int reset_serial, FiltPartConv *const *spp_engine, float *fp_buffer, int i_num_frames, int i_num_channels,
	bool b_preview_on, DesiredSourceFn desired_source)
{
	const int processed_channels = (i_num_channels < FILT_BRICKWALL_MAX_CHANNELS) ? i_num_channels : FILT_BRICKWALL_MAX_CHANNELS;
	int frame = 0;
	int desired;

	if (i_num_channels <= 0 || i_num_frames <= 0)
	{
		return;
	}

	brickwallAudioStateSetRate(state, sample_rate);
	brickwallIirPoll(state, exchange, sample_rate, reset_serial);
	desired = desired_source();

	if (state->source == BRICKWALL_SOURCE_DRY && desired == BRICKWALL_SOURCE_DRY && state->crossfade_from < 0 &&
		state->gain_pos >= state->fade_frames && state->preview_pos == 0 && !b_preview_on)
	{
		brickwallIirStop(state);
		state->gain_dir = 0;
		state->preview_was_on = false;
		return;
	}

	if (b_preview_on && !state->preview_was_on && state->preview_pos == 0)
	{
		state->preview_mean_square = 0.0f;
		state->preview_gain = 1.0f;
	}
	state->preview_was_on = b_preview_on;

	while (frame < i_num_frames)
	{
		int switches = 0;

		if (frame > 0)
		{
			brickwallIirPoll(state, exchange, sample_rate, reset_serial);
			desired = desired_source();
		}
		while (brickwallStepSource(state, desired) && ++switches < 3)
		{
			desired = desired_source();
		}

		frame += brickwallRunSegment(state, (state->source == BRICKWALL_SOURCE_FIR) ? *spp_engine : NULL,
			&fp_buffer[(size_t)frame * (size_t)i_num_channels], i_num_frames - frame, i_num_channels,
			processed_channels, b_preview_on);
	}
}

/*
 * FUNCTION: debugVerifyBrickwallFilterResponse()
 * DESCRIPTION:
 *   Debug-only runtime self-check of the brickwall filter's frequency response,
 *   in place of a unit test (this codebase has no test framework under dsp/).
 *   Asserts (fails loudly in Debug builds), for every (sample rate, steepness)
 *   combination of parameter sets from brickwallDesignIirParams(), that:
 *     - the passband (1kHz and 20kHz) is essentially untouched by the filter -
 *       it is a low cut only, with no high cut;
 *     - the cascade's actual response at the 20Hz target cutoff is the
 *       intended -3.0103dB, confirming filtBrickwallCalibrateCascadeCutoff()
 *       (FiltBrickwall.cpp) correctly compensates for the cascade-shift effect;
 *     - the preview's complementary low-pass is -3dB at the cutoff too, and
 *       content well above the cutoff doesn't leak into "what's removed".
 *   Then, in the time domain, that filtBrickwallProcessSample()'s double-
 *   precision sections really implement the designed response: a 30Hz tone
 *   through the 20Hz Standard cascade comes out of both bands at the level the
 *   analytic response predicts, and at the hardest case for precision (5Hz
 *   cutoff at 96kHz, Ultra Steep: poles closest to z = 1) a 1kHz tone passes
 *   the high-pass at full level and stays out of the preview's low-pass.
 */
static void debugVerifyBrickwallFilterResponse()
{
	const double sample_rates[] = { 44100.0, 48000.0, 96000.0 };
	const DfxDsp::BrickwallSteepness steepness_values[] = {
		DfxDsp::BrickwallSteepness::Gentle,
		DfxDsp::BrickwallSteepness::Standard,
		DfxDsp::BrickwallSteepness::Steep,
		DfxDsp::BrickwallSteepness::UltraSteep
	};
	size_t rate_index, steepness_index;

	for (rate_index = 0; rate_index < sizeof(sample_rates) / sizeof(sample_rates[0]); rate_index++)
	{
		double sample_rate = sample_rates[rate_index];

		for (steepness_index = 0; steepness_index < sizeof(steepness_values) / sizeof(steepness_values[0]); steepness_index++)
		{
			int num_sections = brickwallNumSectionsForSteepness(steepness_values[steepness_index]);
			BrickwallIirParams params;

			brickwallDesignIirParams((int)sample_rate, steepness_values[steepness_index], (float)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, 7u, &params);
			assert(params.num_hp_sections == num_sections && params.num_lp_sections == num_sections);
			assert(params.sample_rate == (int)sample_rate && params.reset_serial == 7u);

			// Low cut only: the high-pass band alone (0 low-pass sections).
			double passband_db = filtBrickwallCalcResponseDb(1000.0, sample_rate, num_sections, 0, &params.hp_coeffs, &params.lp_coeffs);
			double top_of_band_db = filtBrickwallCalcResponseDb(20000.0, sample_rate, num_sections, 0, &params.hp_coeffs, &params.lp_coeffs);
			double response_at_hp_target_db = filtBrickwallCalcResponseDb(DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, sample_rate, num_sections, 0, &params.hp_coeffs, &params.lp_coeffs);

			// With double-precision coefficients and response evaluation, the
			// calibration lands on -3.0103dB to well under 0.001dB (the float32
			// version needed a 1.5dB tolerance here).
			assert(passband_db > -0.1);
			assert(top_of_band_db > -0.1);
			assert(fabs(response_at_hp_target_db - (-3.0103)) < 0.01);

			// The preview's complementary low-pass band alone.
			double preview_at_cutoff_db = filtBrickwallCalcResponseDb(DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, sample_rate, 0, num_sections, &params.hp_coeffs, &params.lp_coeffs);
			double preview_at_1khz_db = filtBrickwallCalcResponseDb(1000.0, sample_rate, 0, num_sections, &params.hp_coeffs, &params.lp_coeffs);

			assert(fabs(preview_at_cutoff_db - (-3.0103)) < 0.01);
			assert(preview_at_1khz_db < -60.0);
		}
	}

	{
		struct ToneCase {
			int sample_rate;
			DfxDsp::BrickwallSteepness steepness;
			float cutoff_hz;
			double tone_hz;
		};
		const ToneCase cases[] = {
			{ 48000, DfxDsp::BrickwallSteepness::Standard, 20.0f, 30.0 },
			{ 96000, DfxDsp::BrickwallSteepness::UltraSteep, 5.0f, 1000.0 }
		};
		size_t case_index;

		for (case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index++)
		{
			const ToneCase& tone = cases[case_index];
			// 2s, measured over the last 0.5s (a whole number of cycles of both
			// tones), once the filters' start-up transient has died away.
			const int num_frames = 2 * tone.sample_rate;
			const int measure_from = num_frames - tone.sample_rate / 2;
			BrickwallIirParams params;
			FiltBrickwallChannelState state;
			double input_sum = 0.0, filtered_sum = 0.0, removed_sum = 0.0;
			int n;

			brickwallDesignIirParams(tone.sample_rate, tone.steepness, tone.cutoff_hz, 0u, &params);
			filtBrickwallResetChannelState(&state);
			for (n = 0; n < num_frames; n++)
			{
				const double two_pi = 6.283185307179586;
				realtype input = (realtype)(0.5 * sin(two_pi * tone.tone_hz * (double)n / (double)tone.sample_rate));
				realtype filtered = filtBrickwallProcessSample(input, params.num_hp_sections, 0, &params.hp_coeffs, &params.lp_coeffs, &state);
				realtype removed = filtBrickwallProcessSample(input, 0, params.num_lp_sections, &params.hp_coeffs, &params.lp_coeffs, &state);

				if (n >= measure_from)
				{
					input_sum += (double)input * (double)input;
					filtered_sum += (double)filtered * (double)filtered;
					removed_sum += (double)removed * (double)removed;
				}
			}

			double filtered_db = 10.0 * log10(filtered_sum / input_sum);
			double removed_db = 10.0 * log10((removed_sum + 1e-30) / input_sum);
			double expected_filtered_db = filtBrickwallCalcResponseDb(tone.tone_hz, (double)tone.sample_rate,
				params.num_hp_sections, 0, &params.hp_coeffs, &params.lp_coeffs);
			double expected_removed_db = filtBrickwallCalcResponseDb(tone.tone_hz, (double)tone.sample_rate,
				0, params.num_lp_sections, &params.hp_coeffs, &params.lp_coeffs);

			assert(fabs(filtered_db - expected_filtered_db) < 0.05);
			if (expected_removed_db > -40.0)
			{
				assert(fabs(removed_db - expected_removed_db) < 0.05);
			}
			else
			{
				assert(removed_db < -60.0);
			}
		}
	}
}

/*
 * FUNCTION: debugVerifyBrickwallIirHandoff()
 * DESCRIPTION:
 *   Debug-only self-check of the Zero Latency parameter hand-off: the
 *   triple buffer always yields the newest published set (older unread ones
 *   are skipped, and an adopt with nothing pending keeps the current set), and
 *   brickwallIirNeedsReset() keeps the history for a cutoff-only change but
 *   resets it for a steepness change, a new reset serial, or a skipped buffer.
 *   Single-threaded: it checks the protocol's logic, not its memory ordering.
 */
static void debugVerifyBrickwallIirHandoff()
{
	BrickwallIirExchange exchange;
	BrickwallIirParams first, cutoff_moved, steeper;
	const BrickwallIirParams *adopted;

	brickwallIirExchangeInit(&exchange);
	adopted = brickwallIirExchangeAdopt(&exchange);
	assert(adopted->sample_rate == 0); // nothing designed yet: audio passes through

	brickwallDesignIirParams(48000, DfxDsp::BrickwallSteepness::Standard, 20.0f, 1u, &first);
	brickwallDesignIirParams(48000, DfxDsp::BrickwallSteepness::Standard, 40.0f, 1u, &cutoff_moved);
	brickwallDesignIirParams(48000, DfxDsp::BrickwallSteepness::UltraSteep, 40.0f, 1u, &steeper);

	brickwallIirExchangePublish(&exchange, &first);
	adopted = brickwallIirExchangeAdopt(&exchange);
	assert(adopted->sample_rate == 48000 && adopted->num_hp_sections == first.num_hp_sections);
	assert(adopted->hp_coeffs.a1 == first.hp_coeffs.a1);
	// First use after nothing ran: reset.
	assert(brickwallIirNeedsReset(adopted, 0u, -1, -1, false));

	// Cutoff-only change: same serial and section counts, history kept.
	brickwallIirExchangePublish(&exchange, &cutoff_moved);
	adopted = brickwallIirExchangeAdopt(&exchange);
	assert(adopted->hp_coeffs.a1 == cutoff_moved.hp_coeffs.a1);
	assert(!brickwallIirNeedsReset(adopted, 1u, first.num_hp_sections, first.num_lp_sections, true));

	// Nothing pending: the same set stays adopted.
	assert(brickwallIirExchangeAdopt(&exchange) == adopted);

	// Two publishes before an adopt: only the newest is seen. A steepness
	// change resets the history.
	brickwallIirExchangePublish(&exchange, &first);
	brickwallIirExchangePublish(&exchange, &steeper);
	adopted = brickwallIirExchangeAdopt(&exchange);
	assert(adopted->num_hp_sections == steeper.num_hp_sections && adopted->hp_coeffs.a1 == steeper.hp_coeffs.a1);
	assert(brickwallIirNeedsReset(adopted, 1u, cutoff_moved.num_hp_sections, cutoff_moved.num_lp_sections, true));

	// A newer reset serial (filter/power on, format change) resets the history.
	assert(brickwallIirNeedsReset(adopted, 0u, steeper.num_hp_sections, steeper.num_lp_sections, true));
	// So does a buffer the IIR path skipped.
	assert(brickwallIirNeedsReset(adopted, 1u, steeper.num_hp_sections, steeper.num_lp_sections, false));
	assert(!brickwallIirNeedsReset(adopted, 1u, steeper.num_hp_sections, steeper.num_lp_sections, true));

	// A 0Hz cutoff designs no sections, and re-enabling the band resets.
	BrickwallIirParams bypassed;
	brickwallDesignIirParams(48000, DfxDsp::BrickwallSteepness::Standard, 0.0f, 1u, &bypassed);
	assert(bypassed.num_hp_sections == 0 && bypassed.num_lp_sections == 0 && bypassed.sample_rate == 48000);
	assert(brickwallIirNeedsReset(&first, 1u, bypassed.num_hp_sections, bypassed.num_lp_sections, true));
}

/*
 * FUNCTION: debugVerifyBrickwallKaiserResponse()
 * DESCRIPTION:
 *   Debug-only self-check of the Kaiser-window linear-phase design for every
 *   latency mode at 44.1/48/96kHz: odd tap count, a flat passband all the way
 *   up to 20kHz (there is no high cut), and a steep low edge in the longer
 *   modes. Low's passband is checked from 200Hz because its ~185Hz-wide
 *   transition band, centred on the 20Hz cutoff, reaches ~115Hz; Low gets its
 *   own honest bounds instead (see below).
 */
static void debugVerifyBrickwallKaiserResponse()
{
	const double sample_rates[] = { 44100.0, 48000.0, 96000.0 };
	const double passband_freqs[] = { 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 15000.0, 20000.0 };
	size_t rate_index, freq_index;
	int mode;

	for (rate_index = 0; rate_index < sizeof(sample_rates) / sizeof(sample_rates[0]); rate_index++)
	{
		double sample_rate = sample_rates[rate_index];

		for (mode = DfxDsp::BrickwallLinearPhaseLatency::Low; mode <= DfxDsp::BrickwallLinearPhaseLatency::Max; mode++)
		{
			int num_taps = filtBrickwallFirNumTaps((realtype)brickwallFirLatencyMsForMode(mode), (realtype)sample_rate, brickwallFirBlockSizeForMode(mode));
			std::vector<realtype> coeffs(num_taps);

			filtBrickwallFirDesignKaiser(
				(realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ,
				(realtype)sample_rate, num_taps, coeffs.data());

			assert(num_taps > 0 && (num_taps % 2) == 1);

			for (freq_index = 0; freq_index < sizeof(passband_freqs) / sizeof(passband_freqs[0]); freq_index++)
			{
				double passband_db = filtBrickwallFirCalcResponseDb((realtype)passband_freqs[freq_index], (realtype)sample_rate, coeffs.data(), num_taps);
				assert(fabs(passband_db) < 0.1);
			}
			if (mode != DfxDsp::BrickwallLinearPhaseLatency::Low)
			{
				assert(fabs(filtBrickwallFirCalcResponseDb((realtype)100.0, (realtype)sample_rate, coeffs.data(), num_taps)) < 0.1);
			}

			double at_half_cutoff_db = filtBrickwallFirCalcResponseDb((realtype)(DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ / 2.0), (realtype)sample_rate, coeffs.data(), num_taps);
			if (mode == DfxDsp::BrickwallLinearPhaseLatency::High || mode == DfxDsp::BrickwallLinearPhaseLatency::Max)
			{
				assert(at_half_cutoff_db < -90.0);
			}
			else if (mode == DfxDsp::BrickwallLinearPhaseLatency::Medium)
			{
				assert(at_half_cutoff_db < -60.0);
			}
			else
			{
				// Low (20ms) can't cut steeply this low: its transition band is
				// far wider than the cutoff (physics, not a design flaw). Honest
				// bounds at the 20Hz default, matching its tooltip: a soft cut
				// (about -5.6 to -6.2dB at 10Hz), a slight bass loss just above
				// the cutoff (about -2.2 to -2.3dB at 40Hz), and an untouched
				// midrange.
				double at_40hz_db = filtBrickwallFirCalcResponseDb((realtype)40.0, (realtype)sample_rate, coeffs.data(), num_taps);
				double at_1khz_db = filtBrickwallFirCalcResponseDb((realtype)1000.0, (realtype)sample_rate, coeffs.data(), num_taps);

				assert(at_half_cutoff_db < -5.0);
				assert(at_40hz_db < -1.5 && at_40hz_db > -3.0);
				assert(fabs(at_1khz_db) < 0.1);
			}
		}
	}
}

/*
 * FUNCTION: debugVerifyPartConvEquivalence()
 * DESCRIPTION:
 *   Debug-only self-check that the FFT partitioned convolver produces the same
 *   output as direct convolution (delayed by its block size), for both
 *   partition sizes in use, across a mid-stream filter swap, and that its dry
 *   output is the input delayed by exactly the reported latency. Inputs and
 *   coefficients come from a fixed LCG, so the check is deterministic.
 */
static void debugVerifyPartConvEquivalence()
{
	const int block_sizes[] = { 128, 512 };
	const int num_taps = 3001;
	const int num_frames = 8000;
	const int swap_frame = 4000;
	const int num_channels = 2;
	unsigned int seed = 12345u;
	std::vector<realtype> h1(num_taps), h2(num_taps);
	std::vector<float> x((size_t)num_frames * num_channels);
	size_t block_index;
	int i;

	auto next_random = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
		};

	for (i = 0; i < num_taps; i++)
	{
		h1[i] = (realtype)(0.01f * next_random());
		h2[i] = (realtype)(0.01f * next_random());
	}
	for (i = 0; i < num_frames * num_channels; i++)
	{
		x[i] = next_random();
	}

	for (block_index = 0; block_index < sizeof(block_sizes) / sizeof(block_sizes[0]); block_index++)
	{
		int block_size = block_sizes[block_index];
		FiltPartConv *conv = filtPartConvCreate(block_size, num_taps, num_channels, h1.data());
		int latency = 0;
		int n, ch, k;

		assert(conv != NULL);
		latency = filtPartConvGetLatencySamples(conv);
		assert(latency == (num_taps - 1) / 2 + block_size);

		for (n = 0; n < num_frames; n++)
		{
			float frame[2];
			float dry[2];
			int m = n - block_size;

			if (n == swap_frame)
			{
				filtPartConvPublishFilter(conv, h2.data(), num_taps);
			}

			frame[0] = x[(size_t)n * num_channels];
			frame[1] = x[(size_t)n * num_channels + 1];
			filtPartConvProcessFrame(conv, frame, dry);

			for (ch = 0; ch < num_channels; ch++)
			{
				float expected_dry = (n - latency >= 0) ? x[(size_t)(n - latency) * num_channels + ch] : 0.0f;
				assert(dry[ch] == expected_dry);

				if (m < 0)
				{
					assert(fabsf(frame[ch]) < 1e-6f);
				}
				else if (n < swap_frame || n >= swap_frame + 2 * block_size)
				{
					const std::vector<realtype>& h = (n < swap_frame) ? h1 : h2;
					double expected = 0.0;

					for (k = 0; k < num_taps && k <= m; k++)
					{
						expected += (double)h[k] * (double)x[(size_t)(m - k) * num_channels + ch];
					}
					assert(fabs((double)frame[ch] - expected) < 1e-4);
				}
			}
		}

		filtPartConvDestroy(conv);
	}
}

/*
 * FUNCTION: debugVerifyPartConvLatency()
 * DESCRIPTION:
 *   Debug-only self-check that, for every latency mode at 44.1/48/96kHz, the
 *   engine's reported latency equals the mode's nominal latency in samples,
 *   and that an impulse's filtered peak (the FIR's centre tap) comes out
 *   exactly that many frames later.
 */
static void debugVerifyPartConvLatency()
{
	const double sample_rates[] = { 44100.0, 48000.0, 96000.0 };
	size_t rate_index;
	int mode;

	for (rate_index = 0; rate_index < sizeof(sample_rates) / sizeof(sample_rates[0]); rate_index++)
	{
		double sample_rate = sample_rates[rate_index];

		for (mode = DfxDsp::BrickwallLinearPhaseLatency::Low; mode <= DfxDsp::BrickwallLinearPhaseLatency::Max; mode++)
		{
			double latency_ms = brickwallFirLatencyMsForMode(mode);
			int block_size = brickwallFirBlockSizeForMode(mode);
			int num_taps = filtBrickwallFirNumTaps((realtype)latency_ms, (realtype)sample_rate, block_size);
			int expected_latency = (int)floor(latency_ms * sample_rate / 1000.0 + 0.5);
			std::vector<realtype> coeffs(num_taps);
			FiltPartConv *conv;
			int peak_index = -1;
			float peak = 0.0f;
			int n;

			filtBrickwallFirDesignKaiser(
				(realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ,
				(realtype)sample_rate, num_taps, coeffs.data());
			conv = filtPartConvCreate(block_size, num_taps, 2, coeffs.data());
			assert(conv != NULL);
			assert(filtPartConvGetLatencySamples(conv) == expected_latency);

			for (n = 0; n < expected_latency + 2 * block_size; n++)
			{
				float frame[2] = { (n == 0) ? 1.0f : 0.0f, 0.0f };

				filtPartConvProcessFrame(conv, frame, NULL);
				if (fabsf(frame[0]) > peak)
				{
					peak = fabsf(frame[0]);
					peak_index = n;
				}
			}
			assert(peak_index == expected_latency);

			filtPartConvDestroy(conv);
		}
	}
}

/*
 * FUNCTION: debugVerifyBrickwallPreviewLeveller()
 * DESCRIPTION:
 *   Debug-only self-check of the preview auto-leveller at 48kHz stereo:
 *   a quiet 30Hz tone (-43dBFS RMS) is raised to roughly the target level
 *   within 3 seconds; a very loud one (+7dBFS peak) never leaves the soft
 *   limiter's range (never above 1.0); and digital silence stays silent.
 */
static void debugVerifyBrickwallPreviewLeveller()
{
	const int sample_rate = 48000;
	const int num_frames = 3 * sample_rate;
	const int measure_from = num_frames - sample_rate / 2;
	BrickwallPreviewLevelCoeffs coeffs = brickwallPreviewLevelCoeffs(sample_rate);
	const float amplitudes[] = { 0.01f, 3.0f, 0.0f };
	size_t case_index;

	for (case_index = 0; case_index < sizeof(amplitudes) / sizeof(amplitudes[0]); case_index++)
	{
		float amplitude = amplitudes[case_index];
		float mean_square = 0.0f;
		float gain = 1.0f;
		double measured_sum = 0.0;
		float max_magnitude = 0.0f;
		int n;

		for (n = 0; n < num_frames; n++)
		{
			const double two_pi = 6.283185307179586;
			float value = amplitude * (float)sin(two_pi * 30.0 * (double)n / (double)sample_rate);
			float frame[2] = { value, value };

			brickwallLevelPreviewFrame(frame, 2, &coeffs, &mean_square, &gain);
			if (fabsf(frame[0]) > max_magnitude)
			{
				max_magnitude = fabsf(frame[0]);
			}
			if (n >= measure_from)
			{
				measured_sum += (double)frame[0] * (double)frame[0];
			}
		}

		double measured_rms = sqrt(measured_sum / (double)(num_frames - measure_from));

		assert(max_magnitude <= 1.0f);
		if (amplitude == 0.0f)
		{
			assert(max_magnitude == 0.0f);
		}
		else if (amplitude < 0.1f)
		{
			assert(measured_rms > 0.07 && measured_rms < 0.14);
		}
	}
}

DfxDspPrivate::DfxDspPrivate()
{
	dfxp_handle_ = NULL;
	slout1_ = NULL;
	midi_to_rval_qnt_handle_ = NULL;
	rval_to_midi_qnt_handle_ = NULL;

	swprintf(product_specific_.wcp_registry_product_name, PT_MAX_GENERIC_STRLEN, L"%s", DFXG_REGISTRY_DFX_PRODUCT_NAME_WIDE);
	swprintf(product_specific_.wcp_displayed_product_name, PT_MAX_GENERIC_STRLEN, L"%s", DFXG_DISPLAYED_DFX_PRODUCT_NAME_WIDE);
	product_specific_.full_version = static_cast<float>(13.028);
	product_specific_.major_version = static_cast<int>(13.028);
	vendor_specific_.vendor_code = DFXP_VENDOR_CODE_UNIVERSAL;


	// Initialize the processing mode
	if (dfxpInit(&dfxp_handle_,
		L"DFX", 23,
		14, 1, IS_FALSE,
		IS_FALSE, 0,
		IS_FALSE,
		IS_FALSE,
		IS_FALSE, slout1_) != OKAY)
	{
		//return(NOT_OKAY);
		MessageBox(NULL, L"TTEST", L"TEST", MB_OK);
	}

	// Make sure the vocal reduction is turned off 
	if (dfxpSetButtonValue(dfxp_handle_, DFX_UI_BUTTON_VOCAL_REDUCTION_ON, IS_FALSE) != OKAY)
	{
		//return(NOT_OKAY);
	}

	// From dfxg_InitStaticQnts() in dfxgQnt.cpp
	/*
	* Initalize the qnt handle for calculating the real value based
	* on a MIDI value.
	*/
	if (qntIToRInit(&(midi_to_rval_qnt_handle_), slout1_,
		MIDI_MIN_VALUE,
		MIDI_MAX_VALUE,
		DFX_UI_MIN_VALUE, DFX_UI_MAX_VALUE,
		IS_FALSE, 0,
		IS_FALSE, (realtype)0.0,
		IS_FALSE, QNT_RESPONSE_LINEAR) != OKAY)
	{
	}

	/*
	* Initialize the qnt handle for calculating the midi value
	* based on the real value.
	*/
	if (qntRToIInit(&(rval_to_midi_qnt_handle_), slout1_,
		DFX_UI_MIN_VALUE, DFX_UI_MAX_VALUE,
		MIDI_MIN_VALUE,
		MIDI_MAX_VALUE,
		IS_FALSE, 0,
		IS_FALSE) != OKAY)
	{
	}

	// No IIR parameter set exists until the designer thread publishes its
	// first one (it designs as soon as it starts), so until then the Zero
	// Latency path passes audio through.
	brickwall_cached_sample_rate_ = 44100;
	brickwall_cached_num_channels_ = 2;
	brickwallIirExchangeInit(&brickwall_iir_exchange_);
	for (int channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
	{
		filtBrickwallResetChannelState(&brickwall_channel_states_[channel]);
	}

#ifdef _DEBUG
	{
		// Setting FXSOUND_SKIP_DSP_SELFCHECKS=1 skips the self-checks (they take
		// a noticeable moment at every start of a Debug build). They run by
		// default.
		char skip_value[4] = {};
		DWORD skip_length = GetEnvironmentVariableA("FXSOUND_SKIP_DSP_SELFCHECKS", skip_value, sizeof(skip_value));

		if (!(skip_length == 1 && skip_value[0] == '1'))
		{
			debugVerifyBrickwallFilterResponse();
			debugVerifyBrickwallIirHandoff();
			debugVerifyBrickwallKaiserResponse();
			debugVerifyPartConvEquivalence();
			debugVerifyPartConvLatency();
			debugVerifyBrickwallPreviewLeveller();
		}
	}
#endif

	// Started last, once every member it reads is initialised.
	brickwall_designer_thread_ = std::thread(&DfxDspPrivate::brickwallDesignerThreadMain, this);

	//return(OKAY);
}


DfxDspPrivate::~DfxDspPrivate()
{
	// The audio thread is expected to have been stopped by AudioPassthru
	// before DfxDsp is destroyed. being_destroyed_ only narrows the window if
	// it hasn't: DfxDsp::processAudio()/setSignalFormat() stop calling in once
	// they see it, but a call already inside this object is not waited for, so
	// this is no guarantee.
	being_destroyed_ = true;

	// Stop the designer before freeing engines it may still be building or
	// publishing to. The stop flag is set under the mutex, so the designer
	// either sees it before waiting or is already waiting and is woken.
	{
		std::lock_guard<std::mutex> lock(brickwall_designer_mutex_);
		brickwall_designer_stop_ = true;
	}
	brickwall_designer_wake_.notify_one();
	if (brickwall_designer_thread_.joinable())
	{
		brickwall_designer_thread_.join();
	}
	filtPartConvDestroy(brickwall_fir_pending_engine_.exchange(nullptr));
	filtPartConvDestroy(brickwall_fir_retired_engine_.exchange(nullptr));
	filtPartConvDestroy(brickwall_fir_active_engine_);
	brickwall_fir_active_engine_ = nullptr;

	// Free the prelst
	if (preset_list_handle_ != NULL)
	{
		if (prelstFreeUp(&(preset_list_handle_)) != OKAY)
		{
		}
	}
	// Free the midi to rval and visa versa qnt handles
	if (midi_to_rval_qnt_handle_ != NULL)
	{
		if (qntFreeUp(&(midi_to_rval_qnt_handle_)) != OKAY)
		{
		}
	}
	if (rval_to_midi_qnt_handle_ != NULL)
	{
		if (qntFreeUp(&(rval_to_midi_qnt_handle_)) != OKAY)
		{
		}
	}

	// Free dfxp handle
	dfxpFreeAll();
	free(dfxp_handle_);
	//*dfxp_handle_ = NULL;

	// Free the slout handle
	if (slout1_ != NULL)
		delete slout1_;
}

/**
dfxg_UpdateFromRegistryAllSettings()
**/
void DfxDspPrivate::processTimer()
{
	bool anything_changed = false;
	int i_eq_changed = IS_FALSE;

	if (update_from_registry_)
	{
		eqUpdateFromRegistry(&i_eq_changed);
		update_from_registry_ = false;
	}

	if (i_eq_changed)
	{
		anything_changed = true;
	}

	/* If any settings have been changed, communicate all the changes to the DSP module */
	// NOTE: I find that without this if condition and call dfxpCOmmunicateAll() repeatedly will mess up the audio.
	if (anything_changed)
	{
		dfxpCommunicateAll(dfxp_handle_);
	}
}

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

	// Called on the audio thread before every buffer, so no filter design and
	// no system calls here: only atomics. The designer thread picks the
	// request up on its next timed wake (it is not notified from here).
	if (i_srate != brickwall_cached_sample_rate_ || i_nch != brickwall_cached_num_channels_)
	{
		// Neither path's history is valid in the new format and must never
		// play again, even if the format changes back (e.g. 44.1k -> 48k ->
		// 44.1k before anything new is adopted). Bumping the reset serial
		// (lock-free) makes the active linear-phase engine and IIR parameter
		// set fail the audio thread's serial check - audio passes through
		// until fresh ones are ready, and the IIR history is then reset - and
		// forces the designer to build a new engine. Bumped before the design
		// request below, as in brickwallFilterOn().
		brickwall_fir_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
		brickwall_cached_sample_rate_ = i_srate;
		brickwall_cached_num_channels_ = i_nch;
		brickwall_fir_request_sample_rate_ = i_srate;
		brickwall_fir_request_num_channels_ = (i_nch < FILT_PART_CONV_MAX_CHANNELS) ? i_nch : FILT_PART_CONV_MAX_CHANNELS;
		requestBrickwallDesign(false);
	}

	return OKAY;
}

/*
 * FUNCTION: DfxDspPrivate::requestBrickwallDesign()
 * DESCRIPTION:
 *   Asks the designer thread for a new IIR parameter set (and, when linear
 *   phase is on, a new linear-phase filter) for the latest settings. Bursts
 *   are coalesced. wake_designer wakes it at once; it must be false on the
 *   audio thread, where the request is picked up on the designer's next timed
 *   wake instead. Taking the mutex between the increment and the notify means
 *   the designer either sees the new generation in its wait predicate or is
 *   already waiting and gets the notification, so no UI request is lost.
 */
void DfxDspPrivate::requestBrickwallDesign(bool wake_designer)
{
	brickwall_fir_request_generation_.fetch_add(1, std::memory_order_acq_rel);

	if (wake_designer)
	{
		{
			std::lock_guard<std::mutex> lock(brickwall_designer_mutex_);
		}
		brickwall_designer_wake_.notify_one();
	}
}

/*
 * FUNCTION: DfxDspPrivate::brickwallDesignerThreadMain()
 * DESCRIPTION:
 *   Background thread that does all filter design work, so neither the UI
 *   thread nor the audio thread (which also runs setSignalFormat()) ever
 *   designs filters or allocates for the audio path. It sleeps until a
 *   request notifies it, or for at most 200ms (requests from the audio thread
 *   don't notify). Each time it frees an engine the audio thread has retired
 *   and, if a newer request is waiting, handles the latest one (bursts are
 *   coalesced):
 *     - Zero Latency: always designs a fresh IIR parameter set for the
 *       requested sample rate, steepness and cutoff, stamped with the reset
 *       serial, and publishes it through brickwall_iir_exchange_ - whether or
 *       not linear phase is on, so a current set is ready the moment the
 *       Zero Latency path runs again.
 *     - Linear phase (only while it is on): a cutoff-only change on an engine
 *       of the same configuration is a filter swap that keeps the audio
 *       history; anything else (sample rate, channels, latency mode, or a
 *       reset serial bumped because filtering resumed) builds a new engine
 *       with empty history.
 *   The reset serial is read after the request generation (every setter
 *   bumps it before the generation), and before the engine is created and
 *   stamped with it, so an engine or set carrying serial S was provably
 *   designed after the serial was bumped to S.
 */
void DfxDspPrivate::brickwallDesignerThreadMain()
{
	unsigned int handled_generation = 0;
	FiltPartConv *latest_engine = nullptr; // newest engine this thread built (pending or active)
	int latest_sample_rate = 0;
	int latest_num_channels = 0;
	int latest_mode = -1;
	unsigned int latest_reset_serial = 0;
	std::vector<realtype> coeffs;
	std::unique_lock<std::mutex> lock(brickwall_designer_mutex_, std::defer_lock);

	while (!brickwall_designer_stop_)
	{
		filtPartConvDestroy(brickwall_fir_retired_engine_.exchange(nullptr, std::memory_order_acq_rel));

		unsigned int generation = brickwall_fir_request_generation_.load(std::memory_order_acquire);
		if (generation != handled_generation)
		{
			handled_generation = generation;

			int sample_rate = brickwall_fir_request_sample_rate_;
			int num_channels = brickwall_fir_request_num_channels_;
			int mode = brickwall_fir_latency_mode_;
			unsigned int reset_serial = brickwall_fir_reset_serial_.load(std::memory_order_acquire);
			float cutoff_hz = brickwall_hp_cutoff_hz_.load();
			BrickwallIirParams iir_params;

			brickwallDesignIirParams(sample_rate, brickwall_filter_steepness_.load(), cutoff_hz, reset_serial, &iir_params);
			brickwallIirExchangePublish(&brickwall_iir_exchange_, &iir_params);

			if (brickwall_linear_phase_on_)
			{
				int block_size = brickwallFirBlockSizeForMode(mode);
				int num_taps = filtBrickwallFirNumTaps((realtype)brickwallFirLatencyMsForMode(mode), (realtype)sample_rate, block_size);

				coeffs.resize(num_taps);
				filtBrickwallFirDesignKaiser(
					(realtype)cutoff_hz,
					(realtype)sample_rate, num_taps, coeffs.data());

				if (latest_engine != nullptr && reset_serial == latest_reset_serial &&
					sample_rate == latest_sample_rate && num_channels == latest_num_channels && mode == latest_mode)
				{
					filtPartConvPublishFilter(latest_engine, coeffs.data(), num_taps);
				}
				else
				{
					FiltPartConv *engine = filtPartConvCreate(block_size, num_taps, num_channels, coeffs.data());

					if (engine != nullptr)
					{
						engine->tag = sample_rate;
						engine->serial = reset_serial;
						// A pending engine the audio thread never adopted is replaced
						// and freed here; it was never seen by the audio thread.
						filtPartConvDestroy(brickwall_fir_pending_engine_.exchange(engine, std::memory_order_acq_rel));
						latest_engine = engine;
						latest_sample_rate = sample_rate;
						latest_num_channels = num_channels;
						latest_mode = mode;
						latest_reset_serial = reset_serial;
					}
				}
			}
		}

		lock.lock();
		brickwall_designer_wake_.wait_for(lock, std::chrono::milliseconds(200), [this, handled_generation]() {
			return brickwall_designer_stop_.load() ||
				brickwall_fir_request_generation_.load(std::memory_order_acquire) != handled_generation;
			});
		lock.unlock();
	}
}

/*
 * FUNCTION: DfxDspPrivate::adoptPendingBrickwallFirEngine()
 * DESCRIPTION:
 *   Audio thread only. Takes a newly built engine, if one is pending, and
 *   hands the previous one back for the designer thread to free. Only adopts
 *   while the retired slot is empty, so no engine is ever overwritten there
 *   and leaked. Lock-free: atomic load/exchange/store only.
 */
void DfxDspPrivate::adoptPendingBrickwallFirEngine()
{
	if (brickwall_fir_retired_engine_.load(std::memory_order_acquire) != nullptr)
	{
		return;
	}

	FiltPartConv *next = brickwall_fir_pending_engine_.exchange(nullptr, std::memory_order_acq_rel);
	if (next == nullptr)
	{
		return;
	}

	brickwall_fir_retired_engine_.store(brickwall_fir_active_engine_, std::memory_order_release);
	brickwall_fir_active_engine_ = next;
	brickwall_fir_active_latency_ms_ = 1000.0 * (double)filtPartConvGetLatencySamples(next) / (double)next->tag;
}

/*
 * FUNCTION: DfxDspPrivate::brickwallDesiredSource()
 * DESCRIPTION:
 *   Audio thread: which source (BRICKWALL_SOURCE_*) the low cut filter's
 *   output should be playing now. Dry when the filter or FxSound's power is
 *   off, or when nothing matching the current format and reset serial is
 *   ready yet. In Linear Phase mode a pending engine is adopted here, but
 *   only while the current source isn't FIR - an engine is never retired
 *   while it is still being played (or faded out). While an engine is
 *   pending, the active one is not wanted any more: the current one fades
 *   out, the new one is adopted while silent and faded in. Lock-free.
 */
int DfxDspPrivate::brickwallDesiredSource(bool filter_on, int processed_channels, unsigned int reset_serial)
{
	if (!filter_on)
	{
		return BRICKWALL_SOURCE_DRY;
	}

	if (brickwall_linear_phase_on_)
	{
		if (brickwall_audio_.source != BRICKWALL_SOURCE_FIR)
		{
			adoptPendingBrickwallFirEngine();
		}

		// The engine must cover exactly the channels processed now: one built
		// for fewer channels would delay only some of them. An engine stamped
		// with an older reset serial holds history from before filtering last
		// resumed (see brickwall_fir_reset_serial_).
		FiltPartConv *engine = brickwall_fir_active_engine_;
		if (engine != nullptr && engine->tag == brickwall_cached_sample_rate_ &&
			engine->num_channels == processed_channels && engine->serial == reset_serial &&
			brickwall_fir_pending_engine_.load(std::memory_order_acquire) == nullptr)
		{
			return BRICKWALL_SOURCE_FIR;
		}
		return BRICKWALL_SOURCE_DRY;
	}

	return brickwallIirReady(&brickwall_audio_, brickwall_cached_sample_rate_, reset_serial) ?
		BRICKWALL_SOURCE_IIR : BRICKWALL_SOURCE_DRY;
}

/*
 * FUNCTION: DfxDspPrivate::applyBrickwallFilter()
 * DESCRIPTION:
 *   Audio thread: applies the low cut filter to one buffer. All transitions
 *   (filter/power on and off, Zero Latency <-> Linear Phase, latency mode,
 *   cutoff, steepness, preview) are ramped - see the transition overview
 *   above brickwallRampWeight() and brickwallProcessBuffer(). When the
 *   filter is off and every fade has finished, this returns at once.
 */
void DfxDspPrivate::applyBrickwallFilter(float *audio_buffer, int num_sample_sets)
{
	const int num_channels = brickwall_cached_num_channels_;
	const int processed_channels = (num_channels < FILT_PART_CONV_MAX_CHANNELS) ? num_channels : FILT_PART_CONV_MAX_CHANNELS;
	// Read the switches before the reset serial: the UI thread bumps the
	// serial before it turns them on (see brickwallFilterOn()).
	const bool filter_on = brickwall_filter_on_ && brickwall_power_on_;
	const bool preview_on = filter_on && brickwall_filter_preview_on_;
	const unsigned int reset_serial = brickwall_fir_reset_serial_.load(std::memory_order_acquire);

	brickwallProcessBuffer(&brickwall_audio_, &brickwall_iir_exchange_, brickwall_cached_sample_rate_, reset_serial,
		&brickwall_fir_active_engine_, audio_buffer, num_sample_sets, num_channels, preview_on,
		[this, filter_on, processed_channels, reset_serial]() {
			return brickwallDesiredSource(filter_on, processed_channels, reset_serial);
		});
}

void DfxDspPrivate::brickwallFilterOn(bool on)
{
	if (on)
	{
		// Neither path is fed while the filter is off, so resuming must not
		// play stale history. Bump the reset serial before the audio thread can
		// see the filter on: it then passes audio through until a linear-phase
		// engine or IIR parameter set stamped with the new serial is ready, and
		// the IIR history is reset on the audio thread before it next runs (see
		// brickwall_fir_reset_serial_ and brickwallIirNeedsReset()).
		if (!brickwall_filter_on_)
		{
			brickwall_fir_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
		}

		brickwall_filter_on_ = true;
		requestBrickwallDesign(true);
	}
	else
	{
		brickwall_filter_on_ = false;
		brickwall_filter_preview_on_ = false;
	}
}

bool DfxDspPrivate::isBrickwallFilterOn()
{
	return brickwall_filter_on_;
}

void DfxDspPrivate::setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness)
{
	int value = static_cast<int>(steepness);

	if (value < DfxDsp::BrickwallSteepness::Gentle)
	{
		value = DfxDsp::BrickwallSteepness::Gentle;
	}
	else if (value > DfxDsp::BrickwallSteepness::UltraSteep)
	{
		value = DfxDsp::BrickwallSteepness::UltraSteep;
	}

	// The designer thread designs the new cascade; the audio thread resets its
	// history when it adopts a set with a different section count.
	brickwall_filter_steepness_ = value;
	requestBrickwallDesign(true);
}

DfxDsp::BrickwallSteepness DfxDspPrivate::getBrickwallFilterSteepness()
{
	return static_cast<DfxDsp::BrickwallSteepness>(brickwall_filter_steepness_.load());
}

void DfxDspPrivate::brickwallFilterPreviewOn(bool on)
{
	brickwall_filter_preview_on_ = on && brickwall_filter_on_;
}

bool DfxDspPrivate::isBrickwallFilterPreviewOn()
{
	return brickwall_filter_preview_on_;
}

void DfxDspPrivate::brickwallFilterLinearPhaseOn(bool on)
{
	// As in brickwallFilterOn(): the engine isn't fed while linear phase is
	// off, so bump the reset serial before the audio thread can see it on.
	// (Turning it off needs nothing: the Zero Latency path resets its own
	// history when it next runs, see brickwallIirNeedsReset().)
	if (on && !brickwall_linear_phase_on_)
	{
		brickwall_fir_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
	}

	brickwall_linear_phase_on_ = on;
	if (on)
	{
		requestBrickwallDesign(true);
	}
}

bool DfxDspPrivate::isBrickwallFilterLinearPhaseOn()
{
	return brickwall_linear_phase_on_;
}

double DfxDspPrivate::getBrickwallFilterLatencyMs()
{
	if (!brickwall_linear_phase_on_)
	{
		return 0.0;
	}

	return brickwall_fir_active_latency_ms_;
}

void DfxDspPrivate::setBrickwallFilterHighPassCutoff(float cutoff_hz)
{
	if (cutoff_hz < DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MIN_HZ)
	{
		cutoff_hz = DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MIN_HZ;
	}
	else if (cutoff_hz > DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MAX_HZ)
	{
		cutoff_hz = DFXG_BRICKWALL_HIGH_PASS_CUTOFF_MAX_HZ;
	}

	// Unlike a steepness change, moving the cutoff keeps the section count and
	// FIR tap count the same, so existing filter history stays valid and is
	// kept - resetting it on every step of a slider drag would cause audible
	// dropouts. The one exception, re-enabling a bypassed (0Hz) high-pass band,
	// changes the IIR section count, so the audio thread resets that history
	// when it adopts the new set (see brickwallIirNeedsReset()).
	brickwall_hp_cutoff_hz_ = cutoff_hz;
	requestBrickwallDesign(true);
}

float DfxDspPrivate::getBrickwallFilterHighPassCutoff()
{
	return brickwall_hp_cutoff_hz_;
}

void DfxDspPrivate::setBrickwallFilterLinearPhaseLatency(DfxDsp::BrickwallLinearPhaseLatency latency)
{
	int mode = static_cast<int>(latency);

	if (mode < DfxDsp::BrickwallLinearPhaseLatency::Low)
	{
		mode = DfxDsp::BrickwallLinearPhaseLatency::Low;
	}
	else if (mode > DfxDsp::BrickwallLinearPhaseLatency::Max)
	{
		mode = DfxDsp::BrickwallLinearPhaseLatency::Max;
	}

	brickwall_fir_latency_mode_ = mode;
	requestBrickwallDesign(true);
}

DfxDsp::BrickwallLinearPhaseLatency DfxDspPrivate::getBrickwallFilterLinearPhaseLatency()
{
	return static_cast<DfxDsp::BrickwallLinearPhaseLatency>(brickwall_fir_latency_mode_.load());
}


void DfxDspPrivate::powerOn(bool on)
{
	if (on)
	{
		if (dfxpSetButtonValue(dfxp_handle_, DFX_UI_BUTTON_BYPASS, 0) != OKAY)
		{
		}
	}
	else
	{
		if (dfxpSetButtonValue(dfxp_handle_, DFX_UI_BUTTON_BYPASS, 1) != OKAY)
		{
		}
	}

	// The low cut filter runs after the effects (see processAudio()), so the
	// effects' bypass above doesn't cover it: bypass it here too. Neither of
	// its paths is fed while power is off, so on resuming, invalidate their
	// history first (bump the reset serial) - before the audio thread can see
	// power on, as in brickwallFilterOn(). The IIR history itself is reset on
	// the audio thread.
	if (on)
	{
		if (!brickwall_power_on_)
		{
			brickwall_fir_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
			brickwall_power_on_ = true;
			requestBrickwallDesign(true);
		}
	}
	else
	{
		brickwall_power_on_ = false;
	}
}

bool DfxDspPrivate::isPowerOn()
{
	int value;
	
	dfxpGetButtonValue(dfxp_handle_, DFX_UI_BUTTON_BYPASS, &value);
	if (value != 0)
	{
		return true;
	}
	else
	{
		return false;
	}
}

float DfxDspPrivate::getEffectValue(DfxDsp::Effect effect)
{
	switch (effect)
	{
	case DfxDsp::Effect::Fidelity:
		return fidelity_.value;

	case DfxDsp::Effect::Ambience:
		return ambience_.value;

	case DfxDsp::Effect::Surround:
		return surround_.value;

	case DfxDsp::Effect::DynamicBoost:
		return dynamic_boost_.value;

	case DfxDsp::Effect::Bass:
		return bass_boost_.value;
	}

	return -1.0f;
}

void DfxDspPrivate::setEffectValue(DfxDsp::Effect effect, float value)
{
	int button;
	int knob;

	switch (effect)
	{
	case DfxDsp::Effect::Fidelity:
		button = DFX_UI_BUTTON_FIDELITY;
		knob = DFX_UI_KNOB_FIDELITY;
		fidelity_.value = (realtype)value / (realtype)10.0;
		break;

	case DfxDsp::Effect::Ambience:
		button = DFX_UI_BUTTON_AMBIENCE;
		knob = DFX_UI_KNOB_AMBIENCE;
		ambience_.value = (realtype)value / (realtype)10.0;
		break;

	case DfxDsp::Effect::Surround:
		button = DFX_UI_BUTTON_SURROUND;
		knob = DFX_UI_KNOB_SURROUND;
		surround_.value = (realtype)value / (realtype)10.0;
		break;

	case DfxDsp::Effect::DynamicBoost:
		button = DFX_UI_BUTTON_DYNAMIC_BOOST;
		knob = DFX_UI_KNOB_DYNAMIC_BOOST;
		dynamic_boost_.value = (realtype)value / (realtype)10.0;
		break;

	case DfxDsp::Effect::Bass:
		button = DFX_UI_BUTTON_BASS_BOOST;
		knob = DFX_UI_KNOB_BASS_BOOST;
		bass_boost_.value = (realtype)value / (realtype)10.0;
		break;

	default:
		return;
	}

	if (value != 0.0)
	{
		dfxpSetButtonValue(dfxp_handle_, button, 1);
	}
	else
	{
		dfxpSetButtonValue(dfxp_handle_, button, 0);
	}

	dfxpSetKnobValue(dfxp_handle_, knob, (realtype)value / (realtype)10.0, false);
}

unsigned long DfxDspPrivate::getTotalAudioProcessedTime()
{
	unsigned long value;

	dfxpGetTotalAudioProcessedTime(dfxp_handle_, &value);

	return value;
}

void DfxDspPrivate::resetTotalAudioProcessedTime()
{
	dfxpSetTotalAudioProcessedTime(dfxp_handle_, 0);
}

/*
 * 
 */
int DfxDspPrivate::dfxpFreeAll()
{
	struct dfxpHdlType *cast_handle;

	cast_handle = (struct dfxpHdlType *)(dfxp_handle_);

	if (cast_handle == NULL)
		return(OKAY);

	/* Free all the midi_to_dsp qnt handles */
	if (cast_handle->midi_to_dsp.fidelity_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.fidelity_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.spaciousness_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.spaciousness_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.ambience_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.ambience_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.dynamic_boost_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.dynamic_boost_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.bass_boost_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.bass_boost_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Fixed Aural Activation Specific */
	if (cast_handle->midi_to_dsp.aural_filter_gain_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.aural_filter_gain_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.aural_filter_a1_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.aural_filter_a1_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.aural_filter_a0_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.aural_filter_a0_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Fixed Reverb Specific */
	if (cast_handle->midi_to_dsp.room_size_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.room_size_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.damping_bandwidth_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.damping_bandwidth_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.rolloff_bandwidth_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.rolloff_bandwidth_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.motion_rate_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.motion_rate_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.motion_depth_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.motion_depth_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.screen_lex_main_knob3_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.screen_lex_main_knob3_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.screen_lex_main_knob4_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.screen_lex_main_knob4_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Fixed Optimizer Specific */
	if (cast_handle->midi_to_dsp.release_time_beta_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.release_time_beta_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.screen_opt_main_knob3_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.screen_opt_main_knob3_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Fixed Widener Specific */
	if (cast_handle->midi_to_dsp.dispersion_delay_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.dispersion_delay_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.wid_filter_gain_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.wid_filter_gain_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.wid_filter_a1_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.wid_filter_a1_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_dsp.wid_filter_a0_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.wid_filter_a0_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Delay specific */
	if (cast_handle->midi_to_dsp.dly_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_dsp.dly_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Free all the other qnt handles */
	if (cast_handle->real_to_midi_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->real_to_midi_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->midi_to_real_qnt_hdl != NULL)
	{
		if (qntFreeUp(&(cast_handle->midi_to_real_qnt_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Free the com handles */
	if (cast_handle->com_hdl_front != NULL)
	{
		if (comFreeUp(&(cast_handle->com_hdl_front)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->com_hdl_rear != NULL)
	{
		if (comFreeUp(&(cast_handle->com_hdl_rear)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->com_hdl_side != NULL)
	{
		if (comFreeUp(&(cast_handle->com_hdl_side)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->com_hdl_center != NULL)
	{
		if (comFreeUp(&(cast_handle->com_hdl_center)) != OKAY)
			return(NOT_OKAY);
	}

	if (cast_handle->com_hdl_subwoofer != NULL)
	{
		if (comFreeUp(&(cast_handle->com_hdl_subwoofer)) != OKAY)
			return(NOT_OKAY);
	}

	// Free EQ handle
	if (cast_handle->eq.graphicEq_hdl != NULL)
	{
		if (GraphicEqFreeUp(&(cast_handle->eq.graphicEq_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	// Free SurroundSyn handle
	if (cast_handle->SurroundSyn_hdl != NULL)
	{
		if (SurroundSynFreeUp(&(cast_handle->SurroundSyn_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Free the spectrum handle */
	if (cast_handle->spectrum.spectrum_hdl != NULL)
	{
		if (spectrumFreeUp(&(cast_handle->spectrum.spectrum_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Free the binauralSyn handle */
	if (cast_handle->BinauralSyn_hdl != NULL)
	{
		if (BinauralSynFreeUp(&(cast_handle->BinauralSyn_hdl)) != OKAY)
			return(NOT_OKAY);
	}

	/* Free shared memory library */
	if (cast_handle->hp_sharedUtil != NULL)
	{
		if (dfxSharedUtilFreeUp(&(cast_handle->hp_sharedUtil)) != OKAY)
			return(NOT_OKAY);
	}

	return(OKAY);
}

void DfxDspPrivate::getSpectrumBandValues(float* rp_band_values, int i_array_size)
{
    dfxpSpectrumGetBandValues(dfxp_handle_, rp_band_values, i_array_size);
}

