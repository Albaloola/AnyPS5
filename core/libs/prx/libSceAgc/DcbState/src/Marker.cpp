#include "prx/libSceAgc/DcbState/include/Marker.hpp"

#include "prx/libSceAgc/Command/include/Packet.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::uint32_t OpcodeNop = 0x10;
constexpr std::uint32_t CustomSetMarker = 0x0a;
constexpr std::uint32_t CustomPushMarker = 0x0b;
constexpr std::uint32_t CustomPopMarker = 0x0c;

}

namespace Agc::Marker {

std::uint32_t* Write(CommandBuffer* buf, const char* str, std::size_t size, std::uint32_t color, bool push, const char* function) {
    Agc::Command::Require(buf != nullptr, function, "null command buffer");
    Agc::Command::Require(str != nullptr || size == 0, function, "null marker text");
    Agc::Command::Require(size <= 0x3fffu * sizeof(std::uint32_t), function, "marker text too long");
    const auto payload = static_cast<std::uint32_t>((size + 3) / 4);
    const auto header = Agc::Command::Header(OpcodeNop, payload + 2, (push ? CustomPushMarker : CustomSetMarker) << 2);
    std::vector<char> text;
    if (size != 0) text.assign(str, str + size);
    auto* packet = Agc::Command::Allocate(buf, payload + 2, function);
    packet[0] = header;
    packet[1] = color;
    if (payload != 0) {
        std::memset(packet + 2, 0, payload * sizeof(std::uint32_t));
        std::memcpy(packet + 2, text.data(), size);
    }
    return packet;
}

std::uint32_t* Push(CommandBuffer* buf, const char* str, std::uint32_t color, const char* function) {
    return Write(buf, str, str ? std::strlen(str) : 0, color, true, function);
}

std::uint32_t* Set(CommandBuffer* buf, const char* str, std::uint32_t color, const char* function) {
    return Write(buf, str, str ? std::strlen(str) : 0, color, false, function);
}

std::uint32_t* Pop(CommandBuffer* buf, const char* function) {
    Agc::Command::Require(buf != nullptr, function, "null command buffer");
    auto* packet = Agc::Command::Allocate(buf, 2, function);
    packet[0] = Agc::Command::Header(OpcodeNop, 2, CustomPopMarker << 2);
    packet[1] = 0;
    return packet;
}

}

extern "C" {

uint32_t* APS5_VABI sceAgcDcbSetMarker(CommandBuffer* buf, const char* str, uint32_t color) {
    return Agc::Marker::Set(buf, str, color, __func__);
}

uint32_t* APS5_VABI sceAgcDcbPopMarker(CommandBuffer* buf) {
    return Agc::Marker::Pop(buf, __func__);
}

uint32_t* APS5_VABI sceAgcDcbPushMarker(CommandBuffer* buf, const char* str, uint32_t color) {
    return Agc::Marker::Push(buf, str, color, __func__);
}

uint32_t* APS5_VABI sceAgcDcbPushMarkerSpan(CommandBuffer* buf, const char* str, uint32_t length, uint32_t color) {
    return Agc::Marker::Write(buf, str, length, color, true, __func__);
}

uint32_t* APS5_VABI sceAgcDcbSetMarkerSpan(CommandBuffer* buf, const char* str, uint32_t length, uint32_t color) {
    return Agc::Marker::Write(buf, str, length, color, false, __func__);
}

}
