#pragma once

#include <string>

#include "ini.h"

class SettingsStorage : public mINI::INIStructure
{
public:
    explicit SettingsStorage(const std::string& filename = "gs.ini");

    bool save(bool pretty = false);
    bool read();
    void setPath(const std::string& filename);
    void loadGroundStationConfig();
    void loadGround2AirConfig();
    void saveGroundStationConfig();
    void saveGround2AirConfig();

private:
    mINI::INIFile m_ini_file;
};
