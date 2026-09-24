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
	mEnvIndex      = -1;
	mEnvOrigin     = 0.0;
	mLastDetectSec = -1;
	mDetectedBpm   = 0.0;
	mPeakR         = 0.0;
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
}

double Tracker::CurrentBpm() const
{
	double bpm = mSettings.hostBpm;
	switch( mSettings.tempoSource )
	{
	case 1:
		if( mDetectedBpm > 0.0 )
			bpm = mDetectedBpm;
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
	double onsetTotal = 0.0;
	const uint32_t pass = static_cast< uint32_t >( mPass );

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
		onsetTotal += flux;

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

	FeedTempo( seconds, onsetTotal );
}

void Tracker::FeedTempo( double seconds, double onset )
{
	const int64_t index = static_cast< int64_t >( std::floor( ( seconds - mEnvOrigin ) * kTempoRate ) );
	if( index < 0 )
		return;
	if( mEnvIndex < 0 )
	{
		mEnv[ index % kEnvSize ] = onset;
		mEnvIndex                = index;
	}
	else if( index > mEnvIndex )
	{
		// Hold the value across every grid cell the frame spanned, so a
		// 24 fps host and a 144 fps host produce the same envelope shape.
		const int64_t from = std::max( mEnvIndex + 1, index - kEnvSize );
		for( int64_t i = from; i <= index; ++i )
			mEnv[ i % kEnvSize ] = onset;
		mEnvIndex = index;
	}
	else
	{
		mEnv[ index % kEnvSize ] = std::max( mEnv[ index % kEnvSize ], onset );
	}

	const int64_t second = static_cast< int64_t >( std::floor( seconds - mEnvOrigin ) );
	if( second > mLastDetectSec )
	{
		mLastDetectSec = second;
		if( mEnvIndex + 1 >= 3 * kTempoRate )
			DetectTempo();
	}
}

void Tracker::DetectTempo()
{
	const int n = static_cast< int >( std::min< int64_t >( mEnvIndex + 1, kTempoWindow ) );
	double x[ kTempoWindow ];
	double mean = 0.0;
	for( int i = 0; i < n; ++i )
	{
		x[ i ] = mEnv[ ( mEnvIndex - n + 1 + i ) % kEnvSize ];
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

	// A pulse train correlates at every multiple of its period, so the
	// longest lag in range would otherwise win half the time. Prefer the
	// lag at half the best when it scores nearly as well.
	for( int half : { best / 2, ( best + 1 ) / 2 } )
		if( half >= kTempoLagMin && half < best && r[ half ] >= 0.7 * r[ best ] )
		{
			best = half;
			break;
		}

	if( r[ best ] < kTempoSettleR )
		return;

	// Parabolic refinement through the neighbours.
	double lag = best;
	if( best > kTempoLagMin - 1 && best < kTempoLagMax + 1 )
	{
		const double a = r[ best - 1 ], b = r[ best ], c = r[ best + 1 ];
		const double d = a - 2.0 * b + c;
		if( d < 0.0 )
			lag += 0.5 * ( a - c ) / d;
	}
	lag *= 1.0 + debug.lagBias;
	mPeakR       = r[ best ];
	mDetectedBpm = 60.0 * kTempoRate / lag;
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
		mEnvOrigin -= mLastSeconds - seconds;
		mLastSeconds = seconds;
	}

	const double dt = seconds - mLastSeconds;
	mLastSeconds    = seconds;
	mNow            = seconds;

	ReanchorIfNeeded( seconds );
	AdvanceRows( seconds );
	Listen( seconds, m, dt );
}

} // namespace pattern
