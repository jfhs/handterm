#pragma once

#include <stdint.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    uint32_t CellSize[2];
    uint32_t TermSize[2];
    uint32_t TopLeftMargin[2];
    uint32_t BlinkModulate;
    uint32_t MarginColor;
    uint32_t StrikeMin;
    uint32_t StrikeMax;
    uint32_t UnderlineMin;
    uint32_t UnderlineMax;
} renderer_const_buffer;

#define RENDERER_CELL_BLINK 0x80000000
typedef struct
{
    uint32_t GlyphIndex;
    uint32_t Foreground;
    uint32_t Background; // NOTE(casey): The top bit of the background flag indicates blinking
} renderer_cell;

typedef struct
{
    ID3D11Device *Device;
    ID3D11DeviceContext *DeviceContext;
    ID3D11DeviceContext1 *DeviceContext1;

    IDXGISwapChain2 *SwapChain;
    HANDLE FrameLatencyWaitableObject;

    ID3D11ComputeShader *ComputeShader;
    ID3D11PixelShader *PixelShader;
    ID3D11VertexShader *VertexShader;

    ID3D11Buffer *ConstantBuffer;
    ID3D11RenderTargetView *RenderTarget;
    ID3D11UnorderedAccessView *RenderView;

    ID3D11Buffer *CellBuffer;
    ID3D11ShaderResourceView *CellView;

    ID3D11Texture2D *GlyphTexture;
    ID3D11ShaderResourceView *GlyphTextureView;

    ID3D11Texture2D *GlyphTransfer;
    ID3D11ShaderResourceView *GlyphTransferView;
    IDXGISurface *GlyphTransferSurface;

    // NOTE(casey): These are for DirectWrite
    struct ID2D1RenderTarget *DWriteRenderTarget;
    struct ID2D1SolidColorBrush *DWriteFillBrush;

    uint32_t CurrentWidth;
    uint32_t CurrentHeight;
    uint32_t MaxCellCount;

    int UseComputeShader;
} d3d11_renderer;

typedef struct TermOutputBuffer TermOutputBuffer;

d3d11_renderer AcquireD3D11Renderer(HWND Window, int EnableDebugging);

void SetD3D11MaxCellCount(d3d11_renderer *Renderer, uint32_t Count);
void SetD3D11GlyphCacheDim(d3d11_renderer *Renderer, uint32_t Width, uint32_t Height);
void SetD3D11GlyphTransferDim(d3d11_renderer* Renderer, uint32_t Width, uint32_t Height);
void RendererDraw(TermOutputBuffer* Terminal, uint32_t Width, uint32_t Height, uint32_t BlinkModulate);

#ifdef __cplusplus
}
#endif