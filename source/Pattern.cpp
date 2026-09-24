#include "Pattern.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Diag.h"

namespace pattern
{
static_assert( PT_COUNT_ - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
			   "the About block's size changed with the generated header -- "
			   "add or remove a PT_ABOUT_BUTTON_n to match" );

namespace
{
/// Turn a normalised option parameter back into an index. Resolume hands
/// option parameters back as the element *value*, which for these is the
/// index already, but a host that normalises would give 0..1 -- so both are
/// accepted and clamped. Getting this wrong silently selects mode 0 for ever.
int ToOption( float v, int count )
{
	if( count <= 1 )
		return 0;
	int i = ( v <= 1.0f && count > 2 && v != std::floor( v ) )
				? static_cast< int >( v * static_cast< float >( count - 1 ) + 0.5f )
				: static_cast< int >( v + 0.5f );
	return std::min( std::max( i, 0 ), count - 1 );
}

bool Bool( float v )
{
	return v > 0.5f;
}
} // namespace

PatternPlugin::PatternPlugin()
{
	static std::atomic< int > sNextInstance{ 1 };
	mInstanceId = sNextInstance.fetch_add( 1 );
	mTag        = "[" + std::to_string( mInstanceId ) + "] ";

	// A source: no inputs at all. Resolume decides where a plugin appears in
	// its browser from this and nothing else.
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	auto group = [ this ]( unsigned int from, unsigned int to, const char* name ) {
		for( unsigned int i = from; i <= to; ++i )
			SetParamGroup( i, name );
	};
	auto option = [ this ]( unsigned int id, const char* name, std::initializer_list< const char* > elements,
							float def ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( elements.size() ), def );
		unsigned int i = 0;
		for( const char* e : elements )
		{
			SetParamElementInfo( id, i, e, static_cast< float >( i ) );
			++i;
		}
		mParams[ id ] = def;
	};
	auto boolean = [ this ]( unsigned int id, const char* name, bool def ) {
		SetParamInfo( id, name, FF_TYPE_BOOLEAN, def );
		mParams[ id ] = def ? 1.0f : 0.0f;
	};
	auto standard = [ this ]( unsigned int id, const char* name, float def ) {
		SetParamInfo( id, name, FF_TYPE_STANDARD, def );
		mParams[ id ] = def;
	};
	auto integer = [ this ]( unsigned int id, const char* name, int def, int lo, int hi ) {
		// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so a count can be
		// declared as the count it is.
		SetParamInfo( id, name, FF_TYPE_INTEGER, static_cast< float >( def ) );
		SetParamRange( id, static_cast< float >( lo ), static_cast< float >( hi ) );
		mParams[ id ] = static_cast< float >( def );
	};

	// Declaration order is the order the host draws these, and SetParamGroup
	// collapses runs -- so an id moved out of its run splits its group in two.

	// -- Clock -----------------------------------------------------------------
	option( PT_TEMPO_SOURCE, "Tempo Source", { "Host", "Detected", "Manual" }, 0.0f );
	standard( PT_BPM, "BPM", kBpmDefault );
	integer( PT_SPEED, "Speed", kSpeedDefault, kSpeedMin, kSpeedMax );
	standard( PT_SWING, "Swing", 0.0f );
	option( PT_ROWS, "Rows", { "64", "32" }, 0.0f );
	boolean( PT_KEEP, "Keep Notes", false );
	group( PT_TEMPO_SOURCE, PT_KEEP, "Clock" );

	// -- Listening -------------------------------------------------------------
	option( PT_CHANNELS, "Channels", { "4", "8" }, 0.0f );
	option( PT_BIN_LAW, "Bin Law", { "Linear", "Log" }, 0.0f );
	option( PT_BIN_VALUE, "Bin Value", { "Magnitude", "Power" }, 0.0f );
	standard( PT_SENSITIVITY, "Sensitivity", kSensitivityDefault );
	standard( PT_BAND_SPLIT, "Band Split", kBandSplitDefault );
	boolean( PT_FOLD, "Fold Octaves", true );
	// Declared with a real element list so the host knows how many bins to
	// fill. It is the only parameter here the operator does not set.
	SetBufferParamInfo( PT_AUDIO, "Audio", bands::kBins, FF_USAGE_FFT );
	for( int i = 0; i < bands::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	group( PT_CHANNELS, PT_AUDIO, "Listening" );

	// -- Display ---------------------------------------------------------------
	option( PT_THEME, "Theme", { "Grey", "Blue", "Green" }, 0.0f );
	integer( PT_ROWS_VISIBLE, "Rows Visible", kRowsVisibleDefault, kRowsVisibleMin, kRowsVisibleMax );
	boolean( PT_SHOW_VU, "Show VU", true );
	boolean( PT_SHOW_EFFECTS, "Show Effects", true );
	integer( PT_SCALE, "Scale", kScaleDefault, kScaleMin, kScaleMax );
	group( PT_THEME, PT_SCALE, "Display" );

	// -- About -----------------------------------------------------------------
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	group( PT_ABOUT_TEXT, PT_COUNT_ - 1, "About" );

	mTracker.SetSettings( CurrentSettings() );
}

FFResult PatternPlugin::InitGL( const FFGLViewportStruct* vp )
{
	if( mGlReady )
		return CFFGLPlugin::InitGL( vp );

	diag::init();
	auto glString = []( GLenum name ) {
		const GLubyte* s = glGetString( name );
		return s != nullptr ? std::string( reinterpret_cast< const char* >( s ) ) : std::string( "?" );
	};
	diag::info( mTag + "GL vendor=" + glString( GL_VENDOR ) + " renderer=" + glString( GL_RENDERER ) +
				" version=" + glString( GL_VERSION ) );

	if( !mRenderer.InitGL() )
	{
		diag::error( mTag + "InitGL failed: " + mRenderer.Note() );
		return FF_FAIL;
	}
	mGlReady = true;
	return CFFGLPlugin::InitGL( vp );
}

FFResult PatternPlugin::DeInitGL()
{
	mRenderer.DeInitGL();
	mGlReady = false;
	return FF_SUCCESS;
}

FFResult PatternPlugin::SetTime( double time )
{
	mHostTimeSeen = true;
	mHostTime     = time;
	return CFFGLPlugin::SetTime( time );
}

void PatternPlugin::SetBeatInfo( float bpm, float barPhase )
{
	CFFGLPlugin::SetBeatInfo( bpm, barPhase );
	if( bpm > 0.0f && std::isfinite( bpm ) )
		mHostBpm = bpm;
}

void PatternPlugin::SetSampleRate( unsigned int sampleRate )
{
	CFFGLPlugin::SetSampleRate( sampleRate );
	if( sampleRate >= 8000 && sampleRate <= 384000 )
		mSampleRate = sampleRate;
}

FFResult PatternPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT_ )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	mParams[ index ] = value;
	return FF_SUCCESS;
}

float PatternPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT_ ? mParams[ index ] : 0.0f;
}

FFResult PatternPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// Must return FF_SUCCESS for the About block, or no host can instantiate
	// the plugin at all: the base class fails an unknown text parameter, and
	// a host that sets one during instantiation treats that as the plugin
	// refusing to load.
	(void)value;
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return FF_FAIL;
}

char* PatternPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return const_cast< char* >( "" );
}

int PatternPlugin::OptionIndex( unsigned int param, int count ) const
{
	return ToOption( mParams[ param ], count );
}

int PatternPlugin::Int( unsigned int param, int lo, int hi ) const
{
	return std::min( std::max( static_cast< int >( std::lround( mParams[ param ] ) ), lo ), hi );
}

TrackerSettings PatternPlugin::CurrentSettings() const
{
	TrackerSettings s;
	s.tempoSource = OptionIndex( PT_TEMPO_SOURCE, kTempoSourceCount );
	s.manualBpm   = BpmFromControl( mParams[ PT_BPM ] );
	s.hostBpm     = mHostBpm;
	s.speed       = Int( PT_SPEED, kSpeedMin, kSpeedMax );
	s.swing       = SwingFromControl( mParams[ PT_SWING ] );
	s.rows        = OptionIndex( PT_ROWS, 2 ) == 1 ? 32 : 64;
	s.keep        = Bool( mParams[ PT_KEEP ] );
	s.channels    = OptionIndex( PT_CHANNELS, 2 ) == 1 ? 8 : 4;
	s.binLaw      = OptionIndex( PT_BIN_LAW, kBinLawCount );
	s.binValue    = OptionIndex( PT_BIN_VALUE, kBinValueCount );
	s.ratio       = ThresholdRatio( mParams[ PT_SENSITIVITY ] );
	s.gamma       = SplitExponent( mParams[ PT_BAND_SPLIT ] );
	s.fold        = Bool( mParams[ PT_FOLD ] );
	s.sampleRate  = mSampleRate;
	return s;
}

DisplaySettings PatternPlugin::CurrentDisplay() const
{
	DisplaySettings d;
	d.rowsVisible = Int( PT_ROWS_VISIBLE, kRowsVisibleMin, kRowsVisibleMax );
	d.showVu      = Bool( mParams[ PT_SHOW_VU ] );
	d.showEffects = Bool( mParams[ PT_SHOW_EFFECTS ] );
	return d;
}

int PatternPlugin::ReadSpectrum( float* bins, int capacity ) const
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return 0;
	const int n = std::min( capacity, static_cast< int >( info->elements.size() ) );
	for( int i = 0; i < n; ++i )
		bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	return n;
}

double PatternPlugin::TickClock()
{
	mClock.Tick( mHostTime, mHostTimeSeen );
	return mClock.Seconds();
}

void PatternPlugin::Advance( double hostSeconds )
{
	mTracker.SetSettings( CurrentSettings() );
	float     bins[ bands::kBins ] = {};
	const int n                    = ReadSpectrum( bins, bands::kBins );
	mTracker.Frame( hostSeconds, bins, n );
}

Screen PatternPlugin::BuildScreen() const
{
	return Compose( mTracker, CurrentDisplay() );
}

Layout PatternPlugin::LayoutFor( int width, int height ) const
{
	return pattern::LayoutFor( BuildScreen(), width, height, Int( PT_SCALE, kScaleMin, kScaleMax ) );
}

FFResult PatternPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL == nullptr || !mGlReady )
		return FF_FAIL;

	const double seconds = TickClock();
	diag::stateChanged( mTag + "clock", mTag + "host clock is " + mClock.Unit() );

	const int width  = static_cast< int >( currentViewport.width );
	const int height = static_cast< int >( currentViewport.height );
	if( width <= 0 || height <= 0 )
		return FF_SUCCESS;

	Advance( seconds );

	// A pattern that never fills is the commonest support question there
	// is, and from the front it is indistinguishable from a broken plugin.
	{
		float bins[ bands::kBins ] = {};
		const int n = ReadSpectrum( bins, bands::kBins );
		bool any = false;
		for( int i = 0; i < n; ++i )
			any = any || bins[ i ] > 1e-6f;
		diag::stateChanged( mTag + "audio", mTag + ( any ? "audio present on the layer"
														  : "no audio on this layer -- every FFT bin is zero" ) );
		const TrackerSettings& ts = mTracker.Settings();
		diag::stateChanged( mTag + "tempo", mTag + "tempo source " + std::to_string( ts.tempoSource ) + ", " +
												std::to_string( static_cast< int >( std::lround( mTracker.Bpm() ) ) ) +
												" BPM, speed " + std::to_string( ts.speed ) );
	}

	const Screen screen = BuildScreen();
	const Layout layout = pattern::LayoutFor( screen, width, height, Int( PT_SCALE, kScaleMin, kScaleMax ) );
	mRenderer.Draw( screen, layout, PaletteFor( OptionIndex( PT_THEME, kThemeCount ) ), width, height, pGL->HostFBO,
					debugFractionalScale );
	return FF_SUCCESS;
}

} // namespace pattern
