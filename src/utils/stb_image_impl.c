/*
 * STB Image Implementation
 * Single-header library for image loading (PNG, JPEG, BMP, etc.)
 * Windows-compatible configuration
 */

/* Must be defined before including stb_image.h */
#define STB_IMAGE_IMPLEMENTATION

/* Only include formats we need */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG  
#define STBI_ONLY_BMP

/* Disable formats we don't need */
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_LINEAR

/* Windows compatibility */
#ifdef _WIN32
    #define STBI_WINDOWS_UTF8
#endif

/* Use standard library functions */
#define STBI_MALLOC(sz) malloc(sz)
#define STBI_REALLOC(p, newsz) realloc(p, newsz)
#define STBI_FREE(p) free(p)

#include <stdlib.h>
#include "stb_image.h"
