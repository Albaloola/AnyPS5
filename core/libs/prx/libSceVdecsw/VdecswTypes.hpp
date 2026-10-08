#pragma once

#include <cstddef>
#include <cstdint>
#include "prx/libc/include/general/VabiMacros.hpp"
#include "AvcMetadata.hpp"

namespace SceVdecsw {

struct DecoderConfig {
    std::uint64_t thisSize;
    std::uint32_t resourceType, codecType, profile, maxLevel;
    std::int32_t maxFrameWidth, maxFrameHeight, maxDpbFrameCount;
    std::uint32_t decodeInputQueueDepth;
    void* computeQueue;
    std::uint64_t cpuAffinityMask;
    std::int32_t cpuThreadPriority;
    std::uint8_t optimizeProgressiveVideo, checkMemoryType, reserved0;
    std::int8_t extraDpbFrameCount;
    void* extraConfigInfo;
    std::uint8_t disableSyncDecode;
    std::uint8_t reserved1[3];
    std::uint32_t maxPendingSyncCount;
};

struct DecoderMemory {
    std::uint64_t thisSize, cpuMemorySize;
    void* cpuMemory;
    std::uint64_t gpuMemorySize;
    void* gpuMemory;
    std::uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
    std::uint64_t maxFrameBufferSize;
    std::uint32_t frameBufferAlignment, reserved0;
};

struct InputData {
    std::uint64_t thisSize;
    const void* auData;
    std::uint64_t auSize, ptsData, dtsData, attachedData;
};

struct InputResult {
    std::uint64_t thisSize;
    const void* decodedAu;
    std::uint32_t outputFrameCount, reserved0;
};

struct OutputInfo {
    std::uint64_t thisSize;
    std::uint8_t isValid, isLastFrame, isErrorFrame, pictureCount;
    std::uint32_t codecType, frameWidth, framePitch, frameHeight;
    std::uint8_t isDiscardedFrame;
    std::uint8_t reserved[3];
    void* frameBuffer;
    std::uint64_t frameBufferSize;
    std::uint32_t frameFormat, framePitchInBytes;
};

struct FrameBuffer {
    std::uint64_t thisSize;
    void* frameBuffer;
    std::uint64_t frameBufferSize;
};

struct ComputeMemory {
    std::uint64_t thisSize, cpuGpuMemorySize;
    void* cpuGpuMemory;
};

struct ComputeConfig {
    std::uint64_t thisSize;
    std::uint16_t computePipeId, computeQueueId;
    std::uint8_t checkMemoryType, reserved0;
    std::uint16_t reserved1;
};

enum Error : std::int32_t {
    ApiFail = static_cast<std::int32_t>(0x81510100),
    StructSize = static_cast<std::int32_t>(0x81510101),
    ArgumentPointer = static_cast<std::int32_t>(0x81510102),
    DecoderInstance = static_cast<std::int32_t>(0x81510103),
    MemorySize = static_cast<std::int32_t>(0x81510104),
    MemoryPointer = static_cast<std::int32_t>(0x81510105),
    FrameBufferSize = static_cast<std::int32_t>(0x81510106),
    FrameBufferPointer = static_cast<std::int32_t>(0x81510107),
    FrameBufferAlignment = static_cast<std::int32_t>(0x81510108),
    ComputeQueue = static_cast<std::int32_t>(0x81510110),
    AccessUnitSize = static_cast<std::int32_t>(0x8151010d),
    AccessUnitPointer = static_cast<std::int32_t>(0x8151010e),
    InvalidOutputInfo = static_cast<std::int32_t>(0x8151010f),
    InputQueueFull = static_cast<std::int32_t>(0x81510113),
    OutputBufferFull = static_cast<std::int32_t>(0x81510114),
    OutputPending = static_cast<std::int32_t>(0x81510115),
    InputQueueEmpty = static_cast<std::int32_t>(0x81510116),
    DecodePending = static_cast<std::int32_t>(0x81510117),
    OutputBufferEmpty = static_cast<std::int32_t>(0x81510118),
    ConfigInfo = static_cast<std::int32_t>(0x81510200),
    ComputePipeId = static_cast<std::int32_t>(0x81510201),
    ComputeQueueId = static_cast<std::int32_t>(0x81510202),
    ResourceType = static_cast<std::int32_t>(0x81510203),
    CodecType = static_cast<std::int32_t>(0x81510204),
    ProfileLevel = static_cast<std::int32_t>(0x81510205),
    InputQueueDepth = static_cast<std::int32_t>(0x81510206),
    DpbFrameCount = static_cast<std::int32_t>(0x81510209),
    FrameWidthHeight = static_cast<std::int32_t>(0x8151020a),
    AccessUnit = static_cast<std::int32_t>(0x81510301),
    OversizeDecode = static_cast<std::int32_t>(0x81510302),
    InvalidSequence = static_cast<std::int32_t>(0x81510303),
};

static_assert(sizeof(DecoderConfig) == 0x50);
static_assert(offsetof(DecoderConfig, extraConfigInfo) == 0x40);
static_assert(offsetof(DecoderConfig, disableSyncDecode) == 0x48);
static_assert(sizeof(DecoderMemory) == 0x48);
static_assert(sizeof(InputData) == 0x30);
static_assert(sizeof(InputResult) == 0x18);
static_assert(sizeof(OutputInfo) == 0x38);
static_assert(offsetof(OutputInfo, pictureCount) == 0x0b);
static_assert(offsetof(OutputInfo, frameBuffer) == 0x20);
static_assert(sizeof(FrameBuffer) == 0x18);
static_assert(sizeof(ComputeMemory) == 0x18);
static_assert(sizeof(ComputeConfig) == 0x10);

}

extern "C" {
int APS5_VABI sceVdecswQueryComputeMemoryInfo(SceVdecsw::ComputeMemory* memory);
int APS5_VABI sceVdecswAllocateComputeQueue(const SceVdecsw::ComputeConfig* config, const SceVdecsw::ComputeMemory* memory, void** queue);
int APS5_VABI sceVdecswReleaseComputeQueue(void* queue);
int APS5_VABI sceVdecswQueryDecoderMemoryInfo(const SceVdecsw::DecoderConfig* config, SceVdecsw::DecoderMemory* memory);
int APS5_VABI sceVdecswCreateDecoder(const SceVdecsw::DecoderConfig* config, const SceVdecsw::DecoderMemory* memory, void** decoder);
int APS5_VABI sceVdecswDeleteDecoder(void* decoder);
int APS5_VABI sceVdecswSetDecodeInput(void* decoder, const SceVdecsw::InputData* input);
int APS5_VABI sceVdecswTrySyncDecodeInput(void* decoder, SceVdecsw::InputResult* result);
int APS5_VABI sceVdecswSetDecodeOutput(void* decoder, const SceVdecsw::FrameBuffer* buffer);
int APS5_VABI sceVdecswTrySyncDecodeOutput(void* decoder, SceVdecsw::OutputInfo* output);
int APS5_VABI sceVdecswFinalizeDecodeSequence(void* decoder);
int APS5_VABI sceVdecswResetDecoder(void* decoder);
int APS5_VABI sceVdecswGetAvcPictureInfo(const SceVdecsw::OutputInfo* output, SceVdecsw::AvcPictureInfo* first, SceVdecsw::AvcPictureInfo* second);
}
