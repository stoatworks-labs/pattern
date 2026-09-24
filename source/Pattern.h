#pragma once

#include <string>

#include "Clock.h"
#include "Controls.h"
#include "Render.h"
#include "Screen.h"
#include "tracker/Tracker.h"

#include <FFGLSDK.h>

/**
	The plugin: a tracker's pattern editor, written by the music as it plays.

	This class is the wiring. Everything that decides anything is elsewhere
	and testable without a host, and without any GL at all:

	  `tracker/Bands`    bins to frequencies, frequencies to notes, bands
	  `tracker/Tracker`  the row clock, the ring, the onset detector, the
	                     tempo detector, the meters
	  `Screen`           the picture as a grid of cells, and where it sits
	  `Render`           two textures and one triangle into the host's FBO

	## Per frame

	`ProcessOpenGL` measures the host's clock unit (`Clock`), reads the
	spectrum out of the buffer parameter, hands both to `Advance`, composes
	the screen and draws it. `Advance` and `BuildScreen` are what the harness
	calls when it wants no GL involved; `LayoutFor` is how it learns where
	the plugin put the screen so a probe is never a transcription.

	## The host's tempo

	`SetBeatInfo` is the only way FFGL 2.1 tells a plugin the composition's
	BPM. The SDK defaults it to 120 and Resolume is understood to call it
	every frame; nothing here has measured how often, or whether `barPhase`
	is usable as a downbeat. The clock is rate-locked to the BPM and
	free-running in phase -- see AGENTS.md, "Open questions".
*/
namespace pattern
{
class PatternPlugin : public CFFGLPlugin
{
public:
	PatternPlugin();
	~PatternPlugin() override = default;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float    GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char*    GetTextParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;
	void     SetBeatInfo( float bpm, float barPhase ) override;
	void     SetSampleRate( unsigned int sampleRate ) override;

	/// The tracker's settings the current parameters mean.
	TrackerSettings CurrentSettings() const;
	DisplaySettings CurrentDisplay() const;

	/// Read the host's spectrum out of the buffer parameter, as the frame
	/// path does. Returns the number of bins found.
	int ReadSpectrum( float* bins, int capacity ) const;

	/// Advance the tracker one host frame at `hostSeconds`. `ProcessOpenGL`
	/// calls this; the harness calls it directly when it wants no GL.
	void Advance( double hostSeconds );

	/// Run the host-clock measurement for this frame and return the seconds
	/// it reads -- the first thing `ProcessOpenGL` does. For the harness, so
	/// the millisecond path is exercised with no GL context.
	double TickClock();

	/// The screen the next ProcessOpenGL would draw, and where it would put
	/// it on an output of this size. Both const and GL-free.
	Screen BuildScreen() const;
	Layout LayoutFor( int width, int height ) const;

	const Tracker& State() const { return mTracker; }
	Tracker&       StateForTest() { return mTracker; }

	/// For the harness, which sends seconds -- or milliseconds at the host's
	/// magnitude -- and wants no guessing.
	void ForceSecondsClock() { mClock.ForceSeconds(); }
	void ForceMillisecondsClock() { mClock.ForceMilliseconds(); }

	/// The --grid negative control: a fractional scale in the shader. 0 off.
	float debugFractionalScale = 0.0f;

private:
	int OptionIndex( unsigned int param, int count ) const;
	int Int( unsigned int param, int lo, int hi ) const;

	float mParams[ PT_COUNT_ ] = {};

	Renderer mRenderer;
	Clock    mClock;
	Tracker  mTracker;

	int         mInstanceId = 0;
	std::string mTag;

	bool   mGlReady      = false;
	bool   mHostTimeSeen = false;
	double mHostTime     = 0.0;
	double mHostBpm      = 120.0;
	double mSampleRate   = 44100.0;
};

} // namespace pattern
