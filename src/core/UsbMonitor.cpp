#include "core/UsbMonitor.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <QSet>

namespace {

// USB vendor ids of common Android phone makers.
const QSet<quint16> &androidVendors()
{
    static const QSet<quint16> ids{
        0x18d1, // Google
        0x04e8, // Samsung
        0x2717, // Xiaomi
        0x2a70, // OnePlus
        0x22b8, // Motorola
        0x1004, // LG
        0x12d1, // Huawei
        0x0fce, // Sony
        0x0bb4, // HTC
        0x22d9, // OPPO / realme
        0x2d95, // vivo
        0x2e04, // HMD / Nokia
        0x0b05, // ASUS
        0x17ef, // Lenovo
        0x19d2, // ZTE
        0x1ebf, // Coolpad
        0x2970, // Wiko
        0x1949, // Amazon
        0x29a9, // Nothing
        0x3310, // Fairphone (MediaTek based)
        0x0e8d, // MediaTek
        0x1782, // Spreadtrum / Unisoc
        0x05c6, // Qualcomm
    };
    return ids;
}

quint32 intProperty(io_service_t service, CFStringRef key)
{
    quint32 value = 0;
    if (CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0)) {
        if (CFGetTypeID(ref) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &value);
        CFRelease(ref);
    }
    return value;
}

QString stringProperty(io_service_t service, CFStringRef key)
{
    QString value;
    if (CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0)) {
        if (CFGetTypeID(ref) == CFStringGetTypeID())
            value = QString::fromCFString(static_cast<CFStringRef>(ref));
        CFRelease(ref);
    }
    return value;
}

// Collects bInterfaceClass of every interface below a USB device.
void collectInterfaceClasses(io_registry_entry_t entry, QSet<quint32> &classes, int depth = 0)
{
    if (depth > 4)
        return;
    io_iterator_t it = 0;
    if (IORegistryEntryGetChildIterator(entry, kIOServicePlane, &it) != KERN_SUCCESS)
        return;
    while (io_registry_entry_t child = IOIteratorNext(it)) {
        if (CFTypeRef ref = IORegistryEntryCreateCFProperty(child, CFSTR("bInterfaceClass"), kCFAllocatorDefault, 0)) {
            quint32 cls = 0;
            if (CFGetTypeID(ref) == CFNumberGetTypeID())
                CFNumberGetValue(static_cast<CFNumberRef>(ref), kCFNumberSInt32Type, &cls);
            classes.insert(cls);
            CFRelease(ref);
        }
        collectInterfaceClasses(child, classes, depth + 1);
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
}

// Phones expose only MTP/PTP (0x06), vendor specific (0xFF: ADB, charge-only),
// CDC / RNDIS tethering (0x02, 0x0A, 0xE0) or MIDI-less audio-free configs.
// Docks, mice, headsets, webcams and disks from the same vendors expose
// hub, HID, audio, video or mass-storage interfaces instead.
bool looksLikePhone(io_service_t service)
{
    const quint32 deviceClass = intProperty(service, CFSTR("bDeviceClass"));
    if (deviceClass == 0x09) // hub
        return false;
    QSet<quint32> classes;
    collectInterfaceClasses(service, classes);
    static const QSet<quint32> phoneClasses{0x06, 0xFF, 0x02, 0x0A, 0xE0, 0xEF};
    if (classes.isEmpty())
        return true; // interfaces not published yet; vendor match is all we have
    bool hasPhoneInterface = false;
    for (quint32 c : classes) {
        if (!phoneClasses.contains(c))
            return false;
        if (c == 0x06 || c == 0xFF)
            hasPhoneInterface = true;
    }
    return hasPhoneInterface;
}

} // namespace

UsbMonitor::UsbMonitor(QObject *parent)
    : QObject(parent)
{
}

UsbMonitor::~UsbMonitor()
{
    if (m_addedIter)
        IOObjectRelease(m_addedIter);
    if (m_removedIter)
        IOObjectRelease(m_removedIter);
    if (m_port)
        IONotificationPortDestroy(static_cast<IONotificationPortRef>(m_port));
}

bool UsbMonitor::start()
{
    auto port = IONotificationPortCreate(kIOMainPortDefault);
    if (!port)
        return false;
    m_port = port;
    // Qt's macOS event dispatcher runs the main CFRunLoop, so callbacks arrive on the GUI thread.
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);

    CFMutableDictionaryRef matchAdd = IOServiceMatching("IOUSBHostDevice");
    CFMutableDictionaryRef matchRemove = IOServiceMatching("IOUSBHostDevice");
    if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, matchAdd,
                                         reinterpret_cast<IOServiceMatchingCallback>(&UsbMonitor::onMatched),
                                         this, &m_addedIter) != KERN_SUCCESS)
        return false;
    drain(m_addedIter, true);
    if (IOServiceAddMatchingNotification(port, kIOTerminatedNotification, matchRemove,
                                         reinterpret_cast<IOServiceMatchingCallback>(&UsbMonitor::onTerminated),
                                         this, &m_removedIter) != KERN_SUCCESS)
        return false;
    drain(m_removedIter, false);
    return true;
}

void UsbMonitor::onMatched(void *refcon, unsigned int iterator)
{
    static_cast<UsbMonitor *>(refcon)->drain(iterator, true);
    emit static_cast<UsbMonitor *>(refcon)->devicesChanged();
}

void UsbMonitor::onTerminated(void *refcon, unsigned int iterator)
{
    static_cast<UsbMonitor *>(refcon)->drain(iterator, false);
    emit static_cast<UsbMonitor *>(refcon)->devicesChanged();
}

void UsbMonitor::drain(unsigned int iterator, bool added)
{
    while (io_service_t service = IOIteratorNext(iterator)) {
        uint64_t regId = 0;
        IORegistryEntryGetRegistryEntryID(service, &regId);
        if (added) {
            const quint16 vendor = quint16(intProperty(service, CFSTR("idVendor")));
            if (androidVendors().contains(vendor) && looksLikePhone(service)) {
                UsbDevice d;
                d.registryId = regId;
                d.vendorId = vendor;
                d.productId = quint16(intProperty(service, CFSTR("idProduct")));
                d.productName = stringProperty(service, CFSTR("USB Product Name"));
                if (d.productName.isEmpty())
                    d.productName = stringProperty(service, CFSTR("kUSBProductString"));
                m_android.append(d);
            }
        } else {
            m_android.removeIf([regId](const UsbDevice &d) { return d.registryId == regId; });
        }
        IOObjectRelease(service);
    }
}
