#pragma once

template<typename T>
T clamp(T value, T min, T max) 
{
    return (value < min) ? min : (value > max) ? max : value;
}
extern int smallestPowerOfTwo(int value, int minValue);
