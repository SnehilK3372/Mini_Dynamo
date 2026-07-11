#pragma once

#include <string>
#include <utility>

#include "../replica_client.h"

// Production ReplicaClient: speaks the framed internal protocol to a peer node
// over TCP. This is the object the Coordinator fans out through in a real
// cluster; tests substitute an in-process fake for the same interface.
//
//   write:  REPLICATE|<key>|<b64(data)>|<origin>|<clock>   -> RESPONSE|OK | RESPONSE|ERROR|...
//   read:   READ|<key>|<origin>                            -> VAL|<b64(data)>|<clock> |
//   VAL|NOTFOUND
class TcpReplicaClient : public ReplicaClient {
   public:
    explicit TcpReplicaClient(std::string origin) : origin_(std::move(origin)) {}

    ReplicaWriteResult writeReplica(const NodeInfo &peer, const std::string &key,
                                    const VersionedValue &value,
                                    std::chrono::milliseconds timeout) override;

    ReplicaReadResult readReplica(const NodeInfo &peer, const std::string &key,
                                  std::chrono::milliseconds timeout) override;

   private:
    std::string origin_;  // this node's id, stamped into the request's origin field
};
