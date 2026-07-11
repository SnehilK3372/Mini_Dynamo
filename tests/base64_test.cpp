#include "base64.h"

#include <gtest/gtest.h>

#include <string>

using std::string;

TEST(Base64, RoundTripsAsciiAndEmpty) {
    for (const string &s :
         {string(""), string("a"), string("ab"), string("abc"), string("hello world")}) {
        EXPECT_EQ(base64::decode(base64::encode(s)), s);
    }
}

TEST(Base64, RoundTripsDelimiterAndControlBytes) {
    // The whole reason base64 is here: values that contain the wire delimiters
    // must survive encoding so the pipe-split protocol stays unambiguous.
    string s = "a|b|c\nline2\r\twith\ttabs";
    string enc = base64::encode(s);
    EXPECT_EQ(enc.find('|'), string::npos);
    EXPECT_EQ(enc.find('\n'), string::npos);
    EXPECT_EQ(base64::decode(enc), s);
}

TEST(Base64, RoundTripsBinaryIncludingNul) {
    string s;
    for (int i = 0; i < 256; ++i) s.push_back(static_cast<char>(i));
    EXPECT_EQ(base64::decode(base64::encode(s)), s);
}

TEST(Base64, KnownVector) {
    EXPECT_EQ(base64::encode("Man"), "TWFu");
    EXPECT_EQ(base64::encode("Ma"), "TWE=");
    EXPECT_EQ(base64::encode("M"), "TQ==");
}
