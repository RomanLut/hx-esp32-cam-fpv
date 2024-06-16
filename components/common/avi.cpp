#include "avi.h"

#include <cstring>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#endif

/* AVI file format:
header:
 310 bytes
per jpeg:
 4 byte 00dc marker
 4 byte jpeg size
 jpeg frame content
0-3 bytes filler to align on DWORD boundary
per PCM (audio file)
 4 byte 01wb marker
 4 byte pcm size
 pcm content
 0-3 bytes filler to align on DWORD boundary
footer:
 4 byte idx1 marker
 4 byte index size
 per jpeg:
  4 byte 00dc marker
  4 byte 0000
  4 byte jpeg location
  4 byte jpeg size
 per pcm:
  4 byte 01wb marker
  4 byte 0000
  4 byte pcm location
  4 byte pcm size
*/

#include "avi.h"

// avi header data
const uint8_t dcBuf[4] = {0x30, 0x30, 0x64, 0x63};   // 00dc
const uint8_t wbBuf[4] = {0x30, 0x31, 0x77, 0x62};   // 01wb
static const uint8_t idx1Buf[4] = {0x69, 0x64, 0x78, 0x31}; // idx1
static const uint8_t zeroBuf[4] = {0x00, 0x00, 0x00, 0x00}; // 0000
static uint8_t* idxBuf;

uint8_t aviHeader[AVI_HEADER_LEN] = { // AVI header template
  0x52, 0x49, 0x46, 0x46, 0xD8, 0x01, 0x0E, 0x00, 0x41, 0x56, 0x49, 0x20, 0x4C, 0x49, 0x53, 0x54,
  0xD0, 0x00, 0x00, 0x00, 0x68, 0x64, 0x72, 0x6C, 0x61, 0x76, 0x69, 0x68, 0x38, 0x00, 0x00, 0x00,
  0xA0, 0x86, 0x01, 0x00, 0x80, 0x66, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x49, 0x53, 0x54, 0x84, 0x00, 0x00, 0x00,
  0x73, 0x74, 0x72, 0x6C, 0x73, 0x74, 0x72, 0x68, 0x30, 0x00, 0x00, 0x00, 0x76, 0x69, 0x64, 0x73,
  0x4D, 0x4A, 0x50, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x74, 0x72, 0x66,
  0x28, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00,
  0x01, 0x00, 0x18, 0x00, 0x4D, 0x4A, 0x50, 0x47, 0x00, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x4E, 0x46, 0x4F,
  0x10, 0x00, 0x00, 0x00,  'h',  'x',  '-',  'e',  's',  'p',  '3',  '2',  '-',  'c',  'a',  'm',
   '-',  'f',  'p',  'v', 0x4C, 0x49, 0x53, 0x54, 0x00, 0x01, 0x0E, 0x00, 0x6D, 0x6F, 0x76, 0x69,
};

struct frameSizeStruct 
{
  uint8_t frameWidth[2];
  uint8_t frameHeight[2];
  uint8_t frameHeight16x9[2];
};

#define IDX_ENTRY 16 // bytes per index entry

static size_t idxPtr;
static size_t idxOffset;
size_t moviSize;
static size_t indexLen;


//=============================================================================================
//=============================================================================================
void prepAviBuffers() 
{
#ifdef ESP_PLATFORM
  idxBuf = (uint8_t*)heap_caps_malloc((DVR_MAX_FRAMES+1)*IDX_ENTRY, MALLOC_CAP_SPIRAM);
  if ( !idxBuf )
  {
    ESP_LOGE("AVI","Unable to allocate AVI buffer");
  }
#else  
  idxBuf = (uint8_t*)malloc((DVR_MAX_FRAMES+1)*IDX_ENTRY);
#endif  
}

//=============================================================================================
//=============================================================================================
void prepAviIndex() 
{
  memcpy(idxBuf, idx1Buf, 4); // index header
  idxPtr = CHUNK_HDR;  // leave 4 bytes for index size
  moviSize = indexLen = 0;
}

//=============================================================================================
//=============================================================================================
void buildAviHdr(uint8_t FPS, int frameWidth, int frameHeight, uint16_t frameCnt ) 
{
  // update AVI header template with file specific details
  size_t aviSize = moviSize + AVI_HEADER_LEN + (CHUNK_HDR+IDX_ENTRY) * frameCnt + 8 - 8; // AVI content size excluding riff header (+8 idx header -8 riff header)
  // update aviHeader with relevant stats

/*
  ESP_LOGE("AVI","moviSize %d\n", moviSize);
  ESP_LOGE("AVI","Filesize %d\n", aviSize);
  ESP_LOGE("AVI","Framecount %d\n", frameCnt);
*/
  memcpy(aviHeader+4, &aviSize, 4);
  uint32_t usecs = (uint32_t)round(1000000.0f / FPS); // usecs_per_frame 
  memcpy(aviHeader+0x20, &usecs, 4); 
  memcpy(aviHeader+0x30, &frameCnt, 2);
  memcpy(aviHeader+0x8C, &frameCnt, 2);
  memcpy(aviHeader+0x84, &FPS, 1);

  uint32_t dataSize = moviSize + frameCnt*CHUNK_HDR + 4; 
  memcpy(aviHeader+0xe8, &dataSize, 4);

  // apply video framesize to avi header
  memcpy(aviHeader+0x40, &frameWidth, 2);
  memcpy(aviHeader+0xA8, &frameWidth, 2);
  memcpy(aviHeader+0x44, &frameHeight, 2);
  memcpy(aviHeader+0xAC, &frameHeight, 2);

  // reset state for next recording
  moviSize = idxOffset = idxPtr = 0;
}

//=============================================================================================
//=============================================================================================
void buildAviIdx(size_t dataSize) 
{
  // build AVI video index into buffer - 16 bytes per frame
  // called from saveFrame() for each frame
  moviSize += dataSize;
  memcpy(idxBuf+idxPtr, dcBuf, 4);
  memcpy(idxBuf+idxPtr+4, zeroBuf, 4);
  memcpy(idxBuf+idxPtr+8, &idxOffset, 4); 
  memcpy(idxBuf+idxPtr+12, &dataSize, 4); 
  idxOffset += dataSize + CHUNK_HDR;
  idxPtr += IDX_ENTRY; 
}

//=============================================================================================
//=============================================================================================
size_t writeAviIndex(uint8_t* clientBuf, size_t buffSize) 
{
  // write completed index to avi file
  // called repeatedly from closeAvi() until return 0
  if (idxPtr < indexLen) 
  {
    if ((indexLen-idxPtr) > buffSize) 
    {
      memcpy(clientBuf, idxBuf+idxPtr, buffSize);
      idxPtr += buffSize;
      return buffSize;
    } 
    else 
    {
      // final part of index
      size_t finalPart = indexLen-idxPtr;
      memcpy(clientBuf, idxBuf+idxPtr, finalPart);
      idxPtr = indexLen;
      return finalPart;
    }
  }
  return 0;
}
  
//=============================================================================================
//=============================================================================================
void finalizeAviIndex(uint16_t frameCnt) 
{
  // update index with size
  uint32_t sizeOfIndex = frameCnt*IDX_ENTRY;
  memcpy(idxBuf+4, &sizeOfIndex, 4); // size of index 
  indexLen = sizeOfIndex + CHUNK_HDR;
  idxPtr = 0; // prepare for writeAviIndex()
}


