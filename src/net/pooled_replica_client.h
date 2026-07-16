#pragma once

#include <string>
#include <utility>

#include "../replica_client.h"
#include "connection_pool.h"

// Production ReplicaClient that reuses persistent connections from a
// ConnectionPool instead of opening a fresh socket per call (the one-shot
// TcpReplicaClient). Same wire protocol, same interface — this is a drop-in
// replacement injected into the Node so the coordinator's quorum fan-out stops
// churning sockets under Tier 4's gossip/hint/anti-entropy traffic.
//
//   write:  REPLICATE|<key>|<b64(data)>|<origin>|<clock>  -> RESPONSE|OK | RESPONSE|ERROR|...
//   read:   READ|<key>|<origin>                           -> VAL|<b64(data)>|<clock> | VAL|NOTFOUND
//
// The pool is borrowed (owned by main), so this object must not outlive it.
class PooledReplicaClient : public ReplicaClient {
   public:
    PooledReplicaClient(ConnectionPool *pool, std::string origin)
        : pool_(pool), origin_(std::move(origin)) {}

    ReplicaWriteResult writeReplica(const NodeInfo &peer, const std::string &key,
                                    const VersionedValue &value,
                                    std::chrono::milliseconds timeout) override;

    ReplicaReadResult readReplica(const NodeInfo &peer, const std::string &key,
                                  std::chrono::milliseconds timeout) override;

   private:
    // Send one framed request and read one framed reply over a pooled connection.
    // Handles the half-open case: if a checked-out (possibly stale) connection
    // fails on the first send/recv, the connection is discarded and the exchange
    // is retried once on a fresh one. Returns the reply, or "" on failure.
    // `reused_ok_out` is set true when the exchange completed and the connection
    // was returned to the pool for reuse.
    std::string exchange(const NodeInfo &peer, const std::string &request,
                         std::chrono::milliseconds timeout);

    ConnectionPool *pool_;
    std::string origin_;
};
