#include "android_bitmap_jpeg_decoder.h"
#include "../../../../../components/common/Clock.h"

#include <jni.h>
#include <android/asset_manager.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>

#include "Log.h"
#include "gs_jpeg_dct_postprocessing.h"
#include "gs_shared_state.h"
#include "gs_video_stabilization_shared.h"

namespace
{

constexpr uint32_t kDefaultMinDecodeMs = 99;

JavaVM* g_java_vm = nullptr;
AAssetManager* g_asset_manager = nullptr;
jclass g_bitmap_decode_bridge_class = nullptr;
jmethodID g_decode_rgb565_method = nullptr;
jmethodID g_decode_rgb8888_method = nullptr;
jclass g_decode_result_class = nullptr;
jfieldID g_decode_result_pixels = nullptr;
jfieldID g_decode_result_width = nullptr;
jfieldID g_decode_result_height = nullptr;
jfieldID g_decode_result_stride = nullptr;

//===================================================================================
//===================================================================================
// RAII guard that attaches the current thread to the JVM on construction and
// detaches it on destruction if attachment was performed here.
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

//===================================================================================
//===================================================================================
// Wraps a JNI global reference to a direct ByteBuffer in a shared_ptr whose
// custom deleter releases the global reference when the last owner drops it.
std::shared_ptr<void> makeDirectBufferRef(jobject global_pixels_buffer)
{
    return std::shared_ptr<void>(
        global_pixels_buffer,
        [](void* ref)
        {
            if (ref == nullptr || g_java_vm == nullptr)
            {
                return;
            }

            AttachGuard jni;
            if (!jni.attach())
            {
                return;
            }

            jni.env->DeleteGlobalRef(static_cast<jobject>(ref));
        });
}

//===================================================================================
//===================================================================================
// Constructs a DecodeStats value from the given raw counter values.
AndroidBitmapJpegDecoder::DecodeStats makeStats(uint32_t broken_frames,
                                          uint32_t input_submitted_count,
                                          uint32_t dropped_input_count,
                                          uint32_t decoded_count,
                                          uint32_t decoded_total_ms,
                                          uint32_t decoded_min_ms,
                                          uint32_t decoded_max_ms)
{
    AndroidBitmapJpegDecoder::DecodeStats stats;
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

//===================================================================================
//===================================================================================
// Returns the global JavaVM pointer set during JNI_OnLoad.
JavaVM* androidGetJavaVm()
{
    return g_java_vm;
}

//===================================================================================
//===================================================================================
// Returns the global AAssetManager pointer set via androidSetAssetManager.
AAssetManager* androidGetAssetManager()
{
    return g_asset_manager;
}

//===================================================================================
//===================================================================================
// Stores the AAssetManager pointer for use by native code.
void androidSetAssetManager(AAssetManager* asset_manager)
{
    g_asset_manager = asset_manager;
}

//===================================================================================
//===================================================================================
// Defined in native_bridge.cpp — looks up NativeCore.createRecordingFd for MediaStore recording.
void initRecordingJniRefs(JNIEnv* env);

//===================================================================================
//===================================================================================
// Called by the JVM when the native library is loaded. Stores the JavaVM pointer
// and looks up all JNI class and method references needed for JPEG decoding.
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

    g_decode_rgb8888_method = env->GetStaticMethodID(
        g_bitmap_decode_bridge_class,
        "decodeRgb8888",
        "([B)Lcom/esp32camfpv/androidgs/BitmapDecodeBridge$Result;");
    if (g_decode_rgb8888_method == nullptr)
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

    initRecordingJniRefs(env);

    return JNI_VERSION_1_6;
}

//===================================================================================
//===================================================================================
// Constructor. Starts the single decode worker thread that processes the decode queue.
AndroidBitmapJpegDecoder::AndroidBitmapJpegDecoder(GsVideoRenderer& renderer)
    : m_renderer(renderer)
{
    m_thread = std::thread(&AndroidBitmapJpegDecoder::workerThreadProc, this);
}

//===================================================================================
//===================================================================================
// Destructor. Signals worker threads to exit and waits for them to finish.
AndroidBitmapJpegDecoder::~AndroidBitmapJpegDecoder()
{
    m_exit.store(true);
    m_input_cv.notify_all();

    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

//===================================================================================
//===================================================================================
// Enqueues a JPEG buffer for decoding, replacing any previously pending frame.
// Drops the buffer silently if it is empty.
void AndroidBitmapJpegDecoder::submitJpeg(gs::core::VideoFrameAssembler::FrameBufferPtr jpeg_buffer, uint32_t frame_id)
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

//===================================================================================
//===================================================================================
// Drops queued JPEG frames that have not yet reached a decoder worker.
void AndroidBitmapJpegDecoder::clearPending()
{
    std::lock_guard<std::mutex> lock(m_input_mutex);
    m_input_queue.clear();
}

//===================================================================================
//===================================================================================
// Returns the total number of decoded frames submitted to the renderer since construction.
uint64_t AndroidBitmapJpegDecoder::submittedFrameCount() const
{
    return m_submitted_frames.load();
}

//===================================================================================
//===================================================================================
// Returns a snapshot of the current decode statistics without resetting the counters.
AndroidBitmapJpegDecoder::DecodeStats AndroidBitmapJpegDecoder::statsSnapshot() const
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

//===================================================================================
//===================================================================================
// Returns the current decode statistics and resets all counters to zero.
AndroidBitmapJpegDecoder::DecodeStats AndroidBitmapJpegDecoder::consumeStats()
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

//===================================================================================
//===================================================================================
// Worker thread loop: waits for queued JPEG frames, decodes each one via the
// Android BitmapDecodeBridge JNI call, and submits the resulting RGBA8888 pixels
// to the renderer.
void AndroidBitmapJpegDecoder::workerThreadProc()
{
    AttachGuard jni;
    if (!jni.attach())
    {
        LOGE("Failed to attach decoder thread to JVM");
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

        gs::render::VideoPostprocessingParams postprocessing_params = {};
        gs::render::buildJpegDctPostprocessingParams(input.jpeg_buffer->data.data(),
                                                     input.jpeg_buffer->data.size(),
                                                     postprocessing_params);

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

        const auto decode_begin = Clock::now();
        const bool use_rgb565 =
            s_postprocessingState.pipeline_mode == PostprocessingState::PipelineMode::RGB565;
        jobject result = jni.env->CallStaticObjectMethod(
            g_bitmap_decode_bridge_class,
            use_rgb565 ? g_decode_rgb565_method : g_decode_rgb8888_method,
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

        const jint minimum_stride = use_rgb565 ? width * 2 : width * 4;
        if (pixels == nullptr || capacity <= 0 || width <= 0 || height <= 0 || stride < minimum_stride)
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
                Clock::now() - decode_begin)
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

        bool run_prepare_after_publish = false;
        const GsVisionImageFormat vision_format = use_rgb565
            ? GS_VISION_IMAGE_FORMAT_RGB565
            : GS_VISION_IMAGE_FORMAT_RGBA8;
        if (gs::stabilization::isEnabled())
        {
            gs::stabilization::estimateFrame(pixels,
                                             static_cast<size_t>(capacity),
                                             static_cast<int>(width),
                                             static_cast<int>(height),
                                             static_cast<int>(stride),
                                             vision_format,
                                             false,
                                             0);
            gs::stabilization::updateRenderTrajectoryStateForFrame(
                input.frame_id,
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height));
            run_prepare_after_publish = true;
        }
        else
        {
            gs::stabilization::resetRenderTrajectoryState();
        }

        m_renderer.submitFrame(
            makeDirectBufferRef(global_pixels_buffer),
            pixels,
            static_cast<size_t>(capacity),
            static_cast<int>(width),
            static_cast<int>(height),
            static_cast<int>(stride),
            input.frame_id,
            use_rgb565
                ? GsVideoRenderer::PixelFormat::RGB565
                : GsVideoRenderer::PixelFormat::RGBA8888,
            true,
            postprocessing_params);
        m_submitted_frames.fetch_add(1);

        if (run_prepare_after_publish)
        {
            // Mirror Linux ordering: publish first, then precompute next-frame features.
            gs::stabilization::prepareFrameFeatures(pixels,
                                                    static_cast<size_t>(capacity),
                                                    static_cast<int>(width),
                                                    static_cast<int>(height),
                                                    static_cast<int>(stride),
                                                    vision_format);
        }
    }
}
