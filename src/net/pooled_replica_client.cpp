#include "pooled_replica_client.h"

#include <unistd.h>

#include <vector>

#include "../base64.h"
#include "framing.h"

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

// One framed round-trip on an already-connected fd. Returns false on any
// send/recv failure, which means the connection is unusable and must be
// discarded (never checked back in — a partial response would desync the next
// caller's frame).
bool roundTrip(int fd, const string &request, string &reply) {
    if (!framing::sendFramed(fd, request)) return false;
    reply.clear();
    if (!framing::recvFramed(fd, reply)) return false;
    return true;
}
}  // namespace

string PooledReplicaClient::exchange(const NodeInfo &peer, const string &request,
                                     chrono::milliseconds timeout) {
    // Attempt on a pooled (possibly idle/stale) connection first; on failure,
    // discard it and retry once on a guaranteed-fresh connection. This absorbs
    // the half-open case where the peer closed an idle connection we still hold.
    for (int attempt = 0; attempt < 2; ++attempt) {
        int fd = pool_->checkout(peer.host, peer.port, timeout);
        if (fd < 0) return "";  // could not connect at all (peer down) — no retry helps

        string reply;
        if (roundTrip(fd, request, reply)) {
            pool_->checkin(peer.host, peer.port, fd);  // healthy → reuse
            return reply;
        }
        pool_->discard(fd);  // broken/desynced → close, don't pool
        // Loop once more with a fresh connection. If a fresh connect also failed
        // the round trip, the peer is genuinely unresponsive; give up.
    }
    return "";
}

ReplicaWriteResult PooledReplicaClient::writeReplica(const NodeInfo &peer, const string &key,
                                                     const VersionedValue &value,
                                                     chrono::milliseconds timeout) {
    string msg = "REPLICATE|" + key + "|" + base64::encode(value.data) + "|" + origin_ + "|" +
                 value.clock.serialize();
    string reply = exchange(peer, msg, timeout);
    ReplicaWriteResult r;
    // Any well-formed OK acknowledgement counts; anything else (empty reply from a
    // timeout/refusal, or an ERROR status) is a non-ack.
    r.ok = reply.rfind("RESPONSE|OK", 0) == 0;
    return r;
}

ReplicaReadResult PooledReplicaClient::readReplica(const NodeInfo &peer, const string &key,
                                                   chrono::milliseconds timeout) {
    string msg = "READ|" + key + "|" + origin_;
    string reply = exchange(peer, msg, timeout);
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
