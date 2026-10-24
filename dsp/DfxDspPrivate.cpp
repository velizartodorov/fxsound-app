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

	filter_cached_sample_rate_ = 44100;
	filter_cached_num_channels_ = 2;
	filterIirExchangeInit(&filter_iir_exchange_);
	filterAudioStateInit(&filter_audio_);

	filter_designer_thread_ = std::thread(&DfxDspPrivate::filterDesignerThreadMain, this);

	//return(OKAY);
}


DfxDspPrivate::~DfxDspPrivate()
{
	// So the thread/timer will not attempt to use this object while it's being destroyed.
	being_destroyed_ = true;

	{
		std::lock_guard<std::mutex> lock(filter_designer_mutex_);
		filter_designer_stop_ = true;
	}
	filter_designer_wake_.notify_one();
	if (filter_designer_thread_.joinable())
	{
		filter_designer_thread_.join();
	}

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

	applyFilters(reinterpret_cast<float*>(si_output_samples), i_num_sample_sets);

	return OKAY;
}

int DfxDspPrivate::setSignalFormat(int i_bps, int i_nch, int i_srate, int i_valid_bits)
{
	int result = dfxpUniversalSetSignalFormat(dfxp_handle_, i_bps, i_nch, i_srate, i_valid_bits);

	if (i_srate != filter_cached_sample_rate_ || i_nch != filter_cached_num_channels_ || i_bps != filter_cached_bps_)
	{
		filter_format_serial_++;
		filter_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
		filter_cached_sample_rate_ = i_srate;
		filter_cached_num_channels_ = i_nch;
		filter_cached_bps_ = i_bps;
		filter_request_sample_rate_ = i_srate;
		requestFilterDesign(false);
		filter_designer_wake_.notify_one();
	}

	return (result == OKAY) ? OKAY : NOT_OKAY;
}

void DfxDspPrivate::requestFilterDesign(bool wake_designer)
{
	filter_request_generation_.fetch_add(1, std::memory_order_acq_rel);

	if (wake_designer)
	{
		{
			std::lock_guard<std::mutex> lock(filter_designer_mutex_);
		}
		filter_designer_wake_.notify_one();
	}
}

void DfxDspPrivate::filterDesignerThreadMain()
{
	unsigned int handled_generation = 0;
	std::unique_lock<std::mutex> lock(filter_designer_mutex_, std::defer_lock);

	while (!filter_designer_stop_)
	{
		unsigned int generation = filter_request_generation_.load(std::memory_order_acquire);
		if (generation != handled_generation)
		{
			handled_generation = generation;

			int sample_rate = filter_request_sample_rate_;
			unsigned int reset_serial = filter_reset_serial_.load(std::memory_order_acquire);
			FilterIirParams iir_params;

			filterDesignParams(sample_rate, cut_slope_db_.load(), filterSettings(), reset_serial, &iir_params);
			filterIirExchangePublish(&filter_iir_exchange_, &iir_params);
		}

		lock.lock();
		filter_designer_wake_.wait_for(lock, std::chrono::milliseconds(200), [this, handled_generation]() {
			return filter_designer_stop_.load() ||
				filter_request_generation_.load(std::memory_order_acquire) != handled_generation;
			});
		lock.unlock();
	}
}

int DfxDspPrivate::filterDesiredSource(bool filter_on, unsigned int reset_serial)
{
	return filterDesiredSourceFor(&filter_audio_, filter_on, filter_cached_sample_rate_, reset_serial);
}

void DfxDspPrivate::applyFilters(float *audio_buffer, int num_sample_sets)
{
	if (filter_cached_bps_ != 32)
	{
		return;
	}

	const int num_channels = filter_cached_num_channels_;
	const FilterSettings settings = filterSettings();
	const bool power_on = filter_power_on_;
	// Previews play even when the filters are not heard (global or own switch off): what they would remove.
	const int preview_mask = power_on ? filterPreviewMaskFor(filter_preview_.load(), settings) : 0;
	const bool filter_on = power_on && (filterAudibleMaskFor(settings) != 0 || preview_mask != 0);
	const unsigned int reset_serial = filter_reset_serial_.load(std::memory_order_acquire);

	filterProcessBuffer(&filter_audio_, &filter_iir_exchange_, filter_cached_sample_rate_, reset_serial,
		filter_format_serial_, audio_buffer, num_sample_sets, num_channels, preview_mask,
		[this, filter_on, reset_serial]() {
			return filterDesiredSource(filter_on, reset_serial);
		});
}

FilterSettings DfxDspPrivate::filterSettings()
{
	FilterSettings settings;

	settings.filters_on = filters_on_;
	settings.low_on = low_cut_on_;
	settings.low_hz = low_cut_cutoff_hz_;
	settings.high_on = high_cut_on_;
	settings.high_hz = high_cut_cutoff_hz_;
	settings.mono_on = mono_bass_on_;
	settings.mono_hz = mono_bass_cutoff_hz_;
	return settings;
}

// Called on the GUI thread by every filter setter just before it stores its new value, and by setFilterPreview. The
// preview selection is stored here, with flags the new settings make unavailable dropped. When the IIR chain goes from
// not needed (nothing heard, nothing previewed) to needed, the reset serial is bumped before any store, so the audio
// thread sees the new serial no later than it sees the chain needed and starts again from a freshly designed filter.
// Returns true when the serial was bumped; the caller must then request a design.
bool DfxDspPrivate::beginFilterChange(const FilterSettings &next, int requested_preview)
{
	const int current_preview = filter_preview_.load();
	const int next_preview = filterPreviewSelectionFor(requested_preview, next);
	bool bumped = false;

	if (!filterIirNeededFor(current_preview, filterSettings()) && filterIirNeededFor(next_preview, next))
	{
		filter_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
		bumped = true;
	}
	if (next_preview != current_preview)
	{
		filter_preview_ = next_preview;
	}

	return bumped;
}

bool DfxDspPrivate::beginFilterChange(const FilterSettings &next)
{
	return beginFilterChange(next, filter_preview_.load());
}

// The shared part of lowCutOn / highCutOn / monoBassOn. stage_bit is the filter's FILT_CHAIN_CHANGED_* bit, which is
// also its preview flag: switching the filter off also releases its delta.
void DfxDspPrivate::setStageOn(int stage_bit, std::atomic<bool> &stage_on, bool on)
{
	const FilterSettings current = filterSettings();
	const FilterSettings next = filterSettingsWithStage(current, stage_bit, on, filterStageHz(current, stage_bit));

	beginFilterChange(next, on ? filter_preview_.load() : (filter_preview_.load() & ~stage_bit));
	stage_on = on;
	requestFilterDesign(true);
}

// The shared part of the cutoff setters, after each one's own clamp or snap.
void DfxDspPrivate::setStageCutoff(int stage_bit, std::atomic<float> &stage_cutoff, float clamped_hz)
{
	const FilterSettings current = filterSettings();
	const FilterSettings next = filterSettingsWithStage(current, stage_bit, filterStageOn(current, stage_bit), clamped_hz);

	beginFilterChange(next);
	stage_cutoff = clamped_hz;
	requestFilterDesign(true);
}

void DfxDspPrivate::filtersOn(bool on)
{
	FilterSettings next = filterSettings();

	next.filters_on = on;
	beginFilterChange(next);
	filters_on_ = on;
	requestFilterDesign(true);
}

bool DfxDspPrivate::isFiltersOn()
{
	return filters_on_;
}

void DfxDspPrivate::setCutSlope(int db_per_octave)
{
	cut_slope_db_ = filtLowCutSnapSlopeDb(db_per_octave);
	requestFilterDesign(true);
}

int DfxDspPrivate::getCutSlope()
{
	return cut_slope_db_;
}

void DfxDspPrivate::lowCutOn(bool on)
{
	setStageOn(FILT_CHAIN_CHANGED_LOW_CUT, low_cut_on_, on);
}

bool DfxDspPrivate::isLowCutOn()
{
	return low_cut_on_;
}

void DfxDspPrivate::setLowCutCutoff(float hz)
{
	if (hz < (float)DfxDsp::LOW_CUT_MIN_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::LOW_CUT_MIN_CUTOFF_HZ;
	}
	else if (hz > (float)DfxDsp::LOW_CUT_MAX_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::LOW_CUT_MAX_CUTOFF_HZ;
	}

	setStageCutoff(FILT_CHAIN_CHANGED_LOW_CUT, low_cut_cutoff_hz_, hz);
}

float DfxDspPrivate::getLowCutCutoff()
{
	return low_cut_cutoff_hz_;
}

void DfxDspPrivate::highCutOn(bool on)
{
	setStageOn(FILT_CHAIN_CHANGED_HIGH_CUT, high_cut_on_, on);
}

bool DfxDspPrivate::isHighCutOn()
{
	return high_cut_on_;
}

void DfxDspPrivate::setHighCutCutoff(float hz)
{
	if (hz < (float)DfxDsp::HIGH_CUT_MIN_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::HIGH_CUT_MIN_CUTOFF_HZ;
	}
	hz = roundf(hz / (float)DfxDsp::HIGH_CUT_STEP_HZ) * (float)DfxDsp::HIGH_CUT_STEP_HZ;
	if (hz > (float)DfxDsp::HIGH_CUT_MAX_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::HIGH_CUT_OFF_HZ;
	}

	setStageCutoff(FILT_CHAIN_CHANGED_HIGH_CUT, high_cut_cutoff_hz_, hz);
}

float DfxDspPrivate::getHighCutCutoff()
{
	return high_cut_cutoff_hz_;
}

void DfxDspPrivate::monoBassOn(bool on)
{
	setStageOn(FILT_CHAIN_CHANGED_MONO, mono_bass_on_, on);
}

bool DfxDspPrivate::isMonoBassOn()
{
	return mono_bass_on_;
}

void DfxDspPrivate::setMonoBassCutoff(float hz)
{
	if (hz < (float)DfxDsp::MONO_BASS_MIN_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::MONO_BASS_MIN_CUTOFF_HZ;
	}
	else if (hz > (float)DfxDsp::MONO_BASS_MAX_CUTOFF_HZ)
	{
		hz = (float)DfxDsp::MONO_BASS_MAX_CUTOFF_HZ;
	}

	setStageCutoff(FILT_CHAIN_CHANGED_MONO, mono_bass_cutoff_hz_, hz);
}

float DfxDspPrivate::getMonoBassCutoff()
{
	return mono_bass_cutoff_hz_;
}

bool DfxDspPrivate::isFilterPreviewAvailable(int preview_flags)
{
	return filterPreviewAvailableFor(preview_flags, filterSettings());
}

void DfxDspPrivate::setFilterPreview(int preview_flags)
{
	if (beginFilterChange(filterSettings(), preview_flags))
	{
		requestFilterDesign(true);
	}
}

int DfxDspPrivate::getFilterPreview()
{
	return filter_preview_.load();
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

	if (on)
	{
		if (!filter_power_on_)
		{
			filter_reset_serial_.fetch_add(1, std::memory_order_acq_rel);
			filter_power_on_ = true;
			requestFilterDesign(true);
		}
	}
	else
	{
		filter_power_on_ = false;
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

