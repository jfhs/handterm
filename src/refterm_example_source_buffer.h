#pragma once
#include "refterm_glyph_cache.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    size_t AbsoluteP;
    size_t Count;
    char *Data;
} source_buffer_range;

typedef struct 
{
    size_t DataSize;
    char *Data;

    // NOTE(casey): For circular buffer
    size_t RelativePoint;
    
    // NOTE(casey): For cache checking
    size_t AbsoluteFilledSize;
} source_buffer;

extern char unsigned DefaultSeed[16];

glyph_hash ComputeGlyphHash(size_t Count, char unsigned* At, char unsigned* Seedx16);
glyph_hash ComputeHashForTileIndex(glyph_hash Tile0Hash, uint32_t TileIndex);

#ifdef __cplusplus
}
#endif
