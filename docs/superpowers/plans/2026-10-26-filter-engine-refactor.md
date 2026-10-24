# Filter Engine Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shrink `dsp/DfxDspPrivate.cpp` (557 lines before the filters work, 2,997 now) by moving the filter engine and its self-checks into their own files, with no change in behaviour.

**Architecture:** The engine (state structs, triple-buffer exchange, segment mixer, preview mask rules, preview leveller), now `static` code in `DfxDspPrivate.cpp`, moves into a `FilterEngine` module with a header of C-style free functions. Its 13 Debug-startup self-checks become a standalone test program, like `FiltChainTest`. Then the engine's `LowCut*` names become `Filter*`, the preview leveller gets its own unit, and the repetitive per-filter setters in `DfxDspPrivate` collapse into one helper. A golden-output checksum test, recorded in Task 1, proves that every later task leaves the audio bit-identical.

**Tech Stack:** C++ (MSVC, VS 2022 toolset v143), standalone `cl` test runners, existing `FiltLowCut` / `FiltChain` modules.

**Spec:** `docs/superpowers/specs/2026-10-24-filters-page-design.md` (behaviour reference; this plan changes structure only). Motivation and scope were agreed in chat on 2026-10-26.

## Global Constraints

- **No behaviour change.** Audio output, timing, thresholds and every constant value stay identical. The golden-output test from Task 1 must give the same checksum after every task.
- The public API in `dsp/include/DfxDsp.h` is unchanged, and so is everything under `fxsound/` and `audiopassthru/`.
- `FiltLowCut` and `FiltChain` (`dsp/ptutil/...`) are unchanged.
- Real-time safety: no allocation, locks or blocking on the audio path. Moved code keeps its exact logic.
- Every commit touching `dsp/` carries the body line `Higher-risk: touches the real-time DSP path (dsp/); needs human review before merge.` No Claude or Co-Authored-By attribution lines.
- Keep CRLF line endings in files that have them, and create new files with CRLF. Don't use tools that rewrite line endings (e.g. `sed -i` in Git Bash).
- Build (PowerShell): `& "$(& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find 'MSBuild\**\Bin\MSBuild.exe')" fxsound\Project\FxSound.sln /p:Configuration=<Debug|Release> /p:Platform=x64 /p:PlatformToolset=v143 /m /v:minimal`. Success means zero errors and `FxSound.exe` is produced. If a running `FxSound.exe` locks the output, report it rather than killing the process.
- Existing tests must keep passing: `cmd /c dsp\ptutil\Filt\test\run_filt_chain_test.cmd` (117 checks, 0 failures) and `cmd /c dsp\ptutil\Filt\test\run_filt_low_cut_test.cmd` (2370 checks, 0 failures).

## Review Focus

- **Checks must not be lost.** The Debug-startup self-checks are removed, so a failing check must now fail the new test runner with a non-zero exit. Task 1 adds a deliberately failing smoke step to prove the runner fails.
- **Silent audio drift from a rename or move**, such as a swapped argument or a constant lost in a rename. Task 1 adds the golden checksum test (`testGoldenOutputUnchanged`), and every task re-runs it.
- **Release-only differences:** code moved out of a translation unit can change inlining or `static` linkage. Tasks 1 and 3 build Release too.
- **Missing project-file entries:** new `.cpp`/`.h` files must be in `dsp/DfxDsp.vcxproj` and `.vcxproj.filters`. Every task verifies with the full solution build, not just the test runner.
- **Designer thread and setters:** Task 4 changes the shape of the setters. The engine test's availability and transition checks cover the rules, and Task 4 adds `testSetterRules` for the collapsed helper.

---

### Task 1: `FilterEngine` module + standalone engine test (pure move)

**Files:**
- Create: `dsp/FilterEngine.h`, `dsp/FilterEngine.cpp`
- Create: `dsp/test/FilterEngineTest.cpp`, `dsp/test/run_filter_engine_test.cmd`
- Modify: `dsp/DfxDspPrivate.cpp` (remove lines ~57–2214: the engine and the `debugVerify*` routines; remove the `#ifdef _DEBUG` self-check block in the constructor, ~2215–2237, including the `FXSOUND_SKIP_DSP_SELFCHECKS` env check)
- Modify: `dsp/u_DfxDsp.h` (move the engine structs and `#define`s at ~84–150 and `DfxDspPrivate::FilterSettings` into `FilterEngine.h`; include it)
- Modify: `dsp/DfxDsp.vcxproj`, `dsp/DfxDsp.vcxproj.filters` (add the two module files)

**Interfaces:**
- Produces: `FilterEngine.h` declares, unchanged in name and signature, every engine function currently `static` in `DfxDspPrivate.cpp` lines 82–943 (`lowCutPreviewLevelCoeffs`, `lowCutLevelPreviewFrame`, `filterDesignedMaskFor` … `filterIirNeededFor`, `lowCutDesignIirParams`, `lowCutDesignFilterParams`, `lowCutIirExchangeInit/Publish/Adopt`, `lowCutAudioStateInit`, `lowCutAudioStateSetRate`, `lowCutAudioStateFormatChanged`, `lowCutIirPoll`, `lowCutRunSegment`, `lowCutDesiredSourceFor`, and the rest). Small helpers that are only used inside the engine (`lowCutRampWeight`, `lowCutRescaleRamp`, `lowCutClampCount`, …) stay `static` in `FilterEngine.cpp`.
- `lowCutProcessBuffer` is a template (`DesiredSourceFn`), so it moves into `FilterEngine.h` as an inline template.
- `struct FilterSettings` becomes a namespace-scope type in `FilterEngine.h` with the same fields; `DfxDspPrivate` uses it as `FilterSettings`.
- The `static_assert`s and `DFXG_LOW_CUT_PREVIEW_*` constants move into `FilterEngine.cpp` unchanged.

- [ ] **Step 1: Baseline.** On the current HEAD, run the two existing runners and record their output in the commit message body (counts only).
- [ ] **Step 2: Write `dsp/test/FilterEngineTest.cpp`.** Use the same `CHECK` macro and `main` pattern as `dsp/ptutil/Filt/test/FiltLowCutTest.cpp`. Each `debugVerify*` routine becomes `static void test<Name>()` (e.g. `debugVerifyFilterPreviewNoLeak` → `testFilterPreviewNoLeak`). Bodies move verbatim; `assert(x)` becomes `CHECK(x, "<routine>:<line>")`. All 13 run from `main`. Return non-zero when anything failed, and print `N checks, M failures`.
  - Add `testGoldenOutputUnchanged`. Feed `lowCutProcessBuffer` (48 kHz, 2 ch, 480-frame buffers, a fixed 3-tone input of 50 Hz + 1 kHz + 17 kHz at 0.2 each) with this script:
    1. Low cut 40 Hz on.
    2. Add high cut 16 kHz.
    3. Mono 125 Hz on.
    4. Low cut Δ, then High cut Δ, then global Δ, then none.
    5. Global off.

    Use 20 buffers per phase, with parameter sets designed by `lowCutDesignFilterParams` and published through the exchange. Compute an FNV-1a 64-bit hash over the output float bytes. On the first run, print the hash and hard-code it as `kGoldenHash` (Step 5). The check is `CHECK(hash == kGoldenHash)`.
- [ ] **Step 3: Write `run_filter_engine_test.cmd`.** Copy `run_filt_chain_test.cmd`; output dir `%TEMP%\filter_engine_test`. Compile `FilterEngineTest.cpp`, `..\FilterEngine.cpp`, `..\ptutil\Filt\FiltChain.cpp` and `..\ptutil\Filt\FiltLowCut.cpp`, with includes `..\include`, `..\ptutil\include` and `..`.
- [ ] **Step 4: Run it, expect a compile failure** (`FilterEngine.h` missing).
- [ ] **Step 5: Do the move.** Move the files' contents as listed under Files, with no logic edits. Run the test: it should report 13 routines' checks with 0 failures, and print the golden hash. Paste the hash into `kGoldenHash`, re-run, and expect 0 failures.
- [ ] **Step 6: Prove the runner fails loudly.** Temporarily change one bound (e.g. the NoLeak bound 0.02 → 1e-9), run it, and confirm a non-zero exit and a `FAIL` line. Revert the change.
- [ ] **Step 7: Full build, Debug and Release** (Global Constraints command). Expect zero errors. `DfxDspPrivate.cpp` should now be about 760 lines (`wc -l`).
- [ ] **Step 8: Re-run** both existing runners and the engine runner.
- [ ] **Step 9: Commit:** `refactor(dsp): move the filter engine and its self-checks out of DfxDspPrivate` (plus the higher-risk line).

### Task 2: Rename the engine from `LowCut*` to `Filter*`

**Files:**
- Modify: `dsp/FilterEngine.h`, `dsp/FilterEngine.cpp`, `dsp/u_DfxDsp.h`, `dsp/DfxDspPrivate.cpp`, `dsp/test/FilterEngineTest.cpp`

**Interfaces (exact renames; nothing else changes):**

| Old | New |
|---|---|
| `LowCutPreviewLevelCoeffs`, `LowCutIirParams`, `LowCutIirExchange`, `LowCutAudioState` | `FilterPreviewLevelCoeffs`, `FilterIirParams`, `FilterIirExchange`, `FilterAudioState` |
| `LOW_CUT_IIR_NUM_SLOTS` / `_SLOT_DIRTY` / `_SLOT_MASK`, `LOW_CUT_SOURCE_DRY` / `_IIR`, `LOW_CUT_NUM_SOURCES`, `LOW_CUT_CROSSFADE_MS`, `LOW_CUT_FADE_MS` | `FILTER_IIR_NUM_SLOTS` / … , `FILTER_SOURCE_DRY` / `_IIR`, `FILTER_NUM_SOURCES`, `FILTER_CROSSFADE_MS`, `FILTER_FADE_MS` |
| `DFXG_LOW_CUT_PREVIEW_*` | `DFXG_FILTER_PREVIEW_*` |
| every engine function `lowCutXxx` | `filterXxx` (e.g. `lowCutRunSegment` → `filterRunSegment`, `lowCutProcessBuffer` → `filterProcessBuffer`, `lowCutIirPoll` → `filterIirPoll`, `lowCutDesignFilterParams` → `filterDesignParams`, `lowCutDesignIirParams` → `filterDesignIirParams`) |
| `DfxDspPrivate`: `applyLowCut`, `requestLowCutDesign`, `lowCutDesignerThreadMain`, `lowCutDesiredSource` | `applyFilters`, `requestFilterDesign`, `filterDesignerThreadMain`, `filterDesiredSource` |
| `DfxDspPrivate` engine-wide members `low_cut_power_on_`, `low_cut_cached_sample_rate_` / `_num_channels_` / `_bps_`, `low_cut_format_serial_`, `low_cut_iir_exchange_`, `low_cut_audio_`, `low_cut_request_sample_rate_` / `_generation_`, `low_cut_reset_serial_`, `low_cut_designer_stop_` / `_mutex_` / `_wake_` / `_thread_` | same names with `low_cut_` → `filter_` |
| test routine names `testLowCut*` | `testFilter*` |

Kept as they are, because they really are per low cut: `low_cut_on_`, `low_cut_cutoff_hz_`, `lowCutOn`, `setLowCutCutoff` / `getLowCutCutoff`, `isLowCutOn`, `FILT_CHAIN_CHANGED_LOW_CUT`, and the `FiltLowCut` module.

- [ ] **Step 1:** Apply the renames with a CRLF-safe tool (an editor, or a script that preserves line endings). Then `git grep -n "lowCut\|LowCut\|LOW_CUT_\(IIR\|SOURCE\|NUM\|CROSSFADE\|FADE\)\|DFXG_LOW_CUT"` over `dsp/` should list only the "kept" names above.
- [ ] **Step 2:** Engine runner: 0 failures and an unchanged `kGoldenHash`. Both existing runners pass.
- [ ] **Step 3:** Debug build, zero errors.
- [ ] **Step 4: Commit:** `refactor(dsp): name the filter engine after filters, not the low cut` (plus the higher-risk line).

### Task 3: Preview leveller as its own unit

**Files:**
- Create: `dsp/FilterPreviewLeveller.h`, `dsp/FilterPreviewLeveller.cpp`
- Modify: `dsp/FilterEngine.h/.cpp` (remove the leveller pieces), `dsp/u_DfxDsp.h` (if it references them), `dsp/test/FilterEngineTest.cpp`, `dsp/test/run_filter_engine_test.cmd`, `dsp/DfxDsp.vcxproj(.filters)`

**Interfaces:**
- Produces `struct FilterPreviewLeveller { float mean_square; float gain; float peak_env; int peak_hold; };`. It groups the four `preview_*` leveller fields now in `FilterAudioState`, which then holds one `FilterPreviewLeveller preview_leveller;`.
- Also produces `FilterPreviewLevelCoeffs` (moved here), plus:
  - `FilterPreviewLevelCoeffs filterPreviewLevelCoeffs(int sample_rate);`
  - `void filterPreviewLevellerReset(FilterPreviewLeveller *leveller);` (sets mean_square 0, gain 1, peak_env 0, peak_hold 0, exactly as the current reset sites do)
  - `void filterPreviewLevellerRescaleHold(FilterPreviewLeveller *leveller, int old_hold_frames, int new_hold_frames);`
  - `void filterPreviewLevelFrame(float *fp_frame, int i_num_channels, const FilterPreviewLevelCoeffs *cp_coeffs, FilterPreviewLeveller *leveller, bool b_update);` (today's `lowCutLevelPreviewFrame` logic verbatim, reading and writing through the struct)
- The `DFXG_FILTER_PREVIEW_*` leveller constants move into `FilterPreviewLeveller.cpp`.

- [ ] **Step 1:** Move the code. Replace every reset site that zeroes the four fields with `filterPreviewLevellerReset`, and the rate-change rescale with `filterPreviewLevellerRescaleHold`.
- [ ] **Step 2:** Add `FilterPreviewLeveller.cpp` to the test runner's compile line.
- [ ] **Step 3:** Engine runner: 0 failures and an unchanged `kGoldenHash`. Both existing runners pass.
- [ ] **Step 4:** Debug and Release builds, zero errors.
- [ ] **Step 5: Commit:** `refactor(dsp): give the filter preview leveller its own unit` (plus the higher-risk line).

### Task 4: Collapse the per-filter setters in `DfxDspPrivate`

**Files:**
- Modify: `dsp/DfxDspPrivate.cpp` (`lowCutOn`/`highCutOn`/`monoBassOn` and `setLowCutCutoff`/`setHighCutCutoff`/`setMonoBassCutoff`, ~lines 2456–2620 before Task 1), `dsp/u_DfxDsp.h`
- Test: `dsp/test/FilterEngineTest.cpp`

**Interfaces:**
- The public `DfxDsp` and `DfxDspPrivate` method names and signatures are unchanged.
- New private helpers:
  - `void setStageOn(int stage_bit, std::atomic<bool> &stage_on, bool on);` does what the three `xOn` bodies do today: copy the settings, set the field, call `beginFilterChange`, dropping the stage's preview flag when switching off, then store and call `requestFilterDesign(true)`. The `FilterSettings` field to set is chosen from `stage_bit`.
  - `void setStageCutoff(int stage_bit, std::atomic<float> &stage_cutoff, float clamped_hz);` does the shared part of the three cutoff setters after each one's own clamp or snap, which stays in its public setter.
- Pure helper in `FilterEngine.h/.cpp`: `FilterSettings filterSettingsWithStage(FilterSettings settings, int stage_bit, bool on, float hz);` returns `settings` with that stage's `*_on` and `*_hz` replaced.

- [ ] **Step 1: Write the failing test** `testSetterRules`:
  - `filterSettingsWithStage(s, FILT_CHAIN_CHANGED_HIGH_CUT, false, 16000.0f)` changes only `high_on` and `high_hz`.
  - The same holds for the low cut and mono bits.
  - With a preview selection of `PreviewLowCut | PreviewHighCut` and `high_on` switched off, `filterPreviewSelectionFor(selection & ~DfxDsp::PreviewHighCut, next) == DfxDsp::PreviewLowCut`.
- [ ] **Step 2:** Run it and expect a compile failure (helper missing).
- [ ] **Step 3:** Implement the helpers and rewrite the six setters to use them.
- [ ] **Step 4:** Engine runner: 0 failures and an unchanged `kGoldenHash`. Both existing runners pass. Debug build, zero errors.
- [ ] **Step 5: Commit:** `refactor(dsp): one helper for the per-filter switch and cutoff setters` (plus the higher-risk line).

### Task 5: Wrap-up

- [ ] **Step 1:** In the spec's "Tests" paragraph (`docs/superpowers/specs/2026-10-24-filters-page-design.md`), replace the mention of Debug self-checks with `dsp/test/run_filter_engine_test.cmd`.
- [ ] **Step 2:** Run `wc -l dsp/DfxDspPrivate.cpp dsp/FilterEngine.cpp dsp/FilterPreviewLeveller.cpp dsp/test/FilterEngineTest.cpp`, and report the numbers in the commit body.
- [ ] **Step 3:** Run all three runners and both builds a final time. Then rebuild Debug and launch `fxsound\Project\x64\Debug\App\FxSound.exe` once to confirm it starts. The FxSound driver is installed on this machine.
- [ ] **Step 4: Commit:** `docs: filter engine tests now live in dsp/test`.
