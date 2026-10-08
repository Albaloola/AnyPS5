#ifndef CORE_LIBS_PRX_LIBSCEHTTP2_HTTP2POOLSTATS_HPP
#define CORE_LIBS_PRX_LIBSCEHTTP2_HTTP2POOLSTATS_HPP

#include <cstddef>
#include <cstdint>

struct Http2MemoryPoolStats {
    std::size_t poolSize;
    std::size_t maxInuseSize;
    std::size_t currentInuseSize;
    std::int32_t reserved;
};

static_assert(sizeof(Http2MemoryPoolStats) == 32);
static_assert(offsetof(Http2MemoryPoolStats, reserved) == 24);

#endif
