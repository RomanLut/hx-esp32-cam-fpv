 #pragma once

#include <unistd.h>
#include <cstdint>  

#define AVI_HEADER_LEN 240 // AVI header length
#define CHUNK_HDR 8 // bytes per jpeg hdr in AVI 
#define DVR_MAX_FRAMES  25000   //50fps ~ 8 minues


extern const uint8_t dcBuf[]; // 00dc
extern size_t moviSize;

extern uint8_t aviHeader[AVI_HEADER_LEN];

extern void prepAviBuffers();
extern void prepAviIndex();
extern void finalizeAviIndex(uint16_t frameCnt);
extern size_t writeAviIndex(uint8_t* clientBuf, size_t buffSize);
extern void buildAviHdr(uint8_t FPS, int frameWidth, int frameHeight, uint16_t frameCnt);
extern void buildAviIdx(size_t dataSize);



