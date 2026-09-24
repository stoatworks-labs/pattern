#pragma once

/**
	Bins to frequencies, frequencies to notes, and the 64 bins to channels.

	Pure functions, no state, no GL, no host: everything the harness's
	`--pitch` check reasons about is in here.

	## What is assumed about the 64 bins, said outright

	Resolume fills an `FF_USAGE_FFT` buffer of 64 floats once per frame, and
	nobody in this fleet has measured how those bins are laid out in
	frequency, whether they are magnitudes or powers, or what the sample rate
	behind them is. A pattern editor cannot dodge the first question the way
	needle's level meter could (a sum over every bin is layout-independent by
	Parseval): a *band* is a range of bins, and a *pitch* is a bin's frequency.
	So the layout is a control, `Bin Law`:

	- **Linear**: bin j covers [j, j + 1) / 64 of the Nyquist frequency, which
	  is what a 128-point FFT hands back and what the fleet has always assumed.
	- **Log**: bin j is centred at 20 Hz x (fN / 20 Hz)^((j + 1/2) / 64), which
	  is what a host that presents "bands" rather than bins would send.

	The sample rate comes from the host's `SetSampleRate`; 44100 Hz when the
	host never says.

	## Notes

	A tracker's note range, C-1 to B-3, is three octaves of note NAMES. What
	frequency those names mean depends on the sample that is played, so the
	mapping here is the usual convention: the tracker's C-2 is middle C
	(MIDI 60, 261.63 Hz), so C-1 is 130.81 Hz and B-3 is 987.77 Hz. A
	frequency is taken to the nearest semitone at A4 = 440 Hz. Outside the
	range it is either `---` or, with Fold on, moved by whole octaves until it
	is inside -- see Tracker.h for why Fold is on by default.
*/
namespace pattern::bands
{

inline constexpr int kBins = 64;

/// The tracker's note range, as semitones from MIDI 0. C-1 is MIDI 48, B-3
/// is MIDI 83: thirty-six notes.
inline constexpr int kFirstMidi = 48;
inline constexpr int kNoteCount = 36;

/// The centre frequency of bin j under a law.
double BinFrequency( int j, int law, double sampleRate );

/// A frequency to the nearest MIDI note number (A4 = 440 is 69). Returns a
/// negative number for a non-positive frequency.
int MidiFromFrequency( double hz );

/// A MIDI note to the tracker's 0..35 index, or -1 when it is outside C-1 to
/// B-3. With `fold`, moved by octaves into the range first.
int TrackerNote( int midi, bool fold );

/// The three-character name of tracker note 0..35: "C-1", "C#1", ... "B-3".
/// Anything out of range is "---".
const char* NoteName( int trackerNote );

/// All the band edges at once: `edges` has `channels + 1` entries, channel k
/// covers bins [edges[ k ], edges[ k + 1 ]), edges[ 0 ] is 0 and
/// edges[ channels ] is kBins. Every channel gets at least one bin, and the
/// edges are strictly increasing by construction.
void BandEdges( int channels, int law, double gamma, int* edges );

/// One edge of the above, for a caller that wants a single number.
int BandEdge( int k, int channels, int law, double gamma );

} // namespace pattern::bands
