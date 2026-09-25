#pragma once

#include <cstdint>

#include "Bands.h"

/**
	The tracker, run backwards: a row cursor on the tracker's own clock, and
	an onset detector that writes notes into the pattern as the music plays.

	No GL, no host, no time of its own. `Frame` takes the host's clock reading
	in seconds and the 64-bin spectrum, and everything the picture shows is
	published state read back from here. That is what lets `pntest` measure
	every claim with no rasteriser in the room.

	## The clock

	A row lasts `Speed x 2.5 / BPM` seconds: a tick is 2.5 / BPM s and a row
	is Speed ticks, which at the default Speed 6 is four rows to a beat. The
	row phase is kept as `base + ( t - anchor ) / T` in double: `anchor` is
	the host time at which the period was last changed and `base` the phase
	then, so at a constant tempo the row is a pure function of the host time
	with no accumulation and nothing to drift, and a tempo change carries the
	phase across rather than re-deriving it from the start of time. The
	subtraction is frame-relative on purpose: Resolume's clock has been
	measured near 499 million milliseconds, where a float resolves 0.03 s (a
	quarter of a row at 125 BPM) and even a double's 6e-11 s is only safe
	because it is used as a difference. `debug.floatPhase` is the negative
	control for that, and `pntest --timing` requires it to fail.

	The floor that turns a phase into a row carries a 1e-6 row allowance,
	copperlist's lesson: 1/60 s is not a double, so the frames that sit
	exactly on a row boundary arrive a few ULPs either side of it, and at any
	host rate no frame that is not ON a boundary is closer than 1/1800 row to
	one.

	Swing delays every odd row by a fraction of a row: within each pair of
	rows the even row runs from 0 to 1 + swing and the odd one from there to
	2, so the pair keeps its length and the beat stays where it was.

	## The ring

	Entering a row on a new pass clears it (every channel to `---`), unless
	Keep is on. Clearing is the default because it is what makes the picture
	honest: a sparse passage leaves dots, a busy one fills every row, and a
	change in the arrangement shows as the old notes being replaced rather
	than joined. With Keep on nothing is ever removed and the pattern fills
	to solid over a few passes. A jump of more than a whole pattern clears
	every row once.

	## Onsets

	Each channel owns a band of bins (Bands.h). Its onset function is
	half-wave-rectified spectral flux -- the sum over the band of every bin's
	rise since the last frame -- against an adaptive floor: a one-second
	one-pole mean of that flux, times the threshold ratio Sensitivity maps
	to. A note is written on a rising frame that crosses the threshold, with
	a 50 ms refractory per channel. Its pitch is the band's loudest bin,
	through the bin law, to the nearest tracker note; its sample number is
	the channel number; its `Cxx` volume is 64 x log2( flux / threshold ) / 3,
	clamped to 01..40 hex, so an onset just over the line is quiet and one
	eight times over it is full. The strongest onset in a row wins that row.

	## Frame one

	The fleet's trap: a detector that starts from zero reads every bin as
	risen from silence on its first frame, writes a note in every channel,
	and its floor -- snapped to that flux -- then makes it deaf for a second
	or more. So the first frame primes and steps nothing: the previous
	spectrum is set to the current one (flux zero), and each band's floor is
	seeded with an eighth of the band's LEVEL. A stationary signal's
	frame-to-frame flux is a few per cent of its level, so an eighth sits well
	above anything a held sound can produce; a real onset's flux is comparable
	to the level itself, so it clears the seeded threshold at once. (The first
	version seeded the whole level, and a clip that started on a kick drum was
	deaf for two seconds while it decayed.) The seed relaxes to the true mean
	over the one-second time constant. A repeated or reversed host time
	advances nothing either. `debug.noPrime` is the negative control; `pntest
	--prime` requires it to fail.

	## Detected tempo

	The onset envelope is kept in three registers rather than as one sum: LOW
	(bin centres below 400 Hz under the bin law: kick, bass), MID (400 Hz to
	5 kHz: snare, clap, most pitched parts) and HIGH (above: hats, ticks), each
	its own half-wave-rectified flux, resampled onto a 100 Hz grid. Once a
	second, over the last six seconds (from three), each register is smoothed
	(a Gaussian of 20 ms, which covers the one-frame jitter of an onset on a
	60 fps grid), standardised to zero mean and unit variance, weighted by
	how periodic it is on its own (its best autocorrelation at periods of
	0.3 to 2 s, clamped at zero, so a register holding only noise drops out) and
	by 1, 1 and 0.5, and summed. Standardising is the fix for v0.1.0's octave
	error: on the raw sum the kick's flux dwarfed the clap's, the envelope
	repeated at the half note, and the detector read 62 for 125.

	The combined envelope's autocorrelation R is then scored as a pulse with
	its grouping and, at half weight, its subdivision:

	    S( T ) = 0.5 R( T / 2 ) + R( T ) + R( 2 T )

	The best S under a log-Gaussian prior around 120 BPM (one octave wide)
	picks the FAMILY of periods, which rules out the 4/3 and 5/4 relatives a
	dense groove also correlates at. The metrical LEVEL within the family
	(x1/4 .. x4 inside 60..200 BPM) is the FASTEST member whose S is at least
	0.70 of the family's best, with no prior: a half-note period scores well
	because its subdivision is the beat, but so does the beat, and the beat is
	faster; an eighth-note period loses because its own subdivision, the
	sixteenth, is mostly empty. A pure pulse scores 0.8 of its half-tempo
	relative by construction; over every reading of `pntest --groove` the true
	beat scored at least 0.737 of its family's best and the double-time
	impostor at most 0.672. The lag is refined by a parabola on the peak of R
	at the largest multiple of it that fits (up to four), divided back down.
	A reading is published only when the one before it agrees within 2 %, so
	the first tempo appears at four seconds and a new one needs two readings
	in a row: a single short-window octave slip never re-anchors the clock.
	Until then, or while R at the chosen lag is under 0.2, the host's tempo
	stands in. `debug.legacyTempo` runs v0.1.0's
	detector, and `pntest --groove` requires it to fail.

	## VU

	A note sets its channel's meter to volume / 64 of 48 steps, and the meter
	falls one step every 20 ms -- a vblank-driven meter's rate, one pixel a
	field. Pure: the level is computed from the time it was set, not
	accumulated.
*/
namespace pattern
{

struct TrackerSettings
{
	int    tempoSource = 0;///< 0 host, 1 detected, 2 manual
	double manualBpm   = 125.0;
	double hostBpm     = 120.0;///< the last SetBeatInfo; 120 when the host never says
	int    speed       = 6;
	double swing       = 0.0;///< fraction of a row, 0..0.5
	int    rows        = 64;
	bool   keep        = false;
	int    channels    = 4;
	int    binLaw      = 0;///< Bands.h: 0 linear, 1 log
	int    binValue    = 0;///< 0 magnitude, 1 power (square root taken)
	double ratio       = 4.24;///< onset threshold over the running mean flux
	double gamma       = 2.2; ///< band split exponent
	bool   fold        = true;
	double sampleRate  = 44100.0;
};

/// The negative controls. None is reachable from a parameter.
struct TrackerDebug
{
	bool   floatPhase = false;///< the row phase in float, at the host's magnitude
	bool   noPrime    = false;///< the detector starts from zero on frame one
	bool   noClear    = false;///< a new pass never clears a row
	double lagBias    = 0.0;  ///< the detected lag scaled by (1 + bias)
	bool   legacyTempo = false;///< v0.1.0's detector: the raw summed flux, the best lag, a half at 70 %
	bool   levelDetector = false;///< fires on level, not change: no memory, no rising-edge test
	double detuneSemitones = 0.0;///< every pitch read this many semitones off
};

struct Cell
{
	int8_t   note     = -1;///< 0..35, or -1 for ---
	uint8_t  sample   = 0; ///< 1..8, or 0 for none
	uint8_t  volume   = 0; ///< 1..64 (the Cxx effect), or 0 for none
	bool     written  = false;
	uint32_t pass     = 0;
	double   strength = 0.0;
};

constexpr int    kMaxRows        = 64;
constexpr int    kMaxChannels    = 8;
constexpr int    kVuSteps        = 48;
constexpr double kVuFallSeconds  = 0.02;
constexpr double kRowAllowance   = 1e-6;///< rows; see the header
constexpr double kFloorSeconds   = 1.0; ///< the onset floor's time constant
constexpr double kPrimeFraction  = 0.125;///< of the band's level, the floor's seed on frame one
constexpr double kRefractory     = 0.05;///< seconds, per channel
constexpr double kAbsoluteFloor  = 1e-4;///< flux below this is never an onset
constexpr int    kTempoRate      = 100; ///< Hz, the onset envelope's grid
constexpr int    kTempoWindow    = 600; ///< samples: six seconds
constexpr int    kTempoLagMin    = 30;  ///< 200 BPM
constexpr int    kTempoLagMax    = 100; ///< 60 BPM
constexpr double kTempoSettleR   = 0.2; ///< normalised peak correlation to trust
constexpr int    kTempoBands     = 3;   ///< low, mid, high registers
constexpr double kTempoLowHz     = 400.0; ///< bin centres below: the low register
constexpr double kTempoHighHz    = 5000.0;///< bin centres at or above: the high register
constexpr double kTempoLevelRatio = 0.70;///< the fastest level scoring this share of the best wins
constexpr double kTempoPriorBpm  = 120.0;///< the family prior's centre
constexpr double kTempoPriorOct  = 1.0; ///< and its width, in octaves
constexpr int    kTempoMaxLag    = 302; ///< samples of autocorrelation kept (3 s)
constexpr double kTempoAgree     = 0.02;///< two readings in a row within this ratio publish

class Tracker
{
public:
	Tracker();

	void Reset();
	void SetSettings( const TrackerSettings& s );
	const TrackerSettings& Settings() const { return mSettings; }

	TrackerDebug debug;

	/// One host frame. `bins` may be shorter than 64; the rest read as zero.
	void Frame( double seconds, const float* bins, int count );

	// -- published state --------------------------------------------------
	bool    Primed() const { return mPrimed; }
	int64_t RowsElapsed() const { return mRowsTotal; }
	int     Row() const { return mRow; }
	int64_t Pass() const { return mPass; }
	double  Period() const { return mPeriod; }
	double  Bpm() const { return mBpm; }
	double  DetectedBpm() const { return mDetectedBpm; }
	double  TempoPeakR() const { return mPeakR; }
	/// The last detection's family, for the harness: each member's tempo and
	/// its score as a share of the family's best. The level chosen is the
	/// fastest with a share of at least kTempoLevelRatio.
	int     TempoMembers() const { return mMemberCount; }
	double  TempoMemberBpm( int i ) const { return mMemberBpm[ i ]; }
	double  TempoMemberShare( int i ) const { return mMemberShare[ i ]; }
	bool    TempoSettled() const { return mDetectedBpm > 0.0; }
	double  RawPhase() const { return mRawPhase; }
	int     BandEdge( int k ) const { return mEdges[ k ]; }
	int     Vu( int channel ) const;
	int     NotesWrittenLastFrame() const { return mWrittenLastFrame; }
	double  Flux( int channel ) const { return mFlux[ channel ]; }
	double  Floor( int channel ) const { return mFloor[ channel ]; }

	const Cell& At( int row, int channel ) const { return mCells[ row ][ channel ]; }

private:
	void   Prime( double seconds, const double* m );
	double CurrentBpm() const;
	void   ReanchorIfNeeded( double seconds );
	void   AdvanceRows( double seconds );
	void   Listen( double seconds, const double* m, double dt );
	void   FeedTempo( double seconds, const double* bandFlux );
	void   DetectTempo();
	void   DetectTempoLegacy( int n );
	void   ClearRow( int row );

	TrackerSettings mSettings;

	// The row clock.
	double  mBase      = 0.0;
	double  mAnchor    = 0.0;
	double  mPeriod    = 0.0;
	double  mBpm       = 0.0;
	double  mRawPhase  = 0.0;
	int64_t mRowsTotal = 0;
	int     mRow       = 0;
	int64_t mPass      = 0;

	// The detector.
	bool   mPrimed     = false;
	double mLastSeconds = 0.0;
	double mPrev[ bands::kBins ]  = {};
	double mFlux[ kMaxChannels ]  = {};
	double mFluxPrev[ kMaxChannels ] = {};
	double mFloor[ kMaxChannels ] = {};
	double mLastFire[ kMaxChannels ] = {};
	int    mEdges[ kMaxChannels + 1 ] = {};
	int    mWrittenLastFrame = 0;

	// The pattern.
	Cell mCells[ kMaxRows ][ kMaxChannels ];

	// The meters.
	int    mVuSet[ kMaxChannels ]  = {};
	double mVuTime[ kMaxChannels ] = {};
	double mNow = 0.0;

	// The tempo detector.
	int     mTempoBand[ bands::kBins ] = {};///< each bin's register, 0..2
	double  mEnv[ kTempoBands ][ 1024 ] = {};
	int64_t mEnvIndex      = -1;///< last grid index written
	double  mEnvOrigin     = 0.0;
	int64_t mLastDetectSec = -1;
	double  mDetectedBpm   = 0.0;
	double  mCandidateBpm  = 0.0;///< the last reading, published once the next agrees
	double  mPeakR         = 0.0;
	int     mMemberCount   = 0;
	double  mMemberBpm[ 5 ]   = {};
	double  mMemberShare[ 5 ] = {};
};

} // namespace pattern
