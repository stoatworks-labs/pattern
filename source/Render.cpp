#include "Render.h"

#include <algorithm>
#include <vector>

#include "Font.h"
#include "Shaders.h"

namespace pattern
{
namespace
{
// Colours named by colour. Each is chosen so that, at 8 bits, it lands on a
// whole code value with room to spare (multiples of 1/15 or 1/3), which is
// what lets the harness assert an exact palette entry on a flat cell.
constexpr Palette kPalettes[ 3 ] = {
	// Grey: a light grey desktop, black text, a blue cursor bar.
	{ {
		{ 0.6666667f, 0.6666667f, 0.6666667f },// back
		{ 0.0f, 0.0f, 0.0f },                  // text
		{ 0.3333333f, 0.3333333f, 0.3333333f },// dim
		{ 0.0f, 0.3333333f, 0.6666667f },      // cursor back
		{ 1.0f, 1.0f, 1.0f },                  // cursor text
		{ 0.0f, 0.2f, 0.6f },                  // accent
		{ 0.4f, 0.4f, 0.4f },                  // frame
		{ 0.0f, 0.6f, 0.0f },                  // vu low
		{ 0.8f, 0.6666667f, 0.0f },            // vu mid
		{ 0.8f, 0.1333333f, 0.0f },            // vu high
	} },
	// Blue: a deep blue, pale text, a brighter blue cursor bar.
	{ {
		{ 0.0f, 0.0666667f, 0.2666667f },
		{ 0.7333333f, 0.8f, 1.0f },
		{ 0.2666667f, 0.3333333f, 0.5333333f },
		{ 0.1333333f, 0.3333333f, 0.8f },
		{ 1.0f, 1.0f, 1.0f },
		{ 0.5333333f, 0.8f, 1.0f },
		{ 0.2f, 0.2666667f, 0.5333333f },
		{ 0.2666667f, 0.7333333f, 1.0f },
		{ 0.5333333f, 0.8666667f, 1.0f },
		{ 1.0f, 1.0f, 1.0f },
	} },
	// Green: black, phosphor green, a dim green cursor bar.
	{ {
		{ 0.0f, 0.0f, 0.0f },
		{ 0.2666667f, 1.0f, 0.2666667f },
		{ 0.0666667f, 0.4f, 0.0666667f },
		{ 0.0666667f, 0.2666667f, 0.0666667f },
		{ 0.8f, 1.0f, 0.8f },
		{ 0.6666667f, 1.0f, 0.6666667f },
		{ 0.1333333f, 0.4f, 0.1333333f },
		{ 0.1333333f, 0.8f, 0.1333333f },
		{ 0.5333333f, 1.0f, 0.2666667f },
		{ 1.0f, 1.0f, 0.4f },
	} },
};

void SetI( GLuint program, const char* name, int v )
{
	glUniform1i( glGetUniformLocation( program, name ), v );
}
void SetF( GLuint program, const char* name, float v )
{
	glUniform1f( glGetUniformLocation( program, name ), v );
}
void SetI2( GLuint program, const char* name, int a, int b )
{
	glUniform2i( glGetUniformLocation( program, name ), a, b );
}
} // namespace

const Palette& PaletteFor( int theme )
{
	return kPalettes[ std::min( std::max( theme, 0 ), 2 ) ];
}

bool Renderer::InitGL()
{
	if( mReady )
		return true;
	mNote.clear();

	if( !mProgram.Compile( kVertexShader, kFragmentShader ) )
	{
		mNote = "the screen shader would not compile";
		return false;
	}

	glGenVertexArrays( 1, &mVao );

	// The queue is drained first: a plugin that reads a GL error the HOST
	// left behind as its own failure abandons a perfectly good upload.
	while( glGetError() != GL_NO_ERROR )
	{
	}

	const std::vector< uint8_t > pixels = font::Texture();
	glGenTextures( 1, &mFont );
	glBindTexture( GL_TEXTURE_2D, mFont );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, font::kTextureWidth, font::kTextureHeight, 0, GL_RED, GL_UNSIGNED_BYTE,
				  pixels.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenTextures( 1, &mScreen );
	glBindTexture( GL_TEXTURE_2D, mScreen );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	mScreenCols = mScreenRows = 0;

	const GLenum err = glGetError();
	if( err != GL_NO_ERROR )
	{
		mNote = "the font texture would not upload (GL error 0x" + std::to_string( static_cast< int >( err ) ) + ")";
		DeInitGL();
		return false;
	}

	mReady = true;
	return true;
}

void Renderer::DeInitGL()
{
	if( mFont != 0 )
	{
		glDeleteTextures( 1, &mFont );
		mFont = 0;
	}
	if( mScreen != 0 )
	{
		glDeleteTextures( 1, &mScreen );
		mScreen = 0;
	}
	if( mVao != 0 )
	{
		glDeleteVertexArrays( 1, &mVao );
		mVao = 0;
	}
	mProgram.FreeGLResources();
	mReady = false;
}

void Renderer::Draw( const Screen& s, const Layout& l, const Palette& palette, int width, int height, GLuint hostFBO,
					 float frac )
{
	if( !mReady || width <= 0 || height <= 0 || s.cols <= 0 || s.rows <= 0 )
		return;

	// The screen, as an 8-bit integer texture: exact fetches, no filtering
	// question to ask.
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, mScreen );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	if( s.cols != mScreenCols || s.rows != mScreenRows )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8UI, s.cols, s.rows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
					  s.cells.data() );
		mScreenCols = s.cols;
		mScreenRows = s.rows;
	}
	else
	{
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, s.cols, s.rows, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, s.cells.data() );
	}

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( 0, 0, width, height );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );

	const GLuint prog = mProgram.GetGLID();
	glUseProgram( prog );

	SetI2( prog, "uSize", width, height );
	SetI2( prog, "uOrigin", l.originX, l.originY );
	SetI( prog, "uScale", std::max( 1, l.scale ) );
	SetF( prog, "uFrac", frac );
	SetI2( prog, "uCells", s.cols, s.rows );
	glUniform3fv( glGetUniformLocation( prog, "uPal" ), PAL_COUNT, &palette.rgb[ 0 ][ 0 ] );

	{
		GLint     bars[ 8 * 4 ] = {};
		const int n             = std::min< int >( static_cast< int >( s.bars.size() ), 8 );
		for( int b = 0; b < n; ++b )
		{
			bars[ b * 4 + 0 ] = s.bars[ static_cast< size_t >( b ) ].x;
			bars[ b * 4 + 1 ] = s.bars[ static_cast< size_t >( b ) ].y;
			bars[ b * 4 + 2 ] = s.bars[ static_cast< size_t >( b ) ].w;
			bars[ b * 4 + 3 ] = s.bars[ static_cast< size_t >( b ) ].h;
		}
		SetI( prog, "uBarCount", n );
		glUniform4iv( glGetUniformLocation( prog, "uBar" ), 8, bars );
	}

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mFont );
	SetI( prog, "uFont", 0 );
	SetI( prog, "uScreen", 1 );

	glBindVertexArray( mVao );
	glDrawArrays( GL_TRIANGLES, 0, 3 );

	// ---- hand the context back ---------------------------------------------
	glBindVertexArray( 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
}

} // namespace pattern
