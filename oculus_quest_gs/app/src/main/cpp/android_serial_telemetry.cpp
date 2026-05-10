#include "android_serial_telemetry.h"

#include <jni.h>
#include <algorithm>
#include <cstring>

#include "android_jni_shared.h"
#include "Log.h"

AndroidSerialTelemetry g_androidSerialTelemetry;

namespace
{

// JNI handles for calling NativeCore.serialTelemetryWrite([B) from C++.
// Resolved once during JNI_OnLoad. Cannot be resolved lazily from an
// AttachCurrentThread'ed thread: that env uses the system class loader and
// FindClass("com/esp32camfpv/androidgs/NativeCore") returns null.
jclass g_native_core_class = nullptr;
jmethodID g_write_method = nullptr;

}  // namespace

void initSerialTelemetryJniRefs(JNIEnv* env)
{
    if (env == nullptr || g_native_core_class != nullptr)
    {
        return;
    }
    jclass local = env->FindClass("com/esp32camfpv/androidgs/NativeCore");
    if (local == nullptr)
    {
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
        }
        return;
    }
    g_native_core_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_write_method = env->GetStaticMethodID(g_native_core_class, "serialTelemetryWrite", "([B)V");
}

bool AndroidSerialTelemetry::init(const std::string& /*port_name*/)
{
    // The Java controller is the source of truth for whether the port is open.
    // Consumers only check isOpen() before reading/writing.
    return true;
}

bool AndroidSerialTelemetry::isOpen() const
{
    return m_open.load(std::memory_order_acquire);
}

int AndroidSerialTelemetry::read(uint8_t* buf, size_t max_bytes)
{
    if (buf == nullptr || max_bytes == 0)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_rx_mutex);
    if (m_rx_buffer.empty())
    {
        return 0;
    }
    const size_t n = std::min(max_bytes, m_rx_buffer.size());
    std::memcpy(buf, m_rx_buffer.data(), n);
    m_rx_buffer.erase(m_rx_buffer.begin(), m_rx_buffer.begin() + n);
    return static_cast<int>(n);
}

void AndroidSerialTelemetry::write(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0 || !isOpen())
    {
        return;
    }

    JavaVM* vm = androidGetJavaVm();
    if (vm == nullptr)
    {
        return;
    }

    JNIEnv* env = nullptr;
    bool didAttach = false;
    const jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED)
    {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            return;
        }
        didAttach = true;
    }
    else if (envResult != JNI_OK || env == nullptr)
    {
        return;
    }

    if (g_native_core_class != nullptr && g_write_method != nullptr)
    {
        jbyteArray arr = env->NewByteArray(static_cast<jsize>(size));
        if (arr != nullptr)
        {
            env->SetByteArrayRegion(
                arr, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
            env->CallStaticVoidMethod(g_native_core_class, g_write_method, arr);
            env->DeleteLocalRef(arr);
        }
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
        }
    }

    if (didAttach)
    {
        vm->DetachCurrentThread();
    }
}

void AndroidSerialTelemetry::onJavaOpened()
{
    {
        std::lock_guard<std::mutex> lock(m_rx_mutex);
        m_rx_buffer.clear();
        m_rx_buffer.reserve(kRxRingCapacity);
    }
    m_open.store(true, std::memory_order_release);
    LOGI("AndroidSerialTelemetry: port opened");
}

void AndroidSerialTelemetry::onJavaClosed()
{
    m_open.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_rx_mutex);
    m_rx_buffer.clear();
    LOGI("AndroidSerialTelemetry: port closed");
}

void AndroidSerialTelemetry::onJavaBytesReceived(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_rx_mutex);
    // Drop oldest bytes if the consumer can't keep up — telemetry is best-effort.
    if (m_rx_buffer.size() + size > kRxRingCapacity)
    {
        const size_t overflow = (m_rx_buffer.size() + size) - kRxRingCapacity;
        if (overflow >= m_rx_buffer.size())
        {
            m_rx_buffer.clear();
        }
        else
        {
            m_rx_buffer.erase(m_rx_buffer.begin(), m_rx_buffer.begin() + overflow);
        }
    }
    m_rx_buffer.insert(m_rx_buffer.end(), data, data + size);
}
