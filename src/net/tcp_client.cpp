#include "tcp_client.h"

#include <unistd.h>

#include "framing.h"
#include "tcp_connect.h"

using namespace std;

string TCPClient::sendAndReceiveFramed(const string &host, int port, const string &data,
                                       int timeout_ms) {
    // One-shot: connect and read share the same deadline.
    int sock = tcpconnect::dial(host, port, timeout_ms, timeout_ms);
    if (sock < 0) return "";

    if (!framing::sendFramed(sock, data)) {
        close(sock);
        return "";
    }

    string reply;
    if (!framing::recvFramed(sock, reply)) {
        close(sock);
        return "";
    }
    close(sock);
    return reply;
}
