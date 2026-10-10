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
 *   returns the front slot, which nothing else writes while it is the front.
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

static const BrickwallIirParams *brickwallIirExchangeAdopt(BrickwallIirExchange *exchange)
{
	if (exchange->middle_slot.load(std::memory_order_relaxed) & BRICKWALL_IIR_SLOT_DIRTY)
	{
		int previous = exchange->middle_slot.exchange(exchange->front_slot, std::memory_order_acq_rel);
		exchange->front_slot = previous & BRICKWALL_IIR_SLOT_MASK;
	}

	return &exchange->slots[exchange->front_slot];
}

/*
 * FUNCTION: brickwallIirNeedsReset()
 * DESCRIPTION:
 *   Audio thread: whether the IIR cascade's history must be cleared before
 *   running the parameter set *params. It must when the set belongs to a newer
 *   reset serial (filter on, power on, format change), when the section counts
 *   changed (steepness change, or a 0Hz cutoff bypass toggled), or when the
 *   IIR path skipped the previous buffer (linear phase was on, the filter or
 *   power was off, or no matching set was ready), since its history is then
 *   stale. A cutoff-only change keeps the history, so dragging the cutoff
 *   slider doesn't click.
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

void DfxDspPrivate::applyBrickwallFilter(float *audio_buffer, int num_sample_sets)
{
	int sample_index;
	int channel;

	// Bypassed when the filter is off or FxSound's power is off (see powerOn()).
	if (!brickwall_filter_on_ || !brickwall_power_on_)
	{
		brickwall_iir_ran_last_buffer_ = false;
		return;
	}

	// Read the atomic once per buffer; both paths use it. Each time the
	// preview is switched on, its auto-leveller starts again from 0dB and rises
	// to the listening level (see brickwallLevelPreviewFrame()).
	bool preview_on = brickwall_filter_preview_on_;
	if (preview_on && !brickwall_preview_was_on_)
	{
		brickwall_preview_mean_square_ = 0.0f;
		brickwall_preview_gain_ = 1.0f;
	}
	brickwall_preview_was_on_ = preview_on;
	// The leveller's coefficients only depend on the sample rate.
	if (brickwall_preview_level_coeffs_rate_ != brickwall_cached_sample_rate_)
	{
		brickwall_preview_level_coeffs_ = brickwallPreviewLevelCoeffs(brickwall_cached_sample_rate_);
		brickwall_preview_level_coeffs_rate_ = brickwall_cached_sample_rate_;
	}

	if (brickwall_linear_phase_on_)
	{
		// The Zero Latency path isn't fed while linear phase is on, so its
		// history is stale when it next runs (see brickwallIirNeedsReset()).
		brickwall_iir_ran_last_buffer_ = false;

		// Linear-phase path: FFT partitioned convolution. Engines are built on
		// the designer thread; this thread only adopts a finished one. Until an
		// engine matching the current format is ready, audio passes through
		// unfiltered. An engine stamped with an older reset serial holds history
		// from before filtering last resumed, so it also passes audio through
		// until its fresh replacement is adopted (see
		// brickwall_fir_reset_serial_). filtPartConvProcessFrame() also returns
		// the dry input delayed by the same latency, so the preview residual
		// compares time-aligned samples.
		adoptPendingBrickwallFirEngine();

		// The engine must cover exactly the channels processed now: one built
		// for fewer channels would delay only some of them.
		FiltPartConv *engine = brickwall_fir_active_engine_;
		int processed_channels = (brickwall_cached_num_channels_ < FILT_PART_CONV_MAX_CHANNELS) ?
			brickwall_cached_num_channels_ : FILT_PART_CONV_MAX_CHANNELS;
		if (engine == nullptr || engine->tag != brickwall_cached_sample_rate_ ||
			engine->num_channels != processed_channels ||
			engine->serial != brickwall_fir_reset_serial_.load(std::memory_order_acquire))
		{
			return;
		}

		float dry[FILT_PART_CONV_MAX_CHANNELS];

		for (sample_index = 0; sample_index < num_sample_sets; sample_index++)
		{
			float *frame = &audio_buffer[sample_index * brickwall_cached_num_channels_];

			filtPartConvProcessFrame(engine, frame, preview_on ? dry : NULL);

			if (preview_on)
			{
				for (channel = 0; channel < engine->num_channels; channel++)
				{
					frame[channel] = dry[channel] - frame[channel];
				}
				brickwallLevelPreviewFrame(frame, engine->num_channels, &brickwall_preview_level_coeffs_,
					&brickwall_preview_mean_square_, &brickwall_preview_gain_);
			}
		}

		return;
	}

	{
		// Zero Latency path. Adopt the newest parameter set the designer thread
		// has published, if any (lock-free; see brickwallIirExchangeAdopt()).
		// Until a set designed for the current sample rate and the current
		// reset serial is ready, audio passes through unfiltered - the same
		// rule as the linear-phase path.
		const BrickwallIirParams *params = brickwallIirExchangeAdopt(&brickwall_iir_exchange_);

		if (params->sample_rate != brickwall_cached_sample_rate_ ||
			params->reset_serial != brickwall_fir_reset_serial_.load(std::memory_order_acquire))
		{
			brickwall_iir_ran_last_buffer_ = false;
			return;
		}

		int processed_channels = (brickwall_cached_num_channels_ < FILT_BRICKWALL_MAX_CHANNELS) ?
			brickwall_cached_num_channels_ : FILT_BRICKWALL_MAX_CHANNELS;

		// History is only ever reset here, on the audio thread, so it never
		// races the cascade. A cutoff-only change keeps it (no click while
		// dragging the cutoff slider).
		if (brickwallIirNeedsReset(params, brickwall_iir_history_serial_, brickwall_iir_history_hp_sections_,
			brickwall_iir_history_lp_sections_, brickwall_iir_ran_last_buffer_))
		{
			for (channel = 0; channel < FILT_BRICKWALL_MAX_CHANNELS; channel++)
			{
				filtBrickwallResetChannelState(&brickwall_channel_states_[channel]);
			}
			brickwall_iir_history_serial_ = params->reset_serial;
			brickwall_iir_history_hp_sections_ = params->num_hp_sections;
			brickwall_iir_history_lp_sections_ = params->num_lp_sections;
		}
		brickwall_iir_ran_last_buffer_ = true;

		// The high-pass count is 0 when the user's low cutoff is 0Hz.
		int num_hp_sections = params->num_hp_sections;
		int num_lp_sections = params->num_lp_sections;

		// Defensive clamp: brickwallNumSectionsForSteepness() only ever returns
		// 1/4/8/11, but filtBrickwallProcessSample() does not itself bounds-check
		// its section counts against FILT_BRICKWALL_MAX_SECTIONS before indexing
		// into fixed-size arrays (the library code is generic and doesn't know
		// about steepness policy). Clamp here, in the real-time audio path, as a
		// safety net.
		if (num_hp_sections > FILT_BRICKWALL_MAX_SECTIONS)
		{
			num_hp_sections = FILT_BRICKWALL_MAX_SECTIONS;
		}
		else if (num_hp_sections < 0)
		{
			num_hp_sections = 0;
		}
		if (num_lp_sections > FILT_BRICKWALL_MAX_SECTIONS)
		{
			num_lp_sections = FILT_BRICKWALL_MAX_SECTIONS;
		}
		else if (num_lp_sections < 0)
		{
			num_lp_sections = 0;
		}

		for (sample_index = 0; sample_index < num_sample_sets; sample_index++)
		{
			for (channel = 0; channel < processed_channels; channel++)
			{
				int buffer_index = sample_index * brickwall_cached_num_channels_ + channel;
				realtype pre_filter_sample = (realtype)audio_buffer[buffer_index];

				// The high-pass (the filter) and the complementary low-pass (the
				// preview, see brickwallDesignIirParams()) both run on every
				// sample, using separate section histories, so toggling the
				// preview never starts either one from stale history.
				realtype filtered_sample = filtBrickwallProcessSample(
					pre_filter_sample,
					num_hp_sections,
					0,
					&params->hp_coeffs,
					&params->lp_coeffs,
					&brickwall_channel_states_[channel]);
				realtype removed_sample = filtBrickwallProcessSample(
					pre_filter_sample,
					0,
					num_lp_sections,
					&params->hp_coeffs,
					&params->lp_coeffs,
					&brickwall_channel_states_[channel]);

				if (preview_on)
				{
					audio_buffer[buffer_index] = (float)(num_lp_sections > 0 ? removed_sample : (realtype)0.0);
				}
				else
				{
					audio_buffer[buffer_index] = (float)filtered_sample;
				}
			}

			if (preview_on)
			{
				brickwallLevelPreviewFrame(&audio_buffer[sample_index * brickwall_cached_num_channels_], processed_channels,
					&brickwall_preview_level_coeffs_, &brickwall_preview_mean_square_, &brickwall_preview_gain_);
			}
		}
	}
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

