/**
 * Pattern — browser demo.
 *
 * A tracker's pattern editor, written by the music as it plays. The one idea,
 * from `AGENTS.md`: **run the tracker backwards.** The row cursor runs on the
 * tracker's own clock (a row is Speed × 2.5 / BPM seconds, four rows to a beat
 * at Speed 6), and an onset in a channel's band of the 64-bin spectrum writes a
 * note into that channel on the current row. After row 3F the cursor wraps and
 * the next pass overwrites the last.
 *
 * Two halves, and they are not equally faithful:
 *
 *   The renderer is the plugin's. `VERTEX_SHADER`, `FRAGMENT_SHADER_A` and
 *   `FRAGMENT_SHADER_B` below are the three raw strings in
 *   `source/Shaders.cpp` — the fragment shader is two adjacent raw strings
 *   there, and it is two constants here, joined the way the C++ compiler joins
 *   them. They were spliced in by script, not typed, and
 *   `demo/tools/check_shaders.py` compares every one of them (and the joined
 *   fragment text) to the C++ character for character; `tools/verify.sh` runs
 *   it. One program, one full-screen triangle from gl_VertexID, an R8 font
 *   texture and an RGBA8UI screen texture — the same single pass as
 *   `Renderer::Draw`.
 *
 *   Everything the CPU does is a PORT, and nothing checks the port but a
 *   reader. `tracker/Bands` (bin to hertz under a law, hertz to a tracker
 *   note, the band edges at (k / N)^gamma), `tracker/Tracker` (the row clock as
 *   `base + ( t - anchor ) / T` in double with the 1e-6 row allowance, swing,
 *   the ring that clears a row as it is entered, the spectral-flux onset
 *   detector primed on frame one and seeded at an eighth of the level, the
 *   refractory, the `Cxx` volume law, the tempo detector (three standardised
 *   registers, a pulse scored with its grouping and subdivision, the fastest
 *   level within 0.70 of the best, two readings in a row to publish, parabolic
 *   refinement), the VU meters falling one
 *   step every 20 ms), `Screen` (the 6 × 8 cell grid: counters, VU rectangles,
 *   the pattern rows), `LayoutFor` (the integer scale and origin), the three
 *   palettes in `Render.cpp`, every `Controls.h` conversion, `ToOption` and
 *   `Int` from `Pattern.cpp`, `Renderer::Draw`'s uploads and uniform packing,
 *   and `Font.cpp`'s 5 × 7 table (spliced by the same script) are all below.
 *   `pntest` checks the C++ with no GL context; it has never seen this file.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **There is no audio in a browser, and this page does not ask for a
 * microphone.** The plugin reads one thing from its host: Resolume's
 * FF_USAGE_FFT buffer, 64 bins, once per frame. A pattern editor with no audio
 * is an empty grid, so this page SYNTHESISES a spectrum: a drum-loop-like
 * programme (a kick, a clap, a stab and hats on a 16-step grid at 125 BPM) is
 * written directly into 64 bins as decaying spectral shapes on the page's own
 * clock, with a small deterministic per-frame jitter the way the plugin's own
 * harness jitters its synthetic spectra. Nothing is sampled, nothing is
 * licensed, and every frame is a function of the clock alone, so Step and
 * Restart are exact. Chosen from the kit's one transport dropdown, labelled
 * "Programme".
 *
 * **It is the page's spectrum, not a host's.** The bins are laid out as the
 * plugin's Linear law assumes — bin j centred at (j + ½) / 64 of a 22.05 kHz
 * Nyquist — and written as magnitudes, so Bin Law on Linear and Bin Value on
 * Magnitude read the programme as it was generated, and the other settings
 * show what the other assumptions do to the same numbers. Resolume's window,
 * normalisation, bin layout and sample rate are unknown; the plugin's onset
 * thresholds were set on synthetic spectra like these and one synthetic WAV,
 * never on programme material through Resolume's FFT. That is said in the
 * banner, the disclosure, under the transport and in the hints.
 *
 * **The page is the host's tempo.** Resolume tells a plugin its BPM through
 * `SetBeatInfo`; the page hands the port the programme's own tempo the same
 * way, so Tempo Source = Host is the composition's BPM here as it would be
 * there. Detected runs the ported autocorrelation on the page's onsets for
 * real; Manual is the BPM control. `SetSampleRate` is never called, so the
 * port assumes 44.1 kHz, exactly as the plugin does when a host never says.
 *
 * **No clip picker, and no "use my own file".** Pattern is an FFGL source —
 * `SetMinInputs( 0 )`, `SetMaxInputs( 0 )` — so both kit controls are removed
 * from the DOM after mounting, as needle's and astable's pages do, and
 * `demo.blurb` replaces the banner's "on generated clips" clause.
 *
 * **Speed, Rows Visible and Scale are FF_TYPE_INTEGER in the plugin.** The kit
 * has no integer control, so they are dropdowns of every value in the plugin's
 * range (copperlist's shape); the value handed to the port is the integer.
 *
 * **The Audio buffer parameter and the About block are absent from the panel.**
 * The buffer is written by a host, never by an operator; the programme is its
 * stand-in. The About block is a text line and link buttons so a host has
 * somewhere to put them; a web page has links of its own. Every other
 * parameter — 17 of them — is here, with the constructor's names, groups,
 * types, element lists and defaults, in `Controls.h` order.
 *
 * **The clock is the kit's, already in seconds.** The plugin's `Clock` measures
 * what unit a host's SetTime is in; that is not ported, because the page's
 * clock has no unit to discover. Restart sends time backwards, which the
 * tracker reads as a loop point: the phase is carried across and no time
 * passes on that frame — the plugin's behaviour, kept.
 *
 * ---------------------------------------------------------------------------
 * What this page is NOT evidence about
 * ---------------------------------------------------------------------------
 *
 * GLSL ES 3.00 in WebGL2 rather than desktop GL 4.1 core; JavaScript doubles
 * where the plugin has floats in places (the parameter conversions are rounded
 * through Math.fround to match, the tracker is double on both sides); a
 * browser's frame clock rather than a host's. Nothing here measures anything:
 * the cursor on floor( t / T ) mod 64 from a host clock at 499 million ms, one
 * note per onset in the right channel on the right row, no false note on frame
 * one, the pitch the bin law predicts, the ring, a detected tempo within
 * ±1 BPM in 4 s on a metronome and on drum grooves at the right metrical
 * level, and every glyph on the whole-pixel grid are checked by `pntest
 * --timing`, `--onset`, `--prime`, `--pitch`, `--ring`, `--detected`,
 * `--groove` and `--grid` in the repository, and that harness is the reason to believe
 * the tracker.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// Spliced from the C++ by script (tabs and all); check_shaders.py compares each
// raw string and the join character for character and rejects any backslash or
// template interpolation, so nothing can hide a difference.
//---------------------------------------------------------------------------

const VERTEX_SHADER = `#version 410 core
// One triangle that covers the viewport. No vertex buffer: the corners come
// from gl_VertexID, so the only thing bound at draw time is the VAO.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
`;

const FRAGMENT_SHADER_A = `#version 410 core
out vec4 fragColour;

uniform ivec2 uSize;     // the output, pixels
uniform ivec2 uOrigin;   // where the screen's top-left lands, pixels (may be negative)
uniform int   uScale;    // whole pixels per screen pixel
uniform float uFrac;     // 0.0 shipped; the --grid negative control otherwise
uniform ivec2 uCells;    // columns, rows
uniform vec3  uPal[ 10 ];
uniform int   uBarCount;
uniform ivec4 uBar[ 8 ]; // x, y, w, h in screen pixels
uniform usampler2D uScreen;// R glyph, G foreground index, B background index
uniform sampler2D  uFont;  // graticule's 5x7 table, glyph c at column c * 5

const int kCellW = 6;
const int kCellH = 8;

vec3 pal( uint i )
{
	return uPal[ int( min( i, 9u ) ) ];
}
`;

const FRAGMENT_SHADER_B = `
void main()
{
	// Top-down integer pixel: row 0 is the top row of the output.
	ivec2 pixel = ivec2( int( gl_FragCoord.x ), uSize.y - 1 - int( gl_FragCoord.y ) );
	ivec2 p     = pixel - uOrigin;

	vec3 colour = uPal[ 0 ];
	if( p.x >= 0 && p.y >= 0 && p.x < uCells.x * kCellW * uScale && p.y < uCells.y * kCellH * uScale )
	{
		// Screen pixel. Integer division on non-negative operands, so every
		// boundary sits exactly on a multiple of uScale from the origin.
		ivec2 q = ( uFrac > 0.0 ) ? ivec2( floor( vec2( p ) / ( float( uScale ) * uFrac ) ) ) : p / uScale;

		ivec2 cellIndex = q / ivec2( kCellW, kCellH );
		ivec2 inCell    = q - cellIndex * ivec2( kCellW, kCellH );

		uvec4 cell = texelFetch( uScreen, cellIndex, 0 );
		bool  lit  = false;
		if( inCell.x < 5 && inCell.y < 7 )
			lit = texelFetch( uFont, ivec2( int( cell.r ) * 5 + inCell.x, inCell.y ), 0 ).r > 0.5;
		colour = lit ? pal( cell.g ) : pal( cell.b );

		// The meters, over the cells. Coloured by height in thirds: the
		// bottom two thirds low, the next mid, the top sixth high.
		for( int b = 0; b < 8; ++b )
		{
			if( b >= uBarCount )
				break;
			ivec4 bar = uBar[ b ];
			if( bar.w <= 0 || bar.z <= 0 )
				continue;
			if( q.x >= bar.x && q.x < bar.x + bar.z && q.y >= bar.y && q.y < bar.y + bar.w )
			{
				int fromBottom = ( bar.y + bar.w ) - q.y;// 1 at the bottom pixel
				colour = fromBottom > 40 ? uPal[ 9 ] : ( fromBottom > 32 ? uPal[ 8 ] : uPal[ 7 ] );
			}
		}
	}

	fragColour = vec4( colour, 1.0 );
}
`;

const FRAGMENT_SHADER = FRAGMENT_SHADER_A + FRAGMENT_SHADER_B;

//===========================================================================
// PORT — source/tracker/Bands.{h,cpp}
//===========================================================================

const kBins = 64;
const kFirstMidi = 48;
const kNoteCount = 36;
const kLogFloorHz = 20.0;

/** std::lround: half away from zero. Math.round is not. */
const lround = (x) => (x < 0 ? -Math.floor(-x + 0.5) : Math.floor(x + 0.5));
const clampi = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const clampd = (v, lo, hi) => Math.min(Math.max(v, lo), hi);

/** Bin j's centre in the law's own 0..1 coordinate. */
const centre01 = (j) => (j + 0.5) / kBins;

function binFrequency(j, law, sampleRate) {
  const nyquist = 0.5 * sampleRate;
  const u = centre01(clampi(j, 0, kBins - 1));
  if (law === 1) return kLogFloorHz * Math.pow(nyquist / kLogFloorHz, u);
  return u * nyquist;
}

function midiFromFrequency(hz) {
  if (!(hz > 0.0)) return -1;
  return lround(69.0 + 12.0 * Math.log2(hz / 440.0));
}

function trackerNote(midi, fold) {
  if (midi < 0) return -1;
  let n = midi - kFirstMidi;
  if (fold) {
    while (n < 0) n += 12;
    while (n >= kNoteCount) n -= 12;
  }
  return n >= 0 && n < kNoteCount ? n : -1;
}

const NOTE_NAMES = [
  'C-1', 'C#1', 'D-1', 'D#1', 'E-1', 'F-1', 'F#1', 'G-1', 'G#1', 'A-1', 'A#1', 'B-1',
  'C-2', 'C#2', 'D-2', 'D#2', 'E-2', 'F-2', 'F#2', 'G-2', 'G#2', 'A-2', 'A#2', 'B-2',
  'C-3', 'C#3', 'D-3', 'D#3', 'E-3', 'F-3', 'F#3', 'G-3', 'G#3', 'A-3', 'A#3', 'B-3',
];
const noteName = (n) => (n < 0 || n >= kNoteCount ? '---' : NOTE_NAMES[n]);

/** `edges` gets channels + 1 entries; every channel keeps at least one bin. */
function bandEdges(channels, law, gamma, edges) {
  void law; // the edges are placed in the law's own coordinate, which is bin index either way
  channels = Math.max(1, Math.min(channels, kBins));
  edges[0] = 0;
  for (let k = 1; k < channels; k += 1) {
    const u = Math.pow(k / channels, gamma);
    let edge = lround(u * kBins);
    edge = Math.max(edge, edges[k - 1] + 1);
    edge = Math.min(edge, kBins - (channels - k));
    edges[k] = edge;
  }
  edges[channels] = kBins;
}

//===========================================================================
// PORT — source/tracker/Tracker.{h,cpp}
//===========================================================================

const kMaxRows = 64;
const kMaxChannels = 8;
const kVuSteps = 48;
const kVuFallSeconds = 0.02;
const kRowAllowance = 1e-6;
const kFloorSeconds = 1.0;
const kPrimeFraction = 0.125;
const kRefractory = 0.05;
const kAbsoluteFloor = 1e-4;
const kTempoRate = 100;
const kTempoWindow = 600;
const kTempoLagMin = 30;
const kTempoLagMax = 100;
const kTempoSettleR = 0.2;
const kTempoBands = 3;
const kTempoLowHz = 400.0;
const kTempoHighHz = 5000.0;
const kTempoLevelRatio = 0.70;
const kTempoPriorBpm = 120.0;
const kTempoPriorOct = 1.0;
const kTempoMaxLag = 302;
const kTempoAgree = 0.02;
const kEnvSize = 1024;

/** Each bin's tempo register from its centre under the bin law: 0 low, 1 mid, 2 high. */
function tempoBands(law, sampleRate, out) {
  for (let j = 0; j < kBins; j += 1) {
    const hz = binFrequency(j, law, sampleRate);
    out[j] = hz < kTempoLowHz ? 0 : (hz < kTempoHighHz ? 1 : 2);
  }
}

/** Unbiased autocorrelation of zero-mean x at `lag`, normalised by the mean square. */
function autoCorr(x, n, lag, power) {
  let acc = 0.0;
  for (let i = 0; i + lag < n; i += 1) acc += x[i] * x[i + lag];
  return acc / (n - lag) / power;
}

/** The largest linearly interpolated R within one sample of fractional lag t (0 past the lags kept). */
function peakNear(r, maxLag, t) {
  const lo = Math.floor(t - 1.0);
  const hi = Math.ceil(t + 1.0);
  if (lo < 0 || hi + 1 > maxLag) return 0.0;
  let best = -1e300;
  for (let q = 4 * lo; q <= 4 * hi; q += 1) {
    const i = q >> 2;
    const f = (q & 3) * 0.25;
    best = Math.max(best, r[i] * (1.0 - f) + r[i + 1] * f);
  }
  return best;
}

const newCell = () => ({ note: -1, sample: 0, volume: 0, written: false, pass: 0, strength: 0.0 });

class Tracker {
  constructor() {
    this.settings = {
      tempoSource: 0, manualBpm: 125.0, hostBpm: 120.0, speed: 6, swing: 0.0, rows: 64, keep: false,
      channels: 4, binLaw: 0, binValue: 0, ratio: 4.24, gamma: 2.2, fold: true, sampleRate: 44100.0,
    };
    this.prev = new Float64Array(kBins);
    this.flux = new Float64Array(kMaxChannels);
    this.fluxPrev = new Float64Array(kMaxChannels);
    this.floor = new Float64Array(kMaxChannels);
    this.lastFire = new Float64Array(kMaxChannels);
    this.edges = new Int32Array(kMaxChannels + 1);
    this.cells = Array.from({ length: kMaxRows }, () => Array.from({ length: kMaxChannels }, newCell));
    this.vuSet = new Int32Array(kMaxChannels);
    this.vuTime = new Float64Array(kMaxChannels);
    this.env = Array.from({ length: kTempoBands }, () => new Float64Array(kEnvSize));
    this.tempoBand = new Int32Array(kBins);
    this.reset();
  }

  reset() {
    this.base = 0.0; this.anchor = 0.0;
    this.period = 0.0; this.bpm = 0.0; this.rawPhase = 0.0;
    this.rowsTotal = 0; this.row = 0; this.pass = 0;
    this.primed = false;
    this.lastSeconds = 0.0;
    this.prev.fill(0); this.flux.fill(0); this.fluxPrev.fill(0); this.floor.fill(0); this.lastFire.fill(0);
    this.writtenLastFrame = 0;
    for (const row of this.cells) for (let k = 0; k < kMaxChannels; k += 1) row[k] = newCell();
    this.vuSet.fill(0); this.vuTime.fill(0);
    this.now = 0.0;
    for (const e of this.env) e.fill(0);
    tempoBands(this.settings.binLaw, this.settings.sampleRate, this.tempoBand);
    this.envIndex = -1;
    this.envOrigin = 0.0;
    this.lastDetectSec = -1;
    this.detectedBpm = 0.0;
    this.candidateBpm = 0.0;
    this.peakR = 0.0;
    bandEdges(this.settings.channels, this.settings.binLaw, this.settings.gamma, this.edges);
  }

  setSettings(s) {
    this.settings = { ...s };
    this.settings.rows = clampi(s.rows, 1, kMaxRows);
    this.settings.channels = clampi(s.channels, 1, kMaxChannels);
    this.settings.speed = Math.max(s.speed, 1);
    this.settings.swing = clampd(s.swing, 0.0, 0.5);
    bandEdges(this.settings.channels, this.settings.binLaw, this.settings.gamma, this.edges);
    tempoBands(this.settings.binLaw, this.settings.sampleRate, this.tempoBand);
  }

  tempoSettled() { return this.detectedBpm > 0.0; }
  bandEdge(k) { return this.edges[k]; }
  at(row, channel) { return this.cells[row][channel]; }

  currentBpm() {
    let bpm = this.settings.hostBpm;
    switch (this.settings.tempoSource) {
      case 1: if (this.detectedBpm > 0.0) bpm = this.detectedBpm; break;
      case 2: bpm = this.settings.manualBpm; break;
      default: break;
    }
    if (!(bpm >= 20.0 && bpm <= 999.0)) bpm = 120.0;
    return bpm;
  }

  vu(channel) {
    if (channel < 0 || channel >= kMaxChannels || this.vuSet[channel] <= 0) return 0;
    const elapsed = this.now - this.vuTime[channel];
    if (elapsed < 0.0) return this.vuSet[channel];
    const fallen = Math.floor(elapsed / kVuFallSeconds);
    return Math.max(0, this.vuSet[channel] - fallen);
  }

  prime(seconds, m) {
    this.anchor = seconds;
    this.base = 0.0;
    this.bpm = this.currentBpm();
    this.period = this.settings.speed * 2.5 / this.bpm;
    this.lastSeconds = seconds;
    this.now = seconds;
    this.envOrigin = seconds;
    this.envIndex = -1;

    this.prev.set(m);
    for (let k = 0; k < this.settings.channels; k += 1) {
      let level = 0.0;
      for (let j = this.edges[k]; j < this.edges[k + 1]; j += 1) level += m[j];
      this.floor[k] = level * kPrimeFraction;
    }
    for (let k = 0; k < kMaxChannels; k += 1) {
      this.fluxPrev[k] = 0.0;
      this.lastFire[k] = seconds - 1.0;
    }
    this.primed = true;
    this.rawPhase = 0.0;
    this.rowsTotal = 0;
    this.row = 0;
    this.pass = 0;
  }

  reanchorIfNeeded(seconds) {
    const bpm = this.currentBpm();
    const period = this.settings.speed * 2.5 / bpm;
    if (period === this.period) {
      this.bpm = bpm;
      return;
    }
    this.base = this.base + (seconds - this.anchor) / this.period;
    this.anchor = seconds;
    this.period = period;
    this.bpm = bpm;
  }

  advanceRows(seconds) {
    const raw = this.base + (seconds - this.anchor) / this.period;
    this.rawPhase = raw;

    const r = raw + kRowAllowance;
    const pairs = Math.floor(r * 0.5);
    const within = r - 2.0 * pairs;
    const total = 2 * Math.trunc(pairs) + (within >= 1.0 + this.settings.swing ? 1 : 0);

    if (total > this.rowsTotal) {
      const entered = total - this.rowsTotal;
      if (!this.settings.keep) {
        if (entered >= this.settings.rows) {
          for (let row = 0; row < this.settings.rows; row += 1) this.clearRow(row);
        } else {
          for (let t = this.rowsTotal + 1; t <= total; t += 1) this.clearRow(t % this.settings.rows);
        }
      }
    }
    this.rowsTotal = Math.max(total, 0);
    this.row = this.rowsTotal % this.settings.rows;
    this.pass = Math.floor(this.rowsTotal / this.settings.rows);
  }

  clearRow(row) {
    for (let k = 0; k < kMaxChannels; k += 1) this.cells[row][k] = newCell();
  }

  listen(seconds, m, dt) {
    this.writtenLastFrame = 0;
    const pass = this.pass;

    // The tempo detector's registers read every bin, whatever the channels.
    const bandFlux = new Float64Array(kTempoBands);
    for (let j = 0; j < kBins; j += 1) bandFlux[this.tempoBand[j]] += Math.max(0.0, m[j] - this.prev[j]);

    for (let k = 0; k < this.settings.channels; k += 1) {
      let flux = 0.0;
      let peak = this.edges[k];
      for (let j = this.edges[k]; j < this.edges[k + 1]; j += 1) {
        flux += Math.max(0.0, m[j] - this.prev[j]);
        if (m[j] > m[peak]) peak = j;
      }
      this.flux[k] = flux;

      const threshold = Math.max(kAbsoluteFloor, this.floor[k] * this.settings.ratio);
      const rising = flux > this.fluxPrev[k];
      const armed = seconds - this.lastFire[k] >= kRefractory;
      if (flux > threshold && rising && armed) {
        const strength = flux / threshold;
        const volume = Math.trunc(clampd(lround(64.0 * Math.log2(strength) / 3.0), 1.0, 64.0));
        const hz = binFrequency(peak, this.settings.binLaw, this.settings.sampleRate);
        const note = trackerNote(midiFromFrequency(hz), this.settings.fold);

        const cell = this.cells[this.row][k];
        if (!cell.written || cell.pass !== pass || strength > cell.strength) {
          cell.note = note;
          cell.sample = k + 1;
          cell.volume = volume;
          cell.written = true;
          cell.pass = pass;
          cell.strength = strength;
          this.writtenLastFrame += 1;
        }
        this.vuSet[k] = lround(kVuSteps * volume / 64.0);
        this.vuTime[k] = seconds;
        this.lastFire[k] = seconds;
      }

      if (dt > 0.0) {
        const a = 1.0 - Math.exp(-dt / kFloorSeconds);
        this.floor[k] += (flux - this.floor[k]) * a;
      }
      this.fluxPrev[k] = flux;
    }
    this.prev.set(m);

    this.feedTempo(seconds, bandFlux);
  }

  feedTempo(seconds, bandFlux) {
    const index = Math.floor((seconds - this.envOrigin) * kTempoRate);
    if (index < 0) return;
    for (let b = 0; b < kTempoBands; b += 1) {
      const env = this.env[b];
      if (this.envIndex < 0) {
        env[index % kEnvSize] = bandFlux[b];
      } else if (index > this.envIndex) {
        const from = Math.max(this.envIndex + 1, index - kEnvSize);
        for (let i = from; i <= index; i += 1) env[i % kEnvSize] = bandFlux[b];
      } else {
        env[index % kEnvSize] = Math.max(env[index % kEnvSize], bandFlux[b]);
      }
    }
    this.envIndex = Math.max(this.envIndex, index);

    const second = Math.floor(seconds - this.envOrigin);
    if (second > this.lastDetectSec) {
      this.lastDetectSec = second;
      if (this.envIndex + 1 >= 3 * kTempoRate) this.detectTempo();
    }
  }

  detectTempo() {
    const n = Math.min(this.envIndex + 1, kTempoWindow);
    const kWeight = [1.0, 1.0, 0.5];
    const kSmooth = 6;
    const kSigma = 2.0;
    const kOwnLagMax = 200;

    const kernel = new Float64Array(2 * kSmooth + 1);
    for (let k = -kSmooth; k <= kSmooth; k += 1) kernel[k + kSmooth] = Math.exp(-0.5 * (k / kSigma) * (k / kSigma));

    const x = new Float64Array(kTempoWindow);
    const raw = new Float64Array(kTempoWindow);
    const z = new Float64Array(kTempoWindow);
    for (let b = 0; b < kTempoBands; b += 1) {
      for (let i = 0; i < n; i += 1) raw[i] = this.env[b][(this.envIndex - n + 1 + i) % kEnvSize];
      // Smooth, renormalising the kernel where it runs off either end.
      let mean = 0.0;
      for (let i = 0; i < n; i += 1) {
        let acc = 0.0, wsum = 0.0;
        for (let k = -kSmooth; k <= kSmooth; k += 1) {
          if (i + k >= 0 && i + k < n) {
            acc += kernel[k + kSmooth] * raw[i + k];
            wsum += kernel[k + kSmooth];
          }
        }
        z[i] = acc / wsum;
        mean += z[i];
      }
      mean /= n;
      let power = 0.0;
      for (let i = 0; i < n; i += 1) {
        z[i] -= mean;
        power += z[i] * z[i];
      }
      power /= n;
      if (!(power > 1e-18)) continue;
      let own = 0.0;
      const ownMax = Math.min(kOwnLagMax, n - 60);
      for (let lag = kTempoLagMin; lag <= ownMax; lag += 1) own = Math.max(own, autoCorr(z, n, lag, power));
      const w = kWeight[b] * own / Math.sqrt(power);
      for (let i = 0; i < n; i += 1) x[i] += w * z[i];
    }

    let power = 0.0;
    for (let i = 0; i < n; i += 1) power += x[i] * x[i];
    power /= n;
    if (!(power > 1e-18)) return;
    const maxLag = Math.min(kTempoMaxLag, n - 60);
    const r = new Float64Array(kTempoMaxLag + 2);
    for (let lag = 0; lag <= maxLag; lag += 1) r[lag] = autoCorr(x, n, lag, power);

    // A pulse, its grouping, and (at half weight) its subdivision.
    const score = (t) => 0.5 * peakNear(r, maxLag, 0.5 * t) + peakNear(r, maxLag, t) + peakNear(r, maxLag, 2.0 * t);
    // The family: the best score under the prior, on a quarter-sample grid.
    let family = kTempoLagMin, bestScore = -1e300;
    for (let q = 4 * kTempoLagMin; q <= 4 * kTempoLagMax; q += 1) {
      const t = 0.25 * q;
      const octs = Math.log2(60.0 * kTempoRate / t / kTempoPriorBpm) / kTempoPriorOct;
      const sc = score(t) * Math.exp(-0.5 * octs * octs);
      if (sc > bestScore) {
        bestScore = sc;
        family = t;
      }
    }
    // The level: the fastest member within the ratio of the best, no prior.
    const members = [], scores = [];
    let top = -1e300;
    for (let j = -2; j <= 2; j += 1) {
      const t = family * 2 ** j;
      if (t < kTempoLagMin || t > kTempoLagMax) continue;
      members.push(t);
      scores.push(score(t));
      top = Math.max(top, scores[scores.length - 1]);
    }
    let best = family;
    for (let i = 0; i < members.length; i += 1) {
      if (scores[i] >= kTempoLevelRatio * top) {
        best = members[i];
        break;
      }
    }

    const peak = peakNear(r, maxLag, best);
    if (peak < kTempoSettleR) {
      this.candidateBpm = 0.0;
      return;
    }

    // Refine on the peak of R at the largest multiple that fits (up to 4).
    const m = Math.max(1, Math.min(4, Math.trunc((maxLag - 3) / best)));
    const c = m * best;
    const hw = Math.max(1, Math.trunc(best / 8.0));
    const lo = Math.max(1, Math.floor(c - hw));
    const hi = Math.min(maxLag - 1, Math.ceil(c + hw));
    let at = lo;
    for (let lag = lo; lag <= hi; lag += 1) if (r[lag] > r[at]) at = lag;
    let lag = at;
    {
      const a = r[at - 1], b = r[at], cc = r[at + 1];
      const d = a - 2.0 * b + cc;
      if (d < 0.0) lag += 0.5 * (a - cc) / d;
    }
    lag /= m;
    // Published only when the reading before agrees within 2 %.
    const reading = 60.0 * kTempoRate / lag;
    const agrees = (a, b) => b > 0.0 && Math.abs(a / b - 1.0) <= kTempoAgree;
    const publish = agrees(reading, this.candidateBpm) || agrees(reading, this.detectedBpm);
    this.candidateBpm = reading;
    this.peakR = peak;
    if (publish) this.detectedBpm = reading;
  }

  /** One host frame. `bins` is a Float32Array — the host's floats. */
  frame(seconds, bins, count) {
    const m = new Float64Array(kBins);
    for (let j = 0; j < kBins; j += 1) {
      const v = bins != null && j < count ? bins[j] : 0.0;
      const clean = v > 0.0 ? v : 0.0;
      m[j] = this.settings.binValue === 1 ? Math.sqrt(clean) : clean;
    }

    if (!this.primed) {
      this.prime(seconds, m);
      return;
    }

    if (seconds < this.lastSeconds) {
      this.base = this.base + (this.lastSeconds - this.anchor) / this.period;
      this.anchor = seconds;
      for (let k = 0; k < kMaxChannels; k += 1) {
        this.vuTime[k] -= this.lastSeconds - seconds;
        this.lastFire[k] -= this.lastSeconds - seconds;
      }
      this.envOrigin -= this.lastSeconds - seconds;
      this.lastSeconds = seconds;
    }

    const dt = seconds - this.lastSeconds;
    this.lastSeconds = seconds;
    this.now = seconds;

    this.reanchorIfNeeded(seconds);
    this.advanceRows(seconds);
    this.listen(seconds, m, dt);
  }
}

//===========================================================================
// PORT — source/Screen.{h,cpp}: the character grid, the VU rectangles, the
// layout. Cells are written straight into the RGBA8UI upload: R glyph, G
// foreground index, B background index, A pad.
//===========================================================================

const PAL_BACK = 0, PAL_TEXT = 1, PAL_DIM = 2, PAL_CURSOR_BACK = 3, PAL_CURSOR_TEXT = 4, PAL_ACCENT = 5,
  PAL_FRAME = 6;
const PAL_COUNT = 10;

const kCellW = 6, kCellH = 8;
const kHeaderRows = 2;
const kVuRows = 6;

const kHex = '0123456789ABCDEF';

const channelWidth = (showEffects) => (showEffects ? 10 : 6);
const channelColumn = (k, showEffects) => 3 + k * (1 + channelWidth(showEffects)) + 1;

function put(s, col, row, c, fg, bg) {
  if (col < 0 || row < 0 || col >= s.cols || row >= s.rows) return;
  const code = c.charCodeAt(0);
  const i = (row * s.cols + col) * 4;
  s.cells[i] = code >= 32 && code < 127 ? code : 63;
  s.cells[i + 1] = fg;
  s.cells[i + 2] = bg;
}
function text(s, col, row, t, fg, bg) {
  for (let i = 0; i < t.length; i += 1) put(s, col + i, row, t[i], fg, bg);
}

const pad3 = (n) => String(n).padStart(3, ' ');
const pad02 = (n) => String(n).padStart(2, '0');

function compose(tracker, display) {
  const ts = tracker.settings;
  const channels = ts.channels;
  const visible = Math.max(1, display.rowsVisible);
  const colw = channelWidth(display.showEffects);

  const s = {
    cols: 3 + channels * (1 + colw) + 1,
    rows: kHeaderRows + (display.showVu ? kVuRows : 0) + visible,
    cells: null,
    bars: [],
  };
  s.cells = new Uint8Array(s.cols * s.rows * 4);
  for (let i = 0; i < s.cols * s.rows; i += 1) {
    s.cells[i * 4] = 32;
    s.cells[i * 4 + 1] = PAL_TEXT;
    s.cells[i * 4 + 2] = PAL_BACK;
  }

  // -- the counters --------------------------------------------------------
  {
    const pos = tracker.pass % 128;
    const pat = tracker.pass % 64;
    const bpm = Math.min(lround(tracker.bpm), 999);
    let col = 0;
    const label = (t) => { text(s, col, 0, t, PAL_ACCENT, PAL_BACK); col += t.length; };
    const value = (t) => { text(s, col, 0, t, PAL_TEXT, PAL_BACK); col += t.length; };
    label('POS ');
    value(`${kHex[pos >> 4]}${kHex[pos & 15]} `);
    label('PAT ');
    value(`${kHex[pat >> 4]}${kHex[pat & 15]} `);
    label('BPM ');
    value(`${pad3(bpm)}${ts.tempoSource === 1 ? (tracker.tempoSettled() ? '*' : '?') : ' '} `);
    label('SPD ');
    value(pad02(Math.min(ts.speed, 99)));
  }

  // -- the VU bars -----------------------------------------------------------
  if (display.showVu) {
    const top = kHeaderRows * kCellH;
    const bottom = top + kVuRows * kCellH;
    const barW = 4 * kCellW;
    for (let k = 0; k < channels; k += 1) {
      const colPx = channelColumn(k, display.showEffects) * kCellW;
      const level = Math.min(tracker.vu(k), kVuSteps);
      s.bars.push({
        x: colPx + Math.trunc((colw * kCellW - barW) / 2),
        y: bottom - level,
        w: barW,
        h: level,
        level,
      });
    }
  }

  // -- the pattern -----------------------------------------------------------
  const firstRow = kHeaderRows + (display.showVu ? kVuRows : 0);
  const centre = Math.trunc(visible / 2);
  for (let v = 0; v < visible; v += 1) {
    const screenRow = firstRow + v;
    let row = tracker.row + (v - centre);
    row = ((row % ts.rows) + ts.rows) % ts.rows;
    const cursor = v === centre;
    const bg = cursor ? PAL_CURSOR_BACK : PAL_BACK;
    const tx = cursor ? PAL_CURSOR_TEXT : PAL_TEXT;
    const dim = cursor ? PAL_CURSOR_TEXT : PAL_DIM;
    const beat = cursor ? PAL_CURSOR_TEXT : (row % 4 === 0 ? PAL_ACCENT : PAL_TEXT);

    for (let c = 0; c < s.cols; c += 1) put(s, c, screenRow, ' ', tx, bg);

    put(s, 0, screenRow, kHex[row >> 4], beat, bg);
    put(s, 1, screenRow, kHex[row & 15], beat, bg);

    for (let k = 0; k < channels; k += 1) {
      const col = channelColumn(k, display.showEffects);
      const cell = tracker.at(row, k);
      put(s, col - 1, screenRow, '|', PAL_FRAME, bg);

      if (cell.written) {
        text(s, col, screenRow, noteName(cell.note), tx, bg);
        put(s, col + 4, screenRow, kHex[cell.sample >> 4], cursor ? tx : PAL_ACCENT, bg);
        put(s, col + 5, screenRow, kHex[cell.sample & 15], cursor ? tx : PAL_ACCENT, bg);
        if (display.showEffects) {
          put(s, col + 7, screenRow, 'C', tx, bg);
          put(s, col + 8, screenRow, kHex[cell.volume >> 4], tx, bg);
          put(s, col + 9, screenRow, kHex[cell.volume & 15], tx, bg);
        }
      } else {
        text(s, col, screenRow, '--- ..', dim, bg);
        if (display.showEffects) text(s, col + 7, screenRow, '...', dim, bg);
      }
    }
    put(s, s.cols - 1, screenRow, '|', PAL_FRAME, bg);
  }

  return s;
}

/** PORT — LayoutFor. C++ integer division truncates towards zero: Math.trunc. */
function layoutFor(screen, width, height, requested) {
  const screenW = screen.cols * kCellW;
  const screenH = screen.rows * kCellH;
  let scale;
  if (requested > 0) scale = requested;
  else scale = Math.max(1, Math.min(screenW > 0 ? Math.trunc(width / screenW) : 1, screenH > 0 ? Math.trunc(height / screenH) : 1));
  return {
    scale,
    originX: Math.trunc((width - screenW * scale) / 2),
    originY: Math.trunc((height - screenH * scale) / 2),
    screenW,
    screenH,
  };
}

//===========================================================================
// PORT — source/Render.cpp: the three palettes (Float32Array rounds each
// literal the way the C++ `f` suffix does) and Renderer::InitGL / Draw.
//===========================================================================

const PALETTES = [
  // Grey: a light grey desktop, black text, a blue cursor bar.
  new Float32Array([
    0.6666667, 0.6666667, 0.6666667,
    0.0, 0.0, 0.0,
    0.3333333, 0.3333333, 0.3333333,
    0.0, 0.3333333, 0.6666667,
    1.0, 1.0, 1.0,
    0.0, 0.2, 0.6,
    0.4, 0.4, 0.4,
    0.0, 0.6, 0.0,
    0.8, 0.6666667, 0.0,
    0.8, 0.1333333, 0.0,
  ]),
  // Blue: a deep blue, pale text, a brighter blue cursor bar.
  new Float32Array([
    0.0, 0.0666667, 0.2666667,
    0.7333333, 0.8, 1.0,
    0.2666667, 0.3333333, 0.5333333,
    0.1333333, 0.3333333, 0.8,
    1.0, 1.0, 1.0,
    0.5333333, 0.8, 1.0,
    0.2, 0.2666667, 0.5333333,
    0.2666667, 0.7333333, 1.0,
    0.5333333, 0.8666667, 1.0,
    1.0, 1.0, 1.0,
  ]),
  // Green: black, phosphor green, a dim green cursor bar.
  new Float32Array([
    0.0, 0.0, 0.0,
    0.2666667, 1.0, 0.2666667,
    0.0666667, 0.4, 0.0666667,
    0.0666667, 0.2666667, 0.0666667,
    0.8, 1.0, 0.8,
    0.6666667, 1.0, 0.6666667,
    0.1333333, 0.4, 0.1333333,
    0.1333333, 0.8, 0.1333333,
    0.5333333, 1.0, 0.2666667,
    1.0, 1.0, 0.4,
  ]),
];
const paletteFor = (theme) => PALETTES[clampi(theme, 0, 2)];

//===========================================================================
// PORT — source/Font.cpp: graticule's 5×7 table, spliced from the C++ by the
// same script as the shaders. Glyph for code c starts at column c * 5.
//===========================================================================

const GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 32 space
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'], // 33 !
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'], // 34 "
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'], // 35 #
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'], // 36 $
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'], // 37 %
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'], // 38 &
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'], // 39 '
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'], // 40 (
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'], // 41 )
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'], // 42 *
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'], // 43 +
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // 44 ,
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'], // 45 -
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'], // 46 .
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'], // 47 /
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // 48 0
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 49 1
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'], // 50 2
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'], // 51 3
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'], // 52 4
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'], // 53 5
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'], // 54 6
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'], // 55 7
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'], // 56 8
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'], // 57 9
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // 58 :
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'], // 59 ;
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'], // 60 <
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'], // 61 =
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'], // 62 >
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'], // 63 ?
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'], // 64 @
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 65 A
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'], // 66 B
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'], // 67 C
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'], // 68 D
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'], // 69 E
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'], // 70 F
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'], // 71 G
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 72 H
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 73 I
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'], // 74 J
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'], // 75 K
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'], // 76 L
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'], // 77 M
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'], // 78 N
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 79 O
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'], // 80 P
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'], // 81 Q
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 82 R
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'], // 83 S
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 84 T
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 85 U
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 86 V
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'], // 87 W
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'], // 88 X
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'], // 89 Y
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'], // 90 Z
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'], // 91 [
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'], // 92 backslash
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'], // 93 ]
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'], // 94 ^
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'], // 95 _
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'], // 96 `
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 97 a
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'], // 98 b
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'], // 99 c
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 100 d
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 101 e
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'], // 102 f
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 103 g
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 104 h
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 105 i
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'], // 106 j
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'], // 107 k
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 108 l
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'], // 109 m
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 110 n
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 111 o
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 112 p
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'], // 113 q
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 114 r
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'], // 115 s
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'], // 116 t
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'], // 117 u
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 118 v
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'], // 119 w
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'], // 120 x
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'], // 121 y
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'], // 122 z
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'], // 123 {
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 124 |
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'], // 125 }
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'], // 126 ~
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 127 del
];

const FONT_FIRST = 32;
const FONT_WIDTH = 5;
const FONT_HEIGHT = 7;
const FONT_TEXTURE_WIDTH = FONT_WIDTH * 128;

/** PORT — font::Texture(): kWidth*128 by kHeight, R8, glyph c at column c*kWidth. */
function fontTexture() {
  const out = new Uint8Array(FONT_TEXTURE_WIDTH * FONT_HEIGHT);
  for (let code = FONT_FIRST; code < 128; code += 1) {
    const glyph = GLYPHS[code - FONT_FIRST];
    for (let y = 0; y < FONT_HEIGHT; y += 1) {
      for (let x = 0; x < FONT_WIDTH; x += 1) {
        if (glyph[y][x] === '#') out[y * FONT_TEXTURE_WIDTH + code * FONT_WIDTH + x] = 255;
      }
    }
  }
  return out;
}

//===========================================================================
// PORT — source/Controls.h and Pattern.cpp's ToOption / Bool / Int /
// CurrentSettings / CurrentDisplay. The C++ takes a float: Math.fround.
//===========================================================================

const f32 = Math.fround;
const BpmFromControl = (v) => 60.0 + 140.0 * f32(v);
const kBpmDefault = 0.4642857;
const SwingFromControl = (v) => 0.5 * f32(v);
const ThresholdRatio = (v) => 1.5 * Math.pow(2.0, 3.0 * (1.0 - f32(v)));
const kSensitivityDefault = 0.5;
const SplitExponent = (v) => 1.0 + 3.0 * f32(v);
const kBandSplitDefault = 0.4;

const kSpeedMin = 1, kSpeedMax = 31, kSpeedDefault = 6;
const kRowsVisibleMin = 3, kRowsVisibleMax = 33, kRowsVisibleDefault = 17;
const kScaleMin = 0, kScaleMax = 8, kScaleDefault = 0;

const kTempoSourceCount = 3;
const kBinLawCount = 2;
const kBinValueCount = 2;
const kThemeCount = 3;

function toOption(v, count) {
  if (count <= 1) return 0;
  v = f32(v);
  const i = (v <= 1.0 && count > 2 && v !== Math.floor(v))
    ? Math.trunc(f32(v * f32(count - 1)) + 0.5)
    : Math.trunc(v + 0.5);
  return clampi(i, 0, count - 1);
}
const isOn = (v) => f32(v) > 0.5;
const intParam = (v, lo, hi) => clampi(lround(f32(v)), lo, hi);

//===========================================================================
// The integer controls. Speed, Rows Visible and Scale are FF_TYPE_INTEGER in
// the plugin, holding a real integer with a real range. The kit has no integer
// control, so -- copperlist's shape -- each is a dropdown of every value in the
// plugin's range, and `integerValue` turns the dropdown's index back into it.
//===========================================================================

const INTEGER_RANGES = {
  speed: [kSpeedMin, kSpeedMax],
  rowsVisible: [kRowsVisibleMin, kRowsVisibleMax],
  scale: [kScaleMin, kScaleMax],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return clampi(low + Math.round(index), low, high);
}

function currentSettings(p, hostBpm) {
  return {
    tempoSource: toOption(p.get('tempoSource'), kTempoSourceCount),
    manualBpm: BpmFromControl(p.get('bpm')),
    hostBpm,
    speed: intParam(integerValue('speed', p.get('speed')), kSpeedMin, kSpeedMax),
    swing: SwingFromControl(p.get('swing')),
    rows: toOption(p.get('rows'), 2) === 1 ? 32 : 64,
    keep: isOn(p.get('keep')),
    channels: toOption(p.get('channels'), 2) === 1 ? 8 : 4,
    binLaw: toOption(p.get('binLaw'), kBinLawCount),
    binValue: toOption(p.get('binValue'), kBinValueCount),
    ratio: ThresholdRatio(p.get('sensitivity')),
    gamma: SplitExponent(p.get('bandSplit')),
    fold: isOn(p.get('fold')),
    sampleRate: 44100.0, // SetSampleRate is never called here, as when a host never says
  };
}

function currentDisplay(p) {
  return {
    rowsVisible: intParam(integerValue('rowsVisible', p.get('rowsVisible')), kRowsVisibleMin, kRowsVisibleMax),
    showVu: isOn(p.get('showVu')),
    showEffects: isOn(p.get('showEffects')),
  };
}

//===========================================================================
// NOT THE PLUGIN — the programme, and the 64-bin spectrum made from it.
//
// This is the one part of the page that stands in for the host rather than
// porting the plugin. Resolume's FFT is unknown (window, normalisation, bin
// layout, sample rate), so nothing here pretends to be it. What it does:
//
//   1. A programme is a list of hits on a 16-step grid at a tempo: a kick, a
//      clap, a stab (whose peak bin moves, so its notes differ) and hats, each
//      a spectral SHAPE over the 64 bins with an amplitude and a decay.
//   2. The bins are laid out as the plugin's Linear law assumes -- bin j
//      centred at (j + 1/2) / 64 of a 22.05 kHz Nyquist -- and written as
//      MAGNITUDES. Under the default 4-channel split (edges at 3, 14, 34) the
//      kick lands in channel 1, the clap in 2, the stab in 3 and the hats in
//      4, which is why the picture reads as a drum pattern.
//   3. Every frame is a function of the page clock alone -- each hit's
//      envelope decays from its own time -- with a +-5 % per-bin jitter from an
//      integer hash of the millisecond and the bin, the way the plugin's
//      harness jitters its synthetic spectra. Step and Restart are exact.
//
// The plugin's onset thresholds were set on synthetic spectra like these and
// one synthetic WAV through the harness's own FFT, never on programme material
// through Resolume's FFT. This page is the same kind of evidence, not more.
//===========================================================================

const BINS = 64;

/** A spectral shape: a Gaussian in bin index, peak 1 at `centre`. */
function shape(centre, width) {
  const w = new Float64Array(BINS);
  for (let j = 0; j < BINS; j += 1) w[j] = Math.exp(-(((j - centre) / width) ** 2));
  return w;
}

const HITS = {
  kick: { shape: shape(0.3, 0.9), amp: 2.0, tau: 0.12 },
  clap: { shape: shape(7, 3.5), amp: 1.2, tau: 0.08 },
  hat: { shape: shape(46, 6), amp: 0.5, tau: 0.04 },
  open: { shape: shape(46, 6), amp: 0.6, tau: 0.3 },
  // The stab's centre is per hit, so the band's peak bin -- and the note the
  // pattern shows -- changes from one to the next.
  stab16: { shape: shape(16, 2.5), amp: 0.8, tau: 0.15 },
  stab20: { shape: shape(20, 2.5), amp: 0.8, tau: 0.15 },
  stab24: { shape: shape(24, 2.5), amp: 0.8, tau: 0.15 },
  stab28: { shape: shape(28, 2.5), amp: 0.8, tau: 0.15 },
};

/** [step on a 16-step-per-bar grid, hit] */
const PROGRAMMES = [
  {
    id: 'beats',
    name: 'Drum loop, 125 BPM, two bars',
    hint: 'A kick on every beat, a clap on 2 and 4, a stab riff whose pitch moves, closed hats on the eighths and an open hat before the turnaround. Four instruments in the four default bands.',
    bpm: 125,
    bars: 2,
    hits: [
      [0, 'kick'], [4, 'kick'], [8, 'kick'], [12, 'kick'],
      [16, 'kick'], [20, 'kick'], [24, 'kick'], [28, 'kick'], [30, 'kick'],
      [4, 'clap'], [12, 'clap'], [20, 'clap'], [28, 'clap'],
      [3, 'stab16'], [7, 'stab20'], [10, 'stab24'],
      [19, 'stab16'], [23, 'stab28'], [27, 'stab24'], [29, 'stab20'],
      [0, 'hat'], [2, 'hat'], [4, 'hat'], [6, 'open'], [8, 'hat'], [10, 'hat'], [12, 'hat'], [14, 'open'],
      [16, 'hat'], [18, 'hat'], [20, 'hat'], [22, 'open'], [24, 'hat'], [26, 'hat'], [28, 'hat'], [30, 'open'],
    ],
  },
  {
    id: 'metronome',
    name: 'Metronome, 120 BPM',
    hint: 'One kick-shaped click on every beat and nothing else. Tempo Source = Detected should read 120 within ±1 BPM after four seconds, and the header’s ? becomes *.',
    bpm: 120,
    bars: 1,
    hits: [[0, 'kick'], [4, 'kick'], [8, 'kick'], [12, 'kick']],
  },
  {
    id: 'sparse',
    name: 'Sparse, 125 BPM, four bars',
    hint: 'A kick on the one, a stab every other bar, one open hat. Most rows stay ---, which is what the ring is for: the pattern shows what was heard and nothing else.',
    bpm: 125,
    bars: 4,
    hits: [[0, 'kick'], [16, 'kick'], [32, 'kick'], [48, 'kick'], [6, 'stab20'], [38, 'stab28'], [30, 'open'], [62, 'open']],
  },
  {
    id: 'silence',
    name: 'Silence',
    hint: 'Every bin zero. The cursor still runs on the clock; nothing is written and the ring clears what was.',
    bpm: 120,
    bars: 1,
    hits: [],
  },
];

/** An integer hash of (millisecond, bin) in 0..1, for the jitter. */
function jitter01(ms, j) {
  let h = (Math.imul(ms | 0, 0x9e3779b1) ^ Math.imul(j + 1, 0x85ebca6b)) >>> 0;
  h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d) >>> 0;
  h = Math.imul(h ^ (h >>> 12), 0x297a2d39) >>> 0;
  return ((h ^ (h >>> 15)) >>> 0) / 4294967296;
}

/** The page's spectrum at `time`, as the host's floats. */
function writeSpectrum(bins, programme, time) {
  bins.fill(0);
  if (!(time >= 0)) return;
  const stepSeconds = 60.0 / programme.bpm / 4.0;
  const period = programme.bars * 16 * stepSeconds;
  const u = time - Math.floor(time / period) * period;
  const acc = new Float64Array(BINS);
  for (const [step, kind] of programme.hits) {
    const hit = HITS[kind];
    const at = step * stepSeconds;
    for (const start of [at, at - period]) {
      const dt = u - start;
      if (dt < 0 || dt > 5 * hit.tau) continue;
      const a = hit.amp * Math.exp(-dt / hit.tau);
      for (let j = 0; j < BINS; j += 1) acc[j] += a * hit.shape[j];
    }
  }
  const ms = Math.floor(time * 1000);
  for (let j = 0; j < BINS; j += 1) {
    const v = acc[j] * (1.0 + 0.1 * (jitter01(ms, j) - 0.5));
    bins[j] = v > 1e-6 ? v : 0.0;
  }
}

//===========================================================================
// PORT — Renderer::InitGL and Renderer::Draw. The screen is an RGBA8UI texture
// re-uploaded every frame, reallocated when the cell grid changes shape; the
// font is uploaded once. `uFrac` is 0.0, as on every frame the plugin renders.
//===========================================================================

function createRenderer(gl) {
  const program = new Program(gl, VERTEX_SHADER, FRAGMENT_SHADER, 'pattern', { attribs: {} });

  // No vertex buffer: the corners come from gl_VertexID, so only a VAO is bound.
  const vao = gl.createVertexArray();

  const font = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, font);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, FONT_TEXTURE_WIDTH, FONT_HEIGHT, 0, gl.RED, gl.UNSIGNED_BYTE, fontTexture());
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const screenTex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, screenTex);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  let screenCols = 0;
  let screenRows = 0;

  const tracker = new Tracker();
  const bins = new Float32Array(BINS);
  const barData = new Int32Array(8 * 4);

  const loc = (name) => program.location(`${name}[0]`) ?? program.location(name);

  function draw(s, l, palette, width, height) {
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, screenTex);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    if (s.cols !== screenCols || s.rows !== screenRows) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8UI, s.cols, s.rows, 0, gl.RGBA_INTEGER, gl.UNSIGNED_BYTE, s.cells);
      screenCols = s.cols;
      screenRows = s.rows;
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, s.cols, s.rows, gl.RGBA_INTEGER, gl.UNSIGNED_BYTE, s.cells);
    }

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.BLEND);

    program.use();
    gl.uniform2i(program.location('uSize'), width, height);
    gl.uniform2i(program.location('uOrigin'), l.originX, l.originY);
    program.setInt('uScale', Math.max(1, l.scale));
    program.set('uFrac', 0.0);
    gl.uniform2i(program.location('uCells'), s.cols, s.rows);
    gl.uniform3fv(loc('uPal'), palette);

    barData.fill(0);
    const n = Math.min(s.bars.length, 8);
    for (let b = 0; b < n; b += 1) {
      barData[b * 4 + 0] = s.bars[b].x;
      barData[b * 4 + 1] = s.bars[b].y;
      barData[b * 4 + 2] = s.bars[b].w;
      barData[b * 4 + 3] = s.bars[b].h;
    }
    program.setInt('uBarCount', n);
    gl.uniform4iv(loc('uBar'), barData);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, font);
    program.setSampler('uFont', 0);
    program.setSampler('uScreen', 1);

    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    gl.bindVertexArray(null);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, null);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, null);
    gl.useProgram(null);
  }

  return {
    tracker,
    bins,
    render({ params, width, height, time, variant }) {
      const programme = PROGRAMMES.find((p) => p.id === variant) ?? PROGRAMMES[0];

      // The host's spectrum for this frame -- the page's, a pure function of
      // the clock -- and the host's tempo, which the page sets to the
      // programme's own as SetBeatInfo would.
      writeSpectrum(bins, programme, time);

      // PORT — PatternPlugin::Advance, then BuildScreen and LayoutFor.
      tracker.setSettings(currentSettings(params, programme.bpm));
      tracker.frame(time, bins, BINS);

      const screen = compose(tracker, currentDisplay(params));
      const layout = layoutFor(screen, width, height, intParam(integerValue('scale', params.get('scale')), kScaleMin, kScaleMax));
      draw(screen, layout, paletteFor(toOption(params.get('theme'), kThemeCount)), width, height);
      readout(tracker, programme, layout);
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const std = (id, name, def, group, extra = {}) =>
  ({ id, name, type: 'standard', default: def, group, ...(typeof extra === 'string' ? { hint: extra } : extra) });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def ? 1 : 0, group, hint });
const integer = (id, name, value, group, hint) =>
  ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: value - INTEGER_RANGES[id][0], group, hint });

const NOT_AUDIO =
  'There is no audio on this page: the 64 bins are written by this page from the programme chosen under the picture, laid out linearly and as magnitudes — not by Resolume’s FFT, whose layout, value law and sample rate nobody has measured.';

const edgesText = (v) => {
  const edges = new Int32Array(kMaxChannels + 1);
  bandEdges(4, 0, SplitExponent(v), edges);
  return `γ ${SplitExponent(v).toFixed(2)} (${edges[1]},${edges[2]},${edges[3]})`;
};

const PARAMS = [
  // -- Clock --------------------------------------------------------------
  opt('tempoSource', 'Tempo Source', ['Host', 'Detected', 'Manual'], 0, 'Clock',
    'Where the row clock’s BPM comes from. Host: what SetBeatInfo says — on this page the programme’s own tempo, in Resolume the composition’s. Detected: the ported tempo detector — the onset envelope in three registers (low, mid, high), each standardised, autocorrelated once a second over the last six, the beat chosen as the fastest level scoring within 0.70 of the best, and published once two readings in a row agree; the header shows ? until it has settled and * after. Manual: the BPM control.'),
  std('bpm', 'BPM', kBpmDefault, 'Clock', {
    display: (v) => `${BpmFromControl(v).toFixed(1)} BPM`,
    hint: 'Manual tempo, 60 to 200. The default of 0.4642857 is 125 BPM, the figure the Amiga-era trackers ran at (a tick of 20 ms). Read only when Tempo Source is Manual.',
  }),
  integer('speed', 'Speed', kSpeedDefault, 'Clock',
    'Ticks a row, 1 to 31; a tick is 2.5 / BPM seconds, so 6 is four rows to a beat. FF_TYPE_INTEGER in the plugin; the kit has no integer control, so this is a dropdown of every value.'),
  std('swing', 'Swing', 0.0, 'Clock', {
    display: (v) => `${(SwingFromControl(v) * 100).toFixed(0)}% of a row`,
    hint: 'How late an odd row starts, up to half a row. A pair of rows keeps its length, so the beat stays where it was.',
  }),
  opt('rows', 'Rows', ['64', '32'], 0, 'Clock',
    'The pattern’s length. After the last row the cursor wraps and the next pass overwrites the last.'),
  bool('keep', 'Keep Notes', false, 'Clock',
    'Off: entering a row on a new pass clears it, so a sparse passage leaves dots and an arrangement change shows as replacement. On: nothing is ever removed and the pattern fills to solid.'),

  // -- Listening ----------------------------------------------------------
  opt('channels', 'Channels', ['4', '8'], 0, 'Listening',
    'How many channels the 64 bins are divided between, one band each. With 8 the page’s four instruments straddle bands, as music does.'),
  opt('binLaw', 'Bin Law', ['Linear', 'Log'], 0, 'Listening',
    `How the 64 bins are laid out in frequency, which decides what pitch a band’s peak means. Nobody has measured what Resolume sends. ${NOT_AUDIO} So Linear reads the programme as it was written, and Log shows what the other assumption does to the same numbers.`),
  opt('binValue', 'Bin Value', ['Magnitude', 'Power'], 0, 'Listening',
    'Whether a bin is a magnitude or a power (the square root is taken). The page writes magnitudes, so Magnitude reads them as written.'),
  std('sensitivity', 'Sensitivity', kSensitivityDefault, 'Listening', {
    display: (v) => `ratio ${ThresholdRatio(v).toFixed(2)}`,
    hint: `The onset threshold as a multiple of a band’s one-second mean flux: 12 at 0, 4.24 at 0.5, 1.5 at 1, logarithmic. The plugin’s thresholds were set on synthetic spectra; this is the hedge. ${NOT_AUDIO}`,
  }),
  std('bandSplit', 'Band Split', kBandSplitDefault, 'Listening', {
    display: edgesText,
    hint: 'The exponent that tilts the band edges towards the low end: edges at (k / N)^γ in bin index, γ from 1 (equal widths) to 4. The bracket is the 4-channel edges in bins. At the default the kick, clap, stab and hats each own a band.',
  }),
  bool('fold', 'Fold Octaves', true, 'Listening',
    'A pitch outside C-1..B-3 is moved by whole octaves into it, so the note name is the pitch class the band’s peak has and only the octave digit is a fiction. Off: --- wherever the pitch is out of range, which under Linear at 44.1 kHz is every bin but the first three.'),

  // -- Display ------------------------------------------------------------
  opt('theme', 'Theme', ['Grey', 'Blue', 'Green'], 0, 'Display',
    'Colour schemes named by colour. No tracker’s name appears anywhere.'),
  integer('rowsVisible', 'Rows Visible', kRowsVisibleDefault, 'Display',
    'How many pattern rows are drawn, 3 to 33, the cursor at the centre. FF_TYPE_INTEGER in the plugin, a dropdown here.'),
  bool('showVu', 'Show VU', true, 'Display',
    'One meter per channel, 48 steps, set by a note’s volume and falling one step every 20 ms.'),
  bool('showEffects', 'Show Effects', true, 'Display',
    'The Cxx volume column. Off narrows every channel to note and sample.'),
  integer('scale', 'Scale', kScaleDefault, 'Display',
    'Whole output pixels per screen pixel, 1 to 8; 0 is Auto, the largest that fits both ways. A screen that does not fit is centred and cropped. FF_TYPE_INTEGER in the plugin, a dropdown here.'),
];

let readoutNode = null;
let readoutAt = 0;

function readout(tracker, programme, layout) {
  if (!readoutNode) return;
  const now = performance.now();
  if (now - readoutAt < 150) return;
  readoutAt = now;
  const detected = tracker.detectedBpm > 0 ? `${tracker.detectedBpm.toFixed(1)} BPM` : 'not settled';
  const rowHex = `${kHex[tracker.row >> 4]}${kHex[tracker.row & 15]}`;
  readoutNode.textContent =
    `Programme: ${programme.bpm} BPM (the page’s, handed to the port as the host’s) · clock in use ${tracker.bpm.toFixed(1)} BPM · row ${rowHex} of pass ${tracker.pass} · detected ${detected} · screen ${layout.screenW} × ${layout.screenH} at Scale ${layout.scale}, origin ${layout.originX}, ${layout.originY}`;
}

const mounted = mountDemo({
  name: 'Pattern',
  // The FFGL type the plugin registers (PluginInfo), for the kit banner's
  // closing sentence.
  kind: 'source',
  pluginId: 'PN01',
  tagline:
    'A tracker’s pattern editor, written by the music as it plays. The row cursor runs on the tracker’s own clock — Speed ticks a row, a tick of 2.5 / BPM seconds — and an onset in each channel’s band of the 64-bin spectrum writes a note, a sample number and a Cxx volume into the current row. After row 3F the cursor wraps and the next pass overwrites the last.',
  repo: 'https://github.com/stoatworks-labs/pattern',
  page: 'https://stoatworks-labs.com/software/pattern/',

  // Pattern is a SOURCE. The stock clause says "on generated clips".
  blurb:
    'It is Pattern’s own GLSL, ported from the repository to WebGL2, driven by a JavaScript port of the plugin’s tracker, screen composer and layout — same parameters, same maths, no install. There is no audio here: the 64-bin spectrum is synthesised by this page from a drum-loop-like programme on its own clock. That is the page’s spectrum, not a host’s, and the plugin’s onset thresholds were set on synthetic spectra like it, never on music through Resolume’s FFT.',

  // The screen is drawn opaque over the whole output, as the plugin does, so
  // there is no alpha for a backdrop to show through.
  showBackdrop: false,

  sources: [],

  variants: {
    label: 'Programme',
    default: 'beats',
    options: PROGRAMMES.map(({ id, name, hint }) => ({ id, name, hint })),
  },

  params: PARAMS,

  differences: [
    'There is no audio. The plugin reads one thing from its host — Resolume’s 64-bin FFT buffer, once per frame — and a browser has no Resolume. This page does not ask for a microphone. A pattern editor with no audio is an empty grid, so the page synthesises a spectrum: a drum-loop-like programme (a kick, a clap, a stab whose pitch moves, closed and open hats on a 16-step grid at 125 BPM; or a metronome, a sparse pattern, silence) is written straight into 64 bins as decaying spectral shapes on the page’s clock, with a ±5 % per-bin jitter from an integer hash. Nothing is sampled and nothing is licensed.',
    'It is the page’s spectrum, not a host’s. The bins are laid out as the plugin’s Linear law assumes — bin j centred at (j + ½) / 64 of a 22.05 kHz Nyquist — and written as magnitudes, so Bin Law on Linear and Bin Value on Magnitude read the programme as written and the other settings show what the other assumptions do to the same numbers. Resolume’s window, normalisation, bin layout and sample rate are unknown; SetSampleRate is never called here, so the port assumes 44.1 kHz exactly as the plugin does when a host never says. The plugin’s onset thresholds (the ratio, the one-second floor, the 50 ms refractory, the eighth-of-level seed) were set on synthetic spectra and one synthetic WAV, never on programme material through Resolume’s FFT, and this page is the same kind of evidence.',
    'The page is the host’s tempo. Resolume tells a plugin its BPM through SetBeatInfo; here the port is handed the programme’s own tempo the same way, so Tempo Source = Host is exactly right on this page and only as right as the composition’s BPM in Resolume. Detected runs the ported detector on the page’s onsets for real (the drum loop reads 125 from about four seconds, the metronome 120; the sparse programme has no beat in range and stays ?), and its ? / * in the header is the port’s own verdict.',
    'Speed, Rows Visible and Scale are FF_TYPE_INTEGER in the plugin, holding a real integer with a real range. The kit has no integer control, so they are dropdowns of every value in the plugin’s range, and the integer is what the port receives.',
    'The Audio buffer parameter and the About block are absent from the panel. The buffer is written by a host, not an operator — the programme stands in for it — and the About block’s text line and link buttons exist so a host has somewhere to put links a web page already has. The other 17 parameters are all here, in the constructor’s order and groups, with its names, element lists and defaults.',
    'There is no clip, no "use my own file" and no backdrop. Pattern is a source with zero inputs, and its screen is drawn opaque over the whole output; the kit offers the clip controls to every demo and this page removes them rather than leaving them present and inert.',
    'The whole CPU half is a JavaScript port, and nothing checks it but a reader: the bin law and the tracker notes, the band edges, the row clock as base + (t − anchor) / T in double with the 1e-6 row allowance and swing, the ring that clears a row as it is entered (or keeps it), the spectral-flux onset detector primed on frame one and seeded at an eighth of the level, the refractory, the strongest-onset-wins rule, the Cxx volume law, the v0.1.1 tempo detector (three standardised registers, the pulse-grouping-subdivision score, the fastest level within 0.70 of the best, the two-reading agreement, the parabolic refinement at a multiple of the lag), the VU meters, the screen composer, LayoutFor, the three palettes, every Controls.h conversion (rounded through Math.fround where the C++ takes a float), ToOption, Renderer::Draw’s uploads and uniform packing, and the 5 × 7 font. The shaders are the plugin’s, unedited and spliced in by script; demo/tools/check_shaders.py fails the repository’s verify script if a character drifts. uFrac, the harness’s negative control in the shader, is 0.0 here as on every frame the plugin renders.',
    'The clock is the browser’s frame clock in seconds. The plugin’s Clock, which works out what unit a host’s SetTime is in, is not ported, because there is no unit to discover here. Restart sends the clock backwards, which the tracker treats as a loop point — the phase is carried across and no time passes on that frame — and a paused page moving a control re-runs the same frame with the same spectrum, as a host re-sends its buffer.',
    'Nothing here is measured. The cursor on floor( t / T ) mod 64 on every frame of ten minutes from a host clock at 499 million milliseconds, one note per onset in the right channel on the right row, no false note on frame one with loud audio already playing, the pitch the bin law predicts for all 64 bins, the ring, a detected tempo within ±1 BPM in 4.0 s on a metronome and on drum grooves from 80 to 170 BPM at the right metrical level and every glyph on the whole-pixel grid at every Scale are checked by pntest in the repository, with no GL context for all but the last. That harness, not this page, is the reason to believe the tracker.',
    'The plugin has never been loaded into Resolume on any platform and no real audio has reached it in a host. This page is a browser and is evidence about neither.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The two controls a source has no use for, removed rather than left dead —
// needle's and astable's precedent. And two lines under the picture: what the
// programme is, and what the port is doing this frame.
//---------------------------------------------------------------------------
for (const field of document.querySelectorAll('.transport__field')) {
  if (field.querySelector('.transport__label')?.textContent === 'Clip') field.remove();
}
document.querySelector('.transport__file')?.remove();

const transportBar = document.querySelector('.transport');
if (transportBar && mounted) {
  const note = document.createElement('p');
  note.className = 'stage__status';
  note.textContent =
    'A synthesised spectrum, not audio. The 64 bins are written by this page from the programme chosen above — a drum-loop-like set of spectral shapes on the page’s own clock, nothing sampled — and the port of the plugin’s tracker listens to them. That is not Resolume’s FFT: the plugin’s onset thresholds were set on synthetic spectra like this one, and how a host’s bins are laid out is unknown.';
  readoutNode = document.createElement('p');
  readoutNode.className = 'stage__status stage__readout';
  transportBar.after(note, readoutNode);
}
