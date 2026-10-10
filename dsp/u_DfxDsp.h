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
#include <mutex>
#include <condition_variable>
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

/* One-pole smoothing coefficients for the preview auto-leveller at a sample
 * rate (see brickwallPreviewLevelCoeffs() in DfxDspPrivate.cpp). */
struct BrickwallPreviewLevelCoeffs {
	float rms;
	float rise;
	float fall;
};

/* An immutable Zero Latency (IIR) parameter set, designed on the designer
 * thread for one sample rate / steepness / cutoff and published to the audio
 * thread through BrickwallIirExchange. */
struct BrickwallIirParams {
	FiltBrickwallBiquadCoeffs hp_coeffs;
	FiltBrickwallBiquadCoeffs lp_coeffs;  /* complementary low-pass, for the preview only */
	int num_hp_sections;                  /* 0 when the cutoff is 0Hz (high-pass band bypassed) */
	int num_lp_sections;
	int sample_rate;                      /* 0 = no set designed yet */
	unsigned int reset_serial;            /* brickwall_fir_reset_serial_ when it was designed */
};

#define BRICKWALL_IIR_NUM_SLOTS   3
/* Set in middle_slot when it holds a set the audio thread hasn't taken yet. */
#define BRICKWALL_IIR_SLOT_DIRTY  0x4
#define BRICKWALL_IIR_SLOT_MASK   0x3

/* Lock-free triple buffer of IIR parameter sets (the same scheme as
 * FiltPartConv's filter spectra): the audio thread owns front_slot, the
 * designer thread (the only publisher) owns back_slot, and middle_slot is
 * exchanged atomically between them. */
struct BrickwallIirExchange {
	BrickwallIirParams slots[BRICKWALL_IIR_NUM_SLOTS];
	int front_slot;                       /* audio thread only */
	int back_slot;                        /* designer thread only */
	std::atomic<int> middle_slot;
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

	// Read by DfxDsp::processAudio()/setSignalFormat() on the audio thread.
	std::atomic<bool> being_destroyed_{ false };
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

	// Brickwall filter (dsp/ptutil/include/FiltBrickwall.h). Zero Latency
	// (IIR) parameter sets are designed on the designer thread and adopted by
	// the audio thread in applyBrickwallFilter().
	void applyBrickwallFilter(float *audio_buffer, int num_sample_sets);

	// Brickwall filter, linear-phase mode: FFT partitioned convolution
	// (dsp/ptutil/include/FiltPartConv.h), designed on a background thread.
	// wake_designer must be false on the audio thread (no system calls there).
	void requestBrickwallDesign(bool wake_designer);
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
	BrickwallPreviewLevelCoeffs brickwall_preview_level_coeffs_ = {};
	int brickwall_preview_level_coeffs_rate_ = -1; // sample rate the coefficients above are for; -1 = not computed (audio thread only)
	// Settings written by the UI thread and read by the designer thread.
	std::atomic<int> brickwall_filter_steepness_{ DfxDsp::BrickwallSteepness::Standard };
	std::atomic<float> brickwall_hp_cutoff_hz_{ 20.0f }; // 0 = high-pass band bypassed
	// The current signal format: audio thread only (setSignalFormat() and
	// applyBrickwallFilter()), apart from initialisation in the constructor.
	int brickwall_cached_sample_rate_ = 44100;
	int brickwall_cached_num_channels_ = 2;

	// Zero Latency (IIR) hand-off: the designer thread publishes immutable
	// parameter sets into brickwall_iir_exchange_; the audio thread adopts the
	// newest one at the start of each buffer. Everything below the exchange is
	// audio thread only, so the cascade's history is only ever reset there.
	BrickwallIirExchange brickwall_iir_exchange_;
	FiltBrickwallChannelState brickwall_channel_states_[FILT_BRICKWALL_MAX_CHANNELS];
	unsigned int brickwall_iir_history_serial_ = 0; // reset_serial of the set the history was last reset for
	int brickwall_iir_history_hp_sections_ = -1;    // section counts the history belongs to
	int brickwall_iir_history_lp_sections_ = -1;
	bool brickwall_iir_ran_last_buffer_ = false;    // false whenever the IIR path skipped a buffer

	// Linear-phase engine hand-off. The UI and audio threads only store and
	// exchange these atomics; brickwall_designer_thread_ does all design and
	// allocation (IIR parameter sets as well as linear-phase engines; the
	// request generation and reset serial below drive both). pending:
	// designer -> audio. retired: audio -> designer (holds at most one engine;
	// the audio thread adopts only when it is empty, so nothing is leaked).
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
	// The designer sleeps on brickwall_designer_wake_ (with a timeout, so
	// requests made on the audio thread, which never notifies, are still
	// picked up). UI-thread requests and the destructor notify it.
	std::mutex brickwall_designer_mutex_;
	std::condition_variable brickwall_designer_wake_;
	std::thread brickwall_designer_thread_;
};

