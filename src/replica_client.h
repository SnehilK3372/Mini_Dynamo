#pragma once

#include <chrono>
#include <string>

#include "node_info.h"
#include "versioned_value.h"

// The seam that decouples coordinator logic from the network. The coordinator
// talks to peer replicas only through this interface, so the quorum /
// reconciliation / read-repair logic — the part worth testing and defending — is
// pure and socket-free. Production injects a TCP-backed implementation
// (TcpReplicaClient); tests inject a fake that returns programmed results and can
// simulate a down or slow replica without any sockets.
//
// Calls are synchronous single-peer operations; the coordinator fans out across
// peers on its own threads and enforces the quorum deadline. Implementations
// must be safe to call concurrently from many threads.
struct ReplicaWriteResult {
    bool ok = false;  // true iff the replica durably acknowledged the write
};

struct ReplicaReadResult {
    bool ok = false;     // true iff the replica responded at all (found or not)
    bool found = false;  // true iff the replica had the key
    VersionedValue value;
};

class ReplicaClient {
   public:
    virtual ~ReplicaClient() = default;

    virtual ReplicaWriteResult writeReplica(const NodeInfo &peer, const std::string &key,
                                            const VersionedValue &value,
                                            std::chrono::milliseconds timeout) = 0;

    virtual ReplicaReadResult readReplica(const NodeInfo &peer, const std::string &key,
                                          std::chrono::milliseconds timeout) = 0;
};
