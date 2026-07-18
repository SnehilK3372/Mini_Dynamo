#include "swim.h"

#include <algorithm>
#include <cmath>

#include "../util.h"

namespace gossip {

namespace {

// One membership entry rendered into hash64. '\x01' separators prevent
// (id="ab", inc=1) and (id="a", ...) style ambiguities; the state class is one
// char: 'A' alive-or-suspect, 'D' dead, 'L' left.
uint64_t entryDigest(const std::string &id, char state_class, uint64_t incarnation) {
    std::string buf;
    buf.reserve(id.size() + 12);
    buf += id;
    buf += '\x01';
    buf += state_class;
    buf += '\x01';
    for (int i = 0; i < 8; ++i) buf += static_cast<char>((incarnation >> (8 * i)) & 0xff);
    return hash64(buf);
}

char stateClass(MemberState s) {
    switch (s) {
        case MemberState::Dead:
            return 'D';
        case MemberState::Left:
            return 'L';
        default:
            return 'A';  // Alive and Suspect collapse — suspicion is transient
    }
}

}  // namespace

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
        if (event.type == EventType::Leave) {
            // The one thing we do NOT argue with. Suspect/Dead about us are
            // *inferences* a healthy node is entitled to refute; a Leave is an
            // operator's decision, and a node refuting its own retirement would
            // make decommission impossible for any node still running — exactly
            // the case where you need it to work.
            if (self_left_) return false;  // already retired: stop re-disseminating
            self_left_ = true;
            fireCallbacks(self_, MemberState::Left);  // drop ourselves from our own ring
            return true;                              // pass the order on, once
        }
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
            // A departed node stays departed. Checked BEFORE the incarnation
            // comparison because the tombstone outranks incarnation entirely: a
            // decommissioned node that is still running keeps gossiping Alive, and
            // will happily refute its way to an arbitrarily high incarnation. No
            // incarnation may buy its way back in.
            if (it->second.state == MemberState::Left) return false;

            // Everything here is RELAYED gossip — a third party telling us about
            // someone else — so it only wins with a STRICTLY newer incarnation,
            // whatever state we hold. (The join handshake, where the node speaks
            // for itself, goes through applyDirectJoin instead.)
            //
            // Strictly-newer matters for Suspect in particular. This used to accept
            // same-incarnation (`>=`), which meant a stale relayed Alive/Join at the
            // node's current incarnation cleared suspicion. Worse, gossip re-enqueues
            // any event that changed state, so that event was re-disseminated, came
            // back, cleared suspicion again, and never died: a failed node could be
            // resurrected forever and never be declared Dead. Only the suspected node
            // itself may refute, and it does so by bumping its own incarnation —
            // which is strictly newer, so it still wins here.
            if (event.incarnation <= it->second.incarnation) return false;

            it->second.state = MemberState::Alive;
            it->second.incarnation = event.incarnation;
            if (!event.host.empty()) {
                it->second.info.host = event.host;
                it->second.info.port = event.port;
            }
            fireCallbacks(it->second.info, MemberState::Alive);
            return true;
        }

        case EventType::Suspect: {
            if (it == members_.end()) return false;
            // A suspect with a higher incarnation overrides alive.
            if (event.incarnation < it->second.incarnation) return false;
            if (it->second.state == MemberState::Dead) return false;
            if (it->second.state == MemberState::Left) return false;  // terminal
            if (it->second.state == MemberState::Suspect &&
                event.incarnation <= it->second.incarnation) {
                return false;
            }
            it->second.state = MemberState::Suspect;
            it->second.incarnation = event.incarnation;
            it->second.suspect_since = std::chrono::steady_clock::now();
            return true;
        }

        case EventType::Dead: {
            if (it == members_.end()) {
                // Record a Dead for a node we never knew, instead of dropping it
                // (mirror of the unknown-Leave case below). Dropping was harmless
                // when events were fire-and-forget; with digest-based sync (Tier
                // 4.7) it made "I hold X:Dead, you never heard of X" an
                // *unresolvable* digest mismatch — every comparison differs,
                // every sync replays the same event, nothing converges. No ring
                // callback: the node was never Alive here, so there is nothing to
                // add or evict.
                MemberEntry entry;
                entry.info = {event.node_id, event.host, event.port};
                entry.state = MemberState::Dead;
                entry.incarnation = event.incarnation;
                members_[event.node_id] = entry;
                return true;
            }
            if (it->second.state == MemberState::Dead) return false;
            if (it->second.state == MemberState::Left) return false;  // terminal
            if (event.incarnation < it->second.incarnation) return false;
            it->second.state = MemberState::Dead;
            it->second.incarnation = event.incarnation;
            fireCallbacks(it->second.info, MemberState::Dead);
            return true;
        }

        case EventType::Leave: {
            // Deliberately NOT a synonym for Dead, and deliberately not gated on
            // incarnation — the two differences that make this whole tier work.
            //
            //   - Not Dead: Dead keeps the node's ring slots (a transient failure
            //     must not reshuffle ownership); Leave surrenders them. The
            //     Left callback is what evicts it from the ring.
            //   - Not gated: every other event loses to a newer incarnation. An
            //     operator's decision is not a stale observation to be outvoted,
            //     and gating it would make decommission fail against a flapping
            //     node — the very node you are trying to retire.
            //
            // Terminal, so it converges: the first Leave changes state and is
            // re-disseminated, every later one returns false and dies out.
            if (it == members_.end()) {
                // We have never heard of this node — record the tombstone anyway,
                // rather than dropping the event. Otherwise a Leave that overtakes
                // the Alive it refers to (possible during a join race, since both
                // are racing through the same dissemination stream) would leave us
                // with no tombstone, and the trailing Alive would add a departed
                // node to our ring alone. host/port are empty on a Leave and are
                // never needed: a Left entry is excluded from every peer-selection
                // path and exists only to be checked.
                MemberEntry entry;
                entry.info = {event.node_id, event.host, event.port};
                entry.state = MemberState::Left;
                entry.incarnation = event.incarnation;
                members_[event.node_id] = entry;
                return true;  // relay it onward; nothing to evict locally
            }
            if (it->second.state == MemberState::Left) return false;
            it->second.state = MemberState::Left;
            fireCallbacks(it->second.info, MemberState::Left);
            return true;
        }

        default:
            return false;
    }
}

std::optional<uint64_t> Swim::applyDirectJoin(const MemberEvent &event) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (event.node_id == self_.node_id) return incarnation_;  // never about ourselves

    auto it = members_.find(event.node_id);
    if (it == members_.end()) {
        MemberEntry entry;
        entry.info = {event.node_id, event.host, event.port};
        entry.state = MemberState::Alive;
        entry.incarnation = event.incarnation;
        members_[event.node_id] = entry;
        fireCallbacks(entry.info, MemberState::Alive);
        return event.incarnation;
    }

    // Believe a node about its own liveness — EXCEPT when it has been retired.
    //
    // This is the one check the whole decommission design rests on. Everywhere
    // else, a stale revival is stopped by the incarnation guard; here there is no
    // incarnation guard by design (a restarted process returns at 0 and must still
    // be believed — that is the Tier-testing rejoin fix). So without this line a
    // decommissioned node that is merely *restarted* would hand itself straight
    // back into the ring, and the operator's decision would silently evaporate.
    //
    // nullopt, not an incarnation: the caller must disseminate nothing at all.
    if (it->second.state == MemberState::Left) return std::nullopt;

    // The node is telling us directly that it is up, so believe it. Bump past any
    // record we hold, so the Alive we disseminate is strictly newer than the
    // Dead/Suspect our peers may be holding — otherwise they would reject it and
    // the node would come back for us but stay invisible to everyone else.
    uint64_t eff = std::max(event.incarnation, it->second.incarnation + 1);
    bool changed = it->second.state != MemberState::Alive;
    it->second.state = MemberState::Alive;
    it->second.incarnation = eff;
    if (!event.host.empty()) {
        it->second.info.host = event.host;
        it->second.info.port = event.port;
    }
    if (changed) fireCallbacks(it->second.info, MemberState::Alive);
    return eff;
}

void Swim::leave(const std::string &node_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    MemberEvent ev;
    ev.type = EventType::Leave;
    ev.node_id = node_id;

    // Retiring ourselves: there is no members_ entry for self, so the flag is the
    // whole state. Peers learn from the disseminated event.
    if (node_id == self_.node_id) {
        if (self_left_) return;
        self_left_ = true;
        ev.incarnation = incarnation_;
        pending_events_.push_back({ev, disseminationLimit()});
        fireCallbacks(self_, MemberState::Left);
        return;
    }

    auto it = members_.find(node_id);
    if (it == members_.end()) return;                   // unknown node: nothing to retire
    if (it->second.state == MemberState::Left) return;  // already done, stay quiet

    // No incarnation or state guard: an operator may retire a node we hold Alive,
    // Suspect or Dead. Dead is in fact the common case — reclaiming the ring slots
    // of a node that is never coming back is the reason this exists.
    it->second.state = MemberState::Left;
    ev.incarnation = it->second.incarnation;
    pending_events_.push_back({ev, disseminationLimit()});

    fireCallbacks(it->second.info, MemberState::Left);
}

bool Swim::hasLeft() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return self_left_;
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
    // Left is terminal, and demoting it to Dead would put the node back in the
    // ring on the next Alive it manages to get accepted.
    if (it->second.state == MemberState::Left) return;

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

std::vector<NodeInfo> Swim::deadPeers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<NodeInfo> out;
    for (const auto &[id, entry] : members_) {
        if (entry.state == MemberState::Dead) out.push_back(entry.info);
    }
    return out;
}

std::vector<MemberEntry> Swim::allMembers() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MemberEntry> out;
    for (const auto &[id, entry] : members_) {
        if (entry.state != MemberState::Dead && entry.state != MemberState::Left) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<NodeInfo> Swim::randomPeers(int k, const std::vector<std::string> &exclude) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<NodeInfo> candidates;
    for (const auto &[id, entry] : members_) {
        if (entry.state == MemberState::Dead || entry.state == MemberState::Left) continue;
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

std::optional<MemberState> Swim::stateOf(const std::string &node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (node_id == self_.node_id) {
        return self_left_ ? MemberState::Left : MemberState::Alive;
    }
    auto it = members_.find(node_id);
    if (it == members_.end()) return std::nullopt;
    return it->second.state;
}

uint64_t Swim::digest() const {
    std::lock_guard<std::mutex> lk(mtx_);
    // XOR-combined per-entry hashes: order-independent by construction, so it is
    // identical across nodes regardless of how each stores or iterates its view.
    // Self is an entry like any other — that is what makes the stranded-partition
    // case detectable: MY entry for me says Alive while YOUR entry for me says
    // Dead, so our digests differ and one of us initiates the sync that fixes it.
    //
    // Terminal states (Dead/Left) digest CLASS-ONLY — incarnation zeroed. Two
    // nodes legitimately hold the same dead member at different incarnations
    // (suspicions expire at different moments; applying a Leave keeps the local
    // incarnation), and syncing cannot reconcile the number: both sides would
    // mismatch forever while exchanging payloads that change nothing. Class
    // convergence is what matters; the held incarnation still gates revival
    // locally, and any class *flip* (say a stale Alive reviving one side) makes
    // the digests differ again, which is exactly when a sync helps.
    uint64_t h = entryDigest(self_.node_id, self_left_ ? 'L' : 'A', self_left_ ? 0 : incarnation_);
    for (const auto &[id, entry] : members_) {
        char cls = stateClass(entry.state);
        h ^= entryDigest(id, cls, cls == 'A' ? entry.incarnation : 0);
    }
    return h;
}

std::vector<MemberEvent> Swim::fullState() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<MemberEvent> out;
    out.reserve(members_.size() + 1);

    MemberEvent self_ev;
    self_ev.type = self_left_ ? EventType::Leave : EventType::Alive;
    self_ev.node_id = self_.node_id;
    self_ev.host = self_.host;
    self_ev.port = self_.port;
    self_ev.incarnation = incarnation_;
    out.push_back(self_ev);

    for (const auto &[id, entry] : members_) {
        MemberEvent ev;
        switch (entry.state) {
            case MemberState::Dead:
                ev.type = EventType::Dead;
                break;
            case MemberState::Left:
                ev.type = EventType::Leave;
                break;
            default:
                // Suspect travels as Alive: suspicion is this node's private
                // judgement mid-probe, not a fact to replicate.
                ev.type = EventType::Alive;
                break;
        }
        ev.node_id = id;
        ev.host = entry.info.host;
        ev.port = entry.info.port;
        ev.incarnation = entry.incarnation;
        out.push_back(ev);
    }
    return out;
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
        if (entry.state != MemberState::Dead && entry.state != MemberState::Left) ++count;
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
