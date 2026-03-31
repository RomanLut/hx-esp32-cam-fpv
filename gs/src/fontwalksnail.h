#pragma once
 
#include <cstddef>
 #include <stdint.h>
#include <vector>

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
	FontWalksnail(const void* pngData, size_t pngSize);
	~FontWalksnail();

  bool loaded;

  void drawChar(uint16_t code, int x1, int y1, int width, int height);

  void drawTest();

  void destroy();

private:
  void initFromDecodedImage(unsigned char* image,
                            unsigned width,
                            unsigned height,
                            const char* sourceName);
};
