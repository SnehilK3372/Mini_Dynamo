#include "message.h"

// Format: TYPE|KEY|VALUE|ORIGIN
// Note: KEY and VALUE should not contain '|'.
// For simplicity of a student project, this is acceptable.

std::string Message::serialize() const {
    std::ostringstream oss;
    oss << type << "|" << key << "|" << value << "|" << origin;
    if(type == "JOIN") {
            oss << "|" << host << "|" << port;
        }
    return oss.str();
}

static std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> parts;
    std::string temp;
    for (char c : s) {
        if (c == delim) {
            parts.push_back(temp);
            temp.clear();
        } else temp.push_back(c);
    }
    parts.push_back(temp);
    return parts;
}

Message Message::deserialize(const std::string& data) {
        Message msg;
        // (Simplified split logic for brevity)
        std::vector<std::string> parts;
        std::stringstream ss(data);
        std::string item;
        while (std::getline(ss, item, '|')) parts.push_back(item);

        // Guard against empty messages
        if (parts.size() >= 1) msg.type = parts[0];
        if (parts.size() >= 2) msg.key = parts[1];
        if (parts.size() >= 3) msg.value = parts[2];
        if (parts.size() >= 4) msg.origin = parts[3];
        if (parts.size() >= 5) msg.host = parts[4];
        
        // Safe integer parsing to prevent crash
        if (parts.size() >= 6 && !parts[5].empty()) {
            try { msg.port = std::stoi(parts[5]); } catch(...) { msg.port = 0; }
        }
        return msg;
    }


