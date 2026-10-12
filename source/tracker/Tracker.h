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

	## Confirmed, live and lost (Tempo Out)

	A publication is good enough for the row clock, which a wrong tempo only
	makes look wrong. It is not good enough to set a show's master tempo, and
	two facts measured here say so:

	- **Noise publishes.** Steady filtered noise rendered as audio (72 runs of
	  60 s: twelve seeds, low-, high- and band-pass, 30 and 60 fps) published
	  a tempo in most runs, at peak correlations of 0.20 to 0.37, and twice
	  confirmed itself into a run of three.
	- **A window that straddles a change publishes something in between.**
	  A metronome at 120 turning into noise published 117.72 at R 0.475 as the
	  window filled; a metronome going silent drifted from 140 to 141.2.
	  Confirmation stops those two, which end in no beat. It does NOT stop
	  the in-between values of a change from one beat to another: they move a
	  little each second, each within 2 % of the last, so they confirm one
	  after another (v0.2.0 sent a cut from 124 to 128 as 125.0, 127.1 and
	  127.7). The fast re-lock below is the answer to that.

	So Tempo Out trusts a CONFIRMED tempo: three publications in three
	consecutive detection cycles (a second apart), each with R at least
	`kTempoTrustR` (0.3) and each made with sound in the last second, all
	within the 2 % agreement. The tempo is the mean of the RUN: the latest
	consecutive publications that agree within 2 %, up to
	`kTempoAverageMax` (8), so a value that crosses the window as the music
	changes drops out as soon as it disagrees with the newest. Over the same
	72 noise runs that is 0 confirmations (5 at R 0.2); every one of the 45
	asserted grooves and the release video's soundtrack confirm, the
	soundtrack from 6 s. The cost is two seconds: a steady beat is first
	confirmed at about 6 s, not 4. (The breakbeat and the house groove at
	160 on 30 fps never confirm, and a confirmation is still at whatever
	metrical level the detector chose: house at 80 and 90 confirm 160 and
	180, as they read.) `debug.unconfirmedTempo` trusts every publication
	instead, and `pntest --send` requires noise to get through with it.

	Why up to eight and not three (v0.2.1): a settled reading wobbles by a
	few tenths, and at 30 fps a backbeat at 128 alternated 127.7 / 128.4
	from one reading to the next, so the mean of three crossed Tempo Out's
	0.5 BPM line every few seconds and re-sent 127.8, 128.4, 127.8 ... for
	as long as it played (29 of 150 steady runs re-sent). The mean of up to
	eight sits still. `debug.averageThree` is v0.2.0's mean of three.

	## A change, once a tempo is live (v0.2.1)

	The six-second window is slow to follow a change: it holds the old music
	for six seconds, and a window straddling the change publishes values in
	between, which a run of three then confirms one after another (a hard
	cut from 124 to 128 was sent as 125.0, 127.1, 127.7, and stayed there).
	So while a tempo is live, each detection cycle also reads the last THREE
	seconds (`Estimate( kTempoShortWindow )`), and a change is confirmed at
	once when two of those readings in a row
	  - agree within `kTempoFastAgree` (1 %),
	  - each have R at least `kTempoFastR` (0.5) and sound in the last second,
	  - average more than `kTempoFastJump` (2 %) from the live tempo,
	  - and the jump is not x1/2, x2/3, x3/2 or x2 (within 3 %): an octave or
	    a triplet relative is the metrical-level question the six-second
	    window and its run of three answer, and keeps the slow path.
	The value: when the six-second reading of the same cycle agrees with the
	newer short reading within 1 %, the change is old enough that all three
	readings are of the new music, and it is their mean (a single short
	reading scatters by a few tenths of a BPM); when it does not, the change
	is recent, the earlier short window straddles it and reads in between,
	and the newer short reading stands alone. For `kTempoFastHoldSeconds` (6)
	afterwards the six-second window, still holding the old music, cannot
	move the tempo: a publication that agrees with it only keeps it live and
	clears the run, and one that disagrees joins a run that may not confirm
	until the hold is over -- so a wrong re-lock is undone at the end of the
	hold, before it could go stale. Measured over the house-124 change set
	(`pntest --follow`): a hard cut is confirmed and sent 3 to 4 s after it,
	not 5 to 8, as one message. `debug.noFastRelock` is v0.2.0's slow path,
	and `debug.laxFastRelock` the fast path on the six-second window's
	thresholds (R 0.2, 2 %), on which a beat turning into noise sends a false
	tempo in 18 of 72 runs.

	The tempo is LIVE while confirmations keep coming (one within
	`kTempoLostSeconds`) and there has been sound within
	`kTempoSilenceSeconds`; once confirmed and no longer live it is LOST. A
	silence long enough to lose it ends that confirmation for good: when the
	sound comes back the tempo stays lost until a new confirmation (v0.2.0
	let one from before the silence come back to life, so When Lost =
	Default sent the old tempo again for a moment; `debug.staleRevival`).
	The header shows `*` only while live, and `?` otherwise.

	`TempoCurrent()` is narrower than live, for Tempo Out's status row: live,
	and a qualifying publication has agreed with the confirmed tempo within
	`kTempoCurrentSeconds`. After a cut to music that never confirms, the
	row stops saying the old tempo is OK within a few seconds rather than
	after the eight it takes to be lost.

	`TempoTarget()` is what Tempo Out sends: the confirmed tempo while live;
	otherwise Default BPM when When Lost says Default, else 0 -- Hold sends
	nothing new, so Resolume keeps the last tempo it was given. Detected's
	row clock follows `TempoTarget()` under Default; under Hold, the default,
	it is exactly the v0.1.1 clock (the latest publication, the host's tempo
	before one).

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
	double fallbackBpm = 120.0;///< Default BPM
	int    whenLost    = 0;    ///< 0 hold, 1 go to fallbackBpm
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
	bool   unconfirmedTempo = false;///< Tempo Out trusts every publication, not a confirmed run
	// v0.2.0's rules, one at a time, for the negative controls of the v0.2.1 checks.
	bool   noFastRelock = false;///< no three-second re-lock: a change waits for the six-second window
	bool   averageThree = false;///< the confirmed tempo is the mean of the last three, not up to eight
	bool   staleRevival = false;///< a confirmation from before a silence comes back with the sound
	bool   laxFastRelock = false;///< the fast re-lock on the six-second window's thresholds (R 0.2, 2 %)
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
constexpr int    kTempoConfirmRun     = 3;   ///< publications in consecutive cycles to confirm
constexpr double kTempoTrustR         = 0.3; ///< each with at least this peak correlation
constexpr double kTempoQuietSeconds   = 1.0; ///< and sound no longer ago than this
constexpr double kTempoLostSeconds    = 8.0; ///< without a confirmation: lost
constexpr double kTempoSilenceSeconds = 2.0; ///< of silence: lost
constexpr double kSoundLevel          = 1e-6;///< a bin above this is sound (the log's own test)
constexpr int    kTempoAverageMax      = 8;   ///< the confirmed tempo: the mean of up to this many agreeing publications
constexpr int    kTempoShortWindow     = 300; ///< samples: the fast re-lock reads the last three seconds
constexpr double kTempoFastR           = 0.5; ///< each short reading's peak correlation, at least
constexpr double kTempoFastAgree       = 0.01;///< two short readings in a row within this ratio
constexpr double kTempoFastJump        = 0.02;///< and their mean more than this ratio from the live tempo
constexpr double kTempoRelatedSlack    = 0.03;///< a jump this close to x1/2, x2/3, x3/2 or x2 keeps the slow path
constexpr double kTempoFastHoldSeconds = 6.0; ///< after a fast re-lock the six-second window cannot pull it back
constexpr double kTempoCurrentSeconds  = 3.0; ///< TempoCurrent(): a publication agreed with the tempo this recently

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
	/// The last confirmed tempo (see "Confirmed, live and lost"), or 0.
	double  ConfirmedBpm() const { return debug.unconfirmedTempo ? mDetectedBpm : mConfirmedBpm; }
	/// Confirmations coming and sound playing: the header's `*`.
	bool    TempoLive() const;
	/// Confirmed once, and no longer live.
	bool    TempoLost() const { return ConfirmedBpm() > 0.0 && !TempoLive(); }
	/// Live, and what the detector hears now still agrees with it: Tempo
	/// Out's status row says OK only while this holds.
	bool    TempoCurrent() const;
	/// What Tempo Out sends; 0 for nothing.
	double  TempoTarget() const;
	/// The last confirmation came from the three-second window.
	bool    FastRelocked() const { return mLastConfirmFast; }
	double  LastPublishAt() const { return mLastPublishAt; }
	double  LastConfirmAt() const { return mLastConfirmAt; }
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

	/// One reading of the last `n` envelope samples.
	struct Reading
	{
		enum Status
		{
			Silent,///< no signal in the window: nothing to say
			Weak,  ///< R at the chosen lag under kTempoSettleR
			Read   ///< `bpm` is a reading
		};
		Status status = Silent;
		double bpm    = 0.0;
		double peak   = 0.0;
		int    members = 0;
		double memberBpm[ 5 ]   = {};
		double memberShare[ 5 ] = {};
	};
	Reading Estimate( int n ) const;
	void    DetectTempo();
	void    FastRelock( const Reading& wide );
	void    DetectTempoLegacy( int n );
	void    Confirm();
	void    ClearRow( int row );

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
	double  mLastPublishAt = 0.0;///< seconds: the last publication, a new tempo or the same again
	double  mLastSoundAt   = 0.0;///< seconds: the last frame with any bin above kSoundLevel
	bool    mPublished     = false;///< this detection cycle published
	double  mRun[ kTempoAverageMax ] = {};///< the run: consecutive agreeing publications, oldest first
	int     mRunCount      = 0;
	double  mConfirmedBpm  = 0.0;
	double  mLastConfirmAt = 0.0;
	bool    mLastConfirmFast = false;///< the last confirmation was the fast re-lock's
	double  mLastAgreeAt   = 0.0;///< a qualifying publication last agreed with the confirmed tempo
	double  mShortPrev     = 0.0;///< the last short reading that qualified, or 0
	double  mFastBpm       = 0.0;///< this cycle's fast re-lock, or 0
	double  mFastUntil     = -1e300;///< until then the six-second window cannot pull the tempo back
	double  mSoundBackAt   = -1e300;///< sound came back after a silence that lost the tempo
	double  mPeakR         = 0.0;
	int     mMemberCount   = 0;
	double  mMemberBpm[ 5 ]   = {};
	double  mMemberShare[ 5 ] = {};
};

} // namespace pattern
