#pragma once
#include <string>

class TCPClient {
public:
    bool send(const std::string &host, int port, const std::string &data);
    std::string sendAndReceive(const std::string &host, int port, const std::string &data);
};