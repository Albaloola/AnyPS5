#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace SceVdecsw {

struct AvcPictureInfo {
    std::uint64_t thisSize;
    std::uint8_t isValid, reserved0[7];
    std::uint64_t ptsData, dtsData, attachedData;
    std::uint8_t idrPictureFlag, profileIdc, levelIdc, reserved1;
    std::uint32_t picWidthInMbsMinus1, picHeightInMapUnitsMinus1;
    std::uint8_t frameMbsOnlyFlag, frameCroppingFlag;
    std::uint16_t reserved2;
    std::uint32_t frameCropLeftOffset, frameCropRightOffset, frameCropTopOffset, frameCropBottomOffset;
    std::uint8_t aspectRatioInfoPresentFlag, aspectRatioIdc;
    std::uint16_t sarWidth, sarHeight;
    std::uint8_t videoSignalTypePresentFlag, videoFormat, videoFullRangeFlag, colourDescriptionPresentFlag;
    std::uint8_t colourPrimaries, transferCharacteristics, matrixCoefficients, timingInfoPresentFlag;
    std::uint16_t reserved3;
    std::uint32_t numUnitsInTick, timeScale;
    std::uint8_t fixedFrameRateFlag, bitstreamRestrictionFlag, maxDecFrameBuffering, picStructPresentFlag;
    std::uint8_t picStruct, fieldPicFlag, bottomFieldFlag, sequenceParameterSetPresentFlag;
    std::uint8_t pictureParameterSetPresentFlag, auDelimiterPresentFlag, endOfSequencePresentFlag, endOfStreamPresentFlag;
    std::uint8_t fillerDataPresentFlag, pictureTimingSeiPresentFlag, bufferingPeriodSeiPresentFlag;
    std::uint8_t constraintSet0Flag, constraintSet1Flag, constraintSet2Flag, constraintSet3Flag, constraintSet4Flag, constraintSet5Flag;
    std::uint8_t reserved4[3];
};

static_assert(sizeof(AvcPictureInfo) == 0x78);
static_assert(offsetof(AvcPictureInfo, ptsData) == 0x10);
static_assert(offsetof(AvcPictureInfo, picWidthInMbsMinus1) == 0x2c);
static_assert(offsetof(AvcPictureInfo, numUnitsInTick) == 0x58);
static_assert(offsetof(AvcPictureInfo, constraintSet5Flag) == 0x74);

class AvcMetadataParser {
public:
    AvcMetadataParser();
    ~AvcMetadataParser();
    AvcMetadataParser(const AvcMetadataParser&) = delete;
    AvcMetadataParser& operator=(const AvcMetadataParser&) = delete;
    std::optional<AvcPictureInfo> ParseAccessUnit(std::span<const std::uint8_t> annexB, std::uint64_t pts,
                                                std::uint64_t dts, std::uint64_t attached);
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
