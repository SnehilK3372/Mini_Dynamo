#pragma once

#include <string>

// Shared POSIX connect helper. Opens a blocking TCP socket to host:port with a
// bounded connect wait (non-blocking connect + select), then restores blocking
// mode and applies send/recv timeouts so a peer that accepts but never replies
// can't wedge the caller. Returns a connected fd (>= 0) or -1 on any failure
// (resolve, socket, connect timeout).
//
// Extracted so both the one-shot TCPClient and the ConnectionPool open
// connections the same way (same timeout semantics), rather than duplicating the
// non-blocking-connect dance.
namespace tcpconnect {

// `timeout_ms` bounds the connect; `io_timeout_ms` sets SO_RCVTIMEO/SO_SNDTIMEO
// on the returned socket (pass the same value for one-shot request/response, or
// a larger idle bound for a pooled connection that lives across requests).
int dial(const std::string &host, int port, int timeout_ms, int io_timeout_ms);

}  // namespace tcpconnect
