#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../node_info.h"

namespace gossip {

// Membership event types disseminated via SWIM piggyback.
enum class EventType : char {
    Join = 'J',     // node joined the cluster
    Suspect = 'S',  // node suspected unreachable
    Dead = 'D',     // node confirmed dead (removed from ring)
    Leave = 'L',    // node gracefully departed
    Alive = 'A'     // node refuted suspicion (incremented incarnation)
};

struct MemberEvent {
    // Default-initialized: deserialize() returns a partially-filled event on
    // malformed input (empty node_id, which callers drop), and an indeterminate
    // enum read there would be UB.
    EventType type = EventType::Alive;
    std::string node_id;
    std::string host;
    uint16_t port = 0;
    uint64_t incarnation = 0;

    // Compact text serialization for piggybacking on SWIM messages.
    // Format: "TYPE:node_id:host:port:incarnation"
    // For Suspect/Dead/Leave/Alive the host:port fields are empty (already known).
    std::string serialize() const;
    static MemberEvent deserialize(const std::string &s);
};

// A list of events piggybacked on a single SWIM message.
std::string serializeEvents(const std::vector<MemberEvent> &events);
std::vector<MemberEvent> deserializeEvents(const std::string &s);

}  // namespace gossip
