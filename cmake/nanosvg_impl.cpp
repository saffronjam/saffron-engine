// The single translation unit that compiles nanosvg + its rasterizer. The renderer
// includes the headers (declarations only) and links this TU.
#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>

#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>
