#include "circular_buffer.h"


size_t Circular_Buffer::size() const
{
    return m_size;
}

bool Circular_Buffer::empty() const
{
    return m_size == 0;
}

size_t Circular_Buffer::get_space_left() const
{
    return m_capacity - m_size;
}

size_t Circular_Buffer::capacity() const
{
    return m_capacity;
}

void Circular_Buffer::resize(size_t size)
{
    assert(size < capacity());
    m_size = size;
}

bool Circular_Buffer::write(const void* data, size_t size)
{
    if (size >= get_space_left())
        return false;

    size_t idx = (m_start + m_size) % m_capacity;

    if (idx + size <= m_capacity) //no wrap
        memcpy(m_data + idx, data, size);
    else //wrap
    {
        size_t first = m_capacity - idx;
        memcpy(m_data + idx, data, first);
        memcpy(m_data, (uint8_t*)data + first, size - first);
    }
    m_size += size;
    return true;
}

bool Circular_Buffer::writeBytes(uint8_t b, size_t size)
{
    if (size >= get_space_left())
        return false;

    size_t idx = (m_start + m_size) % m_capacity;

    if (idx + size <= m_capacity) //no wrap
        memset(m_data + idx, b, size);
    else //wrap
    {
        size_t first = m_capacity - idx;
        memset(m_data + idx, b, first);
        memset(m_data, b, size - first);
    }
    m_size += size;
    return true;
}

bool Circular_Buffer::read(void* dst, size_t size)
{
    if (m_size < size)
        return false;

    if (m_start + size <= m_capacity) //no wrap around
    {
        memcpy(dst, m_data + m_start, size);
        m_start = (m_start + size) % m_capacity;
        m_size -= size;
        return true;
    }

    //wrap around, 2 steps
    size_t first = m_capacity - m_start;
    memcpy(dst, m_data + m_start, first);
    memcpy((uint8_t*)dst + first, m_data, size - (first));
    m_start = (m_start + size) % m_capacity;
    m_size -= size;
    return true;
}

bool Circular_Buffer::skip(size_t size)
{
    if (m_size < size)
        return false;

    m_start = (m_start + size) % m_capacity;
    m_size -= size;
    return true;
}

const void* Circular_Buffer::start_reading(size_t& size)
{
    if (m_size == 0)
    {
        size = 0;
        return nullptr;
    }

    if (size > m_size) //clamp to the actual size
        size = m_size;

    if (m_start + size > m_capacity) //wrap around
        size = m_capacity - m_start;
    
    return m_data + m_start;
}

void Circular_Buffer::end_reading(size_t size) //call with the same size as the one returned by start_reading
{
    if (size == 0)
        return;

    assert(size <= m_size);

    m_start = (m_start + size) % m_capacity;
    m_size -= size;
}

void Circular_Buffer::clear()
{
    m_start = 0;
    m_size = 0;
}

uint8_t Circular_Buffer::peek( size_t offset)
{
    offset = (m_start + offset) % m_capacity;
    return this->m_data[offset];
}