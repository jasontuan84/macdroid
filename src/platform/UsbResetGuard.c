// Works around a crash inside libmtp + libusb on macOS.
//
// When LIBMTP_Open_Raw_Device_Uncached() cannot open a PTP session (phone
// busy, held by Image Capture, stale session...), libmtp calls
// libusb_reset_device() and then close_usb(), which sends control transfers
// on the same handle. On macOS the reset can re-enumerate the device, which
// leaves the handle without a backing IOKit device, so libusb dereferences a
// NULL pointer in darwin_submit_transfer() and the whole app crashes.
//
// This library interposes libusb_reset_device() and turns it into a no-op.
// libmtp then closes and reopens the interface without a reset and either
// opens the session or fails cleanly, so MacDroid can show its normal
// "phone is busy" / "Release Phone" screen instead of crashing.
//
// dyld only applies __interpose entries from libraries loaded at launch, not
// from the main executable, so this must stay a separate dylib linked by
// MacDroid.

#include <libusb.h>
#include <stdio.h>

static int macdroid_libusb_reset_device(libusb_device_handle *handle)
{
    (void)handle;
    fprintf(stderr, "MacDroid: skipped libusb_reset_device() (avoids a libusb crash after re-enumeration)\n");
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

__attribute__((used)) static const struct {
    const void *replacement;
    const void *original;
} macdroid_interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)macdroid_libusb_reset_device, (const void *)libusb_reset_device },
};

// Referenced by the app so the linker always keeps this library loaded.
int macdroid_usb_guard_installed(void)
{
    return 1;
}
