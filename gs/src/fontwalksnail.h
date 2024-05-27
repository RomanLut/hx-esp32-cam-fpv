#pragma once

 #include <stdint.h>

//======================================================
//======================================================
class FontWalksnail 
{
private:

  uint32_t fontTextureId = 0;


  unsigned int charWidth;
  unsigned int charHeight;

  unsigned int fontTextureWidth;
  unsigned int fontTextureHeight;

  void calculateTextureHeight(unsigned int imageWidth, unsigned int imageHeight);

public:
	FontWalksnail(const char* fileName);
	~FontWalksnail();

  bool loaded;

  void drawChar(uint16_t code, int x1, int y1, int width, int height);

  void drawTest();

  void destroy();
};
