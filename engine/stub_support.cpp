// Call recording for the generated engine stubs.
#include "engine/stub_support.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct Recorder {
    std::mutex lock;
    std::map<std::string, unsigned> counts;
    std::vector<std::string> firstCallOrder;
    unsigned totalCalls = 0;
    std::string lastCall;
    bool traceAll = getenv("SHOGO_TRACE_ALL") != nullptr;
};

Recorder &rec()
{
    static Recorder *r = new Recorder;  // never destroyed: usable from atexit/signal handlers
    return *r;
}
}

void shogo_stub_called(const char *name)
{
    Recorder &r = rec();
    std::lock_guard<std::mutex> g(r.lock);
    unsigned &n = r.counts[name];
    ++r.totalCalls;
    r.lastCall = name;
    if (n++ == 0) {
        r.firstCallOrder.push_back(name);
        fprintf(stderr, "[stub #%u] first call: %s\n", (unsigned)r.firstCallOrder.size(), name);
    } else if (r.traceAll) {
        fprintf(stderr, "[stub] %s\n", name);
    }
}

const char *shogo_stub_last_call()
{
    return rec().lastCall.empty() ? "(none)" : rec().lastCall.c_str();
}

void shogo_stub_print_summary(FILE *out)
{
    Recorder &r = rec();
    std::lock_guard<std::mutex> g(r.lock);
    fprintf(out, "\n=== Engine API calls: %u total, %zu distinct unimplemented functions ===\n", r.totalCalls,
            r.counts.size());
    for (const std::string &name : r.firstCallOrder)
        fprintf(out, "  %6u  %s\n", r.counts[name], name.c_str());
}
