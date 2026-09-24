#pragma once

/**
	The GLSL, as string literals.

	`#version 410 core` because that is macOS's ceiling and the rest of the
	fleet already sits there.

	## It computes no layout

	The CPU composes a character screen (Screen.h) and decides an integer
	origin and an integer scale. The fragment shader turns a pixel into a
	cell with integer arithmetic -- subtract the origin, divide by the scale,
	divide by the cell size -- fetches the cell from an integer texture, the
	glyph bit from the font texture, and picks a palette entry. Nothing in it
	can put a colour boundary anywhere but on the scaled cell grid, which is
	what `pntest --grid` measures out of the picture.

	`uFrac` is the negative control for that check: when it is non-zero the
	pixel-to-cell step is done in float at a fractional scale, and the grid
	check must fail. It is 0.0 on every frame the plugin renders.

	## Reserved words

	`patch`, `sample`, `input`, `output`, `filter`, `common`, `active`, `half`,
	`layout` and `flat` are GLSL keywords. The locals here are `cellIndex`,
	`inCell`, `bar`.

	## Two adjacent literals

	MSVC caps one string literal at about 16 KB (C2026). This shader is well
	under it, but it is kept as two raw strings anyway so `tools/verify.sh`'s
	extractor, which joins adjacent literals, is exercised rather than
	assumed to work.
*/
namespace pattern
{
extern const char* const kVertexShader;
extern const char* const kFragmentShader;

} // namespace pattern
