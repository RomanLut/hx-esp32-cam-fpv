#include "Video_Decoder.h"
#include "Log.h"

#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <condition_variable>
#include "fmt/format.h"
#include "Clock.h"
#include "Pool.h"
#include "IHAL.h"
#include <SDL2/SDL.h>
#include "main.h"

extern "C"
{
#ifdef TEST_LATENCY
#include "pigpio.h"
#endif

#include <turbojpeg.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
}

//===================================================================================
//===================================================================================
struct Input
{
    int32_t test_value = -1;
    std::vector<uint8_t> data;
};
using Input_ptr = Pool<Input>::Ptr;

//===================================================================================
//===================================================================================
struct Output
{
    uint32_t pbo;
    size_t pbo_size = 0;
    uint32_t texture=0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb_data;

    ~Output(){
        if(texture != 0)
        {
            glDeleteTextures(1,&texture);
            LOGI("die texture:{}",texture);
        }
        if (pbo != 0) 
        {
            glDeleteBuffers(1, &pbo);
        }
    }
};
using Output_ptr = Pool<Output>::Ptr;

//===================================================================================
//===================================================================================
struct Video_Decoder::Impl
{
    SDL_Window* window = nullptr;
    std::vector<SDL_GLContext> contexts;
    std::vector<std::thread> threads;

    Pool<Input> input_pool;

    std::mutex input_queue_mutex;
    std::deque<Input_ptr> input_queue;
    std::condition_variable input_queue_cv;

    Pool<Output> output_pool;

    std::mutex output_queue_mutex;
    std::deque<Output_ptr> output_queue;

    std::deque<Output_ptr> locked_outputs;
};


//===================================================================================
//===================================================================================
Video_Decoder::Video_Decoder()
    : m_texture(0), m_impl(new Impl)
{
}

//===================================================================================
//===================================================================================
Video_Decoder::~Video_Decoder()
{
    {
        std::unique_lock<std::mutex> lg(m_impl->input_queue_mutex);
        m_exit = true;
    }
    m_impl->input_queue_cv.notify_all();

    for (auto& t: m_impl->threads)
        if (t.joinable())
            t.join();
}

//===================================================================================
//===================================================================================
bool Video_Decoder::init(IHAL& hal)
{
    m_hal = &hal;

    m_impl->window = (SDL_Window*)hal.get_window();
    assert(m_impl->window != nullptr);

    m_impl->output_pool.on_acquire = [this](Output& output) 
    {
    };

#ifdef TEST_DISPLAY_LATENCY
    for (size_t i = 0; i < 1; i++)
#else
    for (size_t i = 0; i < 4; i++)
#endif
    {
        SDL_GLContext context = SDL_GL_CreateContext(m_impl->window);
        assert(context != nullptr);
        m_impl->contexts.push_back(context);
        m_impl->threads.push_back(std::thread([this, i]() { decoder_thread_proc(i); }));
    }

    return true;
}

//===================================================================================
//===================================================================================
uint32_t Video_Decoder::get_video_texture_id() const
{
    return m_texture;
}


//===================================================================================
//===================================================================================
ImVec2 Video_Decoder::get_video_resolution() const
{
    return m_resolution;
}


//===================================================================================
//===================================================================================
bool Video_Decoder::decode_data(void const* data, size_t size)
{
    if (!data || size == 0)
    {
        s_gs_stats.brokenFrames++;
        return false;
    }
        
    Input_ptr input = m_impl->input_pool.acquire();
    input->data.resize(size);
    memcpy(input->data.data(), data, size);

    {
        std::unique_lock<std::mutex> lg(m_impl->input_queue_mutex);
        m_impl->input_queue.push_back(input);
    }

    m_impl->input_queue_cv.notify_all();

    return true;
}


//===================================================================================
//===================================================================================
void Video_Decoder::inject_test_data(uint32_t value)
{
    Input_ptr input = m_impl->input_pool.acquire();
    input->test_value = value;

    {
        std::unique_lock<std::mutex> lg(m_impl->input_queue_mutex);
        m_impl->input_queue.push_back(input);
    }

    m_impl->input_queue_cv.notify_all();
}

Clock::time_point s_start = Clock::now();


//===================================================================================
//===================================================================================
void Video_Decoder::decoder_thread_proc(size_t thread_index)
{
    LOGI("SDL window: {}", (size_t)m_impl->window);
    SDLCHK(SDL_GL_MakeCurrent(m_impl->window, m_impl->contexts[thread_index]));

    while (!m_exit)
    {
        Input_ptr input;
        {
            std::unique_lock<std::mutex> lg(m_impl->input_queue_mutex);
            if (m_impl->input_queue.empty())
                m_impl->input_queue_cv.wait(lg, [this] { return m_impl->input_queue.empty() == false || m_exit == true; });

            if (m_exit)
                break;

            if (!m_impl->input_queue.empty())
            {
                input = m_impl->input_queue.back();
                m_impl->input_queue.clear();
            }
            else
                continue;
        }
#ifdef TEST_DISPLAY_LATENCY
        uint32_t width = 800;
        uint32_t height = 600;

        Output_ptr output = m_impl->output_pool.acquire();
        output->width = width;
        output->height = height;

        output->planes[0].resize(tjPlaneSizeYUV(0, width, 0, height, TJSAMP_422));
        if (input->test_value == 0)
            memset(output->planes[0].data(), 0, output->planes[0].size());
        else
            memset(output->planes[0].data(), 255, output->planes[0].size());

        output->planes[1].resize(tjPlaneSizeYUV(1, width, 0, height, TJSAMP_422));
        memset(output->planes[1].data(), 128, output->planes[1].size());
        output->planes[2].resize(tjPlaneSizeYUV(2, width, 0, height, TJSAMP_422));
        memset(output->planes[2].data(), 128, output->planes[2].size());

        {
            //LOGI("Enq buffer {}/{} at {}", input->test_value, (size_t)output.get(), std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_start).count());
            std::lock_guard<std::mutex> lg(m_impl->output_queue_mutex);
            m_impl->output_queue.push_back(std::move(output));
        }
#else
        const uint8_t* data = (const uint8_t*)input->data.data();
        size_t size = input->data.size();

        //find the end marker for JPEG. Data after that can be discarded
        const uint8_t* dptr = &data[size - 2];
        while (dptr > data)
        {
            if (dptr[0] == 0xFF && dptr[1] == 0xD9)
            {
                dptr += 2;
                size = dptr - data;
                if ((size & 0x1FF) == 0)
                    size += 1; 
                if ((size % 100) == 0)
                    size += 1;
                break;
            }
            dptr--;
        }

        //LOGI("In size = {}, correct size = {}", _size, size);

        //auto start_tp = Clock::now();

        int width, height;
        int inSubsamp, inColorspace;

        tjhandle tjInstance = tjInitDecompress();

        if (tjDecompressHeader3(tjInstance, data, size, &width, &height, &inSubsamp, &inColorspace) < 0)
        {
            s_gs_stats.brokenFrames++;
            tjDestroy(tjInstance);
            LOGE("Jpeg header error: {}", tjGetErrorStr());
            continue;
        }

        Output_ptr output = m_impl->output_pool.acquire();

        output->rgb_data.resize(tjBufSize(width,height,inSubsamp));

        output->width = width;
        output->height = height;

        Clock::time_point t1 = Clock::now();

        //without flags decoding performance is 80% slower
        //visually there is no impact on image quality
        int flags = TJ_FASTUPSAMPLE | TJFLAG_FASTDCT;
        
        //if (tjDecompressToYUVPlanes(tjInstance, data, size, planesPtr.data(), 0, nullptr, 0, flags) < 0)
        if(tjDecompress2(tjInstance, data, size,output->rgb_data.data(),width,0,height,TJPF_RGB,flags))
        {
            s_gs_stats.brokenFrames++;
            //tjDestroy(m_impl->tjInstance);
            LOGE("decompressing JPEG image: {}", tjGetErrorStr());
            //return false;
        }
        
        tjDestroy(tjInstance);

        Clock::time_point t2 = Clock::now();
        int duration = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        s_gs_stats.decodedJpegCount++;
        s_gs_stats.decodedJpegTimeTotalMS += duration;
        s_gs_stats.decodedJpegTimeMinMS = std::min( s_gs_stats.decodedJpegTimeMinMS, duration );
        s_gs_stats.decodedJpegTimeMaxMS = std::max( s_gs_stats.decodedJpegTimeMaxMS, duration );

        {
            std::lock_guard<std::mutex> lg(m_impl->output_queue_mutex);
            m_impl->output_queue.push_back(std::move(output));
        }

        //LOGI("Decompressed in {}us, {}", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_tp).count(), input->data.size() - size);
#endif
    }
}

//===================================================================================
//===================================================================================
size_t Video_Decoder::lock_output()
{
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lg(m_impl->output_queue_mutex);
        count = m_impl->output_queue.size();
        if (count == 0)
            return 0;

        m_impl->locked_outputs.push_back(m_impl->output_queue.back());
        m_impl->output_queue.clear();
    }

    Output& output = *m_impl->locked_outputs.back();

    //LOGI("* Rcv buffer {} at {}", (size_t)&output, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_start).count());
    if(output.texture == 0){
        GLCHK(glGenBuffers(1, &output.pbo));
        GLCHK(glGenTextures(1, &output.texture));
        GLCHK(glBindTexture(GL_TEXTURE_2D, output.texture));
        GLCHK(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
        GLCHK(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GLCHK(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        LOGI("Texture: {}", output.texture);
    }

#if defined(IMGUI_IMPL_OPENGL_ES2)
    GLCHK(glBindTexture(GL_TEXTURE_2D, output.texture));
    GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, output.width, output.height, 0, GL_RGB, GL_UNSIGNED_BYTE, output.rgb_data.data()));
#else
    //calculate total size
    GLCHK(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, output.pbo));
    size_t pbo_size = output.rgb_data.size();

    {
            GLCHK(glBufferData(GL_PIXEL_UNPACK_BUFFER, pbo_size, nullptr, GL_STREAM_DRAW));
        output.pbo_size = pbo_size;
    }

    // map the buffer object into client's memory
    uint8_t* ptr = (uint8_t*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, pbo_size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (ptr)
    {
        //copy the page into the pbo, aligned to 16 bytes
        memcpy(ptr,output.rgb_data.data(),output.rgb_data.size());
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER); // release the mapped buffer
    }

    GLCHK(glBindTexture(GL_TEXTURE_2D, output.texture));
    GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, output.width, output.height, 0, GL_RGB, GL_UNSIGNED_BYTE, 0));

    GLCHK(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0));
#endif

    m_texture = output.texture;
    m_resolution = ImVec2((float)output.width, (float)output.height);

    return count;
}


//===================================================================================
//===================================================================================
bool Video_Decoder::unlock_output()
{
    if (m_impl->locked_outputs.empty())
        return false;

    while (m_impl->locked_outputs.size() > 4)
        m_impl->locked_outputs.pop_front();

    return true;
}

//===================================================================================
//===================================================================================
bool Video_Decoder::isAspect16x9()
{
   if ( m_resolution.y == 0 ) return false;
   float aspect =  m_resolution.x / m_resolution.y;
   return aspect > 1.4f;
}