#pragma once
/* Single configuration point for miniaudio: device output and decoding only. */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_THREADING_DEBUG
#if defined(__APPLE__)
#define MA_NO_RUNTIME_LINKING
#endif
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#include <miniaudio.h>
