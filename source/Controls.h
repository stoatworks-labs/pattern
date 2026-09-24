#pragma once

#include <cmath>

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

/**
	Every parameter, and what its host-side value means.

	## The two units

	- **`FF_TYPE_STANDARD` is 0..1**, always. `SetParamInfo` clamps the default
	  into that range before `SetParamRange` could widen it, so a standard
	  parameter that means anything else is mapped here, in a named inline
	  function, and nowhere else.
	- **`FF_TYPE_INTEGER` holds a real integer** with a real range: Speed,
	  Rows Visible and Scale are counts, and the clamp does not apply to them.
	- **`FF_TYPE_OPTION` holds the element VALUE**, which for every dropdown
	  here is its index. Its SDK range reads back 0..1 whatever the element
	  count (the fleet trap); `ToOption` in Pattern.cpp accepts either an index
	  or a normalised 0..1, and `pntest --list` prints the real range for the
	  sweep.
	- **`FF_TYPE_BOOLEAN` is 0 or 1**, read as `> 0.5f`.
	- **`FF_TYPE_BUFFER`** holds the host's 64-bin spectrum. The host writes
	  it, not the operator; it is the only parameter that carries information
	  rather than intent.

	## Order is load-bearing

	The host draws parameters in declaration order and `SetParamGroup`
	collapses *runs* of the same group name into one fold, so an id moved out
	of its run splits its group in two. The enum below is the inspector, top
	to bottom.

	## Names

	FFGL truncates a parameter name at 16 characters, silently. `pntest
	--names` lists any that are over, and any duplicate: `--set`, the cue
	script and `tools/sweep.py` all find a parameter by its name.
*/
namespace pattern
{
enum ParamId : FFUInt32
{
	// -- Clock --------------------------------------------------------------
	PT_TEMPO_SOURCE,
	PT_BPM,
	PT_SPEED,
	PT_SWING,
	PT_ROWS,
	PT_KEEP,

	// -- Listening ----------------------------------------------------------
	PT_CHANNELS,
	PT_BIN_LAW,
	PT_BIN_VALUE,
	PT_SENSITIVITY,
	PT_BAND_SPLIT,
	PT_FOLD,
	PT_AUDIO,///< the host's spectrum; see tracker/Tracker.h

	// -- Display ------------------------------------------------------------
	PT_THEME,
	PT_ROWS_VISIBLE,
	PT_SHOW_VU,
	PT_SHOW_EFFECTS,
	PT_SCALE,

	// -- About --------------------------------------------------------------
	// One text line and one button per link. Its size is decided by
	// StoatworksAbout.h at compile time, so Pattern.cpp static_asserts this
	// run against `about::kParamCount`. With a guide URL that is five
	// entries: the text, then User guide, Project page, Source on GitHub and
	// Support the work.
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,

	PT_COUNT_
};

/// Where the row clock's tempo comes from.
enum class TempoSource : int
{
	Host = 0, ///< the host's transport, through SetBeatInfo
	Detected, ///< autocorrelation of the onset envelope
	Manual,   ///< the BPM control
	Count
};
constexpr int kTempoSourceCount = static_cast< int >( TempoSource::Count );

/// How the 64 bins are laid out in frequency. Nobody has measured what
/// Resolume sends, so it is a control -- see tracker/Bands.h.
enum class BinLaw : int
{
	Linear = 0,///< bin j centred at (j + 1/2) / 64 of Nyquist
	Log,       ///< bin j centred at 20 Hz x (Nyquist / 20 Hz)^((j + 1/2) / 64)
	Count
};
constexpr int kBinLawCount = static_cast< int >( BinLaw::Count );

/// Whether a bin value is a magnitude or a power. The fleet disagrees with
/// itself about this (regauss takes the square root, spasis does not), so it
/// is a control here as it is in needle.
enum class BinValue : int
{
	Magnitude = 0,
	Power,
	Count
};
constexpr int kBinValueCount = static_cast< int >( BinValue::Count );

/// Colour schemes, named by colour. No tracker's name appears anywhere.
enum class Theme : int
{
	Grey = 0,
	Blue,
	Green,
	Count
};
constexpr int kThemeCount = static_cast< int >( Theme::Count );

// ---------------------------------------------------------------------------
// The 0..1 controls, in engineering units.
// ---------------------------------------------------------------------------

/// Manual tempo, 60 to 200 BPM. The default of 0.4642857 is **125 BPM**, the
/// figure the Amiga-era trackers ran at by default (a tick of 20 ms, one PAL
/// field).
inline double BpmFromControl( float v )
{
	return 60.0 + 140.0 * static_cast< double >( v );
}
inline constexpr float kBpmDefault = 0.4642857f;

/// Swing: how late an odd row starts, as a fraction of a row. 0 to 0.5.
inline double SwingFromControl( float v )
{
	return 0.5 * static_cast< double >( v );
}

/// Sensitivity to the onset detector's threshold ratio. The detector fires
/// when a band's spectral flux exceeds its own running mean by this factor:
/// 12 at Sensitivity 0, 4.24 at 0.5, 1.5 at 1. Logarithmic, so the middle of
/// the travel is a usable middle.
inline double ThresholdRatio( float v )
{
	return 1.5 * std::pow( 2.0, 3.0 * ( 1.0 - static_cast< double >( v ) ) );
}
inline constexpr float kSensitivityDefault = 0.5f;

/// Band Split: the exponent that tilts the band edges towards the low end.
/// Edges sit at (k / N)^gamma in the bin law's own coordinate, gamma from 1
/// (equal widths) to 4. Under Log, 1 is octave-equal bands already; under
/// Linear, 1 gives every channel the same slab of hertz, which puts the
/// whole of the music into channel 1.
inline double SplitExponent( float v )
{
	return 1.0 + 3.0 * static_cast< double >( v );
}
inline constexpr float kBandSplitDefault = 0.4f;

/// Integer ranges.
inline constexpr int kSpeedMin = 1, kSpeedMax = 31, kSpeedDefault = 6;
inline constexpr int kRowsVisibleMin = 3, kRowsVisibleMax = 33, kRowsVisibleDefault = 17;
inline constexpr int kScaleMin = 0, kScaleMax = 8, kScaleDefault = 0;///< 0 is Auto

} // namespace pattern
