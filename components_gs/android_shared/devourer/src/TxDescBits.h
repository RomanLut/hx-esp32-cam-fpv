#ifndef DEVOURER_TX_DESC_BITS_H
#define DEVOURER_TX_DESC_BITS_H

/* Little-endian TX-descriptor bit-field write helpers, shared across chip
 * generations (Jaguar1 FrameParser SET_TX_DESC_*_8812 macros and Jaguar3
 * FrameParserJaguar3 SET_TX_DESC_*_8822C macros all build on
 * SET_BITS_TO_LE_4BYTE). Kept neutral so neither generation depends on the
 * other's parser header. The cpu_to_le and le32_to_cpu helpers used by the macro
 * bodies come from hal/basic_types.h at the expansion site (as before). */

typedef unsigned int __u32;

#ifndef __u16
typedef unsigned short __u16;
#endif

typedef signed char s8;
typedef unsigned char u8;

typedef signed short s16;
typedef unsigned short u16;

typedef signed int s32;
typedef unsigned int u32;

#define WriteLE4Byte(_ptr, _val) ((*((u32*)(_ptr))) = cpu_to_le32(_val))
#define WriteLE2Byte(_ptr, _val) ((*((u16*)(_ptr))) = cpu_to_le16(_val))
#define WriteLE1Byte(_ptr, _val) ((*((u8*)(_ptr))) = ((u8)(_val)))

#ifndef BIT_LEN_MASK_32
#define BIT_LEN_MASK_32(__BitLen) ((u32)(0xFFFFFFFF >> (32 - (__BitLen))))
#endif

#ifndef LE_P4BYTE_TO_HOST_4BYTE
#define LE_P4BYTE_TO_HOST_4BYTE(__pStart) (le32_to_cpu(*((u32*)(__pStart))))
#endif

#define BIT_OFFSET_LEN_MASK_32(__BitOffset, __BitLen) ((u32)(BIT_LEN_MASK_32(__BitLen) << (__BitOffset)))

#define LE_BITS_CLEARED_TO_4BYTE(__pStart, __BitOffset, __BitLen)                                                      \
    (LE_P4BYTE_TO_HOST_4BYTE(__pStart) & (~BIT_OFFSET_LEN_MASK_32(__BitOffset, __BitLen)))

#define SET_BITS_TO_LE_4BYTE(__pStart, __BitOffset, __BitLen, __Value)                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (__BitOffset == 0 && __BitLen == 32)                                                                        \
            WriteLE4Byte(__pStart, __Value);                                                                           \
        else                                                                                                           \
        {                                                                                                              \
            WriteLE4Byte(__pStart,                                                                                     \
                         LE_BITS_CLEARED_TO_4BYTE(__pStart, __BitOffset, __BitLen) |                                   \
                             ((((u32)__Value) & BIT_LEN_MASK_32(__BitLen)) << (__BitOffset)));                         \
        }                                                                                                              \
    } while (0)

#endif /* DEVOURER_TX_DESC_BITS_H */
