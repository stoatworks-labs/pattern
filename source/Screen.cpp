#include "Screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pattern
{
namespace
{
const char kHex[] = "0123456789ABCDEF";

struct Writer
{
	Screen& s;
	void put( int col, int row, char c, uint8_t fg, uint8_t bg )
	{
		if( col < 0 || row < 0 || col >= s.cols || row >= s.rows )
			return;
		ScreenCell& cell = s.cells[ static_cast< size_t >( row ) * s.cols + col ];
		cell.glyph       = static_cast< uint8_t >( ( c >= 32 && c < 127 ) ? c : '?' );
		cell.fg          = fg;
		cell.bg          = bg;
	}
	void text( int col, int row, const char* t, uint8_t fg, uint8_t bg )
	{
		for( ; *t != '\0'; ++t, ++col )
			put( col, row, *t, fg, bg );
	}
};
} // namespace

int Screen::CursorRow( const DisplaySettings& d ) const
{
	return kHeaderRows + ( d.showVu ? kVuRows : 0 ) + d.rowsVisible / 2;
}

int Screen::ChannelColumn( int k, bool showEffects )
{
	// "rr " then, per channel, "|" and the text.
	return 3 + k * ( 1 + ChannelWidth( showEffects ) ) + 1;
}

Screen Compose( const Tracker& tracker, const DisplaySettings& display )
{
	const TrackerSettings& ts = tracker.Settings();
	const int channels  = ts.channels;
	const int visible   = std::max( 1, display.rowsVisible );
	const int colw      = Screen::ChannelWidth( display.showEffects );

	Screen s;
	s.cols = 3 + channels * ( 1 + colw ) + 1;
	s.rows = Screen::kHeaderRows + ( display.showVu ? Screen::kVuRows : 0 ) + visible;
	s.cells.assign( static_cast< size_t >( s.cols ) * s.rows, ScreenCell{} );
	Writer w{ s };

	// -- the counters --------------------------------------------------------
	{
		const int pos = static_cast< int >( tracker.Pass() % 128 );
		const int pat = static_cast< int >( tracker.Pass() % 64 );
		const int bpm = std::min( static_cast< int >( std::lround( tracker.Bpm() ) ), 999 );
		char      value[ 8 ];
		int       col = 0;
		// Labels in the accent colour, values in the text colour. Explicit
		// segments rather than "letters are labels": POS and PAT are hex.
		auto label = [ & ]( const char* t ) { w.text( col, 0, t, PAL_ACCENT, PAL_BACK ); col += static_cast< int >( std::strlen( t ) ); };
		auto text  = [ & ]( const char* t ) { w.text( col, 0, t, PAL_TEXT, PAL_BACK ); col += static_cast< int >( std::strlen( t ) ); };
		label( "POS " );
		std::snprintf( value, sizeof( value ), "%c%c ", kHex[ pos >> 4 ], kHex[ pos & 15 ] );
		text( value );
		label( "PAT " );
		std::snprintf( value, sizeof( value ), "%c%c ", kHex[ pat >> 4 ], kHex[ pat & 15 ] );
		text( value );
		label( "BPM " );
		std::snprintf( value, sizeof( value ), "%3d%c ", bpm, ts.tempoSource == 1 ? ( tracker.TempoSettled() ? '*' : '?' ) : ' ' );
		text( value );
		label( "SPD " );
		std::snprintf( value, sizeof( value ), "%02d", std::min( ts.speed, 99 ) );
		text( value );
	}

	// -- the VU bars -----------------------------------------------------------
	if( display.showVu )
	{
		const int top = Screen::kHeaderRows * Screen::kCellH;
		const int bottom = top + Screen::kVuRows * Screen::kCellH;// 48 below the top
		const int barW   = 4 * Screen::kCellW;
		for( int k = 0; k < channels; ++k )
		{
			VuBar bar;
			const int colPx = Screen::ChannelColumn( k, display.showEffects ) * Screen::kCellW;
			bar.x     = colPx + ( colw * Screen::kCellW - barW ) / 2;
			bar.w     = barW;
			bar.level = std::min( tracker.Vu( k ), kVuSteps );
			bar.h     = bar.level;
			bar.y     = bottom - bar.level;
			s.bars.push_back( bar );
		}
	}

	// -- the pattern -----------------------------------------------------------
	const int firstRow = Screen::kHeaderRows + ( display.showVu ? Screen::kVuRows : 0 );
	const int centre   = visible / 2;
	for( int v = 0; v < visible; ++v )
	{
		const int  screenRow = firstRow + v;
		int        row       = tracker.Row() + ( v - centre );
		row                  = ( ( row % ts.rows ) + ts.rows ) % ts.rows;
		const bool cursor    = v == centre;
		const uint8_t bg     = cursor ? PAL_CURSOR_BACK : PAL_BACK;
		const uint8_t text   = cursor ? PAL_CURSOR_TEXT : PAL_TEXT;
		const uint8_t dim    = cursor ? PAL_CURSOR_TEXT : PAL_DIM;
		const uint8_t beat   = cursor ? PAL_CURSOR_TEXT : ( row % 4 == 0 ? PAL_ACCENT : PAL_TEXT );

		// Every cell on the row takes the row's background first.
		for( int c = 0; c < s.cols; ++c )
			w.put( c, screenRow, ' ', text, bg );

		w.put( 0, screenRow, kHex[ row >> 4 ], beat, bg );
		w.put( 1, screenRow, kHex[ row & 15 ], beat, bg );

		for( int k = 0; k < channels; ++k )
		{
			const int   col  = Screen::ChannelColumn( k, display.showEffects );
			const Cell& cell = tracker.At( row, k );
			w.put( col - 1, screenRow, '|', PAL_FRAME, bg );

			if( cell.written )
			{
				w.text( col, screenRow, bands::NoteName( cell.note ), text, bg );
				w.put( col + 4, screenRow, kHex[ cell.sample >> 4 ], cursor ? text : PAL_ACCENT, bg );
				w.put( col + 5, screenRow, kHex[ cell.sample & 15 ], cursor ? text : PAL_ACCENT, bg );
				if( display.showEffects )
				{
					w.put( col + 7, screenRow, 'C', text, bg );
					w.put( col + 8, screenRow, kHex[ cell.volume >> 4 ], text, bg );
					w.put( col + 9, screenRow, kHex[ cell.volume & 15 ], text, bg );
				}
			}
			else
			{
				w.text( col, screenRow, "--- ..", dim, bg );
				if( display.showEffects )
					w.text( col + 7, screenRow, "...", dim, bg );
			}
		}
		w.put( s.cols - 1, screenRow, '|', PAL_FRAME, bg );
	}

	return s;
}

Layout LayoutFor( const Screen& screen, int width, int height, int requested )
{
	Layout l;
	l.screenW = screen.PixelWidth();
	l.screenH = screen.PixelHeight();
	if( requested > 0 )
		l.scale = requested;
	else
		l.scale = std::max( 1, std::min( l.screenW > 0 ? width / l.screenW : 1, l.screenH > 0 ? height / l.screenH : 1 ) );
	// Integer division towards zero on a possibly negative number: a screen
	// wider than the output gets a negative origin, and (a - b) / 2 in C++
	// truncates, which keeps it whole either way.
	l.originX = ( width - l.screenW * l.scale ) / 2;
	l.originY = ( height - l.screenH * l.scale ) / 2;
	return l;
}

} // namespace pattern
