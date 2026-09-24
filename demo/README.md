# demo/ — the browser demo

Live at **<https://pattern-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html    the shell
    plugin.js     this plugin's parameters, its ported tracker and screen, its shaders
    vendor/       the shared kit, copied in from stoatworks-backend — DO NOT EDIT
    tools/        check_shaders.py, run by tools/verify.sh
    _headers      CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

The shader is the plugin's, copied across unedited. `VERTEX_SHADER`,
`FRAGMENT_SHADER_A` and `FRAGMENT_SHADER_B` in `plugin.js` are the three raw
strings in `source/Shaders.cpp` — the fragment shader is two adjacent raw
strings there — spliced in by script, and the page joins A + B exactly as the
C++ compiler does. `tools/check_shaders.py` compares every raw string and the
joined fragment text character for character, and `../tools/verify.sh` runs it.

The CPU half is a port: `tracker/Bands`, `tracker/Tracker` (the row clock, the
ring, the spectral-flux onset detector primed on frame one, the autocorrelation
tempo detector, the VU meters), `Screen::Compose`, `LayoutFor`, the three
palettes, `Controls.h`, `ToOption` / `Int`, `Renderer::Draw`'s uploads and
uniform packing, and `Font.cpp`. **Nothing checks the port but a reader.**
Change any of those in C++ and the page silently disagrees.

**There is no audio.** The plugin's only input is Resolume's 64-bin FFT buffer,
and a pattern editor with no audio is an empty grid. So the page synthesises
the spectrum itself — a drum-loop-like programme chosen in the "Programme"
dropdown (a kick, a clap, a stab whose pitch moves, closed and open hats on a
16-step grid at 125 BPM; or a metronome, a sparse pattern, silence), written
into 64 bins as decaying spectral shapes on the page's own clock, laid out as
the plugin's Linear law assumes and as magnitudes, with a small deterministic
jitter. Nothing is sampled or licensed; every frame is a function of the clock,
so Step and Restart are exact. The page hands the port the programme's tempo as
a host would through `SetBeatInfo`. **It is the page's spectrum, not a host's**:
Resolume's window, normalisation, bin layout and sample rate are unknown and are
not reproduced, and the plugin's onset thresholds were set on synthetic spectra
like this one; the page says so in its banner, its disclosure, under the
transport and in the hints on Bin Law, Bin Value and Sensitivity.

Everything else is not the plugin either: no Resolume, no composition, no FFGL,
GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core. A pixel here is not a
measurement of a pixel there, and a note here is not a measurement of the
tracker — `pntest` is.

## What is deliberately absent

- **The `Audio` buffer parameter.** A host writes it, never an operator; the
  programme stands in for it.
- **The clip picker, the "use my own file" button and the backdrop.** Pattern is
  a source with zero inputs whose screen is drawn opaque. The kit builds the
  clip controls for every demo and `plugin.js` removes them.
- **The About block.** A text line and link buttons exist so a *host* has
  somewhere to put them. A web page has links of its own.
- **The plugin's `Clock`.** The kit's clock is already in seconds.
- **Integer controls.** Speed, Rows Visible and Scale are `FF_TYPE_INTEGER`; the
  kit has no integer type, so each is a dropdown of every value in its range.

## Working on it

```bash
python3 -m http.server 8953          # from this directory
python3 tools/check_shaders.py       # the copies still match the C++
../tools/verify.sh                   # everything, including the above
```

There is no build step. It is hand-written ES modules and what is committed is
what is served.

**After changing `source/Shaders.cpp`, copy it across here too** —
`check_shaders.py` will tell you which string and where the first difference
is. Do not edit the GLSL in `plugin.js` to make something compile in WebGL2:
`port()` in `vendor/gl.js` handles the version line and the precision qualifiers
and nothing else, and if a shader will not compile here the answer is to say so
on the page.

**`vendor/` is a copy.** Fix the kit in
`stoatworks-backend/resolume-demo/kit/` and re-run
`stoatworks-backend/resolume-demo/sync.sh pattern`.

## Deploying

From the **repository root**, not from here. A push to main runs
`.github/workflows/deploy.yml`; by hand:

```bash
cf-run npx wrangler deploy
curl -s 'https://pattern-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.
The host is a Worker **route** on a proxied `AAAA 100::` DNS record (the zone
has no custom domains left); delete that record and the page goes dark while
deploys stay green.

## Embed mode

`?embed=1` renders the output and nothing else, so the page can be a video
source. `?size=1920x1080` and any parameter id work as query parameters. The
programme is the kit's variant and is not in the URL, so an embed runs the
default drum loop.

    https://pattern-demo.stoatworks-labs.com/?embed=1&size=1920x1080&theme=2
