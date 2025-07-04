 #pragma once

#include <unistd.h>
#include <cstdint>  

#define AVI_HEADER_LEN 240 // AVI header length
#define CHUNK_HDR 8 // bytes per jpeg hdr in AVI 
#define DVR_MAX_FRAMES  25000   //50fps ~ 8 minues

#pragma pack(push, 1) // exact fit - no padding

typedef struct {
    uint32_t dwMicroSecPerFrame;    // offset: 0x20
    uint32_t dwMaxBytesPerSec;      // offset: 0x24
    uint32_t dwPaddingGranularity;  // offset: 0x28
    uint32_t dwFlags;               // offset: 0x2C
    uint32_t dwTotalFrames;         // offset: 0x30
    uint32_t dwInitialFrames;       // offset: 0x34
    uint32_t dwStreams;             // offset: 0x38
    uint32_t dwSuggestedBufferSize; // offset: 0x3C
    uint32_t dwWidth;               // offset: 0x40
    uint32_t dwHeight;              // offset: 0x44
    uint32_t dwReserved[4];         // offset: 0x48, 0x4C, 0x50, 0x54
} avi_main_header_t;


typedef struct {
    uint32_t fcc;
    uint32_t cb;
} avi_chunk_t;

typedef struct {
    uint32_t fcc;       // offset: 0xD4 // 'LIST'
    uint32_t cb;        // offset: 0xD8 // размер списка
    uint32_t fccType;   // offset: 0xDC // 'movi'
    // JPEG chunks follow, starting with 8 byte markers  offset: e0
} avi_movi_list_t;

typedef struct {
    avi_chunk_t riff;               // offset: 0x00
    uint32_t file_type;             // offset: 0x08 // 'AVI '
    avi_chunk_t list_hdrl;          // offset: 0x0C
    uint32_t list_type_hdrl;        // offset: 0x14 // 'hdrl'
    avi_chunk_t avih_chunk;         // offset: 0x18
    avi_main_header_t main_header;  // offset: 0x20 (size 0x38)
    avi_chunk_t list_strl;          // offset: 0x58
    uint32_t list_type_strl;        // offset: 0x60 // 'strl'
    avi_chunk_t strh_chunk;         // offset: 0x64
    // strh_chunk_data
    uint32_t fccType;               // offset: 0x6C
    uint32_t fccHandler;            // offset: 0x70
    uint32_t dwFlags;               // offset: 0x74
    uint16_t wPriority;             // offset: 0x78
    uint16_t wLanguage;             // offset: 0x7A
    uint32_t dwInitialFrames;       // offset: 0x7C
    uint32_t dwScale;               // offset: 0x80
    uint32_t dwRate;                // offset: 0x84
    uint32_t dwStart;               // offset: 0x88
    uint32_t dwLength;              // offset: 0x8C
    uint32_t dwSuggestedBufferSize; // offset: 0x90
    uint32_t dwQuality;             // offset: 0x94
    uint32_t dwSampleSize;          // offset: 0x98
    struct {                        // offset: 0x9C //fixme: whole structure is missing in binary avi header
        uint16_t left;              // offset: 0x9C  
        uint16_t top;               // offset: 0x9E
        uint16_t right;             // offset: 0xA0
        uint16_t bottom;            // offset: 0xA2
    } rcFrame;
    avi_chunk_t strf_chunk;         // offset: 0xA4
    // BITMAPINFOHEADER
    uint32_t biSize;                // offset: 0xAC
    int32_t  biWidth;               // offset: 0xB0
    int32_t  biHeight;              // offset: 0xB4
    uint16_t biPlanes;              // offset: 0xB8
    uint16_t biBitCount;            // offset: 0xBA
    uint32_t biCompression;         // offset: 0xBC
    uint32_t biSizeImage;           // offset: 0xC0
    int32_t  biXPelsPerMeter;       // offset: 0xC4
    int32_t  biYPelsPerMeter;       // offset: 0xC8
    uint32_t biClrUsed;             // offset: 0xCC
    uint32_t biClrImportant;        // offset: 0xD0
    //offset: 0xD4 info

    //avi_movi_list_t movi;           // offset: 0xEC
    //offset 0xE0
} avi_header_t;



typedef struct { 
    uint32_t fcc;      // '00dc' or '01wb'
    uint32_t dwFlags;  // 0
    uint32_t dwOffset; // offset of the chunk from the start of 'movi' list
    uint32_t dwSize;   // size of the chunk
} avi_idx_entry_t;

#pragma pack(pop)


extern const uint8_t dcBuf[]; // 00dc
//extern const uint8_t wbBuf[]; // 01wb
extern size_t moviSize;

extern uint8_t aviHeader[AVI_HEADER_LEN];

extern void prepAviBuffers();
extern void prepAviIndex();
extern void finalizeAviIndex(uint16_t frameCnt);
extern size_t writeAviIndex(uint8_t* clientBuf, size_t buffSize);
extern void buildAviHdr(uint8_t FPS, int frameWidth, int frameHeight, uint16_t frameCnt);
extern void buildAviIdx(size_t dataSize);
