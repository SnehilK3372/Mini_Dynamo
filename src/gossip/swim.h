#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../node_info.h"
#include "member_event.h"

namespace gossip {

// SWIM member states (per the SWIM paper by Das et al.), plus `Left`.
//
// Dynamo draws a hard line between two things a failure detector cannot tell
// apart, and this enum is where that line lives:
//
//   Dead — a *temporary* failure, inferred from a gossip timeout. The node KEEPS
//          its ring slots: key ownership must not churn on every blip, and the
//          coordinator handles the outage per request (skip on reads, hint +
//          stand-in on writes). A Dead node can come back; that is the point.
//   Left — a *permanent* departure, asserted by an operator. The node LOSES its
//          ring slots, and the record is a tombstone: terminal and sticky.
//
// `Left` is never inferred, only declared, precisely because no timeout can
// distinguish "slow" from "gone forever".
enum class MemberState { Alive, Suspect, Dead, Left };

struct MemberEntry {
    NodeInfo info;
    MemberState state = MemberState::Alive;
    uint64_t incarnation = 0;
    std::chrono::steady_clock::time_point suspect_since{};
};

// Callback signature for membership change notifications.
// Fired when a node transitions to Alive (join/recovery), Dead (confirmed
// failure) or Left (administrative removal).
using MemberChangeCallback = std::function<void(const NodeInfo &, MemberState)>;

// SWIM protocol state machine. Thread-safe: the gossip thread drives the
// protocol tick, while the TCP handler applies incoming events concurrently.
class Swim {
   public:
    Swim(const NodeInfo &self, int suspicion_mult = 5);

    // Apply a membership event (from incoming SWIM message or local detection).
    // Returns true if the event caused a state change (should be re-disseminated).
    //
    // Events here are RELAYED — third-party gossip — so they are gated on
    // incarnation and can never resurrect a node we hold Dead. Use
    // applyDirectJoin() for the join handshake, where the node speaks for itself.
    bool applyEvent(const MemberEvent &event);

    // Apply a JOIN received directly from the joining node (the SWIM_JOIN
    // handshake). Unlike relayed gossip, this is the node itself announcing that
    // it is up, so it is authoritative and is never rejected on incarnation.
    //
    // A restarted process comes back at incarnation 0 while we may still hold it
    // Dead at >= 0. Simply accepting it at 0 would revive it locally but leave
    // every *other* node stuck: they hold Dead@0 and reject a relayed Alive@0. So
    // the reviver bumps past its own stale record and returns the incarnation the
    // cluster should now use — the caller disseminates Alive at that value, which
    // peers accept because it is strictly newer than the Dead they hold.
    //
    // A node we hold `Left` is the ONE case this refuses. Because this path is
    // deliberately not gated on incarnation, the Left tombstone is the only thing
    // standing between a decommissioned node and a straight walk back into the
    // ring via the handshake.
    //
    // Returns the effective incarnation to disseminate the joiner as Alive at, or
    // NULLOPT if the join was refused (the joiner is tombstoned). The caller must
    // honour nullopt by disseminating nothing: an Alive broadcast on behalf of a
    // departed node would be rejected by every peer holding the tombstone but
    // ACCEPTED by any that does not yet, reviving it in a corner of the cluster.
    std::optional<uint64_t> applyDirectJoin(const MemberEvent &event);

    // Mark a peer as suspect (called when ping + indirect probe fail).
    void suspect(const std::string &node_id);

    // Confirm a suspect as dead (called when suspicion timeout expires).
    void confirmDead(const std::string &node_id);

    // Administratively remove a node from the cluster, permanently: drop it from
    // the ring and tombstone it so nothing can revive it. Disseminates a Leave.
    //
    // Unlike confirmDead(), this is an assertion by an operator, not an inference
    // from a timeout — so it is accepted whatever state or incarnation we hold for
    // the node, and it may name a node that is already Dead (the main use case:
    // reclaiming the ring slots of a node that is never coming back).
    //
    // Safe to call for `self`, which is how a node learns it has been retired.
    void leave(const std::string &node_id);

    // True once this node has been decommissioned (it saw a Leave naming itself).
    bool hasLeft() const;

    // Get all peers in Alive or Suspect state (for ping target selection).
    std::vector<NodeInfo> alivePeers() const;

    // Get all members (any state except Dead or Left).
    //
    // Excluding Left is what keeps a departed node out of the membership list we
    // hand a joiner in the SWIM_JOIN ack: a fresh node never even hears of it, so
    // it cannot resurrect it from a view that predates the tombstone.
    std::vector<MemberEntry> allMembers() const;

    // Pick K random alive peers, excluding specific node IDs.
    std::vector<NodeInfo> randomPeers(int k, const std::vector<std::string> &exclude = {}) const;

    // Get pending events to piggyback on outgoing SWIM messages.
    // Each event is returned up to `dissemination_limit` times (3*log2(N)).
    std::vector<MemberEvent> getEventsToSend();

    // Queue a new event for dissemination.
    void enqueueEvent(const MemberEvent &event);

    // Increment own incarnation to refute suspicion about self.
    uint64_t refute();

    // Check if a peer is known and alive.
    bool isAlive(const std::string &node_id) const;

    // The state we hold for a peer, or nullopt if we have never heard of it.
    // Unlike allMembers(), this reports Dead and Left entries — callers that need
    // to tell "departed" from "never known" (and tests asserting on the tombstone)
    // cannot use a list that hides exactly those two.
    std::optional<MemberState> stateOf(const std::string &node_id) const;

    // Check and expire suspects whose suspicion timeout has elapsed.
    // Returns node IDs that were confirmed dead.
    std::vector<std::string> expireSuspects(std::chrono::milliseconds suspicion_timeout);

    // Number of known members (alive + suspect; not dead, not departed).
    size_t memberCount() const;

    // Register a callback for membership state changes.
    void onMemberChange(MemberChangeCallback cb);

    // Own incarnation number.
    uint64_t incarnation() const;

    NodeInfo self() const { return self_; }

   private:
    NodeInfo self_;
    uint64_t incarnation_ = 0;
    bool self_left_ = false;  // we have been decommissioned
    int suspicion_mult_;      // suspicion timeout = suspicion_mult * protocol_period

    mutable std::mutex mtx_;
    std::map<std::string, MemberEntry> members_;  // node_id -> entry (excludes self)

    // Dissemination queue: events + remaining send count.
    struct PendingEvent {
        MemberEvent event;
        int remaining;  // decremented each time it's sent
    };
    std::vector<PendingEvent> pending_events_;

    mutable std::mt19937 rng_;

    std::vector<MemberChangeCallback> callbacks_;

    int disseminationLimit() const;
    void fireCallbacks(const NodeInfo &info, MemberState state);
};

}  // namespace gossip
