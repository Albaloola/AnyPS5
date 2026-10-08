#include "prx/libkernel/Module/EhFrame.hpp"
#include <array>
#include <cstdio>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Function>
static bool Throws(Function function) {
    try { function(); } catch (const std::runtime_error&) { return true; }
    return false;
}

int main() {
    try {
        std::array<std::uint8_t, 128> image{};
        const auto base = reinterpret_cast<std::uintptr_t>(image.data());
        std::size_t limit = image.size();
        const auto contains = [&](std::uintptr_t address, std::uint64_t size) {
            return address >= base && address - base <= limit && size <= limit - (address - base);
        };
        const auto store = [&]<typename T>(std::size_t offset, T value) {
            std::memcpy(image.data() + offset, &value, sizeof(value));
        };
        store(0, std::uint32_t{0xffffffff});
        store(4, std::uint64_t{8});
        store(20, std::uint32_t{4});
        Check(EhFrame::FramesSize(contains, base, "test") == 28, "extended frame length rejected or counted incorrectly");
        limit = 10;
        Check(Throws([&] { EhFrame::FramesSize(contains, base, "test"); }), "truncated extended length accepted");
        limit = 19;
        Check(Throws([&] { EhFrame::FramesSize(contains, base, "test"); }), "truncated extended body accepted");
        limit = 28;
        Check(Throws([&] { EhFrame::FramesSize(contains, base, "test"); }), "missing terminator accepted");
        limit = image.size();
        store(4, std::numeric_limits<std::uint64_t>::max());
        Check(Throws([&] { EhFrame::FramesSize(contains, base, "test"); }), "overflowing extended length accepted");
        store(0, std::uint32_t{0xfffffffc});
        Check(Throws([&] { EhFrame::FramesSize(contains, base, "test"); }), "oversized ordinary length accepted");
        image.fill(0);
        store(0, std::uint32_t{4});
        store(8, std::uint32_t{8});
        Check(EhFrame::FramesSize(contains, base, "test") == 20, "ordinary record sequence size");
        store(0, std::uint32_t{0});
        Check(EhFrame::FramesSize(contains, base, "test") == 0, "empty frame sequence size");
        image.fill(0);
        image[0] = 1;
        image[1] = 0x1b;
        image[2] = 0x03;
        image[3] = 0x3b;
        store(4, std::int32_t{60});
        store(8, std::uint32_t{2});
        store(64, std::uint32_t{4});
        const auto tables = EhFrame::ReadTables(contains, base, "test");
        Check(tables.header == base && tables.headerSize == 28 && tables.frames == base + 64 && tables.framesSize == 8,
              "encoded frame header layout");
        store(8, std::numeric_limits<std::uint32_t>::max());
        Check(Throws([&] { EhFrame::ReadTables(contains, base, "test"); }), "oversized header table accepted");
        store(8, std::uint32_t{2});
        image[1] = 0x9b;
        Check(Throws([&] { EhFrame::ReadTables(contains, base, "test"); }), "unsupported indirect pointer accepted");
        image[1] = 0x1b;
        image[0] = 2;
        Check(Throws([&] { EhFrame::ReadTables(contains, base, "test"); }), "unsupported header version accepted");
        image[0] = 1;
        limit = 3;
        Check(Throws([&] { EhFrame::ReadTables(contains, base, "test"); }), "truncated header accepted");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
