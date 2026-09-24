# pattern — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **source** for Resolume Arena/Avenue that draws a
tracker's pattern editor and lets the music write it: a row cursor on the
tracker's own clock, and an onset detector per channel that writes notes into
the pattern from the host's 64-bin spectrum. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/pattern`; built and released 2026-09-24 as v0.1.0,
**never loaded into Resolume on macOS**.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, every
number in the harness and where its tolerance comes from, the negative
controls, the mutation test, the traps this build actually hit, what is
verified and what is assumed, and the decisions taken without asking.

---

## The one idea

**Run the tracker backwards.** A Protracker-style tracker plays a pattern of 64
rows at a rate set by Speed (ticks a row) and BPM (a tick is 2.5 / BPM s).
FFGL gives a plugin no audio output, only the spectrum of what is playing. So
the row cursor runs on the real tracker clock — four rows to a beat at the
default speed of 6, rate-locked to the host's BPM — and an onset in a channel's
band writes a note into that channel on the current row. Its pitch is the
band's peak bin through the bin law, its volume a `Cxx` effect from the onset's
strength, its sample number the channel number. After row `3F` the cursor wraps
and the next pass overwrites the last.

Everything else follows from that rather than being arranged: the cursor steps
on the beat because it runs on a clock; a busy passage fills every row and a
sparse one leaves dots because nothing is written that was not heard; an
arrangement change shows as old notes being replaced because the ring clears a
row as it enters it.

---

## Shape of the code

    source/tracker/Bands.*    bins to hertz under a law, hertz to tracker notes,
                              the 64 bins split into channels. Pure functions.
    source/tracker/Tracker.*  the row clock (anchor + base in double), the ring,
                              the spectral-flux onset detector primed on frame
                              one, the autocorrelation tempo detector, the VU
                              meters. No GL, no host, no time of its own.
    source/Screen.*           the picture as a grid of 6x8 cells (glyph, fg, bg)
                              plus the VU rectangles; the integer origin and
                              scale for an output size.
    source/Shaders.cpp        one triangle; pixel -> cell by integer arithmetic;
                              a glyph bit from the font texture; a palette entry.
    source/Render.*           the two textures (R8 font, RGBA8UI screen) and the
                              draw into the host's FBO. Three palettes.
    source/Pattern.*          the plugin: parameters, the host's clock, beat info
                              and sample rate, Advance / BuildScreen / LayoutFor.
    source/Controls.h         parameter ids, 0..1 to engineering units.
    source/{Clock,Diag,Font}  carried from graticule via needle.
    tools/pntest/main.mm      the harness: the checks, --out, --pipe, --bench,
                              synthetic spectra, a WAV reader with a 2048-point
                              FFT, macroblock's spectrum file, the cue script.
    tools/sweep.py            no control is silently dead, at two rasters.
    tools/verify.sh           all of it. Bash: the pipe step reads PIPESTATUS.

---

## Every number in the harness

The tracker checks open no GL context: they read `Tracker`'s published state
through the plugin's own `Advance`, which is what `ProcessOpenGL` calls, and
write the spectrum with `SetParamElementValue`, which is what a host calls.
`--grid` is the one check that reads pixels.

| Check | The number | Where it comes from |
| --- | --- | --- |
| `--timing` | **0** wrong frames over six ten-minute runs (36,000 at 60 fps, 30,000 at 50, 18,000 at 30) | Expected rows = `floor( 2 i BPM / ( 5 Speed fps ) )` in int64, exactly, from the row period `Speed x 2.5 / BPM`; with swing 1/4 the odd row starts when `8 i BPM >= ( 8 P + 5 ) x 5 Speed fps`. The host clock is sent in milliseconds from frame 29,940,000 (499,000,000 ms, the fleet's measured magnitude) and from frame 0. The plugin's 1e-6-row allowance in the floor is the host's representation error with a margin: at 5e5 s a double resolves 6e-11 s = 5e-10 row; at 30, 50 or 60 fps no frame that is not ON a row boundary is closer than 1/1800 row to one (the phase's fractional part is a multiple of 1/(5 Speed fps)). The BPMs are integers delivered by `SetBeatInfo`, or the Manual control at 0.5 = 130 exactly, because `60 + 140 x 0.4642857f` is 124.999998 and a period 2e-8 off drifts 1e-4 row in ten minutes. |
| `--timing` tempo change | **0** row jumps over a minute across 125 -> 90 -> 180 BPM | The rows-elapsed counter never goes backwards and never advances more than one per frame: the re-anchor carries the phase across. |
| `--onset` | **1** cell written, the right one, for every channel at 4 and 8 channels under both laws; **1** for a burst held 8 frames across a row boundary | Exact integers on the pattern's cells. The burst fills the channel's band as the plugin reports it (`BandEdge`), so the check does not transcribe the split. |
| `--prime` | **0** false notes in the first second, from a clock at 40 s and at 499,000,000 ms; a real onset at 1.5 s heard | Stationary noise 0.3 per bin, +-5 % per frame from an integer hash. Its per-band flux is about 2.5 % of the band's level; the seeded floor is 12.5 % of the level times the threshold ratio 4.24, a margin of about 20. |
| `--pitch` | **0** of 64 bins differ from the predicted note under Linear and Log, folded and unfolded; **3** in range under Linear, **19** under Log; monotone; 96 kHz moves bin 1 from C-3 to C#3 | The harness computes the frequency from the law's formula as written in Bands.h's comment (not by calling it) and the note by `round( 69 + 12 log2( f / 440 ) ) - 48`. The two transcriptions agree bit for bit; a disagreement in the rounding of a frequency that sits exactly between two semitones would show here, and none does. |
| `--ring` | **3 of 3** pass-0-only rows cleared; both-pass rows carry pass 1; untouched rows empty; with Keep on, 3 of 3 survive; a jump of 520 rows clears all | Exact on cells. Onsets are placed on the first frame the plugin reports the cursor on a target row. |
| `--detected` | within **+-1 BPM by 3.00 s** at 120/60 fps, 100/50, 150/60, 120/24 fps, holding for 12 s; the row clock at 120.00 over a host saying 77 | The detector needs three seconds of envelope before it speaks (300 samples at 100 Hz) and reports once a second, so 3.00 s is its first opportunity. +-1 BPM is the spec's allowance; the metronome's lag is a whole number of grid samples, so the parabolic peak lands on it exactly and the measured error is the frame-grid resampling (0.00 to 0.01). |
| `--grid` blocks | **0** of up to 57,600 s x s blocks not one colour | The shader divides `( pixel - origin )` by an integer scale and then by the cell size, all in integers on non-negative operands, so a colour boundary can only fall on a multiple of the scale from the origin. No tolerance: byte equality within a block. |
| `--grid` outside | **0** of up to 172,800 pixels outside the screen not the background, +-1 code value | Every palette entry is a multiple of 1/15 or 1/3, so its 8-bit value is a whole number with room to spare (0.6666667 x 255 = 170.00001); +-1 is the float-to-unorm allowance and nothing else. |
| `--grid` glyphs | **0** of 576 (or 336, 192, 768) probed cell pixels differ from the font table, with 144 (or so) of them lit | Up to a dozen cells the layout says are wholly on the raster, on the cursor row, the row above it, the counter row, and then any row when those are cropped off; `litExpected > 0` so a run of blank cells cannot pass by having nothing to draw. Same +-1 as above. |
| `--grid` Auto | scale = min( 1280 / 288, 720 / 200 ) = **3**; origin `( 1280 - 288 x 3 ) / 2`, an integer | Integer arithmetic on the CPU, read back through `LayoutFor`. |
| `--grid` resize | 256 cells identical after 640x360 -> 320x180 mid-run | The pattern is CPU state; the screen texture is re-uploaded every frame. Cheap, and it records the property the addendum asks for. |
| sweep | every control changes >= 1 subpixel at 640x360 and 320x180 | No tolerance; see the traps for the four contexts it needs. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's GPU. |

**Negative controls, all shipped and all run by `verify.sh`:**

| Check | Perturbation | Result |
| --- | --- | --- |
| `--timing` | the phase in float at the host's magnitude (`TrackerDebug::floatPhase`) | 2,400 of 36,000 frames wrong, first at frame 7 |
| `--onset` | a detector that fires on level rather than change: no memory of the last frame and no rising-edge test (`levelDetector`) | the held burst writes 2 cells |
| `--prime` | the detector starts from zero on frame one (`noPrime`) | 4 false notes in the first second |
| `--pitch` | every pitch read one semitone sharp (`detuneSemitones = 1`) | 64 of 64 bins differ |
| `--ring` | a new pass never clears a row (`noClear`) | 0 of 3 cleared |
| `--detected` | the detected lag scaled by 1.1 (`lagBias`) | reads 109.09, never within +-1 |
| `--grid` | a fractional scale of 1.5 in the shader's pixel-to-cell step (`debugFractionalScale`) | 576 glyph pixels off the table |

None of the perturbations is reachable from a parameter.

**Mutation test, run on the committed tree (2026-09-24).** One character of the
shipped GLSL: in `main`, the top-down pixel `uSize.y - 1 - int( gl_FragCoord.y )`
became `uSize.y - int( gl_FragCoord.y )`, a one-row shift. `--grid` failed 36
assertions: every probed-glyph comparison (the cells are one row off the
table), every outside-the-screen comparison (70 pixels of the top row of the
screen now read as outside), and the block-uniformity comparisons at Scale 2, 3
and 4 (5,450 of 51,840 blocks at Scale 2, 640x360) — at Scale 1 every block is
one pixel and a whole-pixel shift keeps it uniform, which is why the grid check
has three classes and not one. `--timing`, `--onset`, `--prime`, `--pitch`,
`--ring` and `--detected` correctly did not fail: there is no shader in them.
Reverted with `git checkout -- source/Shaders.cpp` on a committed file, and
`git diff --stat` read empty before the rebuild; `--grid` then passed again.

## Would this hold on another rasteriser, at another raster?

- `--timing`, `--onset`, `--prime`, `--pitch`, `--ring`, `--detected`, `--names`,
  `--font`: **yes, by construction** — no GL context exists while they run. The
  raster does not enter. The one floating-point question in them, the row
  floor at a host time of 5e5 s, is argued above from the double's resolution
  and the frame grid's coarseness, and holds on any IEEE machine.
- `--grid` blocks: **yes** — integer arithmetic in the shader on non-negative
  operands; GLSL 4.10 integer division is exact. Both rasters run it, at
  Scale 1 through 4 and Auto (which is 1 at both), with the screen wider than
  the raster at the larger scales (a negative origin, a cropped screen).
- `--grid` outside and glyphs: **yes** — a `texelFetch` from an RGBA8UI texture
  is exact, a `texelFetch` from an R8 texture compared with 0.5 is exact for
  values of 0 and 255, and a palette constant written to an RGBA8 target lands
  within the unorm rounding allowance the GL spec permits; every palette entry
  was chosen to sit at least 1e-5 of a code value from a rounding tie.
- What has NOT been proved: any of this on a GPU other than this Mac's, or on
  Apple's software renderer (CI is written and has not run), or on llvmpipe.
- Where the raster could matter and does not: `LayoutFor` at 320x180 with 8
  channels gives a screen of 546 x 200 at Scale 1, wider than the raster, so
  the origin is negative and the left and right of the screen are cropped. The
  block check handles a block cut by the raster's edge by judging the part
  that is there, and the probe list skips cells that are not wholly on the
  raster — which is the fix recorded in the traps.

---

## The traps

Ordered by how much time they cost.

**Seeding the onset floor with the band's level made the detector deaf for two
seconds.** The first priming rule set each band's floor to its level on frame
one — an upper bound on any flux, which guarantees `--prime`. It also meant a
clip that starts on a kick drum (level 2.3 in channel 0) needs the floor to
decay to under a kick's flux (2.0 over 4.24) before it hears the next one, and
at a one-second time constant that is 2.5 s. `--grid`'s "the drum pattern wrote
notes" assertion found it as 2 cells in 1.5 s. The seed is now an eighth of
the level: a stationary signal's frame-to-frame flux is a few per cent of its
level, so the margin in `--prime` is still about 20, and a real onset — whose
flux is comparable to the level — clears the seeded threshold at once.

**"No memory" is not a perturbation the onset check can see.** The intended
negative control dropped the flux's memory of the last frame, so a held burst
reads as a full onset on every frame. It wrote one cell, not two, because the
shipped detector also requires the flux to be *rising*, and a level held
constant is not. The rising-edge term is doing real work. The negative control
is now "a level detector" — no memory and no rising-edge test — which is what
the fleet's earlier detectors were.

**The probed cells were off the raster.** `--grid` probed column 0 of the
cursor row. At Scale 3 and 4 on 640x360, and Scale 2 upwards on 320x180, the
screen is wider than the raster and column 0 is cropped; the check read "0 of
0 pixels differ" and passed on nothing until the `glyphPixels > 0` guard made
it fail. The probes are now chosen from the cells the layout says are wholly
on the raster, and a `litExpected > 0` guard stops a run of blanks passing.

**A burst spanning two bands.** The `--ring` jump test injected bins 0..3 and
read 2 cells written where it expected 1: with the default split, bin 3 is
channel 1's. Every synthetic burst now takes its bins from the plugin's own
`BandEdge`.

**needle's Clock has no `ForceMilliseconds`.** copperlist's does; it was added
here so `--timing` and `--prime` can drive the millisecond path with no GL.

**The SDK never initialises `sampleRate`.** `CFFGLPlugin`'s constructor sets
`bpm` and `barPhase` and leaves `sampleRate` uninitialised, so the base class
member is never read here: `SetSampleRate` is overridden into a member of this
class that defaults to 44100 and accepts 8 kHz to 384 kHz.

**`git checkout` of an untracked file does nothing** (copperlist's trap). The
mutation test was therefore run only after the first commit, and the revert
checked with `git diff --stat` before rebuilding.

**zsh's `=word` expansion.** A shell one-liner with `echo ======` failed with
"===== not found" in zsh, which expands a leading `=` to a command path. Not a
repo trap, but it cost a round trip; verify.sh is bash for a different reason
(PIPESTATUS) and does not hit it.

Inherited from the fleet and honoured without incident: the OBJECT library;
`SetTextParameter` returning success for the About block; the 0..1 clamp on
STANDARD defaults (the counts here are `FF_TYPE_INTEGER`); an option's range
reading back 0..1; `StoatworksAboutParams.h` after the SDK; the synthetic clock
in the harness; integer hashing for the harness's jitter; the reserved GLSL
words; the universal-arch latch; the scoped bindings clearing rather than
restoring (this plugin binds by hand and puts things back); the 16-character
name limit (`--names` passed first time; the longest here is 12).

---

## Decisions taken without asking

**Bin Law is Linear / Log, the frequency layout; Bin Value is Magnitude /
Power.** The spec calls the option `Bin Law` "as needle has it", but needle's
Bin Law is magnitude-or-power. For a pattern editor the layout is the question
that matters — a band is a range of bins and a pitch is a bin's frequency — so
`Bin Law` is the layout, and the value question keeps needle's semantics under
a second name. Both default to the fleet's habit (Linear, Magnitude).

**Fold Octaves is on by default.** Under Linear at 44.1 kHz only bins 0..2 are
inside C-1..B-3 (bin 3 is 1206 Hz, D6), so an unfolded default shows `---` in
nearly every cell and reads as broken. Folding moves the pitch by whole
octaves: the note name is the pitch class the band's peak has and only the
octave digit is a fiction. Off gives the spec's literal behaviour.

**Keep Notes is off by default.** Clearing a row as the cursor enters it is
what makes the ring legible: a sparse passage leaves dots, a busy one fills,
an arrangement change shows as replacement. With Keep on the pattern fills to
solid over a few passes and never empties. The spec asked which and why.

**The note range is C-1..B-3 with C-2 as middle C** (MIDI 48..83), the usual
reading of the Amiga convention that a sample tuned to middle C plays there at
C-2. Nothing standardises it; it is one constant (`kFirstMidi`).

**Rate lock only, no phase lock.** `SetBeatInfo` also carries `barPhase`, and
a row 0 that lands on the host's downbeat would look better. Nothing has
measured how Resolume drives that value — float, how often, whether it wraps
cleanly — and a phase lock built on an unmeasured signal is how a cursor
stutters on a wall. Row 0 is where the clip started. Open question 1.

**The row phase is `base + ( t - anchor ) / T`**, re-anchored when the period
changes, rather than a pure function of `t` (a tempo change would then
re-derive the row from the start of time and jump) or an accumulator (drift,
and at 5e5 s the deltas lose bits). copperlist's accumulator with re-anchoring,
in seconds rather than fields.

**A seek backwards is a loop point.** A host time smaller than the last one
carries the phase across and lets no time pass, so a looping clip keeps its
cursor moving rather than snapping to row 0.

**The Cxx volume is `64 log2( flux / threshold ) / 3`, clamped to 01..40.** An
onset just over the line is quiet, one eight times over it is full. On the
synthetic WAV real onsets read 01..18 because the floor adapts to a busy
signal; that is a tuning question (open question 3), not a correctness one,
and the harness asserts only that the value is in 01..40.

**The strongest onset in a row wins the cell.** Two onsets in one channel in
one row (a row is 120 ms at 125 BPM, the refractory 50 ms) keep the louder.

**The VU meter is 48 steps falling one step every 20 ms**, set to
`volume / 64 x 48` on a note. A vblank-driven meter's rate, one pixel a field.
This repo's own model; no figure was read from any tracker.

**The screen is a character grid of 6 x 8 cells**, graticule's 5 x 7 glyph with
a one-pixel gutter. Not a tracker's font, and stated as such in
ATTRIBUTIONS.md. Designing a fresh 8 x 8 face was considered and set aside for
0.1.0: graticule's is in-repo, MIT, original, and has every glyph the screen
uses (`--font` prints them).

**Scale 0 is Auto**, the largest whole scale at which the screen fits both
ways, never less than 1; a screen that does not fit at 1 is centred and
cropped. Scale 1..8 are exact.

**Rows Visible is any count 3..33**; the cursor is at index `visible / 2`, so
an even count has one more row below the cursor than above.

**Header counters:** POS is the pass modulo 128, PAT the pass modulo 64, both
in hex; BPM shows the tempo in use with `*` when Detected has settled and `?`
while it has not; SPD the speed. No song, no order list.

**The detected tempo prefers the half lag.** A pulse train correlates at every
multiple of its period, so the longest lag in range would otherwise win half
the time; a lag at half the best is taken when it scores at least 70 % as
well. 60..200 BPM, six-second window, 100 Hz grid, once a second, trusted when
the normalised peak is at least 0.2.

**The harness's `--frames 0` always exits 1.** Its only way to end is the
reader hanging up, and the fleet's rule is that a hang-up is exit 1 rather
than SIGPIPE's silent 141. Documented in the harness header; `--frames N`
ends cleanly with 0.

**Two new negative-control knobs and one plugin flag** (`levelDetector`,
`detuneSemitones`, `debugFractionalScale`) exist only for the harness. None
is reachable from a parameter, and `debugFractionalScale` is 0.0 on every
frame the plugin renders; the shader's `uFrac` branch is the cost of a grid
check that can fail.

**No presets and no OpenFX port.** Neither is required for 0.1.0. The user
guide (`docs/USER-GUIDE.md`) and the browser demo (`demo/`) were added at
release.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple Silicon, macOS 26.4),
2026-09-24**, `tools/verify.sh` all green: the numbers in the tables above; 17
controls live at 640x360 and 320x180; the pipe's byte count and its exit
status on a hang-up; a universal bundle that exports `plugMain`, ad-hoc signs,
instantiates under `oxbow selftest` and probes as **SW Pattern / PN01 / source
/ inputs 0..0 / 22 params**. `--wav` was exercised on a synthesised ten-second
track (a 124 BPM kick, snare, hats and bass written with Python's stdlib): 600
frames out at 60 fps, notes landing in four channels through the plugin's own
audio input; and a cue script drove `Host BPM`, `Theme` and `Bin 30`.

**The cost**, `pntest --bench`, fastest of five passes of 200 frames with the
drum loop playing and other plugin builds loading this machine: **0.041 ms at
1280x720, 0.054 at 1920x1080, 0.128 at 3840x2160** (0.2–0.8 % of a 60 fps
frame). One triangle, two small textures, a few hundred multiply-adds on the
CPU; the screen texture is re-uploaded every frame (at most 92 x 41 cells).

**Assumed, or not yet done:**

- **Never loaded into Resolume on macOS.** No real audio has reached it in a
  host. How the parameters present in the inspector there is untested. The
  harness has run on this Mac's GPU and on CI's software renderer (green on
  the release commit).
- **The 64 bins.** Layout, value law and sample rate are the operator's
  switches and the host's `SetSampleRate`, not measurements. An hour in Arena
  with a signal generator would settle all three for the whole fleet.
- **The detector's thresholds** (ratio 1.5..12, a one-second floor, 50 ms
  refractory, the 1e-4 absolute floor, the eighth-of-level seed) were set on
  synthetic spectra and one synthetic WAV through the harness's own FFT, whose
  normalisation is not Resolume's. Sensitivity is the hedge.
- **The tempo detector** has met a metronome and the harness's drum loop, and
  on the release video's synthesised groove it was wrong: see the traps.
- **`barPhase`** is unused (open question 1).
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by the backend's
  `sync-about.py` and `sync-attributions.py` since registration; the About
  block has five entries (`PT_ABOUT_BUTTON_4` carries the user guide).

**Found filming the release video** (2026-09-24, `stoatworks-backend/video/projects/pattern/render.py`,
a synthesised 125 BPM track through `--wav`, so the harness's FFT and
normalisation, not Resolume's):

- **A quiet band hears the leading edge of any sharp transient.** A 150 Hz
  burst with a hard attack wrote all eight channels on its first frame: the
  first block containing an onset holds only its first milliseconds, whose
  spectrum is broad, and a silent band's threshold is `kAbsoluteFloor` = 1e-4,
  far below a −60 dB leak of a 0.6 kick. Each band's floor learns the leak in
  about `kFloorSeconds`, after which the same kick stays in its band. The
  video's instruments got 6 ms attacks and 6th/8th-order Butterworths. Whether
  Resolume's FFT leaks the same way is unmeasured; the guide documents the
  behaviour rather than a fix, because the absolute floor is a tuning with no
  measurement behind either side.
- **A band with a sustained instrument in it adapts to that instrument.** With
  the bass under the kick (both in bin 0), eight channels wrote the kick in
  channel 2 (bins 1–2, the leak) and never in channel 1: bin 0's floor was
  held up by the bass's own frame-to-frame movement and the kick failed the
  4.24 ratio there. Not a defect of the detector as specified; a fact about
  running it on programme material.
- **The tempo detector read the bar, not the beat.** Three takes read 62 for
  125 (a clap on two and four and a bass figure turning every bar correlate
  best at the whole bar), a syncopated first cut read 100 (its arpeggio gate's
  strongest recurrence was five sixteenths), and a straight kick-and-hats pulse
  read 62 as well. Tried: halving the best lag repeatedly at a 50 % score
  instead of once at 70 % — `--detected` still passed and the reading did not
  change, so it was reverted rather than shipped unverified. The video uses
  Host and Manual; the guide records the limitation. Open question 7.

---

## Open questions

1. **Should row 0 lock to the host's downbeat?** `SetBeatInfo`'s `barPhase`
   could place the cursor: `rows = 16 x barPhase + 64 x bars`, or a soft
   correction of the anchor towards it. It needs a measurement of how Resolume
   drives that float — cadence, resolution, wrap — before it is safe.
2. **What Resolume actually puts in the 64 bins**, and at what sample rate.
   `Bin Law`, `Bin Value` and the 44.1 kHz default are the hedge.
3. **Is the volume law right for programme material?** On the synthetic WAV
   real onsets read `C01`..`C18` because the floor adapts to a busy signal and
   the strength is measured against it. A law that measured the onset against
   the band's recent maximum, or a slower floor, would read fuller. Untested on
   music through the host.
4. **Should a fresh 8 x 8 face replace graticule's 5 x 7?** The classic tracker
   look is a wider cell. It would be drawn here, not copied; it is a day's
   work and a `--grid` rerun.
5. **Is 64 rows in 7.7 s (at 125 BPM) the right ring length for a wall?** A
   32-row option exists; a 16-row one would make the overwrite visible every
   two bars.
6. **Should Detected tempo smooth its updates?** Each second's reading
   re-anchors the clock; on music with a weak pulse that may wander. A median
   of the last three readings is the obvious next step.
7. **Should the tempo detector weight the low band, or the onsets, rather
   than the total flux?** On the release video's groove the whole-envelope
   autocorrelation preferred the bar (see the traps). A kick-band envelope,
   or a comb over the candidate lags scored against their multiples, would
   be the two things to try, with a harness check on a synthesised groove
   rather than a metronome.
