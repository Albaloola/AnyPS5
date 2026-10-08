#include "UsbdTest.hpp"

#include <limits>

int main() {
    const int result = sceUsbdInit();
    if (result != 0) {
        std::fprintf(stderr, "USBD host backend initialization failed: 0x%x\n", static_cast<unsigned>(result));
        return 1;
    }
    libusb_device** list = nullptr;
    const auto count = sceUsbdGetDeviceList(&list);
    Require(count >= 0 && list != nullptr && list[count] == nullptr);
    sceUsbdFreeDeviceList(list, 1);
    Require(sceUsbdGetDeviceList(nullptr) == static_cast<int>(0x80240002));
    Require(sceUsbdHandleEventsTimeout(nullptr) == static_cast<int>(0x80240002));
    const UsbdTimeval invalid{0, 1000000};
    Require(sceUsbdHandleEventsTimeout(&invalid) == static_cast<int>(0x80240002));
    const UsbdTimeval timeout{0, 0};
    Require(sceUsbdHandleEventsTimeout(&timeout) == 0);
    auto* transfer = sceUsbdAllocTransfer(0);
    Require(transfer != nullptr);
    sceUsbdFreeTransfer(transfer);
    Require(sceUsbdAllocTransfer(-1) == nullptr);
    Require(sceUsbdOpen(nullptr, nullptr) == static_cast<int>(0x80240002));
    sceUsbdExit();
    return 0;
}
