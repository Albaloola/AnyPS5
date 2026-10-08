#ifndef CORE_LIBS_TESTS_USBDTEST_HPP
#define CORE_LIBS_TESTS_USBDTEST_HPP

#include "prx/libc/include/general/VabiMacros.hpp"
#include <libusb.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct UsbdTimeval {
    std::int64_t seconds;
    std::int64_t microseconds;
};

extern "C" {
std::int32_t APS5_VABI sceUsbdInit();
void APS5_VABI sceUsbdExit();
std::int64_t APS5_VABI sceUsbdGetDeviceList(libusb_device***);
void APS5_VABI sceUsbdFreeDeviceList(libusb_device**, std::int32_t);
std::int32_t APS5_VABI sceUsbdHandleEventsTimeout(const UsbdTimeval*);
std::int32_t APS5_VABI sceUsbdEventHandlingOk();
libusb_transfer* APS5_VABI sceUsbdAllocTransfer(std::int32_t);
void APS5_VABI sceUsbdFreeTransfer(libusb_transfer*);
void APS5_VABI sceUsbdFillInterruptTransfer(libusb_transfer*, libusb_device_handle*, std::uint8_t, std::uint8_t*, std::int32_t, libusb_transfer_cb_fn, void*, std::uint32_t);
std::int32_t APS5_VABI sceUsbdSubmitTransfer(libusb_transfer*);
std::int32_t APS5_VABI sceUsbdCancelTransfer(libusb_transfer*);
std::int32_t APS5_VABI sceUsbdOpen(libusb_device*, libusb_device_handle**);
void APS5_VABI sceUsbdClose(libusb_device_handle*);
libusb_device* APS5_VABI sceUsbdRefDevice(libusb_device*);
void APS5_VABI sceUsbdUnrefDevice(libusb_device*);
std::uint8_t APS5_VABI sceUsbdGetBusNumber(libusb_device*);
std::uint8_t APS5_VABI sceUsbdGetDeviceAddress(libusb_device*);
std::int32_t APS5_VABI sceUsbdCheckConnected(libusb_device_handle*);
std::int32_t APS5_VABI sceUsbdClaimInterface(libusb_device_handle*, std::int32_t);
std::int32_t APS5_VABI sceUsbdReleaseInterface(libusb_device_handle*, std::int32_t);
std::int32_t APS5_VABI sceUsbdKernelDriverActive(libusb_device_handle*, std::int32_t);
std::int32_t APS5_VABI sceUsbdAttachKernelDriver(libusb_device_handle*, std::int32_t);
std::int32_t APS5_VABI sceUsbdSetConfiguration(libusb_device_handle*, std::int32_t);
std::int32_t APS5_VABI sceUsbdResetDevice(libusb_device_handle*);
std::int32_t APS5_VABI sceUsbdGetDeviceDescriptor(libusb_device*, libusb_device_descriptor*);
std::int32_t APS5_VABI sceUsbdGetConfigDescriptor(libusb_device*, std::uint8_t, libusb_config_descriptor**);
std::int32_t APS5_VABI sceUsbdGetActiveConfigDescriptor(libusb_device*, libusb_config_descriptor**);
void APS5_VABI sceUsbdFreeConfigDescriptor(libusb_config_descriptor*);
std::int32_t APS5_VABI sceUsbdControlTransfer(libusb_device_handle*, std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t*, std::int32_t, std::uint32_t);
std::int32_t APS5_VABI sceUsbdGetStringDescriptor(libusb_device_handle*, std::uint8_t, std::uint16_t, std::uint8_t*, std::int32_t);
}

inline void RequireUsbd(bool condition, int line) {
    if (condition) return;
    std::fprintf(stderr, "USBD check failed at line %d\n", line);
    std::abort();
}

#define Require(condition) RequireUsbd((condition), __LINE__)

#endif
