#pragma once

#include <memory>
#include "imgui.h"

class IHAL;

class Video_Decoder
{
public:
    Video_Decoder();
    ~Video_Decoder();

    bool decode_data(void const* data, size_t size);
    void inject_test_data(uint32_t value);

    bool init(IHAL& hal);

    size_t lock_output();
    uint32_t get_video_texture_id() const;
    ImVec2 get_video_resolution() const;
    bool unlock_output();

    bool isAspect16x9();

    struct Impl;

private:
    void decoder_thread_proc(size_t thread_index);

    IHAL* m_hal = nullptr;
    bool m_exit = false;
    ImVec2 m_resolution;
    uint32_t m_texture;
    uint32_t videoWidth;
    std::unique_ptr<Impl> m_impl;
};

