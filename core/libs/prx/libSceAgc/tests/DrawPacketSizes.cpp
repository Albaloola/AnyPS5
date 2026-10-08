#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "SceTypes.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

extern "C" {
std::uint32_t APS5_VABI sceAgcDcbBeginOcclusionQueryGetSize(std::uint8_t);
std::uint32_t APS5_VABI sceAgcDcbEndOcclusionQueryGetSize();
std::uint32_t APS5_VABI sceAgcDcbSetIndexIndirectArgsGetSize();
std::uint32_t* APS5_VABI sceAgcDcbSetIndexIndirectArgs(CommandBuffer*, std::uint64_t, std::uint32_t);
}

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void expectFailure(Action action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("invalid indirect index arguments were accepted");
}

void checkIndirectArguments() {
    std::array<std::uint32_t, 6> words{};
    CommandBuffer buffer{words.data() + 1, words.data() + 5, words.data() + 1, words.data() + 5, nullptr, nullptr, 0};
    for (const auto address : std::array<std::uint64_t, 3>{0, 0x123456789abcdef0ull, 0xfffffffffffffff0ull}) {
        for (const auto count : std::array<std::uint32_t, 3>{0, 7, 0xffffu}) {
            words.fill(0xdeadbeefu);
            buffer.cursor_up = buffer.bottom;
            const auto* packet = sceAgcDcbSetIndexIndirectArgs(&buffer, address, count);
            check(packet == words.data() + 1 && buffer.cursor_up == buffer.top, "incorrect indirect index packet cursor");
            check(packet[0] == 0xc0029100u && packet[1] == static_cast<std::uint32_t>(address) &&
                  packet[2] == static_cast<std::uint32_t>(address >> 32u) && packet[3] == count, "incorrect indirect index packet");
            check(words.front() == 0xdeadbeefu && words.back() == 0xdeadbeefu, "indirect index packet overwrote guard words");
            check(sceAgcDcbSetIndexIndirectArgsGetSize() == 4 * sizeof(std::uint32_t), "indirect index size disagrees with packet");
        }
    }
    words.fill(0xdeadbeefu);
    const auto before = words;
    buffer.cursor_up = buffer.bottom;
    expectFailure([&] { sceAgcDcbSetIndexIndirectArgs(&buffer, 1, 0); });
    expectFailure([&] { sceAgcDcbSetIndexIndirectArgs(&buffer, 0x1000, 0x10000u); });
    expectFailure([&] { sceAgcDcbSetIndexIndirectArgs(&buffer, 0x1000, 0xffffffffu); });
    expectFailure([&] { sceAgcDcbSetIndexIndirectArgs(nullptr, 0x1000, 0); });
    check(words == before && buffer.cursor_up == buffer.bottom, "invalid indirect index arguments modified the buffer");
    buffer.cursor_down = buffer.bottom + 3;
    expectFailure([&] { sceAgcDcbSetIndexIndirectArgs(&buffer, 0x1000, 7); });
    check(words == before && buffer.cursor_up == buffer.bottom, "indirect index allocation failure modified the buffer");
}

}

int main() {
    try {
        check(sceAgcDcbBeginOcclusionQueryGetSize(0) == 16, "begin occlusion query size for type 0");
        check(sceAgcDcbBeginOcclusionQueryGetSize(1) == 288, "begin occlusion query size for type 1");
        bool rejected = false;
        try {
            static_cast<void>(sceAgcDcbBeginOcclusionQueryGetSize(2));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "unknown occlusion query type was accepted");
        check(sceAgcDcbEndOcclusionQueryGetSize() == 16, "end occlusion query size");
        check(sceAgcDcbSetIndexIndirectArgsGetSize() == 16, "index indirect args size");
        checkIndirectArguments();
        LibcRunShutdown_nid_postfix();
        std::puts("AGC draw packet size tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
