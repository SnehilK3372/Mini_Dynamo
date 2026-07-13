#include "tcp_connect.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace tcpconnect {

namespace {
// Connect with a bounded wait: put the socket in non-blocking mode, start the
// connect, and select() until it completes or the timeout elapses. Without this,
// a peer that is down (host unreachable) would block the caller for the kernel's
// default connect timeout — tens of seconds — defeating the caller's own
// per-request deadline.
bool connectWithTimeout(int sock, const sockaddr *addr, socklen_t addrlen, int timeout_ms) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(sock, addr, addrlen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return true;  // connected immediately (loopback)
    }
    if (errno != EINPROGRESS) return false;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(sock, &wset);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = ::select(sock + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) return false;  // timeout or error

    int soErr = 0;
    socklen_t len = sizeof(soErr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) return false;

    fcntl(sock, F_SETFL, flags);  // back to blocking for framed send/recv
    return true;
}
}  // namespace

int dial(const std::string &host, int port, int timeout_ms, int io_timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0 || res == nullptr) {
        std::cerr << "[tcp_connect] resolve failed for " << host << ": " << gai_strerror(err)
                  << "\n";
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (!connectWithTimeout(sock, res->ai_addr, res->ai_addrlen, timeout_ms)) {
        freeaddrinfo(res);
        close(sock);
        return -1;
    }
    freeaddrinfo(res);

    timeval tv{};
    tv.tv_sec = io_timeout_ms / 1000;
    tv.tv_usec = (io_timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return sock;
}

}  // namespace tcpconnect
