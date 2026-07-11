#include "tcp_replica_client.h"

#include <vector>

#include "../base64.h"
#include "tcp_client.h"

using namespace std;

namespace {
// Split on '|' preserving empty fields (an empty clock is a legitimate trailing
// empty field, so we must not drop it the way getline would).
vector<string> splitAll(const string &s, char delim) {
    vector<string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(delim, start);
        if (p == string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}
}  // namespace

ReplicaWriteResult TcpReplicaClient::writeReplica(const NodeInfo &peer, const string &key,
                                                  const VersionedValue &value,
                                                  chrono::milliseconds timeout) {
    string msg = "REPLICATE|" + key + "|" + base64::encode(value.data) + "|" + origin_ + "|" +
                 value.clock.serialize();

    TCPClient client;
    string reply =
        client.sendAndReceiveFramed(peer.host, peer.port, msg, static_cast<int>(timeout.count()));
    ReplicaWriteResult r;
    // Any well-formed OK acknowledgement counts; anything else (empty reply from a
    // timeout/refusal, or an ERROR status) is a non-ack.
    r.ok = reply.rfind("RESPONSE|OK", 0) == 0;
    return r;
}

ReplicaReadResult TcpReplicaClient::readReplica(const NodeInfo &peer, const string &key,
                                                chrono::milliseconds timeout) {
    string msg = "READ|" + key + "|" + origin_;

    TCPClient client;
    string reply =
        client.sendAndReceiveFramed(peer.host, peer.port, msg, static_cast<int>(timeout.count()));
    ReplicaReadResult r;
    if (reply.empty()) return r;  // ok=false: the replica did not respond

    auto f = splitAll(reply, '|');
    if (f.empty() || f[0] != "VAL") return r;  // unexpected shape → treat as no response

    r.ok = true;  // the replica answered
    if (f.size() >= 2 && f[1] == "NOTFOUND") {
        r.found = false;
        return r;
    }
    // VAL|<b64(data)>|<clock>
    r.found = true;
    r.value.data = base64::decode(f.size() >= 2 ? f[1] : "");
    r.value.clock = VectorClock::parse(f.size() >= 3 ? f[2] : "");
    return r;
}
