# Attributions

Pattern is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

**Provisional hand copy.** In a released repo this file is generated — the master
lists live in the `stoatworks-backend` repo and are pushed out by
`scripts/sync-attributions.py`. pattern is not registered yet, so this copy was
written by hand in the same shape; the first sync overwrites it.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Graticule bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Font.{h,cpp} — the 5×7 bitmap font every character on the pattern screen is drawn from — carried across from graticule unchanged, by way of needle. It is graticule's own design, drawn as pictures in Font.cpp; it is not a copy of, and was not traced from, any computer's ROM font or any tracker's screen font. Here each glyph sits in a 6×8 cell.

### Graticule Diag logger and host Clock — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.* (the log-file writer, from orrery via graticule) and source/Clock.* (the clock that measures what unit the host's SetTime arrives in, from flipbook via graticule), renamed into this namespace. Clock gained `ForceMilliseconds` from copperlist.

### Source-plugin template and harness plumbing — Stoatworks needle

<https://github.com/stoatworks-labs/needle>  
Licence: MIT  
Copyright: Stoatworks Labs

needle is the structural template for a source plugin: the CMake shape, the OBJECT core, SourcePlugin.cpp, the About headers, the harness's CGL plumbing, PNG writer, --list/--names/--out/--pipe, the audio buffer written through SetParamElementValue, and the two-raster discipline. Nothing of the meter engine is here.

### Pipe cue sheet and field arithmetic — Stoatworks copperlist

<https://github.com/stoatworks-labs/copperlist>  
Licence: MIT  
Copyright: Stoatworks Labs

The --pipe cue-script format and parser, the two-raster sweep in verify.sh, and the lesson that a floor over a host time needs a 1e-6 allowance because 1/60 is not a double.

### Verify, sweep and workflows — Stoatworks needle and tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/verify.sh, tools/sweep.py and the two workflows are adapted from needle's, which descend from tinsel's; graticule's and tinsel's trap lists are applied throughout.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Amiga-era trackers, as a genre

The pattern editor's arithmetic — 64 rows, a tick of 2.5 / BPM seconds, Speed ticks to a row, hexadecimal row numbers, sample numbers and a `Cxx` volume effect, the note range C-1 to B-3 with C-2 as middle C — is the genre's public convention, and that is all that was taken. No source code, screen layout, font, colour scheme or asset was taken from any tracker, no tracker's output was captured or consulted, and no tracker is named or depicted on screen: the themes are named by colour. The font is graticule's (above). The VU meters' 48 steps and 20 ms fall are this repo's own model of a vblank-driven meter, not a figure read from any program.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
