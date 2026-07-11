#pragma once

#include <string>

// A one-shot framed TCP request/response client: connect, send one framed
// message, read one framed reply, close. Used by the Node to forward a client
// PUT to the primary owner and by TcpReplicaClient to talk to peer replicas.
//
// Fire-and-forget replication is gone in Tier 1A: every replica interaction is
// now a request that waits for an acknowledgement, which is what makes real W/R
// quorum possible.
class TCPClient {
public:
    // Returns the peer's framed reply payload, or "" on any failure (DNS,
    // connect timeout, send/recv error, malformed frame). `timeout_ms` bounds
    // both the connect and the read so a dead peer can't wedge the caller.
    std::string sendAndReceiveFramed(const std::string &host, int port,
                                     const std::string &data, int timeout_ms = 2000);
};
