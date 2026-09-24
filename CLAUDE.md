# pattern

A tracker's pattern editor written by the music as it plays, as an FFGL
**source** for Resolume Arena/Avenue. C++17/GLSL 4.10, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. Released as v0.1.0 on
2026-09-24; never loaded into Resolume on macOS.

Read `AGENTS.md` before touching the row clock, the onset detector, the band
split, or any tolerance in the harness.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64` (use a different
  directory, e.g. `build-dev`; `tools/verify.sh` deletes and rebuilds `build`)
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (untested; never run by this repo's own workflow)
- Render a frame offline: `./build/pntest --out /tmp/f.png --size 1920x1080 --frames 300 --audio beats --host-bpm 125`
- Set anything by name: `--set "Channels=1" --set "Theme=2" --set "Bin Law=1"`
  (options by index: Tempo Source 0 Host, 1 Detected, 2 Manual; Rows 0 = 64,
  1 = 32; Channels 0 = 4, 1 = 8; Bin Law 0 Linear, 1 Log; Bin Value 0
  Magnitude, 1 Power; Theme 0 Grey, 1 Blue, 2 Green)
- Feed it audio: `--audio beats|metronome|tone:N|noise|sweep|silence`
  (`--audio-bpm N`, `--audio-until S`), `--wav track.wav`, `--spectrum bins.txt`;
  the host's tempo: `--host-bpm N`
- Frames for video: `./build/pntest --pipe --size 1920x1080 --fps 60 --wav track.wav --host-bpm 124 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov`
  — a cue script is `frame  Parameter Name  value`; `Host BPM` and `Bin 0`..`Bin 63`
  are accepted there too. A reader that hangs up gets exit 1 (SIGPIPE is
  ignored); `--frames 0` runs until that hang-up and so always exits 1.
- List parameters: `./build/pntest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check + two sweeps, ~1 min; bash, not zsh)
- **The tracker, with no GL context at all:**
  - `./build/pntest --timing` — the cursor on floor( t / T ) mod Rows, every frame of ten minutes, host clock at 499 million ms
  - `./build/pntest --onset` — a burst in band k writes one note in channel k on the current row, nowhere else
  - `./build/pntest --prime` — loud audio already playing when the clip starts writes no false note
  - `./build/pntest --pitch` — a peak at bin j is the note the bin law predicts, both laws, folded and not
  - `./build/pntest --ring` — the next pass overwrites; a row with no onset is cleared, or kept with Keep Notes
  - `./build/pntest --detected` — a metronome within ±1 BPM inside six seconds (measured: 3.0)
  - `./build/pntest --names` — no name over 16 characters, none duplicated
  - `./build/pntest --font` — the glyph table
- The one check that reads pixels: `./build/pntest --grid` (640x360 and 320x180, Scale 1–4 and Auto)
- Cost: `./build/pntest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`, `--binary PATH`)

Every check carries its negative control and asserts that it fails.

## Notes
- **The CPU decides everything; the shader draws a character screen.**
  `tracker/Tracker` is the clock, the ring, the onsets, the tempo detector and
  the meters; `Screen` turns its state into a grid of cells and an integer
  origin and scale; the shader only decides which cell a pixel is in. A wrong
  note is a tracker fix; a wrong pixel is a Screen or shader fix.
- **The row phase is `base + ( t - anchor ) / T` in double**, re-anchored when
  the period changes, with a 1e-6 row allowance in the floor. Never accumulate
  it, never keep it in float: Resolume's clock is ~499 million ms.
- **Frame one primes**: the previous spectrum is set to the current one and each
  band's floor is seeded at an eighth of its level. A zero or reversed host
  time advances nothing.
- **The onset is spectral flux over a one-second mean**, on a rising frame, with
  a 50 ms refractory. The rising-edge term is what stops a held note re-firing;
  the negative control has to drop it AND the memory.
- **Band edges are (k / N)^gamma in bin index**, forced strictly increasing so
  no channel is ever empty. `Bin Law` decides what a bin *means* in hertz, not
  which bins a channel gets.
- **Fold Octaves is on by default**, because under Linear at 44.1 kHz only three
  bins are inside C-1..B-3 and a column of `---` reads as broken.
- **Keep Notes is off by default**: clearing is what makes a sparse passage
  leave dots and an arrangement change show as replacement.
- **GLSL reserved words**: `patch sample input output filter common active half
  layout flat`. The locals are `cellIndex`, `inCell`, `bar`.
- `SetParamInfo` clamps a STANDARD default into 0..1; Speed, Rows Visible and
  Scale are `FF_TYPE_INTEGER`, which is exempt. An option's SDK range reads back
  0..1; `--list` prints the real one.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin.
- `pattern_core` is an OBJECT library, not STATIC.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PN01`. Display name `SW Pattern`. Bundle id `com.stoatworks.ffgl.pattern`.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's
  `sync-about.py` and `sync-attributions.py`; do not edit them by hand. The
  About block is five entries (`PT_ABOUT_BUTTON_4` is the user guide) and the
  `static_assert` in `Pattern.cpp` holds the count.

## Not done yet
- Never loaded into Resolume on macOS; no real audio has reached it in a host.
  The harness has run on this Mac's GPU and on CI's software renderer.
- No phase lock to the host's `barPhase` (rate lock only).
- The Detected tempo source read the bar (62) for a 125 BPM groove in the
  release video; a quiet band hears the leading edge of a sharp transient. Both
  in AGENTS.md's traps and the user guide.
- No presets, no OpenFX port. User guide in `docs/`, browser demo in `demo/`.

## Browser demo
- Live at https://pattern-demo.stoatworks-labs.com (Cloudflare Worker `pattern-demo`,
  `wrangler.toml`: a route on a proxied `AAAA 100::` DNS record, not a custom
  domain — the zone is at its 100-domain limit; delete the record and the page
  goes dark with a green deploy). A push to main deploys it (`deploy.yml`); by
  hand: `cf-run npx wrangler deploy` from the repo root, then verify by content:
  `curl -s 'https://pattern-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.
- `demo/plugin.js` carries `source/Shaders.cpp`'s GLSL verbatim as three constants
  (vertex, and the fragment shader's two raw strings joined A + B), spliced in by
  script. `demo/tools/check_shaders.py` compares them character for character and
  `tools/verify.sh` runs it. **Change a shader, copy it across.**
- Everything else in `plugin.js` is a hand port (Bands, Tracker, Screen, LayoutFor,
  the palettes, Controls.h, ToOption, Render's uploads and uniforms, Font). Change
  the C++ and the page silently disagrees; only a reader checks it.
- No audio: the page synthesises a 64-bin spectrum from a drum-loop-like programme
  on its own clock and hands the port the programme's tempo as the host's. Not
  Resolume's FFT. Speed, Rows Visible and Scale are dropdowns (no integer type in
  the kit).
- `demo/vendor/` is the shared kit — never edit it; fix it in
  `stoatworks-backend/resolume-demo/kit/` and re-run its `sync.sh pattern`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It records the GL driver, the host clock's unit, whether audio
reached the layer, and which tempo the clock follows.

    ~/Library/Logs/pattern/pattern.YYYY-MM-DD.log
