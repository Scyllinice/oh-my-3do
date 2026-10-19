#ifndef OH3DO_PNGLE_TYPES_H
#define OH3DO_PNGLE_TYPES_H
#include <stddef.h>
#include <limits.h>
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#if UINT_MAX == 0xffffffffU
typedef unsigned int uint32_t;
#else
#error PNG decoder requires 32-bit unsigned int
#endif
typedef unsigned int uint_fast8_t;
typedef int int_fast8_t;
#endif
