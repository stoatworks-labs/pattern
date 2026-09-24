#pragma once

#include <cstdint>
#include <vector>

#include "tracker/Tracker.h"

/**
	The picture as a character screen: a grid of cells, each a glyph and two
	palette indices, plus the VU bars as rectangles on the same pixel grid.

	Composed on the CPU from the tracker's published state, and this is the
	whole layout: the shader is handed the grid, an integer origin and an
	integer scale and computes nothing but which cell a pixel is in. That is
	what makes `pntest --grid` a measurement rather than a transcription: at
	Scale s every s x s block of the output aligned to the origin must be one
	colour, because nothing in the shader can produce anything else.

	## Cells

	A cell is 6 x 8 pixels at Scale 1: graticule's 5 x 7 glyph with a one-pixel
	gutter right and below. The layout in cells, top to bottom:

	    row 0    POS pp PAT pp BPM bbb SPD ss       the counters
	    row 1    blank
	    6 rows   the VU bars, if Show VU (48 pixels: one per meter step)
	    N rows   the pattern, Rows Visible of them, the cursor row at the
	             centre (index N / 2), rows wrapping past the pattern end

	and left to right on a pattern row:

	    rr |C-2 01 C40|--- .. ...|...|      Show Effects on
	    rr |C-2 01|--- ..|...|              Show Effects off

	Palette indices are fixed meanings; the theme decides the colours.
*/
namespace pattern
{

enum PaletteIndex : uint8_t
{
	PAL_BACK = 0,
	PAL_TEXT,
	PAL_DIM,
	PAL_CURSOR_BACK,
	PAL_CURSOR_TEXT,
	PAL_ACCENT,
	PAL_FRAME,
	PAL_VU_LOW,
	PAL_VU_MID,
	PAL_VU_HIGH,
	PAL_COUNT
};

struct ScreenCell
{
	uint8_t glyph = 32;
	uint8_t fg    = PAL_TEXT;
	uint8_t bg    = PAL_BACK;
	uint8_t pad   = 0;
};

/// One VU bar, in Scale-1 pixels within the screen. `level` is 0..kVuSteps
/// and `h` equals it: one pixel a step.
struct VuBar
{
	int x = 0, y = 0, w = 0, h = 0;
	int level = 0;
};

struct DisplaySettings
{
	int  rowsVisible = 17;
	bool showVu      = true;
	bool showEffects = true;
};

struct Screen
{
	int                       cols = 0, rows = 0;
	std::vector< ScreenCell > cells;
	std::vector< VuBar >      bars;

	static constexpr int kCellW = 6, kCellH = 8;
	static constexpr int kHeaderRows = 2;
	static constexpr int kVuRows     = 6;///< 48 pixels

	int PixelWidth() const { return cols * kCellW; }
	int PixelHeight() const { return rows * kCellH; }

	/// The screen row the cursor sits on.
	int CursorRow( const DisplaySettings& d ) const;
	/// The first cell column of channel k's text.
	static int ChannelColumn( int k, bool showEffects );
	static int ChannelWidth( bool showEffects ) { return showEffects ? 10 : 6; }

	const ScreenCell& At( int col, int row ) const
	{
		return cells[ static_cast< size_t >( row ) * static_cast< size_t >( cols ) + static_cast< size_t >( col ) ];
	}
};

Screen Compose( const Tracker& tracker, const DisplaySettings& display );

/// Where the screen sits on an output of `width` x `height`: an integer
/// scale (the largest that fits when `requested` is 0, else `requested`) and
/// an integer origin that centres it. A screen that does not fit at scale 1
/// is centred and cropped.
struct Layout
{
	int scale = 1;
	int originX = 0, originY = 0;
	int screenW = 0, screenH = 0;///< Scale-1 pixels
};
Layout LayoutFor( const Screen& screen, int width, int height, int requested );

} // namespace pattern
