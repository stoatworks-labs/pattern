// This file must not include FFGLSDK.h or anything that does: on Windows the
// SDK's headers reach windows.h, which drags in the old winsock.h, and
// winsock2.h after that is a redefinition storm. OscSend.h is plain C++.
#include "OscSend.h"

#if defined( _WIN32 )
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
	#include <arpa/inet.h>
	#include <cerrno>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <sys/socket.h>
	#include <unistd.h>
using socket_t = int;
#endif

#include <atomic>
#include <cmath>
#include <cstring>

namespace pattern
{
namespace osc
{
namespace
{
void PutString( std::vector< uint8_t >& out, const char* text )
{
	const size_t n = std::strlen( text );
	const size_t w = Padded( n );
	const size_t at = out.size();
	out.resize( at + w, 0 );// every pad byte, and the terminator, is NUL
	std::memcpy( out.data() + at, text, n );
}

void PutFloat( std::vector< uint8_t >& out, float value )
{
	uint32_t bits = 0;
	static_assert( sizeof( bits ) == sizeof( value ), "float32" );
	std::memcpy( &bits, &value, sizeof( bits ) );
	// OSC is big-endian whatever the machine is.
	out.push_back( static_cast< uint8_t >( bits >> 24 ) );
	out.push_back( static_cast< uint8_t >( bits >> 16 ) );
	out.push_back( static_cast< uint8_t >( bits >> 8 ) );
	out.push_back( static_cast< uint8_t >( bits ) );
}

std::string LastSocketError( const char* what )
{
#if defined( _WIN32 )
	return std::string( what ) + " failed (WSA error " + std::to_string( WSAGetLastError() ) + ")";
#else
	return std::string( what ) + " failed: " + std::strerror( errno );
#endif
}
} // namespace

size_t Padded( size_t textLength )
{
	// The "+ 4" supplies the mandatory terminator: a string whose length is
	// already a multiple of four still gets four NULs.
	return ( textLength + 4 ) & ~static_cast< size_t >( 3 );
}

std::vector< uint8_t > AbsoluteFloat( const char* address, float value )
{
	std::vector< uint8_t > out;
	out.reserve( 64 );
	PutString( out, address );
	PutString( out, ",sf" );
	PutString( out, "a" );
	PutFloat( out, value );
	return out;
}

std::vector< uint8_t > PlainFloat( const char* address, float value )
{
	std::vector< uint8_t > out;
	out.reserve( 64 );
	PutString( out, address );
	PutString( out, ",f" );
	PutFloat( out, value );
	return out;
}

UdpSender::~UdpSender()
{
	Close();
}

void UdpSender::Close()
{
	if( mSocket != -1 )
	{
#if defined( _WIN32 )
		::closesocket( static_cast< socket_t >( mSocket ) );
#else
		::close( static_cast< socket_t >( mSocket ) );
#endif
		mSocket = -1;
	}
#if defined( _WIN32 )
	if( mStarted )
	{
		::WSACleanup();
		mStarted = false;
	}
#endif
}

bool UdpSender::Send( int port, const std::vector< uint8_t >& packet )
{
	if( port <= 0 || port > 65535 || packet.empty() )
	{
		mError = "no port to send to";
		return false;
	}
	if( mSocket == -1 )
	{
#if defined( _WIN32 )
		if( !mStarted )
		{
			// Reference-counted by Windows and paired with WSACleanup in
			// Close; never from DllMain, which is why it is here.
			WSADATA data;
			if( ::WSAStartup( MAKEWORD( 2, 2 ), &data ) != 0 )
			{
				mError = "WSAStartup failed";
				return false;
			}
			mStarted = true;
		}
		const socket_t s = ::socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		if( s == INVALID_SOCKET )
		{
			mError = LastSocketError( "socket()" );
			return false;
		}
		u_long nonBlocking = 1;
		::ioctlsocket( s, FIONBIO, &nonBlocking );
		// Not inherited by anything Resolume starts.
		::SetHandleInformation( reinterpret_cast< HANDLE >( s ), HANDLE_FLAG_INHERIT, 0 );
		mSocket = static_cast< std::intptr_t >( s );
#else
		const socket_t s = ::socket( AF_INET, SOCK_DGRAM, 0 );
		if( s < 0 )
		{
			mError = LastSocketError( "socket()" );
			return false;
		}
		::fcntl( s, F_SETFD, FD_CLOEXEC );
		::fcntl( s, F_SETFL, ::fcntl( s, F_GETFL, 0 ) | O_NONBLOCK );
		mSocket = static_cast< std::intptr_t >( s );
#endif
	}

	sockaddr_in to{};
	to.sin_family      = AF_INET;
	to.sin_port        = htons( static_cast< uint16_t >( port ) );
	to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );

#if defined( _WIN32 )
	const int sent = ::sendto( static_cast< socket_t >( mSocket ), reinterpret_cast< const char* >( packet.data() ),
							   static_cast< int >( packet.size() ), 0, reinterpret_cast< const sockaddr* >( &to ),
							   static_cast< int >( sizeof( to ) ) );
	const bool ok = sent == static_cast< int >( packet.size() );
#else
	const ssize_t sent = ::sendto( static_cast< socket_t >( mSocket ), packet.data(), packet.size(), 0,
								   reinterpret_cast< const sockaddr* >( &to ), sizeof( to ) );
	const bool ok = sent == static_cast< ssize_t >( packet.size() );
#endif
	if( !ok )
	{
		mError = LastSocketError( "sendto()" );
		return false;
	}
	mError.clear();
	return true;
}
} // namespace osc

// ---------------------------------------------------------------------------
// TempoSender
// ---------------------------------------------------------------------------
namespace
{
/// The instance that owns the tempo (0 for none), and when it last ran a
/// frame, in milliseconds of the caller's monotonic clock. Plain atomics in
/// the bundle: no thread_local, nothing that would pin the image.
std::atomic< int >     sOwner{ 0 };
std::atomic< int64_t > sOwnerBeatMs{ 0 };
/// Whether the owner's target is a detected tempo rather than Default BPM.
std::atomic< bool > sOwnerLive{ false };
/// The last instance to hold the job, still set after it let go: a sender
/// that gets the job back can tell whether anyone else had it meanwhile.
std::atomic< int > sLastOwner{ 0 };

int64_t Ms( double seconds )
{
	return static_cast< int64_t >( std::llround( seconds * 1000.0 ) );
}
} // namespace

int TempoSender::Owner()
{
	return sOwner.load();
}

double TempoSender::Rounded( double bpm )
{
	return std::round( bpm * 10.0 ) / 10.0;
}

bool TempoSender::Claim( double now, bool live )
{
	int current = sOwner.load();
	if( current == mInstance )
	{
		sOwnerBeatMs.store( Ms( now ) );
		sOwnerLive.store( live );
		return true;
	}
	const bool free     = current == 0 || Ms( now ) - sOwnerBeatMs.load() > Ms( kOwnerTimeout );
	const bool outranks = live && !sOwnerLive.load() && !debug.keepOwnership;// a detected tempo over Default BPM
	if( ( free || outranks ) && sOwner.compare_exchange_strong( current, mInstance ) )
	{
		sOwnerBeatMs.store( Ms( now ) );
		sOwnerLive.store( live );
		// Another instance had the job since this one last did: what is in
		// Resolume now is its doing, not this sender's.
		if( sLastOwner.exchange( mInstance ) != mInstance )
			Reset();
		return true;
	}
	return false;
}

void TempoSender::Release()
{
	int me = mInstance;
	if( me != 0 )
		sOwner.compare_exchange_strong( me, 0 );
}

void TempoSender::Reset()
{
	mLastSent   = 0.0;
	mLastSendAt = -1e300;
	mResends    = 0;
	mDelivered  = false;
	mByHand     = false;
}

void TempoSender::Forget()
{
	Reset();
	if( mState != State::Off && mState != State::NotOwner )
		mState = State::Waiting;
}

bool TempoSender::Transmit( double value, double now, const Sink& sink )
{
	const bool ok = sink ? sink( value ) : false;
	mLastSent     = value;
	mLastSendAt   = now;
	mDelivered    = false;
	mByHand       = false;
	++mSends;
	mState = ok ? State::Sent : State::Failed;
	return true;
}

bool TempoSender::Step( bool enabled, double target, bool live, double hostBpm, bool hostSeen, double now, const Sink& sink )
{
	if( !enabled )
	{
		Release();
		Reset();
		mState = State::Off;
		return false;
	}

	auto atHost = [ & ]( double v ) { return hostSeen && std::fabs( hostBpm - v ) <= kAckTolerance; };

	// Watched every frame, owner or not: Resolume's tempo became the value
	// sent (delivered), or later left it without this sender (by hand).
	if( mLastSent > 0.0 && atHost( mLastSent ) )
	{
		mDelivered = true;
		mByHand    = false;
	}
	else if( mDelivered && hostSeen )
		mByHand = true;

	const bool hasTarget = target > 0.0 && std::isfinite( target );
	if( !hasTarget && !debug.keepOwnership )
	{
		// Nothing to say: do not hold the job, so another instance that has
		// a tempo can send it.
		Release();
		mState = State::Waiting;
		return false;
	}
	if( !Claim( now, hasTarget && live ) )
	{
		// Somebody else owns the tempo. Start from nothing if we ever get it.
		mState = State::NotOwner;
		Reset();
		return false;
	}
	if( mState == State::Off || mState == State::NotOwner )
		mState = State::Waiting;

	if( hasTarget )
	{
		const double value = Rounded( target );
		// The music's tempo moved from what this sender last said (v0.2.0's
		// whole test) ...
		const bool moved = mLastSent <= 0.0 || std::fabs( value - mLastSent ) >= kHysteresis || debug.noHysteresis;
		// ... or Resolume's tempo is somewhere else, when Resolume does not
		// hold this sender's last value. While it does, that value IS
		// Resolume's tempo and the test above is the same test. When it held
		// it and somebody then moved it by hand, the hand's tempo is theirs
		// to keep until the music moves. And a value still being sent is the
		// resends' business below.
		const bool pending   = value == mLastSent && ( mState == State::Sent || mState == State::NotTaken || mState == State::Failed );
		const bool notHeld   = !mDelivered || ( mByHand && debug.ignoreHand );
		const bool elsewhere = !debug.lastSentOnly && hostSeen && notHeld && std::fabs( value - hostBpm ) >= kHysteresis && !pending;
		if( moved || elsewhere )
		{
			if( now - mLastSendAt < kInterval )
				return false;
			if( atHost( value ) && !debug.noHysteresis )
			{
				// Resolume is already there: nothing to say.
				mLastSent  = value;
				mDelivered = true;
				mByHand    = false;
				mResends   = 0;
				mState     = State::Taken;
				return false;
			}
			mResends = 0;
			return Transmit( value, now, sink );
		}
	}

	// Resolume has the value (or had it, before a hand moved it) -- late news
	// included, after the sender had given up.
	if( mDelivered )
	{
		mState = mByHand ? State::Overridden : State::Taken;
		return false;
	}
	if( mState == State::Sent || mState == State::Failed )
	{
		if( now - mLastSendAt >= kAckSeconds )
		{
			if( mResends < kMaxResends )
			{
				++mResends;
				return Transmit( mLastSent, now, sink );
			}
			if( mState == State::Sent )
				mState = State::NotTaken;
		}
	}
	return false;
}

} // namespace pattern
