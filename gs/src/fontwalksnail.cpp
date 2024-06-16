#include "main.h"

#include "lodepng.h"

#include "fontwalksnail.h"
#include "Log.h"
#include "util.h"

#include "imgui.h"

extern "C"
{
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
}

#define OSD_CHAR_WIDTH_24 24
#define OSD_CHAR_HEIGHT_24 36

#define OSD_CHAR_WIDTH_36 36
#define OSD_CHAR_HEIGHT_36 54

#define CHARS_PER_TEXTURE_ROW 14

//======================================================
//======================================================
FontWalksnail::FontWalksnail(const char* fileName)
{
  this->loaded = false;

  unsigned char* image = 0;
  unsigned width, height;

  unsigned int error = lodepng_decode32_file(&image, &width, &height, fileName);
  if (error)
  {
    LOGE("error {}: {}\n", error, lodepng_error_text(error));
    return;
  }

  if ((width != OSD_CHAR_WIDTH_24) && (width != OSD_CHAR_WIDTH_36))
  {
    LOGE("Unexpected image size: {}\n", fileName);
    return;
  }

  this->charWidth = width;
  this->charHeight = height / 512;

  int charsCount = 512;

  if ( this->charHeight != OSD_CHAR_HEIGHT_24 )
  {
    this->charHeight = height / 256;
    charsCount = 256;
  }

  if ((this->charWidth == OSD_CHAR_WIDTH_24) && (this->charHeight != OSD_CHAR_HEIGHT_24))
  {
    LOGE("Unexpected image size: {}\n", fileName);
    return;
  }

  this->calculateTextureHeight(this->charWidth * CHARS_PER_TEXTURE_ROW, this->charHeight * (512 + CHARS_PER_TEXTURE_ROW - 1) / CHARS_PER_TEXTURE_ROW);

  uint8_t* buffer = new uint8_t[this->fontTextureWidth * this->fontTextureHeight * 4];

  memset((void*)buffer, 0, this->fontTextureWidth * this->fontTextureHeight * 4);

  for (int charIndex = 0; charIndex < charsCount; charIndex++)
  {
    int ix = 0;
    int iy = charIndex * this->charHeight;

    int tx = (charIndex % CHARS_PER_TEXTURE_ROW) * this->charWidth;
    int ty = (charIndex / CHARS_PER_TEXTURE_ROW) * this->charHeight;

    for (unsigned int y = 0; y < this->charHeight; y++)
    {
      const uint8_t* pi = image + (iy+y) * width * 4 + ix * 4;
      uint8_t* pt = buffer + (ty+y) * this->fontTextureWidth * 4 + tx * 4;

      for (unsigned int x = 0; x < this->charWidth; x++)
      {
        pt[0] = pi[0];
        pt[1] = pi[1];
        pt[2] = pi[2];
        pt[3] = pi[3];

        pi += 4;
        pt += 4;
      }
    }
    this->loaded = true;
  }

/*  
  char fname[1000];
  strcpy(fname, fileName);
  strcpy(fname + strlen(fname)-4 , "_texture.png");
  lodepng_encode_file(fname, buffer, this->fontTextureWidth, this->fontTextureHeight, LCT_RGBA, 8);
*/ 

  GLCHK(glGenTextures(1, &this->fontTextureId));
  GLCHK(glBindTexture(GL_TEXTURE_2D, this->fontTextureId));
  GLCHK(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
  GLCHK(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
  GLCHK(glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
  GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
  GLCHK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
  LOGI("Texture: {}", this->fontTextureId);

  GLCHK(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, this->fontTextureWidth, this->fontTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer));

  delete[] buffer;

  free(image);
}


//======================================================
//======================================================
FontWalksnail::~FontWalksnail()
{
  GLCHK(glBindTexture(GL_TEXTURE_2D, 0)); 
  GLCHK(glDeleteTextures(1, &this->fontTextureId));
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
    draw_list->AddImageQuad((ImTextureID)this->fontTextureId, pos[0], pos[1], pos[2], pos[3], uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
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
        glDeleteTextures(1,&this->fontTextureId);
    }
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

    draw_list->AddImageQuad((ImTextureID)this->fontTextureId, pos[0], pos[1], pos[2], pos[3], uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
}

