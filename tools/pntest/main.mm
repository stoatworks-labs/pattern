/**
	pntest -- the offline harness.

	It drives **the real code that ships**: `PatternPlugin` through its
	parameter list, its `SetTime`, `SetBeatInfo`, the Audio buffer parameter
	and its `Advance`, which is exactly what `ProcessOpenGL` calls. Nothing
	below is a re-transcription of the tracker.

	    --timing    the cursor is on row floor( t / T ) mod Rows at every frame
	                of a ten-minute run, from a host clock at 499 million ms
	    --onset     a burst in band k writes one note, in channel k, on the row
	                current at that frame, and nowhere else
	    --prime     loud audio already playing when the clip starts writes no
	                false note in the first second
	    --pitch     a peak at bin j becomes the note the bin law predicts, under
	                both laws, folded and unfolded
	    --ring      the next pass overwrites; a row with no onset is cleared,
	                or kept with Keep Notes on
	    --detected  a metronome at 120 (and 100, 150) BPM is detected within
	                +-1 BPM inside six seconds
	    --groove    drum grooves rendered as AUDIO through the FFT -- backbeat,
	                kick-heavy, syncopated kick, syncopated pluck across 80..170
	                BPM, the release video's house groove at 115..135 -- are
	                detected within +-1 BPM at the right metrical level inside
	                six seconds; v0.1.0's detector must fail the same set
	    --tempo-wav f.wav --truth N [--from S] [--fps N] [--legacy]
	                the detected tempo of a real file, once a second
	    --names     no parameter name over FFGL's 16 characters, none duplicated
	    --font      the glyph table has every character the screen draws
	    --list      the fleet's parameter listing, for tools/sweep.py
	    --grid      every glyph lands on the whole-pixel grid at every Scale,
	                at 640x360 and 320x180 (the one check that reads pixels)
	    --bench     ms/frame at 720p, 1080p and 4K

	    --out f.png [--size WxH] [--frames N] [--fps N] [--audio PRESET]
	                [--audio-bpm N] [--host-bpm N] [--set "Name=value" ...]
	    --pipe      raw RGBA frames on stdout, for the video pipeline

	Every physics check carries its own negative control: the model perturbed
	in one named way (`TrackerDebug`, or the plugin's `debugFractionalScale`),
	and the check asserted to FAIL against it. A check that cannot fail is not
	one.

	## Where the measuring happens

	The clock, the ring, the onsets, the pitches and the detected tempo are
	claims about `source/tracker/`, so they are measured on the tracker's
	published state with no rasteriser in the room: `--timing`, `--onset`,
	`--prime`, `--pitch`, `--ring`, `--detected`, `--groove`, `--names` and
	`--font` open no GL context at all and give the same answer on a GPU-less runner by
	construction. `--grid` is the one check that reads pixels, and what it
	asserts is a property no rasteriser can bend: at Scale s every s x s block
	of the output aligned to the plugin's own origin is one colour, the blocks
	outside the screen are the theme's background, and a cell's block is the
	font table's bits at that scale, exactly.

	## The synthetic input

	There is no host, so the spectrum is written straight into the buffer
	parameter with `SetParamElementValue`, the same call a host makes, and
	the tempo with `SetBeatInfo`. `Synth` makes deterministic spectra: a
	burst, a tone, a metronome, a drum pattern, stationary noise, silence.
	`--wav` takes a real audio file through a 2048-point FFT folded into 64
	linear bins, and `--spectrum` takes macroblock's one-line-per-frame file.
	All three arrive through the same buffer parameter.

	## --pipe

	The fleet's frame format: raw RGBA, top row first, on stdout, until
	`--frames` or until the reader hangs up. `--script` is `frame  Parameter
	Name  value` (or `frame  Name=value`) lines, held before the first key and
	after the last and linear between. Two pseudo-parameters are accepted in
	a script and nowhere else: `Host BPM` (delivered through SetBeatInfo) and
	`Bin N` for N in 0..63 (written into the Audio buffer, over whatever
	`--audio`/`--wav`/`--spectrum` put there). Time is the frame counter:
	frame n is `SetTime( n / fps )`.

	    pntest --pipe --size 1920x1080 --fps 60 --wav track.wav --host-bpm 124 \
	      | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i - out.mov

	SIGPIPE is ignored, so a reader that hangs up makes the write fail and the
	take end with a message on stderr and **exit status 1**, never a silent
	141. That includes `--frames 0` (run until the reader hangs up): the only
	way that mode ends is the hang-up, so it always exits 1; use `--frames N`
	for a take that can end cleanly.
*/
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "Font.h"
#include "Pattern.h"
#include "tracker/Bands.h"

using namespace pattern;

static int failures = 0;
static void Check( bool ok, const std::string& what )
{
	std::printf( "  %s %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

static std::string F( double v, int places = 3 )
{
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.*f", places, v );
	return buffer;
}

namespace
{
constexpr int kBins = bands::kBins;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS. Carried from graticule via needle.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf                       compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	std::vector< unsigned char > header;
	putU32( header, static_cast< uint32_t >( width ) );
	putU32( header, static_cast< uint32_t >( height ) );
	header.push_back( 8 );
	header.push_back( 6 );
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );
	putChunk( png, "IHDR", header );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	std::FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	// PNTEST_RENDERER=software asks for Apple's software renderer outright,
	// which is what a GPU-less CI runner falls back to.
	const char* renderer = std::getenv( "PNTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		CGLPixelFormatAttribute sw[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										 kCGLPFARendererID, (CGLPixelFormatAttribute)kCGLRendererGenericFloatID,
										 kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
		CGLPixelFormatObj       pix  = nullptr;
		GLint                   npix = 0;
		CGLContextObj           ctx  = nullptr;
		if( CGLChoosePixelFormat( sw, &pix, &npix ) != kCGLNoError || pix == nullptr ||
			CGLCreateContext( pix, nullptr, &ctx ) != kCGLNoError )
			return nullptr;
		CGLSetCurrentContext( ctx );
		std::printf( "renderer: %s\n", reinterpret_cast< const char* >( glGetString( GL_RENDERER ) ) );
		return ctx;
	}
	CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
										(CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj       pix  = nullptr;
	GLint                   npix = 0;
	if( CGLChoosePixelFormat( attrs, &pix, &npix ) != kCGLNoError || pix == nullptr )
	{
		// No accelerated context (a CI runner): take whatever there is.
		CGLPixelFormatAttribute soft[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
										   kCGLPFAColorSize, (CGLPixelFormatAttribute)24, (CGLPixelFormatAttribute)0 };
		if( CGLChoosePixelFormat( soft, &pix, &npix ) != kCGLNoError || pix == nullptr )
			return nullptr;
	}
	CGLContextObj ctx = nullptr;
	if( CGLCreateContext( pix, nullptr, &ctx ) != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( ctx );
	return ctx;
}

bool openGL()
{
	static CGLContextObj ctx = nullptr;
	if( ctx == nullptr )
		ctx = createContext();
	if( ctx == nullptr )
		std::printf( "no GL context\n" );
	return ctx != nullptr;
}

struct Target
{
	GLuint fbo = 0, colour = 0;
	int    w = 0, h = 0;

	Target( int width, int height ) : w( width ), h( height )
	{
		glGenTextures( 1, &colour );
		glBindTexture( GL_TEXTURE_2D, colour );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	}
	~Target()
	{
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &colour );
	}
};

/// Top-down RGBA, so (x, y) indexes the way the screen is laid out.
struct Image
{
	int                          w = 0, h = 0;
	std::vector< unsigned char > px;

	const unsigned char* at( int x, int y ) const
	{
		static const unsigned char zero[ 4 ] = { 0, 0, 0, 0 };
		if( x < 0 || y < 0 || x >= w || y >= h )
			return zero;
		return px.data() + ( static_cast< size_t >( y ) * w + x ) * 4;
	}
	bool is( int x, int y, const float* rgb, int tol = 1 ) const
	{
		const unsigned char* p = at( x, y );
		for( int c = 0; c < 3; ++c )
			if( std::abs( p[ c ] - static_cast< int >( std::lround( rgb[ c ] * 255.0f ) ) ) > tol )
				return false;
		return true;
	}
	bool same( int x, int y, int x2, int y2 ) const { return std::memcmp( at( x, y ), at( x2, y2 ), 4 ) == 0; }
	std::string str( int x, int y ) const
	{
		const unsigned char* p = at( x, y );
		return "(" + std::to_string( p[ 0 ] ) + "," + std::to_string( p[ 1 ] ) + "," + std::to_string( p[ 2 ] ) +
			   "," + std::to_string( p[ 3 ] ) + ")";
	}
};

Image readBack( const Target& t )
{
	std::vector< unsigned char > raw( static_cast< size_t >( t.w ) * t.h * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, t.fbo );
	glReadPixels( 0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	Image img;
	img.w = t.w;
	img.h = t.h;
	img.px.resize( raw.size() );
	for( int y = 0; y < t.h; ++y )
		std::memcpy( img.px.data() + static_cast< size_t >( y ) * t.w * 4,
					 raw.data() + static_cast< size_t >( t.h - 1 - y ) * t.w * 4, static_cast< size_t >( t.w ) * 4 );
	return img;
}

//---------------------------------------------------------------------------
// Synthetic spectra. Deterministic: the jitter is an integer hash of (frame,
// bin), so two runs give the same bins and two machines the same picture.
//---------------------------------------------------------------------------
uint32_t hashU( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
/// -1..1
double jitter( int frame, int bin, int salt = 0 )
{
	const uint32_t h = hashU( static_cast< uint32_t >( frame ) * 73856093u ^ static_cast< uint32_t >( bin ) * 19349663u ^
							  static_cast< uint32_t >( salt ) * 83492791u );
	return static_cast< double >( h & 0xffffffu ) / static_cast< double >( 0xffffffu ) * 2.0 - 1.0;
}

struct Synth
{
	enum Kind
	{
		Silence,
		Noise,    ///< stationary: 0.3 in every bin, +-5 % per frame
		Tone,     ///< a steady peak at `bin`
		Metronome,///< one-frame bursts in bins 4..6 on every beat
		Beats,    ///< a drum pattern at `bpm`: kick, snare, hats, a bass note
		Sweep     ///< a tone rising one bin every half second
	};
	Kind   kind = Silence;
	double bpm  = 120.0;
	int    bin  = 8;
	double fps  = 60.0;

	static bool parse( const std::string& s, Synth& out )
	{
		if( s == "silence" )
			out.kind = Silence;
		else if( s == "noise" )
			out.kind = Noise;
		else if( s == "metronome" )
			out.kind = Metronome;
		else if( s == "beats" )
			out.kind = Beats;
		else if( s == "sweep" )
			out.kind = Sweep;
		else if( s.rfind( "tone", 0 ) == 0 )
		{
			out.kind = Tone;
			if( s.size() > 5 && s[ 4 ] == ':' )
				out.bin = std::atoi( s.c_str() + 5 );
		}
		else
			return false;
		return true;
	}

	/// Whether frame `frame` is the first frame of a new beat subdivision:
	/// `per` subdivisions a beat.
	bool onsetAt( int frame, int per ) const
	{
		const double t0 = ( frame - 1 ) / fps, t1 = frame / fps;
		const double rate = bpm / 60.0 * per;
		return frame == 0 || std::floor( t1 * rate ) != std::floor( t0 * rate );
	}
	int beatIndex( int frame ) const { return static_cast< int >( std::floor( frame / fps * bpm / 60.0 ) ); }

	void bins( int frame, float* out ) const
	{
		for( int j = 0; j < kBins; ++j )
			out[ j ] = 0.0f;
		switch( kind )
		{
		case Silence:
			break;
		case Noise:
			for( int j = 0; j < kBins; ++j )
				out[ j ] = static_cast< float >( 0.3 * ( 1.0 + 0.05 * jitter( frame, j ) ) );
			break;
		case Tone:
			for( int j = 0; j < kBins; ++j )
				out[ j ] = static_cast< float >( 0.005 * ( 1.0 + 0.5 * jitter( frame, j ) ) );
			if( bin >= 0 && bin < kBins )
				out[ bin ] = 0.8f;
			if( bin - 1 >= 0 )
				out[ bin - 1 ] = 0.2f;
			if( bin + 1 < kBins )
				out[ bin + 1 ] = 0.2f;
			break;
		case Metronome:
			for( int j = 0; j < kBins; ++j )
				out[ j ] = static_cast< float >( 0.02 * ( 1.0 + 0.5 * jitter( frame, j ) ) );
			if( onsetAt( frame, 1 ) )
				for( int j = 4; j <= 6; ++j )
					out[ j ] = 1.0f;
			break;
		case Beats:
		{
			for( int j = 0; j < kBins; ++j )
				out[ j ] = static_cast< float >( 0.01 * ( 1.0 + 0.5 * jitter( frame, j ) ) );
			const int beat = beatIndex( frame );
			// Kick on every beat, two frames long, with a decaying tail.
			if( onsetAt( frame, 1 ) || onsetAt( frame - 1, 1 ) )
				for( int j = 0; j <= 1; ++j )
					out[ j ] = onsetAt( frame, 1 ) ? 1.0f : 0.6f;
			// Snare on beats 2 and 4.
			if( ( beat % 4 == 1 || beat % 4 == 3 ) && ( onsetAt( frame, 1 ) || onsetAt( frame - 1, 1 ) ) )
				for( int j = 8; j <= 12; ++j )
					out[ j ] = onsetAt( frame, 1 ) ? 0.7f : 0.4f;
			// Hats on every eighth.
			if( onsetAt( frame, 2 ) )
				for( int j = 40; j <= 56; ++j )
					out[ j ] = 0.25f;
			// A bass note that changes pitch every bar: bins 2..5 in turn.
			const int bassBin = 2 + ( beat / 4 ) % 4;
			if( onsetAt( frame, 1 ) && beat % 2 == 0 )
				out[ bassBin ] = 0.9f;
			else
				out[ bassBin ] = std::max( out[ bassBin ], 0.3f );
			break;
		}
		case Sweep:
		{
			for( int j = 0; j < kBins; ++j )
				out[ j ] = static_cast< float >( 0.005 * ( 1.0 + 0.5 * jitter( frame, j ) ) );
			const int b = static_cast< int >( std::floor( frame / fps * 2.0 ) ) % kBins;
			out[ b ]    = 0.8f;
			break;
		}
		}
	}
};

//---------------------------------------------------------------------------
// A WAV reader and a 2048-point FFT, folded into 64 linear bins -- so the
// video pipeline can drive the plugin from music through the real audio
// input path.
//---------------------------------------------------------------------------
struct Wav
{
	double               rate = 0.0;
	std::vector< float > mono;

	static uint32_t u32( const unsigned char* p ) { return p[ 0 ] | p[ 1 ] << 8 | p[ 2 ] << 16 | static_cast< uint32_t >( p[ 3 ] ) << 24; }
	static uint16_t u16( const unsigned char* p ) { return static_cast< uint16_t >( p[ 0 ] | p[ 1 ] << 8 ); }

	bool load( const std::string& path, std::string& error )
	{
		std::ifstream file( path, std::ios::binary );
		if( !file )
		{
			error = "cannot open " + path;
			return false;
		}
		std::vector< unsigned char > bytes( ( std::istreambuf_iterator< char >( file ) ), std::istreambuf_iterator< char >() );
		if( bytes.size() < 12 || std::memcmp( bytes.data(), "RIFF", 4 ) != 0 || std::memcmp( bytes.data() + 8, "WAVE", 4 ) != 0 )
		{
			error = path + ": not a RIFF WAVE file";
			return false;
		}
		int      format = 0, channels = 0, bits = 0;
		size_t   dataAt = 0, dataLen = 0;
		size_t   at     = 12;
		while( at + 8 <= bytes.size() )
		{
			const uint32_t len = u32( bytes.data() + at + 4 );
			if( std::memcmp( bytes.data() + at, "fmt ", 4 ) == 0 && len >= 16 )
			{
				format   = u16( bytes.data() + at + 8 );
				channels = u16( bytes.data() + at + 10 );
				rate     = u32( bytes.data() + at + 12 );
				bits     = u16( bytes.data() + at + 22 );
				if( format == 0xFFFE && len >= 40 )
					format = u16( bytes.data() + at + 32 );// WAVE_FORMAT_EXTENSIBLE: the sub-format's first word
			}
			else if( std::memcmp( bytes.data() + at, "data", 4 ) == 0 )
			{
				dataAt  = at + 8;
				dataLen = std::min< size_t >( len, bytes.size() - dataAt );
			}
			at += 8 + len + ( len & 1 );
		}
		if( channels <= 0 || rate <= 0.0 || dataLen == 0 )
		{
			error = path + ": no fmt or data chunk";
			return false;
		}
		const bool   isFloat = format == 3;
		const int    bytesPer = bits / 8;
		if( !( ( isFloat && bits == 32 ) || ( format == 1 && ( bits == 8 || bits == 16 || bits == 24 || bits == 32 ) ) ) )
		{
			error = path + ": unsupported format " + std::to_string( format ) + " at " + std::to_string( bits ) + " bits";
			return false;
		}
		const size_t frames = dataLen / static_cast< size_t >( bytesPer * channels );
		mono.resize( frames );
		const unsigned char* p = bytes.data() + dataAt;
		for( size_t i = 0; i < frames; ++i )
		{
			double sum = 0.0;
			for( int c = 0; c < channels; ++c, p += bytesPer )
			{
				double v = 0.0;
				if( isFloat )
				{
					float f;
					std::memcpy( &f, p, 4 );
					v = f;
				}
				else if( bits == 8 )
					v = ( static_cast< int >( p[ 0 ] ) - 128 ) / 128.0;
				else if( bits == 16 )
					v = static_cast< int16_t >( u16( p ) ) / 32768.0;
				else if( bits == 24 )
					v = ( static_cast< int32_t >( ( p[ 0 ] << 8 | p[ 1 ] << 16 | static_cast< uint32_t >( p[ 2 ] ) << 24 ) ) >> 8 ) / 8388608.0;
				else
					v = static_cast< int32_t >( u32( p ) ) / 2147483648.0;
				sum += v;
			}
			mono[ i ] = static_cast< float >( sum / channels );
		}
		return true;
	}

	/// The 64-bin spectrum of the block of 2048 samples ending at `seconds`:
	/// Hann window, radix-2 FFT, 1024 bins summed in power sixteen at a time,
	/// magnitude normalised so a full-scale sine reads about 1.0 in its bin.
	void bins( double seconds, float* out ) const
	{
		constexpr int N = 2048;
		static std::vector< std::complex< double > > x( N );
		const long end = static_cast< long >( std::llround( seconds * rate ) );
		for( int i = 0; i < N; ++i )
		{
			const long  s = end - N + i;
			const double v = ( s >= 0 && s < static_cast< long >( mono.size() ) ) ? mono[ static_cast< size_t >( s ) ] : 0.0;
			const double w = 0.5 - 0.5 * std::cos( 2.0 * M_PI * i / N );
			x[ i ]         = v * w;
		}
		// In-place iterative radix-2.
		for( int i = 1, j = 0; i < N; ++i )
		{
			int bit = N >> 1;
			for( ; j & bit; bit >>= 1 )
				j ^= bit;
			j ^= bit;
			if( i < j )
				std::swap( x[ i ], x[ j ] );
		}
		for( int len = 2; len <= N; len <<= 1 )
		{
			const std::complex< double > wl( std::cos( -2.0 * M_PI / len ), std::sin( -2.0 * M_PI / len ) );
			for( int i = 0; i < N; i += len )
			{
				std::complex< double > w( 1.0, 0.0 );
				for( int k = 0; k < len / 2; ++k )
				{
					const std::complex< double > u = x[ i + k ], v = x[ i + k + len / 2 ] * w;
					x[ i + k ]           = u + v;
					x[ i + k + len / 2 ] = u - v;
					w *= wl;
				}
			}
		}
		const double norm = N / 4.0;
		for( int b = 0; b < kBins; ++b )
		{
			double power = 0.0;
			for( int k = b * 16; k < ( b + 1 ) * 16; ++k )
				power += std::norm( x[ k ] );
			out[ b ] = static_cast< float >( std::sqrt( power ) / norm );
		}
	}
};

/// One row of kBins numbers per line; `#` starts a comment. A row with the
/// wrong count is refused, because a short row silently padded with zeros
/// would read as a quieter signal than the one analysed.
std::vector< std::vector< float > > loadSpectrum( const std::string& path, std::string& error )
{
	std::vector< std::vector< float > > rows;
	std::ifstream                       file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return rows;
	}
	std::string line;
	int         lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream   in( line );
		std::vector< float > row;
		float                v = 0.0f;
		while( in >> v )
			row.push_back( v );
		if( row.empty() )
			continue;
		if( static_cast< int >( row.size() ) != kBins )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": " + std::to_string( row.size() ) + " bins, expected " +
					std::to_string( kBins );
			return {};
		}
		rows.push_back( std::move( row ) );
	}
	return rows;
}

//---------------------------------------------------------------------------
// The plugin, driven the way a host drives it.
//---------------------------------------------------------------------------
unsigned int paramByName( PatternPlugin& p, const std::string& name )
{
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* n = p.GetParamName( id );
		if( n != nullptr && name == n )
			return id;
	}
	return PT_COUNT_;
}

void inject( PatternPlugin& p, const float* bins )
{
	for( int i = 0; i < kBins; ++i )
		p.SetParamElementValue( PT_AUDIO, static_cast< unsigned >( i ), bins[ i ] );
}

void injectSilence( PatternPlugin& p )
{
	float bins[ kBins ] = {};
	inject( p, bins );
}

/// A burst: `level` in bins [from, to], silence elsewhere.
void injectBurst( PatternPlugin& p, int from, int to, float level )
{
	float bins[ kBins ] = {};
	for( int j = std::max( 0, from ); j <= std::min( kBins - 1, to ); ++j )
		bins[ j ] = level;
	inject( p, bins );
}

/// Manual tempo at exactly 130 BPM (0.5 maps to 60 + 70): a period that is
/// exactly representable arithmetic, which --timing needs and the others
/// might as well share.
void manual130( PatternPlugin& p )
{
	p.SetFloatParameter( PT_TEMPO_SOURCE, 2.0f );
	p.SetFloatParameter( PT_BPM, 0.5f );
}

int writtenCells( const Tracker& t, int rows, int channels )
{
	int n = 0;
	for( int r = 0; r < rows; ++r )
		for( int k = 0; k < channels; ++k )
			n += t.At( r, k ).written ? 1 : 0;
	return n;
}

int channelOfBin( const Tracker& t, int j )
{
	const int channels = t.Settings().channels;
	for( int k = 0; k < channels; ++k )
		if( j >= t.BandEdge( k ) && j < t.BandEdge( k + 1 ) )
			return k;
	return -1;
}

struct Gl
{
	PatternPlugin& plugin;
	Target         target;

	Gl( PatternPlugin& p, int w, int h ) : plugin( p ), target( w, h )
	{
		FFGLViewportStruct vp = { 0, 0, static_cast< FFUInt32 >( w ), static_cast< FFUInt32 >( h ) };
		if( plugin.InitGL( &vp ) != FF_SUCCESS )
			std::printf( "  InitGL FAILED\n" );
	}
	~Gl() { plugin.DeInitGL(); }

	Image frame( double t )
	{
		ProcessOpenGLStruct gl = {};
		gl.HostFBO             = target.fbo;
		glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
		glViewport( 0, 0, target.w, target.h );
		glClearColor( 1.0f, 0.0f, 1.0f, 1.0f );// a sentinel no palette entry uses
		glClear( GL_COLOR_BUFFER_BIT );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		plugin.SetTime( t );
		plugin.ProcessOpenGL( &gl );
		return readBack( target );
	}
};

struct Raster
{
	int w, h;
};
const Raster kRasters[] = { { 640, 360 }, { 320, 180 } };
} // namespace

//---------------------------------------------------------------------------
// --timing
//---------------------------------------------------------------------------
namespace
{
struct TimingCase
{
	const char* name;
	int         bpm;    ///< delivered through SetBeatInfo, or 130 by the Manual control
	bool        manual; ///< true: Tempo Source Manual at 130
	int         speed;
	int         fps;
	int         swingQ; ///< swing in quarters of a row: 0 or 1 (0.25)
	int         rows;   ///< 64 or 32
	int64_t     epochFrames;
};

/// Frames `f` = 0..n-1 of a host at `fps` whose clock started `epochFrames`
/// frames ago, sent in MILLISECONDS as Resolume sends them. Returns the
/// number of frames whose cursor was not where the exact arithmetic says.
int measureTiming( const TimingCase& c, int n, bool floatPhase, int64_t& worstFrame )
{
	PatternPlugin p;
	p.ForceMillisecondsClock();
	p.StateForTest().debug.floatPhase = floatPhase;
	if( c.manual )
		manual130( p );
	else
		p.SetBeatInfo( static_cast< float >( c.bpm ), 0.0f );
	p.SetFloatParameter( PT_SPEED, static_cast< float >( c.speed ) );
	p.SetFloatParameter( PT_SWING, c.swingQ == 0 ? 0.0f : 0.5f );// 0.5 of the control is 0.25 of a row
	p.SetFloatParameter( PT_ROWS, c.rows == 32 ? 1.0f : 0.0f );
	injectSilence( p );

	const int64_t b = c.bpm, s = c.speed, f = c.fps;
	int           wrong = 0;
	worstFrame          = -1;
	for( int64_t i = 0; i < n; ++i )
	{
		const int64_t frame = c.epochFrames + i;
		p.SetTime( static_cast< double >( frame ) * 1000.0 / static_cast< double >( f ) );
		p.Advance( p.TickClock() );

		// rows since frame 0 = i / fps / ( speed x 2.5 / bpm ) = 2 i b / ( 5 s f ),
		// in exact integers. With swing 1/4: pair P = floor( raw / 2 ), and the
		// odd row starts at raw >= 2P + 5/4, i.e. 8 i b >= ( 8 P + 5 ) x 5 s f.
		int64_t expected;
		if( c.swingQ == 0 )
			expected = ( 2 * i * b ) / ( 5 * s * f );
		else
		{
			const int64_t P = ( i * b ) / ( 5 * s * f );
			expected        = 2 * P + ( 8 * i * b >= ( 8 * P + 5 ) * 5 * s * f ? 1 : 0 );
		}
		if( p.State().RowsElapsed() != expected || p.State().Row() != static_cast< int >( expected % c.rows ) )
		{
			++wrong;
			if( worstFrame < 0 )
				worstFrame = i;
		}
	}
	return wrong;
}
} // namespace

int runTiming()
{
	std::printf( "the cursor against the tracker's clock, from a host at Resolume's magnitude\n\n"
				 "  row period T = Speed x 2.5 / BPM; expected rows = floor( 2 i BPM / ( 5 Speed fps ) ) in int64;\n"
				 "  host time in milliseconds from frame 29,940,000 at 60 fps (= 499,000,000 ms)\n\n" );

	// 499,000,000 ms at each host rate, as a frame count.
	const TimingCase cases[] = {
		{ "host 125 BPM, speed 6, 60 fps, 64 rows", 125, false, 6, 60, 0, 64, 29940000 },
		{ "host 140 BPM, speed 3, 50 fps, 32 rows", 140, false, 3, 50, 0, 32, 24950000 },
		{ "host 100 BPM, speed 8, 30 fps", 100, false, 8, 30, 0, 64, 14970000 },
		{ "manual 130 BPM, speed 6, 60 fps", 130, true, 6, 60, 0, 64, 29940000 },
		{ "host 125 BPM, speed 6, 60 fps, swing 1/4 row", 125, false, 6, 60, 1, 64, 29940000 },
		{ "host 125 BPM, speed 6, 60 fps, from frame 0", 125, false, 6, 60, 0, 64, 0 },
	};
	for( const TimingCase& c : cases )
	{
		const int n = 600 * c.fps;// ten minutes
		int64_t   first = -1;
		const int wrong = measureTiming( c, n, false, first );
		Check( wrong == 0, std::string( c.name ) + ": " + std::to_string( n ) + " frames, " + std::to_string( wrong ) +
							   " wrong" + ( wrong > 0 ? " (first at frame " + std::to_string( first ) + ")" : "" ) );
	}

	// A tempo change mid-run carries the phase across: the rows elapsed never
	// jump backwards or by more than one row per frame.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetBeatInfo( 125.0f, 0.0f );
		injectSilence( p );
		int     jumps = 0;
		int64_t last  = 0;
		for( int i = 0; i < 3600; ++i )
		{
			if( i == 1200 )
				p.SetBeatInfo( 90.0f, 0.0f );
			if( i == 2400 )
				p.SetBeatInfo( 180.0f, 0.0f );
			p.Advance( 40.0 + i / 60.0 );
			const int64_t now = p.State().RowsElapsed();
			if( now < last || now > last + 1 )
				++jumps;
			last = now;
		}
		Check( jumps == 0, "a tempo change (125 -> 90 -> 180 BPM) carries the phase across: no row jumps in a minute (" +
							   std::to_string( jumps ) + ")" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		int64_t   first = -1;
		const int wrong = measureTiming( cases[ 0 ], 36000, true, first );
		Check( wrong > 0, "the phase kept in float at 499 million ms is rejected (" + std::to_string( wrong ) +
							  " of 36000 frames wrong, first at " + std::to_string( first ) + ")" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --onset
//---------------------------------------------------------------------------
namespace
{
/// Fresh plugin, silence primed, a one-frame burst filling band k at frame
/// `burstFrame`, held for `hold` frames. Returns the number of cells written
/// anywhere, and whether channel k on the row current at the burst holds one.
struct OnsetResult
{
	int  written = 0;
	bool inPlace = false;
	int  sample  = 0;
	int  volume  = 0;
};

OnsetResult onsetOnce( int channels, int law, int k, int burstFrame, int hold, bool levelDetector )
{
	PatternPlugin p;
	p.ForceSecondsClock();
	manual130( p );
	p.SetFloatParameter( PT_CHANNELS, channels == 8 ? 1.0f : 0.0f );
	p.SetFloatParameter( PT_BIN_LAW, static_cast< float >( law ) );
	p.StateForTest().debug.levelDetector = levelDetector;

	OnsetResult r;
	int         rowAtBurst = -1;
	for( int f = 0; f < burstFrame + hold + 30; ++f )
	{
		if( f >= burstFrame && f < burstFrame + hold )
			injectBurst( p, p.State().BandEdge( k ), p.State().BandEdge( k + 1 ) - 1, 1.0f );
		else
			injectSilence( p );
		p.Advance( f / 60.0 );
		if( f == burstFrame )
		{
			rowAtBurst    = p.State().Row();
			const Cell& c = p.State().At( rowAtBurst, k );
			r.inPlace     = c.written;
			r.sample      = c.sample;
			r.volume      = c.volume;
		}
	}
	r.written = writtenCells( p.State(), 64, channels );
	return r;
}
} // namespace

int runOnset()
{
	std::printf( "a burst in band k writes one note in channel k on the current row, and nowhere else\n\n" );
	for( int channels : { 4, 8 } )
		for( int law : { 0, 1 } )
		{
			bool all = true, one = true, samples = true;
			for( int k = 0; k < channels; ++k )
			{
				const OnsetResult r = onsetOnce( channels, law, k, 30 + 5 * k, 1, false );
				all     = all && r.inPlace;
				one     = one && r.written == 1;
				samples = samples && r.sample == k + 1 && r.volume >= 1 && r.volume <= 64;
			}
			const std::string where = std::to_string( channels ) + " channels, " + ( law ? "Log" : "Linear" );
			Check( all, where + ": every channel's burst writes its own cell on the row current at that frame" );
			Check( one, where + ": and exactly one cell in the whole pattern" );
			Check( samples, where + ": with the channel number as the sample and a Cxx volume in 01..40" );
		}

	// A burst held across a row boundary writes once: the detector fires on
	// the rise, not on the level.
	{
		const OnsetResult r = onsetOnce( 4, 0, 1, 30, 8, false );
		Check( r.written == 1 && r.inPlace, "a burst held for 8 frames (over a row boundary) writes exactly one note (" +
												std::to_string( r.written ) + ")" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		const OnsetResult r = onsetOnce( 4, 0, 1, 30, 8, true );
		Check( r.written > 1, "a detector that fires on level rather than change writes the held burst again on the next row (" +
								  std::to_string( r.written ) + " cells)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --prime
//---------------------------------------------------------------------------
namespace
{
/// Loud stationary noise already playing when the clip starts, from a host
/// clock at `startSeconds`. Returns the cells written in the first second,
/// and whether a real onset at 1.5 s was then heard.
void primeRun( double startSeconds, bool noPrime, bool millis, int& falseNotes, bool& heardLater )
{
	PatternPlugin p;
	if( millis )
		p.ForceMillisecondsClock();
	else
		p.ForceSecondsClock();
	manual130( p );
	p.StateForTest().debug.noPrime = noPrime;
	Synth noise;
	noise.kind = Synth::Noise;

	float bins[ kBins ];
	for( int f = 0; f <= 60; ++f )
	{
		noise.bins( f, bins );
		inject( p, bins );
		const double t = startSeconds + f / 60.0;
		if( millis )
		{
			p.SetTime( t * 1000.0 );
			p.Advance( p.TickClock() );
		}
		else
			p.Advance( t );
	}
	falseNotes = writtenCells( p.State(), 64, 4 );

	// Then a real event: the noise, plus a burst four times its level in
	// channel 0's band, for one frame at 1.5 s.
	const int before = falseNotes;
	for( int f = 61; f <= 120; ++f )
	{
		noise.bins( f, bins );
		if( f == 90 )
			for( int j = p.State().BandEdge( 0 ); j < p.State().BandEdge( 1 ); ++j )
				bins[ j ] = 1.2f;
		inject( p, bins );
		const double t = startSeconds + f / 60.0;
		if( millis )
		{
			p.SetTime( t * 1000.0 );
			p.Advance( p.TickClock() );
		}
		else
			p.Advance( t );
	}
	heardLater = writtenCells( p.State(), 64, 4 ) > before;
}
} // namespace

int runPrime()
{
	std::printf( "frame one: loud audio already playing when the clip starts\n\n"
				 "  stationary noise, 0.3 in every bin +-5 %% a frame; the clip triggers at 40 s and at 499,000 s\n\n" );
	struct Case
	{
		const char* name;
		double      start;
		bool        millis;
	};
	const Case cases[] = { { "clock at 40 s", 40.0, false }, { "clock at 499,000,000 ms", 499000.0, true } };
	for( const Case& c : cases )
	{
		int  falseNotes = 0;
		bool heard      = false;
		primeRun( c.start, false, c.millis, falseNotes, heard );
		Check( falseNotes == 0, std::string( c.name ) + ": no false note in the first second (" + std::to_string( falseNotes ) + ")" );
		Check( heard, std::string( c.name ) + ": and a real onset at 1.5 s is still heard -- the detector is not deaf" );
	}

	// A repeated host time advances nothing and writes nothing.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		manual130( p );
		Synth noise;
		noise.kind = Synth::Noise;
		float bins[ kBins ];
		for( int f = 0; f < 10; ++f )
		{
			noise.bins( f, bins );
			inject( p, bins );
			p.Advance( 40.0 );
		}
		Check( p.State().RowsElapsed() == 0 && writtenCells( p.State(), 64, 4 ) == 0,
			   "ten frames at the same host time advance no row and write no note" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		int  falseNotes = 0;
		bool heard      = false;
		primeRun( 40.0, true, false, falseNotes, heard );
		Check( falseNotes > 0, "an unprimed detector writes false notes in the first second (" + std::to_string( falseNotes ) + ")" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --pitch
//---------------------------------------------------------------------------
namespace
{
/// The harness's own reading of the bin law, written from Bands.h's comment
/// rather than by calling it.
double expectedHz( int j, int law, double fs )
{
	const double u = ( j + 0.5 ) / 64.0;
	return law == 1 ? 20.0 * std::pow( fs * 0.5 / 20.0, u ) : u * fs * 0.5;
}
int expectedNote( int j, int law, double fs, bool fold )
{
	const int midi = static_cast< int >( std::lround( 69.0 + 12.0 * std::log2( expectedHz( j, law, fs ) / 440.0 ) ) );
	int       n    = midi - 48;
	if( fold )
	{
		while( n < 0 )
			n += 12;
		while( n >= 36 )
			n -= 12;
	}
	return ( n >= 0 && n < 36 ) ? n : -1;
}

/// The note channel k wrote for a one-frame peak at bin j, or -2 for none.
int noteForBin( int j, int law, bool fold, double detune, unsigned int sampleRate )
{
	PatternPlugin p;
	p.ForceSecondsClock();
	manual130( p );
	p.SetFloatParameter( PT_BIN_LAW, static_cast< float >( law ) );
	p.SetFloatParameter( PT_FOLD, fold ? 1.0f : 0.0f );
	if( sampleRate != 0 )
		p.SetSampleRate( sampleRate );
	p.StateForTest().debug.detuneSemitones = detune;
	injectSilence( p );
	p.Advance( 0.0 );
	injectSilence( p );
	p.Advance( 1 / 60.0 );
	// The peak, with quieter neighbours so the band's loudest bin is j.
	float bins[ kBins ] = {};
	bins[ j ]           = 1.0f;
	if( j > 0 )
		bins[ j - 1 ] = 0.3f;
	if( j + 1 < kBins )
		bins[ j + 1 ] = 0.3f;
	inject( p, bins );
	p.Advance( 2 / 60.0 );
	const int   k = channelOfBin( p.State(), j );
	const Cell& c = p.State().At( p.State().Row(), k );
	return c.written ? c.note : -2;
}
} // namespace

int runPitch()
{
	std::printf( "a peak at bin j becomes the note the bin law predicts\n\n"
				 "  Linear: f = ( j + 1/2 ) / 64 x fs/2;  Log: f = 20 Hz x ( fs/2 / 20 )^(( j + 1/2 ) / 64);  A4 = 440;\n"
				 "  C-1..B-3 is MIDI 48..83 (the tracker's C-2 is middle C); fs 44100 unless the host says\n\n" );
	for( int law : { 0, 1 } )
		for( bool fold : { false, true } )
		{
			int wrong = 0, notes = 0, dashes = 0, nonMonotone = 0, lastNote = -1;
			for( int j = 0; j < kBins; ++j )
			{
				const int got  = noteForBin( j, law, fold, 0.0, 0 );
				const int want = expectedNote( j, law, 44100.0, fold );
				if( got != want )
					++wrong;
				if( got >= 0 )
					++notes;
				else if( got == -1 )
					++dashes;
				if( !fold && got >= 0 )
				{
					if( got < lastNote )
						++nonMonotone;
					lastNote = got;
				}
			}
			const std::string where = std::string( law ? "Log" : "Linear" ) + ( fold ? ", folded" : ", unfolded" );
			Check( wrong == 0, where + ": all 64 bins give the predicted note or --- (" + std::to_string( wrong ) + " differ; " +
								   std::to_string( notes ) + " notes, " + std::to_string( dashes ) + " dashes)" );
			if( !fold )
				Check( nonMonotone == 0, where + ": the note never falls as the bin rises" );
			if( fold )
				Check( dashes == 0 && notes == 64, where + ": folding leaves no bin without a note" );
		}
	// Unfolded, the two laws cover the range differently, which is the point
	// of the switch: under Linear at 44.1 kHz only the first few bins are in
	// C-1..B-3; under Log the bottom bin is below it and most of the middle is in.
	{
		int linearNotes = 0, logNotes = 0;
		for( int j = 0; j < kBins; ++j )
		{
			linearNotes += noteForBin( j, 0, false, 0.0, 0 ) >= 0;
			logNotes += noteForBin( j, 1, false, 0.0, 0 ) >= 0;
		}
		Check( linearNotes >= 1 && linearNotes <= 4 && logNotes > 12,
			   "unfolded, Linear puts " + std::to_string( linearNotes ) + " bins in range and Log " + std::to_string( logNotes ) +
				   " -- the laws differ, so the switch is not decorative" );
	}
	// The host's sample rate moves the notes.
	{
		const int at44 = noteForBin( 1, 0, true, 0.0, 44100 );
		const int at96 = noteForBin( 1, 0, true, 0.0, 96000 );
		Check( at44 != at96 && at96 == expectedNote( 1, 0, 96000.0, true ),
			   "SetSampleRate( 96000 ) moves bin 1 from " + std::string( bands::NoteName( at44 ) ) + " to " +
				   bands::NoteName( at96 ) + ", as predicted" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		int wrong = 0;
		for( int j = 0; j < kBins; ++j )
			wrong += noteForBin( j, 1, true, 1.0, 0 ) != expectedNote( j, 1, 44100.0, true );
		Check( wrong > 32, "a pitch read one semitone sharp is rejected (" + std::to_string( wrong ) + " of 64 bins differ)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --ring
//---------------------------------------------------------------------------
namespace
{
struct RingResult
{
	bool pass0Rows = true; ///< rows written on pass 0 only: cleared (or kept)
	bool pass1Rows = true; ///< rows written on both: carry pass 1's note
	bool untouched = true; ///< rows never written: empty
	int  clearedCount = 0;
};

RingResult ringRun( bool keep, bool noClear )
{
	PatternPlugin p;
	p.ForceSecondsClock();
	manual130( p );
	p.SetFloatParameter( PT_KEEP, keep ? 1.0f : 0.0f );
	p.StateForTest().debug.noClear = noClear;

	const std::vector< int > pass0 = { 4, 9, 20, 40, 63 };
	const std::vector< int > pass1 = { 9, 40 };
	int                      lastRow = -1;
	int64_t                  lastPass = -1;
	injectSilence( p );
	p.Advance( 0.0 );
	// Two full passes and a bit: 64 rows at 130 BPM, speed 6, are 7.38 s.
	for( int f = 1; f < 60 * 16; ++f )
	{
		// Onsets are placed on the first frame the cursor lands on a target row.
		const int     row  = p.State().Row();
		const int64_t pass = p.State().Pass();
		bool          fire = false;
		if( row != lastRow || pass != lastPass )
		{
			const auto& targets = pass == 0 ? pass0 : pass1;
			fire                = pass <= 1 && std::find( targets.begin(), targets.end(), row ) != targets.end();
		}
		lastRow  = row;
		lastPass = pass;
		if( fire )
			injectBurst( p, 0, p.State().BandEdge( 1 ) - 1, pass == 0 ? 1.0f : 0.5f );
		else
			injectSilence( p );
		p.Advance( f / 60.0 );
		if( p.State().Pass() >= 2 && p.State().Row() >= 1 )
			break;
	}

	// Note: the onset lands on the frame AFTER the cursor was seen on the
	// row, which is still the same row (a row is 6.9 frames long).
	RingResult r;
	for( int row = 0; row < 64; ++row )
	{
		const Cell& c        = p.State().At( row, 0 );
		const bool  in0      = std::find( pass0.begin(), pass0.end(), row ) != pass0.end();
		const bool  in1      = std::find( pass1.begin(), pass1.end(), row ) != pass1.end();
		if( in1 )
			r.pass1Rows = r.pass1Rows && c.written && c.pass == 1;
		else if( in0 )
		{
			if( keep )
				r.pass0Rows = r.pass0Rows && c.written && c.pass == 0;
			else
			{
				r.pass0Rows = r.pass0Rows && !c.written;
				r.clearedCount += !c.written;
			}
		}
		else
			r.untouched = r.untouched && !c.written;
	}
	return r;
}
} // namespace

int runRing()
{
	std::printf( "the ring: after 64 rows the next pass overwrites\n\n"
				 "  pass 0 writes rows 04 09 14 28 3F; pass 1 writes 09 28 only; read after pass 1 is complete\n\n" );
	{
		const RingResult r = ringRun( false, false );
		Check( r.pass0Rows, "Keep Notes off: rows written on pass 0 only are cleared to --- on pass 1 (" +
								std::to_string( r.clearedCount ) + " of 3)" );
		Check( r.pass1Rows, "rows written on both passes carry pass 1's note" );
		Check( r.untouched, "rows never written stay empty" );
	}
	{
		const RingResult r = ringRun( true, false );
		Check( r.pass0Rows, "Keep Notes on: rows written on pass 0 only survive pass 1" );
		Check( r.pass1Rows && r.untouched, "and pass 1's rows are overwritten, the rest still empty" );
	}
	// A jump of more than a pattern clears every row once.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		manual130( p );
		injectSilence( p );
		p.Advance( 0.0 );
		injectBurst( p, 0, p.State().BandEdge( 1 ) - 1, 1.0f );
		p.Advance( 1 / 60.0 );
		injectSilence( p );
		const int before = writtenCells( p.State(), 64, 4 );
		p.Advance( 60.0 );// 520 rows later
		Check( before == 1 && writtenCells( p.State(), 64, 4 ) == 0, "a jump of 520 rows clears the pattern (" +
																		 std::to_string( before ) + " -> " +
																		 std::to_string( writtenCells( p.State(), 64, 4 ) ) + ")" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		const RingResult r = ringRun( false, true );
		Check( !r.pass0Rows, "a pass that never clears is rejected (" + std::to_string( r.clearedCount ) + " of 3 cleared)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --detected
//---------------------------------------------------------------------------
namespace
{
/// A metronome at `bpm` for `seconds`, at `fps`. Returns the first time the
/// detected tempo was within +-1 of the truth, and whether it stayed there.
void detectRun( double bpm, double fps, double lagBias, double& settledAt, bool& stayed, double& finalBpm )
{
	PatternPlugin p;
	p.ForceSecondsClock();
	p.SetFloatParameter( PT_TEMPO_SOURCE, 1.0f );
	p.StateForTest().debug.lagBias = lagBias;
	Synth m;
	m.kind = Synth::Metronome;
	m.bpm  = bpm;
	m.fps  = fps;

	settledAt = -1.0;
	stayed    = true;
	float bins[ kBins ];
	const int frames = static_cast< int >( 12.0 * fps );
	for( int f = 0; f < frames; ++f )
	{
		m.bins( f, bins );
		inject( p, bins );
		p.Advance( f / fps );
		const bool within = p.State().TempoSettled() && std::fabs( p.State().DetectedBpm() - bpm ) <= 1.0;
		if( within && settledAt < 0.0 )
			settledAt = f / fps;
		if( settledAt >= 0.0 && !within )
			stayed = false;
	}
	finalBpm = p.State().DetectedBpm();
}
} // namespace

int runDetected()
{
	std::printf( "detected tempo: a metronome's onset train, autocorrelated\n\n" );
	struct Case
	{
		double bpm, fps;
	};
	const Case cases[] = { { 120.0, 60.0 }, { 100.0, 50.0 }, { 150.0, 60.0 }, { 120.0, 24.0 } };
	for( const Case& c : cases )
	{
		double settledAt = -1.0, finalBpm = 0.0;
		bool   stayed = true;
		detectRun( c.bpm, c.fps, 0.0, settledAt, stayed, finalBpm );
		Check( settledAt >= 0.0 && settledAt <= 6.0,
			   F( c.bpm, 0 ) + " BPM at " + F( c.fps, 0 ) + " fps: within +-1 BPM by " + F( settledAt, 2 ) + " s (allowed 6)" );
		Check( stayed, "  and stays there for the rest of 12 s (last reading " + F( finalBpm, 2 ) + ")" );
	}
	// And the row clock follows it: with Tempo Source Detected the tracker's
	// BPM is the detected one once settled.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetFloatParameter( PT_TEMPO_SOURCE, 1.0f );
		p.SetBeatInfo( 77.0f, 0.0f );// a host saying something else
		Synth m;
		m.kind = Synth::Metronome;
		m.bpm  = 120.0;
		float bins[ kBins ];
		for( int f = 0; f < 600; ++f )
		{
			m.bins( f, bins );
			inject( p, bins );
			p.Advance( f / 60.0 );
		}
		Check( std::fabs( p.State().Bpm() - 120.0 ) <= 1.0, "the row clock runs at the detected tempo, not the host's 77 (" +
																 F( p.State().Bpm(), 2 ) + ")" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		double settledAt = -1.0, finalBpm = 0.0;
		bool   stayed = true;
		detectRun( 120.0, 60.0, 0.1, settledAt, stayed, finalBpm );
		Check( settledAt < 0.0, "a lag scaled by 1.1 never lands within +-1 BPM (reads " + F( finalBpm, 2 ) + ")" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --groove: the detected tempo on programme-shaped grooves, not a metronome
//---------------------------------------------------------------------------
namespace
{
/// An RBJ-cookbook biquad, direct form I. Two in series stand in for the
/// fourth-order Butterworths the release video's track used.
struct Biquad
{
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;

	static Biquad make( int type, double hz, double q, double rate )
	{
		const double w = 2.0 * M_PI * hz / rate, c = std::cos( w ), alpha = std::sin( w ) / ( 2.0 * q );
		double       nb0, nb1, nb2;
		if( type == 0 )// low-pass
			nb0 = ( 1 - c ) / 2, nb1 = 1 - c, nb2 = ( 1 - c ) / 2;
		else if( type == 1 )// high-pass
			nb0 = ( 1 + c ) / 2, nb1 = -( 1 + c ), nb2 = ( 1 + c ) / 2;
		else// band-pass, 0 dB peak
			nb0 = alpha, nb1 = 0, nb2 = -alpha;
		const double a0 = 1 + alpha;
		Biquad       f;
		f.b0 = nb0 / a0, f.b1 = nb1 / a0, f.b2 = nb2 / a0, f.a1 = -2 * c / a0, f.a2 = ( 1 - alpha ) / a0;
		return f;
	}
	double step( double x )
	{
		const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1, x1 = x, y2 = y1, y1 = y;
		return y;
	}
};

/// A drum groove rendered as audio, so it reaches the plugin the way music
/// does: through a 2048-point FFT folded into 64 bins (the `--wav` path),
/// with the leakage, the window's smear and the frame grid's jitter that
/// the one-frame bursts of `Synth` do not have. Every sample is a pure
/// function of the arguments; the noise is an integer hash.
struct Groove
{
	enum Kind
	{
		Backbeat, ///< kick on 1 and 3, clap on 2 and 4, hats on the eighths
		KickHeavy,///< the same with the clap 12 dB and the hats 20 dB down
		SyncKick, ///< a kick on the "and" of 2 as well
		Arp,      ///< the backbeat under the video's first-cut pluck: 0,2,3,5,7 of each eight sixteenths
		House,    ///< the release video's groove: kick every beat, off-beat bass, clap, hats, ticks, pluck
		Break,    ///< kick 1, "a" of 1, "and" of 3; snare 2 and 4 with ghosts; sixteenth hats
		Count
	};
	static const char* name( int k )
	{
		static const char* const names[] = { "backbeat", "kick-heavy", "syncopated kick", "syncopated pluck", "house (the video's)", "breakbeat" };
		return names[ k ];
	}

	static constexpr double kRate = 48000.0;

	static double noise( uint32_t i, uint32_t salt ) { return jitter( static_cast< int >( i ), 7, static_cast< int >( salt ) ); }
	static double env( double t, double attack, double decay ) { return std::min( 1.0, t / attack ) * std::exp( -t / decay ); }

	static std::vector< double > kick( double amp )
	{
		std::vector< double > v( static_cast< size_t >( 0.30 * kRate ) );
		Biquad                l1 = Biquad::make( 0, 300, 0.707, kRate ), l2 = l1;
		double                phase = 0.0;
		for( size_t i = 0; i < v.size(); ++i )
		{
			const double t = i / kRate;
			phase += 2.0 * M_PI * ( 48.0 + 62.0 * std::exp( -t / 0.03 ) ) / kRate;
			v[ i ] = l2.step( l1.step( amp * env( t, 0.004, 0.09 ) * std::sin( phase ) ) );
		}
		return v;
	}
	static std::vector< double > filtered( double seconds, double attack, double decay, int type, double hz, double q,
										   double amp, uint32_t salt )
	{
		std::vector< double > v( static_cast< size_t >( seconds * kRate ) );
		Biquad                f1 = Biquad::make( type, hz, q, kRate ), f2 = f1;
		for( size_t i = 0; i < v.size(); ++i )
			v[ i ] = f2.step( f1.step( amp * env( i / kRate, attack, decay ) * noise( static_cast< uint32_t >( i ), salt ) ) );
		return v;
	}
	static std::vector< double > tone( double hz, double seconds, double attack, double decay, double amp )
	{
		std::vector< double > v( static_cast< size_t >( seconds * kRate ) );
		for( size_t i = 0; i < v.size(); ++i )
			v[ i ] = amp * env( i / kRate, attack, decay ) * std::sin( 2.0 * M_PI * hz * i / kRate );
		return v;
	}

	/// `seconds` of the groove at `bpm`, as a mono WAV the harness's FFT reads.
	static Wav render( int kind, double bpm, double seconds )
	{
		struct Part
		{
			std::vector< double > sound;
			std::vector< int >    steps;
			double                gain = 1.0;
		};
		const bool   heavy = kind == KickHeavy, house = kind == House;
		const double kickAmp = house ? 0.6 : 0.9, clapAmp = house ? 0.4 : ( heavy ? 0.3 : 1.2 );
		const double hatAmp  = heavy ? 0.035 : 0.35;
		std::vector< Part > parts;
		const auto clap = filtered( 0.25, 0.003, 0.06, 2, 2450.0, 1.0, clapAmp * 1.7, 11 );
		const auto hat  = filtered( 0.08, 0.002, 0.025, 1, 7000.0, 0.707, hatAmp * 1.7, 13 );
		const auto hatOff = filtered( 0.11, 0.002, 0.05, 1, 7000.0, 0.707, hatAmp * 1.7 * ( house ? 0.77 : 0.6 ), 17 );
		const std::vector< int > eighthsOn = { 0, 4, 8, 12 }, eighthsOff = { 2, 6, 10, 14 };
		switch( kind )
		{
		case Backbeat:
		case KickHeavy:
		case Arp:
			parts.push_back( { kick( kickAmp ), { 0, 8 } } );
			parts.push_back( { clap, { 4, 12 } } );
			break;
		case SyncKick:
			parts.push_back( { kick( kickAmp ), { 0, 6, 8 } } );
			parts.push_back( { clap, { 4, 12 } } );
			break;
		case House:
			parts.push_back( { kick( kickAmp ), { 0, 4, 8, 12 } } );
			parts.push_back( { tone( 55.0, 0.2, 0.006, 0.25, 0.36 ), eighthsOff } );
			parts.push_back( { clap, { 4, 12 } } );
			parts.push_back( { filtered( 0.03, 0.002, 0.008, 1, 14000.0, 0.707, 0.34 * 1.7, 19 ), { 1, 3, 5, 7, 9, 11, 13, 15 } } );
			break;
		case Break:
			parts.push_back( { kick( kickAmp ), { 0, 3, 10 } } );
			parts.push_back( { clap, { 4, 12 } } );
			parts.push_back( { clap, { 7, 15 }, 0.3 } );
			parts.push_back( { hatOff, { 1, 3, 5, 7, 9, 11, 13, 15 } } );
			break;
		default:
			break;
		}
		parts.push_back( { hat, eighthsOn } );
		if( kind != Break )
			parts.push_back( { hatOff, eighthsOff } );
		else
			parts.push_back( { hat, eighthsOff } );

		Wav wav;
		wav.rate = kRate;
		wav.mono.assign( static_cast< size_t >( seconds * kRate ), 0.0f );
		std::vector< double > mix( wav.mono.size(), 0.0 );
		const double          sixteenth = 15.0 / bpm;
		const int             bars      = static_cast< int >( seconds / ( 16 * sixteenth ) ) + 1;
		auto                  put       = [ & ]( const std::vector< double >& s, double at, double gain ) {
            const size_t i0 = static_cast< size_t >( std::llround( at * kRate ) );
            for( size_t i = 0; i < s.size() && i0 + i < mix.size(); ++i )
                mix[ i0 + i ] += gain * s[ i ];
		};
		for( int bar = 0; bar < bars; ++bar )
			for( const Part& part : parts )
				for( int s : part.steps )
					put( part.sound, ( bar * 16 + s ) * sixteenth, part.gain );
		if( kind == Arp || kind == House )
		{
			// The pluck: sines on bin centres, so each note sits in one bin.
			static const double arpHz[] = { 1875.0, 2625.0, 2250.0, 3000.0, 3375.0 };
			const std::vector< int > gate = kind == Arp ? std::vector< int >{ 0, 2, 3, 5, 7, 8, 10, 11, 13, 15 }
														: std::vector< int >{ 1, 3, 5, 7, 9, 11, 13, 15 };
			int note = 0;
			for( int bar = 0; bar < bars; ++bar )
				for( int s : gate )
					put( tone( arpHz[ note++ % 5 ], 0.15, 0.004, 0.05, kind == Arp ? 0.45 : 0.30 ), ( bar * 16 + s ) * sixteenth, 1.0 );
		}
		double peak = 1e-9;
		for( double v : mix )
			peak = std::max( peak, std::fabs( v ) );
		for( size_t i = 0; i < mix.size(); ++i )
			wav.mono[ i ] = static_cast< float >( 0.89 * mix[ i ] / peak );
		return wav;
	}
};

struct TempoRun
{
	double settledAt = -1.0;///< the first frame after which every frame reads within +-1
	double finalBpm  = 0.0;
	double share = 1.0, faster = 0.0;///< the true level's lowest share of its family's best, and double time's highest
	double worst     = 0.0;///< the largest |error| after settling
	std::vector< double > perSecond;
};

/// A WAV through the plugin's audio input, with Tempo Source = Detected and a
/// host saying something else. Reads the detected tempo on every frame.
TempoRun tempoOf( const Wav& wav, double truth, double fps, double seconds, bool legacy )
{
	PatternPlugin p;
	p.ForceSecondsClock();
	p.SetSampleRate( static_cast< unsigned int >( wav.rate ) );
	p.SetFloatParameter( PT_TEMPO_SOURCE, 1.0f );
	p.SetBeatInfo( 77.0f, 0.0f );
	p.StateForTest().debug.legacyTempo = legacy;
	TempoRun run;
	float    bins[ kBins ];
	const int frames = static_cast< int >( seconds * fps );
	int       lastWrong = -1;
	std::vector< double > readings( static_cast< size_t >( frames ) );
	for( int f = 0; f < frames; ++f )
	{
		wav.bins( ( f + 1 ) / fps, bins );
		inject( p, bins );
		p.Advance( f / fps );
		const double bpm    = p.State().TempoSettled() ? p.State().DetectedBpm() : 0.0;
		readings[ static_cast< size_t >( f ) ] = bpm;
		if( f >= static_cast< int >( 3.0 * fps ) && p.State().TempoSettled() )
			for( int i = 0; i < p.State().TempoMembers(); ++i )
			{
				// The member at the true tempo, and the one at twice it.
				const double b = p.State().TempoMemberBpm( i ), share = p.State().TempoMemberShare( i );
				if( std::fabs( b / truth - 1.0 ) < 0.02 )
					run.share = std::min( run.share, share );
				else if( std::fabs( b / truth - 2.0 ) < 0.04 )
					run.faster = std::max( run.faster, share );
			}
		if( !( std::fabs( bpm - truth ) <= 1.0 ) )
			lastWrong = f;
		if( f > 0 && std::floor( f / fps ) != std::floor( ( f - 1 ) / fps ) )
			run.perSecond.push_back( bpm );
	}
	run.finalBpm = readings.back();
	if( lastWrong < frames - 1 )
	{
		run.settledAt = ( lastWrong + 1 ) / fps;
		for( int f = lastWrong + 1; f < frames; ++f )
			run.worst = std::max( run.worst, std::fabs( readings[ static_cast< size_t >( f ) ] - truth ) );
	}
	return run;
}
} // namespace

int runGroove()
{
	std::printf( "detected tempo on grooves: audio through the plugin's input, Tempo Source = Detected, host at 77\n"
				 "(16 s each at 60 fps; settled = within +-1 BPM from then to the end; allowed 6 s)\n\n" );
	constexpr double kSeconds = 16.0, kAllowed = 6.0;
	struct Case
	{
		int    kind;
		double bpm, fps;
	};
	std::vector< Case > cases;
	const double tempos[] = { 80, 90, 100, 110, 125, 140, 150, 160, 170 };
	for( int kind : { Groove::Backbeat, Groove::KickHeavy, Groove::SyncKick, Groove::Arp } )
		for( double bpm : tempos )
			cases.push_back( { kind, bpm, 60.0 } );
	for( double bpm : { 115.0, 120.0, 125.0, 130.0, 135.0 } )
		cases.push_back( { Groove::House, bpm, 60.0 } );
	for( double fps : { 24.0, 30.0, 50.0 } )
		cases.push_back( { Groove::Backbeat, 125.0, fps } );
	cases.push_back( { Groove::House, 125.0, 30.0 } );

	double latest = 0.0, worst = 0.0, minShare = 1.0, maxFaster = 0.0;
	int    legacyFailed = 0;
	std::vector< std::string > legacyFails;
	for( const Case& c : cases )
	{
		const Wav      wav = Groove::render( c.kind, c.bpm, kSeconds );
		const TempoRun run = tempoOf( wav, c.bpm, c.fps, kSeconds, false );
		const TempoRun old = tempoOf( wav, c.bpm, c.fps, kSeconds, true );
		const bool     ok  = run.settledAt >= 0.0 && run.settledAt <= kAllowed;
		Check( ok, std::string( Groove::name( c.kind ) ) + " at " + F( c.bpm, 0 ) + " BPM, " + F( c.fps, 0 ) + " fps: reads " +
					   F( run.finalBpm, 2 ) + ", settled by " + ( run.settledAt >= 0.0 ? F( run.settledAt, 2 ) + " s" : "never" ) +
					   " (worst after " + F( run.worst, 2 ) + "; the beat's share " + F( run.share, 2 ) + ", double time's " + F( run.faster, 2 ) +
					   "; v0.1.0 read " + F( old.finalBpm, 2 ) + ")" );
		if( ok )
		{
			latest    = std::max( latest, run.settledAt );
			worst     = std::max( worst, run.worst );
			minShare  = std::min( minShare, run.share );
			maxFaster = std::max( maxFaster, run.faster );
		}
		if( !( old.settledAt >= 0.0 && old.settledAt <= kAllowed ) )
		{
			++legacyFailed;
			legacyFails.push_back( std::string( Groove::name( c.kind ) ) + " " + F( c.bpm, 0 ) + "->" + F( old.finalBpm, 1 ) );
		}
	}
	std::printf( "\n  every groove: settled by %.2f s at the latest, worst error after settling %.2f BPM\n", latest, worst );
	std::printf( "  the level margins: every true beat scored >= %.3f of its family's best, every double-time\n"
				 "  impostor <= %.3f, either side of the ratio %.2f\n", minShare, maxFaster, kTempoLevelRatio );

	// And it reaches the clock and the picture: the row clock runs at the
	// detected tempo, not the host's 77, and the header says so with a '*'.
	{
		const Wav     wav = Groove::render( Groove::House, 125.0, 8.0 );
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetSampleRate( 48000 );
		p.SetFloatParameter( PT_TEMPO_SOURCE, 1.0f );
		p.SetBeatInfo( 77.0f, 0.0f );
		float bins[ kBins ];
		for( int f = 0; f < 8 * 30; ++f )
		{
			wav.bins( ( f + 1 ) / 30.0, bins );
			inject( p, bins );
			p.Advance( f / 30.0 );
		}
		const Screen screen = p.BuildScreen();
		std::string  header;
		for( int c = 0; c < screen.cols; ++c )
			header += static_cast< char >( screen.At( c, 0 ).glyph );
		Check( std::fabs( p.State().Bpm() - 125.0 ) <= 1.0 && header.find( "BPM 125*" ) != std::string::npos,
			   "the row clock follows it (" + F( p.State().Bpm(), 2 ) + " BPM) and the header reads \"" +
				   header.substr( 0, header.find_last_not_of( ' ' ) + 1 ) + "\"" );
	}

	std::printf( "\n  measured, NOT asserted (AGENTS.md: metres this detector reads ambiguously):\n" );
	auto measure = [ & ]( int kind, double bpm ) {
		const TempoRun run = tempoOf( Groove::render( kind, bpm, kSeconds ), bpm, 60.0, kSeconds, false );
		std::printf( "       %-20s at %3.0f BPM reads %6.2f%s\n", Groove::name( kind ), bpm, run.finalBpm,
					 run.settledAt >= 0.0 && run.settledAt <= kAllowed ? "" : "  (not within +-1 by 6 s)" );
	};
	for( double bpm : tempos )
		measure( Groove::Break, bpm );
	for( double bpm : { 80.0, 90.0, 100.0, 150.0, 160.0, 170.0 } )
		measure( Groove::House, bpm );

	std::printf( "\n  negative control -- v0.1.0's detector (legacyTempo) must FAIL:\n" );
	Check( legacyFailed > 0, "v0.1.0's detector fails " + std::to_string( legacyFailed ) + " of " + std::to_string( cases.size() ) + " grooves" );
	{
		std::string list;
		for( size_t i = 0; i < legacyFails.size(); ++i )
			list += ( i ? ", " : "" ) + legacyFails[ i ];
		std::printf( "       %s\n", list.c_str() );
	}
	{
		const TempoRun old = tempoOf( Groove::render( Groove::House, 125.0, kSeconds ), 125.0, 30.0, kSeconds, true );
		Check( !( old.settledAt >= 0.0 && old.settledAt <= kAllowed ),
			   "and on the release video's groove at 125 BPM, 30 fps it reads " + F( old.finalBpm, 2 ) + ", not 125" );
	}
	return failures == 0 ? 0 : 1;
}

/// `--tempo-wav f.wav --truth N [--fps N] [--from S]`: the detected tempo of a
/// real file, once a second; with --truth, every reading from --from on must be
/// within +-1 BPM. The release video's soundtrack is the fixture verify.sh uses
/// when the backend checkout is present.
int runTempoWav( const std::string& path, double truth, double fps, double from, bool legacy )
{
	Wav         wav;
	std::string error;
	if( !wav.load( path, error ) )
	{
		std::printf( "%s\n", error.c_str() );
		return 2;
	}
	const double seconds = wav.mono.size() / wav.rate;
	const TempoRun run   = tempoOf( wav, truth > 0 ? truth : 120.0, fps, seconds, legacy );
	std::printf( "detected tempo of %s (%.1f s at %.0f Hz, %.0f fps%s), one reading a second:\n", path.c_str(), seconds, wav.rate, fps,
				 legacy ? ", v0.1.0's detector" : "" );
	int bad = 0, counted = 0;
	for( size_t i = 0; i < run.perSecond.size(); ++i )
	{
		const double t = static_cast< double >( i + 1 );
		std::printf( "%s%6.2f", i % 12 == 0 ? "\n  " : " ", run.perSecond[ i ] );
		if( truth > 0 && t >= from )
		{
			++counted;
			if( !( std::fabs( run.perSecond[ i ] - truth ) <= 1.0 ) )
				++bad;
		}
	}
	std::printf( "\n" );
	if( truth > 0 )
	{
		Check( counted > 0 && bad == 0, std::to_string( counted - bad ) + " of " + std::to_string( counted ) + " readings from " + F( from, 0 ) +
											" s within +-1 of " + F( truth, 1 ) + " BPM" );
		return failures == 0 ? 0 : 1;
	}
	return 0;
}

//---------------------------------------------------------------------------
// --names, --list, --font
//---------------------------------------------------------------------------
int runNames()
{
	PatternPlugin plugin;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	int                        bad = 0;
	std::vector< std::string > seen;
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( name == nullptr )
			continue;
		if( std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu characters\n", id, name, std::strlen( name ) );
			++bad;
		}
		if( std::find( seen.begin(), seen.end(), std::string( name ) ) != seen.end() )
		{
			std::printf( "  %-3u  %-28s is a duplicate\n", id, name );
			++bad;
		}
		seen.emplace_back( name );
		for( unsigned int e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el != nullptr && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s\n", id, name, e, el );
				++bad;
			}
		}
	}
	std::printf( "\n  %d problem(s)\n", bad );
	return bad == 0 ? 0 : 1;
}

int runList()
{
	PatternPlugin plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s\n", "id", "name", "kind", "value", "range" );
	for( unsigned int id = 0; id < PT_COUNT_; ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= PT_ABOUT_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "about", "-", "-" );
			continue;
		}
		if( id == PT_AUDIO )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "buffer", "-", "-" );
			continue;
		}
		const char* kind = "standard";
		switch( plugin.GetParamType( id ) )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER: kind = "integer"; break;
		case FF_TYPE_OPTION: kind = "option"; break;
		case FF_TYPE_TEXT: kind = "text"; break;
		default: break;
		}
		RangeStruct range = plugin.GetParamRange( id );
		// An option's range reads back 0..1 whatever its element count (the
		// fleet trap), so the real one is printed for the sweep.
		if( plugin.GetParamType( id ) == FF_TYPE_OPTION )
		{
			range.min = 0.0f;
			range.max = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( id ) ) ) - 1.0f;
		}
		if( plugin.GetParamType( id ) == FF_TYPE_BOOLEAN )
		{
			range.min = 0.0f;
			range.max = 1.0f;
		}
		char rangeText[ 32 ] = {};
		std::snprintf( rangeText, sizeof( rangeText ), "[%g .. %g]", range.min, range.max );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runFont()
{
	std::printf( "the font\n\n" );
	int blank = 0, dup = 0;
	std::vector< std::pair< std::string, int > > seen;
	for( int code = 32; code < 127; ++code )
	{
		const char* const* g = font::Glyph( code );
		std::string        key;
		bool               lit = false;
		for( int y = 0; y < font::kHeight; ++y )
		{
			key += g[ y ];
			for( int x = 0; x < font::kWidth; ++x )
				lit = lit || g[ y ][ x ] == '#';
		}
		if( code != 32 && !lit )
		{
			std::printf( "  glyph %d ('%c') is blank\n", code, code );
			++blank;
		}
		for( const auto& s : seen )
			if( code != 32 && s.first == key )
			{
				std::printf( "  glyph %d ('%c') duplicates %d ('%c')\n", code, code, s.second, s.second );
				++dup;
			}
		seen.emplace_back( key, code );
	}
	// The characters a pattern screen actually draws, printed for a human.
	const char* used = "0123456789ABCDEF#-.|*?POSTBMD";
	for( const char* c = used; *c; ++c )
	{
		const char* const* g = font::Glyph( *c );
		std::printf( "  '%c'\n", *c );
		for( int y = 0; y < font::kHeight; ++y )
			std::printf( "      %s\n", g[ y ] );
	}
	std::printf( "\n  %d blank, %d duplicated\n", blank, dup );
	return ( blank == 0 && dup == 0 ) ? 0 : 1;
}

//---------------------------------------------------------------------------
// --grid
//---------------------------------------------------------------------------
namespace
{
/// Render `frames` frames of the drum pattern and hand back the last one,
/// the screen and the layout the plugin used for it.
Image renderGrid( PatternPlugin& p, Gl& gl, int frames, Screen& screen, Layout& layout )
{
	Synth beats;
	beats.kind = Synth::Beats;
	beats.bpm  = 125.0;
	float bins[ kBins ];
	Image img;
	for( int f = 0; f < frames; ++f )
	{
		beats.bins( f, bins );
		inject( p, bins );
		img = gl.frame( f / 60.0 );
	}
	screen = p.BuildScreen();
	layout = p.LayoutFor( gl.target.w, gl.target.h );
	return img;
}

struct GridResult
{
	long blocks = 0, badBlocks = 0;///< s x s blocks inside the screen area that are not one colour
	long outside = 0, badOutside = 0;///< pixels outside the screen area that are not the background
	int  glyphPixels = 0, badGlyphPixels = 0;///< pixels of the probed cells that are not the font's bit
	int  litExpected = 0;///< of those, the ones the table says are lit
};

GridResult measureGrid( const Image& img, const Screen& s, const Layout& l, const Palette& pal, int cursorScreenRow )
{
	GridResult r;
	const int  sc = l.scale;
	const int  W = img.w, H = img.h;
	const int  sx0 = l.originX, sy0 = l.originY;
	const int  sx1 = sx0 + l.screenW * sc, sy1 = sy0 + l.screenH * sc;

	// Every s x s block aligned to the origin, inside the screen and inside
	// the raster, is one colour. A block cut by the raster's edge is judged
	// on the part that is there.
	for( int by = std::max( sy0, sy0 + ( ( 0 - sy0 ) / sc ) * sc ); by < std::min( sy1, H ); by += sc )
	{
		if( by + sc <= 0 )
			continue;
		for( int bx = sx0 + std::max( 0, ( 0 - sx0 ) / sc ) * sc; bx < std::min( sx1, W ); bx += sc )
		{
			if( bx + sc <= 0 )
				continue;
			int fx = -1, fy = -1;
			bool ok = true;
			for( int y = std::max( by, 0 ); y < std::min( by + sc, H ) && ok; ++y )
				for( int x = std::max( bx, 0 ); x < std::min( bx + sc, W ); ++x )
				{
					if( fx < 0 )
					{
						fx = x;
						fy = y;
					}
					else if( !img.same( x, y, fx, fy ) )
					{
						ok = false;
						break;
					}
				}
			++r.blocks;
			r.badBlocks += !ok;
		}
	}

	// Outside the screen: the background, exactly (to the unorm rounding).
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			if( x >= sx0 && x < sx1 && y >= sy0 && y < sy1 )
				continue;
			++r.outside;
			r.badOutside += !img.is( x, y, pal.rgb[ PAL_BACK ] );
		}

	// Probed cells, compared with the font table bit for bit at this scale,
	// in the palette entries the screen says: every cell of the cursor row,
	// the row above it and the counter row whose whole block is on the
	// raster -- then any other row, for a Scale at which those are cropped
	// off -- up to a dozen spread across them. `litExpected` counts the
	// pixels the table says are lit, so a run of blank cells cannot pass this
	// by having nothing to draw.
	std::vector< std::pair< int, int > > candidates;
	std::vector< int >                   rowsToTry = { cursorScreenRow, cursorScreenRow - 1, 0 };
	for( int row = 0; row < s.rows; ++row )// then every other row, for a raster the cursor row misses
		if( std::find( rowsToTry.begin(), rowsToTry.end(), row ) == rowsToTry.end() )
			rowsToTry.push_back( row );
	for( int row : rowsToTry )
	{
		if( row < 0 || row >= s.rows )
			continue;
		if( candidates.size() >= 24 )
			break;
		for( int col = 0; col < s.cols; ++col )
		{
			const int x0 = sx0 + col * Screen::kCellW * sc, y0 = sy0 + row * Screen::kCellH * sc;
			if( x0 >= 0 && y0 >= 0 && x0 + Screen::kCellW * sc <= W && y0 + Screen::kCellH * sc <= H )
				candidates.emplace_back( col, row );
		}
	}
	const size_t step = std::max< size_t >( 1, candidates.size() / 12 );
	for( size_t i = 0; i < candidates.size(); i += step )
	{
		const int         col  = candidates[ i ].first, row = candidates[ i ].second;
		const ScreenCell& cell = s.At( col, row );
		for( int cy = 0; cy < Screen::kCellH; ++cy )
			for( int cx = 0; cx < Screen::kCellW; ++cx )
			{
				const bool   lit  = cx < font::kWidth && cy < font::kHeight && font::Bit( cell.glyph, cx, cy );
				const float* want = pal.rgb[ lit ? cell.fg : cell.bg ];
				for( int dy = 0; dy < sc; ++dy )
					for( int dx = 0; dx < sc; ++dx )
					{
						const int x = sx0 + ( col * Screen::kCellW + cx ) * sc + dx;
						const int y = sy0 + ( row * Screen::kCellH + cy ) * sc + dy;
						++r.glyphPixels;
						r.litExpected += lit;
						r.badGlyphPixels += !img.is( x, y, want );
					}
			}
	}
	return r;
}
} // namespace

int runGrid()
{
	std::printf( "every glyph on the whole-pixel grid, at every Scale, at two rasters\n\n"
				 "  at Scale s every s x s block aligned to the plugin's origin is one colour; outside the screen is\n"
				 "  the theme background; probed cells equal the font table's bits at that scale\n\n" );
	if( !openGL() )
		return 1;
	std::printf( "  GL %s\n\n", glGetString( GL_VERSION ) );

	for( const Raster& ra : kRasters )
		for( int scale : { 0, 1, 2, 3, 4 } )
			for( int channels : { 4, 8 } )
			{
				PatternPlugin p;
				p.ForceSecondsClock();
				p.SetBeatInfo( 125.0f, 0.0f );
				p.SetFloatParameter( PT_SCALE, static_cast< float >( scale ) );
				p.SetFloatParameter( PT_CHANNELS, channels == 8 ? 1.0f : 0.0f );
				p.SetFloatParameter( PT_SHOW_EFFECTS, channels == 8 ? 0.0f : 1.0f );
				p.SetFloatParameter( PT_THEME, static_cast< float >( scale % 3 ) );
				Gl     gl( p, ra.w, ra.h );
				Screen s;
				Layout l;
				const Image img = renderGrid( p, gl, 90, s, l );
				const GridResult r = measureGrid( img, s, l, PaletteFor( scale % 3 ), s.CursorRow( p.CurrentDisplay() ) );
				const std::string where = std::to_string( ra.w ) + "x" + std::to_string( ra.h ) + " Scale " +
										  ( scale == 0 ? "Auto(" + std::to_string( l.scale ) + ")" : std::to_string( scale ) ) +
										  ", " + std::to_string( channels ) + " ch";
				Check( r.badBlocks == 0 && r.blocks > 1000,
					   where + ": " + std::to_string( r.badBlocks ) + " of " + std::to_string( r.blocks ) + " blocks not one colour" );
				Check( r.badOutside == 0, where + ": " + std::to_string( r.badOutside ) + " of " + std::to_string( r.outside ) +
											  " pixels outside the screen not the background" );
				Check( r.badGlyphPixels == 0 && r.glyphPixels > 0 && r.litExpected > 0,
					   where + ": " + std::to_string( r.badGlyphPixels ) + " of " + std::to_string( r.glyphPixels ) +
						   " probed glyph pixels differ from the font table (" + std::to_string( r.litExpected ) + " lit)" );
			}

	// Auto picks the largest whole scale that fits, and the picture has notes
	// in it (the drum pattern wrote some), so the probes were not all blanks.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetBeatInfo( 125.0f, 0.0f );
		Gl     gl( p, 1280, 720 );
		Screen s;
		Layout l;
		renderGrid( p, gl, 90, s, l );
		const int fitW = 1280 / s.PixelWidth(), fitH = 720 / s.PixelHeight();
		Check( l.scale == std::min( fitW, fitH ) && l.scale >= 2,
			   "1280x720 Auto: scale " + std::to_string( l.scale ) + " is the largest that fits " + std::to_string( s.PixelWidth() ) +
				   "x" + std::to_string( s.PixelHeight() ) );
		Check( writtenCells( p.State(), 64, 4 ) > 8, "the drum pattern wrote notes (" + std::to_string( writtenCells( p.State(), 64, 4 ) ) + " cells)" );
		Check( ( 1280 - s.PixelWidth() * l.scale ) / 2 == l.originX, "and the screen is centred on a whole pixel" );
	}

	// A resize mid-run changes the layout and nothing in the pattern.
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetBeatInfo( 125.0f, 0.0f );
		Screen s;
		Layout l;
		std::vector< Cell > before;
		{
			Gl gl( p, 640, 360 );
			renderGrid( p, gl, 90, s, l );
			for( int r = 0; r < 64; ++r )
				for( int k = 0; k < 4; ++k )
					before.push_back( p.State().At( r, k ) );
		}
		bool same = true;
		{
			Gl gl( p, 320, 180 );
			injectSilence( p );
			gl.frame( 90 / 60.0 );
			size_t i = 0;
			for( int r = 0; r < 64; ++r )
				for( int k = 0; k < 4; ++k, ++i )
				{
					const Cell& c = p.State().At( r, k );
					same = same && c.written == before[ i ].written && c.note == before[ i ].note && c.volume == before[ i ].volume;
				}
			l = p.LayoutFor( 320, 180 );
		}
		Check( same && l.scale == 1, "resize 640x360 -> 320x180 mid-run keeps every cell (now Scale 1)" );
	}

	std::printf( "\n  negative control -- must FAIL:\n" );
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetBeatInfo( 125.0f, 0.0f );
		p.SetFloatParameter( PT_SCALE, 1.0f );
		p.debugFractionalScale = 1.5f;
		Gl     gl( p, 640, 360 );
		Screen s;
		Layout l;
		const Image      img = renderGrid( p, gl, 90, s, l );
		const GridResult r   = measureGrid( img, s, l, PaletteFor( 0 ), s.CursorRow( p.CurrentDisplay() ) );
		Check( r.badGlyphPixels > 0, "a fractional scale of 1.5 is rejected (" + std::to_string( r.badGlyphPixels ) +
										 " glyph pixels off the table, " + std::to_string( r.badBlocks ) + " blocks)" );
	}
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBench()
{
	if( !openGL() )
		return 1;
	std::printf( "ms/frame: five passes of 200 frames after a 30-frame warm-up, the drum pattern playing,\n"
				 "fastest pass quoted, spread beside it. glFinish on both sides.\n\n" );
	std::printf( "  %-12s %-10s %-14s %s\n", "raster", "ms/frame", "spread", "% of a 60 fps frame" );
	const Raster sizes[] = { { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
	for( const Raster& s : sizes )
	{
		PatternPlugin p;
		p.ForceSecondsClock();
		p.SetBeatInfo( 125.0f, 0.0f );
		Gl                  gl( p, s.w, s.h );
		ProcessOpenGLStruct pg = {};
		pg.HostFBO             = gl.target.fbo;
		Synth beats;
		beats.kind = Synth::Beats;
		beats.bpm  = 125.0;
		float bins[ kBins ];
		int   frame = 0;
		auto  one   = [ & ]() {
			beats.bins( frame, bins );
			inject( p, bins );
			p.SetTime( frame / 60.0 );
			p.ProcessOpenGL( &pg );
			++frame;
		};
		for( int f = 0; f < 30; ++f )
			one();
		glFinish();
		double best = 1e9, worst = 0.0;
		for( int pass = 0; pass < 5; ++pass )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < 200; ++f )
				one();
			glFinish();
			const double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / 200.0;
			best            = std::min( best, ms );
			worst           = std::max( worst, ms );
		}
		std::printf( "  %-12s %-10s %-14s %.1f%%\n", ( std::to_string( s.w ) + "x" + std::to_string( s.h ) ).c_str(),
					 F( best ).c_str(), ( F( best ) + "-" + F( worst ) ).c_str(), 100.0 * best / ( 1000.0 / 60.0 ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --out and --pipe share the audio sources and the cue script.
//---------------------------------------------------------------------------
namespace
{
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream                  file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int         lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int                frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string                word;
		while( in >> word )
			words.push_back( word );
		const std::string where = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
		if( words.empty() )
		{
			error = where;
			return {};
		}
		std::string  name;
		float        value  = 0.0f;
		const size_t equals = words.back().find( '=' );
		if( words.size() == 1 || equals != std::string::npos )
		{
			if( equals == std::string::npos )
			{
				error = where;
				return {};
			}
			value = std::strtof( words.back().substr( equals + 1 ).c_str(), nullptr );
			words.back().erase( equals );
		}
		else
		{
			value = std::strtof( words.back().c_str(), nullptr );
			words.pop_back();
		}
		for( const std::string& part : words )
			if( !part.empty() )
				name += name.empty() ? part : " " + part;
		if( name.empty() )
		{
			error = where;
			return {};
		}
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

struct RunOptions
{
	int         width = 640, height = 360;
	int         frames = 120;///< --pipe: 0 means until the reader hangs up
	double      fps    = 60.0;
	double      hostBpm  = 0.0;///< 0: leave the SDK's 120
	double      audioBpm = 0.0;///< the preset's tempo; 0 keeps the preset's 120
	double      audioUntil = 0.0;///< seconds after which the preset falls silent; 0 never
	std::string audio;         ///< a Synth preset
	std::string wavPath, spectrumPath, scriptPath, outPath;
	std::vector< std::pair< std::string, std::string > > sets;
	bool                                                 pipe = false;
};

/// Everything --out and --pipe share: the plugin set up, the audio source
/// chosen, the cue script bound. `perFrame( f )` writes the frame's bins and
/// cues into the plugin; returns false when the audio has run out.
struct Session
{
	PatternPlugin plugin;
	Synth         synth;
	bool          haveSynth = false;
	Wav           wav;
	bool          haveWav = false;
	std::vector< std::vector< float > > spectrum;
	std::map< unsigned int, Track >     automation;
	Track                               hostBpmTrack;
	std::map< int, Track >              binTracks;
	double                              hostBpm = 0.0;
	double                              fps     = 60.0;
	double                              audioUntil = 0.0;

	bool setUp( const RunOptions& o, std::string& error )
	{
		plugin.ForceSecondsClock();
		fps        = o.fps;
		hostBpm    = o.hostBpm;
		audioUntil = o.audioUntil;
		if( !o.audio.empty() )
		{
			if( !Synth::parse( o.audio, synth ) )
			{
				error = "unknown --audio preset '" + o.audio + "' (silence, noise, tone[:bin], metronome, beats, sweep)";
				return false;
			}
			synth.fps = o.fps;
			if( o.audioBpm > 0.0 )
				synth.bpm = o.audioBpm;
			haveSynth = true;
		}
		if( !o.wavPath.empty() )
		{
			if( !wav.load( o.wavPath, error ) )
				return false;
			haveWav = true;
			plugin.SetSampleRate( static_cast< unsigned int >( wav.rate ) );
		}
		if( !o.spectrumPath.empty() )
		{
			spectrum = loadSpectrum( o.spectrumPath, error );
			if( !error.empty() || spectrum.empty() )
			{
				if( error.empty() )
					error = "empty spectrum";
				return false;
			}
		}
		std::map< std::string, unsigned int > byName;
		for( unsigned int id = 0; id < PT_ABOUT_TEXT; ++id )
			if( id != PT_AUDIO )
				if( const char* name = plugin.GetParamName( id ) )
					byName[ name ] = id;
		for( const auto& kv : o.sets )
		{
			const auto found = byName.find( kv.first );
			if( found == byName.end() )
			{
				error = "no parameter named '" + kv.first + "'";
				return false;
			}
			plugin.SetFloatParameter( found->second, static_cast< float >( std::atof( kv.second.c_str() ) ) );
		}
		if( !o.scriptPath.empty() )
		{
			const auto tracks = loadScript( o.scriptPath, error );
			if( !error.empty() )
				return false;
			for( const auto& entry : tracks )
			{
				if( entry.first == "Host BPM" )
				{
					hostBpmTrack = entry.second;
					continue;
				}
				if( entry.first.rfind( "Bin ", 0 ) == 0 )
				{
					const int b = std::atoi( entry.first.c_str() + 4 );
					if( b < 0 || b >= kBins )
					{
						error = "the script names \"" + entry.first + "\"; bins are 0..63";
						return false;
					}
					binTracks[ b ] = entry.second;
					continue;
				}
				const auto found = byName.find( entry.first );
				if( found == byName.end() )
				{
					error = "the script names \"" + entry.first + "\", which is not an automatable parameter (try --list)";
					return false;
				}
				automation[ found->second ] = entry.second;
			}
		}
		return true;
	}

	/// The audio's own length in frames, or 0 for none.
	int audioFrames() const
	{
		if( haveWav )
			return static_cast< int >( std::ceil( wav.mono.size() / wav.rate * fps ) );
		if( !spectrum.empty() )
			return static_cast< int >( spectrum.size() );
		return 0;
	}

	void perFrame( int f )
	{
		for( const auto& track : automation )
			plugin.SetFloatParameter( track.first, valueAt( track.second, f ) );
		const double bpm = hostBpmTrack.empty() ? hostBpm : valueAt( hostBpmTrack, f );
		if( bpm > 0.0 )
			plugin.SetBeatInfo( static_cast< float >( bpm ), 0.0f );

		float bins[ kBins ] = {};
		if( haveWav )
			wav.bins( ( f + 1 ) / fps, bins );
		else if( !spectrum.empty() )
		{
			const auto& row = spectrum[ std::min< size_t >( static_cast< size_t >( f ), spectrum.size() - 1 ) ];
			for( int b = 0; b < kBins; ++b )
				bins[ b ] = row[ static_cast< size_t >( b ) ];
		}
		else if( haveSynth && ( audioUntil <= 0.0 || f / fps < audioUntil ) )
			synth.bins( f, bins );
		for( const auto& track : binTracks )
			bins[ track.first ] = valueAt( track.second, f );
		inject( plugin, bins );
	}
};
} // namespace

int runOut( const RunOptions& o )
{
	if( !openGL() )
		return 1;
	Session     s;
	std::string error;
	if( !s.setUp( o, error ) )
	{
		std::printf( "%s\n", error.c_str() );
		return 1;
	}
	Gl    gl( s.plugin, o.width, o.height );
	Image img;
	for( int f = 0; f < std::max( 1, o.frames ); ++f )
	{
		s.perFrame( f );
		img = gl.frame( f / o.fps );
	}
	if( !writePng( o.outPath, o.width, o.height, img.px ) )
	{
		std::printf( "could not write %s\n", o.outPath.c_str() );
		return 1;
	}
	return 0;
}

int runPipe( const RunOptions& o )
{
	// A reader that hangs up must end the take with exit 1 and a message,
	// not SIGPIPE's silent 141: write() then fails and the loop says so.
	std::signal( SIGPIPE, SIG_IGN );
	if( o.width <= 0 || o.height <= 0 || !( o.fps > 0.0 ) )
	{
		std::fprintf( stderr, "pntest: --pipe needs a positive size and --fps\n" );
		return 1;
	}
	if( !openGL() )
		return 1;
	Session     s;
	std::string error;
	if( !s.setUp( o, error ) )
	{
		std::fprintf( stderr, "pntest: %s\n", error.c_str() );
		return 1;
	}
	// --frames wins; otherwise the audio's own length; otherwise until the
	// reader hangs up.
	const int frames = o.frames > 0 ? o.frames : s.audioFrames();
	if( s.audioFrames() > 0 )
		std::fprintf( stderr, "pntest: %d frames of audio\n", s.audioFrames() );

	int status = 0;
	{
		Gl           gl( s.plugin, o.width, o.height );
		const size_t bytes = static_cast< size_t >( o.width ) * static_cast< size_t >( o.height ) * 4u;
		for( int f = 0; frames <= 0 || f < frames; ++f )
		{
			s.perFrame( f );
			const Image img     = gl.frame( f / o.fps );
			size_t      written = 0;
			while( written < bytes )
			{
				const ssize_t put = write( STDOUT_FILENO, img.px.data() + written, bytes - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			if( written < bytes )
			{
				std::fprintf( stderr, "pntest: the reader hung up after %d frame(s)\n", f );
				status = 1;
				break;
			}
		}
	}
	return status;
}

int main( int argc, char** argv )
{
	RunOptions o;
	bool       framesGiven = false;

	std::map< std::string, std::function< int() > > checks = {
		{ "--timing", runTiming }, { "--onset", runOnset },       { "--prime", runPrime }, { "--pitch", runPitch },
		{ "--ring", runRing },     { "--detected", runDetected }, { "--names", runNames }, { "--list", runList },
		{ "--groove", runGroove },
		{ "--font", runFont },     { "--grid", runGrid },         { "--bench", runBench },
	};

	std::string tempoWav;
	double      truth = 0.0, from = 4.0;
	bool        legacy = false;
	for( int a = 1; a < argc; ++a )
	{
		const std::string arg = argv[ a ];
		if( checks.count( arg ) )
			return checks[ arg ]();
		if( arg == "--tempo-wav" && a + 1 < argc )
			tempoWav = argv[ ++a ];
		else if( arg == "--truth" && a + 1 < argc )
			truth = std::atof( argv[ ++a ] );
		else if( arg == "--from" && a + 1 < argc )
			from = std::atof( argv[ ++a ] );
		else if( arg == "--legacy" )
			legacy = true;
		else if( arg == "--out" && a + 1 < argc )
			o.outPath = argv[ ++a ];
		else if( arg == "--size" && a + 1 < argc )
			std::sscanf( argv[ ++a ], "%dx%d", &o.width, &o.height );
		else if( arg == "--width" && a + 1 < argc )
			o.width = std::atoi( argv[ ++a ] );
		else if( arg == "--height" && a + 1 < argc )
			o.height = std::atoi( argv[ ++a ] );
		else if( arg == "--frames" && a + 1 < argc )
		{
			o.frames    = std::atoi( argv[ ++a ] );
			framesGiven = true;
		}
		else if( arg == "--fps" && a + 1 < argc )
			o.fps = std::atof( argv[ ++a ] );
		else if( arg == "--pipe" )
			o.pipe = true;
		else if( arg == "--audio" && a + 1 < argc )
			o.audio = argv[ ++a ];
		else if( arg == "--audio-bpm" && a + 1 < argc )
			o.audioBpm = std::atof( argv[ ++a ] );
		else if( arg == "--audio-until" && a + 1 < argc )
			o.audioUntil = std::atof( argv[ ++a ] );
		else if( arg == "--wav" && a + 1 < argc )
			o.wavPath = argv[ ++a ];
		else if( arg == "--spectrum" && a + 1 < argc )
			o.spectrumPath = argv[ ++a ];
		else if( arg == "--script" && a + 1 < argc )
			o.scriptPath = argv[ ++a ];
		else if( arg == "--host-bpm" && a + 1 < argc )
			o.hostBpm = std::atof( argv[ ++a ] );
		else if( arg == "--set" && a + 1 < argc )
		{
			const std::string kv = argv[ ++a ];
			const size_t      eq = kv.find( '=' );
			if( eq == std::string::npos )
			{
				std::printf( "--set wants Name=value\n" );
				return 1;
			}
			o.sets.emplace_back( kv.substr( 0, eq ), kv.substr( eq + 1 ) );
		}
	}

	if( !tempoWav.empty() )
		return runTempoWav( tempoWav, truth, o.fps, from, legacy );
	if( o.pipe )
	{
		if( !framesGiven )
			o.frames = 0;
		return runPipe( o );
	}
	if( !o.outPath.empty() )
	{
		if( o.audio.empty() )
			o.audio = "beats";
		return runOut( o );
	}

	std::printf( "usage: pntest --timing | --onset | --prime | --pitch | --ring | --detected | --groove\n"
				 "              --names | --list | --font | --grid | --bench\n"
				 "       pntest --tempo-wav f.wav [--truth BPM] [--from S] [--fps N] [--legacy]\n"
				 "       pntest --out f.png [--size WxH] [--frames N] [--fps N] [--audio PRESET]\n"
				 "              [--audio-bpm N] [--audio-until S] [--host-bpm N] [--set Name=value ...]\n"
				 "       pntest --pipe [--size WxH | --width W --height H] [--fps N] [--frames N]\n"
				 "              [--audio PRESET | --wav f.wav | --spectrum bins.txt] [--host-bpm N]\n"
				 "              [--script cues.txt] [--set Name=value ...]   raw RGBA on stdout\n"
				 "       PRESET: silence | noise | tone[:bin] | metronome | beats | sweep (--audio-bpm sets their tempo)\n" );
	return 2;
}
