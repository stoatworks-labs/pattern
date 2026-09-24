/**
	The FF_SOURCE registration, and nothing else.

	**This file is listed directly in the source target, not in the shared
	object library.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a static archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Pattern.bundle/Contents/MacOS/Pattern | grep plugMain

	That is also why the shared code is an OBJECT library rather than a STATIC
	one, and it is why `oxbow selftest` is the only check that proves a bundle
	actually registers anything.

	`PN01`: four characters, unique across the fleet.
*/
#include "Pattern.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< pattern::PatternPlugin >,                 // Create method
	"PN01",                                                  // Plugin unique ID of maximum length 4
	"SW Pattern",                                            // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_SOURCE,                                               // Plugin type
	"A tracker's pattern editor, written by the music as it plays.\n\n"
	"A row cursor runs on a tracker's own clock -- a row is Speed x 2.5 / BPM seconds, four rows to a beat "
	"at the default speed of 6 -- locked to the host's tempo, a detected tempo, or one you set. The plugin "
	"listens to the host's 64-bin spectrum: each channel owns a band of it, and an onset in that band writes a "
	"note into the channel on the current row -- its pitch from the band's peak, its volume as a Cxx effect "
	"from the onset's strength, its sample number from the channel. After row 3F the cursor wraps and the next "
	"pass overwrites the last, so a busy passage fills every row and a sparse one leaves dots.\n\n"
	"VU bars per channel with a vblank meter's fall rate, position and pattern counters, three colour themes, "
	"a bitmap font on a whole-pixel grid. Four or eight channels, 64 or 32 rows.",// Plugin description
	"Pattern FFGL source"                                    // About
);

extern "C" const char* PatternSourceBuildStamp()
{
	return "pattern " PATTERN_VERSION " source, built " __DATE__ " " __TIME__;
}
