#pragma once
// netping.hpp - the stats overlay's ping: the busiest game server address
// (noted by the libc layer, address only), timed with ping(8) or a TCP connect.

#include <sys/socket.h>

namespace tonic {

// The library sent to (or connected to) A. Any thread; cheap.
void netping_note(const sockaddr* a, socklen_t len);

// Pings only while this is on (the stats overlay shows). Any thread.
void netping_want(bool on);

// The last round trip, ms. -1: no game server (yet), -2: it does not answer
// pings, -3: no ping command here. Any thread.
float netping_ms();

}  // namespace tonic
