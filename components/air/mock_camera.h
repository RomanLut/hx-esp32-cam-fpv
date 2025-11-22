#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Get the mock camera frame data (embedded no_signal.jpg).
 *
 * @param out_buf Pointer to a const uint8_t* that will be set to the start of the JPEG data.
 * @param out_len Pointer to a size_t that will be set to the length of the JPEG data.
 */
void get_mock_camera_frame(const uint8_t** out_buf, size_t* out_len);