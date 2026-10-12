#include "Tracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pattern
{
namespace
{
constexpr int kEnvSize = 1024;

double Clamp( double v, double lo, double hi )
{
	return std::min( std::max( v, lo ), hi );
}

/// Each bin's tempo register from its centre frequency under the bin law:
/// 0 low, 1 mid, 2 high. The registers are the detector's own and do not
/// move with Channels or Band Split.
void TempoBands( int law, double sampleRate, int* out )
{
	for( int j = 0; j < bands::kBins; ++j )
	{
		const double hz = bands::BinFrequency( j, law, sampleRate );
		out[ j ]        = hz < kTempoLowHz ? 0 : ( hz < kTempoHighHz ? 1 : 2 );
	}
}

/// The autocorrelation of x (zero mean) at `lag`, unbiased (each lag's sum
/// over its own overlap) and normalised by the lag-0 value.
double AutoCorr( const double* x, int n, int lag, double power )
{
	double acc = 0.0;
	for( int i = 0; i + lag < n; ++i )
		acc += x[ i ] * x[ i + lag ];
	return acc / static_cast< double >( n - lag ) / power;
}

/// The largest value of R, linearly interpolated, within one sample of the
/// fractional lag t: a period that falls between grid samples is not
/// penalised for it. Zero when the window runs past the lags computed.
double PeakNear( const double* r, int maxLag, double t )
{
	const int lo = static_cast< int >( std::floor( t - 1.0 ) );
	const int hi = static_cast< int >( std::ceil( t + 1.0 ) );
	if( lo < 0 || hi + 1 > maxLag )
		return 0.0;
	double best = -1e300;
	for( int q = 4 * lo; q <= 4 * hi; ++q )
	{
		const int    i = q >> 2;
		const double f = ( q & 3 ) * 0.25;
		best           = std::max( best, r[ i ] * ( 1.0 - f ) + r[ i + 1 ] * f );
	}
	return best;
}
} // namespace

Tracker::Tracker()
{
	Reset();
}

void Tracker::Reset()
{
	mBase = mAnchor = 0.0;
	mPeriod = mBpm = mRawPhase = 0.0;
	mRowsTotal = 0;
	mRow       = 0;
	mPass      = 0;
	mPrimed    = false;
	mLastSeconds = 0.0;
	std::memset( mPrev, 0, sizeof( mPrev ) );
	std::memset( mFlux, 0, sizeof( mFlux ) );
	std::memset( mFluxPrev, 0, sizeof( mFluxPrev ) );
	std::memset( mFloor, 0, sizeof( mFloor ) );
	std::memset( mLastFire, 0, sizeof( mLastFire ) );
	mWrittenLastFrame = 0;
	for( auto& row : mCells )
		for( auto& c : row )
			c = Cell{};
	std::memset( mVuSet, 0, sizeof( mVuSet ) );
	std::memset( mVuTime, 0, sizeof( mVuTime ) );
	mNow = 0.0;
	std::memset( mEnv, 0, sizeof( mEnv ) );
	TempoBands( mSettings.binLaw, mSettings.sampleRate, mTempoBand );
	mEnvIndex      = -1;
	mEnvOrigin     = 0.0;
	mLastDetectSec = -1;
	mDetectedBpm   = 0.0;
	mCandidateBpm  = 0.0;
	mLastPublishAt = 0.0;
	mLastSoundAt   = 0.0;
	mPublished     = false;
	mRunCount      = 0;
	mConfirmedBpm  = 0.0;
	mLastConfirmAt = 0.0;
	mLastConfirmFast = false;
	mLastAgreeAt   = 0.0;
	mShortPrev     = 0.0;
	mFastBpm       = 0.0;
	mFastUntil     = -1e300;
	mSoundBackAt   = -1e300;
	mPeakR         = 0.0;
	mMemberCount   = 0;
	bands::BandEdges( mSettings.channels, mSettings.binLaw, mSettings.gamma, mEdges );
}

void Tracker::SetSettings( const TrackerSettings& s )
{
	mSettings          = s;
	mSettings.rows     = std::min( std::max( s.rows, 1 ), kMaxRows );
	mSettings.channels = std::min( std::max( s.channels, 1 ), kMaxChannels );
	mSettings.speed    = std::max( s.speed, 1 );
	mSettings.swing    = Clamp( s.swing, 0.0, 0.5 );
	bands::BandEdges( mSettings.channels, mSettings.binLaw, mSettings.gamma, mEdges );
	TempoBands( mSettings.binLaw, mSettings.sampleRate, mTempoBand );
}

double Tracker::CurrentBpm() const
{
	double bpm = mSettings.hostBpm;
	switch( mSettings.tempoSource )
	{
	case 1:
		if( mSettings.whenLost == 1 )
		{
			// When Lost = Default: the clock runs on what Tempo Out sends.
			const double target = TempoTarget();
			if( target > 0.0 )
				bpm = target;
		}
		else if( mDetectedBpm > 0.0 )
			bpm = mDetectedBpm;// Hold: exactly the v0.1.1 clock
		break;
	case 2:
		bpm = mSettings.manualBpm;
		break;
	default:
		break;
	}
	// A host that never calls SetBeatInfo leaves the SDK's 120; one that
	// sends nonsense is not allowed to stop the clock or spin it.
	if( !( bpm >= 20.0 && bpm <= 999.0 ) )
		bpm = 120.0;
	return bpm;
}

bool Tracker::TempoLive() const
{
	const double since = debug.unconfirmedTempo ? mLastPublishAt : mLastConfirmAt;
	// A confirmation made before a silence that lost the tempo stays dead
	// when the sound comes back: the music after a silence is new music.
	const bool beforeSilence = !debug.staleRevival && since < mSoundBackAt;
	return ConfirmedBpm() > 0.0 && !beforeSilence && mNow - since <= kTempoLostSeconds &&
		   mNow - mLastSoundAt <= kTempoSilenceSeconds;
}

bool Tracker::TempoCurrent() const
{
	return TempoLive() && mNow - std::max( mLastAgreeAt, mLastConfirmAt ) <= kTempoCurrentSeconds;
}

double Tracker::TempoTarget() const
{
	if( TempoLive() )
		return ConfirmedBpm();
	if( mSettings.whenLost == 1 && mSettings.fallbackBpm > 0.0 )
		return mSettings.fallbackBpm;
	return 0.0;// Hold: nothing new
}

void Tracker::Confirm()
{
	const auto agrees = []( double a, double b ) { return b > 0.0 && std::fabs( a / b - 1.0 ) <= kTempoAgree; };
	if( mFastBpm > 0.0 )
	{
		// The three-second window has agreed with itself on a clear change.
		mConfirmedBpm    = mFastBpm;
		mLastConfirmAt   = mNow;
		mLastConfirmFast = true;
		mFastUntil       = mNow + kTempoFastHoldSeconds;
		mRunCount        = 0;
		return;
	}
	// A cycle that did not publish, or published on little evidence, or with
	// no sound to speak of, breaks the run.
	const bool qualifies = mPublished && mPeakR >= kTempoTrustR && mNow - mLastSoundAt <= kTempoQuietSeconds;
	if( !qualifies )
	{
		mRunCount = 0;
		return;
	}
	const bool hold = mNow < mFastUntil;
	if( agrees( mDetectedBpm, mConfirmedBpm ) )
	{
		mLastAgreeAt = mNow;
		if( hold )
		{
			// For a while after a fast re-lock the six-second window still
			// holds the old music, and what it publishes is in between. It
			// cannot move the tempo: a publication that agrees keeps the
			// re-lock live and clears the run of anything in between ...
			mLastConfirmAt = mNow;
			mRunCount      = 0;
			return;
		}
	}
	// The run: the latest consecutive publications that agree within 2 %,
	// up to eight (v0.2.0: three). Dropping the oldest while they disagree
	// keeps the longest run that agrees, so a value from before a change
	// leaves it as soon as the newest disagrees with it.
	const int cap = debug.averageThree ? kTempoConfirmRun : kTempoAverageMax;
	if( mRunCount == cap )
	{
		for( int i = 0; i + 1 < mRunCount; ++i )
			mRun[ i ] = mRun[ i + 1 ];
		--mRunCount;
	}
	mRun[ mRunCount++ ] = mDetectedBpm;
	for( ;; )
	{
		double lo = mRun[ 0 ], hi = mRun[ 0 ];
		for( int i = 1; i < mRunCount; ++i )
		{
			lo = std::min( lo, mRun[ i ] );
			hi = std::max( hi, mRun[ i ] );
		}
		if( hi / lo - 1.0 <= kTempoAgree )
			break;
		for( int i = 0; i + 1 < mRunCount; ++i )
			mRun[ i ] = mRun[ i + 1 ];
		--mRunCount;
	}
	// ... and one that disagrees joins a run that cannot confirm until the
	// hold is over: if the window says something else for the whole hold,
	// the re-lock was wrong and that run confirms the moment the hold ends,
	// while the re-lock is still live.
	if( mRunCount < kTempoConfirmRun || hold )
		return;
	double sum = 0.0;
	for( int i = 0; i < mRunCount; ++i )
		sum += mRun[ i ];
	mConfirmedBpm    = sum / mRunCount;
	mLastConfirmAt   = mNow;
	mLastConfirmFast = false;
}

int Tracker::Vu( int channel ) const
{
	if( channel < 0 || channel >= kMaxChannels || mVuSet[ channel ] <= 0 )
		return 0;
	const double elapsed = mNow - mVuTime[ channel ];
	if( elapsed < 0.0 )
		return mVuSet[ channel ];
	const int64_t fallen = static_cast< int64_t >( std::floor( elapsed / kVuFallSeconds ) );
	return static_cast< int >( std::max< int64_t >( 0, mVuSet[ channel ] - fallen ) );
}

void Tracker::Prime( double seconds, const double* m )
{
	mAnchor = seconds;
	mBase   = 0.0;
	mBpm    = CurrentBpm();
	mPeriod = mSettings.speed * 2.5 / mBpm;
	mLastSeconds = seconds;
	mNow         = seconds;
	mEnvOrigin   = seconds;
	mEnvIndex    = -1;
	mLastPublishAt = seconds;
	mLastSoundAt   = seconds;
	mLastConfirmAt = seconds;
	mLastAgreeAt   = seconds;
	mRunCount      = 0;
	mShortPrev     = 0.0;
	mFastUntil     = -1e300;
	mSoundBackAt   = -1e300;

	if( debug.noPrime )
	{
		// The fleet's old habit, kept as the negative control: the previous
		// spectrum is silence and every floor is zero, so the first real
		// frame reads every bin as an onset and the floor snaps to it.
		std::memset( mPrev, 0, sizeof( mPrev ) );
		std::memset( mFloor, 0, sizeof( mFloor ) );
	}
	else
	{
		std::memcpy( mPrev, m, sizeof( mPrev ) );
		for( int k = 0; k < mSettings.channels; ++k )
		{
			double level = 0.0;
			for( int j = mEdges[ k ]; j < mEdges[ k + 1 ]; ++j )
				level += m[ j ];
			mFloor[ k ] = level * kPrimeFraction;
		}
	}
	for( int k = 0; k < kMaxChannels; ++k )
	{
		mFluxPrev[ k ] = 0.0;
		mLastFire[ k ] = seconds - 1.0;
	}
	mPrimed = true;
	// Frame one owns row zero of pass zero; nothing has been entered yet, so
	// nothing is cleared.
	mRawPhase  = 0.0;
	mRowsTotal = 0;
	mRow       = 0;
	mPass      = 0;
}

void Tracker::ReanchorIfNeeded( double seconds )
{
	const double bpm    = CurrentBpm();
	const double period = mSettings.speed * 2.5 / bpm;
	if( period == mPeriod )
	{
		mBpm = bpm;
		return;
	}
	// Carry the phase across at this instant, under the OLD period, then
	// start counting under the new one from here.
	mBase   = mBase + ( seconds - mAnchor ) / mPeriod;
	mAnchor = seconds;
	mPeriod = period;
	mBpm    = bpm;
}

void Tracker::AdvanceRows( double seconds )
{
	double raw;
	if( debug.floatPhase )
	{
		const float f = static_cast< float >( mBase ) +
						( static_cast< float >( seconds ) - static_cast< float >( mAnchor ) ) /
							static_cast< float >( mPeriod );
		raw = static_cast< double >( f );
	}
	else
	{
		raw = mBase + ( seconds - mAnchor ) / mPeriod;
	}
	mRawPhase = raw;

	const double  r      = raw + kRowAllowance;
	const double  pairs  = std::floor( r * 0.5 );
	const double  within = r - 2.0 * pairs;
	const int64_t total  = 2 * static_cast< int64_t >( pairs ) + ( within >= 1.0 + mSettings.swing ? 1 : 0 );

	if( total > mRowsTotal )
	{
		const int64_t entered = total - mRowsTotal;
		if( !mSettings.keep && !debug.noClear )
		{
			if( entered >= mSettings.rows )
			{
				for( int row = 0; row < mSettings.rows; ++row )
					ClearRow( row );
			}
			else
			{
				for( int64_t t = mRowsTotal + 1; t <= total; ++t )
					ClearRow( static_cast< int >( t % mSettings.rows ) );
			}
		}
	}
	mRowsTotal = std::max< int64_t >( total, 0 );
	mRow       = static_cast< int >( mRowsTotal % mSettings.rows );
	mPass      = mRowsTotal / mSettings.rows;
}

void Tracker::ClearRow( int row )
{
	for( int k = 0; k < kMaxChannels; ++k )
		mCells[ row ][ k ] = Cell{};
}

void Tracker::Listen( double seconds, const double* m, double dt )
{
	mWrittenLastFrame = 0;
	const uint32_t pass = static_cast< uint32_t >( mPass );

	// The tempo detector's registers read every bin, whatever the channels.
	double bandFlux[ kTempoBands ] = {};
	for( int j = 0; j < bands::kBins; ++j )
		bandFlux[ mTempoBand[ j ] ] += std::max( 0.0, m[ j ] - mPrev[ j ] );

	for( int k = 0; k < mSettings.channels; ++k )
	{
		double flux = 0.0;
		int    peak = mEdges[ k ];
		for( int j = mEdges[ k ]; j < mEdges[ k + 1 ]; ++j )
		{
			flux += std::max( 0.0, m[ j ] - mPrev[ j ] );
			if( m[ j ] > m[ peak ] )
				peak = j;
		}
		mFlux[ k ] = flux;

		const double threshold = std::max( kAbsoluteFloor, mFloor[ k ] * mSettings.ratio );
		const bool   rising    = debug.levelDetector || flux > mFluxPrev[ k ];
		const bool   armed     = seconds - mLastFire[ k ] >= kRefractory;
		if( flux > threshold && rising && armed )
		{
			const double strength = flux / threshold;
			const int    volume   = static_cast< int >( Clamp( std::lround( 64.0 * std::log2( strength ) / 3.0 ), 1.0, 64.0 ) );
			const double hz       = bands::BinFrequency( peak, mSettings.binLaw, mSettings.sampleRate ) *
							  std::pow( 2.0, debug.detuneSemitones / 12.0 );
			const int    note     = bands::TrackerNote( bands::MidiFromFrequency( hz ), mSettings.fold );

			Cell& cell = mCells[ mRow ][ k ];
			if( !cell.written || cell.pass != pass || strength > cell.strength )
			{
				cell.note     = static_cast< int8_t >( note );
				cell.sample   = static_cast< uint8_t >( k + 1 );
				cell.volume   = static_cast< uint8_t >( volume );
				cell.written  = true;
				cell.pass     = pass;
				cell.strength = strength;
				++mWrittenLastFrame;
			}
			mVuSet[ k ]  = static_cast< int >( std::lround( kVuSteps * volume / 64.0 ) );
			mVuTime[ k ] = seconds;
			mLastFire[ k ] = seconds;
		}

		// The floor follows the flux over one second. A zero interval leaves
		// it where it is rather than snapping it, which is the other half of
		// the priming trap.
		if( dt > 0.0 )
		{
			const double a = 1.0 - std::exp( -dt / kFloorSeconds );
			mFloor[ k ] += ( flux - mFloor[ k ] ) * a;
		}
		mFluxPrev[ k ] = flux;
	}
	if( !debug.levelDetector )
		std::memcpy( mPrev, m, sizeof( mPrev ) );

	FeedTempo( seconds, bandFlux );
}

void Tracker::FeedTempo( double seconds, const double* bandFlux )
{
	const int64_t index = static_cast< int64_t >( std::floor( ( seconds - mEnvOrigin ) * kTempoRate ) );
	if( index < 0 )
		return;
	for( int b = 0; b < kTempoBands; ++b )
	{
		double* env = mEnv[ b ];
		if( mEnvIndex < 0 )
			env[ index % kEnvSize ] = bandFlux[ b ];
		else if( index > mEnvIndex )
		{
			// Hold the value across every grid cell the frame spanned, so a
			// 24 fps host and a 144 fps host produce the same envelope shape.
			const int64_t from = std::max( mEnvIndex + 1, index - kEnvSize );
			for( int64_t i = from; i <= index; ++i )
				env[ i % kEnvSize ] = bandFlux[ b ];
		}
		else
			env[ index % kEnvSize ] = std::max( env[ index % kEnvSize ], bandFlux[ b ] );
	}
	mEnvIndex = std::max( mEnvIndex, index );

	const int64_t second = static_cast< int64_t >( std::floor( seconds - mEnvOrigin ) );
	if( second > mLastDetectSec )
	{
		mLastDetectSec = second;
		if( mEnvIndex + 1 >= 3 * kTempoRate )
		{
			mPublished = false;
			if( debug.legacyTempo )
				DetectTempoLegacy( static_cast< int >( std::min< int64_t >( mEnvIndex + 1, kTempoWindow ) ) );
			else
				DetectTempo();
			Confirm();
		}
	}
}

void Tracker::DetectTempo()
{
	const int     n = static_cast< int >( std::min< int64_t >( mEnvIndex + 1, kTempoWindow ) );
	const Reading t = Estimate( n );
	// The fast re-lock is judged against the tempo as it stood before this
	// cycle (nothing above has touched it).
	FastRelock( t );
	if( t.status == Reading::Silent )
		return;
	if( t.status == Reading::Weak )
	{
		mCandidateBpm = 0.0;
		return;
	}
	const auto agrees  = []( double a, double b ) { return b > 0.0 && std::fabs( a / b - 1.0 ) <= kTempoAgree; };
	const bool publish = agrees( t.bpm, mCandidateBpm ) || agrees( t.bpm, mDetectedBpm );
	mCandidateBpm      = t.bpm;
	mPeakR             = t.peak;
	mMemberCount       = t.members;
	for( int i = 0; i < t.members; ++i )
	{
		mMemberBpm[ i ]   = t.memberBpm[ i ];
		mMemberShare[ i ] = t.memberShare[ i ];
	}
	if( publish )
	{
		mDetectedBpm   = t.bpm;
		mLastPublishAt = mNow;
		mPublished     = true;
	}
}

void Tracker::FastRelock( const Reading& wide )
{
	mFastBpm = 0.0;
	if( debug.noFastRelock || !( mConfirmedBpm > 0.0 ) || !TempoLive() )
	{
		mShortPrev = 0.0;
		return;
	}
	// The evidence the short window needs is stricter than the long one's:
	// on the long window's own thresholds (R 0.2, 2 %, `debug.laxFastRelock`)
	// a beat turning into noise sends a false tempo in 18 of 72 runs.
	const double  fastR     = debug.laxFastRelock ? kTempoSettleR : kTempoFastR;
	const double  fastAgree = debug.laxFastRelock ? kTempoAgree : kTempoFastAgree;
	const Reading s         = Estimate( kTempoShortWindow );
	const bool    good = s.status == Reading::Read && s.peak >= fastR && mNow - mLastSoundAt <= kTempoQuietSeconds;
	if( good && mShortPrev > 0.0 && std::fabs( s.bpm / mShortPrev - 1.0 ) <= fastAgree )
	{
		const double ratio   = 0.5 * ( s.bpm + mShortPrev ) / mConfirmedBpm;
		bool         related = false;
		for( double k : { 0.5, 2.0 / 3.0, 1.5, 2.0 } )
			related = related || std::fabs( ratio / k - 1.0 ) <= kTempoRelatedSlack;
		if( std::fabs( ratio - 1.0 ) > kTempoFastJump && !related )
		{
			// The value. If the six-second window already agrees with the
			// newest short reading, the change is old enough that all three
			// readings are of the new music: their mean, which halves the
			// short window's scatter (a few tenths at 30 fps). If it does not,
			// the change is recent, the earlier short window straddles it and
			// reads in between: the newest alone.
			const bool wideAgrees = wide.status == Reading::Read && std::fabs( wide.bpm / s.bpm - 1.0 ) <= kTempoFastAgree;
			mFastBpm              = wideAgrees ? ( s.bpm + mShortPrev + wide.bpm ) / 3.0 : s.bpm;
		}
	}
	mShortPrev = good ? s.bpm : 0.0;
}

Tracker::Reading Tracker::Estimate( int n ) const
{
	Reading out;
	constexpr double kWeight[ kTempoBands ] = { 1.0, 1.0, 0.5 };
	constexpr int    kSmooth                = 6;  ///< kernel half-width, samples
	constexpr double kSigma                 = 2.0;///< samples: 20 ms
	constexpr int    kOwnLagMax             = 200;///< a register's own periodicity: lags 30..200, 0.3 to 2 s

	double kernel[ 2 * kSmooth + 1 ];
	for( int k = -kSmooth; k <= kSmooth; ++k )
		kernel[ k + kSmooth ] = std::exp( -0.5 * ( k / kSigma ) * ( k / kSigma ) );

	double x[ kTempoWindow ] = {};
	double raw[ kTempoWindow ], z[ kTempoWindow ];
	for( int b = 0; b < kTempoBands; ++b )
	{
		for( int i = 0; i < n; ++i )
			raw[ i ] = mEnv[ b ][ ( mEnvIndex - n + 1 + i ) % kEnvSize ];
		// Smooth, renormalising the kernel where it runs off either end.
		double mean = 0.0;
		for( int i = 0; i < n; ++i )
		{
			double acc = 0.0, wsum = 0.0;
			for( int k = -kSmooth; k <= kSmooth; ++k )
				if( i + k >= 0 && i + k < n )
				{
					acc += kernel[ k + kSmooth ] * raw[ i + k ];
					wsum += kernel[ k + kSmooth ];
				}
			z[ i ] = acc / wsum;
			mean += z[ i ];
		}
		mean /= n;
		double power = 0.0;
		for( int i = 0; i < n; ++i )
		{
			z[ i ] -= mean;
			power += z[ i ] * z[ i ];
		}
		power /= n;
		if( !( power > 1e-18 ) )
			continue;
		// How periodic this register is on its own: its best autocorrelation
		// over the tempo range and below. Noise scores near zero and drops out.
		double own = 0.0;
		const int ownMax = std::min( kOwnLagMax, n - 60 );
		for( int lag = kTempoLagMin; lag <= ownMax; ++lag )
			own = std::max( own, AutoCorr( z, n, lag, power ) );
		const double w = kWeight[ b ] * own / std::sqrt( power );
		for( int i = 0; i < n; ++i )
			x[ i ] += w * z[ i ];
	}

	double power = 0.0;
	for( int i = 0; i < n; ++i )
		power += x[ i ] * x[ i ];
	power /= n;
	if( !( power > 1e-18 ) )
		return out;
	const int maxLag = std::min( kTempoMaxLag, n - 60 );
	double    r[ kTempoMaxLag + 2 ] = {};
	for( int lag = 0; lag <= maxLag; ++lag )
		r[ lag ] = AutoCorr( x, n, lag, power );

	// A pulse, its grouping, and (at half weight) its subdivision.
	auto score = [ & ]( double t ) {
		return 0.5 * PeakNear( r, maxLag, 0.5 * t ) + PeakNear( r, maxLag, t ) + PeakNear( r, maxLag, 2.0 * t );
	};
	// The family: the best score under the prior, on a quarter-sample grid.
	double family = kTempoLagMin, bestScore = -1e300;
	for( int q = 4 * kTempoLagMin; q <= 4 * kTempoLagMax; ++q )
	{
		const double t     = 0.25 * q;
		const double octs  = std::log2( 60.0 * kTempoRate / t / kTempoPriorBpm ) / kTempoPriorOct;
		const double prior = std::exp( -0.5 * octs * octs );
		const double s     = score( t ) * prior;
		if( s > bestScore )
		{
			bestScore = s;
			family    = t;
		}
	}
	// The level: the fastest member whose score is within the ratio of the
	// best member's, with no prior.
	double members[ 5 ], scores[ 5 ], top = -1e300;
	int    count = 0;
	for( int j = -2; j <= 2; ++j )
	{
		const double t = family * std::ldexp( 1.0, j );
		if( t < kTempoLagMin || t > kTempoLagMax )
			continue;
		members[ count ] = t;
		scores[ count ]  = score( t );
		top              = std::max( top, scores[ count ] );
		++count;
	}
	double best = family;
	for( int i = 0; i < count; ++i )// ascending lag, so the first to pass is the fastest
		if( scores[ i ] >= kTempoLevelRatio * top )
		{
			best = members[ i ];
			break;
		}

	const double peak = PeakNear( r, maxLag, best );
	if( peak < kTempoSettleR )
	{
		out.status = Reading::Weak;
		return out;
	}

	// Refine on the peak of R at the largest multiple that fits (up to 4):
	// a parabola through the grid samples, divided back down.
	const int m    = std::max( 1, std::min( 4, static_cast< int >( ( maxLag - 3 ) / best ) ) );
	const double c = m * best;
	const int hw   = std::max( 1, static_cast< int >( best / 8.0 ) );
	const int lo   = std::max( 1, static_cast< int >( std::floor( c - hw ) ) );
	const int hi   = std::min( maxLag - 1, static_cast< int >( std::ceil( c + hw ) ) );
	int       at   = lo;
	for( int lag = lo; lag <= hi; ++lag )
		if( r[ lag ] > r[ at ] )
			at = lag;
	double lag = at;
	{
		const double a = r[ at - 1 ], b = r[ at ], cc = r[ at + 1 ];
		const double d = a - 2.0 * b + cc;
		if( d < 0.0 )
			lag += 0.5 * ( a - cc ) / d;
	}
	lag /= m;
	lag *= 1.0 + debug.lagBias;
	out.status  = Reading::Read;
	out.bpm     = 60.0 * kTempoRate / lag;
	out.peak    = peak;
	out.members = count;
	for( int i = 0; i < count; ++i )
	{
		out.memberBpm[ i ]   = 60.0 * kTempoRate / members[ i ];
		out.memberShare[ i ] = scores[ i ] / top;
	}
	return out;
}

/// v0.1.0's detector, kept only as the harness's negative control: the raw
/// sum of every bin's flux, the best autocorrelation lag in range, and the
/// half of it when it scores 70 % as well. `pntest --groove` requires it to fail.
void Tracker::DetectTempoLegacy( int n )
{
	double x[ kTempoWindow ];
	double mean = 0.0;
	for( int i = 0; i < n; ++i )
	{
		const int64_t at = ( mEnvIndex - n + 1 + i ) % kEnvSize;
		x[ i ]           = mEnv[ 0 ][ at ] + mEnv[ 1 ][ at ] + mEnv[ 2 ][ at ];
		mean += x[ i ];
	}
	mean /= n;
	double energy = 0.0;
	for( int i = 0; i < n; ++i )
	{
		x[ i ] -= mean;
		energy += x[ i ] * x[ i ];
	}
	if( !( energy > 0.0 ) )
		return;

	double r[ kTempoLagMax + 2 ] = {};
	for( int lag = kTempoLagMin - 1; lag <= kTempoLagMax + 1; ++lag )
	{
		double acc = 0.0;
		for( int i = 0; i + lag < n; ++i )
			acc += x[ i ] * x[ i + lag ];
		r[ lag ] = acc / energy;
	}
	int best = kTempoLagMin;
	for( int lag = kTempoLagMin; lag <= kTempoLagMax; ++lag )
		if( r[ lag ] > r[ best ] )
			best = lag;
	for( int half : { best / 2, ( best + 1 ) / 2 } )
		if( half >= kTempoLagMin && half < best && r[ half ] >= 0.7 * r[ best ] )
		{
			best = half;
			break;
		}
	if( r[ best ] < kTempoSettleR )
		return;
	double lag = best;
	if( best > kTempoLagMin - 1 && best < kTempoLagMax + 1 )
	{
		const double a = r[ best - 1 ], b = r[ best ], c = r[ best + 1 ];
		const double d = a - 2.0 * b + c;
		if( d < 0.0 )
			lag += 0.5 * ( a - c ) / d;
	}
	lag *= 1.0 + debug.lagBias;
	mPeakR         = r[ best ];
	mDetectedBpm   = 60.0 * kTempoRate / lag;
	mLastPublishAt = mNow;
	mPublished     = true;
}

void Tracker::Frame( double seconds, const float* bins, int count )
{
	double m[ bands::kBins ];
	for( int j = 0; j < bands::kBins; ++j )
	{
		const float v = ( bins != nullptr && j < count ) ? bins[ j ] : 0.0f;
		// A host is entitled to hand back a negative or a NaN in a buffer it
		// has not filled yet; either would poison the floors for good.
		const double clean = ( v > 0.0f ) ? static_cast< double >( v ) : 0.0;
		m[ j ]             = mSettings.binValue == 1 ? std::sqrt( clean ) : clean;
	}

	if( !mPrimed )
	{
		Prime( seconds, m );
		return;
	}

	// A host time that went backwards is a loop point or a seek: carry the
	// phase across and let no time pass on this frame.
	if( seconds < mLastSeconds )
	{
		mBase   = mBase + ( mLastSeconds - mAnchor ) / mPeriod;
		mAnchor = seconds;
		for( int k = 0; k < kMaxChannels; ++k )
		{
			mVuTime[ k ] -= mLastSeconds - seconds;
			mLastFire[ k ] -= mLastSeconds - seconds;
		}
		const double back = mLastSeconds - seconds;
		mEnvOrigin -= back;
		mLastPublishAt -= back;
		mLastSoundAt -= back;
		mLastConfirmAt -= back;
		mLastAgreeAt -= back;
		mFastUntil -= back;
		mSoundBackAt -= back;
		mLastSeconds = seconds;
	}

	const double dt = seconds - mLastSeconds;
	mLastSeconds    = seconds;
	mNow            = seconds;

	for( int j = 0; j < bands::kBins; ++j )
		if( m[ j ] > kSoundLevel )
		{
			// The end of a silence long enough to have lost the tempo.
			if( seconds - mLastSoundAt > kTempoSilenceSeconds )
				mSoundBackAt = seconds;
			mLastSoundAt = seconds;
			break;
		}

	ReanchorIfNeeded( seconds );
	AdvanceRows( seconds );
	Listen( seconds, m, dt );
}

} // namespace pattern
