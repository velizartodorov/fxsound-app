# Brickwall Filter (20Hz - 20kHz) — Design Spec

Date: 2026-09-16
Branch: `feature/brickwall-filter`

## Summary

Add an optional audio bandwidth-limiting filter to FxSound that attenuates
content below 20Hz and above 20kHz (the nominal edges of human hearing).
The filter is user-controlled from the **Audio** section of the Settings
dialog: a master on/off toggle, a choice of three steepness presets (each
with an explanatory tooltip), and a "hear what's removed" preview control
that plays back only the content the filter is cutting.

## Goals

- Let users optionally remove inaudible sub-20Hz rumble and ultra-20kHz
  content from the FxSound output.
- Offer a choice of filter steepness, since steeper filters cost more CPU
  and have more transition-band phase distortion.
- Let users audition exactly what the filter removes, to build confidence
  in the feature before committing to it.
- Reuse existing DSP primitives and existing GUI settings patterns rather
  than introducing new abstractions.

## Non-goals

- True linear-phase filtering (would require a long linear-phase FIR,
  adding tens of milliseconds of latency to the whole passthrough path —
  rejected during design in favor of low-latency IIR; see "Filter type"
  below).
- Textbook-exact per-stage-Q higher-order Butterworth design (would
  require new generalized coefficient-design code; rejected in favor of
  reusing the existing fixed-Q design functions — see "Filter type"
  below).
- Per-band (independent high-pass-only or low-pass-only) control. The
  filter is a single combined bandwidth limiter.

## Filter type

Two design questions were resolved during brainstorming:

**Butterworth vs. Linkwitz-Riley.** Linkwitz-Riley filters exist to solve
crossover recombination: when a signal is split into parallel low-pass and
high-pass paths and summed back together, a plain Butterworth split leaves
a peak at the crossover point; LR (built from cascaded identical
Butterworth stages) fixes that by making the recombined magnitude flat.
This filter has no split/recombine step — it's a single series path
(high-pass then low-pass) — so LR's defining advantage does not apply.
The filter uses a Butterworth-based cascade.

**Cascade approximation vs. true higher-order Butterworth.** The existing
`filtDesign2ndButLowPass`/`filtDesign2ndButHighPass` functions
(`dsp/ptutil/Filt/Fil12But.cpp`) implement a fixed-Q (~0.707) 2nd-order
Butterworth design with no Q parameter. A textbook N-th-order Butterworth
requires cascading N/2 stages each with a *different* Q (per the standard
Butterworth pole-angle layout). This design instead cascades multiple
*identical* fixed-Q stages at the same cutoff. This is a deliberate
approximation: steeper-than-single-stage rejection far from cutoff, with
some passband droop starting below the nominal cutoff and a rolloff that
is only asymptotically N×12dB/octave. This was chosen because:
- It reuses existing, already-tested coefficient-design code as-is.
- The droop and imprecision are concentrated at 20Hz/20kHz — the edges of
  human hearing — where they are inaudible in practice.
- Implementing true per-stage-Q design would add new coefficient-design
  code for a difference that doesn't matter at these specific cutoffs.

**Linear phase.** The filter is IIR and therefore not linear-phase; phase
distortion is concentrated in the transition bands around 20Hz and 20kHz,
outside normal audible/perceptible range. A linear-phase FIR alternative
was rejected because a FIR filter steep enough to matter at a 20Hz cutoff
needs thousands of taps, adding meaningful (tens of ms) latency to the
entire system-wide audio passthrough path — unacceptable for a real-time
tool.

## Steepness presets

Three presets, each a cascade of N identical 2nd-order Butterworth
sections per band (high-pass at 20Hz, low-pass at 20kHz), run in series:

| Preset   | Sections/band | Asymptotic rolloff |
|----------|---------------|---------------------|
| Gentle   | 1             | ~12 dB/octave       |
| Standard | 4             | ~48 dB/octave (default steepness value when the filter is enabled) |
| Steep    | 8             | ~96 dB/octave       |

## Architecture

Three layers, each following an existing pattern in this codebase:

1. **DSP engine** (`dsp/`) — the filter cascade runs inside
   `DfxDspPrivate::processAudio` (`dsp/DfxDspPrivate.cpp`), applied
   *after* the existing `dfxpUniversalModifySamples()` call — i.e., as the
   last processing step before the (already in-place, `float`-format,
   per `AudioPassthruPrivate.cpp`) buffer is handed back for playback.
   This was chosen (over pre-processing the input) so the filter also
   cleans up any sub-20Hz/ultra-20kHz content introduced by upstream
   effects (EQ, bass boost, maximizer, etc).

2. **Public API** (`dsp/include/DfxDsp.h`) — new methods on `DfxDsp`,
   mirroring the existing `eqOn(bool)` / `powerOn(bool)` style:
   - `enum BrickwallSteepness { Gentle = 0, Standard = 1, Steep = 2 }`
   - `void brickwallFilterOn(bool on)`
   - `bool isBrickwallFilterOn()`
   - `void setBrickwallFilterSteepness(BrickwallSteepness steepness)`
   - `BrickwallSteepness getBrickwallFilterSteepness()`
   - `void brickwallFilterPreviewOn(bool on)`
   - `bool isBrickwallFilterPreviewOn()`

3. **GUI** (`fxsound/Source/GUI/FxSettingsDialog.cpp`,
   `AudioSettingsPane`) — a new settings group, plus new pass-through
   methods on `FxController` (mirroring `setAlwaysOnTop`/`isAlwaysOnTop`)
   that call into the `DfxDsp` instance and persist to `Settings`.

## Real-time safety

All coefficient computation happens on the control thread only: when the
filter is switched on, when the steepness changes, or when
`setSignalFormat` reports a new sample rate. Per-channel, per-section
filter state (the `in_minus1/2`, `out_minus1/2` history required by
`filtRun2ndLowPass`/`filtRun2ndHighPass` in `FiltRun.cpp`) is preallocated
at construction time for the maximum supported channel count and the
maximum section count (8 per band). `processAudio` itself only reads
precomputed coefficients and runs multiply-adds — no allocation, no
locking, no blocking calls, consistent with the real-time-safety
requirement called out in this repo's CLAUDE.md for `dsp/` changes.

## Preview ("Hear What's Removed")

When preview is active, `processAudio` computes, per sample:

```
residual = pre_filter_signal - filtered_signal
```

and writes `residual` to the output buffer instead of `filtered_signal`.
This isolates exactly the sub-20Hz/ultra-20kHz content being removed
(typically very quiet — rumble, hum, hiss).

Preview:
- Requires the filter to be on (enforced in the GUI: the preview control
  is disabled/unavailable unless the master toggle is on).
- Replaces normal output while active (mutually exclusive with normal
  playback).
- Auto-reverts to normal output when: the preview control is toggled off,
  the filter is turned off, FxSound is powered off, or the Settings
  dialog is closed.
- Is session-only — never persisted. Always off on app launch.

## Persistence

Two new keys in the existing `Settings` store
(`fxsound/Source/Utils/Settings/Settings.cpp`), read/written the same way
as `power` / `always_on_top`:

- `brickwall_filter_on` (bool, default `false`)
- `brickwall_filter_steepness` (int, default `1` = Standard)

Preview state is not persisted (see above).

## GUI details

In `AudioSettingsPane` (`FxSettingsDialog.h`/`.cpp`), a new group:

- A master `ToggleButton` — "Brickwall Filter (20Hz - 20kHz)".
- Three radio-style `ToggleButton`s (single radio group) for steepness —
  "Gentle", "Standard", "Steep" — each with `setTooltip()` text explaining
  the dB/octave rolloff and the steepness/CPU/phase-distortion trade-off.
  Enabled only when the master toggle is on.
- A toggle `TextButton` — "Hear What's Removed" — starts/stops preview.
  Enabled only when the master toggle is on; label reflects active state
  (e.g. "Stop Previewing" while active).

The dialog's existing `TooltipWindow tooltip_window_` member already
displays tooltips for any component in the dialog, so no new tooltip
infrastructure is needed.

`FxController` gains pass-through methods (`setBrickwallFilterOn`,
`isBrickwallFilterOn`, `setBrickwallFilterSteepness`,
`getBrickwallFilterSteepness`, `setBrickwallFilterPreviewOn`,
`isBrickwallFilterPreviewOn`) that call into the `DfxDsp` member and
persist via `settings_`, following the exact pattern of
`setAlwaysOnTop`/`isAlwaysOnTop` (`FxController.cpp`).

## Edge cases

- **Mono playback devices**: `AudioPassthruPrivate.cpp` already skips all
  DSP processing for mono devices (a documented existing workaround for a
  crash). The brickwall filter is skipped there too, consistent with
  every other effect.
- **44.1kHz Nyquist proximity**: at 44.1kHz, 20kHz is 90.7% of Nyquist
  (22.05kHz). The existing unwarped Butterworth design formula used
  elsewhere in this codebase (e.g. `Play32Butter.c`,
  `r_omega = 2*PI*fc/fs`) has known frequency-placement error as fc
  approaches Nyquist. This will be verified during implementation (see
  Testing) and the target frequency adjusted if needed so the actual
  -3dB point lands close to 20kHz at 44.1/48kHz.
- **Sample rate / channel count changes**: trigger a coefficient/state
  recompute, using the same `setSignalFormat` lifecycle already in place.

## Testing plan

- **Frequency response verification**: write a small check (evaluating
  the cascaded biquad transfer function at `z = e^(jω)`, similar in spirit
  to the existing `filtCalcFirResponse` in `FiltCalcFilterResponse.cpp`
  but for this IIR cascade) confirming the -3dB points land close to
  20Hz/20kHz and rejection is substantial (e.g. at 10Hz and 30kHz) at
  44.1kHz, 48kHz, and 96kHz.
- **Manual verification**: build and run locally (requires the FxSound
  virtual audio driver already installed, per this repo's CLAUDE.md), and
  in the Settings dialog:
  - Toggle the filter on/off and confirm passthrough audio still works.
  - Switch between the three steepness presets and confirm tooltips
    display correctly.
  - Toggle preview on and confirm it plays back only very quiet
    removed-content audio, and that it reverts to normal output when
    toggled off, when the filter is turned off, when FxSound is powered
    off, and when the Settings dialog is closed.

## Review notes

This change touches `dsp/` (the real-time DSP engine), which this repo's
CLAUDE.md flags for extra scrutiny. Per that guidance, the DSP-side
changes (`DfxDspPrivate.cpp`, `dsp/include/DfxDsp.h`, and any new files
under `dsp/ptutil/Filt/`) should be flagged explicitly for human review
before merge, with particular attention to real-time-safety (no
allocation/locking/blocking in `processAudio`) and the frequency-response
verification described above.
