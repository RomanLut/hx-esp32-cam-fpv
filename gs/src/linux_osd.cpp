#include <iostream>
#include <filesystem>
#include <vector>
#include <string>


#include "linux_osd.h"
#include "imgui.h"
#include "gs_linux_runtime.h"
#include "gs_shared_state.h"

namespace fs = std::filesystem;

namespace
{

float targetAspectRatio()
{
    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    if (display_size.x <= 0.0f || display_size.y <= 0.0f)
    {
        return 0.0f;
    }

    switch (s_groundstation_config.screenAspectRatio)
    {
    case ScreenAspectRatio::STRETCH:
        return 0.0f;
    case ScreenAspectRatio::ASPECT5X4:
        return 5.0f / 4.0f;
    case ScreenAspectRatio::ASPECT4X3:
        return 4.0f / 3.0f;
    case ScreenAspectRatio::ASPECT16X9:
        return 16.0f / 9.0f;
    case ScreenAspectRatio::ASPECT16X10:
        return 16.0f / 10.0f;
    case ScreenAspectRatio::LETTERBOX:
    default:
        return display_size.x / display_size.y;
    }
}

}

LinuxOSD g_osd;

//======================================================
//======================================================
LinuxOSD::LinuxOSD()
{
    this->currentFontName[0]=0;
}

//======================================================
//======================================================
void LinuxOSD::init()
{
    this->fontsList = this->getFontsList();
}

//======================================================
//======================================================
void LinuxOSD::loadFont(const char* fontName)
{
    char fileName[1024];
    sprintf( fileName, "../assets_gs/osd_fonts/%s", fontName);
    if (!this->font) delete this->font;
    this->font = new FontWalksnail(fileName);

    strcpy( this->currentFontName, fontName);
}

//======================================================
//======================================================
std::vector<std::string> LinuxOSD::getFontsList()
{
    std::vector<std::string> pngFiles;
    fs::path directoryPath = "../assets_gs/osd_fonts";

    try {
        if (fs::exists(directoryPath) && fs::is_directory(directoryPath)) 
        {
            for (const auto& entry : fs::directory_iterator(directoryPath)) 
            {
                if (entry.is_regular_file() && entry.path().extension() == ".png") 
                {
                    pngFiles.push_back(entry.path().filename().string());
                }
            }
        } 
        else 
        {
            std::cerr << "Directory does not exist or is not a directory: " << directoryPath << std::endl;
        }
    } catch (const fs::filesystem_error& e) 
    {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }

    return pngFiles;
}

//======================================================
//======================================================
void LinuxOSD::draw()
{
    const ImVec2 display_size = ImGui::GetIO().DisplaySize;
    const float video_aspect = s_decoder.isAspect16x9() ? (16.0f / 9.0f) : (4.0f / 3.0f);
    if (s_groundstation_config.vrMode)
    {
        const int half_width = static_cast<int>(display_size.x) / 2;
        OSDBase::drawFittedInRect(0,
                                  0,
                                  half_width,
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
        OSDBase::drawFittedInRect(half_width,
                                  0,
                                  static_cast<int>(display_size.x) - half_width,
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
    }
    else
    {
        OSDBase::drawFittedInRect(0,
                                  0,
                                  static_cast<int>(display_size.x),
                                  static_cast<int>(display_size.y),
                                  video_aspect,
                                  targetAspectRatio());
    }
}

//======================================================
//======================================================
bool LinuxOSD::isFontError()
{
    return !this->font || !this->font->loaded;
}

void LinuxOSD::drawChar(uint16_t code, int x, int y, int width, int height)
{
    if (this->font)
    {
        this->font->drawChar(code, x, y, width, height);
    }
}
