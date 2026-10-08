#include "Ngs2Test.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

static constexpr Ngs2Handle Sentinel = 0x1234;

enum class Storage { Valid, Missing, Small, Misaligned };
enum class FreeFailure { None, Return, Throw };

struct ParentChange {
    Ngs2Handle handle = 0;
    bool replace = false;
    bool changed = false;
};

static void ChangeParent(ParentChange& change) {
    if (change.handle == 0 || change.changed) return;
    Ngs2ContextBufferInfo buffer{};
    Require(sceNgs2SystemDestroy(change.handle, &buffer) == SCE_NGS2_OK);
    change.changed = true;
    if (!change.replace) return;
    Ngs2Handle replacement = 0;
    Require(sceNgs2SystemCreate(nullptr, &buffer, &replacement) == SCE_NGS2_OK);
    Require(replacement == change.handle);
}

struct AllocatorState {
    Storage storage = Storage::Valid;
    FreeFailure freeFailure = FreeFailure::None;
    bool failAllocation = false;
    void* raw = nullptr;
    Ngs2ContextBufferInfo returned{};
    unsigned allocations = 0;
    unsigned frees = 0;
    Ngs2BufferAllocator* mutableAllocator = nullptr;
    ParentChange parent;
    const std::vector<unsigned>* cleanups = nullptr;
    std::size_t expectedCleanups = 0;

    ~AllocatorState() { std::free(raw); }
};

static int APS5_VABI Allocate_nid_no_patch(Ngs2ContextBufferInfo* info) {
    auto& state = *reinterpret_cast<AllocatorState*>(info->user_data);
    ++state.allocations;
    if (state.failAllocation) return -321;
    Require(state.raw == nullptr && info->host_buffer_size != 0);
    state.raw = std::malloc(info->host_buffer_size + 16);
    Require(state.raw != nullptr);
    info->host_buffer = state.raw;
    for (std::size_t i = 0; i < 5; ++i) info->reserved[i] = 0xface + i;
    if (state.storage == Storage::Missing) info->host_buffer = nullptr;
    if (state.storage == Storage::Small) info->host_buffer_size = 1;
    if (state.storage == Storage::Misaligned) info->host_buffer = static_cast<char*>(state.raw) + 1;
    state.returned = *info;
    if (state.mutableAllocator != nullptr) *state.mutableAllocator = {};
    ChangeParent(state.parent);
    return SCE_NGS2_OK;
}

static int APS5_VABI Free_nid_no_patch(Ngs2ContextBufferInfo* info) {
    auto& state = *reinterpret_cast<AllocatorState*>(info->user_data);
    Require(state.frees == 0 && state.raw != nullptr);
    Require(info->host_buffer == state.returned.host_buffer && info->host_buffer_size == state.returned.host_buffer_size);
    for (std::size_t i = 0; i < 5; ++i) Require(info->reserved[i] == state.returned.reserved[i]);
    if (state.cleanups != nullptr) Require(state.cleanups->size() == state.expectedCleanups);
    ++state.frees;
    std::free(std::exchange(state.raw, nullptr));
    if (state.freeFailure == FreeFailure::Throw) throw std::logic_error("free failed");
    return state.freeFailure == FreeFailure::Return ? -456 : SCE_NGS2_OK;
}

static Ngs2BufferAllocator Allocator(AllocatorState& state) {
    return {Allocate_nid_no_patch, Free_nid_no_patch, reinterpret_cast<std::uintptr_t>(&state)};
}

template <typename TException, typename TAction>
static std::string Throws(TAction action) {
    try {
        action();
    } catch (const TException& error) {
        return error.what();
    }
    Require(false);
    return {};
}

static std::uint32_t RackCount(Ngs2Handle system) {
    Ngs2SystemInfo info{};
    Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_OK);
    return info.rack_count;
}

static void TestInvalidStorage() {
    const auto system = CreateSystem();
    CreateRack(system, SCE_NGS2_RACK_ID_MASTERING);
    for (const auto storage : {Storage::Missing, Storage::Small, Storage::Misaligned}) {
        for (const auto failure : {FreeFailure::None, FreeFailure::Return, FreeFailure::Throw}) {
            for (const bool rack : {false, true}) {
                AllocatorState state;
                state.storage = storage;
                state.freeFailure = failure;
                auto allocator = Allocator(state);
                state.mutableAllocator = &allocator;
                Ngs2Handle handle = Sentinel;
                const auto error = Throws<std::invalid_argument>([&] {
                    if (rack) sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_MASTERING, nullptr, &allocator, &handle);
                    else sceNgs2SystemCreateWithAllocator(nullptr, &allocator, &handle);
                });
                Require(error.starts_with("NGS2: the context buffer"));
                Require(state.allocations == 1 && state.frees == 1 && handle == Sentinel);
                Require(RackCount(system) == 1);
            }
        }
    }
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestFailedAllocation() {
    const auto system = CreateSystem();
    for (const bool rack : {false, true}) {
        AllocatorState state;
        state.failAllocation = true;
        const auto allocator = Allocator(state);
        Ngs2Handle handle = Sentinel;
        const auto error = Throws<std::runtime_error>([&] {
            if (rack) sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_MASTERING, nullptr, &allocator, &handle);
            else sceNgs2SystemCreateWithAllocator(nullptr, &allocator, &handle);
        });
        Require(error == "NGS2: the allocator handler failed with -321");
        Require(state.allocations == 1 && state.frees == 0 && handle == Sentinel);
        Require(RackCount(system) == 0);
    }
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

struct EffectCalls {
    std::vector<unsigned> setups;
    std::vector<unsigned> cleanups;
    int failSetup = -1;
    bool throwSetup = false;
    bool failCleanup = false;
    ParentChange parent;
};

struct ModuleState {
    EffectCalls* calls;
    unsigned index;
    bool hasSetup = true;
};

static int APS5_VABI Setup_nid_no_patch(Ngs2UserFx2SetupContext* context) {
    auto& module = *reinterpret_cast<ModuleState*>(context->user_data);
    auto& calls = *module.calls;
    const unsigned id = module.index * 10 + context->voice_index;
    calls.setups.push_back(id);
    Require(context->common != nullptr && context->param != nullptr && context->work != nullptr);
    if (static_cast<int>(id) == calls.failSetup) {
        if (calls.throwSetup) throw std::logic_error("setup failed");
        return -123;
    }
    *static_cast<unsigned char*>(context->work) = static_cast<unsigned char>(id + 1);
    ChangeParent(calls.parent);
    return SCE_NGS2_OK;
}

static int APS5_VABI Cleanup_nid_no_patch(Ngs2UserFx2CleanupContext* context) {
    auto& module = *reinterpret_cast<ModuleState*>(context->user_data);
    auto& calls = *module.calls;
    const unsigned id = module.index * 10 + context->voice_index;
    Require(context->common != nullptr && context->param != nullptr && context->work != nullptr);
    Require(*static_cast<unsigned char*>(context->work) == (module.hasSetup ? id + 1 : 0));
    calls.cleanups.push_back(id);
    if (calls.failCleanup && id == 10) throw std::logic_error("cleanup failed");
    return calls.failCleanup ? -456 : SCE_NGS2_OK;
}

static int APS5_VABI Process_nid_no_patch(Ngs2UserFx2ProcessContext*) { return SCE_NGS2_OK; }

struct CustomOptions {
    std::array<ModuleState, 2> states;
    std::array<Ngs2CustomUserFx2ModuleOption, 2> modules{};
    Ngs2CustomSubmixerRackOption options{};

    explicit CustomOptions(EffectCalls& calls, bool firstSetup = true) : states{{{&calls, 0, firstSetup}, {&calls, 1}}} {
        auto& custom = options.custom_rack_option;
        custom.rack_option.size = sizeof(options);
        custom.rack_option.max_grain_samples = 256;
        custom.rack_option.max_voices = 3;
        custom.rack_option.max_ports = 1;
        custom.rack_option.max_matrices = 1;
        custom.num_buffers = 1;
        custom.num_modules = modules.size();
        for (std::size_t i = 0; i < modules.size(); ++i) {
            modules[i] = {{sizeof(Ngs2CustomUserFx2ModuleOption)}, states[i].hasSetup ? Setup_nid_no_patch : nullptr,
                          Cleanup_nid_no_patch, nullptr, Process_nid_no_patch, 16, 16, 16, reinterpret_cast<std::uintptr_t>(&states[i])};
            custom.module[i].option = &modules[i].custom_module_option;
            custom.module[i].module_id = SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2;
            custom.module[i].state_size = 16;
        }
        options.max_channels = 2;
        options.max_inputs = 1;
    }

    const Ngs2RackOption* Get() const { return &options.custom_rack_option.rack_option; }
};

static void TestCustomFailure(bool callerOwned, bool throwSetup, bool firstSetup) {
    const auto system = CreateSystem();
    CreateRack(system, SCE_NGS2_RACK_ID_MASTERING);
    EffectCalls calls;
    calls.failSetup = 11;
    calls.throwSetup = throwSetup;
    calls.failCleanup = true;
    const CustomOptions options(calls, firstSetup);
    AllocatorState state;
    state.cleanups = &calls.cleanups;
    state.expectedCleanups = 4;
    state.freeFailure = FreeFailure::Throw;
    const auto allocator = Allocator(state);
    Ngs2ContextBufferInfo buffer{};
    if (callerOwned) {
        Ngs2ContextBufferInfo query{};
        Require(sceNgs2RackQueryBufferSize(SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &query) == SCE_NGS2_OK);
        buffer = Buffer(query);
    }
    Ngs2Handle handle = Sentinel;
    const auto create = [&] {
        if (callerOwned) sceNgs2RackCreate(system, SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &buffer, &handle);
        else sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &allocator, &handle);
    };
    if (throwSetup) Require(Throws<std::logic_error>(create) == "setup failed");
    else Require(Throws<std::runtime_error>(create) == "NGS2: the UserFx2 setup handler failed with -123");
    Require(calls.cleanups == (std::vector<unsigned>{10, 2, 1, 0}));
    Require(handle == Sentinel && RackCount(system) == 1);
    Require(state.allocations == (callerOwned ? 0 : 1) && state.frees == (callerOwned ? 0 : 1));
    if (callerOwned) {
        calls.failSetup = -1;
        calls.failCleanup = false;
        calls.cleanups.clear();
        Require(sceNgs2RackCreate(system, SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &buffer, &handle) == SCE_NGS2_OK);
        Ngs2ContextBufferInfo released{};
        Require(sceNgs2RackDestroy(handle, &released) == SCE_NGS2_OK);
        Require(released.host_buffer == buffer.host_buffer && released.host_buffer_size == buffer.host_buffer_size);
        Require(calls.cleanups == (std::vector<unsigned>{0, 10, 1, 11, 2, 12}));
    }
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
}

static void TestParentChanges(bool duringSetup, bool replace) {
    const auto system = CreateSystem();
    AllocatorState state;
    EffectCalls calls;
    const CustomOptions options(calls);
    if (duringSetup) {
        calls.parent = {system, replace};
        state.cleanups = &calls.cleanups;
        state.expectedCleanups = 6;
    } else {
        state.parent = {system, replace};
    }
    const auto allocator = Allocator(state);
    Ngs2Handle handle = Sentinel;
    const int result = duringSetup
        ? sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &allocator, &handle)
        : sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_MASTERING, nullptr, &allocator, &handle);
    Require(result == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE && handle == Sentinel);
    Require(state.allocations == 1 && state.frees == 1);
    if (duringSetup) Require(calls.cleanups == (std::vector<unsigned>{12, 11, 10, 2, 1, 0}));
    if (replace) {
        Require(RackCount(system) == 0);
        Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_OK);
    } else {
        Ngs2SystemInfo info{};
        Require(sceNgs2SystemGetInfo(system, &info, sizeof(info)) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    }
}

static void TestSuccessfulOwnership() {
    AllocatorState systemState;
    auto systemAllocator = Allocator(systemState);
    systemState.mutableAllocator = &systemAllocator;
    Ngs2Handle system = Sentinel;
    Require(sceNgs2SystemCreateWithAllocator(nullptr, &systemAllocator, &system) == SCE_NGS2_OK);
    Require(systemState.allocations == 1 && systemState.frees == 0 && system != Sentinel);
    AllocatorState rackState;
    auto rackAllocator = Allocator(rackState);
    rackState.mutableAllocator = &rackAllocator;
    EffectCalls calls;
    const CustomOptions options(calls);
    Ngs2Handle rack = Sentinel;
    Require(sceNgs2RackCreateWithAllocator(system, SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER, options.Get(), &rackAllocator, &rack) == SCE_NGS2_OK);
    Require(rackState.allocations == 1 && rackState.frees == 0 && rack != Sentinel && RackCount(system) == 1);
    rackState.cleanups = &calls.cleanups;
    rackState.expectedCleanups = 6;
    Ngs2ContextBufferInfo released{};
    Require(sceNgs2SystemDestroy(system, &released) == SCE_NGS2_OK);
    Require(systemState.frees == 1 && rackState.frees == 1 && released.host_buffer == nullptr && released.host_buffer_size == 0);
    Require(calls.cleanups == (std::vector<unsigned>{0, 10, 1, 11, 2, 12}));
    Require(sceNgs2RackDestroy(rack, nullptr) == SCE_NGS2_ERROR_INVALID_RACK_HANDLE);
    Require(sceNgs2SystemDestroy(system, nullptr) == SCE_NGS2_ERROR_INVALID_SYSTEM_HANDLE);
    Require(systemState.frees == 1 && rackState.frees == 1);
}

int main() {
    TestInvalidStorage();
    TestFailedAllocation();
    for (const bool callerOwned : {false, true}) {
        for (const bool throwSetup : {false, true}) {
            TestCustomFailure(callerOwned, throwSetup, true);
        }
    }
    TestCustomFailure(false, false, false);
    for (const bool duringSetup : {false, true}) {
        for (const bool replace : {false, true}) TestParentChanges(duringSetup, replace);
    }
    TestSuccessfulOwnership();
    return 0;
}
