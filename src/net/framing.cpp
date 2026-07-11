#include "framing.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace framing {

namespace {
// Write the whole buffer, tolerating partial writes and EINTR.
bool writeAll(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read exactly len bytes, tolerating partial reads and EINTR.
bool readAll(int fd, char *data, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, data + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // peer closed mid-frame
        got += static_cast<size_t>(n);
    }
    return true;
}
}  // namespace

bool sendFramed(int fd, const std::string &payload) {
    std::string header = std::to_string(payload.size());
    header.push_back('\n');
    if (!writeAll(fd, header.data(), header.size())) return false;
    return writeAll(fd, payload.data(), payload.size());
}

bool recvFramed(int fd, std::string &out) {
    // Read the decimal length line one byte at a time. Headers are short (a few
    // digits), so this is cheap and avoids the read-ahead buffering that would
    // be needed to un-read payload bytes grabbed past the newline.
    std::string lenStr;
    char c;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // EOF before we saw a full header
        if (c == '\n') break;
        if (c < '0' || c > '9') return false;  // malformed header
        lenStr.push_back(c);
        if (lenStr.size() > 20) return false;  // absurdly long → bail
    }
    if (lenStr.empty()) return false;

    unsigned long long len = std::strtoull(lenStr.c_str(), nullptr, 10);
    if (len > kMaxFrame) return false;

    out.resize(static_cast<size_t>(len));
    if (len == 0) return true;
    return readAll(fd, &out[0], static_cast<size_t>(len));
}

}  // namespace framing
