#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
	Tempo Out: the detected tempo pushed into Resolume's composition tempo.

	FFGL gives a plugin no way to set anything in the host. `SetBeatInfo` runs
	host to plugin only, and the only messages a plugin can raise are events
	about its own parameters. So the tempo leaves the way any controller's
	would: one OSC message over UDP to Resolume's OSC input on 127.0.0.1.

	## The message, measured

	Measured in Resolume Arena 7.27.1 (the fleet's Windows VM, UDP to
	127.0.0.1:7000, read back over REST) before this was written:

	    /composition/tempocontroller/tempo ,f 0.2083333   -> 119.99998 BPM
	    /composition/tempocontroller/tempo ,f 140         -> 500 (clamped)
	    /composition/tempocontroller/tempo ,i 120         -> 120
	    /composition/tempocontroller/tempo ,sf "a" 119.9  -> 119.9 exactly
	    /composition/tempocontroller/tempo ,sf "a" 87.25  -> 87.25

	A float is NORMALISED over the tempo's 20..500 range, so the obvious
	message -- a float carrying the BPM, which is what Bitfocus's Companion
	module sends -- sets 500 BPM. Resolume's documented absolute form, the
	string "a" before the value, takes the BPM itself, keeps fractions and
	does not depend on the range. That is the one sent here: 48 bytes.

	    "/composition/tempocontroller/tempo"  34 chars + 2 NULs  = 36
	    ",sf"                                 + 1 NUL            =  4
	    "a"                                   + 3 NULs           =  4
	    float32 BPM, big-endian                                  =  4

	OSC strings are NUL-terminated and then padded to four bytes, and the
	terminator is not optional: a string whose length is already a multiple
	of four still gets four NULs (weblinked's `padded`, the same rule).

	## The socket

	Opened on the first send, never in a constructor -- Resolume constructs
	every plugin at its startup scan, and a scan must not open sockets.
	Non-blocking and unconnected: a send that cannot go now fails now, on the
	render thread, rather than waiting. No thread, nothing thread-local (a
	bundle with TLS can never be unloaded), Winsock or BSD sockets and
	nothing else. Always 127.0.0.1, which also keeps macOS's Local Network
	prompt out of it.

	## When it sends: TempoSender

	Pure policy, no sockets, so `pntest --send` can drive it against a fake
	Resolume. In order, per frame:

	- **Off** sends nothing and gives up the sender's place.
	- **A target, or nothing.** The tracker's `TempoTarget()`: the CONFIRMED
	  tempo while it is live (Tracker.h, "Confirmed, live and lost"), else
	  Default BPM when When Lost says so, else nothing (Hold sends nothing
	  new). `live` says which: a detected tempo, or the fallback.
	- **One sender per process, and only one with something to say.** An
	  instance with nothing to send does not hold the job: it lets go at
	  once, so a second instance that has a confirmed tempo can send (v0.2.0
	  kept the job for whichever ran first, and a second instance that had
	  confirmed showed ANOTHER SENDS for ever). An owner sending only Default
	  BPM gives way to one with a live detected tempo. Otherwise the owner
	  keeps it until it has not run a frame for two seconds (its clip
	  stopped), and then another takes over. A sender that gets the job back
	  after another instance had it forgets what it last sent: Resolume's
	  tempo is the other instance's now.
	- **Rounded to 0.1 BPM, at most once a second, and only on a change of
	  at least 0.5 BPM** -- from the last value sent, or from Resolume's own
	  tempo (SetBeatInfo) when Resolume does not hold that value: it never
	  took it, or another instance has sent since. (While Resolume does hold
	  it the two are the same test.) A settled detector wobbles by a few
	  tenths; a composition tempo that followed it would wobble too.
	- **Already there** counts as sent: if Resolume's tempo is within 0.05 of
	  the value, nothing goes out.
	- **Did Resolume take it?** After a send, Resolume's tempo should arrive
	  back through `SetBeatInfo`. If it has not come within 0.05 of the value
	  in two seconds the message is sent again, at most three more times,
	  and then the sender says NOT TAKEN and logs why it thinks so (OSC
	  Input off, or another port). A NEW value is still sent after that when
	  it is 0.5 from Resolume's tempo, so switching OSC Input on is enough.
	- **It does not fight an operator.** Once Resolume has taken a value, a
	  change to its tempo that this sender did not make -- a hand, Ableton
	  Link, another controller -- is OVERRIDDEN, and stays until the detected
	  tempo moves 0.5 BPM from the value this sender last sent. That rule
	  wins over the comparison with Resolume's tempo, and holds across a
	  lost beat: a tempo set by hand during a break stays when the same beat
	  comes back. (`Debug::lastSentOnly` is v0.2.0's comparison with the
	  last value sent and nothing else.)
*/
namespace pattern
{
namespace osc
{
constexpr const char* kTempoAddress = "/composition/tempocontroller/tempo";

/// The wire size of an OSC string of `textLength` characters: the text, at
/// least one NUL, rounded up to four. Pass the length WITHOUT the terminator.
size_t Padded( size_t textLength );

/// `address ,sf "a" value`: Resolume's absolute form. 48 bytes for the tempo.
std::vector< uint8_t > AbsoluteFloat( const char* address, float value );

/// `address ,f value`: a plain float, which Resolume reads as NORMALISED.
/// Only the harness's negative control sends this.
std::vector< uint8_t > PlainFloat( const char* address, float value );

/// One UDP socket, to 127.0.0.1 only.
class UdpSender
{
public:
	UdpSender() = default;
	~UdpSender();
	UdpSender( const UdpSender& )            = delete;
	UdpSender& operator=( const UdpSender& ) = delete;

	/// One datagram to 127.0.0.1:`port`. Opens the socket on first use.
	/// Never blocks. False on any failure, with `Error()` saying what.
	bool Send( int port, const std::vector< uint8_t >& packet );

	/// Close the socket (and release Winsock). Safe to call when closed.
	void Close();

	bool               Open() const { return mSocket != -1; }
	const std::string& Error() const { return mError; }

private:
	std::intptr_t mSocket  = -1;
	bool          mStarted = false;///< Winsock started (Windows only)
	std::string   mError;
};
} // namespace osc

/// When to send. See the header comment above.
class TempoSender
{
public:
	enum class State
	{
		Off,       ///< Send Tempo is off
		Waiting,   ///< on, with nothing to send (and not holding the sender's place)
		Sent,      ///< sent; Resolume's tempo has not come back yet
		Taken,     ///< Resolume's tempo is the value sent
		NotTaken,  ///< sent four times and Resolume's tempo never followed
		Failed,    ///< the socket refused
		NotOwner,  ///< another instance is sending
		Overridden ///< Resolume took the value, then its tempo was changed elsewhere
	};

	/// The negative controls. Not reachable from a parameter.
	struct Debug
	{
		bool noHysteresis  = false;///< send every second whatever changed
		bool keepOwnership = false;///< v0.2.0: the first instance on keeps the job with nothing to send
		bool lastSentOnly  = false;///< v0.2.0: the 0.5 BPM test against the last value sent only
		bool ignoreHand    = false;///< compare with Resolume's tempo even when it was set by hand
	};

	using Sink = std::function< bool( double bpm ) >;

	static constexpr double kHysteresis   = 0.5; ///< BPM
	static constexpr double kInterval     = 1.0; ///< seconds between sends, at least
	static constexpr double kAckSeconds   = 2.0; ///< for Resolume's tempo to follow
	static constexpr double kAckTolerance = 0.05;///< BPM
	static constexpr int    kMaxResends   = 3;
	static constexpr double kOwnerTimeout = 2.0;///< seconds without a frame before another may take over

	TempoSender() = default;
	~TempoSender() { Release(); }
	TempoSender( const TempoSender& )            = delete;
	TempoSender& operator=( const TempoSender& ) = delete;

	void SetInstance( int instance ) { mInstance = instance; }

	/// One frame. `target` <= 0 means nothing to send; `live` that the
	/// target is a detected tempo, not the fallback. `hostBpm` is the last
	/// SetBeatInfo, `hostSeen` whether there has been one. `now` is a
	/// monotonic clock in seconds shared by every instance in the process.
	/// Returns true when `sink` was called.
	bool Step( bool enabled, double target, bool live, double hostBpm, bool hostSeen, double now, const Sink& sink );

	/// Forget what was sent, so the next frame sends again (a new port).
	void Forget();

	State  GetState() const { return mState; }
	double LastSent() const { return mLastSent; }
	int    Sends() const { return mSends; }
	int    Resends() const { return mResends; }

	/// The value actually sent for a target: 0.1 BPM.
	static double Rounded( double bpm );
	/// The instance that owns the tempo, or 0.
	static int Owner();

	Debug debug;

private:
	bool Claim( double now, bool live );
	void Release();
	void Reset();
	bool Transmit( double value, double now, const Sink& sink );

	int    mInstance   = 0;
	State  mState      = State::Off;
	double mLastSent   = 0.0;
	double mLastSendAt = -1e300;
	int    mSends      = 0;
	int    mResends    = 0;
	bool   mDelivered  = false;///< Resolume's tempo has been the last value sent
	bool   mByHand     = false;///< and has since been changed by something else
};

} // namespace pattern
