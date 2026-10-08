#include "VdecswTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

using namespace SceVdecsw;
constexpr std::uint64_t MemoryAlignment = 0x4000;
constexpr std::uint32_t OutputAlignment = 0x100;
constexpr std::uint64_t PictureInfoBytes = sizeof(AvcPictureInfo);
constexpr std::uint32_t Levels[]{10, 111, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};

void clearPictures(void* owner);

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

struct ContextDeleter {
    void operator()(AVCodecContext* value) const { avcodec_free_context(&value); }
};
struct PacketDeleter {
    void operator()(AVPacket* value) const { av_packet_free(&value); }
};
struct FrameDeleter {
    void operator()(AVFrame* value) const { av_frame_free(&value); }
};
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

struct Queue {
    std::uint16_t pipeId, queueId;
};

struct PictureData {
    std::uint64_t pts, dts, attached;
};

struct Completion {
    const void* input;
    std::uint32_t frames;
    int result;
};

struct Command {
    enum class Kind { Input, Finalize, Reset } kind;
    Packet packet;
    const void* input = nullptr;
    std::shared_ptr<std::promise<void>> finished;
};

struct Decoder {
    DecoderConfig config;
    std::shared_ptr<Queue> queue;
    std::unique_ptr<AVCodecContext, ContextDeleter> context;
    AvcMetadataParser metadata;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Command> commands;
    std::deque<Completion> completions;
    std::deque<Frame> frames;
    std::optional<FrameBuffer> output;
    std::size_t unsynced = 0;
    bool closingSequence = false, finalized = false, resetting = false;
    std::exception_ptr failure;
    std::jthread worker;

    Decoder(const DecoderConfig& requested, std::shared_ptr<Queue> assignedQueue)
        : config(requested), queue(std::move(assignedQueue)) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) throw std::runtime_error("Vdecsw: H.264 decoder is unavailable");
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc();
        context->thread_count = 1;
        context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        context->err_recognition = AV_EF_EXPLODE;
        context->max_pixels = static_cast<std::int64_t>(config.maxFrameWidth) * config.maxFrameHeight;
        if (avcodec_open2(context.get(), codec, nullptr) < 0) throw std::runtime_error("Vdecsw: cannot open H.264 decoder");
        worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~Decoder() {
        worker.request_stop();
        condition.notify_all();
        if (worker.joinable()) worker.join();
    }

    void CheckFailure() const {
        if (failure) std::rethrow_exception(failure);
    }

private:
    int receive(std::stop_token stop, std::uint32_t& count) {
        for (;;) {
            Frame frame(av_frame_alloc());
            if (!frame) throw std::bad_alloc();
            const int result = avcodec_receive_frame(context.get(), frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return 0;
            if (result < 0) return Error::AccessUnit;
            if (frame->width > config.maxFrameWidth || frame->height > config.maxFrameHeight) return Error::OversizeDecode;
            if (frame->width <= 0 || frame->height <= 0) return Error::InvalidSequence;
            if ((frame->flags & AV_FRAME_FLAG_INTERLACED) != 0) throw std::runtime_error("Vdecsw: interlaced AVC output is not implemented");
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P && frame->format != AV_PIX_FMT_NV12) throw std::runtime_error("Vdecsw: only 8-bit 4:2:0 AVC output is implemented");
            if (!frame->opaque_ref || frame->opaque_ref->size != sizeof(AvcPictureInfo) || !reinterpret_cast<const AvcPictureInfo*>(frame->opaque_ref->data)->isValid) throw std::runtime_error("Vdecsw: decoded picture is missing its AVC metadata");
            std::unique_lock lock(mutex);
            const std::size_t capacity = static_cast<std::size_t>(config.decodeInputQueueDepth) + 16;
            condition.wait(lock, [&] { return frames.size() < capacity || resetting || stop.stop_requested(); });
            if (resetting || stop.stop_requested()) return Error::DecodePending;
            frames.push_back(std::move(frame));
            ++count;
        }
    }

    int send(AVPacket* packet, std::stop_token stop, std::uint32_t& count) {
        if (packet) {
            const auto input = *reinterpret_cast<const PictureData*>(packet->opaque_ref->data);
            std::optional<AvcPictureInfo> picture;
            try {
                picture = metadata.ParseAccessUnit({packet->data, static_cast<std::size_t>(packet->size)}, input.pts, input.dts, input.attached);
            } catch (const std::invalid_argument&) {
                return Error::AccessUnit;
            }
            if (picture) {
                const auto width = (static_cast<std::uint64_t>(picture->picWidthInMbsMinus1) + 1) * 16;
                const auto height = (static_cast<std::uint64_t>(picture->picHeightInMapUnitsMinus1) + 1) * 16;
                const auto cropX = (static_cast<std::uint64_t>(picture->frameCropLeftOffset) + picture->frameCropRightOffset) * 2;
                const auto cropY = (static_cast<std::uint64_t>(picture->frameCropTopOffset) + picture->frameCropBottomOffset) * 2;
                if (cropX >= width || cropY >= height || width - cropX > static_cast<std::uint64_t>(config.maxFrameWidth) || height - cropY > static_cast<std::uint64_t>(config.maxFrameHeight)) {
                    metadata.Reset();
                    return Error::OversizeDecode;
                }
                const auto level = picture->levelIdc == 11 && picture->constraintSet3Flag && picture->profileIdc != 100 ? 111u : picture->levelIdc;
                if (picture->profileIdc > config.profile || std::find(std::begin(Levels), std::end(Levels), level) > std::find(std::begin(Levels), std::end(Levels), config.maxLevel)) {
                    metadata.Reset();
                    return Error::InvalidSequence;
                }
            }
            AVBufferRef* snapshot = av_buffer_allocz(sizeof(AvcPictureInfo));
            if (!snapshot) return Error::ApiFail;
            if (picture) std::memcpy(snapshot->data, &*picture, sizeof(*picture));
            av_buffer_unref(&packet->opaque_ref);
            packet->opaque_ref = snapshot;
        }
        int result = avcodec_send_packet(context.get(), packet);
        if (result == AVERROR(EAGAIN)) {
            const int received = receive(stop, count);
            if (received != 0) return received;
            result = avcodec_send_packet(context.get(), packet);
        }
        if (result < 0 && result != AVERROR_EOF) return Error::AccessUnit;
        return receive(stop, count);
    }

    void run(std::stop_token stop) {
        for (;;) {
            Command command;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return !commands.empty() || stop.stop_requested(); });
                if (stop.stop_requested()) return;
                command = std::move(commands.front());
                commands.pop_front();
            }
            try {
                if (command.kind == Command::Kind::Reset) {
                    avcodec_flush_buffers(context.get());
                    metadata.Reset();
                    std::lock_guard lock(mutex);
                    frames.clear();
                    completions.clear();
                    output.reset();
                    unsynced = 0;
                    closingSequence = finalized = resetting = false;
                    failure = nullptr;
                    clearPictures(this);
                    command.finished->set_value();
                } else {
                    std::uint32_t count = 0;
                    const int result = send(command.packet.get(), stop, count);
                    std::lock_guard lock(mutex);
                    if (command.kind == Command::Kind::Input) completions.push_back({command.input, count, result});
                    else {
                        if (result != 0 && result != Error::DecodePending) throw std::runtime_error("Vdecsw: cannot drain the decoding sequence");
                        finalized = true;
                    }
                }
            } catch (...) {
                std::lock_guard lock(mutex);
                failure = std::current_exception();
                if (command.finished) command.finished->set_exception(failure);
            }
            condition.notify_all();
        }
    }
};

std::mutex registryMutex;
std::condition_variable registryCondition;
std::unordered_map<void*, std::shared_ptr<Queue>> queues;
struct DecoderEntry {
    std::shared_ptr<Decoder> decoder;
    std::size_t calls = 0;
    bool closing = false;
};
struct OutputRecord {
    void* owner;
    std::uint64_t bytes;
    AvcPictureInfo picture;
};
std::unordered_map<void*, OutputRecord> pictures;
std::unordered_map<void*, DecoderEntry> decoders;

void clearPictures(void* owner) {
    std::lock_guard lock(registryMutex);
    std::erase_if(pictures, [owner](const auto& entry) { return entry.second.owner == owner; });
}

class DecoderLease {
public:
    DecoderLease() = default;
    explicit DecoderLease(DecoderEntry& entry) : entry(&entry) { ++entry.calls; }
    DecoderLease(const DecoderLease&) = delete;
    DecoderLease& operator=(const DecoderLease&) = delete;
    ~DecoderLease() {
        if (!entry) return;
        std::lock_guard lock(registryMutex);
        --entry->calls;
        registryCondition.notify_all();
    }
    explicit operator bool() const { return entry != nullptr; }
    Decoder* operator->() const { return entry->decoder.get(); }
private:
    DecoderEntry* entry = nullptr;
};

DecoderLease findDecoder(void* handle) {
    std::lock_guard lock(registryMutex);
    const auto found = decoders.find(handle);
    if (found == decoders.end() || found->second.closing) return {};
    return DecoderLease(found->second);
}

int validateConfig(const DecoderConfig* input, DecoderConfig& config) {
    if (!input) return Error::ArgumentPointer;
    if (input->thisSize != 0x48 && input->thisSize != sizeof(DecoderConfig)) return Error::StructSize;
    std::memcpy(&config, input, static_cast<std::size_t>(input->thisSize));
    if (config.resourceType != 1) return Error::ResourceType;
    if (config.codecType == 974921 || config.codecType == 2382845) throw std::runtime_error("Vdecsw: HEVC and VP9 decoding are not implemented");
    if (config.codecType != 1) return Error::CodecType;
    if (config.profile != 66 && config.profile != 77 && config.profile != 100) return Error::ProfileLevel;
    if (std::find(std::begin(Levels), std::end(Levels), config.maxLevel) == std::end(Levels)) return Error::ProfileLevel;
    if (config.maxFrameWidth == -1 || config.maxFrameHeight == -1) throw std::runtime_error("Vdecsw: automatic frame limits are not implemented");
    if (config.maxFrameWidth <= 0 || config.maxFrameHeight <= 0 || av_image_check_size2(config.maxFrameWidth, config.maxFrameHeight, std::numeric_limits<std::int64_t>::max(), AV_PIX_FMT_YUV420P, 0, nullptr) < 0) return Error::FrameWidthHeight;
    if (config.decodeInputQueueDepth == 0) return Error::InputQueueDepth;
    if (config.maxDpbFrameCount < -1 || config.maxDpbFrameCount == 0) return Error::DpbFrameCount;
    if (config.maxDpbFrameCount != -1 || config.extraDpbFrameCount != -1) throw std::runtime_error("Vdecsw: explicit DPB frame reservations are not implemented");
    if (config.reserved0 != 0 || config.optimizeProgressiveVideo > 1 || config.checkMemoryType > 1 || config.disableSyncDecode > 1) return Error::ConfigInfo;
    for (auto value : config.reserved1) if (value != 0) return Error::ConfigInfo;
    if (config.maxPendingSyncCount != 0) throw std::runtime_error("Vdecsw: explicit pending-sync limits are not implemented");
    if (config.checkMemoryType) throw std::runtime_error("Vdecsw: guest direct-memory checks are not implemented");
    if (config.cpuAffinityMask != 0 || config.cpuThreadPriority != -1) throw std::runtime_error("Vdecsw: explicit guest worker scheduling is not implemented");
    if (config.extraConfigInfo) throw std::runtime_error("Vdecsw: codec-specific extra configuration is not implemented");
    return 0;
}

std::uint64_t maximumFrameBytes(const DecoderConfig& config) {
    return alignUp(config.maxFrameWidth, 64) * alignUp(config.maxFrameHeight, 16) * 3 / 2 + PictureInfoBytes;
}

int writeFrame(const AVFrame& frame, const FrameBuffer& destination, OutputInfo& output) {
    const auto pitch = static_cast<std::uint32_t>(alignUp(frame.width, 64));
    const auto height = static_cast<std::uint32_t>(alignUp(frame.height, 16));
    const std::uint64_t pixels = static_cast<std::uint64_t>(pitch) * height * 3 / 2;
    if (destination.frameBufferSize < pixels + PictureInfoBytes) return Error::FrameBufferSize;
    auto* base = static_cast<std::uint8_t*>(destination.frameBuffer);
    std::memset(base, 0, static_cast<std::size_t>(pixels + PictureInfoBytes));
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(base + static_cast<std::size_t>(row) * pitch, frame.data[0] + static_cast<std::ptrdiff_t>(row) * frame.linesize[0], static_cast<std::size_t>(frame.width));
    }
    auto* chroma = base + static_cast<std::size_t>(pitch) * height;
    const int chromaRows = (frame.height + 1) / 2;
    const int chromaColumns = (frame.width + 1) / 2;
    for (int row = 0; row < chromaRows; ++row) {
        auto* target = chroma + static_cast<std::size_t>(row) * pitch;
        const auto* sourceU = frame.data[1] + static_cast<std::ptrdiff_t>(row) * frame.linesize[1];
        if (frame.format == AV_PIX_FMT_NV12) std::memcpy(target, sourceU, static_cast<std::size_t>(chromaColumns) * 2);
        else {
            const auto* sourceV = frame.data[2] + static_cast<std::ptrdiff_t>(row) * frame.linesize[2];
            for (int column = 0; column < chromaColumns; ++column) {
                target[column * 2] = sourceU[column];
                target[column * 2 + 1] = sourceV[column];
            }
        }
    }
    output.isValid = 1;
    output.isErrorFrame = (frame.flags & AV_FRAME_FLAG_CORRUPT) != 0;
    output.pictureCount = 1;
    output.codecType = 1;
    output.frameWidth = static_cast<std::uint32_t>(alignUp(frame.width, 16));
    output.framePitch = pitch;
    output.frameHeight = height;
    output.frameBuffer = destination.frameBuffer;
    output.frameBufferSize = pixels;
    std::memcpy(base + pixels, frame.opaque_ref->data, sizeof(AvcPictureInfo));
    if (output.thisSize == sizeof(OutputInfo)) output.framePitchInBytes = pitch;
    return 0;
}

}

extern "C" {

int APS5_VABI sceVdecswQueryComputeMemoryInfo(SceVdecsw::ComputeMemory* memory) {
    if (!memory) return Error::ArgumentPointer;
    if (memory->thisSize != sizeof(*memory)) return Error::StructSize;
    memory->cpuGpuMemorySize = alignUp(sizeof(Queue), MemoryAlignment);
    memory->cpuGpuMemory = nullptr;
    return 0;
}

int APS5_VABI sceVdecswAllocateComputeQueue(const ComputeConfig* config, const ComputeMemory* memory, void** queue) {
    if (!config || !memory || !queue) return Error::ArgumentPointer;
    if (config->thisSize != sizeof(*config) || memory->thisSize != sizeof(*memory)) return Error::StructSize;
    if (config->reserved0 || config->reserved1 || config->checkMemoryType > 1) return Error::ConfigInfo;
    if (config->computePipeId > 4) return Error::ComputePipeId;
    if (config->computeQueueId > 7) return Error::ComputeQueueId;
    if (config->checkMemoryType) throw std::runtime_error("Vdecsw: guest direct-memory checks are not implemented");
    if (!memory->cpuGpuMemory || reinterpret_cast<std::uintptr_t>(memory->cpuGpuMemory) % MemoryAlignment) return Error::MemoryPointer;
    if (memory->cpuGpuMemorySize < alignUp(sizeof(Queue), MemoryAlignment)) return Error::MemorySize;
    std::lock_guard lock(registryMutex);
    if (queues.contains(memory->cpuGpuMemory) || decoders.contains(memory->cpuGpuMemory)) return Error::MemoryPointer;
    auto owned = std::shared_ptr<Queue>(new(memory->cpuGpuMemory) Queue{config->computePipeId, config->computeQueueId}, [](Queue* value) { value->~Queue(); });
    queues.emplace(memory->cpuGpuMemory, std::move(owned));
    *queue = memory->cpuGpuMemory;
    return 0;
}

int APS5_VABI sceVdecswReleaseComputeQueue(void* queue) {
    std::lock_guard lock(registryMutex);
    const auto found = queues.find(queue);
    if (found == queues.end() || found->second.use_count() != 1) return Error::ComputeQueue;
    queues.erase(found);
    return 0;
}

int APS5_VABI sceVdecswQueryDecoderMemoryInfo(const DecoderConfig* input, DecoderMemory* memory) {
    if (!memory) return Error::ArgumentPointer;
    if (memory->thisSize != sizeof(*memory)) return Error::StructSize;
    DecoderConfig config{};
    const int result = validateConfig(input, config);
    if (result != 0) return result;
    *memory = {sizeof(*memory), alignUp(sizeof(Decoder), MemoryAlignment), nullptr, 0, nullptr, 0, nullptr, maximumFrameBytes(config), OutputAlignment, 0};
    return 0;
}

int APS5_VABI sceVdecswCreateDecoder(const DecoderConfig* input, const DecoderMemory* memory, void** handle) {
    if (!memory || !handle) return Error::ArgumentPointer;
    if (memory->thisSize != sizeof(*memory)) return Error::StructSize;
    DecoderConfig config{};
    const int result = validateConfig(input, config);
    if (result != 0) return result;
    if (!memory->cpuMemory || reinterpret_cast<std::uintptr_t>(memory->cpuMemory) % MemoryAlignment) return Error::MemoryPointer;
    if (memory->cpuMemorySize < alignUp(sizeof(Decoder), MemoryAlignment)) return Error::MemorySize;
    std::lock_guard lock(registryMutex);
    const auto queue = queues.find(config.computeQueue);
    if (queue == queues.end()) return Error::ComputeQueue;
    if (decoders.contains(memory->cpuMemory) || queues.contains(memory->cpuMemory)) return Error::MemoryPointer;
    auto owned = std::shared_ptr<Decoder>(new(memory->cpuMemory) Decoder(config, queue->second), [](Decoder* value) { value->~Decoder(); });
    decoders.emplace(memory->cpuMemory, DecoderEntry{std::move(owned)});
    *handle = memory->cpuMemory;
    return 0;
}

int APS5_VABI sceVdecswDeleteDecoder(void* handle) {
    std::unique_lock lock(registryMutex);
    const auto found = decoders.find(handle);
    if (found == decoders.end() || found->second.closing) return Error::DecoderInstance;
    auto& entry = found->second;
    entry.closing = true;
    registryCondition.wait(lock, [&] { return entry.calls == 0; });
    auto removed = std::move(entry.decoder);
    std::erase_if(pictures, [handle](const auto& item) { return item.second.owner == handle; });
    lock.unlock();
    removed.reset();
    lock.lock();
    decoders.erase(handle);
    return 0;
}

int APS5_VABI sceVdecswSetDecodeInput(void* handle, const InputData* input) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    if (!input) return Error::ArgumentPointer;
    if (input->thisSize != sizeof(*input)) return Error::StructSize;
    if (!input->auData) return Error::AccessUnitPointer;
    if (!input->auSize || input->auSize > static_cast<std::uint64_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) return Error::AccessUnitSize;
    std::lock_guard lock(decoder->mutex);
    decoder->CheckFailure();
    if (decoder->resetting) return Error::DecodePending;
    if (decoder->closingSequence) return Error::InvalidSequence;
    if (decoder->unsynced >= decoder->config.decodeInputQueueDepth) return Error::InputQueueFull;
    Packet packet(av_packet_alloc());
    if (!packet || av_new_packet(packet.get(), static_cast<int>(input->auSize)) < 0) return Error::ApiFail;
    std::memcpy(packet->data, input->auData, static_cast<std::size_t>(input->auSize));
    packet->pts = static_cast<std::int64_t>(input->ptsData);
    packet->dts = static_cast<std::int64_t>(input->dtsData);
    packet->opaque_ref = av_buffer_alloc(sizeof(PictureData));
    if (!packet->opaque_ref) return Error::ApiFail;
    const PictureData picture{input->ptsData, input->dtsData, input->attachedData};
    std::memcpy(packet->opaque_ref->data, &picture, sizeof(picture));
    decoder->commands.push_back({Command::Kind::Input, std::move(packet), input->auData, nullptr});
    ++decoder->unsynced;
    decoder->condition.notify_one();
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeInput(void* handle, InputResult* result) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    if (!result) return Error::ArgumentPointer;
    if (result->thisSize != sizeof(*result)) return Error::StructSize;
    std::lock_guard lock(decoder->mutex);
    decoder->CheckFailure();
    if (decoder->resetting) return Error::DecodePending;
    if (decoder->completions.empty()) return decoder->unsynced ? Error::DecodePending : Error::InputQueueEmpty;
    const auto completed = decoder->completions.front();
    decoder->completions.pop_front();
    --decoder->unsynced;
    result->decodedAu = completed.input;
    result->outputFrameCount = completed.frames;
    result->reserved0 = 0;
    return completed.result;
}

int APS5_VABI sceVdecswSetDecodeOutput(void* handle, const FrameBuffer* buffer) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    if (!buffer) return Error::ArgumentPointer;
    if (buffer->thisSize != sizeof(*buffer)) return Error::StructSize;
    if (!buffer->frameBuffer) return Error::FrameBufferPointer;
    if (reinterpret_cast<std::uintptr_t>(buffer->frameBuffer) % OutputAlignment) return Error::FrameBufferAlignment;
    if (buffer->frameBufferSize < maximumFrameBytes(decoder->config)) return Error::FrameBufferSize;
    std::lock_guard lock(decoder->mutex);
    decoder->CheckFailure();
    if (decoder->resetting) return Error::DecodePending;
    if (decoder->output) return Error::OutputBufferFull;
    decoder->output = *buffer;
    return 0;
}

int APS5_VABI sceVdecswTrySyncDecodeOutput(void* handle, OutputInfo* output) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    if (!output) return Error::ArgumentPointer;
    if (output->thisSize != 0x30 && output->thisSize != sizeof(*output)) return Error::StructSize;
    std::lock_guard lock(decoder->mutex);
    decoder->CheckFailure();
    if (decoder->resetting) return Error::DecodePending;
    if (!decoder->output) return Error::OutputBufferEmpty;
    const auto size = output->thisSize;
    std::memset(output, 0, static_cast<std::size_t>(size));
    output->thisSize = size;
    if (decoder->frames.empty()) {
        if (!decoder->finalized) return Error::DecodePending;
        output->isLastFrame = 1;
        decoder->output.reset();
        return 0;
    }
    const int result = writeFrame(*decoder->frames.front(), *decoder->output, *output);
    if (result != 0) return result;
    {
        std::lock_guard registryLock(registryMutex);
        const auto& picture = *reinterpret_cast<const AvcPictureInfo*>(decoder->frames.front()->opaque_ref->data);
        pictures.insert_or_assign(output->frameBuffer, OutputRecord{handle, output->frameBufferSize, picture});
    }
    decoder->frames.pop_front();
    decoder->output.reset();
    output->isLastFrame = decoder->finalized && decoder->frames.empty();
    decoder->condition.notify_one();
    return 0;
}

int APS5_VABI sceVdecswFinalizeDecodeSequence(void* handle) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    std::lock_guard lock(decoder->mutex);
    decoder->CheckFailure();
    if (decoder->resetting) return Error::DecodePending;
    if (decoder->closingSequence) return Error::InvalidSequence;
    decoder->commands.push_back({Command::Kind::Finalize, nullptr, nullptr, nullptr});
    decoder->closingSequence = true;
    decoder->condition.notify_one();
    return 0;
}

int APS5_VABI sceVdecswResetDecoder(void* handle) {
    const auto decoder = findDecoder(handle);
    if (!decoder) return Error::DecoderInstance;
    auto finished = std::make_shared<std::promise<void>>();
    auto result = finished->get_future();
    {
        std::lock_guard lock(decoder->mutex);
        if (decoder->resetting) return Error::DecodePending;
        decoder->commands.clear();
        decoder->commands.push_back({Command::Kind::Reset, nullptr, nullptr, finished});
        decoder->resetting = true;
    }
    decoder->condition.notify_all();
    result.get();
    return 0;
}

int APS5_VABI sceVdecswGetAvcPictureInfo(const OutputInfo* output, AvcPictureInfo* first, AvcPictureInfo* second) {
    if (!output || !first || first == second) return Error::ArgumentPointer;
    if ((output->thisSize != 0x30 && output->thisSize != sizeof(*output)) || first->thisSize != sizeof(*first) || (second && second->thisSize != sizeof(*second))) return Error::StructSize;
    AvcPictureInfo picture{sizeof(AvcPictureInfo)};
    if (output->isValid) {
        if (output->codecType != 1 || output->pictureCount != 1 || !output->frameBuffer) return Error::InvalidOutputInfo;
        std::lock_guard lock(registryMutex);
        const auto found = pictures.find(output->frameBuffer);
        if (found == pictures.end() || found->second.bytes != output->frameBufferSize) return Error::InvalidOutputInfo;
        picture = found->second.picture;
    } else if (output->pictureCount != 0) return Error::InvalidOutputInfo;
    *first = picture;
    if (second) *second = AvcPictureInfo{sizeof(AvcPictureInfo)};
    return 0;
}

}
