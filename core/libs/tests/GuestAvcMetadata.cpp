#include "prx/libSceVdecsw/AvcMetadata.hpp"
#include "AvcMetadataFixture.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using SceVdecsw::AvcMetadataParser;
using SceVdecsw::AvcPictureInfo;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Writer {
public:
    void Put(std::uint32_t value, unsigned count) {
        for (unsigned index = count; index > 0; --index) {
            if (position % 8 == 0) bytes.push_back(0);
            bytes.back() |= ((value >> (index - 1)) & 1) << (7 - position % 8);
            ++position;
        }
    }
    void Ue(std::uint32_t value) {
        const auto encoded = std::uint64_t{value} + 1;
        unsigned size = 0;
        for (auto remaining = encoded; remaining; remaining >>= 1) ++size;
        Put(0, size - 1);
        Put(static_cast<std::uint32_t>(encoded), size);
    }
    void Se(int value) { Ue(value > 0 ? 2 * value - 1 : -2 * value); }
    Bytes Finish() {
        Put(1, 1);
        while (position % 8) Put(0, 1);
        return bytes;
    }
    Bytes Payload() {
        if (position % 8) return Finish();
        return bytes;
    }

private:
    Bytes bytes;
    unsigned position = 0;
};

Bytes Nal(unsigned header, const Bytes& rbsp) {
    Bytes bytes{0, 0, 0, 1, static_cast<std::uint8_t>(header)};
    unsigned zeros = 0;
    for (const auto byte : rbsp) {
        if (zeros == 2 && byte <= 3) {
            bytes.push_back(3);
            zeros = 0;
        }
        bytes.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return bytes;
}

Bytes Join(std::initializer_list<Bytes> chunks) {
    Bytes joined;
    for (const auto& chunk : chunks) joined.insert(joined.end(), chunk.begin(), chunk.end());
    return joined;
}

struct Configuration {
    unsigned profile = 100, spsId = 0, ppsId = 0, width = 3, height = 2, pocType = 0;
    unsigned chroma = 1, depth = 0, groups = 0, frameOnly = 1, constraints = 0, cropRight = 0;
    unsigned clockStruct = 7;
    bool vui = false, hrd = false, scaling = false, bottomPoc = false, deltaAlwaysZero = false;
};

void Hrd(Writer& writer) {
    writer.Ue(1);
    writer.Put(2, 4);
    writer.Put(3, 4);
    for (unsigned index = 0; index < 2; ++index) {
        writer.Ue(12 + index);
        writer.Ue(16 + index);
        writer.Put(1, 1);
    }
    writer.Put(4, 5);
    writer.Put(3, 5);
    writer.Put(5, 5);
    writer.Put(3, 5);
}

Bytes Sps(const Configuration& config = {}) {
    Writer writer;
    writer.Put(config.profile, 8);
    writer.Put(config.constraints, 8);
    writer.Put(40, 8);
    writer.Ue(config.spsId);
    if (config.profile == 100) {
        writer.Ue(config.chroma);
        writer.Ue(config.depth);
        writer.Ue(config.depth);
        writer.Put(0, 1);
        writer.Put(config.scaling, 1);
        if (config.scaling) {
            for (unsigned index = 0; index < 8; ++index) {
                writer.Put(1, 1);
                writer.Se(-8);
            }
        }
    }
    writer.Ue(0);
    writer.Ue(config.pocType);
    if (config.pocType == 0) writer.Ue(0);
    if (config.pocType == 1) {
        writer.Put(config.deltaAlwaysZero, 1);
        writer.Se(-1);
        writer.Se(2);
        writer.Ue(2);
        writer.Se(1);
        writer.Se(-2);
    }
    writer.Ue(2);
    writer.Put(0, 1);
    writer.Ue(config.width);
    writer.Ue(config.height);
    writer.Put(config.frameOnly, 1);
    if (!config.frameOnly) writer.Put(0, 1);
    writer.Put(1, 1);
    writer.Put(1, 1);
    writer.Ue(0);
    writer.Ue(config.cropRight);
    writer.Ue(0);
    writer.Ue(4);
    writer.Put(config.vui, 1);
    if (config.vui) {
        writer.Put(1, 1);
        writer.Put(255, 8);
        writer.Put(4, 16);
        writer.Put(3, 16);
        writer.Put(1, 1);
        writer.Put(1, 1);
        writer.Put(1, 1);
        writer.Put(5, 3);
        writer.Put(1, 1);
        writer.Put(1, 1);
        writer.Put(9, 8);
        writer.Put(16, 8);
        writer.Put(9, 8);
        writer.Put(1, 1);
        writer.Ue(2);
        writer.Ue(3);
        writer.Put(1, 1);
        writer.Put(1001, 32);
        writer.Put(60000, 32);
        writer.Put(1, 1);
        writer.Put(config.hrd, 1);
        if (config.hrd) Hrd(writer);
        writer.Put(config.hrd, 1);
        if (config.hrd) Hrd(writer);
        if (config.hrd) writer.Put(0, 1);
        writer.Put(1, 1);
        writer.Put(1, 1);
        writer.Put(1, 1);
        writer.Ue(2);
        writer.Ue(1);
        writer.Ue(8);
        writer.Ue(8);
        writer.Ue(2);
        writer.Ue(5);
    }
    return Nal(0x67, writer.Finish());
}

Bytes Pps(const Configuration& config = {}) {
    Writer writer;
    writer.Ue(config.ppsId);
    writer.Ue(config.spsId);
    writer.Put(0, 1);
    writer.Put(config.bottomPoc, 1);
    writer.Ue(config.groups);
    writer.Ue(0);
    writer.Ue(0);
    writer.Put(0, 1);
    writer.Put(0, 2);
    writer.Se(-2);
    writer.Se(0);
    writer.Se(1);
    writer.Put(1, 1);
    writer.Put(0, 1);
    writer.Put(0, 1);
    if (config.scaling) {
        writer.Put(1, 1);
        writer.Put(1, 1);
        for (unsigned index = 0; index < 8; ++index) {
            writer.Put(1, 1);
            writer.Se(-8);
        }
        writer.Se(-1);
    }
    return Nal(0x68, writer.Finish());
}

Bytes Slice(const Configuration& config = {}, unsigned firstMb = 0, unsigned frameNum = 0, unsigned poc = 0, bool idr = true) {
    Writer writer;
    writer.Ue(firstMb);
    writer.Ue(idr ? 2 : 0);
    writer.Ue(config.ppsId);
    writer.Put(frameNum, 4);
    if (idr) writer.Ue(0);
    if (config.pocType == 0) {
        writer.Put(poc, 4);
        if (config.bottomPoc) writer.Se(-2);
    }
    if (config.pocType == 1 && !config.deltaAlwaysZero) {
        writer.Se(3);
        if (config.bottomPoc) writer.Se(-3);
    }
    writer.Put(0xab, 8);
    return Nal(idr ? 0x65 : 0x41, writer.Finish());
}

Bytes Sei(unsigned type, const Bytes& payload) {
    Bytes rbsp;
    while (type >= 255) {
        rbsp.push_back(255);
        type -= 255;
    }
    rbsp.push_back(type);
    auto size = payload.size();
    while (size >= 255) {
        rbsp.push_back(255);
        size -= 255;
    }
    rbsp.push_back(size);
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
    rbsp.push_back(0x80);
    return Nal(6, rbsp);
}

Bytes BufferingPeriod(const Configuration& config) {
    Writer writer;
    writer.Ue(config.spsId);
    if (config.hrd) {
        for (unsigned index = 0; index < 4; ++index) {
            writer.Put(13, 5);
            writer.Put(7, 5);
        }
    }
    return Sei(0, writer.Payload());
}

Bytes PictureTiming(const Configuration& config, bool fullTimestamp = true) {
    Writer writer;
    if (config.hrd) {
        writer.Put(3, 4);
        writer.Put(7, 6);
    }
    writer.Put(config.clockStruct, 4);
    writer.Put(1, 1);
    writer.Put(2, 2);
    writer.Put(0, 1);
    writer.Put(6, 5);
    writer.Put(fullTimestamp, 1);
    writer.Put(0, 1);
    writer.Put(1, 1);
    writer.Put(29, 8);
    if (!fullTimestamp) writer.Put(1, 1);
    writer.Put(59, 6);
    if (!fullTimestamp) writer.Put(1, 1);
    writer.Put(58, 6);
    if (!fullTimestamp) writer.Put(1, 1);
    writer.Put(23, 5);
    writer.Put(7, config.hrd ? 3 : 24);
    writer.Put(0, 1);
    return Sei(1, writer.Payload());
}

AvcPictureInfo Parse(AvcMetadataParser& parser, const Bytes& bytes) {
    const auto info = parser.ParseAccessUnit(bytes, 0xfedcba9876543210, 1234567, 0xffffffffffffffff);
    Check(info.has_value(), "valid access unit rejected");
    return *info;
}

bool Rejects(AvcMetadataParser& parser, const Bytes& bytes, std::uint64_t pts, std::uint64_t dts, std::uint64_t attached) {
    try {
        parser.ParseAccessUnit(bytes, pts, dts, attached);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    } catch (const std::runtime_error&) {
        return true;
    }
}
void TestAllFields() {
    AvcMetadataParser parser;
    Configuration config;
    config.width = 127;
    config.height = 71;
    config.cropRight = 1;
    config.constraints = 0xfc;
    config.vui = config.hrd = config.scaling = true;
    const auto bytes = Join({Nal(9, {0xf0}), Sps(config), Pps(config), BufferingPeriod(config),
        PictureTiming(config), Sei(260, Bytes(300, 7)), Slice(config), Nal(12, {255, 255, 0x80}), Nal(10, {}), Nal(11, {})});
    const auto info = Parse(parser, bytes);
    AvcPictureInfo expected{};
    expected.thisSize = 0x78;
    expected.isValid = 1;
    expected.ptsData = 0xfedcba9876543210;
    expected.dtsData = 1234567;
    expected.attachedData = 0xffffffffffffffff;
    expected.idrPictureFlag = 1;
    expected.profileIdc = 100;
    expected.levelIdc = 40;
    expected.picWidthInMbsMinus1 = 127;
    expected.picHeightInMapUnitsMinus1 = 71;
    expected.frameMbsOnlyFlag = expected.frameCroppingFlag = 1;
    expected.frameCropRightOffset = 1;
    expected.frameCropBottomOffset = 4;
    expected.aspectRatioInfoPresentFlag = 1;
    expected.aspectRatioIdc = 255;
    expected.sarWidth = 4;
    expected.sarHeight = 3;
    expected.videoSignalTypePresentFlag = 1;
    expected.videoFormat = 5;
    expected.videoFullRangeFlag = expected.colourDescriptionPresentFlag = 1;
    expected.colourPrimaries = 9;
    expected.transferCharacteristics = 16;
    expected.matrixCoefficients = 9;
    expected.timingInfoPresentFlag = expected.fixedFrameRateFlag = 1;
    expected.numUnitsInTick = 1001;
    expected.timeScale = 60000;
    expected.bitstreamRestrictionFlag = 1;
    expected.maxDecFrameBuffering = 5;
    expected.picStructPresentFlag = 1;
    expected.picStruct = 7;
    expected.sequenceParameterSetPresentFlag = expected.pictureParameterSetPresentFlag = 1;
    expected.auDelimiterPresentFlag = expected.endOfSequencePresentFlag = expected.endOfStreamPresentFlag = 1;
    expected.fillerDataPresentFlag = expected.pictureTimingSeiPresentFlag = expected.bufferingPeriodSeiPresentFlag = 1;
    expected.constraintSet0Flag = expected.constraintSet1Flag = expected.constraintSet2Flag = 1;
    expected.constraintSet3Flag = expected.constraintSet4Flag = expected.constraintSet5Flag = 1;
    Check(std::memcmp(&info, &expected, sizeof(info)) == 0, "full metadata bytes differ");
    config.hrd = false;
    const auto withoutHrd = Parse(parser, Join({Sps(config), Pps(config), PictureTiming(config, false), Slice(config)}));
    Check(withoutHrd.picStruct == 7 && withoutHrd.pictureTimingSeiPresentFlag, "inferred time offset parsing failed");
}

void TestParameterLifetime() {
    AvcMetadataParser parser;
    Check(!parser.ParseAccessUnit(Join({Sps(), Pps()}), 0, 0, 0), "headers produced picture metadata");
    const auto first = Parse(parser, Slice());
    Check(first.picWidthInMbsMinus1 == 3 && !first.sequenceParameterSetPresentFlag && !first.timingInfoPresentFlag,
        "cached sequence state or gated absent fields wrong");
    Check(!first.videoFormat && !first.colourPrimaries && !first.maxDecFrameBuffering && !first.picStruct,
        "absent raw fields were synthesized");
    Configuration changed;
    changed.width = 7;
    changed.profile = 66;
    const auto second = Parse(parser, Join({Sps(changed), Pps(changed), Slice(changed)}));
    Check(first.picWidthInMbsMinus1 == 3 && second.picWidthInMbsMinus1 == 7 && second.profileIdc == 66,
        "metadata snapshot changed after sequence replacement");
    parser.Reset();
    Check(Rejects(parser, Slice(changed), 0, 0, 0), "reset retained parameter sets");
    Parse(parser, Join({Sps(), Pps(), Slice()}));
    auto broken = Sps(changed);
    broken.pop_back();
    Check(Rejects(parser, broken, 0, 0, 0), "truncated replacement accepted");
    Check(Rejects(parser, Slice(), 0, 0, 0), "failed parse left stale parameter sets active");
}

void TestSlices() {
    for (unsigned poc = 0; poc < 3; ++poc) {
        for (bool deltaZero : {false, true}) {
            AvcMetadataParser parser;
            Configuration config;
            config.pocType = poc;
            config.bottomPoc = true;
            config.deltaAlwaysZero = deltaZero;
            Parse(parser, Join({Sps(config), Pps(config), Slice(config), Slice(config, 2)}));
            const auto predicted = Parse(parser, Slice(config, 0, 1, 2, false));
            Check(!predicted.idrPictureFlag, "non-IDR slice marked IDR");
            Check(Rejects(parser, Join({Slice(config, 0, 1, 2, false), Slice(config, 2, 2, 4, false)}), 0, 0, 0),
                "different pictures accepted as one access unit");
        }
    }
    AvcMetadataParser parser;
    Check(Rejects(parser, Join({Sps(), Pps(), Slice({}, 1)}), 0, 0, 0), "missing initial slice accepted");
    Check(Rejects(parser, Join({Sps(), Pps(), Slice(), Slice()}), 0, 0, 0), "duplicate picture accepted");
    Check(Rejects(parser, Join({Sps(), Pps(), Slice({}, 12)}), 0, 0, 0), "out of range macroblock accepted");
}

void TestRejectedSyntax() {
    std::vector<Configuration> unsupported;
    Configuration config;
    config.frameOnly = 0; unsupported.push_back(config);
    config = {}; config.chroma = 2; unsupported.push_back(config);
    config = {}; config.depth = 2; unsupported.push_back(config);
    config = {}; config.groups = 1; unsupported.push_back(config);
    config = {}; config.profile = 110; unsupported.push_back(config);
    config = {}; config.spsId = 32; unsupported.push_back(config);
    config = {}; config.ppsId = 256; unsupported.push_back(config);
    config = {}; config.constraints = 3; unsupported.push_back(config);
    config = {}; config.cropRight = 32; unsupported.push_back(config);
    config = {}; config.pocType = 3; unsupported.push_back(config);
    for (const auto& item : unsupported) {
        AvcMetadataParser parser;
        Check(Rejects(parser, Join({Sps(item), Pps(item), Slice(item)}), 0, 0, 0), "unsupported syntax accepted");
    }
    const auto sequence = Sps();
    const auto picture = Pps();
    for (std::size_t size = 5; size < sequence.size(); ++size) {
        AvcMetadataParser parser;
        Check(Rejects(parser, Join({Bytes(sequence.begin(), sequence.begin() + size), picture, Slice()}), 0, 0, 0),
            "truncated SPS accepted");
    }
    for (std::size_t size = 5; size < picture.size(); ++size) {
        AvcMetadataParser parser;
        Check(Rejects(parser, Join({sequence, Bytes(picture.begin(), picture.begin() + size), Slice()}), 0, 0, 0),
            "truncated PPS accepted");
    }
    for (const Bytes& malformed : {Bytes{}, Bytes{1, 2, 3}, Bytes{0, 0, 1}, Bytes{0, 0, 1, 0x87, 0x80},
            Bytes{0, 0, 1, 7, 0, 0, 3, 4}, Bytes{0, 0, 1, 7, 0, 0, 3}, Bytes{0, 0, 1, 7, 0, 0, 2},
            Nal(0x67, Bytes(30, 0)), Nal(20, {0x80}), Nal(6, {1, 255, 0x80})}) {
        AvcMetadataParser parser;
        Check(Rejects(parser, malformed, 0, 0, 0), "malformed NAL accepted");
    }
    AvcMetadataParser parser;
    config = {}; config.vui = config.hrd = true;
    Check(Rejects(parser, Join({Sps(config), Pps(config), Sei(1, {}), Slice(config)}), 0, 0, 0), "empty timing SEI accepted");
    config.clockStruct = 9;
    Check(Rejects(parser, Join({Sps(config), Pps(config), PictureTiming(config), Slice(config)}), 0, 0, 0), "reserved picture structure accepted");
    for (unsigned fieldStructure : {1u, 2u}) {
        config.clockStruct = fieldStructure;
        Check(Rejects(parser, Join({Sps(config), Pps(config), PictureTiming(config), Slice(config)}), 0, 0, 0),
            "field picture timing accepted for progressive sequence");
    }
    Check(Rejects(parser, Join({Sps(), Pps(), Sei(1, {}), Slice()}), 0, 0, 0), "ungated picture timing accepted");
    auto referenceSei = Sei(5, {1, 2, 3});
    referenceSei[4] |= 0x20;
    Check(Rejects(parser, Join({Sps(), Pps(), referenceSei, Slice()}), 0, 0, 0), "nonzero SEI nal_ref_idc accepted");
    Check(Rejects(parser, Join({Sps(), Pps(), Nal(9, {0x10}), Nal(9, {0x10}), Slice()}), 0, 0, 0), "duplicate delimiter accepted");
}

void TestErrorCategories() {
    AvcMetadataParser parser;
    bool invalid = false;
    try {
        parser.ParseAccessUnit(Bytes{0, 0, 1}, 0, 0, 0);
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    Check(invalid, "malformed input did not produce invalid_argument");
    Configuration config;
    config.frameOnly = 0;
    bool unsupported = false;
    try {
        parser.ParseAccessUnit(Join({Sps(config), Pps(config), Slice(config)}), 0, 0, 0);
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("unsupported feature was classified as malformed input");
    } catch (const std::runtime_error&) {
        unsupported = true;
    }
    Check(unsupported, "unsupported input did not produce runtime_error");
    Check(!parser.ParseAccessUnit(Join({Sps(), Pps()}), 0, 0, 0), "valid headers-only input was rejected");
    Parse(parser, Slice());
}

void TestSyntheticStream() {
    std::vector<std::size_t> offsets;
    for (std::size_t index = 0; index + 4 < AvcMetadataStream.size(); ++index) {
        if (AvcMetadataStream[index] == 0 && AvcMetadataStream[index + 1] == 0 && AvcMetadataStream[index + 2] == 0 &&
            AvcMetadataStream[index + 3] == 1 && AvcMetadataStream[index + 4] == 9) offsets.push_back(index);
    }
    offsets.push_back(AvcMetadataStream.size());
    Check(offsets.size() == 7, "synthetic fixture is not six pictures");
    AvcMetadataParser parser;
    for (unsigned index = 0; index < 6; ++index) {
        const auto bytes = std::span(AvcMetadataStream).subspan(offsets[index], offsets[index + 1] - offsets[index]);
        const auto info = parser.ParseAccessUnit(bytes, index * 1001, index + 77, index + 100);
        Check(info.has_value(), "synthetic B-frame stream rejected");
        Check(info->profileIdc == 100 && info->levelIdc == 10 && info->picWidthInMbsMinus1 == 3 &&
            info->picHeightInMapUnitsMinus1 == 2 && info->frameCropBottomOffset == 4, "synthetic dimensions/profile wrong");
        Check(info->ptsData == index * 1001 && info->dtsData == index + 77 && info->attachedData == index + 100,
            "synthetic packet identity changed");
        Check(info->auDelimiterPresentFlag && info->idrPictureFlag == (index == 0) &&
            info->sequenceParameterSetPresentFlag == (index == 0) && info->pictureParameterSetPresentFlag == (index == 0),
            "synthetic per-access-unit flags persisted");
        Check(info->aspectRatioIdc == 1 && info->sarWidth == 0 && info->sarHeight == 0 &&
            info->numUnitsInTick == 1 && info->timeScale == 60 && info->maxDecFrameBuffering == 4,
            "synthetic VUI differs");
    }
}

void TestDeterministicMutations() {
    Configuration config;
    config.vui = config.hrd = config.scaling = true;
    const auto seed = Join({Sps(config), Pps(config), BufferingPeriod(config), PictureTiming(config), Slice(config)});
    std::uint32_t state = 0x5ead1234;
    const auto random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (unsigned iteration = 0; iteration < 4000; ++iteration) {
        auto mutated = seed;
        const auto changes = 1 + random() % 8;
        for (unsigned index = 0; index < changes; ++index) mutated[random() % mutated.size()] ^= 1u << (random() % 8);
        if (iteration & 1) mutated.resize(random() % mutated.size());
        AvcMetadataParser parser;
        std::optional<AvcPictureInfo> info;
        try {
            info = parser.ParseAccessUnit(mutated, 1, 2, 3);
        } catch (const std::invalid_argument&) {
        } catch (const std::runtime_error&) {
        }
        if (info) Check(info->thisSize == 0x78 && info->isValid && info->frameMbsOnlyFlag &&
            info->ptsData == 1 && info->dtsData == 2 && info->attachedData == 3, "mutated metadata broke output invariants");
    }
}

}

int main() {
    try {
        TestAllFields();
        TestParameterLifetime();
        TestSlices();
        TestRejectedSyntax();
        TestErrorCategories();
        TestSyntheticStream();
        TestDeterministicMutations();
        std::puts("AVC metadata parser tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "AVC metadata parser test failed: %s\n", error.what());
        return 1;
    }
}
