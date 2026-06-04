#include "android_serial_telemetry.h"

#include <jni.h>
#include <algorithm>
#include <cstring>

#include "android_jni_shared.h"
#include "gs_runtime_config.h"
#include "gs_shared_state.h"
#include "Log.h"

AndroidSerialTelemetry g_androidSerialTelemetry;

namespace
{
std::mutex g_uart_list_mutex;
std::vector<std::string> g_uart_list;
}  // namespace

#if defined(OCULUS_QUEST_GS)
#define ANDROID_GS_NATIVE_CORE_CLASS "com/esp32camfpv/questgs/NativeCore"
#define ANDROID_GS_JNI(name) Java_com_esp32camfpv_questgs_NativeCore_##name
#else
#define ANDROID_GS_NATIVE_CORE_CLASS "com/esp32camfpv/androidgs/NativeCore"
#define ANDROID_GS_JNI(name) Java_com_esp32camfpv_androidgs_NativeCore_##name
#endif

void publishAndroidTelemetryUartList(const std::vector<std::string>& uarts)
{
    std::lock_guard<std::mutex> lock(g_uart_list_mutex);
    g_uart_list = uarts;
}

std::vector<std::string> copyAndroidTelemetryUartList()
{
    std::lock_guard<std::mutex> lock(g_uart_list_mutex);
    return g_uart_list;
}

namespace
{

// JNI handles for calling NativeCore.serialTelemetryWrite([B) from C++.
// Resolved once during JNI_OnLoad. Cannot be resolved lazily from an
// AttachCurrentThread'ed thread: that env uses the system class loader and
// FindClass(ANDROID_GS_NATIVE_CORE_CLASS) returns null.
jclass g_native_core_class = nullptr;
jmethodID g_write_method = nullptr;

}  // namespace

void initSerialTelemetryJniRefs(JNIEnv* env)
{
    if (env == nullptr || g_native_core_class != nullptr)
    {
        return;
    }
    jclass local = env->FindClass(ANDROID_GS_NATIVE_CORE_CLASS);
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

//===================================================================================
//===================================================================================
// gs_runtime_config.h implementations for Android. Listing comes from the Java
// controller's published snapshot. Apply is a no-op since the Kotlin side polls
// every 3 s and reacts to attach/detach broadcasts on its own.
std::vector<std::string> listAvailableTelemetryUarts()
{
    return copyAndroidTelemetryUartList();
}

std::string getTelemetryUartDisplayLabel(const std::string& identifier)
{
    return identifier;
}

void applySelectedTelemetryUart()
{
    // The Kotlin SerialTelemetryUsbController re-reads s_groundstation_config
    // .telemetryUart on each sync (every 3 s + on USB broadcasts). It will
    // close a non-matching open port and open a matching one without any
    // explicit call from C++.
}

//===================================================================================
//===================================================================================
// JNI bindings shared by both Android apps; the Quest target uses a different
// package name and selects its exported symbol names through OCULUS_QUEST_GS.
extern "C" JNIEXPORT void JNICALL
ANDROID_GS_JNI(publishTelemetryUarts)(JNIEnv* env, jclass /* clazz */, jobjectArray uarts)
{
    std::vector<std::string> out;
    if (uarts != nullptr)
    {
        const jsize n = env->GetArrayLength(uarts);
        out.reserve(static_cast<size_t>(n));
        for (jsize i = 0; i < n; i++)
        {
            jstring js = static_cast<jstring>(env->GetObjectArrayElement(uarts, i));
            if (js == nullptr) continue;
            const char* cs = env->GetStringUTFChars(js, nullptr);
            if (cs != nullptr)
            {
                out.emplace_back(cs);
                env->ReleaseStringUTFChars(js, cs);
            }
            env->DeleteLocalRef(js);
        }
    }
    publishAndroidTelemetryUartList(out);
}

extern "C" JNIEXPORT jstring JNICALL
ANDROID_GS_JNI(getTelemetryUartSelection)(JNIEnv* env, jclass /* clazz */)
{
    return env->NewStringUTF(s_groundstation_config.telemetryUart.c_str());
}
