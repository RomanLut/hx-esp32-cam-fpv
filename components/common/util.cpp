#include "util.h"

//==============================================================
//==============================================================
int smallestPowerOfTwo(int value, int minValue)
{
  if (value < minValue)
  {
    return minValue; 
  }

  if (value < 2)
  {
    return 2;
  }

  int powerOfTwo = 1;
  while (powerOfTwo < value)
  {
    powerOfTwo <<= 1;
  }

  return powerOfTwo;
}

