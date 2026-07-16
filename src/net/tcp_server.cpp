#include "tcp_server.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "framing.h"
using namespace std;

TCPServer::TCPServer(const string &host_, int port_, Node *node_, size_t workers,
                     int idle_timeout_ms)
    : host(host_),
      port(port_),
      node(node_),
      idle_timeout_ms_(idle_timeout_ms),
      pool_(make_unique<ThreadPool>(workers)) {}

void TCPServer::start() {  // init a tcp connection at given port associated with the node
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(host.c_str());
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 1024) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // The listening banner is emitted as a structured jlog line from main.cpp
    // ("TCP server listening on ..."); a second plain-text line here would be the
    // only non-JSON thing on stdout, so it's intentionally omitted.

    while (true) {
        int new_socket;
        socklen_t addrlen = sizeof(address);
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) < 0) {
            perror("accept");
            continue;
        }

        // Dispatch to the worker pool instead of spawning a thread per connection.
        // If the pool is shutting down (enqueue fails), close the socket so it
        // isn't leaked.
        int fd = new_socket;
        if (!pool_->enqueue([this, fd] { handleConnection(fd); })) {
            close(fd);
        }
    }
}

void TCPServer::handleConnection(int client_fd) {
    // Bound idle time: an idle-but-alive pooled connection is kept, but a peer
    // that vanishes without a FIN is eventually reaped when recv times out. Set
    // longer than the client-side pool's idle reap so the client closes first in
    // the common case.
    timeval tv{};
    tv.tv_sec = idle_timeout_ms_ / 1000;
    tv.tv_usec = (idle_timeout_ms_ % 1000) * 1000;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Serve framed requests back-to-back on the same connection until the peer
    // closes it (recvFramed returns false on EOF, timeout, or a malformed frame).
    // handleRequest writes the framed reply itself. A one-shot client (e.g. the
    // Java gateway) simply sends one frame and closes, which this loop handles as
    // one iteration followed by an EOF.
    string payload;
    while (framing::recvFramed(client_fd, payload)) {
        node->handleRequest(payload, client_fd);
    }
    close(client_fd);
}
