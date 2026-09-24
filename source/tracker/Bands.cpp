#include "Bands.h"

#include <algorithm>
#include <cmath>

namespace pattern::bands
{
namespace
{
constexpr double kLogFloorHz = 20.0;

/// Bin j's centre in the law's own 0..1 coordinate: linear in hertz under
/// Linear, linear in octaves under Log.
double Centre01( int j )
{
	return ( static_cast< double >( j ) + 0.5 ) / static_cast< double >( kBins );
}
} // namespace

double BinFrequency( int j, int law, double sampleRate )
{
	const double nyquist = 0.5 * sampleRate;
	const double u       = Centre01( std::min( std::max( j, 0 ), kBins - 1 ) );
	if( law == 1 )
		return kLogFloorHz * std::pow( nyquist / kLogFloorHz, u );
	return u * nyquist;
}

int MidiFromFrequency( double hz )
{
	if( !( hz > 0.0 ) )
		return -1;
	return static_cast< int >( std::lround( 69.0 + 12.0 * std::log2( hz / 440.0 ) ) );
}

int TrackerNote( int midi, bool fold )
{
	if( midi < 0 )
		return -1;
	int n = midi - kFirstMidi;
	if( fold )
	{
		// Whole octaves only, so the note NAME is the one the pitch class
		// has, and only the octave digit is a lie.
		while( n < 0 )
			n += 12;
		while( n >= kNoteCount )
			n -= 12;
	}
	return ( n >= 0 && n < kNoteCount ) ? n : -1;
}

const char* NoteName( int trackerNote )
{
	static const char* const kNames[ kNoteCount ] = {
		"C-1", "C#1", "D-1", "D#1", "E-1", "F-1", "F#1", "G-1", "G#1", "A-1", "A#1", "B-1",
		"C-2", "C#2", "D-2", "D#2", "E-2", "F-2", "F#2", "G-2", "G#2", "A-2", "A#2", "B-2",
		"C-3", "C#3", "D-3", "D#3", "E-3", "F-3", "F#3", "G-3", "G#3", "A-3", "A#3", "B-3",
	};
	if( trackerNote < 0 || trackerNote >= kNoteCount )
		return "---";
	return kNames[ trackerNote ];
}

void BandEdges( int channels, int law, double gamma, int* edges )
{
	(void)law;// the edges are placed in the law's own coordinate, which is bin index either way
	channels   = std::max( 1, std::min( channels, kBins ) );
	edges[ 0 ] = 0;
	for( int k = 1; k < channels; ++k )
	{
		// (k / N)^gamma of the way through the bins, rounded, then forced to
		// leave every channel at least one bin: at least one above the edge
		// below it, and room for (channels - k) bins above it.
		const double u    = std::pow( static_cast< double >( k ) / channels, gamma );
		int          edge = static_cast< int >( std::lround( u * kBins ) );
		edge              = std::max( edge, edges[ k - 1 ] + 1 );
		edge              = std::min( edge, kBins - ( channels - k ) );
		edges[ k ]        = edge;
	}
	edges[ channels ] = kBins;
}

int BandEdge( int k, int channels, int law, double gamma )
{
	int edges[ kBins + 1 ];
	BandEdges( channels, law, gamma, edges );
	return edges[ std::min( std::max( k, 0 ), std::max( 1, std::min( channels, kBins ) ) ) ];
}

} // namespace pattern::bands
