#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "../node_info.h"
#include "member_event.h"

namespace gossip {

// SWIM member states (per the SWIM paper by Das et al.).
enum class MemberState { Alive, Suspect, Dead };

struct MemberEntry {
    NodeInfo info;
    MemberState state = MemberState::Alive;
    uint64_t incarnation = 0;
    std::chrono::steady_clock::time_point suspect_since{};
};

// Callback signature for membership change notifications.
// Fired when a node transitions to Alive (join/recovery) or Dead (confirmed failure).
using MemberChangeCallback = std::function<void(const NodeInfo &, MemberState)>;

// SWIM protocol state machine. Thread-safe: the gossip thread drives the
// protocol tick, while the TCP handler applies incoming events concurrently.
class Swim {
   public:
    Swim(const NodeInfo &self, int suspicion_mult = 5);

    // Apply a membership event (from incoming SWIM message or local detection).
    // Returns true if the event caused a state change (should be re-disseminated).
    bool applyEvent(const MemberEvent &event);

    // Mark a peer as suspect (called when ping + indirect probe fail).
    void suspect(const std::string &node_id);

    // Confirm a suspect as dead (called when suspicion timeout expires).
    void confirmDead(const std::string &node_id);

    // Get all peers in Alive or Suspect state (for ping target selection).
    std::vector<NodeInfo> alivePeers() const;

    // Get all members (any state except Dead).
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

    // Check and expire suspects whose suspicion timeout has elapsed.
    // Returns node IDs that were confirmed dead.
    std::vector<std::string> expireSuspects(std::chrono::milliseconds suspicion_timeout);

    // Number of known members (alive + suspect, not dead).
    size_t memberCount() const;

    // Register a callback for membership state changes.
    void onMemberChange(MemberChangeCallback cb);

    // Own incarnation number.
    uint64_t incarnation() const;

    NodeInfo self() const { return self_; }

   private:
    NodeInfo self_;
    uint64_t incarnation_ = 0;
    int suspicion_mult_;  // suspicion timeout = suspicion_mult * protocol_period

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
