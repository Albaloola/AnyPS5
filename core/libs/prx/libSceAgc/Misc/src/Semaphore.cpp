#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

namespace {

std::mutex semaphoreMutex;
std::uint8_t* semaphoreMemory = nullptr;
std::uint64_t semaphoreSize = 0;

constexpr std::int32_t InvalidValue = static_cast<std::int32_t>(0x8a6c000bu);
constexpr std::int32_t InvalidAlignment = static_cast<std::int32_t>(0x8a6c0002u);
constexpr std::int32_t AlreadyInitialized = static_cast<std::int32_t>(0x8a6c0048u);
constexpr std::int32_t NotInitialized = static_cast<std::int32_t>(0x8a6c0049u);

}

extern "C" {

std::int32_t APS5_VABI sceAgcSetAmmSemaphoreMemory(void* memory, std::uint64_t size) {
    std::lock_guard lock(semaphoreMutex);
    if (semaphoreSize != 0) return AlreadyInitialized;
    if (memory == nullptr || size == 0 || ((reinterpret_cast<std::uintptr_t>(memory) | size) & 0x3fffu) != 0) return InvalidAlignment;
    std::memset(memory, 0, static_cast<std::size_t>(size));
    semaphoreMemory = static_cast<std::uint8_t*>(memory);
    semaphoreSize = size;
    return 0;
}

std::int32_t APS5_VABI sceAgcGetSemaphoreLabel(std::uint32_t index, void** label) {
    std::lock_guard lock(semaphoreMutex);
    if (semaphoreSize == 0) return NotInitialized;
    const auto offset = static_cast<std::uint64_t>(index) * 32u;
    if (offset + 32u > semaphoreSize) return InvalidValue;
    if (label == nullptr) return InvalidAlignment;
    *label = semaphoreMemory + offset;
    return 0;
}

}
