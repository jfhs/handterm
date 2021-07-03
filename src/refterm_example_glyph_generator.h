#pragma once

#include "refterm_glyph_cache.h"
#include "refterm_example_d3d11.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    GlyphState_None,
    GlyphState_Sized,
    GlyphState_Rasterized,
} glyph_entry_state;

typedef enum
{
    GlyphGen_UseClearType = 0x1, // TODO(casey): This is not yet supported in the DirectWrite path
    GlyphGen_UseDirectWrite = 0x2,
} glyph_generation_flag;

typedef struct glyph_generator glyph_generator;
typedef struct glyph_dim glyph_dim;

struct glyph_dim
{
    uint32_t TileCount;
    float XScale, YScale;
};

struct glyph_generator
{
    uint32_t FontWidth, FontHeight;
    uint32_t Pitch;
    uint32_t *Pixels;
    
    uint32_t TransferWidth;
    uint32_t TransferHeight;
    
    uint32_t UseClearType;
    uint32_t UseDWrite;

    // NOTE(casey): For GDI-based generation:
    HDC DC;
    HFONT OldFont, Font;
    HBITMAP Bitmap;
    
    // NOTE(casey): For DWrite-based generation:
    struct IDWriteFactory *DWriteFactory;
    struct IDWriteFontFace *FontFace;
    struct IDWriteTextFormat *TextFormat;
};

glyph_generator AllocateGlyphGenerator(uint32_t TransferWidth, uint32_t TransferHeight,
    IDXGISurface* GlyphTransferSurface);
int SetFont(glyph_generator* GlyphGen, wchar_t* FontName, uint32_t FontHeight);
uint32_t GetExpectedTileCountForDimension(glyph_generator* GlyphGen, uint32_t Width, uint32_t Height);
void PrepareTilesForTransfer(glyph_generator* GlyphGen, d3d11_renderer* Renderer, size_t Count, wchar_t* String, glyph_dim Dim);
void TransferTile(glyph_generator* GlyphGen, d3d11_renderer* Renderer, uint32_t TileIndex, gpu_glyph_index DestIndex);
glyph_dim GetGlyphDim(glyph_generator* GlyphGen, glyph_table* Table, size_t Count, wchar_t* String, glyph_hash RunHash);
glyph_dim GetSingleTileUnitDim(void);

#ifdef __cplusplus
}
#endif