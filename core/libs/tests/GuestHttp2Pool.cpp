#include "SceTypes.hpp"
#include "prx/libSceHttp2/Http2PoolStats.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceHttp2Init(int, int, std::size_t, int);
int APS5_VABI sceHttp2CreateTemplate(int, const char*, int, int);
int APS5_VABI sceHttp2CreateRequestWithURL(int, const char*, const char*, std::uint64_t);
int APS5_VABI sceHttp2DeleteRequest(int);
int APS5_VABI sceHttp2DeleteTemplate(int);
int APS5_VABI sceHttp2GetMemoryPoolStats(int, Http2MemoryPoolStats*);
int APS5_VABI sceHttp2SendRequestAsync(int, const void*, std::size_t, Http2AsyncOption*, void*);
int APS5_VABI sceHttp2WaitAsync(int, Http2AsyncResult*, std::uint32_t*, void*);
int APS5_VABI sceHttp2Term(int);
int APS5_VABI sceKernelCreateEqueue(KernelEqueue*, const char*);
int APS5_VABI sceKernelDeleteEqueue(KernelEqueue);
int APS5_VABI sceKernelAddUserEventEdge(KernelEqueue, int);
int APS5_VABI sceKernelDeleteUserEvent(KernelEqueue, int);
}

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F>
static bool Throws(F call) {
    try { call(); } catch (const std::exception&) { return true; }
    return false;
}

static Http2MemoryPoolStats Stats(int context) {
    struct Guarded {
        std::uint64_t before = 0x1122334455667788;
        Http2MemoryPoolStats value{};
        std::uint64_t after = 0x8877665544332211;
    } output;
    Check(sceHttp2GetMemoryPoolStats(context, &output.value) == 0, "pool query failed");
    Check(output.before == 0x1122334455667788 && output.after == 0x8877665544332211, "pool query overwrote canaries");
    Check(output.value.reserved == 0 && output.value.currentInuseSize <= output.value.maxInuseSize &&
          output.value.maxInuseSize <= output.value.poolSize, "pool statistics invariants");
    return output.value;
}

static void Exercise() {
    constexpr std::size_t capacity = 8192;
    const int context = sceHttp2Init(1, 1, capacity, 4);
    auto stats = Stats(context);
    Check(stats.poolSize == capacity && stats.currentInuseSize == 0 && stats.maxInuseSize == 0, "initial statistics");
    const int other = sceHttp2Init(1, 1, capacity * 2, 4);
    const std::string agent(256, 'a');
    const int parent = sceHttp2CreateTemplate(context, agent.c_str(), 2, 0);
    const auto parentUsage = Stats(context).currentInuseSize;
    Check(parentUsage >= agent.size(), "template data not charged");
    Check(Stats(other).currentInuseSize == 0, "independent context charged");
    const std::string address = "http://127.0.0.1/" + std::string(512, 'p');
    const int request = sceHttp2CreateRequestWithURL(parent, "POST", address.c_str(), 0);
    const auto requestUsage = Stats(context).currentInuseSize;
    Check(requestUsage >= parentUsage + address.size(), "request data not charged");

    KernelEqueue queue = 0;
    Check(sceKernelCreateEqueue(&queue, "http2-pool") == 0, "event queue creation");
    Check(sceKernelAddUserEventEdge(queue, request) == 0, "event registration");
    Http2AsyncOption option{};
    option.equeue = queue;
    option.user_event_id = request;
    for (int i = 0; i < 3; ++i) Check(sceHttp2SendRequestAsync(request, nullptr, 0, &option, nullptr) == 0, "queue completion");
    const auto queuedUsage = Stats(context).currentInuseSize;
    Check(queuedUsage >= requestUsage + 3 * sizeof(Http2AsyncResult), "completions not charged");
    Http2AsyncResult completion{};
    Check(sceHttp2WaitAsync(request, &completion, nullptr, nullptr) == 0 && completion.req_id == request &&
          completion.result == static_cast<int>(0x80436063), "offline completion semantics");
    Check(Stats(context).currentInuseSize < queuedUsage, "completion allocation not released");
    Check(sceKernelDeleteUserEvent(queue, request) == 0, "event removal");
    const auto beforeFailure = Stats(context).currentInuseSize;
    Check(Throws([&] { sceHttp2SendRequestAsync(request, nullptr, 0, &option, nullptr); }), "missing event accepted");
    Check(Stats(context).currentInuseSize == beforeFailure, "failed notification leaked completion");
    for (int i = 0; i < 2; ++i) Check(sceHttp2WaitAsync(request, &completion, nullptr, nullptr) == 0, "failed notification discarded prior completion");
    Check(Stats(context).currentInuseSize == requestUsage, "drained completions leaked");
    Check(sceKernelDeleteEqueue(queue) == 0, "event queue destruction");

    const std::string tooLarge(capacity * 2, 'x');
    Check(Throws([&] { sceHttp2CreateRequestWithURL(parent, "GET", tooLarge.c_str(), 0); }), "pool exhaustion not enforced");
    Check(Stats(context).currentInuseSize == requestUsage, "failed allocation leaked pool memory");
    Check(Throws([&] { sceHttp2CreateTemplate(context, tooLarge.c_str(), 2, 0); }), "template exceeded pool capacity");
    Check(Stats(context).currentInuseSize == requestUsage, "failed template allocation leaked");

    Check(sceHttp2DeleteRequest(request) == 0, "request deletion");
    Check(Stats(context).currentInuseSize == parentUsage, "request deletion did not reclaim memory");
    Check(sceHttp2DeleteTemplate(parent) == 0, "template deletion");
    stats = Stats(context);
    Check(stats.currentInuseSize == 0 && stats.maxInuseSize >= queuedUsage, "release or peak accounting");
    const int recreated = sceHttp2CreateTemplate(context, agent.c_str(), 2, 0);
    Check(recreated != parent && Stats(context).currentInuseSize == parentUsage, "released capacity not reusable");
    Check(sceHttp2CreateRequestWithURL(recreated, "GET", address.c_str(), 0) > 0, "create before termination");
    Check(sceHttp2Term(context) == 0, "context termination");
    Http2MemoryPoolStats untouched{11, 22, 33, 44};
    Check(Throws([&] { sceHttp2GetMemoryPoolStats(context, &untouched); }), "dead context accepted");
    Check(untouched.poolSize == 11 && untouched.maxInuseSize == 22 && untouched.currentInuseSize == 33 && untouched.reserved == 44, "failed query changed output");
    Check(Throws([&] { sceHttp2CreateRequestWithURL(recreated, "GET", address.c_str(), 0); }), "dead template accepted");
    Check(Throws([&] { sceHttp2GetMemoryPoolStats(other, nullptr); }), "null statistics accepted");
    Check(Stats(other).poolSize == capacity * 2 && Stats(other).currentInuseSize == 0, "terminating one context damaged another");
    Check(sceHttp2Term(other) == 0, "other context termination");

    const int tiny = sceHttp2Init(1, 1, 1, 1);
    Check(Throws([&] { sceHttp2CreateTemplate(tiny, "a", 2, 0); }), "tiny pool accepted template");
    Check(Stats(tiny).currentInuseSize == 0, "tiny pool leaked");
    Check(sceHttp2Term(tiny) == 0, "tiny context termination");

    const int threaded = sceHttp2Init(1, 1, 65536, 4);
    std::atomic<bool> valid{true};
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        try {
            for (int j = 0; j < 30; ++j) {
                const int source = sceHttp2CreateTemplate(threaded, agent.c_str(), 2, 0);
                const int target = sceHttp2CreateRequestWithURL(source, "GET", address.c_str(), 0);
                Stats(threaded);
                sceHttp2DeleteRequest(target);
                sceHttp2DeleteTemplate(source);
            }
        } catch (...) { valid = false; }
    });
    workers.clear();
    Check(valid && Stats(threaded).currentInuseSize == 0, "concurrent accounting");
    Check(sceHttp2Term(threaded) == 0, "threaded context termination");
}

int main() {
    try {
        Exercise();
        LibcRunShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
