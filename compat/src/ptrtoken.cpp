// Pointer <-> 32-bit token table. See shogo_ptrtoken.h.
// Linked statically into each game module, so each module has its own table.
#include "shogo_ptrtoken.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
std::mutex g_lock;
std::unordered_map<const void *, uint32_t> g_toToken;
std::vector<const void *> g_fromToken{nullptr};  // token 0 is reserved for NULL
}

extern "C" uint32_t shogo_ptr_to_token(const void *p)
{
    if (!p) return 0;
    std::lock_guard<std::mutex> guard(g_lock);
    auto it = g_toToken.find(p);
    if (it != g_toToken.end()) return it->second;
    uint32_t token = (uint32_t)g_fromToken.size();
    g_fromToken.push_back(p);
    g_toToken.emplace(p, token);
    return token;
}

extern "C" void *shogo_token_to_ptr(uint32_t token)
{
    std::lock_guard<std::mutex> guard(g_lock);
    return token < g_fromToken.size() ? const_cast<void *>(g_fromToken[token]) : nullptr;
}
