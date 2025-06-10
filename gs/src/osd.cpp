#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm> 

#include "osd.h"
#include "imgui.h"
#include "main.h"

namespace fs = std::filesystem;

OSD g_osd;

//======================================================
//======================================================
OSD::OSD()
{
    memset( &this->buffer, 0, OSD_BUFFER_SIZE );
    this->currentFontName[0]=0;
}

//======================================================
//======================================================
void OSD::init()
{
    this->fontsList = this->getFontsList();
}

//======================================================
//======================================================
void OSD::loadFont(const char* fontName)
{
    char fileName[1024];
    sprintf( fileName, "assets/osd_fonts/%s", fontName);
    if (!this->font) delete this->font;
    this->font = new FontWalksnail(fileName);

    strcpy( this->currentFontName, fontName);
}

//======================================================
//======================================================
std::vector<std::string> OSD::getFontsList()
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
void OSD::draw()
{
     ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    int x1, y1, x2, y2;
    calculateLetterBoxAndBorder(displaySize.x, displaySize.y, x1, y1, x2, y2);

    ImVec2 screenSize(x2-x1+1,y2-y1+1);

    float fxs = screenSize.x / OSD_COLS;
    float fys = screenSize.y / OSD_ROWS;

    int ixs = (int) fxs;
    int iys = (int) fys;

    int mx = ((int)screenSize.x - OSD_COLS * ixs) / 2 + x1;
    int my = ((int)screenSize.y - OSD_ROWS * iys) / 2 + y1;

    int y = my;
    for ( int row = 0; row < OSD_ROWS; row++ )
    {
        int x = mx;
        int mask = 1;
        int col8 = 0;
        for ( int col = 0; col < OSD_COLS; col++ )
        {
            uint16_t c = this->buffer.screenLow[row][col];
            if ( (this->buffer.screenHigh[row][col8] & mask) !=0 )
            {
                c += 0x100;
            }
            if ( c != 0 )
            {
                this->font->drawChar(c, x, y, ixs, iys);
            }
            x += ixs;
            mask <<= 1;
            if ( mask == 0x100 )
            {
                mask = 1;
                col8++;
            }
        }
        y += iys;
    }
}

//======================================================
//======================================================
void OSD::update(const uint8_t* pData, uint16_t size)
{
  bool highBank = false;
  int count;

  int osdCol = 0;
  int osdRow = 0;

  int byteCount = 0;
  while (byteCount < size)
  {
    uint8_t c = pData[byteCount++];
    if (c == 0)
    {
      c = pData[byteCount++];
      count = (c & 0x7f);
      if (count == 0)
      {
        break; //stop
      }
      highBank ^= (c & 128) != 0;
      c = pData[byteCount++];
    }
    else if (c == 255)
    {
      highBank = !highBank;
      c = pData[byteCount++];
      count = 1;
    }
    else
    {
      count = 1;
    }

    while (count > 0)
    {
      this->buffer.screenLow[osdRow][osdCol] = c;
      int col8 = osdCol >> 3;
      int sh = osdCol & 0x7;
      uint8_t m = 1 << sh;
      this->buffer.screenHigh[osdRow][col8] = (this->buffer.screenHigh[osdRow][col8] & ~m) | (highBank ? m : 0 );
      osdCol++;
      if (osdCol == OSD_COLS)
      {
        osdCol = 0;
        osdRow++;
        if (osdRow == OSD_ROWS)
        {
          osdRow = 0;
        }
      }
      count--;
    }
  }
}

//======================================================
//======================================================
bool OSD::isFontError()
{
    return !this->font || !this->font->loaded;
}
