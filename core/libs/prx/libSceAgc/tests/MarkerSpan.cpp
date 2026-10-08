#include "SceTypes.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" {
std::uint32_t* APS5_VABI sceAgcDcbPushMarkerSpan(CommandBuffer*, const char*, std::uint32_t, std::uint32_t);
std::uint32_t* APS5_VABI sceAgcDcbSetMarkerSpan(CommandBuffer*, const char*, std::uint32_t, std::uint32_t);
std::uint32_t* APS5_VABI sceAgcAcbPushMarkerSpan(CommandBuffer*, const char*, std::uint32_t, std::uint32_t);
std::uint32_t* APS5_VABI sceAgcAcbSetMarkerSpan(CommandBuffer*, const char*, std::uint32_t, std::uint32_t);
}

namespace {

using Builder = std::uint32_t* (APS5_VABI *)(CommandBuffer*, const char*, std::uint32_t, std::uint32_t);

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class F>
void Reject(F call) {
    bool threw = false;
    try { call(); } catch (const std::exception&) { threw = true; }
    Check(threw, "invalid marker accepted");
}

CommandBuffer Buffer(std::uint32_t* words, std::size_t size) {
    CommandBuffer buffer{};
    buffer.bottom = words;
    buffer.top = words + size;
    buffer.cursor_up = words;
    buffer.cursor_down = words + size;
    return buffer;
}

void CheckBuilder(Builder build, std::uint32_t subcommand) {
    std::array<std::uint32_t, 32> storage;
    storage.fill(0xfacecafe);
    auto buffer = Buffer(storage.data() + 1, storage.size() - 2);
    const std::array<char, 7> text{'a', 'b', 'c', 'd', 'e', 'f', 'g'};
    for (const auto length : {0u, 1u, 3u, 4u, 5u, 7u}) {
        auto* start = buffer.cursor_up;
        const auto count = 2 + (length + 3) / 4;
        const auto* packet = build(&buffer, length ? text.data() : nullptr, length, 0x12345678);
        Check(packet == start && buffer.cursor_up == start + count, "marker allocation/cursor wrong");
        Check(packet[0] == (0xc0001000u | ((count - 2) << 16) | (subcommand << 2)) && packet[1] == 0x12345678,
            "marker header/color wrong");
        const auto* bytes = reinterpret_cast<const char*>(packet + 2);
        if (length) Check(std::memcmp(bytes, text.data(), length) == 0, "marker span changed or read past length");
        for (auto index = length; index < (count - 2) * 4; ++index) Check(bytes[index] == 0, "marker padding not zero");
        for (const auto queueType : {0u, 0x20u}) {
            AgcDriver::QueueState queue;
            AgcDriver::Pm4::Validate({packet, count}, queueType);
            AgcDriver::Pm4::Execute({packet, count}, queue);
            const auto& marker = subcommand == 0xa ? *queue.lastSetMarker : queue.markers.back();
            Check(marker.text == std::string(text.data(), length) && marker.color == 0x12345678,
                "driver lost bounded marker text or color");
            Check(subcommand == 0xa ? queue.markers.empty() : !queue.lastSetMarker, "driver mixed set and push state");
        }
    }
    Check(storage.front() == 0xfacecafe && storage.back() == 0xfacecafe, "marker overwrite guard");
    const auto before = storage;
    const auto cursor = buffer.cursor_up;
    Reject([&] { build(&buffer, nullptr, 1, 0); });
    Reject([&] { build(&buffer, text.data(), 0xffffffffu, 0); });
    Reject([&] { build(nullptr, text.data(), 1, 0); });
    auto exhausted = buffer;
    exhausted.top = exhausted.cursor_down = exhausted.cursor_up + 2;
    Reject([&] { build(&exhausted, text.data(), 7, 0); });
    Check(storage == before && buffer.cursor_up == cursor, "failed marker changed buffer");

    std::array<std::uint32_t, 6> overlap{0x64636261, 0x68676665, 0, 0, 0, 0};
    auto alias = Buffer(overlap.data(), overlap.size());
    build(&alias, reinterpret_cast<const char*>(overlap.data()), 8, 0xaabbccdd);
    Check(overlap[1] == 0xaabbccdd && overlap[2] == 0x64636261 && overlap[3] == 0x68676665,
        "source overlap corrupted marker text");

    std::vector<char> largest(0x3fff * 4, 'x');
    std::vector<std::uint32_t> largePacket(0x4003, 0xfeedface);
    auto large = Buffer(largePacket.data() + 1, 0x4001);
    build(&large, largest.data(), static_cast<std::uint32_t>(largest.size()), 0x55);
    Check(largePacket[1] == (0xffff1000u | (subcommand << 2)) && large.cursor_up == large.top &&
          largePacket.front() == 0xfeedface && largePacket.back() == 0xfeedface && largePacket[0x4001] == 0x78787878,
        "maximum packet size/canaries wrong");
}

}

int main() {
    try {
        CheckBuilder(sceAgcDcbPushMarkerSpan, 0xb);
        CheckBuilder(sceAgcDcbSetMarkerSpan, 0xa);
        CheckBuilder(sceAgcAcbPushMarkerSpan, 0xb);
        CheckBuilder(sceAgcAcbSetMarkerSpan, 0xa);
        LibcRunShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
