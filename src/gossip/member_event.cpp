#include "member_event.h"

#include <sstream>

namespace gossip {

std::string MemberEvent::serialize() const {
    std::ostringstream oss;
    oss << static_cast<char>(type) << ':' << node_id << ':' << host << ':' << port << ':'
        << incarnation;
    return oss.str();
}

MemberEvent MemberEvent::deserialize(const std::string &s) {
    MemberEvent e;
    if (s.size() < 3 || s[1] != ':') return e;
    e.type = static_cast<EventType>(s[0]);

    size_t p1 = 2;
    size_t p2 = s.find(':', p1);
    if (p2 == std::string::npos) return e;
    e.node_id = s.substr(p1, p2 - p1);

    p1 = p2 + 1;
    p2 = s.find(':', p1);
    if (p2 == std::string::npos) return e;
    e.host = s.substr(p1, p2 - p1);

    p1 = p2 + 1;
    p2 = s.find(':', p1);
    if (p2 == std::string::npos) return e;
    try {
        e.port = static_cast<uint16_t>(std::stoi(s.substr(p1, p2 - p1)));
    } catch (...) {
        e.port = 0;
    }

    p1 = p2 + 1;
    try {
        e.incarnation = std::stoull(s.substr(p1));
    } catch (...) {
        e.incarnation = 0;
    }
    return e;
}

std::string serializeEvents(const std::vector<MemberEvent> &events) {
    std::ostringstream oss;
    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) oss << ';';
        oss << events[i].serialize();
    }
    return oss.str();
}

std::vector<MemberEvent> deserializeEvents(const std::string &s) {
    std::vector<MemberEvent> out;
    if (s.empty()) return out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(';', start);
        std::string token = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        if (!token.empty()) {
            out.push_back(MemberEvent::deserialize(token));
        }
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

}  // namespace gossip
