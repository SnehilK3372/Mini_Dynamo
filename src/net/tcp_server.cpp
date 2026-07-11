#include "tcp_server.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <thread>

#include "framing.h"
using namespace std;

TCPServer::TCPServer(const string &host_, int port_, Node *node_)
    : host(host_), port(port_), node(node_) {}

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

    if (listen(server_fd, 10) < 0) {
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

        thread(&TCPServer::handleClient, this, new_socket).detach();
    }
}

void TCPServer::handleClient(int client_fd) {  // reads one framed request, dispatches, replies
    string payload;
    if (!framing::recvFramed(client_fd, payload)) {
        close(client_fd);
        return;
    }

    node->handleRequest(payload, client_fd);

    close(client_fd);
}
