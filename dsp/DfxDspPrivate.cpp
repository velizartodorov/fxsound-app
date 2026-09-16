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
 *   Asserts (fails loudly in Debug builds), for every (sample rate, steepness)
 *   combination, that:
 *     - the passband (1kHz) is essentially untouched by the filter;
 *     - the cascade's actual response at the 20Hz and 20kHz target cutoffs is
 *       within a small tolerance of the intended -3.0103dB, confirming
 *       filtBrickwallCalibrateCascadeCutoff() (FiltBrickwall.cpp) is correctly
 *       compensating for the cascade-shift effect. An earlier version of this
 *       filter used uncalibrated, fixed per-section design frequencies; real-
 *       world listening found that this let the low-pass cutoff collapse to as
 *       low as ~9-13kHz for Standard/Steep at 44.1/48kHz - a very audible loss
 *       of treble. This assertion is written precisely enough that it would
 *       have caught that regression.
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

		for (steepness_index = 0; steepness_index < sizeof(steepness_values) / sizeof(steepness_values[0]); steepness_index++)
		{
			int num_sections = brickwallNumSectionsForSteepness(steepness_values[steepness_index]);

			double calibrated_hp_cutoff = filtBrickwallCalibrateCascadeCutoff((realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, (realtype)sample_rate, num_sections, 1);
			double calibrated_lp_cutoff = filtBrickwallCalibrateCascadeCutoff((realtype)DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ, (realtype)sample_rate, num_sections, 0);

			FiltBrickwallBiquadCoeffs hp_coeffs;
			FiltBrickwallBiquadCoeffs lp_coeffs;
			filtBrickwallDesignHighPass((realtype)calibrated_hp_cutoff, (realtype)sample_rate, &hp_coeffs);
			filtBrickwallDesignLowPass((realtype)calibrated_lp_cutoff, (realtype)sample_rate, &lp_coeffs);

			double passband_db = filtBrickwallCalcResponseDb((realtype)1000.0, (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);
			double response_at_hp_target_db = filtBrickwallCalcResponseDb((realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);
			double response_at_lp_target_db = filtBrickwallCalcResponseDb((realtype)DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ, (realtype)sample_rate, num_sections, &hp_coeffs, &lp_coeffs);

			// The high-pass target (20Hz) tolerance is wider than the low-pass one:
			// verified numerically (in double precision) that the calibration search
			// itself is exact, but evaluating it through this module's realtype
			// (32-bit float) coefficients and filtPolyCalcBiquadPowerResponse's
			// float-precision trig math loses meaningful accuracy at such a low
			// absolute frequency relative to these sample rates (observed up to
			// ~0.9dB off in float32, worst case fs=96000/Steep) - inaudible at
			// 10-20Hz, but a real float32 precision limit, not a calibration bug.
			// The low-pass target (20kHz) - the actual user-reported regression -
			// stays accurate to within thousandths of a dB even in float32, so its
			// tolerance stays tight.
			assert(passband_db > -0.1);
			assert(fabs(response_at_hp_target_db - (-3.0103)) < 1.5);
			assert(fabs(response_at_lp_target_db - (-3.0103)) < 0.2);
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
{
	// So the thread/timer will not attempt to use this object while it's being destroyed.
	being_destroyed_ = true;
	
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
	// Coefficients depend on the current section count (cascading shifts the
	// cascade's actual -3dB point away from a single section's own cutoff), so
	// this must be recomputed whenever num_sections changes too, not just the
	// sample rate - see setBrickwallFilterSteepness() below.
	int num_sections = brickwallNumSectionsForSteepness(brickwall_filter_steepness_);

	double calibrated_hp_cutoff = filtBrickwallCalibrateCascadeCutoff((realtype)DFXG_BRICKWALL_HIGH_PASS_CUTOFF_HZ, (realtype)brickwall_cached_sample_rate_, num_sections, 1);
	double calibrated_lp_cutoff = filtBrickwallCalibrateCascadeCutoff((realtype)DFXG_BRICKWALL_LOW_PASS_CUTOFF_HZ, (realtype)brickwall_cached_sample_rate_, num_sections, 0);

	filtBrickwallDesignHighPass((realtype)calibrated_hp_cutoff, (realtype)brickwall_cached_sample_rate_, &brickwall_hp_coeffs_);
	filtBrickwallDesignLowPass((realtype)calibrated_lp_cutoff, (realtype)brickwall_cached_sample_rate_, &brickwall_lp_coeffs_);
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

	// Defensive clamp: brickwallNumSectionsForSteepness() only ever returns
	// 1/4/8, but filtBrickwallProcessSample() does not itself bounds-check
	// i_num_sections against FILT_BRICKWALL_MAX_SECTIONS before indexing into
	// fixed-size arrays (Task 1 library code is generic and doesn't know
	// about steepness policy). Clamp here, in the real-time audio path, as a
	// safety net beyond what the brief's code shows.
	if (num_sections > FILT_BRICKWALL_MAX_SECTIONS)
	{
		num_sections = FILT_BRICKWALL_MAX_SECTIONS;
	}
	else if (num_sections < 0)
	{
		num_sections = 0;
	}

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
	updateBrickwallFilterCoefficients();
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

