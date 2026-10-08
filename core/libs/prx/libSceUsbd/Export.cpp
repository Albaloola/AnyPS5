#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <libusb.h>

#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::int32_t InvalidArgument = static_cast<std::int32_t>(0x80240002);
constexpr std::uint8_t OwnershipFlags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;

struct UsbdTimeval {
    std::int64_t seconds;
    std::int64_t microseconds;
};

static_assert(sizeof(libusb_device_descriptor) == 18);
static_assert(offsetof(libusb_transfer, timeout) == 12);
static_assert(offsetof(libusb_transfer, callback) == 32);
static_assert(offsetof(libusb_transfer, user_data) == 40);
static_assert(offsetof(libusb_transfer, buffer) == 48);
static_assert(offsetof(libusb_transfer, iso_packet_desc) == 60);

struct Context {
    libusb_context* handle = nullptr;
    ~Context() { if (handle != nullptr) libusb_exit(handle); }
};

struct Transfer {
    libusb_transfer_cb_fn callback = nullptr;
    std::uint8_t flags = 0;
    bool submitted = false;
    std::shared_ptr<Context> context;
};

struct Runtime {
    std::recursive_mutex mutex;
    std::shared_ptr<Context> context;
    std::unordered_map<libusb_transfer*, std::shared_ptr<Transfer>> transfers;
};

Runtime& State() {
    static auto* state = new Runtime();
    return *state;
}

std::shared_ptr<Context> CurrentContext() {
    std::lock_guard lock(State().mutex);
    if (State().context == nullptr) throw std::runtime_error("USBD: the library is not initialized");
    return State().context;
}

std::int32_t Error(int result) {
    if (result == LIBUSB_ERROR_OTHER) return static_cast<std::int32_t>(0x802400ff);
    if (result < 0) return static_cast<std::int32_t>(0x80240000u + static_cast<std::uint32_t>(-result));
    return result;
}

thread_local std::exception_ptr callbackError;

void RememberCallbackError() noexcept {
    if (callbackError == nullptr) callbackError = std::current_exception();
}

void RethrowCallbackError() {
    if (callbackError != nullptr) std::rethrow_exception(std::exchange(callbackError, nullptr));
}

void FreeTransfer(libusb_transfer* transfer) {
    const auto found = State().transfers.find(transfer);
    if (found == State().transfers.end()) throw std::invalid_argument("USBD: unknown transfer");
    if (found->second->submitted) throw std::runtime_error("USBD: a submitted transfer cannot be freed before completion");
    if ((transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER) != 0) {
        ApplicationHeapFree_nid_no_patch(transfer->buffer);
        transfer->buffer = nullptr;
    }
    transfer->flags &= ~OwnershipFlags;
    const auto state = found->second;
    State().transfers.erase(found);
    libusb_free_transfer(transfer);
}

void LIBUSB_CALL CompleteTransfer_nid_no_patch(libusb_transfer* transfer) noexcept {
    std::shared_ptr<Transfer> state;
    std::uint8_t flags = 0;
    try {
        {
            std::lock_guard lock(State().mutex);
            const auto found = State().transfers.find(transfer);
            if (found == State().transfers.end()) throw std::runtime_error("USBD: completion for an unknown transfer");
            state = found->second;
            flags = state->flags;
            state->submitted = false;
            transfer->callback = state->callback;
            transfer->flags = state->flags;
        }
        if (state->callback != nullptr) state->callback(transfer);
    } catch (...) {
        RememberCallbackError();
    }
    if (state == nullptr || (flags & LIBUSB_TRANSFER_FREE_TRANSFER) == 0) return;
    try {
        std::lock_guard lock(State().mutex);
        const auto found = State().transfers.find(transfer);
        if (found != State().transfers.end() && found->second == state && !state->submitted) FreeTransfer(transfer);
    } catch (...) {
        RememberCallbackError();
    }
}

}

extern "C" {

std::int32_t APS5_VABI sceUsbdInit() {
    std::lock_guard lock(State().mutex);
    if (State().context != nullptr) return 0;
    auto context = std::make_shared<Context>();
    const int result = libusb_init(&context->handle);
    if (result < 0) return Error(result);
    State().context = std::move(context);
    return 0;
}

void APS5_VABI sceUsbdExit() {
    std::shared_ptr<Context> context;
    {
        std::lock_guard lock(State().mutex);
        context = std::exchange(State().context, nullptr);
    }
}

std::int64_t APS5_VABI sceUsbdGetDeviceList(libusb_device*** list) {
    if (list == nullptr) return InvalidArgument;
    const auto context = CurrentContext();
    libusb_device** devices = nullptr;
    const auto count = libusb_get_device_list(context->handle, &devices);
    if (count < 0) return Error(static_cast<int>(count));
    *list = devices;
    return count;
}

void APS5_VABI sceUsbdFreeDeviceList(libusb_device** list, std::int32_t unrefDevices) {
    if (list != nullptr) libusb_free_device_list(list, unrefDevices != 0);
}

std::int32_t APS5_VABI sceUsbdHandleEventsTimeout(const UsbdTimeval* timeout) {
    if (timeout == nullptr || timeout->seconds < 0 || timeout->microseconds < 0 || timeout->microseconds >= 1000000) return InvalidArgument;
    timeval host{};
    if (timeout->seconds > std::numeric_limits<decltype(host.tv_sec)>::max()) return InvalidArgument;
    host.tv_sec = static_cast<decltype(host.tv_sec)>(timeout->seconds);
    host.tv_usec = static_cast<decltype(host.tv_usec)>(timeout->microseconds);
    const auto context = CurrentContext();
    const int result = libusb_handle_events_timeout(context->handle, &host);
    RethrowCallbackError();
    return Error(result);
}

std::int32_t APS5_VABI sceUsbdEventHandlingOk() {
    const auto context = CurrentContext();
    return libusb_event_handling_ok(context->handle);
}

libusb_transfer* APS5_VABI sceUsbdAllocTransfer(std::int32_t isoPackets) {
    if (isoPackets < 0) return nullptr;
    std::unique_ptr<libusb_transfer, decltype(&libusb_free_transfer)> transfer(libusb_alloc_transfer(isoPackets), libusb_free_transfer);
    if (transfer == nullptr) return nullptr;
    std::lock_guard lock(State().mutex);
    State().transfers.emplace(transfer.get(), std::make_shared<Transfer>());
    return transfer.release();
}

void APS5_VABI sceUsbdFreeTransfer(libusb_transfer* transfer) {
    if (transfer == nullptr) return;
    std::lock_guard lock(State().mutex);
    FreeTransfer(transfer);
}

void APS5_VABI sceUsbdFillInterruptTransfer(libusb_transfer* transfer, libusb_device_handle* handle, std::uint8_t endpoint,
                                           std::uint8_t* buffer, std::int32_t length, libusb_transfer_cb_fn callback,
                                           void* userData, std::uint32_t timeout) {
    if (transfer == nullptr || length < 0 || (length != 0 && buffer == nullptr)) APS5_INVALID_ARG_EX;
    std::lock_guard lock(State().mutex);
    const auto found = State().transfers.find(transfer);
    if (found == State().transfers.end()) APS5_INVALID_ARG_EX;
    if (found->second->submitted) throw std::runtime_error("USBD: a submitted transfer cannot be refilled before completion");
    libusb_fill_interrupt_transfer(transfer, handle, endpoint, buffer, length, callback, userData, timeout);
}

std::int32_t APS5_VABI sceUsbdSubmitTransfer(libusb_transfer* transfer) {
    if (transfer == nullptr || transfer->dev_handle == nullptr || transfer->length < 0 ||
        (transfer->length != 0 && transfer->buffer == nullptr)) return InvalidArgument;
    const auto context = CurrentContext();
    std::lock_guard lock(State().mutex);
    const auto found = State().transfers.find(transfer);
    if (found == State().transfers.end()) return InvalidArgument;
    const auto state = found->second;
    if (state->submitted) return Error(LIBUSB_ERROR_BUSY);
    state->callback = transfer->callback;
    state->flags = transfer->flags;
    state->context = context;
    state->submitted = true;
    transfer->callback = CompleteTransfer_nid_no_patch;
    transfer->flags &= ~OwnershipFlags;
    const int result = libusb_submit_transfer(transfer);
    if (result < 0) {
        state->submitted = false;
        transfer->callback = state->callback;
        transfer->flags = state->flags;
    }
    return Error(result);
}

std::int32_t APS5_VABI sceUsbdCancelTransfer(libusb_transfer* transfer) {
    if (transfer == nullptr) return InvalidArgument;
    std::lock_guard lock(State().mutex);
    const auto found = State().transfers.find(transfer);
    if (found == State().transfers.end()) return InvalidArgument;
    if (!found->second->submitted) return Error(LIBUSB_ERROR_NOT_FOUND);
    return Error(libusb_cancel_transfer(transfer));
}

std::int32_t APS5_VABI sceUsbdOpen(libusb_device* device, libusb_device_handle** handle) {
    if (device == nullptr || handle == nullptr) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_open(device, handle));
}

void APS5_VABI sceUsbdClose(libusb_device_handle* handle) {
    if (handle != nullptr) libusb_close(handle);
}

libusb_device* APS5_VABI sceUsbdRefDevice(libusb_device* device) {
    return device == nullptr ? nullptr : libusb_ref_device(device);
}

void APS5_VABI sceUsbdUnrefDevice(libusb_device* device) {
    if (device != nullptr) libusb_unref_device(device);
}

std::uint8_t APS5_VABI sceUsbdGetBusNumber(libusb_device* device) {
    if (device == nullptr) APS5_INVALID_ARG_EX;
    return libusb_get_bus_number(device);
}

std::uint8_t APS5_VABI sceUsbdGetDeviceAddress(libusb_device* device) {
    if (device == nullptr) APS5_INVALID_ARG_EX;
    return libusb_get_device_address(device);
}

std::int32_t APS5_VABI sceUsbdCheckConnected(libusb_device_handle* handle) {
    if (handle == nullptr) return InvalidArgument;
    const auto context = CurrentContext();
    int configuration = 0;
    return Error(libusb_get_configuration(handle, &configuration));
}

std::int32_t APS5_VABI sceUsbdClaimInterface(libusb_device_handle* handle, std::int32_t interfaceNumber) {
    if (handle == nullptr || interfaceNumber < 0) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_claim_interface(handle, interfaceNumber));
}

std::int32_t APS5_VABI sceUsbdReleaseInterface(libusb_device_handle* handle, std::int32_t interfaceNumber) {
    if (handle == nullptr || interfaceNumber < 0) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_release_interface(handle, interfaceNumber));
}

std::int32_t APS5_VABI sceUsbdKernelDriverActive(libusb_device_handle* handle, std::int32_t interfaceNumber) {
    if (handle == nullptr || interfaceNumber < 0) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_kernel_driver_active(handle, interfaceNumber));
}

std::int32_t APS5_VABI sceUsbdAttachKernelDriver(libusb_device_handle* handle, std::int32_t interfaceNumber) {
    if (handle == nullptr || interfaceNumber < 0) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_attach_kernel_driver(handle, interfaceNumber));
}

std::int32_t APS5_VABI sceUsbdSetConfiguration(libusb_device_handle* handle, std::int32_t configuration) {
    if (handle == nullptr) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_set_configuration(handle, configuration));
}

std::int32_t APS5_VABI sceUsbdResetDevice(libusb_device_handle* handle) {
    if (handle == nullptr) return InvalidArgument;
    const auto context = CurrentContext();
    return Error(libusb_reset_device(handle));
}

std::int32_t APS5_VABI sceUsbdGetDeviceDescriptor(libusb_device* device, libusb_device_descriptor* descriptor) {
    if (device == nullptr || descriptor == nullptr) return InvalidArgument;
    return Error(libusb_get_device_descriptor(device, descriptor));
}

std::int32_t APS5_VABI sceUsbdGetConfigDescriptor(libusb_device* device, std::uint8_t index, libusb_config_descriptor** descriptor) {
    if (device == nullptr || descriptor == nullptr) return InvalidArgument;
    return Error(libusb_get_config_descriptor(device, index, descriptor));
}

std::int32_t APS5_VABI sceUsbdGetActiveConfigDescriptor(libusb_device* device, libusb_config_descriptor** descriptor) {
    if (device == nullptr || descriptor == nullptr) return InvalidArgument;
    return Error(libusb_get_active_config_descriptor(device, descriptor));
}

void APS5_VABI sceUsbdFreeConfigDescriptor(libusb_config_descriptor* descriptor) {
    if (descriptor != nullptr) libusb_free_config_descriptor(descriptor);
}

std::int32_t APS5_VABI sceUsbdControlTransfer(libusb_device_handle* handle, std::uint8_t requestType, std::uint8_t request,
                                           std::uint16_t value, std::uint16_t index, std::uint8_t* data,
                                           std::int32_t length, std::uint32_t timeout) {
    if (handle == nullptr || length < 0 || length > 65535 || (length != 0 && data == nullptr)) return InvalidArgument;
    const auto context = CurrentContext();
    const int result = libusb_control_transfer(handle, requestType, request, value, index, data, static_cast<std::uint16_t>(length), timeout);
    RethrowCallbackError();
    return Error(result);
}

std::int32_t APS5_VABI sceUsbdGetStringDescriptor(libusb_device_handle* handle, std::uint8_t index, std::uint16_t language,
                                               std::uint8_t* data, std::int32_t length) {
    if (handle == nullptr || data == nullptr || length <= 0 || length > 65535) return InvalidArgument;
    const auto context = CurrentContext();
    const int result = libusb_get_string_descriptor(handle, index, language, data, length);
    RethrowCallbackError();
    return Error(result);
}

}
