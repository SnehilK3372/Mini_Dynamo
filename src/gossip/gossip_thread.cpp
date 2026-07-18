#include "gossip_thread.h"

#include <optional>
#include <sstream>

#include "../log.h"
#include "../router.h"
#include "../util.h"

namespace gossip {

namespace {
// SWIM wire format (pipe-delimited, length-prefixed like all other messages):
//   SWIM_PING|<sender_id>|<target_id>|<events>
//   SWIM_PING_REQ|<sender_id>|<target_id>|<via_id>|<events>
//   SWIM_ACK|<sender_id>|<target_id>|<events>|<digest>
//   SWIM_JOIN|<sender_id>|<host>|<port>|<incarnation>|<events>
//   SWIM_SYNC|<sender_id>|<events>          (full state, Tier 4.7)
//   SWIM_SYNC_ACK|<sender_id>|<events>      (full state back — push-pull)
//
// The <digest> on every ack is a hash of the responder's entire membership view
// (Swim::digest()). Piggybacked events have a finite dissemination budget, so a
// node that misses one — partition, drop — used to stay divergent FOREVER; the
// digest turns that into a detectable mismatch, answered by a SWIM_SYNC
// push-pull full-state exchange. Membership anti-entropy, in one round trip.
//
// Departure has no SWIM_* verb of its own: the operator-facing LEAVE command
// (Node::handleLeave) calls requestLeave() on whichever node it reached, and the
// resulting Leave event rides the normal piggyback stream to everyone else.

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(delim, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string field(const std::vector<std::string> &f, size_t i) {
    return i < f.size() ? f[i] : std::string();
}

// Sync trigger tuning. Two consecutive mismatches before syncing: a single one is
// usually just an event still in flight (the comparison raced the piggyback), and
// syncing on it would thrash during perfectly normal churn. Ten protocol periods
// of cooldown per peer bounds sync traffic even while genuinely diverged.
constexpr int kSyncMismatchThreshold = 2;
constexpr uint64_t kSyncCooldownTicks = 10;

// Every k-th tick, ping one member from the DEAD set (rotating through it). A
// truly dead node costs one wasted ping per k periods; a secretly-alive one
// answers, and its ack's digest kicks off the sync + refutation that revives it.
constexpr uint64_t kDeadProbeEveryTicks = 5;
}  // namespace

GossipThread::GossipThread(const NodeInfo &self, Router *router, SendFn send_fn,
                           GossipConfig config)
    : self_(self),
      router_(router),
      send_fn_(std::move(send_fn)),
      config_(config),
      swim_(self, config.suspicion_mult) {
    // A node that joins or recovers enters the ring.
    //
    // A node that DIES stays in the ring, deliberately. The ring is the key's
    // preference list; gossip-detected death is a *temporary* failure, and Dynamo
    // separates that from permanent departure:
    //   - temporary  -> ring unchanged; the coordinator skips the dead owner on
    //                   reads and stores a hint + uses a stand-in on writes.
    //   - permanent  -> an explicit administrative removal changes the ring.
    // Evicting on a gossip timeout conflated the two, and cost twice over:
    //   1. every transient blip reshuffled key ownership across the ring, and
    //   2. it made hinted handoff DEAD CODE. A node is isAlive while Alive *or*
    //      Suspect, and the instant it turned Dead it left the ring — so it was
    //      never an owner the coordinator could see as dead, and
    //      hint_store_->store() was unreachable. Tier 4.2's headline feature had
    //      never stored a single hint. Found by tests/cluster_test.cpp.
    // Liveness now lives where it belongs: in Swim, consulted per request via the
    // coordinator's is_alive_fn.
    //
    // A node that LEAVES is the other half of that distinction, and the one case
    // that does change the ring: an operator has asserted the node is gone for
    // good, which is a claim no failure detector can make on its own.
    swim_.onMemberChange([this](const NodeInfo &info, MemberState state) {
        if (state == MemberState::Alive) {
            router_->addPhysicalNode(info);
            jlog::op("info", "gossip", info.node_id, "added_to_ring");
        } else if (state == MemberState::Dead) {
            jlog::op("info", "gossip", info.node_id, "marked_dead_kept_in_ring");
        } else if (state == MemberState::Left) {
            router_->removePhysicalNode(info.node_id);
            jlog::op("info", "gossip", info.node_id, "removed_from_ring_permanently");
        }
    });
}

GossipThread::~GossipThread() { stop(); }

void GossipThread::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&GossipThread::run, this);
}

void GossipThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void GossipThread::joinViaSeeds(const std::vector<std::string> &seeds) {
    // Build a SWIM_JOIN message with our identity.
    std::ostringstream oss;
    oss << "SWIM_JOIN|" << self_.node_id << "|" << self_.host << "|" << self_.port << "|"
        << swim_.incarnation() << "|";

    std::string msg = oss.str();

    for (const auto &seed : seeds) {
        auto parts = split(seed, ':');
        if (parts.size() < 2) continue;
        std::string host = parts[0];
        uint16_t port = 0;
        try {
            port = static_cast<uint16_t>(std::stoi(parts[1]));
        } catch (...) {
            continue;
        }

        std::string resp = send_fn_(host, port, msg);
        if (!resp.empty()) {
            // Parse the ACK (which carries the responder's member list as events).
            auto f = split(resp, '|');
            if (!f.empty() && f[0] == "SWIM_ACK") {
                applyIncomingEvents(field(f, 3));
                jlog::op("info", "gossip", host + ":" + std::to_string(port), "seed_connected");
                return;  // One seed is enough; gossip will propagate to the rest.
            }
        }
    }
    jlog::msg("warn", "gossip: failed to contact any seed node");
}

bool GossipThread::requestLeave(const std::string &node_id) {
    // Report unknown rather than silently tombstoning a typo'd id: a Leave for a
    // name nobody holds would gossip out and pre-emptively bar a node that
    // legitimately joins under that id later.
    if (!swim_.stateOf(node_id)) return false;
    swim_.leave(node_id);  // idempotent: a second LEAVE is a quiet no-op
    jlog::op("info", "gossip", node_id, "leave_requested");
    return true;
}

void GossipThread::run() {
    while (running_) {
        tick();
        std::this_thread::sleep_for(config_.protocol_period);
    }
}

void GossipThread::tick() {
    ++tick_count_;  // drives the per-peer sync cooldown

    // A retired node stops probing: it has no ring slots and no say in membership,
    // and its pings would only feed peers events from a view they have tombstoned.
    if (swim_.hasLeft()) return;

    // 1. Expire suspects that have timed out.
    auto suspicion_timeout = config_.protocol_period * config_.suspicion_mult;
    swim_.expireSuspects(suspicion_timeout);

    // 1b. Resurrection probe (Tier 4.7): every k-th tick, ping one member from
    // the DEAD set, rotating through it. Without this, membership anti-entropy
    // deadlocks on mutual death: a partition longer than the suspicion timeout
    // makes BOTH sides expire each other, and after it heals nobody probes a
    // Dead node — so there are no acks, no digest comparisons, and the sync that
    // exists to fix exactly this never triggers. One ping per k periods buys the
    // way out: a genuinely dead node ignores it, a secretly-alive one answers
    // with a digest that mismatches, and sync + SWIM self-refutation revive both
    // directions. This must run BEFORE the alivePeers early-return below — in
    // the mutual-death case the alive set is empty, which is precisely when the
    // probe matters most.
    if (tick_count_ % kDeadProbeEveryTicks == 0) {
        auto dead = swim_.deadPeers();
        if (!dead.empty()) {
            probeDead(dead[(tick_count_ / kDeadProbeEveryTicks) % dead.size()]);
        }
    }

    // 2. Pick one random peer to ping.
    auto peers = swim_.alivePeers();
    if (peers.empty()) return;

    // Round-robin shuffled selection to ensure full coverage over time.
    static thread_local size_t idx = 0;
    const NodeInfo &target = peers[idx % peers.size()];
    ++idx;

    pingTarget(target);
}

bool GossipThread::handleAck(const NodeInfo &peer, const std::string &resp) {
    if (resp.empty()) return false;
    auto f = split(resp, '|');
    if (f.empty() || f[0] != "SWIM_ACK") return false;
    // Apply piggybacked events, then compare membership digests: the ack carries
    // the responder's, and a persistent difference means one of us missed an
    // event whose dissemination budget is spent — the case piggybacking alone
    // can never repair.
    applyIncomingEvents(field(f, 3));
    maybeSync(peer, field(f, 4));
    return true;
}

void GossipThread::probeDead(const NodeInfo &target) {
    // The ping still carries piggyback events — deliberately. If the "dead" node
    // is actually alive, this is the channel its peers' pending events (and, on
    // the way back, its own refutation) first travel over after a heal.
    std::string resp = send_fn_(target.host, target.port, buildPing(target.node_id));
    handleAck(target, resp);  // silence is the expected outcome for a real corpse
}

void GossipThread::pingTarget(const NodeInfo &target) {
    std::string ping = buildPing(target.node_id);
    std::string resp = send_fn_(target.host, target.port, ping);

    if (!resp.empty()) {
        handleAck(target, resp);
        return;
    }

    // Direct ping failed — attempt indirect probe via K random peers. The ack
    // (and digest) that comes back is the PROXY's, not the target's — handleAck
    // attributes the comparison to the node that actually answered.
    auto proxies = swim_.randomPeers(config_.indirect_probe_count, {target.node_id});
    bool got_ack = false;

    for (const auto &proxy : proxies) {
        std::string preq = buildPingReq(target.node_id, proxy.node_id);
        std::string presp = send_fn_(proxy.host, proxy.port, preq);
        if (handleAck(proxy, presp)) {
            got_ack = true;
            break;
        }
    }

    if (!got_ack) {
        // All probes failed — mark target as suspect.
        swim_.suspect(target.node_id);
        jlog::op("info", "gossip", target.node_id, "suspected");
    }
}

std::string GossipThread::handleMessage(const std::string &payload) {
    auto f = split(payload, '|');
    if (f.empty()) return "";

    const std::string &type = f[0];

    if (type == "SWIM_PING") {
        // SWIM_PING|sender_id|target_id|events
        applyIncomingEvents(field(f, 3));
        return buildAck(field(f, 1));

    } else if (type == "SWIM_PING_REQ") {
        // SWIM_PING_REQ|sender_id|target_id|via_id|events
        // We are the proxy: ping the target on behalf of the sender.
        applyIncomingEvents(field(f, 4));
        std::string target_id = field(f, 2);

        // Look up the target's address.
        auto members = swim_.allMembers();
        for (const auto &m : members) {
            if (m.info.node_id == target_id) {
                std::string ping = buildPing(target_id);
                std::string resp = send_fn_(m.info.host, m.info.port, ping);
                if (!resp.empty()) {
                    // Forward the ACK back to the original sender.
                    return buildAck(field(f, 1));
                }
                break;
            }
        }
        return "";  // Target unreachable from us too.

    } else if (type == "SWIM_ACK") {
        applyIncomingEvents(field(f, 3));
        return "";

    } else if (type == "SWIM_SYNC") {
        // SWIM_SYNC|sender_id|events — the requester pushed its complete view;
        // apply it and push ours back. Both sides converge in this one exchange.
        applyIncomingEvents(field(f, 2));
        if (on_membership_sync_) on_membership_sync_();
        jlog::op("info", "gossip", field(f, 1), "membership_sync_answered");
        std::ostringstream oss;
        oss << "SWIM_SYNC_ACK|" << self_.node_id << "|" << serializeEvents(swim_.fullState());
        return oss.str();

    } else if (type == "SWIM_SYNC_ACK") {
        // Normally consumed by performSync as the reply on the same connection;
        // handled here too so an out-of-band one is applied rather than dropped.
        applyIncomingEvents(field(f, 2));
        return "";

    } else if (type == "SWIM_JOIN") {
        // SWIM_JOIN|sender_id|host|port|incarnation|events
        std::string sender_id = field(f, 1);
        // Refuse before any state changes: an id carrying a delimiter would be
        // re-serialized into the piggyback stream and corrupt event parsing on
        // every node in the cluster. No ack — the joiner is misconfigured and
        // must not come up believing it joined.
        if (!isValidNodeId(sender_id)) {
            jlog::op("warn", "gossip", sender_id, "join_refused_invalid_node_id");
            return "";
        }
        std::string host = field(f, 2);
        uint16_t port = 0;
        try {
            port = static_cast<uint16_t>(std::stoi(field(f, 3)));
        } catch (...) {
        }
        uint64_t incarnation = 0;
        try {
            incarnation = std::stoull(field(f, 4));
        } catch (...) {
        }
        applyIncomingEvents(field(f, 5));

        // The joiner is speaking for itself, so this is authoritative — unlike the
        // relayed events above, it is not gated on incarnation (a restarted process
        // returns at 0 and would otherwise be rejected forever).
        MemberEvent join_ev;
        join_ev.type = EventType::Join;
        join_ev.node_id = sender_id;
        join_ev.host = host;
        join_ev.port = port;
        join_ev.incarnation = incarnation;
        std::optional<uint64_t> eff = swim_.applyDirectJoin(join_ev);

        if (!eff) {
            // The joiner has been decommissioned. Answer with a Leave naming it and
            // nothing else: applying that event trips the joiner's own self-Leave
            // branch, so it retires itself instead of retrying forever under the
            // impression it is a cluster member. Telling it beats stonewalling it.
            //
            // Critically, we disseminate NO Alive for it. Broadcasting one would be
            // ignored by peers holding the tombstone but honoured by any that do
            // not, quietly reviving a departed node in part of the cluster.
            MemberEvent leave_ev;
            leave_ev.type = EventType::Leave;
            leave_ev.node_id = sender_id;
            jlog::op("info", "gossip", sender_id, "join_refused_node_departed");
            std::ostringstream refused;
            refused << "SWIM_ACK|" << self_.node_id << "|" << sender_id << "|"
                    << serializeEvents({leave_ev});
            return refused.str();
        }

        // Disseminate the revival as Alive at the *effective* incarnation, which
        // applyDirectJoin bumped past whatever we held. Re-broadcasting the join's
        // own (possibly stale, e.g. 0) incarnation would be rejected by every peer
        // still holding the node Dead — it would rejoin for us and nobody else.
        MemberEvent alive_ev = join_ev;
        alive_ev.type = EventType::Alive;
        alive_ev.incarnation = *eff;
        swim_.enqueueEvent(alive_ev);

        // Reply with an ACK that carries our full membership as events so the
        // joiner learns about all existing nodes immediately.
        auto members = swim_.allMembers();
        std::vector<MemberEvent> full_list;
        // Include ourselves.
        MemberEvent self_ev;
        self_ev.type = EventType::Alive;
        self_ev.node_id = self_.node_id;
        self_ev.host = self_.host;
        self_ev.port = self_.port;
        self_ev.incarnation = swim_.incarnation();
        full_list.push_back(self_ev);
        for (const auto &m : members) {
            MemberEvent ev;
            ev.type = EventType::Alive;
            ev.node_id = m.info.node_id;
            ev.host = m.info.host;
            ev.port = m.info.port;
            ev.incarnation = m.incarnation;
            full_list.push_back(ev);
        }

        std::ostringstream oss;
        oss << "SWIM_ACK|" << self_.node_id << "|" << sender_id << "|"
            << serializeEvents(full_list);
        return oss.str();
    }

    return "";
}

std::string GossipThread::buildPing(const std::string &target_id) {
    auto events = swim_.getEventsToSend();
    std::ostringstream oss;
    oss << "SWIM_PING|" << self_.node_id << "|" << target_id << "|" << serializeEvents(events);
    return oss.str();
}

std::string GossipThread::buildPingReq(const std::string &target_id,
                                       const std::string & /*via_id*/) {
    auto events = swim_.getEventsToSend();
    std::ostringstream oss;
    oss << "SWIM_PING_REQ|" << self_.node_id << "|" << target_id << "|" << self_.node_id << "|"
        << serializeEvents(events);
    return oss.str();
}

std::string GossipThread::buildAck(const std::string &target_id) {
    auto events = swim_.getEventsToSend();
    std::ostringstream oss;
    oss << "SWIM_ACK|" << self_.node_id << "|" << target_id << "|" << serializeEvents(events) << "|"
        << swim_.digest();
    return oss.str();
}

void GossipThread::maybeSync(const NodeInfo &peer, const std::string &digest_field) {
    if (digest_field.empty()) return;  // ack without a digest (pre-4.7 peer): nothing to compare
    uint64_t theirs = 0;
    try {
        theirs = std::stoull(digest_field);
    } catch (...) {
        return;
    }
    if (theirs == swim_.digest()) {
        mismatch_.erase(peer.node_id);  // converged; forget any streak
        return;
    }
    auto &m = mismatch_[peer.node_id];
    ++m.consecutive;
    if (m.consecutive < kSyncMismatchThreshold) return;
    if (m.last_sync_tick != 0 && tick_count_ - m.last_sync_tick < kSyncCooldownTicks) return;
    m.consecutive = 0;
    m.last_sync_tick = tick_count_;
    performSync(peer);
}

void GossipThread::performSync(const NodeInfo &peer) {
    // Push-pull: send our complete view, get the peer's complete view back — one
    // round trip converges both sides. Every event is applied through the same
    // gated applyEvent as normal gossip, so stale sync data cannot regress anyone:
    // tombstones win unconditionally, Alive/Dead need strictly newer incarnations,
    // and a Dead-about-self triggers the standard refutation. That last one is
    // what rescues a node stranded Dead by a healed partition — it learns of its
    // own death here and refutes at a strictly newer incarnation.
    std::string payload = "SWIM_SYNC|" + self_.node_id + "|" + serializeEvents(swim_.fullState());
    std::string resp = send_fn_(peer.host, peer.port, payload);
    if (resp.empty()) return;  // unreachable: normal probing will judge it
    auto f = split(resp, '|');
    if (f.empty() || f[0] != "SWIM_SYNC_ACK") return;
    applyIncomingEvents(field(f, 2));
    if (on_membership_sync_) on_membership_sync_();
    jlog::op("info", "gossip", peer.node_id, "membership_sync_completed");
}

void GossipThread::applyIncomingEvents(const std::string &events_field) {
    if (events_field.empty()) return;
    auto events = deserializeEvents(events_field);
    for (auto &ev : events) {
        // Drop malformed or invalid-id events instead of applying them. A
        // malformed parse yields an empty id, and a poisoned id would fabricate a
        // phantom member here and then re-disseminate below — this check is the
        // choke point that stops a corrupted stream from spreading.
        if (!isValidNodeId(ev.node_id)) continue;
        if (swim_.applyEvent(ev)) {
            swim_.enqueueEvent(ev);
        }
    }
}

}  // namespace gossip
