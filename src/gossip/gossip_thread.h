#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "../node_info.h"
#include "swim.h"

class Router;

namespace gossip {

// Configuration for the gossip protocol thread.
struct GossipConfig {
    std::chrono::milliseconds protocol_period{1000};
    std::chrono::milliseconds ping_timeout{200};
    int indirect_probe_count = 3;  // K in the SWIM paper
    int suspicion_mult = 5;        // suspicion timeout = suspicion_mult * protocol_period
};

// Drives the SWIM protocol: periodically pings a random peer, performs indirect
// probes on failure, manages suspicion timeouts, and synchronizes the Router
// with membership changes. Owns the Swim state machine.
class GossipThread {
   public:
    // send_fn: sends a framed message to host:port and returns the reply (or ""
    // on failure). This abstraction lets the gossip thread reuse the existing TCP
    // client infrastructure without owning socket code itself.
    using SendFn = std::function<std::string(const std::string &host, uint16_t port,
                                             const std::string &payload)>;

    GossipThread(const NodeInfo &self, Router *router, SendFn send_fn, GossipConfig config = {});
    ~GossipThread();

    // Start the gossip loop in a background thread.
    void start();
    void stop();

    // Contact seed nodes to announce this node's presence. Called once at startup.
    void joinViaSeeds(const std::vector<std::string> &seeds);

    // Permanently remove `node_id` from the cluster (the LEAVE verb). Drops it
    // from the ring, tombstones it, and gossips the departure to every peer.
    //
    // Deliberately actionable from ANY node, about any node — including one that
    // is already unreachable. The target never has to participate, which it could
    // not do anyway in the case this mainly exists for: reclaiming the ring slots
    // of a node that is permanently gone.
    //
    // Returns false if `node_id` is unknown to this node's membership view.
    bool requestLeave(const std::string &node_id);

    // Observer hook: fired once per completed membership sync exchange this node
    // takes part in (either as requester or responder). Wired to the
    // membership_syncs_total metric in main.cpp; the gossip layer itself stays
    // free of any Metrics dependency.
    void setOnMembershipSync(std::function<void()> cb) { on_membership_sync_ = std::move(cb); }

    // Access the SWIM state (e.g., to register callbacks).
    Swim &swim() { return swim_; }

    // Handle an incoming SWIM message (dispatched from Node::handleRequest).
    // Returns the response payload to send back.
    std::string handleMessage(const std::string &payload);

   private:
    void run();
    void tick();
    void pingTarget(const NodeInfo &target);
    // Direct ping only — no indirect probes, no suspicion. For the resurrection
    // probe: the target is *expected* to be dead, so escalating a non-answer
    // through K proxies would waste three round trips per probe on every
    // genuinely dead member.
    void probeDead(const NodeInfo &target);
    // Parse an ack: apply its piggybacked events and run the digest comparison.
    // Returns false if `resp` is empty or not an ack.
    bool handleAck(const NodeInfo &peer, const std::string &resp);
    std::string buildPing(const std::string &target_id = "");
    std::string buildPingReq(const std::string &target_id, const std::string &via_id);
    std::string buildAck(const std::string &target_id = "");
    void applyIncomingEvents(const std::string &events_field);

    // Membership anti-entropy (Tier 4.7): compare the digest a peer's ack
    // carried against our own; on a *persistent* mismatch, run a push-pull
    // full-state exchange with that peer.
    void maybeSync(const NodeInfo &peer, const std::string &digest_field);
    void performSync(const NodeInfo &peer);

    NodeInfo self_;
    Router *router_;
    SendFn send_fn_;
    GossipConfig config_;
    Swim swim_;

    std::function<void()> on_membership_sync_;

    // Mismatch bookkeeping, confined to the gossip thread (only the tick →
    // pingTarget → maybeSync path touches it), so it needs no lock.
    struct MismatchState {
        int consecutive = 0;
        uint64_t last_sync_tick = 0;
    };
    std::map<std::string, MismatchState> mismatch_;
    uint64_t tick_count_ = 0;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace gossip
