#ifndef TERRA_ALIGNMENT_H
#define TERRA_ALIGNMENT_H
#include <stddef.h>

/* MSVC's C headers do not provide max_align_t. Match the CRT's fundamental
 * malloc alignment: 16 bytes for 64-bit Windows, 8 bytes for 32-bit Windows.
 * https://learn.microsoft.com/cpp/c-runtime-library/reference/malloc
 * The header preceding each owned payload must preserve this alignment. */
#if defined(_MSC_VER) && !defined(__cplusplus)
#if defined(_WIN64)
typedef __declspec(align(16)) struct TerraMaxAlign { unsigned char bytes[16]; } TerraMaxAlign;
#else
typedef __declspec(align(8)) struct TerraMaxAlign { unsigned char bytes[8]; } TerraMaxAlign;
#endif
#else
typedef max_align_t TerraMaxAlign;
#endif
#endif
