#pragma once

#include <string>
#include "../message.h"
#include "../node.h"

class TCPServer{
public:
    TCPServer(const std::string &host_, int port_, Node *node_);
    void start();

private:
    std::string host;
    int port;
    Node *node;

    void handleClient(int client_fd);
};
