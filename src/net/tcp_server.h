#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "../message.h"
#include "../node.h"
#include "thread_pool.h"

class TCPServer {
   public:
    // `workers` bounds the number of concurrent connection handlers (0 → a small
    // default). Each accepted connection is dispatched to the pool and served
    // until the client closes it, so the pool is sized for concurrent
    // connections, not requests.
    TCPServer(const std::string &host_, int port_, Node *node_, size_t workers = 64,
              int idle_timeout_ms = 120000);
    void start();

   private:
    std::string host;
    int port;
    Node *node;
    int idle_timeout_ms_;  // SO_RCVTIMEO on accepted sockets (idle persistent conn bound)
    std::unique_ptr<ThreadPool> pool_;

    // Serve one connection: loop reading framed requests and writing replies until
    // the peer closes it or an idle/read timeout fires, then close.
    void handleConnection(int client_fd);
};
