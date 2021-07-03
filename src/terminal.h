#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <usp10.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include "refterm_example_d3d11.h"
#include "refterm_example_glyph_generator.h"
#include "refterm_glyph_cache.h"
#include "vtparser.h"

enum
{
    TerminalCell_Bold = 0x2,
    TerminalCell_Dim = 0x4,
    TerminalCell_Italic = 0x8,
    TerminalCell_Underline = 0x10,
    TerminalCell_Blinking = 0x20,
    TerminalCell_ReverseVideo = 0x40,
    TerminalCell_Invisible = 0x80,
    TerminalCell_Strikethrough = 0x100,
};

typedef struct
{
    renderer_cell* Cells;
    uint32_t DimX, DimY;
    uint32_t FirstLineY;
} terminal_buffer;

typedef struct
{
    int32_t X, Y;
} terminal_point;

typedef struct
{
    uint32_t Foreground;
    uint32_t Background;
    uint32_t Flags;
} glyph_props;

typedef struct
{
    terminal_point At;
    glyph_props Props;
} cursor_state;

typedef struct
{
    // TODO(casey): Get rid of Uniscribe so this garbage doesn't have to happen

    SCRIPT_DIGITSUBSTITUTE UniDigiSub;
    SCRIPT_CONTROL UniControl;
    SCRIPT_STATE UniState;
    SCRIPT_CACHE UniCache;

    wchar_t Expansion[1024];
    SCRIPT_ITEM Items[1024];
    SCRIPT_LOGATTR Log[1024];
    DWORD SegP[1026];
} example_partitioner;

typedef struct
{
    size_t FirstP;
    size_t OnePastLastP;
    uint32_t ContainsComplexChars;
    glyph_props StartingProps;
} example_line;

typedef struct
{
    HWND Window;
    int Quit;

    terminal_buffer ScreenBuffer;

    HANDLE Legacy_WriteStdIn;
    HANDLE Legacy_ReadStdOut;
    HANDLE Legacy_ReadStdError;

    int EnableFastPipe;
    HANDLE FastPipe;

    HANDLE ChildProcess;

    cursor_state RunningCursor;

    uint32_t CommandLineCount;
    char CommandLine[256];

    int DebugHighlighting;

    uint32_t MaxLineCount;
    uint32_t CurrentLineIndex;
    uint32_t LineCount;
    example_line* Lines;

    int32_t ViewingLineOffset;

    wchar_t RequestedFontName[64];
    uint32_t RequestedFontHeight;
    int RequestClearType;
    int RequestDirectWrite;
    int LineWrap;

} example_terminal;


typedef struct CharAttributes {
    bool bold : 1;
    bool soft_wrap : 1;
    bool complex : 1;
    bool wrappoint : 1;
    int pad0 : 4;
    uint32_t color : 24;

    uint32_t backg : 32;
} CharAttributes;

typedef struct ComplexChar ComplexChar;

struct ComplexChar {
    union {
        wchar_t* start;
        ComplexChar* next_free;
    };
    size_t len;
    glyph_hash hash;
    glyph_dim dim;
};

typedef struct TermChar {
    union {
        wchar_t ch;
        ComplexChar* complex_ch;
    };
    CharAttributes attr;
} TermChar;

typedef struct TermOutputBuffer {
    TermChar* buffer; // left top corner of actual console buffer, cursor_pos {0 0} is here
    TermChar* buffer_start; // actual start of the allocated buffer
    size_t buf_size; // size in bytes
    COORD cursor_pos;
    COORD line_input_saved_cursor;
    COORD size;
    uint32_t scrollback_available; // number of lines of current width "above" buffer that are available. can wrap to the bottom ((char*)buffer_start + buf_size)
    uint32_t scrollback; // in lines, above current view, shouldn't be higher than scrollbac_available
    vt_parse_state vt_state; // tracks vt state, when enabled


    example_partitioner Partitioner;
    d3d11_renderer Renderer;
    glyph_generator GlyphGen;
    void* GlyphTableMem;
    glyph_table* GlyphTable;
    renderer_cell* RenderCells;
    size_t RenderCells_size;

    wchar_t RequestedFontName[64];
    uint32_t RequestedFontHeight;
    int RequestClearType;
    int RequestDirectWrite;

    uint32_t REFTERM_TEXTURE_WIDTH;
    uint32_t REFTERM_TEXTURE_HEIGHT;

    uint32_t TransferWidth;
    uint32_t TransferHeight;

    uint32_t REFTERM_MAX_WIDTH;
    uint32_t REFTERM_MAX_HEIGHT;

    ComplexChar* complex_ch_freelist;

#define MinDirectCodepoint 32
#define MaxDirectCodepoint 126
    gpu_glyph_index ReservedTileTable[MaxDirectCodepoint - MinDirectCodepoint + 1];
} TermOutputBuffer;


inline TermChar* get_wrap_ptr(TermOutputBuffer* tb);
inline TermChar* check_wrap(TermOutputBuffer* tb, TermChar* ptr);
inline TermChar* check_wrap_backwards(TermOutputBuffer* tb, TermChar* ptr);