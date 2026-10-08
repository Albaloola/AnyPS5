#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "Http2PoolStats.hpp"
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

// No network is emulated: contexts, templates and requests can be created, but any request
// that would touch the network fails with the library's network error.
static constexpr int ERROR_NETWORK = static_cast<int>(0x80436063);
static std::atomic<int> g_nextHandle{1};
namespace {

class PoolResource final : public std::pmr::memory_resource {
public:
    explicit PoolResource(std::size_t size) : capacity(size) {}

    Http2MemoryPoolStats Stats() const { return {capacity, peak, used, 0}; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > capacity - used) throw std::bad_alloc();
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        used += bytes;
        if (used > peak) peak = used;
        return result;
    }

    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(address, bytes, alignment);
        used -= bytes;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }

    std::size_t capacity;
    std::size_t used = 0;
    std::size_t peak = 0;
};

struct Template {
    Template(std::pmr::memory_resource* pool, const char* agent, int version, int proxy)
        : userAgent(agent, pool), httpVersion(version), autoProxy(proxy) {}

    std::pmr::string userAgent;
    int httpVersion;
    int autoProxy;
};

struct Request {
    Request(std::pmr::memory_resource* pool, const Template& source, const char* verb,
            const char* address, std::uint64_t length)
        : userAgent(source.userAgent, pool), method(verb, pool), url(address, pool),
          contentLength(length), completions(pool) {}

    std::pmr::string userAgent;
    std::pmr::string method;
    std::pmr::string url;
    std::uint64_t contentLength;
    std::pmr::list<Http2AsyncResult> completions;
};

struct Context {
    explicit Context(std::size_t size) : pool(size), templates(&pool), requests(&pool) {}

    PoolResource pool;
    std::pmr::map<int, Template> templates;
    std::pmr::map<int, Request> requests;
};

std::mutex stateMutex;
std::unordered_map<int, std::unique_ptr<Context>> contexts;
std::unordered_map<int, Context*> templateOwners;
std::unordered_map<int, Context*> requestOwners;

Context& FindContext(int id) {
    const auto it = contexts.find(id);
    if (it == contexts.end()) throw std::invalid_argument("HTTP2 context is not live");
    return *it->second;
}

Request& FindRequest(int id) {
    const auto it = requestOwners.find(id);
    if (it == requestOwners.end()) throw std::invalid_argument("HTTP2 request is not live");
    return it->second->requests.at(id);
}

}

static void CompleteAsync(const char* function, int req_id, const Http2AsyncOption* kqueue_option, const void* option) {
    if (kqueue_option == nullptr) NotImplemented_nid_no_patch(function);
    if (option != nullptr) NotImplemented_nid_no_patch(function);
    std::lock_guard lock(stateMutex);
    auto& request = FindRequest(req_id);
    Http2AsyncResult completion{};
    completion.req_id = req_id;
    completion.result = ERROR_NETWORK;
    request.completions.push_back(completion);
    if (EqueueTriggerEvent_nid_postfix(kqueue_option->equeue, static_cast<uintptr_t>(kqueue_option->user_event_id), EVFILT_USER, kqueue_option->user_data) != 0) {
        request.completions.pop_back();
        NotImplemented_nid_no_patch(function);
    }
}

extern "C" {

int APS5_VABI sceHttp2AddRequestHeader(int id, const char* name, const char* value, uint32_t mode) {
    (void)id;
    (void)name;
    (void)value;
    (void)mode;
    return 0;
}

int APS5_VABI sceHttp2CreateRequestWithURL(int tmpl_id, const char* method, const char* url, uint64_t content_length) {
    if (method == nullptr || url == nullptr) throw std::invalid_argument("HTTP2 request strings are null");
    std::lock_guard lock(stateMutex);
    const auto parent = templateOwners.find(tmpl_id);
    if (parent == templateOwners.end()) throw std::invalid_argument("HTTP2 template is not live");
    auto& context = *parent->second;
    const int id = g_nextHandle.fetch_add(1, std::memory_order_relaxed);
    context.requests.try_emplace(id, &context.pool, context.templates.at(tmpl_id), method, url, content_length);
    try {
        requestOwners.emplace(id, &context);
    } catch (...) {
        context.requests.erase(id);
        throw;
    }
    return id;
}

int APS5_VABI sceHttp2CreateTemplate(int lib_http2_ctx_id, const char* user_agent, int http_ver, int is_auto_proxy_conf) {
    if (user_agent == nullptr) throw std::invalid_argument("HTTP2 user agent is null");
    std::lock_guard lock(stateMutex);
    auto& context = FindContext(lib_http2_ctx_id);
    const int id = g_nextHandle.fetch_add(1, std::memory_order_relaxed);
    context.templates.try_emplace(id, &context.pool, user_agent, http_ver, is_auto_proxy_conf);
    try {
        templateOwners.emplace(id, &context);
    } catch (...) {
        context.templates.erase(id);
        throw;
    }
    return id;
}

int APS5_VABI sceHttp2DeleteRequest(int req_id) {
    std::lock_guard lock(stateMutex);
    const auto it = requestOwners.find(req_id);
    if (it == requestOwners.end()) throw std::invalid_argument("HTTP2 request is not live");
    it->second->requests.erase(req_id);
    requestOwners.erase(it);
    return 0;
}

int APS5_VABI sceHttp2DeleteTemplate(int tmpl_id) {
    std::lock_guard lock(stateMutex);
    const auto it = templateOwners.find(tmpl_id);
    if (it == templateOwners.end()) throw std::invalid_argument("HTTP2 template is not live");
    it->second->templates.erase(tmpl_id);
    templateOwners.erase(it);
    return 0;
}

int APS5_VABI sceHttp2GetAllResponseHeaders(int req_id, char** header, size_t* header_size) {
    (void)req_id;
    (void)header;
    (void)header_size;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2GetResponseContentLength(int req_id, int* result, uint64_t* content_length) {
    (void)req_id;
    (void)result;
    (void)content_length;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2GetStatusCode(int req_id, int* status_code) {
    (void)req_id;
    (void)status_code;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2Init(int libnet_mem_id, int libssl_ctx_id, size_t pool_size, int max_concurrent_request) {
    (void)libnet_mem_id;
    (void)libssl_ctx_id;
    (void)max_concurrent_request;
    if (pool_size == 0) throw std::invalid_argument("HTTP2 pool size is zero");
    std::lock_guard lock(stateMutex);
    const int id = g_nextHandle.fetch_add(1, std::memory_order_relaxed);
    contexts.emplace(id, std::make_unique<Context>(pool_size));
    return id;
}

int APS5_VABI sceHttp2ReadData(int req_id, void* data, size_t size) {
    (void)req_id;
    (void)data;
    (void)size;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2ReadDataAsync(int req_id, void* data, size_t size, void* kqueue_option, void* option) {
    (void)req_id;
    (void)data;
    (void)size;
    (void)kqueue_option;
    (void)option;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2SendRequest(int req_id, const void* post_data, size_t size) {
    (void)req_id;
    (void)post_data;
    (void)size;
    return ERROR_NETWORK;
}

int APS5_VABI sceHttp2SendRequestAsync(int req_id, const void* post_data, size_t size, Http2AsyncOption* kqueue_option, void* option) {
    (void)post_data;
    (void)size;
    CompleteAsync(__func__, req_id, kqueue_option, option);
    return 0;
}

int APS5_VABI sceHttp2SetAuthEnabled(int id, int is_enable) {
    (void)id;
    (void)is_enable;
    return 0;
}

int APS5_VABI sceHttp2SetAutoRedirect(int id, int enable) {
    (void)id;
    (void)enable;
    return 0;
}

int APS5_VABI sceHttp2SetConnectionWaitTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SetConnectTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SetInflateGZIPEnabled(int id, int enable) {
    (void)id;
    (void)enable;
    return 0;
}

int APS5_VABI sceHttp2SetRecvTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SetRedirectCallback(int id, void* cb_func, void* user_arg) {
    (void)id;
    (void)cb_func;
    (void)user_arg;
    return 0;
}

int APS5_VABI sceHttp2SetRequestContentLength(int id, uint64_t content_length) {
    (void)id;
    (void)content_length;
    return 0;
}

int APS5_VABI sceHttp2SetResolveTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SetSendTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SetSslCallback(int id, void* cb_func, void* user_arg) {
    (void)id;
    (void)cb_func;
    (void)user_arg;
    return 0;
}

int APS5_VABI sceHttp2SetTimeOut(int id, uint32_t usec) {
    (void)id;
    (void)usec;
    return 0;
}

int APS5_VABI sceHttp2SslDisableOption(int id, uint32_t ssl_flags) {
    (void)id;
    (void)ssl_flags;
    return 0;
}

int APS5_VABI sceHttp2SslEnableOption(int id, uint32_t ssl_flags) {
    (void)id;
    (void)ssl_flags;
    return 0;
}

int APS5_VABI sceHttp2SetMinSslVersion(int id, uint32_t ssl_version) {
    (void)id;
    (void)ssl_version;
    return 0;
}

int APS5_VABI sceHttp2Term(int lib_http2_ctx_id) {
    std::lock_guard lock(stateMutex);
    auto& context = FindContext(lib_http2_ctx_id);
    for (const auto& entry : context.requests) requestOwners.erase(entry.first);
    for (const auto& entry : context.templates) templateOwners.erase(entry.first);
    contexts.erase(lib_http2_ctx_id);
    return 0;
}

int APS5_VABI sceHttp2WaitAsync(int req_id, Http2AsyncResult* result, uint32_t* timeout, void* option) {
    (void)timeout;
    if (result == nullptr || option != nullptr) NotImplemented_nid_no_patch(__func__);
    std::lock_guard lock(stateMutex);
    auto& pending = FindRequest(req_id).completions;
    if (pending.empty()) NotImplemented_nid_no_patch("sceHttp2WaitAsync: waiting for an operation that has not completed");
    *result = pending.front();
    pending.pop_front();
    return 0;
}

int APS5_VABI sceHttp2AbortRequest(int req_id) {
    (void)req_id;
    return 0;
}

int APS5_VABI sceHttp2GetMemoryPoolStats(int context_id, Http2MemoryPoolStats* stats) {
    if (stats == nullptr) throw std::invalid_argument("HTTP2 pool statistics output is null");
    std::lock_guard lock(stateMutex);
    const auto value = FindContext(context_id).pool.Stats();
    *stats = value;
    return 0;
}

int APS5_VABI sceHttp2CookieFlush(int id) {
    (void)id;
    return 0;
}

int APS5_VABI sceHttp2CreateCookieBox(int lib_http2_ctx_id) {
    (void)lib_http2_ctx_id;
    return g_nextHandle.fetch_add(1, std::memory_order_relaxed);
}

int APS5_VABI sceHttp2SetCookieBox(int id, int cookie_box_id) {
    (void)id;
    (void)cookie_box_id;
    return 0;
}

int APS5_VABI sceHttp2SetRequestNoContentLength(int id) {
    (void)id;
    return 0;
}

}
