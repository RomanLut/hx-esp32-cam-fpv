#include "mock_camera.h"

void get_mock_camera_frame(const uint8_t** out_buf, size_t* out_len)
{
    extern const unsigned char jpg_start[] asm("_binary_nosignal_jpg_start");
    extern const unsigned char jpg_end[]   asm("_binary_nosignal_jpg_end");
    const size_t jpg_size = (jpg_end - jpg_start);

    *out_buf = jpg_start;
    *out_len = jpg_size;
}