#include "cpu_temp.h"
#include <stdio.h>
#include <unistd.h>

//=======================================================
//=======================================================
CPUTemp::CPUTemp()
{
    this->temperature = 0;
    this->last_update_tp = Clock::now();
}

//=======================================================
//=======================================================
void CPUTemp::updateTemperature()
{
  FILE *temperatureFile = fopen ("/sys/class/thermal/thermal_zone0/temp", "r");
  if (!temperatureFile) return;
  float T;
  if ( fscanf (temperatureFile, "%f", &T) != 1 ) 
  {
    fclose(temperatureFile);
    return;
  }
  this->temperature = T / 1000.0f;
  fclose (temperatureFile);

  //printf("CPU Temperature: %.2f°C\n", T);
}

//=======================================================
//=======================================================
void CPUTemp::process()
{
    if (Clock::now() - this->last_update_tp >= std::chrono::milliseconds(3000))
    {
        this->last_update_tp = Clock::now();
        this->updateTemperature();
    }
}

//=======================================================
//=======================================================
float CPUTemp::getTemperature()
{
    return this->temperature;
}


CPUTemp g_CPUTemp;