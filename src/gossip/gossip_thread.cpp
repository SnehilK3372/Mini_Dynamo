#include "gossip_thread.h"

#include <sstream>

#include "../log.h"
#include "../router.h"

namespace gossip {

namespace {
// SWIM wire format (pipe-delimited, length-prefixed like all other messages):
//   SWIM_PING|<sender_id>|<target_id>|<events>
//   SWIM_PING_REQ|<sender_id>|<target_id>|<via_id>|<events>
//   SWIM_ACK|<sender_id>|<target_id>|<events>
//   SWIM_JOIN|<sender_id>|<host>|<port>|<incarnation>|<events>

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
}  // namespace

GossipThread::GossipThread(const NodeInfo &self, Router *router, SendFn send_fn,
                           GossipConfig config)
    : self_(self),
      router_(router),
      send_fn_(std::move(send_fn)),
      config_(config),
      swim_(self, config.suspicion_mult) {
    // When a node joins or recovers, add it to the ring.
    // When a node dies, remove it from the ring.
    swim_.onMemberChange([this](const NodeInfo &info, MemberState state) {
        if (state == MemberState::Alive) {
            router_->addPhysicalNode(info);
            jlog::op("info", "gossip", info.node_id, "added_to_ring");
        } else if (state == MemberState::Dead) {
            router_->removePhysicalNode(info.node_id);
            jlog::op("info", "gossip", info.node_id, "removed_from_ring");
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

void GossipThread::run() {
    while (running_) {
        tick();
        std::this_thread::sleep_for(config_.protocol_period);
    }
}

void GossipThread::tick() {
    // 1. Expire suspects that have timed out.
    auto suspicion_timeout = config_.protocol_period * config_.suspicion_mult;
    swim_.expireSuspects(suspicion_timeout);

    // 2. Pick one random peer to ping.
    auto peers = swim_.alivePeers();
    if (peers.empty()) return;

    // Round-robin shuffled selection to ensure full coverage over time.
    static thread_local size_t idx = 0;
    const NodeInfo &target = peers[idx % peers.size()];
    ++idx;

    pingTarget(target);
}

void GossipThread::pingTarget(const NodeInfo &target) {
    std::string ping = buildPing(target.node_id);
    std::string resp = send_fn_(target.host, target.port, ping);

    if (!resp.empty()) {
        // Got an ACK — target is alive. Process piggybacked events.
        auto f = split(resp, '|');
        if (!f.empty() && f[0] == "SWIM_ACK") {
            applyIncomingEvents(field(f, 3));
        }
        return;
    }

    // Direct ping failed — attempt indirect probe via K random peers.
    auto proxies = swim_.randomPeers(config_.indirect_probe_count, {target.node_id});
    bool got_ack = false;

    for (const auto &proxy : proxies) {
        std::string preq = buildPingReq(target.node_id, proxy.node_id);
        std::string presp = send_fn_(proxy.host, proxy.port, preq);
        if (!presp.empty()) {
            auto f = split(presp, '|');
            if (!f.empty() && f[0] == "SWIM_ACK") {
                applyIncomingEvents(field(f, 3));
                got_ack = true;
                break;
            }
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

    } else if (type == "SWIM_JOIN") {
        // SWIM_JOIN|sender_id|host|port|incarnation|events
        std::string sender_id = field(f, 1);
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

        // Apply the join as a membership event.
        MemberEvent join_ev;
        join_ev.type = EventType::Join;
        join_ev.node_id = sender_id;
        join_ev.host = host;
        join_ev.port = port;
        join_ev.incarnation = incarnation;
        if (swim_.applyEvent(join_ev)) {
            swim_.enqueueEvent(join_ev);
        }

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
    oss << "SWIM_ACK|" << self_.node_id << "|" << target_id << "|" << serializeEvents(events);
    return oss.str();
}

void GossipThread::applyIncomingEvents(const std::string &events_field) {
    if (events_field.empty()) return;
    auto events = deserializeEvents(events_field);
    for (auto &ev : events) {
        if (swim_.applyEvent(ev)) {
            swim_.enqueueEvent(ev);
        }
    }
}

}  // namespace gossip
