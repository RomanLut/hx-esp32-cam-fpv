#include "android_jpeg_decoder.h"

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>

namespace
{

constexpr const char* kLogTag = "AndroidGSBitmapDecode";
constexpr uint32_t kDefaultMinDecodeMs = 99;
constexpr size_t kWorkerCount = 2;

JavaVM* g_java_vm = nullptr;
jclass g_bitmap_decode_bridge_class = nullptr;
jmethodID g_decode_rgb565_method = nullptr;
jclass g_decode_result_class = nullptr;
jfieldID g_decode_result_pixels = nullptr;
jfieldID g_decode_result_width = nullptr;
jfieldID g_decode_result_height = nullptr;
jfieldID g_decode_result_stride = nullptr;

struct AttachGuard
{
    JNIEnv* env = nullptr;
    bool detach = false;

    bool attach()
    {
        if (g_java_vm == nullptr)
        {
            return false;
        }

        const jint get_env_result =
            g_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (get_env_result == JNI_OK)
        {
            return true;
        }

        if (g_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            env = nullptr;
            return false;
        }

        detach = true;
        return true;
    }

    ~AttachGuard()
    {
        if (detach && g_java_vm != nullptr)
        {
            g_java_vm->DetachCurrentThread();
        }
    }
};

AndroidJpegDecoder::DecodeStats makeStats(uint32_t broken_frames,
                                          uint32_t input_submitted_count,
                                          uint32_t dropped_input_count,
                                          uint32_t decoded_count,
                                          uint32_t decoded_total_ms,
                                          uint32_t decoded_min_ms,
                                          uint32_t decoded_max_ms)
{
    AndroidJpegDecoder::DecodeStats stats;
    stats.broken_frames = broken_frames;
    stats.input_submitted_count = input_submitted_count;
    stats.overwritten_pending_count = dropped_input_count;
    stats.decoded_count = decoded_count;
    stats.decoded_total_ms = decoded_total_ms;
    stats.decoded_min_ms = decoded_min_ms;
    stats.decoded_max_ms = decoded_max_ms;
    return stats;
}

} // namespace

JavaVM* androidGetJavaVm()
{
    return g_java_vm;
}

jint JNI_OnLoad(JavaVM* vm, void* /* reserved */)
{
    g_java_vm = vm;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr)
    {
        return JNI_ERR;
    }

    jclass bridge_class_local = env->FindClass("com/esp32camfpv/androidgs/BitmapDecodeBridge");
    if (bridge_class_local == nullptr)
    {
        return JNI_ERR;
    }
    g_bitmap_decode_bridge_class =
        reinterpret_cast<jclass>(env->NewGlobalRef(bridge_class_local));
    env->DeleteLocalRef(bridge_class_local);
    if (g_bitmap_decode_bridge_class == nullptr)
    {
        return JNI_ERR;
    }

    g_decode_rgb565_method = env->GetStaticMethodID(
        g_bitmap_decode_bridge_class,
        "decodeRgb565",
        "([B)Lcom/esp32camfpv/androidgs/BitmapDecodeBridge$Result;");
    if (g_decode_rgb565_method == nullptr)
    {
        return JNI_ERR;
    }

    jclass result_class_local =
        env->FindClass("com/esp32camfpv/androidgs/BitmapDecodeBridge$Result");
    if (result_class_local == nullptr)
    {
        return JNI_ERR;
    }
    g_decode_result_class = reinterpret_cast<jclass>(env->NewGlobalRef(result_class_local));
    env->DeleteLocalRef(result_class_local);
    if (g_decode_result_class == nullptr)
    {
        return JNI_ERR;
    }

    g_decode_result_pixels =
        env->GetFieldID(g_decode_result_class, "pixels", "Ljava/nio/ByteBuffer;");
    g_decode_result_width = env->GetFieldID(g_decode_result_class, "width", "I");
    g_decode_result_height = env->GetFieldID(g_decode_result_class, "height", "I");
    g_decode_result_stride = env->GetFieldID(g_decode_result_class, "stride", "I");
    if (g_decode_result_pixels == nullptr ||
        g_decode_result_width == nullptr ||
        g_decode_result_height == nullptr ||
        g_decode_result_stride == nullptr)
    {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}

AndroidJpegDecoder::AndroidJpegDecoder(AndroidVideoRenderer& renderer)
    : m_renderer(renderer)
{
    for (size_t index = 0; index < kWorkerCount; ++index)
    {
        m_threads.emplace_back(&AndroidJpegDecoder::workerThreadProc, this);
    }
}

AndroidJpegDecoder::~AndroidJpegDecoder()
{
    m_exit.store(true);
    m_input_cv.notify_all();

    for (std::thread& thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void AndroidJpegDecoder::submitJpeg(gs::core::VideoFrameAssembler::FrameBufferPtr jpeg_buffer, uint32_t frame_id)
{
    if (!jpeg_buffer || jpeg_buffer->data.empty())
    {
        return;
    }

    InputFrame input;
    input.jpeg_buffer = std::move(jpeg_buffer);
    input.frame_id = frame_id;

    {
        std::lock_guard<std::mutex> lock(m_input_mutex);
        m_input_submitted_count.fetch_add(1);
        if (!m_input_queue.empty())
        {
            m_dropped_input_count.fetch_add(static_cast<uint32_t>(m_input_queue.size()));
            m_input_queue.clear();
        }
        m_input_queue.push_back(std::move(input));
    }
    m_input_cv.notify_one();
}

uint64_t AndroidJpegDecoder::submittedFrameCount() const
{
    return m_submitted_frames.load();
}

AndroidJpegDecoder::DecodeStats AndroidJpegDecoder::statsSnapshot() const
{
    return makeStats(
        m_broken_frames.load(),
        m_input_submitted_count.load(),
        m_dropped_input_count.load(),
        m_decoded_count.load(),
        m_decoded_total_ms.load(),
        m_decoded_min_ms.load(),
        m_decoded_max_ms.load());
}

AndroidJpegDecoder::DecodeStats AndroidJpegDecoder::consumeStats()
{
    auto stats = makeStats(
        m_broken_frames.exchange(0),
        m_input_submitted_count.exchange(0),
        m_dropped_input_count.exchange(0),
        m_decoded_count.exchange(0),
        m_decoded_total_ms.exchange(0),
        m_decoded_min_ms.exchange(kDefaultMinDecodeMs),
        m_decoded_max_ms.exchange(0));
    if (stats.decoded_count == 0)
    {
        stats.decoded_min_ms = kDefaultMinDecodeMs;
    }
    return stats;
}

void AndroidJpegDecoder::workerThreadProc()
{
    AttachGuard jni;
    if (!jni.attach())
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to attach decoder thread to JVM");
        m_broken_frames.fetch_add(1);
        return;
    }

    InputFrame input;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_input_mutex);
            m_input_cv.wait(lock, [this] { return m_exit.load() || !m_input_queue.empty(); });
            if (m_exit.load())
            {
                break;
            }
            input = std::move(m_input_queue.back());
            m_input_queue.clear();
        }

        jbyteArray jpeg_array =
            jni.env->NewByteArray(static_cast<jsize>(input.jpeg_buffer->data.size()));
        if (jpeg_array == nullptr)
        {
            input.jpeg_buffer.reset();
            m_broken_frames.fetch_add(1);
            continue;
        }
        jni.env->SetByteArrayRegion(
            jpeg_array,
            0,
            static_cast<jsize>(input.jpeg_buffer->data.size()),
            reinterpret_cast<const jbyte*>(input.jpeg_buffer->data.data()));
        input.jpeg_buffer.reset();

        const auto decode_begin = std::chrono::steady_clock::now();
        jobject result = jni.env->CallStaticObjectMethod(
            g_bitmap_decode_bridge_class,
            g_decode_rgb565_method,
            jpeg_array);
        jni.env->DeleteLocalRef(jpeg_array);

        if (jni.env->ExceptionCheck())
        {
            jni.env->ExceptionClear();
            m_broken_frames.fetch_add(1);
            continue;
        }

        if (result == nullptr)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        jobject pixels_buffer = jni.env->GetObjectField(result, g_decode_result_pixels);
        const jint width = jni.env->GetIntField(result, g_decode_result_width);
        const jint height = jni.env->GetIntField(result, g_decode_result_height);
        const jint stride = jni.env->GetIntField(result, g_decode_result_stride);
        uint8_t* pixels = pixels_buffer == nullptr
            ? nullptr
            : static_cast<uint8_t*>(jni.env->GetDirectBufferAddress(pixels_buffer));
        const jlong capacity = pixels_buffer == nullptr
            ? 0
            : jni.env->GetDirectBufferCapacity(pixels_buffer);

        if (pixels == nullptr || capacity <= 0 || width <= 0 || height <= 0 || stride < width * 2)
        {
            if (pixels_buffer != nullptr)
            {
                jni.env->DeleteLocalRef(pixels_buffer);
            }
            jni.env->DeleteLocalRef(result);
            m_broken_frames.fetch_add(1);
            continue;
        }

        jobject global_pixels_buffer = jni.env->NewGlobalRef(pixels_buffer);

        if (pixels_buffer != nullptr)
        {
            jni.env->DeleteLocalRef(pixels_buffer);
        }
        jni.env->DeleteLocalRef(result);

        if (global_pixels_buffer == nullptr)
        {
            m_broken_frames.fetch_add(1);
            continue;
        }

        const uint32_t duration_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - decode_begin)
                .count());
        m_decoded_count.fetch_add(1);
        m_decoded_total_ms.fetch_add(duration_ms);

        uint32_t current_min = m_decoded_min_ms.load();
        while (duration_ms < current_min &&
               !m_decoded_min_ms.compare_exchange_weak(current_min, duration_ms))
        {
        }

        uint32_t current_max = m_decoded_max_ms.load();
        while (duration_ms > current_max &&
               !m_decoded_max_ms.compare_exchange_weak(current_max, duration_ms))
        {
        }

        m_renderer.submitFrame(
            global_pixels_buffer,
            pixels,
            static_cast<size_t>(capacity),
            static_cast<int>(width),
            static_cast<int>(height),
            static_cast<int>(stride),
            input.frame_id,
            AndroidVideoRenderer::PixelFormat::RGB565);
        m_submitted_frames.fetch_add(1);
    }
}
