#include "prx/libSceVdecsw/VdecswTypes.hpp"
#include "VdecswFixture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace SceVdecsw;
using namespace VdecswFixture;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct AlignedDeleter {
    void operator()(std::uint8_t* value) const { ::operator delete(value, std::align_val_t(0x4000)); }
};
using AlignedBytes = std::unique_ptr<std::uint8_t, AlignedDeleter>;

AlignedBytes allocate(std::uint64_t size) {
    return AlignedBytes(static_cast<std::uint8_t*>(::operator new(static_cast<std::size_t>(size), std::align_val_t(0x4000))));
}

template<class TAction>
int poll(TAction action) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const int result = action();
        if (result != Error::DecodePending) return result;
        check(std::chrono::steady_clock::now() < deadline, "decoder did not complete within five seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

struct Session {
    ComputeMemory computeMemory{sizeof(ComputeMemory)};
    DecoderMemory memory{sizeof(DecoderMemory)};
    AlignedBytes computeStorage, decoderStorage, pixels;
    void* queue = nullptr;
    void* decoder = nullptr;
    DecoderConfig config{};

    Session(std::uint32_t depth = 1, bool legacy = false) {
        check(sceVdecswQueryComputeMemoryInfo(&computeMemory) == 0, "compute memory query failed");
        computeStorage = allocate(computeMemory.cpuGpuMemorySize);
        computeMemory.cpuGpuMemory = computeStorage.get();
        const ComputeConfig compute{sizeof(ComputeConfig), 0, 0, 0, 0, 0};
        check(sceVdecswAllocateComputeQueue(&compute, &computeMemory, &queue) == 0, "compute queue allocation failed");
        config.thisSize = legacy ? 0x48 : sizeof(config);
        config.resourceType = config.codecType = 1;
        config.profile = 100;
        config.maxLevel = 31;
        config.maxFrameWidth = Width;
        config.maxFrameHeight = Height;
        config.maxDpbFrameCount = -1;
        config.decodeInputQueueDepth = depth;
        config.computeQueue = queue;
        config.cpuThreadPriority = -1;
        config.extraDpbFrameCount = -1;
        check(sceVdecswQueryDecoderMemoryInfo(&config, &memory) == 0, "decoder memory query failed");
        check(memory.maxFrameBufferSize >= Width * 48 * 3 / 2 && memory.frameBufferAlignment == 256, "memory query underestimates output storage");
        decoderStorage = allocate(memory.cpuMemorySize);
        memory.cpuMemory = decoderStorage.get();
        pixels = allocate(memory.maxFrameBufferSize + 256);
        std::memset(pixels.get(), 0xa5, static_cast<std::size_t>(memory.maxFrameBufferSize + 256));
        check(sceVdecswCreateDecoder(&config, &memory, &decoder) == 0, "decoder creation failed");
    }

    ~Session() {
        if (decoder) sceVdecswDeleteDecoder(decoder);
        if (queue) sceVdecswReleaseComputeQueue(queue);
    }

    FrameBuffer Buffer() const { return {sizeof(FrameBuffer), pixels.get(), memory.maxFrameBufferSize}; }
};

std::vector<std::vector<std::uint8_t>> accessUnits() {
    std::vector<std::size_t> starts;
    for (std::size_t index = 0; index + 4 < Stream.size(); ++index) {
        if (Stream[index] == 0 && Stream[index + 1] == 0 && Stream[index + 2] == 0 && Stream[index + 3] == 1 && (Stream[index + 4] & 31) == 9) starts.push_back(index);
    }
    starts.push_back(Stream.size());
    std::vector<std::vector<std::uint8_t>> units;
    for (std::size_t index = 0; index + 1 < starts.size(); ++index) units.emplace_back(Stream.begin() + starts[index], Stream.begin() + starts[index + 1]);
    return units;
}

std::uint64_t hashNv12(const std::uint8_t* data, std::uint32_t pitch, std::uint32_t height) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](const std::uint8_t* row) {
        for (std::uint32_t column = 0; column < Width; ++column) hash = (hash ^ row[column]) * 0x100000001b3ull;
    };
    for (std::uint32_t row = 0; row < Height; ++row) mix(data + row * pitch);
    for (std::uint32_t row = 0; row < Height / 2; ++row) mix(data + pitch * height + row * pitch);
    return hash;
}

void testValidation() {
    ComputeMemory compute{sizeof(ComputeMemory)};
    check(sceVdecswQueryComputeMemoryInfo(nullptr) == Error::ArgumentPointer, "null query accepted");
    --compute.thisSize;
    check(sceVdecswQueryComputeMemoryInfo(&compute) == Error::StructSize, "short compute structure accepted");
    check(sceVdecswReleaseComputeQueue(reinterpret_cast<void*>(1)) == Error::ComputeQueue, "unknown queue accepted");
    check(sceVdecswDeleteDecoder(reinterpret_cast<void*>(1)) == Error::DecoderInstance, "unknown decoder accepted");
    Session session;
    check(sceVdecswReleaseComputeQueue(session.queue) == Error::ComputeQueue, "live decoder's compute queue was released");
    void* duplicate = nullptr;
    check(sceVdecswCreateDecoder(&session.config, &session.memory, &duplicate) == Error::MemoryPointer && !duplicate, "overlapping decoder storage accepted");
    DecoderMemory missing{sizeof(DecoderMemory)};
    check(sceVdecswCreateDecoder(&session.config, &missing, &duplicate) == Error::MemoryPointer && !duplicate, "missing decoder memory accepted");
    auto config = session.config;
    config.resourceType = 2;
    check(sceVdecswQueryDecoderMemoryInfo(&config, &missing) == Error::ResourceType, "unknown resource type accepted");
    config = session.config;
    config.decodeInputQueueDepth = 0;
    check(sceVdecswQueryDecoderMemoryInfo(&config, &missing) == Error::InputQueueDepth, "zero input queue depth accepted");
    InputData input{sizeof(InputData)};
    check(sceVdecswSetDecodeInput(session.decoder, &input) == Error::AccessUnitPointer, "null input data accepted");
    input.auData = Stream.data();
    check(sceVdecswSetDecodeInput(session.decoder, &input) == Error::AccessUnitSize, "empty input accepted");
    input.auSize = std::numeric_limits<std::uint64_t>::max();
    check(sceVdecswSetDecodeInput(session.decoder, &input) == Error::AccessUnitSize, "overflowing input accepted");
    auto buffer = session.Buffer();
    buffer.frameBuffer = session.pixels.get() + 1;
    check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == Error::FrameBufferAlignment, "misaligned output accepted");
    buffer = session.Buffer();
    buffer.frameBufferSize = 1;
    check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == Error::FrameBufferSize, "short output accepted");
    OutputInfo output{sizeof(OutputInfo)};
    check(sceVdecswTrySyncDecodeOutput(session.decoder, &output) == Error::OutputBufferEmpty, "missing output buffer accepted");
    buffer = session.Buffer();
    check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == 0, "output buffer staging failed");
    check(sceVdecswTrySyncDecodeOutput(session.decoder, &output) == Error::DecodePending, "empty decoder produced a picture");
    check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == Error::OutputBufferFull, "pending output buffer was lost");
    check(sceVdecswResetDecoder(session.decoder) == 0, "empty decoder reset failed");
    check(sceVdecswTrySyncDecodeOutput(session.decoder, &output) == Error::OutputBufferEmpty, "reset retained staged output");
    check(sceVdecswFinalizeDecodeSequence(session.decoder) == 0, "empty finalize failed");
    check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == 0, "empty sequence output staging failed");
    check(poll([&] { return sceVdecswTrySyncDecodeOutput(session.decoder, &output); }) == 0 && output.isLastFrame && !output.isValid, "empty sequence final marker missing");
}

void testDecode(bool legacy) {
    Session session(1, legacy);
    auto units = accessUnits();
    check(units.size() == PictureHashes.size(), "fixture access unit count mismatch");
    for (int sequence = 0; sequence < 2; ++sequence) {
        OutputInfo lastPicture{sizeof(OutputInfo)};
        for (std::size_t index = 0; index < units.size(); ++index) {
            auto bytes = units[index];
            const InputData input{sizeof(InputData), bytes.data(), bytes.size(), 1000 + index, index, 0xa0 + index};
            check(sceVdecswSetDecodeInput(session.decoder, &input) == 0, "input submission failed");
            check(sceVdecswSetDecodeInput(session.decoder, &input) == Error::InputQueueFull, "input queue depth was not enforced");
            std::fill(bytes.begin(), bytes.end(), 0);
            InputResult result{sizeof(InputResult)};
            check(poll([&] { return sceVdecswTrySyncDecodeInput(session.decoder, &result); }) == 0, "input completion failed");
            check(result.decodedAu == input.auData && result.reserved0 == 0, "input completion identity mismatch");
            check(sceVdecswTrySyncDecodeInput(session.decoder, &result) == Error::InputQueueEmpty, "input completion was delivered twice");
        }
        check(sceVdecswFinalizeDecodeSequence(session.decoder) == 0, "sequence finalize failed");
        check(sceVdecswFinalizeDecodeSequence(session.decoder) == Error::InvalidSequence, "sequence finalized twice");
        const InputData late{sizeof(InputData), units[0].data(), units[0].size(), 1, 1, 1};
        check(sceVdecswSetDecodeInput(session.decoder, &late) == Error::InvalidSequence, "input accepted after finalization");
        bool last = false;
        for (std::size_t picture = 0; !last; ++picture) {
            auto buffer = session.Buffer();
            check(sceVdecswSetDecodeOutput(session.decoder, &buffer) == 0, "output staging failed");
            alignas(OutputInfo) std::array<std::uint8_t, sizeof(OutputInfo) + 16> guarded;
            guarded.fill(0xa5);
            auto* output = reinterpret_cast<OutputInfo*>(guarded.data());
            output->thisSize = legacy ? 0x30 : sizeof(OutputInfo);
            check(poll([&] { return sceVdecswTrySyncDecodeOutput(session.decoder, output); }) == 0, "output completion failed");
            const auto untouched = legacy ? 0x30 : sizeof(OutputInfo);
            check(std::all_of(guarded.begin() + untouched, guarded.end(), [](auto value) { return value == 0xa5; }), "output writes beyond declared ABI size");
            if (output->isValid) {
                check(picture < PictureHashes.size(), "too many pictures decoded");
                check(output->codecType == 1 && output->pictureCount == 1 && !output->isErrorFrame, "incorrect picture flags");
                check(output->frameWidth == Width && output->frameHeight == 48 && output->framePitch == 64, "incorrect padded NV12 geometry");
                check(output->frameBufferSize == 64 * 48 * 3 / 2, "incorrect NV12 size");
                check(hashNv12(session.pixels.get(), output->framePitch, output->frameHeight) == PictureHashes[picture], "decoded picture differs from independent reference hash");
                AvcPictureInfo first{sizeof(AvcPictureInfo)}, second{sizeof(AvcPictureInfo)};
                check(sceVdecswGetAvcPictureInfo(output, &first, &second) == 0 && first.isValid && !second.isValid, "AVC picture metadata missing");
                const auto unit = DisplayOrderUnits[picture];
                check(first.ptsData == 1000 + unit && first.dtsData == unit && first.attachedData == 0xa0 + unit, "reordered frame has metadata from a different input");
                check(first.idrPictureFlag == (picture == 0) && first.profileIdc == 100 && first.levelIdc == 10, "AVC profile, level, or IDR metadata mismatch");
                check(first.picWidthInMbsMinus1 == 3 && first.picHeightInMapUnitsMinus1 == 2 && first.frameMbsOnlyFlag == 1, "AVC macroblock geometry mismatch");
                check(first.frameCroppingFlag == 1 && first.frameCropBottomOffset == 4 && first.frameCropLeftOffset == 0 && first.frameCropRightOffset == 0 && first.frameCropTopOffset == 0, "AVC cropping metadata mismatch");
                check(first.auDelimiterPresentFlag == 1 && first.sequenceParameterSetPresentFlag == (unit == 0) && first.pictureParameterSetPresentFlag == (unit == 0), "access-unit flags lost during reorder");
                check(std::memcmp(session.pixels.get() + output->frameBufferSize, &first, sizeof(first)) == 0, "in-buffer metadata differs from getter");
                std::memcpy(&lastPicture, output, static_cast<std::size_t>(output->thisSize));
                check(std::all_of(session.pixels.get() + session.memory.maxFrameBufferSize, session.pixels.get() + session.memory.maxFrameBufferSize + 256, [](auto value) { return value == 0xa5; }), "output buffer guard overwritten");
            }
            last = output->isLastFrame != 0;
            if (last) check(picture + (output->isValid ? 1 : 0) == PictureHashes.size(), "missing reordered pictures after drain");
        }
        check(sceVdecswResetDecoder(session.decoder) == 0, "sequence reset failed");
        AvcPictureInfo stale{sizeof(AvcPictureInfo)};
        check(sceVdecswGetAvcPictureInfo(&lastPicture, &stale, nullptr) == Error::InvalidOutputInfo, "reset retained stale picture metadata");
    }
}

void testMalformedInput() {
    Session session;
    const std::array<std::uint8_t, 5> malformed{0, 0, 1, 0x67, 0};
    const InputData bad{sizeof(InputData), malformed.data(), malformed.size(), 0, 0, 0};
    check(sceVdecswSetDecodeInput(session.decoder, &bad) == 0, "malformed input was not queued for decoding");
    InputResult result{sizeof(InputResult)};
    check(poll([&] { return sceVdecswTrySyncDecodeInput(session.decoder, &result); }) == Error::AccessUnit, "malformed stream was accepted");
    check(result.decodedAu == bad.auData && result.outputFrameCount == 0, "malformed input completion corrupted");
    check(sceVdecswResetDecoder(session.decoder) == 0, "reset after malformed input failed");
}

void testOutputBackpressure() {
    for (bool remove : {false, true}) {
        Session session;
        const auto bytes = accessUnits()[0];
        const InputData input{sizeof(InputData), bytes.data(), bytes.size(), 0, 0, 0};
        bool blocked = false;
        for (int index = 0; index < 64 && !blocked; ++index) {
            check(sceVdecswSetDecodeInput(session.decoder, &input) == 0, "backpressure input submission failed");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            InputResult result{sizeof(InputResult)};
            int status;
            do {
                status = sceVdecswTrySyncDecodeInput(session.decoder, &result);
                if (status != Error::DecodePending) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < deadline);
            check(status == 0 || status == Error::DecodePending, "backpressure decode failed");
            blocked = status == Error::DecodePending;
        }
        check(blocked, "decoder frame queue grew without backpressure");
        if (remove) {
            check(sceVdecswDeleteDecoder(session.decoder) == 0, "delete deadlocked against output backpressure");
            session.decoder = nullptr;
        } else check(sceVdecswResetDecoder(session.decoder) == 0, "reset deadlocked against output backpressure");
    }
}

void testResetWithWorkPending() {
    Session session(64);
    const auto units = accessUnits();
    for (int cycle = 0; cycle < 4; ++cycle) {
        for (int index = 0; index < 32; ++index) {
            const auto& bytes = units[index % units.size()];
            const InputData input{sizeof(InputData), bytes.data(), bytes.size(), static_cast<std::uint64_t>(index), 0, 0};
            check(sceVdecswSetDecodeInput(session.decoder, &input) == 0, "queued input failed before reset");
        }
        check(sceVdecswResetDecoder(session.decoder) == 0, "pending reset failed");
        InputResult result{sizeof(InputResult)};
        check(sceVdecswTrySyncDecodeInput(session.decoder, &result) == Error::InputQueueEmpty, "reset retained completed input");
    }
}

void testConcurrentDelete() {
    for (int cycle = 0; cycle < 24; ++cycle) {
        Session session(32);
        const auto units = accessUnits();
        const InputData input{sizeof(InputData), units[0].data(), units[0].size(), 1, 1, 1};
        check(sceVdecswSetDecodeInput(session.decoder, &input) == 0, "lifecycle test submission failed");
        std::atomic<bool> started{false};
        std::atomic<int> resetResult{0};
        const auto handle = session.decoder;
        std::jthread resetting([&] {
            started = true;
            resetResult = sceVdecswResetDecoder(handle);
        });
        while (!started) std::this_thread::yield();
        check(sceVdecswDeleteDecoder(handle) == 0, "concurrent delete failed");
        session.decoder = nullptr;
        std::memset(session.decoderStorage.get(), 0xa5, static_cast<std::size_t>(session.memory.cpuMemorySize));
        resetting.join();
        check(resetResult == 0 || resetResult == Error::DecoderInstance, "unexpected reset/delete race result");
        check(std::all_of(session.decoderStorage.get(), session.decoderStorage.get() + session.memory.cpuMemorySize, [](auto value) { return value == 0xa5; }), "decoder writes caller memory after deletion");
        check(sceVdecswCreateDecoder(&session.config, &session.memory, &session.decoder) == 0, "released decoder memory cannot be reused");
    }
}

}

int main() {
    try {
        testValidation();
        testDecode(false);
        testDecode(true);
        testResetWithWorkPending();
        testConcurrentDelete();
        testMalformedInput();
        testOutputBackpressure();
        std::puts("Vdecsw queue, decode, drain, reset, ABI, and NV12 reference tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Vdecsw test failed: %s\n", error.what());
        return 1;
    }
}
