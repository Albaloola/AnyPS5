#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

extern "C" {
std::int32_t APS5_VABI sceAgcSetAmmSemaphoreMemory(void*, std::uint64_t);
std::int32_t APS5_VABI sceAgcGetSemaphoreLabel(std::uint32_t, void**);
}

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        std::vector<std::uint8_t> allocation(4 * 16384, 0x5a);
        void* aligned = allocation.data();
        auto space = allocation.size();
        Check(std::align(16384, 3 * 16384, aligned, space) != nullptr, "test memory alignment");
        std::span<std::uint8_t> memory(static_cast<std::uint8_t*>(aligned), 3 * 16384);
        auto* start = memory.data() + 16384;
        void* label = memory.data();
        Check(sceAgcGetSemaphoreLabel(0, &label) == static_cast<int>(0x8a6c0049u), "query before registration");
        Check(label == memory.data(), "failed query changed output");
        Check(sceAgcSetAmmSemaphoreMemory(nullptr, 16384) == static_cast<int>(0x8a6c0002u), "null registration");
        Check(sceAgcSetAmmSemaphoreMemory(start, 0) == static_cast<int>(0x8a6c0002u), "zero-sized registration");
        Check(sceAgcSetAmmSemaphoreMemory(start + 1, 16384) == static_cast<int>(0x8a6c0002u), "misaligned base");
        Check(sceAgcSetAmmSemaphoreMemory(start, 16383) == static_cast<int>(0x8a6c0002u), "misaligned size");
        Check(std::all_of(memory.begin(), memory.end(), [](auto byte) { return byte == 0x5a; }), "failed registration changed memory");
        Check(sceAgcSetAmmSemaphoreMemory(start, 16384) == 0, "registration failed");
        Check(std::all_of(start, start + 16384, [](auto byte) { return byte == 0; }), "registered region not cleared");
        Check(std::all_of(memory.data(), start, [](auto byte) { return byte == 0x5a; }) &&
              std::all_of(start + 16384, memory.data() + memory.size(), [](auto byte) { return byte == 0x5a; }), "registration overwrote guards");
        Check(sceAgcGetSemaphoreLabel(0, &label) == 0 && label == start, "first label");
        Check(sceAgcGetSemaphoreLabel(1, &label) == 0 && label == start + 32, "label stride");
        Check(sceAgcGetSemaphoreLabel(511, &label) == 0 && label == start + 16384 - 32, "last label");
        const auto* last = label;
        Check(sceAgcGetSemaphoreLabel(512, &label) == static_cast<int>(0x8a6c000bu) && label == last, "out-of-range label");
        Check(sceAgcGetSemaphoreLabel(0xffffffffu, &label) == static_cast<int>(0x8a6c000bu) && label == last, "label index overflow");
        Check(sceAgcGetSemaphoreLabel(0, nullptr) == static_cast<int>(0x8a6c0002u), "null label output");
        start[0] = 0xa5;
        Check(sceAgcSetAmmSemaphoreMemory(memory.data(), 16384) == static_cast<int>(0x8a6c0048u), "second registration");
        Check(start[0] == 0xa5 && memory.front() == 0x5a, "second registration modified storage");
        Check(sceAgcGetSemaphoreLabel(0, &label) == 0 && label == start, "second registration replaced storage");
        LibcRunShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
