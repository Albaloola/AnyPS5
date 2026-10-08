#include "AvcMetadata.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace SceVdecsw {
namespace {

struct InvalidData {};
struct UnsupportedData {
    const char* message;
};

void Require(bool condition) {
    if (!condition) throw InvalidData{};
}

void Supported(bool condition, const char* message) {
    if (!condition) throw UnsupportedData{message};
}

class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> bytes) : bytes(bytes) {
        Require(bytes.size() <= std::numeric_limits<std::size_t>::max() / 8);
    }

    std::uint32_t Read(unsigned count) {
        Require(count <= 32 && count <= Remaining());
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index, ++position)
            value = (value << 1) | ((bytes[position / 8] >> (7 - position % 8)) & 1);
        return value;
    }

    std::uint32_t Ue(std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max() - 1) {
        unsigned zeros = 0;
        while (Read(1) == 0) Require(++zeros <= 31);
        const auto value = ((std::uint64_t{1} << zeros) - 1) + Read(zeros);
        Require(value <= maximum);
        return static_cast<std::uint32_t>(value);
    }

    std::int32_t Se(std::int32_t minimum = std::numeric_limits<std::int32_t>::min() + 1,
                    std::int32_t maximum = std::numeric_limits<std::int32_t>::max()) {
        const auto code = Ue();
        const auto value = (code & 1) ? static_cast<std::int64_t>(code / 2) + 1 : -static_cast<std::int64_t>(code / 2);
        Require(value >= minimum && value <= maximum);
        return static_cast<std::int32_t>(value);
    }

    std::size_t Remaining() const { return bytes.size() * 8 - position; }

    bool More() const {
        if (!Remaining()) return false;
        auto copy = *this;
        if (copy.Read(1) != 1) return true;
        while (copy.Remaining()) if (copy.Read(1)) return true;
        return false;
    }

    void Finish() {
        Require(Remaining() > 0 && Remaining() <= 8 && Read(1) == 1);
        while (Remaining()) Require(Read(1) == 0);
    }

    void FinishPayload() {
        if (Remaining()) Finish();
    }

private:
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
};

struct Hrd {
    unsigned cpbCount, initialDelayBits, cpbDelayBits, dpbDelayBits, timeOffsetBits;
};

struct Sps {
    AvcPictureInfo info{};
    unsigned id = 0, frameBits = 0, pocType = 0, pocBits = 0;
    bool deltaAlwaysZero = false;
    std::optional<Hrd> nalHrd, vclHrd;
};

struct Pps {
    unsigned id, spsId;
    bool bottomFieldPoc, redundant;
};

struct Parameters {
    std::array<std::optional<Sps>, 32> sequence;
    std::array<std::optional<Pps>, 256> picture;
};

struct Slice {
    std::uint32_t firstMb = 0, ppsId = 0, frameNum = 0, idrId = 0, poc = 0;
    std::int32_t deltaBottom = 0, delta0 = 0, delta1 = 0;
    bool idr = false, reference = false;

    bool SamePicture(const Slice& other) const {
        return ppsId == other.ppsId && frameNum == other.frameNum && idrId == other.idrId &&
            poc == other.poc && deltaBottom == other.deltaBottom && delta0 == other.delta0 &&
            delta1 == other.delta1 && idr == other.idr && reference == other.reference;
    }
};

void ScalingList(Bits& bits, unsigned size) {
    int last = 8, next = 8;
    for (unsigned index = 0; index < size; ++index) {
        if (next) next = (last + bits.Se(-128, 127) + 256) % 256;
        if (next) last = next;
    }
}

Hrd ReadHrd(Bits& bits) {
    Hrd hrd{};
    hrd.cpbCount = bits.Ue(31) + 1;
    bits.Read(4);
    bits.Read(4);
    for (unsigned index = 0; index < hrd.cpbCount; ++index) {
        bits.Ue();
        bits.Ue();
        bits.Read(1);
    }
    hrd.initialDelayBits = bits.Read(5) + 1;
    hrd.cpbDelayBits = bits.Read(5) + 1;
    hrd.dpbDelayBits = bits.Read(5) + 1;
    hrd.timeOffsetBits = bits.Read(5);
    return hrd;
}

void ReadVui(Bits& bits, Sps& sps, unsigned maxReferences) {
    auto& info = sps.info;
    info.aspectRatioInfoPresentFlag = bits.Read(1);
    if (info.aspectRatioInfoPresentFlag) {
        info.aspectRatioIdc = bits.Read(8);
        Require(info.aspectRatioIdc <= 16 || info.aspectRatioIdc == 255);
        if (info.aspectRatioIdc == 255) {
            info.sarWidth = bits.Read(16);
            info.sarHeight = bits.Read(16);
        }
    }
    if (bits.Read(1)) bits.Read(1);
    info.videoSignalTypePresentFlag = bits.Read(1);
    if (info.videoSignalTypePresentFlag) {
        info.videoFormat = bits.Read(3);
        Require(info.videoFormat <= 5);
        info.videoFullRangeFlag = bits.Read(1);
        info.colourDescriptionPresentFlag = bits.Read(1);
        if (info.colourDescriptionPresentFlag) {
            info.colourPrimaries = bits.Read(8);
            info.transferCharacteristics = bits.Read(8);
            info.matrixCoefficients = bits.Read(8);
        }
    }
    if (bits.Read(1)) {
        bits.Ue(5);
        bits.Ue(5);
    }
    info.timingInfoPresentFlag = bits.Read(1);
    if (info.timingInfoPresentFlag) {
        info.numUnitsInTick = bits.Read(32);
        info.timeScale = bits.Read(32);
        Require(info.numUnitsInTick != 0 && info.timeScale != 0);
        info.fixedFrameRateFlag = bits.Read(1);
    }
    if (bits.Read(1)) sps.nalHrd = ReadHrd(bits);
    if (bits.Read(1)) sps.vclHrd = ReadHrd(bits);
    if (sps.nalHrd && sps.vclHrd) {
        Require(sps.nalHrd->cpbDelayBits == sps.vclHrd->cpbDelayBits &&
            sps.nalHrd->dpbDelayBits == sps.vclHrd->dpbDelayBits &&
            sps.nalHrd->timeOffsetBits == sps.vclHrd->timeOffsetBits);
    }
    if (sps.nalHrd || sps.vclHrd) bits.Read(1);
    info.picStructPresentFlag = bits.Read(1);
    info.bitstreamRestrictionFlag = bits.Read(1);
    if (info.bitstreamRestrictionFlag) {
        bits.Read(1);
        bits.Ue(16);
        bits.Ue(16);
        bits.Ue(16);
        bits.Ue(16);
        const auto reorder = bits.Ue(16);
        info.maxDecFrameBuffering = bits.Ue(16);
        Require(reorder <= info.maxDecFrameBuffering && maxReferences <= info.maxDecFrameBuffering);
    }
}

Sps ReadSps(Bits& bits) {
    Sps sps;
    auto& info = sps.info;
    info.profileIdc = bits.Read(8);
    switch (info.profileIdc) {
    case 66: case 77: case 88: case 100:
        break;
    case 110: case 122: case 244: case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135:
        throw UnsupportedData{"AVC metadata profile is unsupported"};
    default:
        throw InvalidData{};
    }
    info.constraintSet0Flag = bits.Read(1);
    info.constraintSet1Flag = bits.Read(1);
    info.constraintSet2Flag = bits.Read(1);
    info.constraintSet3Flag = bits.Read(1);
    info.constraintSet4Flag = bits.Read(1);
    info.constraintSet5Flag = bits.Read(1);
    Require(bits.Read(2) == 0);
    info.levelIdc = bits.Read(8);
    sps.id = bits.Ue(31);
    if (info.profileIdc == 100) {
        Supported(bits.Ue(3) == 1, "AVC metadata requires 4:2:0 chroma");
        const auto lumaDepth = bits.Ue(6);
        const auto chromaDepth = bits.Ue(6);
        Supported(lumaDepth == 0 && chromaDepth == 0, "AVC metadata requires 8-bit samples");
        bits.Read(1);
        if (bits.Read(1)) {
            for (unsigned index = 0; index < 8; ++index)
                if (bits.Read(1)) ScalingList(bits, index < 6 ? 16 : 64);
        }
    }
    sps.frameBits = bits.Ue(12) + 4;
    sps.pocType = bits.Ue(2);
    if (sps.pocType == 0) sps.pocBits = bits.Ue(12) + 4;
    if (sps.pocType == 1) {
        sps.deltaAlwaysZero = bits.Read(1);
        bits.Se();
        bits.Se();
        const auto cycle = bits.Ue(255);
        for (unsigned index = 0; index < cycle; ++index) bits.Se();
    }
    const auto maxReferences = bits.Ue(16);
    bits.Read(1);
    info.picWidthInMbsMinus1 = bits.Ue(65535);
    info.picHeightInMapUnitsMinus1 = bits.Ue(65535);
    info.frameMbsOnlyFlag = bits.Read(1);
    Supported(info.frameMbsOnlyFlag == 1, "AVC metadata requires progressive frame coding");
    bits.Read(1);
    info.frameCroppingFlag = bits.Read(1);
    if (info.frameCroppingFlag) {
        info.frameCropLeftOffset = bits.Ue();
        info.frameCropRightOffset = bits.Ue();
        info.frameCropTopOffset = bits.Ue();
        info.frameCropBottomOffset = bits.Ue();
        Require((std::uint64_t{info.frameCropLeftOffset} + info.frameCropRightOffset) * 2 <
            (std::uint64_t{info.picWidthInMbsMinus1} + 1) * 16);
        Require((std::uint64_t{info.frameCropTopOffset} + info.frameCropBottomOffset) * 2 <
            (std::uint64_t{info.picHeightInMapUnitsMinus1} + 1) * 16);
    }
    if (bits.Read(1)) ReadVui(bits, sps, maxReferences);
    bits.Finish();
    return sps;
}

Pps ReadPps(Bits& bits, const Parameters& parameters) {
    Pps pps{};
    pps.id = bits.Ue(255);
    pps.spsId = bits.Ue(31);
    Require(parameters.sequence[pps.spsId].has_value());
    bits.Read(1);
    pps.bottomFieldPoc = bits.Read(1);
    Supported(bits.Ue(7) == 0, "AVC metadata does not support flexible macroblock ordering");
    bits.Ue(31);
    bits.Ue(31);
    bits.Read(1);
    Require(bits.Read(2) <= 2);
    bits.Se(-26, 25);
    bits.Se(-26, 25);
    bits.Se(-12, 12);
    bits.Read(1);
    bits.Read(1);
    pps.redundant = bits.Read(1);
    if (bits.More()) {
        const auto transform = bits.Read(1);
        if (bits.Read(1)) {
            for (unsigned index = 0; index < 6 + 2 * transform; ++index)
                if (bits.Read(1)) ScalingList(bits, index < 6 ? 16 : 64);
        }
        bits.Se(-12, 12);
    }
    bits.Finish();
    return pps;
}

Slice ReadSlice(Bits& bits, std::uint8_t header, const Parameters& parameters) {
    Slice slice;
    slice.firstMb = bits.Ue();
    const auto type = bits.Ue(9) % 5;
    slice.ppsId = bits.Ue(255);
    Require(parameters.picture[slice.ppsId].has_value());
    const auto& pps = *parameters.picture[slice.ppsId];
    Require(parameters.sequence[pps.spsId].has_value());
    const auto& sps = *parameters.sequence[pps.spsId];
    Require(slice.firstMb < (std::uint64_t{sps.info.picWidthInMbsMinus1} + 1) *
        (std::uint64_t{sps.info.picHeightInMapUnitsMinus1} + 1));
    slice.frameNum = bits.Read(sps.frameBits);
    slice.idr = (header & 31) == 5;
    slice.reference = (header & 0x60) != 0;
    if (slice.idr) {
        Require(slice.reference && slice.frameNum == 0 && (type == 2 || type == 4));
        slice.idrId = bits.Ue(65535);
    }
    if (sps.pocType == 0) {
        slice.poc = bits.Read(sps.pocBits);
        if (pps.bottomFieldPoc) slice.deltaBottom = bits.Se();
    }
    if (sps.pocType == 1 && !sps.deltaAlwaysZero) {
        slice.delta0 = bits.Se();
        if (pps.bottomFieldPoc) slice.delta1 = bits.Se();
    }
    if (pps.redundant) Require(bits.Ue(127) == 0);
    Require(bits.Remaining() > 0);
    return slice;
}

std::vector<std::uint8_t> Unescape(std::span<const std::uint8_t> bytes) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(bytes.size());
    unsigned zeros = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto byte = bytes[index];
        if (zeros == 2) {
            if (byte == 3) {
                Require(index + 1 < bytes.size() && bytes[index + 1] <= 3);
                zeros = 0;
                continue;
            }
            Require(byte > 3);
        }
        rbsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return rbsp;
}

std::vector<std::span<const std::uint8_t>> SplitNals(std::span<const std::uint8_t> bytes) {
    Require(!bytes.empty());
    std::vector<std::span<const std::uint8_t>> nals;
    std::size_t start = 0, zeros = 0;
    bool found = false;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (bytes[index] == 0) {
            ++zeros;
            continue;
        }
        if (bytes[index] == 1 && zeros >= 2) {
            if (found) {
                Require(index - zeros > start);
                nals.push_back(bytes.subspan(start, index - zeros - start));
            } else Require(index == zeros);
            start = index + 1;
            found = true;
        }
        zeros = 0;
    }
    Require(found && bytes.size() - zeros > start);
    nals.push_back(bytes.subspan(start, bytes.size() - zeros - start));
    return nals;
}

struct Sei {
    unsigned type;
    std::vector<std::uint8_t> bytes;
};

void ReadSei(std::span<const std::uint8_t> rbsp, std::vector<Sei>& messages) {
    Require(!rbsp.empty() && rbsp.back() == 0x80);
    std::size_t position = 0;
    const auto size = rbsp.size() - 1;
    const auto readLength = [&]() {
        std::size_t value = 0;
        unsigned byte;
        do {
            Require(position < size);
            byte = rbsp[position++];
            Require(value <= std::numeric_limits<unsigned>::max() - byte);
            value += byte;
        } while (byte == 255);
        return value;
    };
    while (position < size) {
        const auto type = readLength();
        const auto length = readLength();
        Require(length <= size - position);
        if (type <= 1) messages.push_back({static_cast<unsigned>(type), {rbsp.begin() + position, rbsp.begin() + position + length}});
        position += length;
    }
}

void ReadBufferingPeriod(Bits& bits, const Parameters& parameters, unsigned activeSps) {
    const auto id = bits.Ue(31);
    Require(id == activeSps && parameters.sequence[id].has_value());
    const auto& sps = *parameters.sequence[id];
    for (const auto& hrd : {sps.nalHrd, sps.vclHrd}) {
        if (hrd) {
            for (unsigned index = 0; index < hrd->cpbCount; ++index) {
                bits.Read(hrd->initialDelayBits);
                bits.Read(hrd->initialDelayBits);
            }
        }
    }
    bits.FinishPayload();
}

void ReadPictureTiming(Bits& bits, const Sps& sps, AvcPictureInfo& info) {
    const auto hrd = sps.nalHrd ? sps.nalHrd : sps.vclHrd;
    Require(hrd.has_value() || info.picStructPresentFlag);
    if (hrd) {
        bits.Read(hrd->cpbDelayBits);
        bits.Read(hrd->dpbDelayBits);
    }
    if (info.picStructPresentFlag) {
        info.picStruct = bits.Read(4);
        Require(info.picStruct <= 8 && info.picStruct != 1 && info.picStruct != 2);
        Require(info.picStruct < 7 || info.fixedFrameRateFlag);
        constexpr std::array<unsigned, 9> clockCounts{1, 1, 1, 2, 2, 3, 3, 2, 3};
        for (unsigned index = 0; index < clockCounts[info.picStruct]; ++index) {
            if (!bits.Read(1)) continue;
            Require(bits.Read(2) <= 2);
            bits.Read(1);
            Require(bits.Read(5) <= 6);
            const auto full = bits.Read(1);
            bits.Read(1);
            bits.Read(1);
            bits.Read(8);
            if (full || bits.Read(1)) {
                Require(bits.Read(6) <= 59);
                if (full || bits.Read(1)) {
                    Require(bits.Read(6) <= 59);
                    if (full || bits.Read(1)) Require(bits.Read(5) <= 23);
                }
            }
            bits.Read(hrd ? hrd->timeOffsetBits : 24);
        }
    }
    bits.FinishPayload();
}

}

struct AvcMetadataParser::Impl {
    Parameters parameters;
};

AvcMetadataParser::AvcMetadataParser() : impl(std::make_unique<Impl>()) {}
AvcMetadataParser::~AvcMetadataParser() = default;

void AvcMetadataParser::Reset() {
    impl->parameters = {};
}

std::optional<AvcPictureInfo> AvcMetadataParser::ParseAccessUnit(std::span<const std::uint8_t> annexB,
    std::uint64_t pts, std::uint64_t dts, std::uint64_t attached) {
    try {
        auto parameters = impl->parameters;
        std::optional<Slice> picture;
        std::vector<Sei> messages;
        AvcPictureInfo flags{};
        std::uint32_t lastMb = 0;
        for (const auto nal : SplitNals(annexB)) {
            Require((nal.front() & 0x80) == 0);
            const auto type = nal.front() & 31;
            auto rbsp = Unescape(nal.subspan(1));
            Bits bits(rbsp);
            switch (type) {
            case 1:
            case 5: {
                Require(!flags.endOfSequencePresentFlag && !flags.endOfStreamPresentFlag);
                const auto slice = ReadSlice(bits, nal.front(), parameters);
                if (picture) {
                    Require(slice.SamePicture(*picture) && slice.firstMb != lastMb);
                    Supported(slice.firstMb > lastMb, "AVC metadata does not support arbitrary slice ordering");
                }
                else {
                    Require(slice.firstMb == 0);
                    picture = slice;
                }
                lastMb = slice.firstMb;
                break;
            }
            case 6:
                Require(!picture && (nal.front() & 0x60) == 0);
                ReadSei(rbsp, messages);
                break;
            case 7: {
                Require(!picture && (nal.front() & 0x60) != 0);
                const auto sps = ReadSps(bits);
                parameters.sequence[sps.id] = sps;
                flags.sequenceParameterSetPresentFlag = 1;
                break;
            }
            case 8: {
                Require(!picture && (nal.front() & 0x60) != 0);
                const auto pps = ReadPps(bits, parameters);
                parameters.picture[pps.id] = pps;
                flags.pictureParameterSetPresentFlag = 1;
                break;
            }
            case 9:
                Require(!picture && !flags.auDelimiterPresentFlag && (nal.front() & 0x60) == 0);
                bits.Read(3);
                bits.Finish();
                flags.auDelimiterPresentFlag = 1;
                break;
            case 10:
            case 11:
                Require((nal.front() & 0x60) == 0 && rbsp.empty());
                if (type == 10) flags.endOfSequencePresentFlag = 1;
                else flags.endOfStreamPresentFlag = 1;
                break;
            case 12:
                Require((nal.front() & 0x60) == 0 && !rbsp.empty() && rbsp.back() == 0x80);
                for (std::size_t index = 0; index + 1 < rbsp.size(); ++index) Require(rbsp[index] == 255);
                flags.fillerDataPresentFlag = 1;
                break;
            case 2: case 3: case 4: case 13: case 14: case 15: case 19: case 20: case 21:
                throw UnsupportedData{"AVC metadata NAL extension is unsupported"};
            default:
                throw InvalidData{};
            }
        }
        if (!picture) {
            Require(messages.empty());
            impl->parameters = std::move(parameters);
            return std::nullopt;
        }
        const auto spsId = parameters.picture[picture->ppsId]->spsId;
        const auto& sps = *parameters.sequence[spsId];
        auto info = sps.info;
        for (const auto& message : messages) {
            Bits bits(message.bytes);
            if (message.type == 0) {
                Require(!flags.bufferingPeriodSeiPresentFlag);
                ReadBufferingPeriod(bits, parameters, spsId);
                flags.bufferingPeriodSeiPresentFlag = 1;
            } else {
                Require(!flags.pictureTimingSeiPresentFlag);
                ReadPictureTiming(bits, sps, info);
                flags.pictureTimingSeiPresentFlag = 1;
            }
        }
        info.thisSize = sizeof(info);
        info.isValid = 1;
        info.ptsData = pts;
        info.dtsData = dts;
        info.attachedData = attached;
        info.idrPictureFlag = picture->idr;
        info.sequenceParameterSetPresentFlag = flags.sequenceParameterSetPresentFlag;
        info.pictureParameterSetPresentFlag = flags.pictureParameterSetPresentFlag;
        info.auDelimiterPresentFlag = flags.auDelimiterPresentFlag;
        info.endOfSequencePresentFlag = flags.endOfSequencePresentFlag;
        info.endOfStreamPresentFlag = flags.endOfStreamPresentFlag;
        info.fillerDataPresentFlag = flags.fillerDataPresentFlag;
        info.pictureTimingSeiPresentFlag = flags.pictureTimingSeiPresentFlag;
        info.bufferingPeriodSeiPresentFlag = flags.bufferingPeriodSeiPresentFlag;
        impl->parameters = std::move(parameters);
        return info;
    } catch (const InvalidData&) {
        Reset();
        throw std::invalid_argument("Invalid AVC metadata access unit");
    } catch (const UnsupportedData& error) {
        Reset();
        throw std::runtime_error(error.message);
    }
}

}
