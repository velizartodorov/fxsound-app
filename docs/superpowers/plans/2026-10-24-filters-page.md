# Filters Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the low cut page into a Filters page with low cut, high cut and stereo-to-mono (mono bass), each with its own on/off and Δ (hear what's removed), plus a global on/off and global Δ.

**Architecture:** A new pure-math module `FiltChain` (in `dsp/ptutil`) designs and runs the three stages in series per frame and returns the removed signal by subtraction. The existing low cut engine in `DfxDspPrivate.cpp` (designer thread, triple-buffer exchange, Dry/IIR crossfades, preview leveller) keeps its structure but carries `FiltChainParams` instead of hp/lp section arrays, and its boolean preview becomes a preview *stage*. Controller and GUI are extended on top.

**Tech Stack:** C++ (MSVC, Visual Studio 2022), JUCE 6.1.6, standalone `cl` test runner for `dsp/ptutil/Filt`, Debug-only `assert` self-checks in `DfxDspPrivate.cpp`.

**Spec:** `docs/superpowers/specs/2026-10-24-filters-page-design.md`

## Global Constraints

- Branch: `feature/brickwall-filter`. Commits carry **no** Claude attribution lines (user preference).
- **Higher-risk:** every commit touching `dsp/` gets a body line `Higher-risk: touches the real-time DSP path (dsp/); needs human review before merge.` `audiopassthru/` must not change.
- No allocation, locks or blocking calls on the audio thread. Coefficient design stays on the designer thread.
- Q fixed at 0.71 (`FILT_LOW_CUT_Q_NEUTRAL`); no Q API, slider or setting remains.
- Slopes: 6, 12, 18, 24, 36, 48 dB/oct, shared by low cut and high cut; default 24.
- Low cut: 0–200 Hz, 0 = Off, default on at 20 Hz.
- High cut: 14 000–20 000 Hz in 500 Hz steps, then 20 500 = Off; default on at Off. Cutoffs above 0.45 × sample rate are clamped to it, not bypassed.
- Stereo to mono: 0–150 Hz, 0 = Off, default off at 150 Hz. Side high-passed with Linkwitz-Riley 24 dB/oct (two identical Butterworth 2nd-order HP sections); ignores the slope setting.
- Order: stereo to mono → low cut → high cut, in series.
- Removed signal = stage input − stage output (global Δ = chain input − chain output).
- One Δ at a time. Δ unavailable when global off, its filter's switch off, or its slider at Off. Global Δ available when any filter is active.
- Global off by default. Restore Defaults leaves the global switch alone.
- Settings keys: `filters_on`, `filters_slope_db` (fallback `low_cut_slope_db`), `low_cut_on`, `low_cut_cutoff_hz`, `high_cut_on`, `high_cut_cutoff_hz`, `mono_bass_on`, `mono_bass_cutoff_hz`, `filters_settings_version`. `low_cut_q` is ignored.
- Page fits the existing 176×257 `FxAudioControls` panel. Bottom row: global ⏻ left, Restore Defaults and global Δ right.

## Review Focus

- Switching Δ from one filter straight to another while playing must not click → engine self-check in Task 2 (`debugVerifyFilterPreviewSwitch`).
- High cut at 20 kHz on a 44.1 kHz device must stay stable and still cut → chain test in Task 1 (`testHighCutClampAt44k`).
- Mono (1 ch) and 5.1/7.1 output with stereo to mono on: only channels 0/1 change, 1 ch passes untouched → chain test in Task 1 (`testMonoChannelCounts`).
- Moving a slider to Off (or a switch off) while its Δ plays releases that Δ → engine self-check in Task 3 (`debugVerifyFilterPreviewAvailability`).
- Existing users' settings (`low_cut_on` true/false, stale `low_cut_q`) upgrade without an audible change → manual check in Task 6.

---

### Task 1: `FiltChain` math module

**Files:**
- Create: `dsp/ptutil/include/FiltChain.h`
- Create: `dsp/ptutil/Filt/FiltChain.cpp`
- Create: `dsp/ptutil/Filt/test/FiltChainTest.cpp`
- Create: `dsp/ptutil/Filt/test/run_filt_chain_test.cmd` (copy of `run_filt_low_cut_test.cmd` compiling `FiltChainTest.cpp`, `..\FiltChain.cpp`, `..\FiltLowCut.cpp`; output dir `%TEMP%\filt_chain_test`)
- Modify: `dsp/DfxDsp.vcxproj`, `dsp/DfxDsp.vcxproj.filters` (add the `.cpp`/`.h` next to the `FiltLowCut` entries)

**Interfaces:**
- Consumes: `FiltLowCut.h` (`filtLowCutDesign`, `filtLowCutProcessSample`, `filtLowCutResetChannelState`, `FiltLowCutSection`, `FiltLowCutChannelState`, `FiltLowCutBiquadState`, `FILT_LOW_CUT_MAX_SECTIONS`, `FILT_LOW_CUT_MAX_CHANNELS`, `FILT_LOW_CUT_Q_NEUTRAL`).
- Produces (all `PT_DECLSPEC`, C style like `FiltLowCut.h`):
  - `#define FILT_CHAIN_MAX_CHANNELS FILT_LOW_CUT_MAX_CHANNELS`, `#define FILT_CHAIN_MONO_SECTIONS 2`, `#define FILT_CHAIN_MAX_FREQ_RATIO 0.45`
  - `#define FILT_CHAIN_STAGE_NONE 0`, `_LOW_CUT 1`, `_HIGH_CUT 2`, `_MONO 3`, `_ALL 4`
  - `struct FiltChainParams { FiltLowCutSection low_cut[FILT_LOW_CUT_MAX_SECTIONS]; int num_low_cut; FiltLowCutSection high_cut[FILT_LOW_CUT_MAX_SECTIONS]; int num_high_cut; FiltLowCutSection mono_side[FILT_CHAIN_MONO_SECTIONS]; int num_mono_side; };`
  - `struct FiltChainState { FiltLowCutChannelState channels[FILT_CHAIN_MAX_CHANNELS]; FiltLowCutBiquadState mono_side[FILT_CHAIN_MONO_SECTIONS]; };` (`channels[c].hp_sections` = low cut history, `.lp_sections` = high cut history)
  - `int filtChainDesign(int i_slope_db, double r_low_cut_hz, double r_high_cut_hz, double r_mono_hz, double r_sample_rate_hz, FiltChainParams *sp_out);` A stage with hz ≤ 0 gets 0 sections. High cut hz is clamped to `FILT_CHAIN_MAX_FREQ_RATIO * r_sample_rate_hz` before `filtLowCutDesign(..., 0, ...)`. Mono = `filtLowCutDesign(12, FILT_LOW_CUT_Q_NEUTRAL, mono_hz, fs, 1, ...)` written to both `mono_side` slots. Returns total section count.
  - `void filtChainResetState(FiltChainState *sp_state);`
  - `bool filtChainSameLayout(const FiltChainParams *a, const FiltChainParams *b);` (equal counts and per-section `order`)
  - `void filtChainProcessFrame(const float *cp_in, float *fp_out, float *fp_removed, int i_num_channels, int i_removed_stage, const FiltChainParams *cp_params, FiltChainState *sp_state);` `i_num_channels` ≤ `FILT_CHAIN_MAX_CHANNELS` (caller caps). Mono stage only when `i_num_channels >= 2` and `num_mono_side > 0`, on channels 0/1: `M=(L+R)/2, S=(L−R)/2, S_hp=cascade(S)`, `L'=M+S_hp, R'=M−S_hp`. Then low cut, then high cut, per channel. `fp_removed[c]` = input − output of the stage named by `i_removed_stage` (`_ALL` = chain input − chain output; `_NONE` = 0).

- [ ] **Step 1: Write failing tests in `FiltChainTest.cpp`** (same `CHECK` macro/`main` pattern as `FiltLowCutTest.cpp`; drive with 48 kHz unless stated, settle 1 s before measuring RMS):
  - `testDesignOffStages`: `filtChainDesign(24, 0, 0, 0, 48000, &p)` returns 0 and all three counts are 0; `(24, 20, 0, 0)` → `num_low_cut == filtLowCutDesign(24, 0.71, 20, 48000, 1, tmp)`, others 0; `(24, 0, 18000, 0)` → only `num_high_cut > 0`; `(24, 0, 0, 150)` → `num_mono_side == 2`, both sections `order == 2`, `high_pass == 1`.
  - `testMonoLinkwitzRiley`: with mono 150 Hz, `filtLowCutCascadeResponseDb(150, 48000, p.mono_side, 2)` within 0.05 dB of −6.02; at 1500 Hz above −0.1 dB.
  - `testMonoKeepsMidExact`: random L==R noise, mono 150 only → `out == in` within 1e-6 and removed (`_MONO`) within 1e-6 of 0.
  - `testMonoFoldsSideBass`: L=+sin(40 Hz), R=−sin(40 Hz), mono 150 → output RMS ≥ 20 dB below input; same at 1 kHz → output RMS within 0.5 dB of input.
  - `testHighCutResponse`: high cut 18000 at slope 24 → response at 18000 within 0.1 dB of −3.01; 1 kHz sine passes within 0.1 dB.
  - `testHighCutClampAt44k`: `(24, 0, 20000, 0, 44100)` → `num_high_cut > 0`; impulse response through `filtChainProcessFrame` finite and `|y| < 1e-6` after 4410 samples; 1 kHz passes within 0.1 dB; response at 20000 Hz below −3 dB.
  - `testRemovedAddsBack`: 8 ch random noise, all three stages on (20 / 16000 / 150 Hz, slope 48); for each stage in `_LOW_CUT, _HIGH_CUT, _MONO` run a fresh state and check stage removed + that stage's output equals that stage's input (compute by running a second chain with only that stage); for `_ALL` check `out[c] + removed[c] == in[c]` within 1e-5 for every sample and channel.
  - `testMonoChannelCounts`: 1 ch with mono on → output equals low/high-cut-only output (mono skipped); 6 ch → channels 2–5 identical to a run with mono off.
- [ ] **Step 2: Run, expect FAIL (compile error: `FiltChain.h` missing)**
  Run: `cmd /c dsp\ptutil\Filt\test\run_filt_chain_test.cmd`
- [ ] **Step 3: Implement `FiltChain.h` / `FiltChain.cpp`** per the Interfaces block. Use `double` internally like `filtLowCutProcessSample`. File header = the AGPL header from `FiltLowCut.h`.
- [ ] **Step 4: Run, expect `N checks, 0 failures`, exit 0**
  Run: `cmd /c dsp\ptutil\Filt\test\run_filt_chain_test.cmd` and `cmd /c dsp\ptutil\Filt\test\run_filt_low_cut_test.cmd` (still passes).
- [ ] **Step 5: Commit** `feat(dsp): add FiltChain module for low cut, high cut and mono bass` (with the higher-risk body line).

### Task 2: Engine audio path runs the chain with stage previews

**Files:**
- Modify: `dsp/u_DfxDsp.h` (`LowCutIirParams`, `LowCutAudioState`)
- Modify: `dsp/DfxDspPrivate.cpp` (static helpers lines ~134–737, self-checks ~739–1150)

**Interfaces:**
- Consumes: Task 1 `FiltChain*`.
- Produces:
  - `struct LowCutIirParams { FiltChainParams chain; int sample_rate; unsigned int reset_serial; };` (hp/lp arrays and counts removed)
  - `LowCutAudioState`: `iir_states` becomes `FiltChainState iir_states[2];`, `iir_channels` stays; add `int preview_stage;` (the stage currently audible in the preview ramp).
  - `static void lowCutDesignIirParams(int sample_rate, int slope_db, float low_cut_hz, float high_cut_hz, float mono_hz, unsigned int reset_serial, LowCutIirParams *params);`
  - `lowCutProcessBuffer(..., int i_preview_stage, DesiredSourceFn)` and `lowCutRunSegment(..., int i_preview_stage)` replace the `bool b_preview_on` parameters.

- [ ] **Step 1: Update self-checks first** so they express the new behaviour (they run in Debug at `DfxDspPrivate` construction, lines ~1221–1232):
  - Existing `debugVerifyLowCut*` checks: port calls to the new `lowCutDesignIirParams` signature (low cut only: `high_cut_hz = 0, mono_hz = 0`), `preview_on` → `FILT_CHAIN_STAGE_LOW_CUT` / `FILT_CHAIN_STAGE_NONE`, reference filter via `filtChainProcessFrame`. Drop the assertions that read `num_lp_sections`.
  - New `debugVerifyFilterChainSteady`: publish a set with low 100 / high 16000 / mono 150 (2 ch, 48 kHz), run `lowCutProcessBuffer` with desired IIR and preview `FILT_CHAIN_STAGE_NONE` until the fade-in is done, then check each later buffer equals a direct `filtChainProcessFrame` run on the same input (fresh state started at the same frame as the engine's IIR start) within 1e-5.
  - New `debugVerifyFilterPreviewSwitch`: steady IIR with all three stages, preview `_LOW_CUT` for 50 buffers, then request `_HIGH_CUT`. Assert `state->preview_stage` stays `_LOW_CUT` while `preview_pos > 0`, becomes `_HIGH_CUT` only after `preview_pos` reaches 0, then ramps back up; and the max sample-to-sample step on a 100 Hz + 17 kHz test signal across the switch stays below the step seen in steady preview × 2 (same style as the `max_step` check in `debugVerifyLowCutTransitions`).
  - Register both new checks next to the existing ones.
- [ ] **Step 2: Build Debug, expect compile errors** (old signatures)
  Run (PowerShell): `& "$(& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find 'MSBuild\**\Bin\MSBuild.exe')" fxsound\Project\FxSound.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal`
- [ ] **Step 3: Implement** in `DfxDspPrivate.cpp` / `u_DfxDsp.h`:
  - `lowCutDesignIirParams` → `filtChainDesign(slope_db, low, high, mono, fs, &params->chain)`.
  - `lowCutIirSameLayout` → `filtChainSameLayout`; `lowCutIirCopyParams` clamps the three counts to their maxima; resets use `filtChainResetState`.
  - `lowCutIirRunChannel` becomes a per-frame `lowCutIirRunFrame(state, const float *in, int channels, float incoming_weight, int removed_stage, float *filtered, float *removed)` calling `filtChainProcessFrame` on the current set and, while `iir_crossfading`, on the incoming set, mixing both outputs and both removeds by `incoming_weight`.
  - Preview ramp: `preview_dir = +1` only when `i_preview_stage != NONE && i_preview_stage == state->preview_stage`; otherwise ramp down. When `preview_pos == 0`, adopt `state->preview_stage = i_preview_stage` and reset `preview_mean_square = 0, preview_gain = 1` if it changed. Removed is computed for `state->preview_stage`. The early-out for "fully dry" checks `preview_pos == 0 && i_preview_stage == NONE`. `preview_was_on` is replaced by this adoption rule.
- [ ] **Step 4: Build Debug, expect success; run the Task 1 tests again** (they must still pass).
- [ ] **Step 5: Run the Debug app once** (`fxsound\Project\x64\Debug\...\FxSound.exe`, requires the FxSound driver installed) and confirm it starts without an assert dialog. If the driver is not available, report this step as not done.
- [ ] **Step 6: Commit** `feat(dsp): run low cut, high cut and mono bass as one chain with per-stage preview` (+ higher-risk line).

### Task 3: DSP public API, controls and designer

**Files:**
- Modify: `dsp/include/DfxDsp.h`, `dsp/DfxDsp.cpp`, `dsp/u_DfxDsp.h` (`DfxDspPrivate` members), `dsp/DfxDspPrivate.cpp` (lines ~1347–1490, static_asserts at 57–60, self-check registration)

**Interfaces:**
- Consumes: Task 2 `lowCutDesignIirParams`, `lowCutProcessBuffer(..., int i_preview_stage, ...)`.
- Produces on `DfxDsp` (forwarded 1:1 to `DfxDspPrivate`):
  - Constants: `CUT_SLOPES_DB[] = {6,12,18,24,36,48}`, `CUT_NUM_SLOPES`, `CUT_DEFAULT_SLOPE_DB = 24`, `CUT_Q = 0.71f`, `LOW_CUT_MIN_CUTOFF_HZ = 0`, `LOW_CUT_MAX_CUTOFF_HZ = 200`, `LOW_CUT_DEFAULT_CUTOFF_HZ = 20`, `HIGH_CUT_MIN_CUTOFF_HZ = 14000`, `HIGH_CUT_MAX_CUTOFF_HZ = 20000`, `HIGH_CUT_STEP_HZ = 500`, `HIGH_CUT_OFF_HZ = 20500`, `HIGH_CUT_DEFAULT_CUTOFF_HZ = HIGH_CUT_OFF_HZ`, `MONO_BASS_MIN_CUTOFF_HZ = 0`, `MONO_BASS_MAX_CUTOFF_HZ = 150`, `MONO_BASS_DEFAULT_CUTOFF_HZ = 150`. Removed: `LOW_CUT_SLOPES_DB`, `LOW_CUT_NUM_SLOPES`, `LOW_CUT_DEFAULT_SLOPE_DB`, `LOW_CUT_MIN_Q`, `LOW_CUT_MAX_Q`, `LOW_CUT_DEFAULT_Q`.
  - `enum FilterPreview { PreviewNone = 0, PreviewLowCut = 1, PreviewHighCut = 2, PreviewMonoBass = 3, PreviewAll = 4 };` (static_assert equal to `FILT_CHAIN_STAGE_*`)
  - `void filtersOn(bool on); bool isFiltersOn();`
  - `void setCutSlope(int db_per_octave); int getCutSlope();`
  - `void lowCutOn(bool on); bool isLowCutOn(); void setLowCutCutoff(float hz); float getLowCutCutoff();`
  - `void highCutOn(bool on); bool isHighCutOn(); void setHighCutCutoff(float hz); float getHighCutCutoff();`
  - `void monoBassOn(bool on); bool isMonoBassOn(); void setMonoBassCutoff(float hz); float getMonoBassCutoff();`
  - `bool isFilterPreviewAvailable(FilterPreview preview); void setFilterPreview(FilterPreview preview); FilterPreview getFilterPreview();`
  - Removed: `setLowCutQ`, `getLowCutQ`, `setLowCutSlope`, `getLowCutSlope`, `lowCutPreviewOn`, `isLowCutPreviewOn`.
  - Internal pure helper (static in `DfxDspPrivate.cpp`): `static bool filterPreviewAvailableFor(int preview, bool filters_on, bool low_on, float low_hz, bool high_on, float high_hz, bool mono_on, float mono_hz);` — LowCut: `filters_on && low_on && low_hz > 0`; HighCut: `filters_on && high_on && high_hz < HIGH_CUT_OFF_HZ`; MonoBass: `filters_on && mono_on && mono_hz > 0`; All: any of the three; None: true.

- [ ] **Step 1: Write self-check `debugVerifyFilterPreviewAvailability`** asserting `filterPreviewAvailableFor` for: global off → all false except None; low on at 0 Hz → LowCut false; high on at 20500 → HighCut false, at 20000 → true; mono on at 0 → false; only mono active → All true, LowCut false. Register it.
- [ ] **Step 2: Build Debug, expect compile error** (helper missing).
- [ ] **Step 3: Implement.**
  - Members: `std::atomic<bool> filters_on_{false}, high_cut_on_{true}, mono_bass_on_{false}` (keep `low_cut_on_`, default true now); `std::atomic<float> high_cut_cutoff_hz_{20500}, mono_bass_cutoff_hz_{150}`; rename `low_cut_iir_slope_db_` → `cut_slope_db_`; `std::atomic<int> filter_preview_{PreviewNone}`. Remove `low_cut_iir_q_`, `low_cut_preview_on_`.
  - Setters clamp to the constants (high cut snaps to `HIGH_CUT_STEP_HZ` grid, ≥ 20 500 → `HIGH_CUT_OFF_HZ`), call `requestLowCutDesign(true)`, and then: if the current `filter_preview_` is no longer available, set it to `PreviewNone`.
  - "Active" = `filterPreviewAvailableFor(PreviewAll, ...)`. Any setter that moves Active from false to true bumps `low_cut_reset_serial_` before requesting the design (replaces the bump in today's `lowCutOn`).
  - Designer thread: effective hz = low `low_cut_on_ ? cutoff : 0`, high `high_cut_on_ && cutoff < OFF ? cutoff : 0`, mono `mono_bass_on_ ? cutoff : 0`; slope `cut_slope_db_`; Q is `FILT_LOW_CUT_Q_NEUTRAL` inside `FiltChain`.
  - `applyLowCut`: `filter_on = filters_on_ && low_cut_power_on_ && Active`; preview stage = `filter_on ? filter_preview_ : PreviewNone`.
  - `setFilterPreview(p)` stores `p` only if available, else `PreviewNone`.
  - static_asserts at lines 57–60 move to `CUT_*` / `CUT_Q == FILT_LOW_CUT_Q_NEUTRAL`.
- [ ] **Step 4: Build Debug** (the GUI will fail to compile until Task 4; build only the `DfxDsp` project: add `/t:DfxDsp` to the MSBuild command). Expect success. Rerun Task 1 tests.
- [ ] **Step 5: Commit** `feat(dsp): global, high cut and mono bass controls; fixed Q; preview stages` (+ higher-risk line).

### Task 4: Controller API, persistence and migration

**Files:**
- Modify: `fxsound/Source/GUI/FxController.h` (lines ~153–162, 255), `fxsound/Source/GUI/FxController.cpp` (init ~762–766, call sites of `stopLowCutPreview` ~914, ~937, ~1798, setters ~2640–2708)

**Interfaces:**
- Consumes: Task 3 `DfxDsp` API.
- Produces on `FxController`:
  - `bool isFiltersOn(); void setFiltersOn(bool on);`
  - `int getCutSlope(); void setCutSlope(int slope_db);`
  - `bool isLowCutOn(); void setLowCutOn(bool on); int getLowCutCutoff(); void setLowCutCutoff(int cutoff_hz);` (existing, now per filter)
  - `bool isHighCutOn(); void setHighCutOn(bool on); int getHighCutCutoff(); void setHighCutCutoff(int cutoff_hz);`
  - `bool isMonoBassOn(); void setMonoBassOn(bool on); int getMonoBassCutoff(); void setMonoBassCutoff(int cutoff_hz);`
  - `DfxDsp::FilterPreview getFilterPreview(); void setFilterPreview(DfxDsp::FilterPreview preview); bool isFilterPreviewAvailable(DfxDsp::FilterPreview preview);`
  - `void stopFilterPreview();` (replaces `stopLowCutPreview`, same notify-on-change behaviour)
  - `void restoreFilterDefaults();` — preview None, slope 24, low cut on/20, high cut on/`HIGH_CUT_OFF_HZ`, mono off/150; does not touch `filters_on`.
  - Removed: `getLowCutQ`, `setLowCutQ`, `getLowCutSlope`, `setLowCutSlope`, `isLowCutPreviewOn`, `setLowCutPreviewOn`, `stopLowCutPreview`.

- [ ] **Step 1: Implement.** Every setter persists its key and, if `getFilterPreview()` changed as a result, calls `FxModel::getModel().notifyListeners()`.
  Init order: migration → slope → cutoffs → per-filter switches → `filters_on`. Migration, run when `settings_.getInt("filters_settings_version", 0) < 1`:
  ```cpp
  settings_.setBool("filters_on", settings_.getBool("low_cut_on"));
  settings_.setBool("low_cut_on", true);
  settings_.setInt("filters_settings_version", 1);
  ```
  Slope load: `settings_.getInt("filters_slope_db", settings_.getInt("low_cut_slope_db", DfxDsp::CUT_DEFAULT_SLOPE_DB))`. Defaults for absent keys per Global Constraints (`high_cut_on` true, `mono_bass_on` false). Do not read `low_cut_q`.
- [ ] **Step 2: Build the `DfxDsp` project plus the controller**: full-solution build will still fail in `FxAudioControls.cpp` (fixed in Task 6). Confirm the only remaining errors are in `FxAudioControls.cpp`.
- [ ] **Step 3: Commit** `feat(gui): controller and settings for the filters page`.

### Task 5: `FxAudioSlider` "Off" at the top end

**Files:**
- Modify: `fxsound/Source/GUI/FxAudioSlider.h`, `fxsound/Source/GUI/FxAudioSlider.cpp`

**Interfaces:**
- Produces: `void FxAudioSlider::setMaxValueText(const String& text);` — `updateLabel()` shows `text` when `getValue() >= getMaximum()`, mirroring `setZeroValueText`.

- [ ] **Step 1: Implement** (member `String max_value_text_`).
- [ ] **Step 2: Commit** together with Task 6 (no standalone test harness for GUI; verified in Task 6).

### Task 6: Filters page GUI

**Files:**
- Modify: `fxsound/Source/GUI/FxAudioControls.h` (class `FxLowCutControl` → `FxFiltersControl`, `low_cut_control_` → `filters_control_`, `LowCutPage` → `FiltersPage`), `fxsound/Source/GUI/FxAudioControls.cpp` (lines ~700–1000 and helpers `lowCutSlope*`, `lowCutToggleTooltip`)

**Interfaces:**
- Consumes: Task 4 controller API, Task 5 `setMaxValueText`, existing `FxPowerButton`, `PreviewButton`, `ValueSlider`, `FxAudioSlider`.
- Produces: `class FxFiltersControl : public Component` with the same public surface as `FxLowCutControl` (`update()`, `setLookAndFeel(FxTheme&)`).

- [ ] **Step 1: Implement the page**:
  - Members: `slope_title_`, `slope_slider_` (ValueSlider, title "Steepness", same value text/tooltips as today); three blocks each `{FxPowerButton power; Label title; FxAudioSlider cutoff; PreviewButton delta;}` for Low Cut (`"%0.0f Hz"`, range 0–200 step 1, zero text "Off"), High Cut (slider value in **kHz**: range 14.0–20.5 step 0.5, format `"%0.1f kHz"`, `setMaxValueText("Off")`; controller gets `roundToInt(v * 1000)`), Stereo to Mono (`"%0.0f Hz"`, 0–150 step 1, zero text "Off"); bottom row `global_power_button_` + label "Global" at left, `restore_defaults_button_` and `global_delta_button_` at right.
  - Layout: per spec order; rows are header (20 px: ⏻, title, Δ right-aligned) + slider (18 px). Everything must fit inside 176×257 with `X_MARGIN`, `Y_MARGIN` and `BUTTONS_Y` as today.
  - Δ click: toggled on → `setFilterPreview(p)`; toggled off → `setFilterPreview(PreviewNone)`; then `update()` so exactly one Δ shows active.
  - `updateEnabled()`: per block slider/title enabled = global on && block on; Δ enabled = `isFilterPreviewAvailable(p)`; slope enabled = global on && (low on || high on); global Δ enabled = `isFilterPreviewAvailable(PreviewAll)`; Restore Defaults always enabled.
  - Restore Defaults → `controller.restoreFilterDefaults(); update();`.
  - `visibilityChanged()` hidden → `stopFilterPreview()`.
  - Tooltips: reuse low cut wording; high cut "Frequencies above this are removed. Set to Off to pass audio through unfiltered."; stereo to mono "Bass below this is folded to mono; everything above stays stereo. Set to Off (0Hz) to leave stereo untouched."; Δ "Plays back only the content this filter is removing, so you can hear what it affects."; global Δ "Plays back everything the filters are removing."
  - Remove Q title/slider and `LOW_CUT_FIRST_ORDER_SLOPE_DB` Q logic.
- [ ] **Step 2: Build Debug and Release x64, expect success** (MSBuild command from Task 2, then with `/p:Configuration=Release`).
- [ ] **Step 3: Rerun both standalone tests** (`run_filt_chain_test.cmd`, `run_filt_low_cut_test.cmd`), expect 0 failures.
- [ ] **Step 4: Manual check in the Debug app** (needs the FxSound driver; if unavailable, report it as not done rather than claiming it):
  - No assert at startup; page matches the approved mockup; nothing clipped at 176×257.
  - Each ⏻ greys its slider and Δ; global ⏻ off greys all but itself and Restore Defaults; global back on restores previous states.
  - Δ buttons: one at a time; switching Δ while playing doesn't click; slider to Off releases its Δ; leaving the page releases Δ.
  - Stereo-to-mono Δ on a mono track is silent; on a wide track below 150 Hz is audible.
  - Settings upgrade: with a settings file having `low_cut_on=true`, `low_cut_q=1.5`, start → global on, low cut on, sound unchanged apart from Q; with `low_cut_on=false` → global off.
- [ ] **Step 5: Commit** `feat(gui): filters page with low cut, high cut and stereo to mono`.

### Task 7: Wrap-up

- [ ] **Step 1:** `git log --oneline main..HEAD` — every `dsp/` commit has the higher-risk line; `git diff main --stat -- audiopassthru/` is empty.
- [ ] **Step 2:** Hand off via superpowers:finishing-a-development-branch; the PR description must flag the `dsp/` changes as higher-risk and list the manual checks from Task 6 Step 4 with their actual results.
