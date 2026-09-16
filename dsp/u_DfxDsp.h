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
#pragma once
#include <string>
#include <atomic>
#include <thread>
#include "AudioPassthru.h"
#include "codedefs.h"
#include "DfxDsp.h"
#include "pt_defs.h"
#include "slout.h"
#include "FiltBrickwall.h"
#include "FiltBrickwallFir.h"
#include "FiltPartConv.h"

struct dfxg_section_type {
	realtype value;
	int fader_x_center;
	int bypass;
	PT_HANDLE *fader_loc_to_rval_qnt;
	PT_HANDLE *rval_to_fader_loc_qnt;
};

/*
* Information which is product specific.  In other words,
* this info is different for DFX products vs. MP3 Remix products.
*/
struct dfxg_product_specific_info_type {
	realtype full_version; 	                            /* Full Product version (7.2003) */
	int major_version; 	                               /* Major version number (ex. 7) */
	wchar_t wcp_registry_product_name[PT_MAX_GENERIC_STRLEN];  /* Product Name as used in the registry */
	wchar_t wcp_displayed_product_name[PT_MAX_GENERIC_STRLEN]; /* Product Name as displayed to the user */
};



/*
* Information specific to the particular vendor type.  Most of these settings come
* in as parameters to dfxgInit() however some are inferred by those parameters.
*/
struct dfxg_vendor_specific_info_type {
	int vendor_code;
	int subvendor_code;
	int allow_close;
	int i_freemium_version;
	int display_close_dialog;
	long splash_display_frequency_secs;
	int remix_capabilities;
	int standalone_mode;
	int allow_recording;
	int oem_build;
};

class CDerivedSlout1 : public CSlout {
public:
	int Display(int, char *);
	int Error(int, char *);

	int Display_Wide(int, wchar_t *);
	int Error_Wide(int, wchar_t *);

	PT_HANDLE *hp_dfxg;
};

class DfxDspPrivate
{
public:
	DfxDspPrivate();
	~DfxDspPrivate();
	int processAudio(short int *si_input_samples, short int *si_output_samples, int i_num_sample_sets, int i_check_for_duplicate_buffers);
	int setSignalFormat(int i_bps, int i_nch, int i_srate, int i_valid_bits);
	int loadPreset(std::wstring preset_file_full_path);
	int savePreset(std::wstring preset_name, std::wstring preset_file_full_path);
	int exportPreset(std::wstring preset_source_file_full_path, std::wstring preset_name, std::wstring preset_export_path);
	int resetEQ();
	void eqOn(bool on);
	int getNumEqBands();
	void brickwallFilterOn(bool on);
	bool isBrickwallFilterOn();
	void setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness steepness);
	DfxDsp::BrickwallSteepness getBrickwallFilterSteepness();
	void brickwallFilterPreviewOn(bool on);
	bool isBrickwallFilterPreviewOn();
	void brickwallFilterLinearPhaseOn(bool on);
	bool isBrickwallFilterLinearPhaseOn();
	double getBrickwallFilterLatencyMs();
	void setBrickwallFilterLinearPhaseLatency(DfxDsp::BrickwallLinearPhaseLatency latency);
	DfxDsp::BrickwallLinearPhaseLatency getBrickwallFilterLinearPhaseLatency();
	void setBrickwallFilterHighPassCutoff(float cutoff_hz);
	float getBrickwallFilterHighPassCutoff();
	float getBalance();
	void setBalance(float gain_db);
	float getNormalization();
	void setNormalization(float gain_db);
	float getVolumeLeveling();
	void setVolumeLeveling(float gain_db);
	float getMasterGain();
	void setMasterGain(float gain_db);
	float getFilterQ();
	void setFilterQ(float q_multiplier);
	void setNumBands(int num_bands);
    float getEqBandFrequency(int band_num);
    void setEqBandFrequency(int band_num, float freq);
    void getEqBandFrequencyRange(int band_num, float* min_freq, float* max_freq);
    bool getDefaultEqBandFrequency(int band_num, float* freq);
	float getEqBandBoostCut(int band_num);
	void setEqBandBoostCut(int band_num, float boost);
	void powerOn(bool on);
	bool isPowerOn();
	float getEffectValue(DfxDsp::Effect effect);
	void setEffectValue(DfxDsp::Effect effect, float value);
	DfxPreset getPresetInfo(std::wstring preset_file_full_path);
	unsigned long getTotalAudioProcessedTime();
	void resetTotalAudioProcessedTime();
    void getSpectrumBandValues(float* rp_band_values, int i_array_size);

	bool being_destroyed_ = false;
private:
	void processTimer();
	int eqUpdateFromRegistry(int *ip_eq_changed);
	int getStateInfoFromVals(PT_HANDLE *hp_vals, bool b_include_eq = true);
	int getGraphicEqInfoFromVals(PT_HANDLE *hp_vals);
	int createValsFromStateInfo(wchar_t *preset_name, PT_HANDLE **hpp_vals);

	// DfxDspPrivate.cpp
	int dfxpFreeAll();

	// DfxDspRegistry.cpp
	int writeRegistrySessionLongValue(long l_value, wchar_t *wcp_key_name);

	// DfxDspEq.cpp
	int eqSetProcessingOn(int i_storage_type, int i_on);
	int eqGetProcessingOn(int i_storage_type, int *ip_on);

	// Brickwall filter (dsp/ptutil/include/FiltBrickwall.h)
	void updateBrickwallFilterCoefficients();
	void resetBrickwallFilterState();
	void applyBrickwallFilter(float *audio_buffer, int num_sample_sets);

	// Brickwall filter, linear-phase mode: FFT partitioned convolution
	// (dsp/ptutil/include/FiltPartConv.h), designed on a background thread.
	void requestBrickwallFirDesign();
	void brickwallDesignerThreadMain();
	void adoptPendingBrickwallFirEngine();

	// Handles
	int *dfxp_handle_;
	int *preset_list_handle_;
	int *midi_to_rval_qnt_handle_; // Midi to Real Value
	int *rval_to_midi_qnt_handle_; // and visa versa QNT handles

	CDerivedSlout1 *slout1_;

	// Section specific information
	struct dfxg_section_type fidelity_;
	struct dfxg_section_type ambience_;
	struct dfxg_section_type surround_;
	struct dfxg_section_type dynamic_boost_;
	struct dfxg_section_type bass_boost_;

	bool update_from_registry_ = true;
	int headphone_on_;
	int music_mode_;     /* DFXP_MUSIC_MODE_MUSIC1, DFXP_MUSIC_MODE_MUSIC2, DFXP_MUSIC_MODE_SPEECH */

	struct dfxg_vendor_specific_info_type vendor_specific_;
	struct dfxg_product_specific_info_type product_specific_;

	int eq_processing_on_;

	// Written by the UI thread, read by the audio thread (the reset-serial
	// ordering in brickwallFilterOn() relies on these being atomic).
	std::atomic<bool> brickwall_filter_on_{ false };
	std::atomic<bool> brickwall_filter_preview_on_{ false };
	// FxSound's power state as seen by the low cut filter: powerOn(false)
	// bypasses it along with the effects.
	std::atomic<bool> brickwall_power_on_{ true };
	// Preview auto-leveller state (audio thread only; see
	// brickwallLevelPreviewFrame() in DfxDspPrivate.cpp).
	float brickwall_preview_mean_square_ = 0.0f;
	float brickwall_preview_gain_ = 1.0f;
	bool brickwall_preview_was_on_ = false;
	DfxDsp::BrickwallSteepness brickwall_filter_steepness_ = DfxDsp::BrickwallSteepness::Standard;
	std::atomic<float> brickwall_hp_cutoff_hz_{ 20.0f }; // 0 = high-pass band bypassed; read by the designer thread
	int brickwall_num_hp_sections_ = 0;
	int brickwall_num_lp_sections_ = 0;
	int brickwall_cached_sample_rate_ = 44100;
	int brickwall_cached_num_channels_ = 2;
	FiltBrickwallBiquadCoeffs brickwall_hp_coeffs_;
	FiltBrickwallBiquadCoeffs brickwall_lp_coeffs_ = {}; // complementary low-pass for the preview only (the filter is a low cut)
	FiltBrickwallChannelState brickwall_channel_states_[FILT_BRICKWALL_MAX_CHANNELS];

	// Linear-phase engine hand-off. The UI and audio threads only store and
	// exchange these atomics; brickwall_designer_thread_ does all design and
	// allocation. pending: designer -> audio. retired: audio -> designer
	// (holds at most one engine; the audio thread adopts only when it is
	// empty, so nothing is leaked).
	std::atomic<bool> brickwall_linear_phase_on_{ false };
	std::atomic<int> brickwall_fir_latency_mode_{ DfxDsp::BrickwallLinearPhaseLatency::Low };
	std::atomic<int> brickwall_fir_request_sample_rate_{ 44100 };
	std::atomic<int> brickwall_fir_request_num_channels_{ 2 };
	std::atomic<unsigned int> brickwall_fir_request_generation_{ 1 };
	// Fresh-history serial. Bumped by the UI thread whenever filtering resumes
	// (filter or linear phase turned on), since engines aren't fed while it is
	// off. The designer stamps each new engine (FiltPartConv::serial) with the
	// value it read before creating it; an engine whose serial differs from
	// the current value always needs replacing, and the audio thread only
	// processes an engine whose serial matches, passing audio through
	// otherwise. So no history from before the latest resume is ever played.
	std::atomic<unsigned int> brickwall_fir_reset_serial_{ 0 };
	std::atomic<FiltPartConv*> brickwall_fir_pending_engine_{ nullptr };
	std::atomic<FiltPartConv*> brickwall_fir_retired_engine_{ nullptr };
	FiltPartConv *brickwall_fir_active_engine_ = nullptr; // audio thread only
	std::atomic<double> brickwall_fir_active_latency_ms_{ 0.0 };
	std::atomic<bool> brickwall_designer_stop_{ false };
	std::thread brickwall_designer_thread_;
};

