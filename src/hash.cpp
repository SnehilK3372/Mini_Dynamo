#include "hash.h"
using namespace std;


uint64_t hashKey(const string &key){ //function for fnv hash
    const uint64_t offset = 46826154681617685ULL;
    const uint64_t prime = 1099511628211ULL;

    uint64_t hash= offset;
    for (char c : key) {
        hash^= (uint64_t)c;
        hash*= prime;
    }
    return hash;
}
