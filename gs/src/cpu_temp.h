#pragma once

#include "main.h"
#include "Clock.h"

//=======================================================
//=======================================================
class CPUTemp
{
public:
    CPUTemp();
    void process();
    float getTemperature();

private:
    float temperature;
    void updateTemperature();
    Clock::time_point last_update_tp;
};

extern CPUTemp g_CPUTemp;
