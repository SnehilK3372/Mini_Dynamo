#ifndef MESSAGE_H
#define MESSAGE_H

#include <string>
#include <sstream>
#include <vector>
#include <cstdint>

struct Message {
    std::string type;
    std::string key;
    std::string value;
    std::string origin;
    std::string host;   // only used for JOIN messages
    uint16_t port;

    std::string serialize() const;
    static Message deserialize(const std::string &data);
};

#endif
