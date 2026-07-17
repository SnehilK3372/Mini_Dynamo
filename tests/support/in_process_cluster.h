#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coordinator.h"
#include "gossip/gossip_thread.h"
#include "hints/handoff_thread.h"
#include "hints/hint_store.h"
#include "metrics.h"
#include "node_info.h"
#include "replica_client.h"
#include "replica_ops.h"
#include "router.h"
#include "storage/in_memory_storage.h"

// A whole cluster of REAL nodes wired together inside one process — no sockets,
// no Docker, milliseconds per test.
//
// Why this exists: the suite had a chasm. Unit tests use a FakeReplicaClient (no
// cluster at all) and scripts/e2e.sh uses full Docker (slow, coarse, one
// scenario). Nothing exercised several real nodes talking to each other — which
// is where the bugs actually live. A restarted node silently never rejoining, and
// hinted handoff being inert as a result, both sailed through 89 green tests.
//
// Everything here is the shipped code: real Coordinator, real GossipThread/Swim,
// real HintStore/HandoffThread, real Router, and the real replicate rules via
// replica_ops. Only the *transport* is swapped — each component already takes an
// injectable seam (ReplicaClient / SendFn / DeliverFn), so a function call stands
// in for a socket. Nothing about the coordination logic is simulated.
namespace testcluster {

using namespace std::chrono_literals;

// Gossip tuned for tests: a ~20ms protocol period makes convergence and failure
// detection happen in tens of milliseconds instead of seconds. Suspicion timeout
// = protocol_period * suspicion_mult = ~60ms.
inline gossip::GossipConfig fastGossip() {
    gossip::GossipConfig c;
    c.protocol_period = 20ms;
    c.ping_timeout = 10ms;
    c.indirect_probe_count = 2;
    c.suspicion_mult = 3;
    return c;
}

class InProcessCluster;

// One node's whole world.
struct NodeCtx {
    explicit NodeCtx(NodeInfo i) : info(std::move(i)) {}

    NodeInfo info;
    Router router{128};  // same vnode count as production
    InMemoryStorageEngine storage;
    InMemoryMetrics metrics;
    HintStore hints{std::chrono::seconds(3600), 10000};
    std::unique_ptr<ReplicaClient> replicas;
    std::unique_ptr<Coordinator> coord;
    std::unique_ptr<HandoffThread> handoff;

    // "Crashed": the transport refuses everything to/from this node, exactly as a
    // dead peer looks from the outside.
    std::atomic<bool> down{false};

    // The gossip thread is held by shared_ptr and guarded, because restart()
    // replaces it while *other* nodes' gossip threads may be calling into it.
    // Callers take a shared_ptr copy under the lock and then release the lock
    // before use — so a swap can never free an object mid-call, and no lock is
    // ever held while calling out into another node (which would risk a cycle).
    std::mutex gmtx;
    std::shared_ptr<gossip::GossipThread> gossip;

    std::shared_ptr<gossip::GossipThread> gossipRef() {
        std::lock_guard<std::mutex> lk(gmtx);
        return gossip;
    }
};

// Routes replica reads/writes to the peer's real storage through replica_ops —
// the same never-regress rule Node::handleReplicate applies on the wire.
class InProcessReplicaClient : public ReplicaClient {
   public:
    InProcessReplicaClient(InProcessCluster *cluster, std::string self)
        : cluster_(cluster), self_(std::move(self)) {}

    ReplicaWriteResult writeReplica(const NodeInfo &peer, const std::string &key,
                                    const VersionedValue &value,
                                    std::chrono::milliseconds timeout) override;
    ReplicaReadResult readReplica(const NodeInfo &peer, const std::string &key,
                                  std::chrono::milliseconds timeout) override;

   private:
    InProcessCluster *cluster_;
    std::string self_;
};

class InProcessCluster {
   public:
    // Creates node1..nodeN (host == id, ports 5001+). Nothing is started yet.
    explicit InProcessCluster(int n, QuorumConfig qcfg = {}) : qcfg_(qcfg) {
        for (int i = 0; i < n; ++i) {
            std::string id = "node" + std::to_string(i + 1);
            auto ctx = std::make_unique<NodeCtx>(NodeInfo(id, id, static_cast<uint16_t>(5001 + i)));
            ctx->router.addPhysicalNode(ctx->info);  // a node always knows itself
            nodes_.push_back(std::move(ctx));
        }
        for (auto &n_ : nodes_) wire(*n_);
    }

    ~InProcessCluster() {
        // Drop gossip first so its threads stop before the objects they borrow.
        for (auto &n : nodes_) {
            std::shared_ptr<gossip::GossipThread> g;
            {
                std::lock_guard<std::mutex> lk(n->gmtx);
                g = std::move(n->gossip);
                n->gossip.reset();
            }
            g.reset();  // destructor joins the gossip thread, outside the lock
        }
        for (auto &n : nodes_) {
            if (n->handoff) n->handoff->stop();
        }
    }

    // node1 self-bootstraps; everyone else joins through it, then all gossip.
    void startGossip() {
        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto g = nodes_[i]->gossipRef();
            if (i > 0) g->joinViaSeeds({seedEndpoint()});
            g->start();
        }
    }

    // Simulate a crash: the transport starts failing for this node and its gossip
    // thread stops, so peers stop hearing from it and will suspect then evict it.
    void kill(const std::string &id) {
        NodeCtx &n = node(id);
        n.down = true;
        std::shared_ptr<gossip::GossipThread> g;
        {
            std::lock_guard<std::mutex> lk(n.gmtx);
            g = std::move(n.gossip);
            n.gossip.reset();
        }
        g.reset();  // joins the thread outside the lock
    }

    // Simulate a process restart: a BRAND NEW GossipThread/Swim, so the node comes
    // back at *incarnation 0* — exactly what a fresh process does. This is the
    // whole point: a harness that reused the old Swim would keep the incarnation
    // and never reproduce the rejoin bug.
    void restart(const std::string &id) {
        NodeCtx &n = node(id);
        auto fresh = makeGossip(n);
        {
            std::lock_guard<std::mutex> lk(n.gmtx);
            n.gossip = fresh;
        }
        n.down = false;
        fresh->joinViaSeeds({seedEndpoint()});
        fresh->start();
    }

    // --- polling helpers: assert on a condition with a deadline, never sleep ---

    bool waitFor(const std::function<bool()> &pred, std::chrono::milliseconds timeout = 5s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return pred();
    }

    // Every live node's ring holds exactly `expected` physical nodes.
    bool waitForRingEverywhere(size_t expected, std::chrono::milliseconds timeout = 5s) {
        return waitFor(
            [&] {
                for (auto &n : nodes_) {
                    if (n->down.load()) continue;
                    if (n->router.getAllPhysicalNodes().size() != expected) return false;
                }
                return true;
            },
            timeout);
    }

    // Every live node's ring contains (or does not contain) `id`.
    bool waitForRingContains(const std::string &id, bool present,
                             std::chrono::milliseconds timeout = 5s) {
        return waitFor(
            [&] {
                for (auto &n : nodes_) {
                    if (n->down.load() || n->info.node_id == id) continue;
                    if (ringHas(*n, id) != present) return false;
                }
                return true;
            },
            timeout);
    }

    // Every live peer's gossip view agrees on whether `id` is alive.
    //
    // This — not ring membership — is the signal for failure detection now: a
    // dead node deliberately STAYS in the ring (a temporary failure must not
    // reshuffle key ownership), and liveness is tracked in Swim and consulted per
    // request. Asserting on the ring would be asserting on a side effect that no
    // longer exists.
    bool waitForAliveEverywhere(const std::string &id, bool alive,
                                std::chrono::milliseconds timeout = 5s) {
        return waitFor(
            [&] {
                for (auto &n : nodes_) {
                    if (n->down.load() || n->info.node_id == id) continue;
                    auto g = n->gossipRef();
                    if (!g || g->swim().isAlive(id) != alive) return false;
                }
                return true;
            },
            timeout);
    }

    static bool ringHas(NodeCtx &n, const std::string &id) {
        for (const auto &p : n.router.getAllPhysicalNodes()) {
            if (p.node_id == id) return true;
        }
        return false;
    }

    NodeCtx &node(const std::string &id) {
        for (auto &n : nodes_) {
            if (n->info.node_id == id) return *n;
        }
        throw std::runtime_error("no such node: " + id);
    }
    size_t size() const { return nodes_.size(); }

    // Transport lookup: who lives at host:port, if anyone.
    NodeCtx *find(const std::string &host, uint16_t port) {
        for (auto &n : nodes_) {
            if (n->info.host == host && n->info.port == port) return n.get();
        }
        return nullptr;
    }

   private:
    std::string seedEndpoint() const {
        return nodes_[0]->info.host + ":" + std::to_string(nodes_[0]->info.port);
    }

    std::shared_ptr<gossip::GossipThread> makeGossip(NodeCtx &n) {
        // The SendFn is the whole network: find the peer and call it directly. A
        // down peer returns "" — indistinguishable from a timeout/refusal to the
        // gossip protocol, which is the point.
        auto send = [this](const std::string &host, uint16_t port,
                           const std::string &payload) -> std::string {
            NodeCtx *t = find(host, port);
            if (!t || t->down.load()) return "";
            auto g = t->gossipRef();  // copy under lock, call without it
            if (!g) return "";
            return g->handleMessage(payload);
        };

        auto g = std::make_shared<gossip::GossipThread>(n.info, &n.router, send, fastGossip());
        // Same wiring main.cpp does: a recovered peer wakes hint delivery.
        NodeCtx *ctx = &n;
        g->swim().onMemberChange([ctx](const NodeInfo &info, gossip::MemberState st) {
            if (st == gossip::MemberState::Alive && ctx->handoff) {
                ctx->handoff->notifyRecovery(info);
            }
        });
        return g;
    }

    void wire(NodeCtx &n) {
        n.replicas = std::make_unique<InProcessReplicaClient>(this, n.info.node_id);
        n.coord = std::make_unique<Coordinator>(n.info, &n.router, &n.storage, n.replicas.get(),
                                                &n.metrics, qcfg_);
        n.coord->setHintStore(&n.hints);

        // Liveness comes from this node's own gossip view, as in production.
        NodeCtx *ctx = &n;
        n.coord->setLivenessCheck([ctx](const std::string &peer_id) {
            auto g = ctx->gossipRef();
            if (!g) return true;  // no gossip view yet: assume alive (production default)
            return g->swim().isAlive(peer_id);
        });

        // Deliver a hint straight into the recovered peer's storage, via the same
        // replicate rule the wire path uses.
        auto deliver = [this](const NodeInfo &target, const std::string &key,
                              const VersionedValue &value) -> bool {
            NodeCtx *t = find(target.host, target.port);
            if (!t || t->down.load()) return false;
            replica_ops::applyReplicate(t->storage, key, value);
            return true;
        };
        n.handoff = std::make_unique<HandoffThread>(&n.hints, deliver, 10ms);
        n.handoff->start();

        n.gossip = makeGossip(n);
    }

    QuorumConfig qcfg_;
    std::vector<std::unique_ptr<NodeCtx>> nodes_;
};

// --- InProcessReplicaClient, now that InProcessCluster is complete ------------

inline ReplicaWriteResult InProcessReplicaClient::writeReplica(const NodeInfo &peer,
                                                               const std::string &key,
                                                               const VersionedValue &value,
                                                               std::chrono::milliseconds) {
    NodeCtx *t = cluster_->find(peer.host, peer.port);
    if (!t || t->down.load()) return {false};  // unreachable → no ack, like a dead peer
    replica_ops::applyReplicate(t->storage, key, value);
    return {true};
}

inline ReplicaReadResult InProcessReplicaClient::readReplica(const NodeInfo &peer,
                                                             const std::string &key,
                                                             std::chrono::milliseconds) {
    NodeCtx *t = cluster_->find(peer.host, peer.port);
    if (!t || t->down.load()) return {false, false, {}};
    ReplicaReadResult r;
    r.ok = true;
    auto v = replica_ops::readLocal(t->storage, key);
    if (v) {
        r.found = true;
        r.value = *v;
    }
    return r;
}

}  // namespace testcluster
