#pragma once

#include <string>

#include "Screen.h"

#include <FFGLSDK.h>

/**
	The draw: one program, one triangle, two textures, into the host's FBO.

	The screen texture is re-uploaded every frame (a few kilobytes: at most
	92 x 41 cells) with `glTexSubImage2D`, reallocated only when the cell
	grid changes shape. The font texture is uploaded once. Every `ffglex::
	Scoped*` binding clears to 0 on exit rather than restoring, so nothing
	here uses them; what this touches is put back by hand at the end of
	`Draw`.

	A source owns the layer's buffer, so the picture is written, not blended:
	the theme's background is opaque and covers the whole output.
*/
namespace pattern
{

struct Palette
{
	float rgb[ PAL_COUNT ][ 3 ];
};

/// The three themes, by index (Controls.h `Theme`).
const Palette& PaletteFor( int theme );

class Renderer
{
public:
	Renderer() = default;

	bool InitGL();
	void DeInitGL();
	bool Ready() const { return mReady; }

	/// `frac` is 0.0 in the plugin; the harness's --grid negative control
	/// passes a fractional scale through it.
	void Draw( const Screen& screen, const Layout& layout, const Palette& palette, int width, int height,
			   GLuint hostFBO, float frac = 0.0f );

	const std::string& Note() const { return mNote; }

private:
	ffglex::FFGLShader mProgram;
	GLuint             mVao    = 0;
	GLuint             mFont   = 0;
	GLuint             mScreen = 0;
	int                mScreenCols = 0, mScreenRows = 0;

	bool        mReady = false;
	std::string mNote;
};

} // namespace pattern
