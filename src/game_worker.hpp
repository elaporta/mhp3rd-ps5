// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <pthread.h>

namespace yakumo::game_worker {
inline constexpr std::size_t stack_bytes = 64u * 1024u * 1024u;
struct StackResult {
    std::size_t bytes{};
    int init{}, query{}, size{}, destroy{};
    bool valid() const { return !init && !query && !size && !destroy && bytes >= stack_bytes; }
};
// FreeBSD/SDK pthread_attr_t is an allocated object: get_np requires init.
// Keep every return code, including destruction, visible to the caller.
template<class Api> StackResult inspect_stack(Api& api) {
    StackResult result;
    pthread_attr_t attributes;
    result.init = api.init(&attributes);
    if (result.init) return result;
    result.query = api.query(&attributes);
    if (!result.query) result.size = api.size(&attributes, &result.bytes);
    result.destroy = api.destroy(&attributes);
    return result;
}
}
