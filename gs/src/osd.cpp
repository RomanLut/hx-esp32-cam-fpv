#include <iostream>
#include <filesystem>
#include <vector>
#include <string>


#include "osd.h"
#include "imgui.h"
#include "main.h"

namespace fs = std::filesystem;

DesktopOSD g_osd;

//======================================================
//======================================================
DesktopOSD::DesktopOSD()
{
    this->currentFontName[0]=0;
}

//======================================================
//======================================================
void DesktopOSD::init()
{
    this->fontsList = this->getFontsList();
}

//======================================================
//======================================================
void DesktopOSD::loadFont(const char* fontName)
{
    char fileName[1024];
    sprintf( fileName, "assets/osd_fonts/%s", fontName);
    if (!this->font) delete this->font;
    this->font = new FontWalksnail(fileName);

    strcpy( this->currentFontName, fontName);
}

//======================================================
//======================================================
std::vector<std::string> DesktopOSD::getFontsList()
{
    std::vector<std::string> pngFiles;
    fs::path directoryPath = "assets/osd_fonts";

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
void DesktopOSD::draw()
{
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    int x1, y1, x2, y2;
    calculateLetterBoxAndBorder(displaySize.x, displaySize.y, x1, y1, x2, y2);
    OSDBase::draw(x1, y1, x2, y2);
}

//======================================================
//======================================================
bool DesktopOSD::isFontError()
{
    return !this->font || !this->font->loaded;
}

void DesktopOSD::drawChar(uint16_t code, int x, int y, int width, int height)
{
    if (this->font)
    {
        this->font->drawChar(code, x, y, width, height);
    }
}
