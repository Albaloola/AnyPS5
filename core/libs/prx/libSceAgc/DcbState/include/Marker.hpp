#ifndef CORE_LIBS_PRX_LIBSCEAGC_DCBSTATE_INCLUDE_MARKER_HPP
#define CORE_LIBS_PRX_LIBSCEAGC_DCBSTATE_INCLUDE_MARKER_HPP

#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"

namespace Agc::Marker {

std::uint32_t* Write(CommandBuffer* buf, const char* str, std::size_t size, std::uint32_t color, bool push, const char* function);
std::uint32_t* Push(CommandBuffer* buf, const char* str, std::uint32_t color, const char* function);
std::uint32_t* Set(CommandBuffer* buf, const char* str, std::uint32_t color, const char* function);
std::uint32_t* Pop(CommandBuffer* buf, const char* function);

}

#endif
