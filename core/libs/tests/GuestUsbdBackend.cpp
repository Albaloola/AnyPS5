#include "UsbdTest.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

int contextStorage;
int deviceStorage;
int handleStorage;
auto* context = reinterpret_cast<libusb_context*>(&contextStorage);
auto* device = reinterpret_cast<libusb_device*>(&deviceStorage);
auto* handle = reinterpret_cast<libusb_device_handle*>(&handleStorage);

struct Mock {
    int result = 0;
    int init = 0;
    int exit = 0;
    int open = 0;
    int close = 0;
    int refs = 0;
    int listFrees = 0;
    int configFrees = 0;
    int allocations = 0;
    int frees = 0;
    int configurationQueries = 0;
    int interfaceNumber = -1;
    int configuration = -1;
    int descriptorIndex = -1;
    int requestType = 0;
    int request = 0;
    int value = 0;
    int index = 0;
    int length = 0;
    unsigned timeout = 0;
    std::int64_t eventSeconds = 0;
    std::int64_t eventMicroseconds = 0;
    libusb_transfer* pending = nullptr;
    bool cancelled = false;
} mock;

template <typename TException, typename TAction>
void RequireThrows(TAction action) {
    try { action(); }
    catch (const TException&) { return; }
    Require(false);
}

}

extern "C" {

int LIBUSB_CALL libusb_init(libusb_context** result) {
    ++mock.init;
    if (mock.result < 0) return mock.result;
    *result = context;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context* value) { Require(value == context); ++mock.exit; }

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context* value, libusb_device*** result) {
    Require(value == context);
    if (mock.result < 0) return mock.result;
    auto** list = static_cast<libusb_device**>(std::calloc(2, sizeof(libusb_device*)));
    list[0] = device;
    *result = list;
    return 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list, int unrefDevices) {
    Require(list[0] == device && list[1] == nullptr && unrefDevices == 1);
    ++mock.listFrees;
    std::free(list);
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context* value, timeval* timeout) {
    Require(value == context);
    mock.eventSeconds = timeout->tv_sec;
    mock.eventMicroseconds = timeout->tv_usec;
    if (mock.pending != nullptr) {
        auto* transfer = mock.pending;
        mock.pending = nullptr;
        Require((transfer->flags & (LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER)) == 0);
        transfer->status = mock.cancelled ? LIBUSB_TRANSFER_CANCELLED : LIBUSB_TRANSFER_COMPLETED;
        transfer->actual_length = mock.cancelled ? 0 : transfer->length;
        mock.cancelled = false;
        transfer->callback(transfer);
    }
    return mock.result;
}

int LIBUSB_CALL libusb_event_handling_ok(libusb_context* value) { Require(value == context); return 1; }

libusb_transfer* LIBUSB_CALL libusb_alloc_transfer(int packets) {
    Require(packets >= 0);
    auto* transfer = static_cast<libusb_transfer*>(std::calloc(1, sizeof(libusb_transfer) + sizeof(libusb_iso_packet_descriptor) * packets));
    transfer->num_iso_packets = packets;
    ++mock.allocations;
    return transfer;
}

void LIBUSB_CALL libusb_free_transfer(libusb_transfer* transfer) {
    Require((transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER) == 0);
    ++mock.frees;
    std::free(transfer);
}

int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* transfer) {
    if (mock.result < 0) return mock.result;
    Require(mock.pending == nullptr && transfer->dev_handle == handle && transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT);
    mock.pending = transfer;
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* transfer) {
    Require(mock.pending == transfer);
    if (mock.result < 0) return mock.result;
    mock.cancelled = true;
    return 0;
}

int LIBUSB_CALL libusb_open(libusb_device* value, libusb_device_handle** result) {
    Require(value == device);
    ++mock.open;
    if (mock.result < 0) return mock.result;
    *result = handle;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* value) { Require(value == handle); ++mock.close; }
libusb_device* LIBUSB_CALL libusb_ref_device(libusb_device* value) { Require(value == device); ++mock.refs; return device; }
void LIBUSB_CALL libusb_unref_device(libusb_device* value) { Require(value == device); --mock.refs; }
std::uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device* value) { Require(value == device); return 7; }
std::uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device* value) { Require(value == device); return 12; }

int LIBUSB_CALL libusb_get_configuration(libusb_device_handle* value, int* configuration) {
    Require(value == handle);
    ++mock.configurationQueries;
    *configuration = 2;
    return mock.result;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle* value, int number) { Require(value == handle); mock.interfaceNumber = number; return mock.result; }
int LIBUSB_CALL libusb_release_interface(libusb_device_handle* value, int number) { Require(value == handle); mock.interfaceNumber = number; return mock.result; }
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle* value, int number) { Require(value == handle); mock.interfaceNumber = number; return mock.result; }
int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle* value, int number) { Require(value == handle); mock.interfaceNumber = number; return mock.result; }
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle* value, int configuration) { Require(value == handle); mock.configuration = configuration; return mock.result; }
int LIBUSB_CALL libusb_reset_device(libusb_device_handle* value) { Require(value == handle); return mock.result; }

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device* value, libusb_device_descriptor* descriptor) {
    Require(value == device);
    if (mock.result < 0) return mock.result;
    *descriptor = {};
    descriptor->bLength = 18;
    descriptor->idVendor = 0x1234;
    descriptor->idProduct = 0xabcd;
    return 0;
}

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device* value, std::uint8_t index, libusb_config_descriptor** descriptor) {
    Require(value == device);
    mock.descriptorIndex = index;
    if (mock.result < 0) return mock.result;
    *descriptor = new libusb_config_descriptor{};
    (*descriptor)->bConfigurationValue = 4;
    return 0;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device* value, libusb_config_descriptor** descriptor) {
    return libusb_get_config_descriptor(value, 1, descriptor);
}

void LIBUSB_CALL libusb_free_config_descriptor(libusb_config_descriptor* descriptor) { ++mock.configFrees; delete descriptor; }

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle* value, std::uint8_t requestType, std::uint8_t request,
                                       std::uint16_t requestValue, std::uint16_t index, unsigned char* data,
                                       std::uint16_t length, unsigned int timeout) {
    Require(value == handle);
    mock.requestType = requestType;
    mock.request = request;
    mock.value = requestValue;
    mock.index = index;
    mock.length = length;
    mock.timeout = timeout;
    if (mock.result < 0) return mock.result;
    if (length != 0) data[0] = 0xaa;
    return length;
}

}

namespace {

struct CallbackState {
    unsigned calls = 0;
    bool resubmit = false;
    bool freeInCallback = false;
    bool throwInCallback = false;
    bool exitInCallback = false;
    libusb_transfer_status expectedStatus = LIBUSB_TRANSFER_COMPLETED;
};

void APS5_VABI Callback_nid_no_patch(libusb_transfer* transfer) {
    auto& state = *static_cast<CallbackState*>(transfer->user_data);
    Require(transfer->callback == Callback_nid_no_patch && transfer->status == state.expectedStatus);
    Require(transfer->actual_length == (state.expectedStatus == LIBUSB_TRANSFER_CANCELLED ? 0 : transfer->length));
    ++state.calls;
    if (state.resubmit && state.calls == 1) Require(sceUsbdSubmitTransfer(transfer) == 0);
    if (state.freeInCallback) sceUsbdFreeTransfer(transfer);
    if (state.exitInCallback) {
        sceUsbdExit();
        Require(mock.exit == 0);
    }
    if (state.throwInCallback) throw std::logic_error("guest USB callback failed");
}

void TestDeviceAndErrors() {
    mock.result = LIBUSB_ERROR_ACCESS;
    Require(sceUsbdInit() == static_cast<int>(0x80240003));
    mock.result = 0;
    Require(sceUsbdInit() == 0 && sceUsbdInit() == 0 && mock.init == 2);
    libusb_device** list = nullptr;
    Require(sceUsbdGetDeviceList(&list) == 1 && list[0] == device && list[1] == nullptr);
    Require(sceUsbdRefDevice(list[0]) == device && mock.refs == 1);
    sceUsbdFreeDeviceList(list, 1);
    Require(mock.listFrees == 1);
    Require(sceUsbdGetBusNumber(device) == 7 && sceUsbdGetDeviceAddress(device) == 12);
    libusb_device_descriptor descriptor{};
    Require(sceUsbdGetDeviceDescriptor(device, &descriptor) == 0 && descriptor.idVendor == 0x1234 && descriptor.idProduct == 0xabcd);
    libusb_config_descriptor* configuration = nullptr;
    Require(sceUsbdGetConfigDescriptor(device, 3, &configuration) == 0 && configuration->bConfigurationValue == 4 && mock.descriptorIndex == 3);
    sceUsbdFreeConfigDescriptor(configuration);
    Require(sceUsbdGetActiveConfigDescriptor(device, &configuration) == 0 && mock.descriptorIndex == 1);
    sceUsbdFreeConfigDescriptor(configuration);
    Require(mock.configFrees == 2);
    libusb_device_handle* opened = nullptr;
    Require(sceUsbdOpen(device, &opened) == 0 && opened == handle && mock.open == 1);
    Require(sceUsbdCheckConnected(handle) == 0 && mock.configurationQueries == 1);
    Require(sceUsbdClaimInterface(handle, 3) == 0 && mock.interfaceNumber == 3);
    Require(sceUsbdReleaseInterface(handle, 4) == 0 && mock.interfaceNumber == 4);
    mock.result = 1;
    Require(sceUsbdKernelDriverActive(handle, 5) == 1 && mock.interfaceNumber == 5);
    mock.result = 0;
    Require(sceUsbdAttachKernelDriver(handle, 6) == 0 && mock.interfaceNumber == 6);
    Require(sceUsbdSetConfiguration(handle, -1) == 0 && mock.configuration == -1);
    for (int error = -1; error >= -12; --error) {
        mock.result = error;
        Require(sceUsbdResetDevice(handle) == static_cast<int>(0x80240000u + static_cast<unsigned>(-error)));
    }
    mock.result = LIBUSB_ERROR_OTHER;
    Require(sceUsbdCheckConnected(handle) == static_cast<int>(0x802400ff));
    mock.result = 0;
    Require(sceUsbdResetDevice(nullptr) == static_cast<int>(0x80240002));
    Require(sceUsbdOpen(nullptr, &opened) == static_cast<int>(0x80240002));
    Require(sceUsbdClaimInterface(handle, -1) == static_cast<int>(0x80240002));
    RequireThrows<std::invalid_argument>([] { sceUsbdGetBusNumber(nullptr); });
    sceUsbdClose(handle);
    sceUsbdUnrefDevice(device);
    Require(mock.close == 1 && mock.refs == 0);
}

void TestControl() {
    std::array<std::uint8_t, 8> data{};
    Require(sceUsbdControlTransfer(handle, 0x81, 7, 0x1234, 0x5678, data.data(), data.size(), 29) == 8);
    Require(data[0] == 0xaa && mock.requestType == 0x81 && mock.request == 7 && mock.value == 0x1234 && mock.index == 0x5678 && mock.timeout == 29);
    Require(sceUsbdGetStringDescriptor(handle, 2, 0x409, data.data(), data.size()) == 8);
    Require(mock.requestType == 0x80 && mock.request == LIBUSB_REQUEST_GET_DESCRIPTOR && mock.value == 0x0302 && mock.index == 0x409 && mock.timeout == 1000);
    Require(sceUsbdControlTransfer(handle, 0, 0, 0, 0, nullptr, 0, 0) == 0);
    Require(sceUsbdControlTransfer(handle, 0, 0, 0, 0, data.data(), 65536, 0) == static_cast<int>(0x80240002));
    Require(sceUsbdGetStringDescriptor(handle, 0, 0, data.data(), -1) == static_cast<int>(0x80240002));
    mock.result = LIBUSB_ERROR_TIMEOUT;
    Require(sceUsbdControlTransfer(handle, 0, 0, 0, 0, data.data(), 1, 0) == static_cast<int>(0x80240007));
    mock.result = 0;
}

void TestTransfers() {
    CallbackState callback;
    std::array<std::uint8_t, 8> data{};
    auto* transfer = sceUsbdAllocTransfer(2);
    Require(transfer != nullptr && transfer->num_iso_packets == 2);
    sceUsbdFillInterruptTransfer(transfer, handle, 0x83, data.data(), data.size(), Callback_nid_no_patch, &callback, 41);
    Require(transfer->dev_handle == handle && transfer->endpoint == 0x83 && transfer->timeout == 41 && transfer->buffer == data.data());
    Require(transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT && transfer->callback == Callback_nid_no_patch && transfer->user_data == &callback);
    mock.result = LIBUSB_ERROR_NO_DEVICE;
    Require(sceUsbdSubmitTransfer(transfer) == static_cast<int>(0x80240004));
    Require(transfer->callback == Callback_nid_no_patch && transfer->user_data == &callback);
    mock.result = 0;
    callback.resubmit = true;
    Require(sceUsbdSubmitTransfer(transfer) == 0);
    Require(sceUsbdSubmitTransfer(transfer) == static_cast<int>(0x80240006));
    RequireThrows<std::runtime_error>([&] { sceUsbdFreeTransfer(transfer); });
    const UsbdTimeval timeout{17, 900001};
    Require(sceUsbdHandleEventsTimeout(&timeout) == 0 && callback.calls == 1 && mock.pending == transfer);
    Require(mock.eventSeconds == 17 && mock.eventMicroseconds == 900001);
    callback.expectedStatus = LIBUSB_TRANSFER_CANCELLED;
    Require(sceUsbdCancelTransfer(transfer) == 0);
    Require(sceUsbdHandleEventsTimeout(&timeout) == 0 && callback.calls == 2 && mock.pending == nullptr);
    Require(sceUsbdCancelTransfer(transfer) == static_cast<int>(0x80240005));
    sceUsbdFreeTransfer(transfer);
    Require(mock.frees == 1);
    for (const bool automatic : {false, true}) {
        callback = {};
        callback.freeInCallback = !automatic;
        callback.throwInCallback = automatic;
        transfer = sceUsbdAllocTransfer(0);
        auto* guestBuffer = static_cast<unsigned char*>(ApplicationHeapAllocate_nid_no_patch(8));
        Require(guestBuffer != nullptr);
        sceUsbdFillInterruptTransfer(transfer, handle, 0x81, guestBuffer, 8, Callback_nid_no_patch, &callback, 0);
        transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | (automatic ? LIBUSB_TRANSFER_FREE_TRANSFER : 0);
        Require(sceUsbdSubmitTransfer(transfer) == 0);
        if (automatic) RequireThrows<std::logic_error>([&] { sceUsbdHandleEventsTimeout(&timeout); });
        else Require(sceUsbdHandleEventsTimeout(&timeout) == 0);
        Require(callback.calls == 1 && mock.pending == nullptr);
    }
    Require(mock.allocations == 3 && mock.frees == 3);
    for (const bool resubmit : {false, true}) {
        callback = {};
        callback.resubmit = resubmit;
        callback.freeInCallback = !resubmit;
        transfer = sceUsbdAllocTransfer(0);
        sceUsbdFillInterruptTransfer(transfer, handle, 0x81, data.data(), data.size(), Callback_nid_no_patch, &callback, 0);
        transfer->flags = LIBUSB_TRANSFER_FREE_TRANSFER;
        const int frees = mock.frees;
        Require(sceUsbdSubmitTransfer(transfer) == 0);
        Require(sceUsbdHandleEventsTimeout(&timeout) == 0 && callback.calls == 1);
        if (resubmit) {
            Require(mock.frees == frees && mock.pending == transfer);
            Require(sceUsbdHandleEventsTimeout(&timeout) == 0 && callback.calls == 2);
        }
        Require(mock.frees == frees + 1 && mock.pending == nullptr);
    }
    Require(sceUsbdAllocTransfer(-1) == nullptr && sceUsbdSubmitTransfer(nullptr) == static_cast<int>(0x80240002));
    Require(sceUsbdCancelTransfer(nullptr) == static_cast<int>(0x80240002));
    const UsbdTimeval invalid{-1, 0};
    Require(sceUsbdHandleEventsTimeout(&invalid) == static_cast<int>(0x80240002));
    if constexpr (sizeof(decltype(timeval::tv_sec)) < sizeof(std::int64_t)) {
        const UsbdTimeval overflow{std::numeric_limits<std::int64_t>::max(), 0};
        Require(sceUsbdHandleEventsTimeout(&overflow) == static_cast<int>(0x80240002));
    }
    Require(sceUsbdEventHandlingOk() == 1);
}

void TestExitDuringCallback() {
    CallbackState callback;
    callback.exitInCallback = true;
    auto* transfer = sceUsbdAllocTransfer(0);
    sceUsbdFillInterruptTransfer(transfer, handle, 0x81, nullptr, 0, Callback_nid_no_patch, &callback, 0);
    transfer->flags = LIBUSB_TRANSFER_FREE_TRANSFER;
    Require(sceUsbdSubmitTransfer(transfer) == 0);
    const UsbdTimeval timeout{};
    Require(sceUsbdHandleEventsTimeout(&timeout) == 0 && callback.calls == 1);
    Require(mock.exit == 1 && mock.allocations == mock.frees);
}

}

int main() {
    std::array<void*, 10> heap{};
    ApplicationHeapRegister_nid_no_patch(heap.data());
    TestDeviceAndErrors();
    TestControl();
    TestTransfers();
    TestExitDuringCallback();
    sceUsbdExit();
    Require(mock.exit == 1);
    sceUsbdExit();
    Require(mock.exit == 1);
    return 0;
}
