#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <iterator>

namespace RT64 {
inline bool fastContainsGpuBytes(const std::map<uint64_t,uint64_t> &ranges,uint64_t begin,uint64_t end) {
    if(begin>=end)return false;
    const auto it=ranges.upper_bound(begin);
    return it!=ranges.begin() && std::prev(it)->second>=end;
}
inline void fastMarkGpuBytes(std::map<uint64_t,uint64_t> &ranges,uint64_t begin,uint64_t end) {
    if(begin>=end)return;
    // Include an existing range with the same start in the containment check;
    // repeatedly drawing that region must not allocate another identical node.
    auto it=ranges.upper_bound(begin);
    if(it!=ranges.begin()) {
        auto previous=std::prev(it);
        if(previous->second>=end)return;
        if(previous->second>=begin) { begin=previous->first;it=ranges.erase(previous); }
    }
    while(it!=ranges.end() && it->first<=end) { end=std::max(end,it->second);it=ranges.erase(it); }
    ranges.emplace(begin,end);
}
}
