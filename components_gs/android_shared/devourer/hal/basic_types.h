#ifndef BASIC_TYPES_H
#define BASIC_TYPES_H

#include "drv_types.h"

#define SUCCESS 0
#define FAIL (-1)

#ifndef TRUE
#define _TRUE 1
#else
#define _TRUE TRUE
#endif

#ifndef FALSE
#define _FALSE 0
#else
#define _FALSE FALSE
#endif

#define cpu_to_le32(x) ((__u32)(x))
#define le32_to_cpu(x) ((__u32)(x))
#define cpu_to_le16(x) ((__u16)(x))
#define le16_to_cpu(x) ((__u16)(x))


#define LE_BITS_TO_4BYTE(__pStart, __BitOffset, __BitLen)                      \
  ((LE_P4BYTE_TO_HOST_4BYTE(__pStart) >> (__BitOffset)) &                      \
   BIT_LEN_MASK_32(__BitLen))
#define LE_P4BYTE_TO_HOST_4BYTE(__pStart) (le32_to_cpu(*((u32 *)(__pStart))))
#define BIT_LEN_MASK_32(__BitLen) ((u32)(0xFFFFFFFF >> (32 - (__BitLen))))

#define ReadLE2Byte(_ptr) le16_to_cpu(*((u16 *)(_ptr)))

#endif /* BASIC_TYPES_H */
