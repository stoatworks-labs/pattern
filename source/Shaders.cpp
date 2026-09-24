#include "Shaders.h"

namespace pattern
{
const char* const kVertexShader = R"(#version 410 core
// One triangle that covers the viewport. No vertex buffer: the corners come
// from gl_VertexID, so the only thing bound at draw time is the VAO.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
)";

// The fragment shader, in two adjacent raw strings -- see Shaders.h.
const char* const kFragmentShader =
R"(#version 410 core
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
)"
R"(
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
)";

} // namespace pattern
