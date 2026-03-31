#pragma once

#include <cstdint>

#include "packets.h"

namespace gs::core
{

class OSDBase
{
public:
    OSDBase();
    virtual ~OSDBase() = default;

    void clear();
    void update(const uint8_t* data, uint16_t size);
    void setLowChar(int row, int col, uint8_t c);
    void draw(int x1, int y1, int x2, int y2);
    void drawFitted(int viewport_width, int viewport_height, float content_aspect, float target_aspect);

    const OSDBuffer& buffer() const;

protected:
    virtual void drawChar(uint16_t code, int x, int y, int width, int height) = 0;

private:
    OSDBuffer m_buffer;
};

}
