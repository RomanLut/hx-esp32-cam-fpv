#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "ISerialTelemetry.h"

// USB-UART telemetry bridge for the Oculus Quest GS.
//
// The actual USB-serial port is owned by the Java side
// (SerialTelemetryUsbController + usb-serial-for-android). This C++ class is the
// shim consumed by gs_session_core: incoming bytes pushed by JNI land in a ring
// buffer that read() drains, and write() forwards bytes back to Java where the
// active UsbSerialPort delivers them.
class AndroidSerialTelemetry : public ISerialTelemetry
{
public:
    bool init(const std::string& /*port_name*/) override;
    bool isOpen() const override;
    int read(uint8_t* buf, size_t max_bytes) override;
    void write(const uint8_t* data, size_t size) override;

    // Called from JNI bindings driven by SerialTelemetryUsbController.
    void onJavaOpened();
    void onJavaClosed();
    void onJavaBytesReceived(const uint8_t* data, size_t size);

private:
    static constexpr size_t kRxRingCapacity = 64 * 1024;

    std::atomic<bool> m_open{false};
    std::mutex m_rx_mutex;
    std::vector<uint8_t> m_rx_buffer;
};

extern AndroidSerialTelemetry g_androidSerialTelemetry;

// Caches NativeCore.serialTelemetryWrite refs. Must run on a thread with the
// app class loader (i.e. from JNI_OnLoad), because FindClass on a thread
// attached via AttachCurrentThread uses the system class loader and cannot
// find application classes.
struct _JNIEnv;
void initSerialTelemetryJniRefs(_JNIEnv* env);
