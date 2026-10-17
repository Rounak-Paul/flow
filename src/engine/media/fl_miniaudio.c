/* Compiles the vendored miniaudio and stb_vorbis implementations exactly once. */
#include "fl_miniaudio.h"
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
