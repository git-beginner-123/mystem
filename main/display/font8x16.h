#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns pointer to 16 bytes bitmap for ASCII char.
// Each byte is one row, MSB is leftmost pixel.
// Unsupported chars return '?'.
const uint8_t* Font8x16_Get(char c);

#ifdef __cplusplus
}
#endif