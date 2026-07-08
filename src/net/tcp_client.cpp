#include "tcp_client.h"
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstring>
#include <vector>

using namespace std;

bool TCPClient::send(const string &host, int port, const string &data) {
    int sock = 0;
    struct addrinfo hints{}, *res = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    string port_str = to_string(port);
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0 || res == nullptr) {
        cerr << "Invalid address / hostname resolution failed: " << host
                  << " (" << gai_strerror(err) << ")\n";
        return false;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        cerr << "Client socket creation error\n";
        freeaddrinfo(res);
        return false;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        cerr << "Connection failed to " << host << ":" << port << "\n";
        freeaddrinfo(res);
        close(sock);
        return false;
    }

    freeaddrinfo(res);

    ssize_t sent = ::send(sock, data.c_str(), data.size(), 0);
    if (sent < 0) {
        cerr << "Send failed\n";
        close(sock);
        return false;
    }

    close(sock);
    return true;
}


string TCPClient::sendAndReceive(const string &host, int port, const string &data) {
    int sock = 0;
    struct addrinfo hints{}, *res = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    string port_str = to_string(port);
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0 || res == nullptr) {
        cerr << "Invalid address" << host<< " (" << gai_strerror(err) << ")\n";
        return ""; 
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        cerr << "Client socket creation error\n";
        freeaddrinfo(res);
        return "";
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        cerr << "Connection failed to " << host << ":" << port << "\n";
        freeaddrinfo(res);
        close(sock);
        return "";
    }

    freeaddrinfo(res);

    ssize_t sent = ::send(sock, data.c_str(), data.size(), 0);
    if (sent < 0) {
        cerr << "Send failed\n";
        close(sock);
        return "";
    }

    vector<char> buf(4096);
    ssize_t n = ::read(sock, buf.data(), buf.size() - 1);
    
    close(sock);

    if (n > 0) {
        buf[n] = '\0';
        return string(buf.data());
    } else if (n < 0) {
        cerr << "Read failed\n";
        return "";
    }
    return "";
}