// tinyexr's implementation, built once. ZIP uses the zlib code of the vendored stb_image / stb_image_write
// (compiled in src/core/Image.cpp), so there is no miniz or zlib dependency; no threads, no OpenMP.
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
