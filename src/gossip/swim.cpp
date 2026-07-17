#include "swim.h"

#include <algorithm>
#include <cmath>

namespace gossip {

Swim::Swim(const NodeInfo &self, int suspicion_mult)
    : self_(self), suspicion_mult_(suspicion_mult), rng_(std::random_device{}()) {}

int Swim::disseminationLimit() const {
    // Each event is piggybacked on 3*ceil(log2(N+1)) outgoing messages, ensuring
    // every member hears it with high probability within O(log N) protocol rounds.
    size_t n = members_.size() + 1;
    return std::max(3, static_cast<int>(3 * std::ceil(std::log2(n + 1))));
}

bool Swim::applyEvent(const MemberEvent &event) {
    std::lock_guard<std::mutex> lk(mtx_);

    // Self-related events: if someone suspects us, refute.
    if (event.node_id == self_.node_id) {
        if (event.type == EventType::Suspect || event.type == EventType::Dead) {
            if (event.incarnation < incarnation_) {
                // Stale suspicion — it targets an older incarnation we have
                // already superseded, so our current Alive already refutes it.
                // A suspicion at our *current* incarnation is still live and must
                // be refuted (bumped past), per the SWIM refutation rule.
                return false;
            }
            // Refute: bump strictly past the suspected incarnation and broadcast Alive.
            incarnation_ = event.incarnation + 1;
            MemberEvent alive;
            alive.type = EventType::Alive;
            alive.node_id = self_.node_id;
            alive.host = self_.host;
            alive.port = self_.port;
            alive.incarnation = incarnation_;
            pending_events_.push_back({alive, disseminationLimit()});
            return true;
        }
        return false;
    }

    auto it = members_.find(event.node_id);

    switch (event.type) {
        case EventType::Join:
        case EventType::Alive: {
            if (it == members_.end()) {
                // New member.
                MemberEntry entry;
                entry.info = {event.node_id, event.host, event.port};
                entry.state = MemberState::Alive;
                entry.incarnation = event.incarnation;
                members_[event.node_id] = entry;
                fireCallbacks(entry.info, MemberState::Alive);
                return true;
            }
            // A Join is the node speaking for ITSELF through the join handshake —
            // authoritative proof it is up, whatever we remember about it. Accept it
            // unconditionally, adopting its fresh incarnation and address.
            //
            // This must not be gated on incarnation: a restarted process starts again
            // at incarnation 0, while we still hold it Dead at >= 0, so a
            // "strictly higher" rule strands a healthy node as Dead *forever* — it
            // gets no traffic, and (because hinted handoff fires on Dead -> Alive) its
            // hints are never delivered and silently expire. The incarnation guard
            // exists to stop STALE THIRD-PARTY GOSSIP (relayed Alive events) from
            // resurrecting a node that really is gone; a direct join is not that.
            if (event.type == EventType::Join && it->second.state == MemberState::Dead) {
                it->second.state = MemberState::Alive;
                it->second.incarnation = event.incarnation;
                if (!event.host.empty()) {
                    it->second.info.host = event.host;
                    it->second.info.port = event.port;
                }
                fireCallbacks(it->second.info, MemberState::Alive);
                return true;
            }
            // Relayed liveness (Alive), or a Join for a node we already track as
            // live: only a strictly newer incarnation may override what we hold.
            if (it->second.state == MemberState::Dead) {
                if (event.incarnation <= it->second.incarnation) return false;
            } else if (it->second.state == MemberState::Alive) {
                if (event.incarnation <= it->second.incarnation) return false;
            }
            // Suspect can be overridden by same-or-higher incarnation alive.
            if (event.incarnation >= it->second.incarnation) {
                it->second.state = MemberState::Alive;
                it->second.incarnation = event.incarnation;
                if (!event.host.empty()) {
                    it->second.info.host = event.host;
                    it->second.info.port = event.port;
                }
                fireCallbacks(it->second.info, MemberState::Alive);
                return true;
            }
            return false;
        }

        case EventType::Suspect: {
            if (it == members_.end()) return false;
            // A suspect with a higher incarnation overrides alive.
            if (event.incarnation < it->second.incarnation) return false;
            if (it->second.state == MemberState::Dead) return false;
            if (it->second.state == MemberState::Suspect &&
                event.incarnation <= it->second.incarnation) {
                return false;
            }
            it->second.state = MemberState::Suspect;
            it->second.incarnation = event.incarnation;
            it->second.suspect_since = std::chrono::steady_clock::now();
            return true;
        }

        case EventType::Dead:
        case EventType::Leave: {
            if (it == members_.end()) return false;
            if (it->second.state == MemberState::Dead) return false;
            if (event.incarnation < it->second.incarnation) return false;
            it->second.state = MemberState::Dead;
            it->second.incarnation = event.incarnation;
            fireCallbacks(it->second.info, MemberState::Dead);
            return true;
        }

        default:
            return false;
    }
}

void Swim::suspect(const std::string &node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = members_.find(node_id);
    if (it == members_.end() || it->second.state != MemberState::Alive) return;

    it->second.state = MemberState::Suspect;
    it->second.suspect_since = std::chrono::steady_clock::now();

    MemberEvent ev;
    ev.type = EventType::Suspect;
    ev.node_id = node_id;
    ev.incarnation = it->second.incarnation;
    pending_events_.push_back({ev, disseminationLimit()});
}

void Swim::confirmDead(const std::string &node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = members_.find(node_id);
    if (it == members_.end() || it->second.state == MemberState::Dead) return;

    it->second.state = MemberState::Dead;

    MemberEvent ev;
    ev.type = EventType::Dead;
    ev.node_id = node_id;
    ev.incarnation = it->second.incarnation;
    pending_events_.push_back({ev, disseminationLimit()});

    fireCallbacks(it->second.info, MemberState::Dead);
}

std::vector<NodeInfo> Swim::alivePeers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<NodeInfo> out;
    for (const auto &[id, entry] : members_) {
        if (entry.state == MemberState::Alive || entry.state == MemberState::Suspect) {
            out.push_back(entry.info);
        }
    }
    return out;
}

std::vector<MemberEntry> Swim::allMembers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MemberEntry> out;
    for (const auto &[id, entry] : members_) {
        if (entry.state != MemberState::Dead) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<NodeInfo> Swim::randomPeers(int k, const std::vector<std::string> &exclude) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<NodeInfo> candidates;
    for (const auto &[id, entry] : members_) {
        if (entry.state == MemberState::Dead) continue;
        if (std::find(exclude.begin(), exclude.end(), id) != exclude.end()) continue;
        candidates.push_back(entry.info);
    }
    std::shuffle(candidates.begin(), candidates.end(), rng_);
    if ((int)candidates.size() > k) candidates.resize(k);
    return candidates;
}

std::vector<MemberEvent> Swim::getEventsToSend() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MemberEvent> out;
    for (auto it = pending_events_.begin(); it != pending_events_.end();) {
        out.push_back(it->event);
        --(it->remaining);
        if (it->remaining <= 0) {
            it = pending_events_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

void Swim::enqueueEvent(const MemberEvent &event) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_events_.push_back({event, disseminationLimit()});
}

uint64_t Swim::refute() {
    std::lock_guard<std::mutex> lk(mtx_);
    ++incarnation_;
    MemberEvent ev;
    ev.type = EventType::Alive;
    ev.node_id = self_.node_id;
    ev.host = self_.host;
    ev.port = self_.port;
    ev.incarnation = incarnation_;
    pending_events_.push_back({ev, disseminationLimit()});
    return incarnation_;
}

bool Swim::isAlive(const std::string &node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = members_.find(node_id);
    if (it == members_.end()) return false;
    return it->second.state == MemberState::Alive || it->second.state == MemberState::Suspect;
}

std::vector<std::string> Swim::expireSuspects(std::chrono::milliseconds suspicion_timeout) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> expired;
    auto now = std::chrono::steady_clock::now();
    for (auto &[id, entry] : members_) {
        if (entry.state == MemberState::Suspect) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.suspect_since);
            if (elapsed >= suspicion_timeout) {
                entry.state = MemberState::Dead;
                expired.push_back(id);

                MemberEvent ev;
                ev.type = EventType::Dead;
                ev.node_id = id;
                ev.incarnation = entry.incarnation;
                pending_events_.push_back({ev, disseminationLimit()});

                fireCallbacks(entry.info, MemberState::Dead);
            }
        }
    }
    return expired;
}

size_t Swim::memberCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t count = 0;
    for (const auto &[id, entry] : members_) {
        if (entry.state != MemberState::Dead) ++count;
    }
    return count;
}

void Swim::onMemberChange(MemberChangeCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    callbacks_.push_back(std::move(cb));
}

uint64_t Swim::incarnation() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return incarnation_;
}

void Swim::fireCallbacks(const NodeInfo &info, MemberState state) {
    for (auto &cb : callbacks_) {
        cb(info, state);
    }
}

}  // namespace gossip
