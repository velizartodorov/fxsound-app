# Filters page: low cut, high cut, stereo to mono

## Intent

FxSound is used for home listening and for live disco sets. The low cut filter
(commit `27b6a2c` on `feature/brickwall-filter`) already removes rumble below a
chosen frequency. Two more tools are wanted for the same use:

- **High cut**: roll off harsh or useless top end (14 kHz and up).
- **Stereo to mono**: fold bass below a chosen frequency to mono, so the low end
  is centred and punchy on club rigs and single-sub setups, while everything
  above stays stereo.

All three live on **one page**, each with its own on/off and its own
"hear what's removed" (Δ) button, plus a global on/off and a global Δ.

## Goals

- Low cut, high cut and stereo to mono, each independently switchable and
  previewable, on a single page of the existing `FxAudioControls` panel.
- Same sound quality, click-free behaviour and real-time safety the low cut
  already has.
- The Q control goes away everywhere; Q is fixed.

## Non-goals

- No new pages. The flip button still cycles Effects → EQ → this page.
- No per-filter slope. Low and high cut share one slope.
- No adjustable Q, no filter type selection (Butterworth only).
- No panel resize: the page fits the existing 168×257 `FxAudioControls` size.

## Anti-goals (would be a failure even if it "works")

- Clicks, pops or level jumps when toggling switches, Δ buttons, or moving
  sliders.
- Any allocation, lock or blocking call on the audio thread.
- Stereo to mono changing the overall level or tone of mono content (the mid
  signal must pass untouched).

## Page layout (decided with the user)

**Current layout (user decisions through 2026-10-26, supersedes the lists
below):** the global ⏻ and "Global" title sit top-left on the flip button's
line. There is no slope control: low and high cut use a fixed 48 dB/oct
(`DfxDsp::CUT_DEFAULT_SLOPE_DB`; saved slope settings are ignored). Each of Low
Cut, High Cut and Stereo to Mono is three rows: the title (12 px, same font as
before), a value row where `[value][⏻][Δ]` move together centred above the
slider thumb (kept within the slider's width), and a full-width slider. Restore
Defaults is bottom-left and the global Δ bottom-right.

Original layout, top to bottom, mockup approved in chat (historical):

1. **Slope** slider: label "Slope", value text e.g. "24 dB/oct". Steps are the
   existing `LOW_CUT_SLOPES_DB` (6, 12, 18, 24, 36, 48). Applies to low cut and
   high cut.
2. **Low cut** block: header row `Low Cut ........ [⏻][Δ]`, then a "Cutoff"
   slider. Range 0–200 Hz as today, 0 shows "Off".
3. **High cut** block: header row `High Cut ....... [⏻][Δ]`, then a "Cutoff"
   slider. 14 000 Hz up to the top; the top position shows "Off" (filter
   bypassed).
4. **Stereo to mono** block: header row `Stereo to Mono . [⏻][Δ]`, then a
   slider 0–150 Hz, 0 shows "Off".
5. **Bottom row**: `[⏻] Global ...... [Restore Defaults] [Δ]`. The global power
   switch sits bottom-left so it lines up with the per-filter power switches
   (explicit user request); Restore Defaults and the global Δ are on the right.

Filter blocks (changed with the user on 2026-10-26): the title sits on its own
line, like the "Steepness" title and in the same font (theme normal font,
12 px); below it the slider row ends with the per-filter ⏻ immediately left of
the Δ, Δ rightmost. Per-filter ⏻ buttons are smaller than the global one
(16×16 px button, 12 px icon). The global ⏻ and its "Global" title sit at the
top-left, on the flip button's line; Restore Defaults is bottom-left and the
global Δ bottom-right.

Per-filter power buttons reuse `FxPowerButton`; Δ buttons reuse the existing
`PreviewButton` look; slider value labels reuse the existing `ValueSlider` /
`FxAudioSlider` "Off" text behaviour. Exact pixel positions are left to the
builder but must fit 168×257 with the flip button and existing margins.

## Behaviour

Changed with the user on 2026-10-26 ("audition with filters off", multiple Δs);
this replaces the earlier one-Δ-at-a-time, greyed-when-off rules.

### Switches

- **Per-filter ⏻** turns that filter on/off. It no longer greys anything out.
- **Global ⏻** off bypasses all three filters and makes every Δ unavailable
  (greyed), releasing any that play (user decision 2026-10-26). Per-filter
  switch states are kept, so global on restores exactly the previous setup.
  Sliders and switches stay usable.
- A filter actually processes audio only when global ⏻ is on, its own ⏻ is on,
  and its slider is not at "Off".
- Sliders, titles, per-filter ⏻ and Steepness are never greyed by switches, so
  a filter can be set up and auditioned before it is switched on. Restore
  Defaults and the global ⏻ are always enabled.
- Every switch toggle is a click-free crossfade: while the chain runs, a
  filter whose slider is not at "Off" is designed and kept running whatever
  the switches say, and the switches only choose which filters are heard.
  When a toggle leaves nothing heard and no Δ playing, the chain stops; the
  next switch-on fades it back in from a fresh state (the fade masks the
  filters' start-up).

### Δ (hear what's removed)

- **Low cut Δ**: plays only the lows the low cut removes.
- **High cut Δ**: plays only the highs the high cut removes.
- **Stereo to mono Δ**: plays the stereo difference that the mono fold removes
  below the cutoff (the low-frequency side signal: + on left, − on right).
  Silent if the material is already mono down there.
- With global ⏻ on, each Δ can be pressed regardless of its own ⏻:
  - filter heard (own on): Δ plays what it removes;
  - filter not heard (own off): Δ plays what it **would** remove at its
    current slider setting; normal audio stays unfiltered by it, and
    releasing Δ returns normal audio.
- With global ⏻ off, every Δ (individual and global) is unavailable.
- Switching a filter's own ⏻ off releases its Δ if it was playing (user decision
  2026-10-26); pressing the Δ again afterwards auditions the filter as above.
- **Several Δs at once**: the individual Δs (Low Cut, High Cut, Stereo to
  Mono) toggle independently; the preview plays the sum of the pressed ones.
  Pressing an active Δ releases it.
- **Global Δ** is a shortcut for "every filter whose own ⏻ is on (and slider
  not at Off)", played together. Pressing global Δ
  clears any individual Δs; pressing an individual Δ while global Δ is active
  turns global Δ off and starts from just that Δ.
- With global ⏻ on, an individual Δ is always available. With its slider at "Off" (low cut 0 Hz,
  high cut at the top "Off" position, stereo to mono 0 Hz) it plays silence,
  since the filter removes nothing (user decision 2026-10-26). The global Δ
  plays the filters whose own ⏻ is on (silence for those at "Off"); it is
  unavailable when no filter has its own ⏻ on, and is released when that
  happens.
- Changing the Δ selection while one plays (another Δ, an added or released
  Δ, global ↔ individual, or the global set changing because an own ⏻
  toggled) crossfades straight from the old removed signal to the new one
  over `LOW_CUT_CROSSFADE_MS`: the normal programme is never heard during the
  switch, and the auto-leveller keeps running (no reset), so loudness stays
  continuous. Only starting or stopping a preview ramps between the normal
  programme and the removed signal. A change arriving mid-crossfade either
  reverses it (back to the selection being faded out) or waits for it to
  finish (at most one crossfade) and then crossfades on (revised 2026-10-26:
  the earlier ramp-out-then-in let the programme leak through on every Δ
  switch).
- Leaving the page (or closing the window / the existing `stopFilterPreview()`
  call sites) releases all Δs, as the low cut preview does today.
- Δ playback keeps the existing auto-levelling and silence hold of the low cut
  preview, applied to whichever removed signal is being played. An
  instant-attack peak follower (50 ms hold, then 100 ms release) caps the applied gain so the
  levelled peak stays at or below 0.25 (never below unity gain), so a loud
  passage or a switch from a quiet Δ to a loud one cannot burst while the
  slower rms detector catches up.

API: `DfxDsp::FilterPreview` values are bit flags (`PreviewLowCut = 1`,
`PreviewHighCut = 2`, `PreviewMonoBass = 4`, `PreviewAll = 8` for the global Δ
mode). `setFilterPreview(int)` / `getFilterPreview()` /
`isFilterPreviewAvailable(int)` take a selection; `DfxDsp::toggledFilterPreview()`
gives the selection after a button press. The DSP drops the global mode when it
becomes unavailable. The engine plays the union of the selected stages, or in
global mode the own-on stages; a selected stage at "Off" has no filter and
contributes silence.

### Q

Fixed at `DfxDsp::LOW_CUT_DEFAULT_Q` (0.71) for low and high cut. The Q slider,
`setLowCutQ`/`getLowCutQ` on `DfxDsp`, `DfxDspPrivate` and `FxController`, and
the `low_cut_q` setting are removed. A stale `low_cut_q` value in existing user
settings is ignored.

## DSP

### High cut

Butterworth low-pass at the high cut cutoff, slope from the shared slope
setting, Q 0.71, designed with the existing `filtLowCutDesign()` with
`i_high_pass_flag = 0`. "Off" (top of slider) means no sections.

### Removed signal for Δ (mirror filters)

Decided by the user on 2026-10-26, reversing the earlier subtraction decision
(removed = input − output). Subtraction leaks: input − HP(input) only falls at
6 dB/oct above the cutoff whatever the slope (a 30 Hz / 48 dB/oct low cut leaks
+5.8 dB at 60 Hz and −5.9 dB at 300 Hz), so the low cut Δ was full of
low-mids.

Every stage now has a **mirror** cascade that computes what it removes, run on
the same stage input as its main filter:

- Low cut: Butterworth low-pass at the same cutoff, slope and Q.
- High cut: Butterworth high-pass at the same cutoff (same 0.45 × fs clamp),
  slope and Q.
- Stereo to mono: side low-pass, LR4 (two identical 2nd-order Butterworth
  low-pass sections at the mono cutoff) on `S = (L − R) / 2`; removed is
  `+S_lp` on left, `−S_lp` on right, nothing on other channels.

The removed signal of a Δ selection is the sum of the mirror outputs of the
selected stages. Mirror sections live next to each stage's sections in
`FiltChainParams`, their histories in `FiltChainState`, and they run whenever
their stage is designed (history stays continuous); layout comparison and the
history carry-over include them.

Consequences, accepted by the user:

- Output + removed no longer equals the input exactly near a cutoff, so the
  global Δ is an approximation of "everything the filters remove" (the sum of
  each filter's mirror), not chain input − chain output.
- CPU: while the chain runs, every designed filter (heard or not) and every
  mirror runs, roughly twice the filter work of the heard filters alone. Still
  small for biquads, but not free.

The existing designer caps filter frequencies at 0.45 × sample rate
(`FILT_LOW_CUT_MAX_FREQ_RATIO`, 19 845 Hz at 44.1 kHz). A high cut cutoff above
that cap is clamped to the cap, not bypassed (my call): the slider keeps the
same steps at every rate, the value label shows what the user picked, and the
filter still cuts. Only the top "Off" position bypasses.

### Stereo to mono

Mid/side on the first two channels (front left/right):

- `M = (L + R) / 2`, `S = (L − R) / 2`.
- The side signal is high-passed at the mono cutoff with a fixed
  Linkwitz-Riley 24 dB/oct (two cascaded Butterworth 2nd-order, Q 0.71).
  It does **not** follow the shared slope slider.
- Output `L' = M + S_hp`, `R' = M − S_hp`. Mid is untouched, so mono content
  passes unchanged.
- Removed signal for Δ: `S_low = S − S_hp`, i.e. `+S_low` on left, `−S_low` on
  right.
- Channels beyond the first two pass through untouched. With fewer than two
  channels the stage is bypassed.

### Order

Stereo to mono first, then low cut and high cut, all at the current low cut
position (after `dfxpUniversalModifySamples()` in `processAudio()`). The global
Δ sums the three removed signals from that chain.

### Real-time safety and engine structure

Build on the existing low cut machinery in `DfxDspPrivate.cpp` / `u_DfxDsp.h`:
the designer thread, the triple-buffer `LowCutIirExchange`, the Dry/IIR output
sources with crossfades and fades, reset serials, format serials, and the
preview auto-leveller. Coefficient design stays on the designer thread;
the audio thread only swaps prepared parameter sets and runs biquads. No new
locks, allocations or blocking on the audio thread.

Every user-visible change (any switch, slider, Δ) must be click-free, using
the existing crossfade lengths (`FILTER_CROSSFADE_MS`, `FILTER_FADE_MS`).

Fixed-size state must grow to hold high cut and mono sections and their
per-channel histories; how (extend `LowCutIirParams` vs. new parallel
structures, renaming `LowCut*` to something broader) is the builder's call.

Extend the filter engine tests (`dsp/test/run_filter_engine_test.cmd`) and
the `dsp/ptutil/Filt/test/` tests to
cover: high cut response at its cutoff, mono stage leaving mono input
bit-identical-or-near and removing low side content, each stage's removed
signal equal to its standalone mirror cascade, the mirrors' stop-band
attenuation, removed of a union equal to the sum of each stage's, a stage
that is designed but not heard passing its input through exactly, switching a
filter on/off keeping every stage's history, and the Δ availability and
selection rules.

## Settings and controller

`FxController` gets getters/setters for each new value and persists them like
the low cut ones do. Keys (my call): `filters_on` (global), `low_cut_on`,
`low_cut_cutoff_hz`, `filters_slope_db` (falls back to existing
`low_cut_slope_db`), `high_cut_on`, `high_cut_cutoff_hz`, `mono_bass_on`,
`mono_bass_cutoff_hz`.

Migration (my call): today `low_cut_on` is the page's only switch. On first
load, if `filters_on` is absent, set it from the old `low_cut_on` value and
set the per-filter `low_cut_on` to true, so existing users hear no change.

Defaults (my call):

| Setting | Default |
|---|---|
| Global | off (matches today's low cut default) |
| Slope | 24 dB/oct |
| Low cut | on, 20 Hz |
| High cut | on, cutoff at "Off" (top of slider), so it has no effect until moved |
| Stereo to mono | off, 125 Hz |

Restore Defaults resets all of the above except the global switch, which keeps
its state (my call: the reset button shouldn't silently turn processing off).

The Δ state is never persisted.

## Risk flag

This touches `dsp/` (real-time audio path). Per CLAUDE.md, commits and the PR
must call this out as higher-risk and ask for human review of the DSP changes
before merge. `audiopassthru/` is not touched.

## Left to the builder

- Exact pixel layout within 168×257.
- Internal naming and structure of the extended engine state.
- High cut slider step size and where exactly "Off" sits at the top end
  (the mockup used 500 Hz steps from 14.0 to 20.0 kHz, then Off).
- Translation strings and tooltips, following the existing low cut wording.
