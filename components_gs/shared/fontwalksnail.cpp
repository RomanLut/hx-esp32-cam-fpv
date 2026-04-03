#include "fontwalksnail.h"

#include "imgui.h"
#include "lodepng.h"
#include "util.h"

#include <cstdio>
#include <GLES2/gl2.h>

#include <cstring>

#define OSD_CHAR_WIDTH_24 24
#define OSD_CHAR_HEIGHT_24 36

#define OSD_CHAR_WIDTH_36 36
#define OSD_CHAR_HEIGHT_36 54

#define CHARS_PER_TEXTURE_ROW 14

namespace
{

void reportFontError(const char* message, const char* detail)
{
  std::fprintf(stderr, "FontWalksnail: %s (%s)\n", message, detail != nullptr ? detail : "");
}

void reportFontError(const char* message, unsigned int detail)
{
  std::fprintf(stderr, "FontWalksnail: %s (%u)\n", message, detail);
}

}

//======================================================
//======================================================
FontWalksnail::FontWalksnail(const char* fileName)
{
  fontTextureId = 0;
  loaded = false;

  unsigned char* image = 0;
  unsigned width, height;

  unsigned int error = lodepng_decode32_file(&image, &width, &height, fileName);
  if (error)
  {
    reportFontError(lodepng_error_text(error), error);
    return;
  }

  initFromDecodedImage(image, width, height, fileName);
  free(image);
}

FontWalksnail::FontWalksnail(const void* pngData, size_t pngSize)
{
  fontTextureId = 0;
  loaded = false;

  unsigned char* image = 0;
  unsigned width = 0;
  unsigned height = 0;
  unsigned int error = lodepng_decode32(&image,
                                        &width,
                                        &height,
                                        static_cast<const unsigned char*>(pngData),
                                        pngSize);
  if (error)
  {
    reportFontError(lodepng_error_text(error), error);
    return;
  }

  initFromDecodedImage(image, width, height, "<memory>");
  free(image);
}


//======================================================
//======================================================
FontWalksnail::~FontWalksnail()
{
  destroy();
}

//======================================================
//======================================================
void FontWalksnail::drawChar(uint16_t code, int x1, int y1, int width, int height)
{
  if (this->fontTextureId == 0) return;

  int px = (code % CHARS_PER_TEXTURE_ROW) * this->charWidth;
  int py = (code / CHARS_PER_TEXTURE_ROW) * this->charHeight;

  float u1 = (float)px;
  float u2 = u1 + this->charWidth;

  float v1 = (float)py;
  float v2 = v1 + this->charHeight;

  u1 /= this->fontTextureWidth;
  v1 /= this->fontTextureHeight;
  u2 /= this->fontTextureWidth;
  v2 /= this->fontTextureHeight;

  float x2 = x1 + width;
  float y2 = y1 + height;

    ImVec2 pos[4] =
        {
            ImVec2(x1,y1),ImVec2(x2,y1), ImVec2(x2,y2),ImVec2(x1,y2)
        };

    ImVec2 uvs[4] =
        {
            ImVec2(u1,v1),ImVec2(u2,v1), ImVec2(u2,v2),ImVec2(u1,v2)
        };

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(this->fontTextureId), pos[0], pos[1], pos[2], pos[3], uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
}

//======================================================
//======================================================
void FontWalksnail::calculateTextureHeight(unsigned int imageWidth, unsigned int imageHeight)
{
  this->fontTextureWidth = smallestPowerOfTwo( imageWidth, 8 );
  this->fontTextureHeight = smallestPowerOfTwo( imageHeight, 8 );
}

//======================================================
//======================================================
void FontWalksnail::destroy()
{
    if(this->fontTextureId != 0)
    {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1,&this->fontTextureId);
        this->fontTextureId = 0;
    }
}

void FontWalksnail::initFromDecodedImage(unsigned char* image,
                                         unsigned width,
                                         unsigned height,
                                         const char* sourceName)
{
  if ((width != OSD_CHAR_WIDTH_24) && (width != OSD_CHAR_WIDTH_36))
  {
    reportFontError("Unexpected image size", sourceName);
    return;
  }

  this->charWidth = width;
  this->charHeight = height / 512;

  int charsCount = 512;
  if (this->charHeight != OSD_CHAR_HEIGHT_24)
  {
    this->charHeight = height / 256;
    charsCount = 256;
  }

  if ((this->charWidth == OSD_CHAR_WIDTH_24 && this->charHeight != OSD_CHAR_HEIGHT_24) ||
      (this->charWidth == OSD_CHAR_WIDTH_36 && this->charHeight != OSD_CHAR_HEIGHT_36))
  {
    reportFontError("Unexpected image size", sourceName);
    return;
  }

  this->calculateTextureHeight(this->charWidth * CHARS_PER_TEXTURE_ROW,
                               this->charHeight * (charsCount + CHARS_PER_TEXTURE_ROW - 1) / CHARS_PER_TEXTURE_ROW);

  std::vector<uint8_t> buffer(static_cast<size_t>(this->fontTextureWidth) * this->fontTextureHeight * 4u, 0);

  for (int charIndex = 0; charIndex < charsCount; charIndex++)
  {
    const int iy = charIndex * this->charHeight;
    const int tx = (charIndex % CHARS_PER_TEXTURE_ROW) * this->charWidth;
    const int ty = (charIndex / CHARS_PER_TEXTURE_ROW) * this->charHeight;

    for (unsigned int y = 0; y < this->charHeight; y++)
    {
      const uint8_t* pi = image + (iy + y) * width * 4u;
      uint8_t* pt = buffer.data() + (static_cast<size_t>(ty) + y) * this->fontTextureWidth * 4u + static_cast<size_t>(tx) * 4u;

      for (unsigned int x = 0; x < this->charWidth; x++)
      {
        const uint8_t* src = pi + x * 4u;
        uint8_t* dst = pt + x * 4u;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
      }
    }
  }

  glGenTextures(1, &this->fontTextureId);
  glBindTexture(GL_TEXTURE_2D, this->fontTextureId);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               this->fontTextureWidth,
               this->fontTextureHeight,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               buffer.data());

  loaded = (this->fontTextureId != 0);
}


//======================================================
//======================================================
void FontWalksnail::drawTest()
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImVec2 pos[4] =
        {
            ImVec2(0,0),ImVec2(500,0), ImVec2(500,500),ImVec2(0,500)
        };

    ImVec2 uvs[4] =
        {
            ImVec2(0,0),ImVec2(1,0), ImVec2(1,1),ImVec2(0,1)
        };

    draw_list->AddImageQuad(reinterpret_cast<ImTextureID>(this->fontTextureId), pos[0], pos[1], pos[2], pos[3], uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
}
