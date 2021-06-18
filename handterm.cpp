#define _CRT_SECURE_NO_DEPRECATE
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <intrin.h>
#include <winternl.h>
#include <stdlib.h>
#include <stdbool.h>
#include "condefs.h"
#include "colors.h"

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)
#define AssertHR(hr) Assert(SUCCEEDED(hr))

#pragma comment (lib, "gdi32.lib")
#pragma comment (lib, "user32.lib")
#pragma comment (lib, "dxguid.lib")
#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "d3dcompiler.lib")

#define STR2(x) #x
#define STR(x) STR2(x)

PfnNtOpenFile _NtOpenFile;
PfnConsoleControl _ConsoleControl;

// GLOBALS
HWND window;
HANDLE window_ready_event;
HANDLE console_mutex;
HANDLE ServerHandle, ReferenceHandle, InputEventHandle;
#define MAX_WIDTH 4096
#define MAX_HEIGHT 4096

// in terminal size
#define MIN_WIDTH 32
#define MIN_HEIGHT 16

ULONG input_console_mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION;
ULONG output_console_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

typedef struct CharAttributes {
    int color;
    int backg;
    bool soft_wrap;
};

typedef struct TermChar {
    int ch;
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
    uint32_t scrollback; // this is "absolute" value, from buffer_start, NOT from buffer
} TermOutputBuffer;

TermOutputBuffer frontbuffer = { 0 };
TermOutputBuffer relayoutbuffer = { 0 };

CharAttributes current_attrs = { 0xffffff, 0, false };

INPUT_RECORD pending_events[256];
volatile PINPUT_RECORD pending_events_write_ptr = pending_events;
volatile PINPUT_RECORD pending_events_read_ptr = pending_events;
PINPUT_RECORD pending_events_wrap_ptr = pending_events + sizeof(pending_events)/sizeof(pending_events[0]);

bool console_exit = false;

wchar_t console_title[256] = L"Handterm";

CONSOLE_API_MSG delayed_io_msg;
bool has_delayed_io_msg = false;
HANDLE delayed_io_event;

bool show_cursor = true;
SHORT default_scrollback = 10;

// END GLOBALS

bool initialize_term_output_buffer(TermOutputBuffer& tb, COORD size, bool zeromem) {
    size_t requested_size = size.X * (size.Y + default_scrollback) * sizeof(tb.buffer[0]);
    if (tb.buf_size < requested_size) {
        TermChar* new_buf = (TermChar*)realloc(tb.buffer_start, requested_size);
        if (new_buf) {
            ZeroMemory(new_buf, requested_size);
            tb.buffer_start = new_buf;
            tb.buf_size = requested_size;
        } else {
            return false;
        }
    } else if (zeromem) {
        ZeroMemory(tb.buffer_start, requested_size);
    }
    tb.buffer = tb.buffer_start;
    tb.cursor_pos = { 0 };
    tb.line_input_saved_cursor = { 0 };
    tb.scrollback_available = 0;
    tb.scrollback = 0;
    tb.size = size;
    return true;
}

inline TermChar* get_wrap_ptr(TermOutputBuffer* tb) {
    return tb->buffer_start + tb->size.X * (tb->size.Y + default_scrollback);
}

inline TermChar* check_wrap(TermOutputBuffer* tb, TermChar* ptr) {
    TermChar* wrap_ptr = get_wrap_ptr(tb);
    if (ptr >= wrap_ptr) {
        return tb->buffer_start + (ptr - wrap_ptr);
    }
    return ptr;
}

inline TermChar* check_wrap_backwards(TermOutputBuffer* tb, TermChar* ptr) {
    if (ptr < tb->buffer_start) {
        return get_wrap_ptr(tb) - (tb->buffer_start - ptr);
    }
    return ptr;
}

HRESULT PushEvent(INPUT_RECORD ev) {
    // buffer full
    PINPUT_RECORD next = pending_events_write_ptr + 1;
    if (next == pending_events_wrap_ptr)
        next = pending_events;
    if (next == pending_events_read_ptr) {
        return STATUS_UNSUCCESSFUL;
    }
    *pending_events_write_ptr = ev;
    pending_events_write_ptr = next;
    SetEvent(InputEventHandle);
    return STATUS_SUCCESS;
}

void FastFast_LockTerminal() {
    HRESULT hr = WaitForSingleObject(console_mutex, INFINITE);
    Assert(hr == WAIT_OBJECT_0);
}
void FastFast_UnlockTerminal() {
    ReleaseMutex(console_mutex);
}

void scroll_up(TermOutputBuffer& tb, UINT lines) {
    tb.cursor_pos.Y -= lines;
    tb.scrollback_available = min(default_scrollback, tb.scrollback_available + lines);
    if (tb.scrollback) {
        if (++tb.scrollback > tb.scrollback_available) {
            tb.scrollback = tb.scrollback_available;
        }
    }

    tb.buffer = check_wrap(&tb, tb.buffer + tb.size.X * lines);
    TermChar* zeroing_start = check_wrap(&tb, tb.buffer + tb.size.X * (tb.size.Y - lines));
    TermChar* screen_end = check_wrap(&tb, tb.buffer + tb.size.X * tb.size.Y);
    // if no wrap
    if (screen_end >= zeroing_start) {
        ZeroMemory(zeroing_start, (char*)screen_end - (char*)zeroing_start);
    } else {
        ZeroMemory(zeroing_start, (char*)get_wrap_ptr(&tb) - (char*)zeroing_start);
        ZeroMemory(tb.buffer_start, (char*)screen_end - (char*)tb.buffer_start);
    }
}

void FastFast_Resize(SHORT width, SHORT height) {
    width = max(MIN_WIDTH, width);
    height = max(MIN_HEIGHT, height);
    FastFast_LockTerminal();

    if (width == frontbuffer.size.X && height == frontbuffer.size.Y) {
        FastFast_UnlockTerminal();
        return;
    }

    Assert(initialize_term_output_buffer(relayoutbuffer, { width, height }, true));
    COORD& cursor_pos = relayoutbuffer.cursor_pos;
    COORD mapped_cursor_pos = { 0, 0 };
    COORD size = relayoutbuffer.size;
    relayoutbuffer.line_input_saved_cursor = { 0, 0 };
    int empty_lines = 0;
    // consider found if it's 0 0
    bool line_input_cursor_found = frontbuffer.line_input_saved_cursor.X == 0 && frontbuffer.line_input_saved_cursor.Y == 0;
    bool cursor_found = frontbuffer.cursor_pos.X == 0 && frontbuffer.cursor_pos.Y == 0;
    TermChar* last_ch_from_prev_row = 0;
    TermChar* frontbuffer_wrap = get_wrap_ptr(&frontbuffer);
    TermChar* old_t = check_wrap_backwards(&frontbuffer, frontbuffer.buffer - frontbuffer.size.X * frontbuffer.scrollback_available);
    bool first = true;
    for (int y = -(int)frontbuffer.scrollback_available; y < frontbuffer.size.Y; ++y) {
        // at each line that's not first, if last char on previous row doesn't have soft_wrap attribute set, we issue a new line
        if (!first) {
            if (old_t == frontbuffer_wrap) {
                old_t = frontbuffer.buffer_start;
            }
            if (!old_t->ch) {
                // empty lines might have zero charachters "printed" (non-zero ch), in general
                // we could just not check ch, but in the beginning, there might be lines that were never printed
                // and those we should leave alone at relayout. So solution here is to count empty lines
                // until we hit non-empty one. Trailing empty lines are ignored
                ++empty_lines;
            } else if(!last_ch_from_prev_row || !last_ch_from_prev_row->attr.soft_wrap) {
                cursor_pos.X = 0;
                cursor_pos.Y += empty_lines + 1;
                empty_lines = 0;
                if (cursor_pos.Y >= size.Y) {
                    scroll_up(relayoutbuffer, cursor_pos.Y - size.Y + 1);
                }
            }
        } else {
            first = false;
        }
        last_ch_from_prev_row = 0;
        for (int x = 0; x < frontbuffer.size.X; ++x) {
            if (!line_input_cursor_found && x == frontbuffer.line_input_saved_cursor.X && y == frontbuffer.line_input_saved_cursor.Y) {
                relayoutbuffer.line_input_saved_cursor = cursor_pos;
                line_input_cursor_found = true;
            }
            if (!cursor_found && x == frontbuffer.cursor_pos.X && y == frontbuffer.cursor_pos.Y) {
                mapped_cursor_pos = cursor_pos;
                cursor_found = true;
            }
            TermChar* t = check_wrap(&relayoutbuffer, relayoutbuffer.buffer + cursor_pos.Y * size.X + cursor_pos.X);
            if (!old_t->ch) {
                old_t += frontbuffer.size.X - x;
                // end of line
                break;
            }
            last_ch_from_prev_row = old_t;

            if (cursor_pos.X == size.X)
            {
                TermChar* prev_char = check_wrap_backwards(&relayoutbuffer, t - 1);
                // todo: do we really need this check? if we ended-up here, that we _should_ have had previous char
                if (prev_char->ch) {
                    prev_char->attr.soft_wrap = true;
                }
                cursor_pos.X = 0;
                cursor_pos.Y++;
                // we don't need to update t, as it already points to the right one
            }
            if (cursor_pos.Y >= size.Y) {
                scroll_up(relayoutbuffer, cursor_pos.Y - size.Y + 1);
                t = check_wrap(&relayoutbuffer, relayoutbuffer.buffer + cursor_pos.Y * size.X + cursor_pos.X);
            }
            *t = *old_t;
            t->attr.soft_wrap = false;
            cursor_pos.X++;

            old_t++;
        }
    }
    // should be always true
    Assert(line_input_cursor_found);
    Assert(cursor_found);
    cursor_pos = mapped_cursor_pos;
    TermOutputBuffer temp = frontbuffer;
    frontbuffer = relayoutbuffer;
    relayoutbuffer = temp;

    INPUT_RECORD resize_event = {};
    resize_event.EventType = WINDOW_BUFFER_SIZE_EVENT;
    resize_event.Event.WindowBufferSizeEvent.dwSize = frontbuffer.size;
    PushEvent(resize_event);
    FastFast_UnlockTerminal();
}

static int num(const wchar_t*& str)
{
    int r = 0;
    while (*str >= L'0' && *str <= L'9')
    {
        r *= 10;
        r += *str++ - '0';
    }
    return r;
}
static int num(const char*& str)
{
    int r = 0;
    while (*str >= '0' && *str <= '9')
    {
        r *= 10;
        r += *str++ - '0';
    }
    return r;
}

void FastFast_UpdateTerminalW(TermOutputBuffer& tb, const wchar_t* str, SIZE_T count)
{
    COORD& cursor_pos = tb.cursor_pos;
    COORD& size = tb.size;
    while (count != 0)
    {
        if (str[0] == L'\x1b')
        {
            str++;
            count--;
            SIZE_T pos = 0;
            while (pos < count && (str[pos] != 'H' && str[pos] != 'm'))
            {
                pos++;
            }
            if (pos == count) return;
            if (str[pos] == 'H')
            {
                const wchar_t* s = str + 1;
                cursor_pos.Y = num(s) - 1;
                s++;
                cursor_pos.X = num(s) - 1;
            }
            else if (str[pos] == 'm')
            {
                BOOL fg = str[1] == L'3';
                const wchar_t* s = str + 6;
                int r = num(s);
                s++;
                int g = num(s);
                s++;
                int b = num(s);

                if (fg) {
                    current_attrs.color = r | (g << 8) | (b << 16);
                } else {
                    current_attrs.backg = r | (g << 8) | (b << 16);
                }
            }
            str += pos + 1;
            count -= pos + 1;
        }
        // backspace
        else if (str[0] == L'\b') {
            bool overwrite = true;            
            COORD backup_limit = { 0, 0 };
            // todo: this should only be done for echoed input characters, not any backspace that app writes
            if (input_console_mode & ENABLE_LINE_INPUT) {
                backup_limit = tb.line_input_saved_cursor;
            }

            if (!cursor_pos.X) {
                if (cursor_pos.Y > backup_limit.Y) {
                    --cursor_pos.Y;
                    cursor_pos.X = size.X - 1;
                } else {
                    // nowhere to backup
                    overwrite = false;
                }
            } else {
                if (cursor_pos.Y > backup_limit.Y || cursor_pos.X > backup_limit.X) {
                    cursor_pos.X--;
                } else {
                    // nowhere to backup
                    overwrite = false;
                }
            }
            if (overwrite) {
                TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
                t->ch = ' ';
                t->attr = current_attrs;
            }
            str++;
            count--;
        }
        else if (str[0] == L'\r') {
            cursor_pos.X = 0;
            str++;
            count--;
        }
        else if (str[0] == L'\n') {
            cursor_pos.X = 0;
            cursor_pos.Y++;
            if (cursor_pos.Y >= size.Y) {
                scroll_up(tb, cursor_pos.Y - size.Y + 1);
            }
            // write space without advancing just to mark this line as "used"
            TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
            t->ch = ' ';
            t->attr = current_attrs;
            str++;
            count--;
        }
        else
        {
            // todo: also do that if cursor moves beyond and WRAP_AT_EOL_OUTPUT is enabled
            if (cursor_pos.Y >= size.Y) {
                scroll_up(tb, cursor_pos.Y - size.Y + 1);
            }
            if (cursor_pos.X >= 0 && cursor_pos.X < size.X && cursor_pos.Y >= 0 && cursor_pos.Y < size.Y)
            {
                TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
                t->ch = *str;
                t->attr = current_attrs;

                cursor_pos.X++;
                if (cursor_pos.X == size.X)
                {
                    cursor_pos.X = 0;
                    cursor_pos.Y++;
                    t->attr.soft_wrap = true;
                }
            }
            str++;
            count--;
        }
    }
}
void FastFast_UpdateTerminalA(TermOutputBuffer& tb, const char* str, SIZE_T count)
{
    COORD& cursor_pos = tb.cursor_pos;
    COORD& size = tb.size;
    while (count != 0)
    {
        if (str[0] == '\x1b')
        {
            str++;
            count--;
            SIZE_T pos = 0;
            while (pos < count && (str[pos] != 'H' && str[pos] != 'm'))
            {
                pos++;
            }
            if (pos == count) return;
            if (str[pos] == 'H')
            {
                const char* s = str + 1;
                cursor_pos.Y = num(s) - 1;
                s++;
                cursor_pos.X = num(s) - 1;
            } else if (str[pos] == 'm')
            {
                BOOL fg = str[1] == L'3';
                const char* s = str + 6;
                int r = num(s);
                s++;
                int g = num(s);
                s++;
                int b = num(s);

                if (fg) {
                    current_attrs.color = r | (g << 8) | (b << 16);
                } else {
                    current_attrs.backg = r | (g << 8) | (b << 16);
                }
            }
            str += pos + 1;
            count -= pos + 1;
        }
        // backspace
        else if (str[0] == '\b') {
            bool overwrite = true;
            COORD backup_limit = { 0, 0 };
            // todo: this should only be done for echoed input characters, not any backspace that app writes
            if (input_console_mode & ENABLE_LINE_INPUT) {
                backup_limit = tb.line_input_saved_cursor;
            }

            if (!cursor_pos.X) {
                if (cursor_pos.Y > backup_limit.Y) {
                    --cursor_pos.Y;
                    cursor_pos.X = size.X - 1;
                } else {
                    // nowhere to backup
                    overwrite = false;
                }
            } else {
                if (cursor_pos.Y > backup_limit.Y || cursor_pos.X > backup_limit.X) {
                    cursor_pos.X--;
                } else {
                    // nowhere to backup
                    overwrite = false;
                }
            }
            if (overwrite) {
                TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
                t->ch = ' ';
                t->attr = current_attrs;
            }
            str++;
            count--;
        } else if (str[0] == '\r') {
            cursor_pos.X = 0;
            str++;
            count--;
        } else if (str[0] == '\n') {
            cursor_pos.X = 0;
            cursor_pos.Y++;
            if (cursor_pos.Y >= size.Y) {
                scroll_up(tb, cursor_pos.Y - size.Y + 1);
            }
            // write space without advancing just to mark this line as "used"
            TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
            t->ch = ' ';
            t->attr = current_attrs;
            str++;
            count--;
        } else
        {
            // todo: also do that if cursor moves beyond and WRAP_AT_EOL_OUTPUT is enabled
            if (cursor_pos.Y >= size.Y) {
                scroll_up(tb, cursor_pos.Y - size.Y + 1);
            }
            if (cursor_pos.X >= 0 && cursor_pos.X < size.X && cursor_pos.Y >= 0 && cursor_pos.Y < size.Y)
            {
                TermChar* t = check_wrap(&tb, tb.buffer + cursor_pos.Y * size.X + cursor_pos.X);
                t->ch = *str;
                t->attr = current_attrs;

                cursor_pos.X++;
                if (cursor_pos.X == size.X)
                {
                    cursor_pos.X = 0;
                    cursor_pos.Y++;
                    t->attr.soft_wrap = true;
                }
            }
            str++;
            count--;
        }
    }
}

const size_t DEFAULT_BUF_SIZE = 128;

thread_local char* buf = 0;
thread_local size_t buf_size = 0;
thread_local size_t buf_bytes_read = 0;
thread_local size_t buf_bytes_written = 0;
thread_local size_t buf_write_idx = 0;

HRESULT HandleReadMessage(PCONSOLE_API_MSG ReceiveMsg, PCD_IO_COMPLETE io_complete) {
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;

    // todo: this should be part of thread initialization to avoid this check on every message
    if (!buf_size) {
        buf = (char*)malloc(DEFAULT_BUF_SIZE);
        buf_size = DEFAULT_BUF_SIZE;
    }

    ULONG user_write_buffer_size = ReceiveMsg->Descriptor.OutputSize - ReceiveMsg->msgHeader.ApiDescriptorSize;

    // todo: these can now be modified/read by 3 threads, probably should lock :|
    PINPUT_RECORD write_ptr = pending_events_write_ptr;
    PINPUT_RECORD read_ptr = pending_events_read_ptr;
    if (read_ptr != write_ptr) {
        PCONSOLE_READCONSOLE_MSG read_msg = &ReceiveMsg->u.consoleMsgL1.ReadConsoleW;

        if (read_msg->InitialNumBytes) {
            // not supported
            return STATUS_UNSUCCESSFUL;
        }

        wchar_t* wbuf = (wchar_t*)buf;
        // todo: mode should probably be copy-stored for delayed IO
        bool line_mode_enabled = input_console_mode & ENABLE_LINE_INPUT;

        CD_IO_OPERATION write_op = { 0 };
        write_op.Identifier = ReceiveMsg->Descriptor.Identifier;
        write_op.Buffer.Offset = ReceiveMsg->msgHeader.ApiDescriptorSize;
       
        if (!line_mode_enabled) {  
            while (read_ptr != write_ptr) {
                // it's expected that in this mode we just skip other events
                if (read_ptr->EventType == KEY_EVENT && read_ptr->Event.KeyEvent.uChar.UnicodeChar) {
                    // todo: support repeat
                    Assert(read_ptr->Event.KeyEvent.wRepeatCount == 1);

                    buf_bytes_written += read_msg->Unicode ? 2 : 1;
                    // without line mode we can return any amount of data
                    // and we should also stop reading as soon as we have enough data in buffer
                    if (buf_bytes_written && (buf_bytes_written > buf_size || buf_bytes_written > user_write_buffer_size)) {
                        break;
                    }
                    if (read_msg->Unicode) {
                        wbuf[buf_write_idx++] = read_ptr->Event.KeyEvent.uChar.UnicodeChar;
                    } else {
                        buf[buf_write_idx++] = read_ptr->Event.KeyEvent.uChar.AsciiChar;
                    }
                }
                if (++read_ptr == pending_events_wrap_ptr) {
                    read_ptr = pending_events;
                }
            }

            // consume processed events no matter what
            pending_events_read_ptr = read_ptr;
            // todo: this is most likely not thread safe
            if (pending_events_read_ptr == pending_events_write_ptr) {
                ResetEvent(InputEventHandle);
            }

            if (!buf_bytes_written) {
                return STATUS_TIMEOUT;
            }
            write_op.Buffer.Data = buf;
            write_op.Buffer.Size = buf_bytes_written;
        } else {
            // no data available to read right away, buffer until we get CR
            if (!buf_bytes_read) {
                bool has_cr = false;
                while (read_ptr != write_ptr) {
                    // it's expected that in this mode we just skip other events
                    if (read_ptr->EventType == KEY_EVENT && read_ptr->Event.KeyEvent.uChar.UnicodeChar) {
                        // todo: support repeat
                        Assert(read_ptr->Event.KeyEvent.wRepeatCount == 1);
                        bool is_cr, is_removing;
                        if (read_msg->Unicode) {
                            is_cr = read_ptr->Event.KeyEvent.uChar.UnicodeChar == L'\r';
                            is_removing = read_ptr->Event.KeyEvent.uChar.UnicodeChar == L'\b';
                        } else {
                            is_cr = read_ptr->Event.KeyEvent.uChar.AsciiChar == '\r';
                            is_removing = read_ptr->Event.KeyEvent.uChar.UnicodeChar == '\b';
                        }
                        has_cr |= is_cr;

                        if (is_removing) {
                            // todo: support other chars
                            if (read_ptr->Event.KeyEvent.uChar.UnicodeChar == L'\b') {
                                if (buf_write_idx) {
                                    buf_bytes_written -= read_msg->Unicode ? 2 : 1;
                                    if (read_msg->Unicode) {
                                        wbuf[buf_write_idx] = L' ';
                                    } else {
                                        buf[buf_write_idx] = ' ';
                                    }
                                    --buf_write_idx;
                                } else {
                                    // nothing to remove
                                }
                            }
                        }
                        else {
                            buf_bytes_written += (is_cr ? 2 : 1) * (read_msg->Unicode ? 2 : 1);
                            if (buf_bytes_written > buf_size) {
                                buf_size = buf_size * 2;
                                buf = (char*)realloc(buf, buf_size);
                                wbuf = (wchar_t*)buf;
                            }
                            if (read_msg->Unicode) {
                                wbuf[buf_write_idx++] = read_ptr->Event.KeyEvent.uChar.UnicodeChar;
                                if (is_cr) {
                                    wbuf[buf_write_idx++] = L'\n';
                                }
                            } else {
                                buf[buf_write_idx++] = read_ptr->Event.KeyEvent.uChar.AsciiChar;
                                if (is_cr) {
                                    buf[buf_write_idx++] = '\n';
                                }
                            }
                        }
                    }
                    if (++read_ptr == pending_events_wrap_ptr) {
                        read_ptr = pending_events;
                    }
                    if (line_mode_enabled && has_cr) {
                        break;
                    }
                }

                // consume processed events no matter what
                pending_events_read_ptr = read_ptr;
                // todo: this is most likely not thread safe
                if (pending_events_read_ptr == pending_events_write_ptr) {
                    ResetEvent(InputEventHandle);
                }

                if (input_console_mode & ENABLE_ECHO_INPUT) {
                    FastFast_LockTerminal();
                    // todo: this is not super efficient way to do it, especially for things like backspace at EOL
                    size_t backup_dist = (frontbuffer.cursor_pos.Y - frontbuffer.line_input_saved_cursor.Y) * frontbuffer.size.X + frontbuffer.cursor_pos.X - frontbuffer.line_input_saved_cursor.X;
                    for (size_t i = 0; i < backup_dist; ++i) {
                        char backspace = '\b';
                        FastFast_UpdateTerminalA(frontbuffer, &backspace, 1);
                    }
                    frontbuffer.cursor_pos = frontbuffer.line_input_saved_cursor;
                    if (read_msg->Unicode) {
                        FastFast_UpdateTerminalW(frontbuffer, wbuf, buf_write_idx);
                    } else {
                        FastFast_UpdateTerminalA(frontbuffer, buf, buf_write_idx);
                    }
                    if (has_cr) {
                        frontbuffer.line_input_saved_cursor = { 0, 0 };
                    }
                    FastFast_UnlockTerminal();
                }

                if (!has_cr) {
                    return STATUS_TIMEOUT;
                }
            }
            if (!buf_bytes_written) {
                return STATUS_TIMEOUT;
            }
            // if we got here, we either got CR or there was some data from previous line read request
            size_t bytes_left = buf_bytes_written - buf_bytes_read;
            ULONG bytes_to_write = min(bytes_left, user_write_buffer_size);
            write_op.Buffer.Data = buf + buf_bytes_read;
            write_op.Buffer.Size = bytes_to_write;
        }
        
        ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_WRITE_OUTPUT, &write_op, sizeof(write_op), 0, 0, &BytesRead, 0);
        hr = ok ? S_OK : GetLastError();
        Assert(SUCCEEDED(hr));

        buf_bytes_read += write_op.Buffer.Size;
        // reset pointers after whole buffer was written to client
        if (buf_bytes_read == buf_bytes_written) {
            buf_bytes_read = 0;
            buf_bytes_written = 0;
            buf_write_idx = 0;
        }

        io_complete->IoStatus.Information = read_msg->NumBytes = write_op.Buffer.Size;
    } else {
        return STATUS_TIMEOUT;
    }
}

HRESULT HandleWriteMessage(PCONSOLE_API_MSG ReceiveMsg, PCD_IO_COMPLETE io_complete) {
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;

    PCONSOLE_WRITECONSOLE_MSG write_msg = &ReceiveMsg->u.consoleMsgL1.WriteConsoleW; // this WriteConsole macro...
    size_t read_offset = ReceiveMsg->msgHeader.ApiDescriptorSize + sizeof(CONSOLE_MSG_HEADER);
    size_t bytes = ReceiveMsg->Descriptor.InputSize - read_offset;
    if (buf_size < bytes) {
        buf = (char*)realloc(buf, bytes);
        buf_size = bytes;
    }
    CD_IO_OPERATION read_op = { 0 };
    read_op.Identifier = ReceiveMsg->Descriptor.Identifier;
    read_op.Buffer.Offset = read_offset;
    read_op.Buffer.Data = buf;
    read_op.Buffer.Size = bytes;
    ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_READ_INPUT, &read_op, sizeof(read_op), 0, 0, &BytesRead, 0);
    hr = ok ? S_OK : GetLastError();
    Assert(SUCCEEDED(hr));

    FastFast_LockTerminal();
    if (write_msg->Unicode) {
        FastFast_UpdateTerminalW(frontbuffer, (wchar_t*)buf, bytes / 2);
    } else {
        FastFast_UpdateTerminalA(frontbuffer, buf, bytes);
    }
    FastFast_UnlockTerminal();

    io_complete->IoStatus.Information = write_msg->NumBytes = bytes;
    return STATUS_SUCCESS;
}

DWORD WINAPI DelayedIoThread(LPVOID lpParameter)
{
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;
    CD_IO_COMPLETE io_complete;

    while (!console_exit) {
        hr = WaitForSingleObject(delayed_io_event, INFINITE);
        
        if (!has_delayed_io_msg) {
            continue;
        }
        ZeroMemory(&io_complete, sizeof(io_complete));
        io_complete.Identifier = delayed_io_msg.Descriptor.Identifier;
        io_complete.Write.Data = &delayed_io_msg.u;
        io_complete.Write.Size = delayed_io_msg.msgHeader.ApiDescriptorSize;

        switch (delayed_io_msg.Descriptor.Function) {
        case CONSOLE_IO_RAW_READ: // we setup api number to correct one
        case CONSOLE_IO_USER_DEFINED: {
            ULONG const LayerNumber = (delayed_io_msg.msgHeader.ApiNumber >> 24) - 1;
            ULONG const ApiNumber = delayed_io_msg.msgHeader.ApiNumber & 0xffffff;
            if (LayerNumber == 0) {
                switch (ApiNumber) {
                case Api_ReadConsole: {
                    io_complete.IoStatus.Status = HandleReadMessage(&delayed_io_msg, &io_complete);
                    // no data available, leave all as is
                    if (io_complete.IoStatus.Status == STATUS_TIMEOUT) {
                        break;
                    }
                    ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_COMPLETE_IO, &io_complete, sizeof(io_complete), 0, 0, &BytesRead, 0);
                    hr = ok ? S_OK : GetLastError();
                    Assert(SUCCEEDED(hr));
                    has_delayed_io_msg = false;
                    // not really required, but useful for debugging
                    ZeroMemory(&delayed_io_msg, sizeof(delayed_io_msg));
                    break;
                }
                default: {
                    Assert(!"Unexpected message type in delayed io queue");
                    break;
                }
                }
            }
            break;
        }
        default: {
            Assert(!"Unexpected message type in delayed io queue");
            break;
        }
        }
    }
    return 0;
}

DWORD WINAPI ConsoleIoThread(LPVOID lpParameter)
{
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;

    CONSOLE_API_MSG ReceiveMsg;
    PCONSOLE_API_MSG ReplyMsg = 0;
    bool send_io_complete = false;
    CD_IO_COMPLETE io_complete = { 0 };
    ULONG ReadOffset = 0;    

    char dbg_buf[256] = { 0 };

    while (!console_exit) {
        // Read next command
        ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_READ_IO, send_io_complete ? &io_complete : 0, send_io_complete ? sizeof(io_complete) : 0, &ReceiveMsg, sizeof(ReceiveMsg), &BytesRead, 0);
        send_io_complete = false;
        ZeroMemory(&io_complete, sizeof(io_complete));
        hr = GetLastError();
        hr = ok ? S_OK : GetLastError();
        if (hr == HRESULT_FROM_WIN32(ERROR_IO_PENDING)) {
            WaitForSingleObjectEx(ServerHandle, 0, FALSE);
        } else {
            if (hr == ERROR_PIPE_NOT_CONNECTED) {
                console_exit = true;
                // ??
                return 0;
            }
            Assert(SUCCEEDED(hr));
        }

        // We got some command

        //CD_IO_COMPLETE complete = { 0 };
        io_complete.Identifier = ReceiveMsg.Descriptor.Identifier;

        switch (ReceiveMsg.Descriptor.Function) {
        case CONSOLE_IO_CONNECT: {
            CONSOLE_SERVER_MSG data = { 0 };
            CD_IO_OPERATION op;
            op.Identifier = ReceiveMsg.Descriptor.Identifier;
            op.Buffer.Offset = ReadOffset;
            op.Buffer.Data = &data;
            op.Buffer.Size = sizeof(data);
            ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_READ_INPUT, &op, sizeof(op), 0, 0, &BytesRead, 0);
            hr = ok ? S_OK : GetLastError();
            Assert(SUCCEEDED(hr));

            if (data.ConsoleApp) {
                CONSOLE_PROCESS_INFO cpi;
                cpi.dwProcessID = ReceiveMsg.Descriptor.Process;
                cpi.dwFlags = CPI_NEWPROCESSWINDOW;

                _ConsoleControl(ConsoleNotifyConsoleApplication, &cpi, sizeof(cpi));
            }
            // todo: allocate new console if not created before
            // todo: notify OS that app is foreground
            // todo: create IO handles?

            // reply with success


            CD_CONNECTION_INFORMATION connect_info = { 0 };
            connect_info.Input = 0;
            connect_info.Output = 1;
            connect_info.Process = 2;

            io_complete.IoStatus.Status = STATUS_SUCCESS;
            io_complete.IoStatus.Information = sizeof(connect_info);
            io_complete.Write.Data = &connect_info;
            io_complete.Write.Size = sizeof(connect_info);
            ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_COMPLETE_IO, &io_complete, sizeof(io_complete), 0, 0, &BytesRead, 0);
            hr = ok ? S_OK : GetLastError();
            Assert(SUCCEEDED(hr));

            break;
        }
        case CONSOLE_IO_DISCONNECT: {
            send_io_complete = true;
            io_complete.IoStatus.Status = STATUS_SUCCESS;
            CONSOLEWINDOWOWNER ConsoleOwner;
            ConsoleOwner.hwnd = 0;
            ConsoleOwner.ProcessId = GetCurrentProcessId();
            ConsoleOwner.ThreadId = GetCurrentThreadId();
            _ConsoleControl(ConsoleSetWindowOwner, &ConsoleOwner, sizeof(ConsoleOwner));
            break;
        }
        case CONSOLE_IO_CREATE_OBJECT: {
            send_io_complete = true;
            PCD_CREATE_OBJECT_INFORMATION const CreateInformation = &ReceiveMsg.CreateObject;
            HANDLE handle = INVALID_HANDLE_VALUE;
            if (CreateInformation->ObjectType == CD_IO_OBJECT_TYPE_CURRENT_INPUT) {
                handle = 0;
            } else if (CreateInformation->ObjectType == CD_IO_OBJECT_TYPE_CURRENT_OUTPUT) {
                handle = (HANDLE)1;
            } else {
                // not supported yet
            }
            io_complete.IoStatus.Status = handle == INVALID_HANDLE_VALUE ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
            io_complete.IoStatus.Information = (ULONG)handle;
            break;
        }
        case CONSOLE_IO_RAW_READ:
        {
            send_io_complete = true;
            ZeroMemory(&ReceiveMsg.u.consoleMsgL1.ReadConsoleW, sizeof(CONSOLE_READCONSOLE_MSG));
            ReceiveMsg.msgHeader.ApiNumber = 0x01000005; // ReadConsole
            ReceiveMsg.msgHeader.ApiDescriptorSize = 0;
            hr = HandleReadMessage(&ReceiveMsg, &io_complete);
            if (hr == STATUS_TIMEOUT) {
                send_io_complete = false;
                delayed_io_msg = ReceiveMsg;
                has_delayed_io_msg = true;
            } else {
                io_complete.IoStatus.Status = hr;
            }
            break;
        }
        case CONSOLE_IO_RAW_WRITE:
        {
            send_io_complete = true;
            ZeroMemory(&ReceiveMsg.u.consoleMsgL1.WriteConsoleW, sizeof(CONSOLE_WRITECONSOLE_MSG));
            ReceiveMsg.msgHeader.ApiNumber = 0x01000006; // WriteConsole
            ReceiveMsg.msgHeader.ApiDescriptorSize = 0;
            hr = HandleWriteMessage(&ReceiveMsg, &io_complete);
            /*
            if (hr == STATUS_TIMEOUT) {
                send_io_complete = false;
                delayed_io_msg = ReceiveMsg;
                has_delayed_io_msg = true;
            } else {*/
                io_complete.IoStatus.Status = hr;
            //}
            break;
        }
        case CONSOLE_IO_USER_DEFINED: {
            ULONG const LayerNumber = (ReceiveMsg.msgHeader.ApiNumber >> 24) - 1;
            ULONG const ApiNumber = ReceiveMsg.msgHeader.ApiNumber & 0xffffff;

            // default is failure
            send_io_complete = true;
            io_complete.IoStatus.Status = STATUS_UNSUCCESSFUL;
            io_complete.Write.Data = &ReceiveMsg.u;
            io_complete.Write.Size = ReceiveMsg.msgHeader.ApiDescriptorSize;

            if (LayerNumber == 0) {
                CONSOLE_L1_API_TYPE l1_type = (CONSOLE_L1_API_TYPE)ApiNumber;
                switch (l1_type) {
                case Api_GetConsoleMode: {
                    PCONSOLE_MODE_MSG get_console_msg = &ReceiveMsg.u.consoleMsgL1.GetConsoleMode;
                    // input
                    if (ReceiveMsg.Descriptor.Object == 0) {
                        get_console_msg->Mode = input_console_mode;
                    } else {
                        get_console_msg->Mode = output_console_mode;
                    }
                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                case Api_SetConsoleMode: {
                    PCONSOLE_MODE_MSG get_console_msg = &ReceiveMsg.u.consoleMsgL1.GetConsoleMode;
                    // output
                    if (ReceiveMsg.Descriptor.Object == 0) {
                        input_console_mode = get_console_msg->Mode;
                    }
                    else {
                        output_console_mode = get_console_msg->Mode;
                    }
                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                case Api_WriteConsole: {
                    hr = HandleWriteMessage(&ReceiveMsg, &io_complete);
                    /*if (hr == STATUS_TIMEOUT) {
                        send_io_complete = false;
                        delayed_io_msg = ReceiveMsg;
                        has_delayed_io_msg = true;
                    } else {
                    */
                        io_complete.IoStatus.Status = hr;
                    // }
                    break;
                }
                case Api_GetConsoleInput: {
                    // write ptr can be modified while we are running, read out value so it doesn't change while we process
                    PINPUT_RECORD write_ptr = pending_events_write_ptr;
                    PINPUT_RECORD read_ptr = pending_events_read_ptr;
                    if (read_ptr != write_ptr) {
                        PCONSOLE_GETCONSOLEINPUT_MSG input_msg = &ReceiveMsg.u.consoleMsgL1.GetConsoleInput;

                        // wrapped, just read till the end, next read operation can read chunk at the beginning
                        if (write_ptr < read_ptr) {
                            write_ptr = pending_events_wrap_ptr;
                        }

                        input_msg->NumRecords = write_ptr - read_ptr;

                        size_t bytes = sizeof(INPUT_RECORD) * input_msg->NumRecords;

                        CD_IO_OPERATION write_op = { 0 };
                        write_op.Identifier = ReceiveMsg.Descriptor.Identifier;
                        write_op.Buffer.Offset = ReceiveMsg.msgHeader.ApiDescriptorSize;
                        write_op.Buffer.Data = read_ptr;
                        write_op.Buffer.Size = bytes;

                        ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_WRITE_OUTPUT, &write_op, sizeof(write_op), 0, 0, &BytesRead, 0);
                        hr = ok ? S_OK : GetLastError();
                        Assert(SUCCEEDED(hr));

                        read_ptr = write_ptr;
                        if (read_ptr == pending_events_wrap_ptr) {
                            read_ptr = pending_events;
                        }
                        pending_events_read_ptr = read_ptr;
                        // todo: this is most likely not thread safe
                        if (pending_events_read_ptr == pending_events_write_ptr) {
                            ResetEvent(InputEventHandle);
                        }

                        io_complete.IoStatus.Status = STATUS_SUCCESS;
                        io_complete.IoStatus.Information = bytes;
                    } else {
                        // should delay reply
                    }
                    break;
                }
                case Api_ReadConsole: {
                    if (input_console_mode & ENABLE_LINE_INPUT) {
                        FastFast_LockTerminal();
                        frontbuffer.line_input_saved_cursor = frontbuffer.cursor_pos;
                        FastFast_UnlockTerminal();
                    }
                    hr = HandleReadMessage(&ReceiveMsg, &io_complete);
                    if (hr == STATUS_TIMEOUT) {
                        send_io_complete = false;
                        delayed_io_msg = ReceiveMsg;
                        has_delayed_io_msg = true;
                    } else {
                        io_complete.IoStatus.Status = hr;
                    }
                    break;
                }
                default:
                    sprintf(dbg_buf, "Unhandled L1 request %d\n", ApiNumber);
                    OutputDebugStringA(dbg_buf);
                }
            }
            else if (LayerNumber == 1) {
                CONSOLE_L2_API_TYPE l2_type = (CONSOLE_L2_API_TYPE)ApiNumber;
                switch (l2_type) {
                case Api_GetConsoleScreenBufferInfo: {
                    PCONSOLE_SCREENBUFFERINFO_MSG buf_info_msg = &ReceiveMsg.u.consoleMsgL2.GetConsoleScreenBufferInfo;
                    buf_info_msg->FullscreenSupported = false;
                    buf_info_msg->CursorPosition = frontbuffer.cursor_pos;
                    buf_info_msg->MaximumWindowSize = frontbuffer.size;
                    buf_info_msg->Size = frontbuffer.size;
                    buf_info_msg->CurrentWindowSize = frontbuffer.size;
                    buf_info_msg->ScrollPosition.X = 0;
                    buf_info_msg->ScrollPosition.Y = 0;
                    buf_info_msg->Attributes = 0;
                    buf_info_msg->PopupAttributes = 0;
                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                case Api_GetConsoleTitle: {
                    PCONSOLE_GETTITLE_MSG get_title_message = &ReceiveMsg.u.consoleMsgL2.GetConsoleTitleW;
                    if (!get_title_message->Unicode) {
                        Assert(!"Not supported");
                        // WideCharToMultiByte
                    }

                    size_t bytes = (lstrlenW(console_title) + 1) * sizeof(wchar_t);
                    CD_IO_OPERATION write_op = { 0 };
                    write_op.Identifier = ReceiveMsg.Descriptor.Identifier;
                    write_op.Buffer.Offset = ReceiveMsg.msgHeader.ApiDescriptorSize;
                    write_op.Buffer.Data = console_title;
                    write_op.Buffer.Size = bytes;

                    ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_WRITE_OUTPUT, &write_op, sizeof(write_op), 0, 0, &BytesRead, 0);
                    hr = ok ? S_OK : GetLastError();
                    Assert(SUCCEEDED(hr));

                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    io_complete.IoStatus.Information = bytes;

                    break;
                }
                case Api_SetConsoleTitle: {
                    PCONSOLE_SETTITLE_MSG set_title_msg = &ReceiveMsg.u.consoleMsgL2.SetConsoleTitleW; 
                    size_t read_offset = ReceiveMsg.msgHeader.ApiDescriptorSize + sizeof(CONSOLE_MSG_HEADER);
                    size_t bytes = ReceiveMsg.Descriptor.InputSize - read_offset;
                    if (!set_title_msg->Unicode) {
                        Assert(!"Not supported");
                        // WideCharToMultiByte
                        break;
                    }
                    if (bytes >= sizeof(console_title) - 1) {
                        // failure
                        break;
                    }
                    CD_IO_OPERATION read_op = { 0 };
                    read_op.Identifier = ReceiveMsg.Descriptor.Identifier;
                    read_op.Buffer.Offset = read_offset;
                    read_op.Buffer.Data = console_title;
                    read_op.Buffer.Size = bytes;

                    ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_READ_INPUT, &read_op, sizeof(read_op), 0, 0, &BytesRead, 0);
                    hr = ok ? S_OK : GetLastError();
                    Assert(SUCCEEDED(hr));

                    console_title[bytes / 2] = 0;

                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                case Api_SetConsoleTextAttribute: {
                    CONSOLE_SETTEXTATTRIBUTE_MSG* set_attr_msg = &ReceiveMsg.u.consoleMsgL2.SetConsoleTextAttribute;
                    char bg_idx = (set_attr_msg->Attributes >> 4) & 0xf;
                    current_attrs.backg = bg_idx ? default_colors[bg_idx] : default_bg_color;
                    char fg_idx = set_attr_msg->Attributes & 0xf;
                    current_attrs.color = fg_idx ? default_colors[fg_idx] : default_fg_color;

                    if (set_attr_msg->Attributes & META_ATTRS) {
                        sprintf(dbg_buf, "Unhandled console attributes in reqeust %d\n", set_attr_msg->Attributes);
                        OutputDebugStringA(dbg_buf);
                    }

                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                default:
                    sprintf(dbg_buf, "Unhandled L2 request %d\n", ApiNumber);
                    OutputDebugStringA(dbg_buf);
                }
            } else if (LayerNumber == 2) {
                CONSOLE_L3_API_TYPE l3_type = (CONSOLE_L3_API_TYPE)ApiNumber;
                switch (l3_type) {
                case Api_GetConsoleWindow: {
                    CONSOLE_GETCONSOLEWINDOW_MSG* get_hwnd_msg = &ReceiveMsg.u.consoleMsgL3.GetConsoleWindow;
                    get_hwnd_msg->hwnd = window;
                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    break;
                }
                default:
                    sprintf(dbg_buf, "Unhandled L3 request %d\n", ApiNumber);
                    OutputDebugStringA(dbg_buf);
                    break;
                }
            } else {
                sprintf(dbg_buf, "Unhandled unknown layer %d request %d\n", LayerNumber, ApiNumber);
                OutputDebugStringA(dbg_buf);
            }
            break;
        }
        default:
            send_io_complete = true;
            io_complete.IoStatus.Status = STATUS_UNSUCCESSFUL;
            sprintf(dbg_buf, "Unhandled request %d\n", ReceiveMsg.Descriptor.Function);
            OutputDebugStringA(dbg_buf);
            break;
        }
    }
    return 0;
}

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_VSCROLL:
        {
            FastFast_LockTerminal();
            switch (LOWORD(wParam)) {
            case SB_BOTTOM: {
                frontbuffer.scrollback = 0;
                break;
            }
            case SB_LINEDOWN: {
                if (frontbuffer.scrollback)
                    --frontbuffer.scrollback;
                break;
            }
            case SB_LINEUP: {
                if (frontbuffer.scrollback < frontbuffer.scrollback_available)
                    ++frontbuffer.scrollback;
                break;
            }
            case SB_PAGEDOWN: {
                frontbuffer.scrollback = max(0, frontbuffer.scrollback - frontbuffer.size.Y);
                break;
            }
            case SB_PAGEUP: {
                frontbuffer.scrollback = min(frontbuffer.scrollback_available, frontbuffer.scrollback + frontbuffer.size.Y);
                break;
            }
            case SB_THUMBTRACK: {
                frontbuffer.scrollback = max(0, min(frontbuffer.scrollback_available, frontbuffer.scrollback_available - HIWORD(wParam)));
                char dbg_buf[256] = { 0 };
                sprintf(dbg_buf, "Thumbtrack event, wparam.hi=%d scrollback=%d\n", HIWORD(wParam), frontbuffer.scrollback);
                OutputDebugStringA(dbg_buf);

                break;
            }
            }
            FastFast_UnlockTerminal();
            return 0;
        }
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

static IDXGISwapChain* swapChain;
static ID3D11Device* device;
static ID3D11DeviceContext* context;
static ID3D11Buffer* vbuffer;
static ID3D11InputLayout* layout;
static ID3D11VertexShader* vshader;
static ID3D11PixelShader* pshader;
static ID3D11Buffer* ubuffer;
static ID3D11ShaderResourceView* textureView;
static ID3D11SamplerState* sampler;
static ID3D11RasterizerState* rasterizerState;
static ID3D11RenderTargetView* rtView;

typedef struct Vertex
{
    float pos[2];
    float uv[2];
    int color;
    int backg;
} Vertex;


static Vertex* AddChar(Vertex* vtx, char ch, int x, int y, int color, int backg)
{
    float w = 8;
    float h = 16;
    float px = x * w;
    float py = y * h;

    float u1 = (float)ch / 256.f;
    float u2 = (float)(ch + 1) / 256.f;

    *vtx++ = Vertex{ { px, py }, { u1, 1 }, color, backg };
    *vtx++ = Vertex{ { px, py + h }, { u1, 0 }, color, backg };
    *vtx++ = Vertex{ { px + w, py }, { u2, 1 }, color, backg };
    *vtx++ = Vertex{ { px + w, py }, { u2, 1 }, color, backg };
    *vtx++ = Vertex{ { px, py + h }, { u1, 0 }, color, backg };
    *vtx++ = Vertex{ { px + w, py + h }, { u2, 0 }, color, backg };
    return vtx;
}

void SetupConsoleAndProcess() 
{
    DWORD RetBytes = 0, err;

    _NtOpenFile = (PfnNtOpenFile)GetProcAddress(LoadLibraryExW(L"ntdll.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32), "NtOpenFile");
    _ConsoleControl = (PfnConsoleControl)GetProcAddress(LoadLibraryExW(L"user32.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32), "ConsoleControl");
    Assert(_NtOpenFile);

    CreateServerHandle(&ServerHandle, FALSE);
    CreateClientHandle(&ReferenceHandle, ServerHandle, L"\\Reference", FALSE);

    InputEventHandle = CreateEventExW(0, 0, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
    delayed_io_event = CreateEventExW(0, 0, 0, EVENT_ALL_ACCESS);

    CD_IO_SERVER_INFORMATION ServerInfo = { 0 };
    ServerInfo.InputAvailableEvent = InputEventHandle;

    DeviceIoControl(ServerHandle, IOCTL_CONDRV_SET_SERVER_INFORMATION, &ServerInfo, sizeof(ServerInfo), 0, 0, &RetBytes, 0);

    HANDLE IoThreadHandle = CreateThread(0, 0, ConsoleIoThread, 0, 0, 0);
    HANDLE DelayedIoThreadHandle = CreateThread(0, 0, DelayedIoThread, 0, 0, 0);

    // This should be enough for Conhost setup

    //WCHAR cmd[] = L"C:/tools/termbench_release_msvc.exe";
    WCHAR cmd[] = L"cmd.exe";
    //WCHAR cmd[] = L"C:/stuff/console_test/Debug/console_test.exe";

    BOOL ok;
    HRESULT hr;
    HANDLE ClientHandles[3];
    hr = CreateClientHandle(&ClientHandles[0], ServerHandle, L"\\Input", TRUE);
    Assert(SUCCEEDED(hr));
    hr = CreateClientHandle(&ClientHandles[1], ServerHandle, L"\\Output", TRUE);
    Assert(SUCCEEDED(hr));
    ok = DuplicateHandle(GetCurrentProcess(), ClientHandles[1], GetCurrentProcess(), &ClientHandles[2], 0, TRUE, DUPLICATE_SAME_ACCESS);
    Assert(ok);

    STARTUPINFOEXW si = { 0 };
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = ClientHandles[0];
    si.StartupInfo.hStdOutput = ClientHandles[1];
    si.StartupInfo.hStdError = ClientHandles[2];

    SIZE_T attrSize;
    InitializeProcThreadAttributeList(NULL, 2, 0, &attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
    ok = InitializeProcThreadAttributeList(si.lpAttributeList, 2, 0, &attrSize);
    Assert(ok);
    /*
    hr = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, con, sizeof(con), NULL, NULL);
    Assert(SUCCEEDED(hr));
    */
    ok = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE, &ReferenceHandle, sizeof(ReferenceHandle), NULL, NULL);
    Assert(ok);
    ok = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, ClientHandles, sizeof(ClientHandles), NULL, NULL);
    Assert(ok);

    PROCESS_INFORMATION pi;
    ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi);
    Assert(ok);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(ReferenceHandle);
    CloseHandle(ClientHandles[0]);
    CloseHandle(ClientHandles[1]);
    CloseHandle(ClientHandles[2]);
}

DWORD WINAPI WindowThread(LPVOID lpParameter) {
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"HandTerminalClass";
    ATOM atom = RegisterClassExW(&wc);
    Assert(atom);

    // window properties - width, height and style
    DWORD exstyle = WS_EX_APPWINDOW;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VSCROLL;

    window = CreateWindowExW(exstyle, wc.lpszClassName, L"Handterm", style, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, wc.hInstance, NULL);
    Assert(window);
    // show the window
    ShowWindow(window, SW_SHOWDEFAULT);
    SetFocus(window);

    SetEvent(window_ready_event);

    bool events_added = false;
    MSG keydown_msg;
    while (!console_exit) {
        MSG msg;
        // OutputDebugStringA("Peekmsg");
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                break;
            }
            if (msg.message == WM_KEYDOWN)
            {
                keydown_msg = msg;
                INPUT_RECORD key_event;
                key_event.EventType = KEY_EVENT;
                key_event.Event.KeyEvent.wVirtualKeyCode = LOWORD(keydown_msg.wParam);
                key_event.Event.KeyEvent.wVirtualScanCode = LOBYTE(HIWORD(keydown_msg.lParam));
                key_event.Event.KeyEvent.bKeyDown = true;
                key_event.Event.KeyEvent.wRepeatCount = LOWORD(msg.lParam);
                key_event.Event.KeyEvent.uChar.UnicodeChar = 0;
                PushEvent(key_event);
                events_added = true;
            }
            if (msg.message == WM_CHAR)
            {
                INPUT_RECORD key_event;
                key_event.EventType = KEY_EVENT;
                key_event.Event.KeyEvent.wVirtualKeyCode = LOWORD(keydown_msg.wParam);
                key_event.Event.KeyEvent.wVirtualScanCode = LOBYTE(HIWORD(keydown_msg.lParam));
                key_event.Event.KeyEvent.bKeyDown = true;
                key_event.Event.KeyEvent.wRepeatCount = LOWORD(msg.lParam);
                key_event.Event.KeyEvent.uChar.UnicodeChar = msg.wParam;
                PushEvent(key_event);
                events_added = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);

            char dbg_buf[256];
            //sprintf(dbg_buf, "Got message %d", msg.message);
            //OutputDebugStringA(dbg_buf);
            continue;
        }
        if (events_added) {
            events_added = false;
            SetEvent(delayed_io_event);
        }
        Sleep(10);
    }
    return 0;
}

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
    HRESULT hr;

    window_ready_event = CreateEventExW(0, 0, 0, EVENT_ALL_ACCESS);
    CreateThread(0, 0, WindowThread, 0, 0, 0);
    WaitForSingleObject(window_ready_event, INFINITE);

    // create swap chain, device and context
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        UINT flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, _countof(levels), D3D11_SDK_VERSION, &desc, &swapChain, &device, NULL, &context);
        AssertHR(hr);
    }

#ifndef NDEBUG
    {
        ID3D11Debug* debug;
        ID3D11InfoQueue* info;
        hr = device->QueryInterface(&debug);
        AssertHR(hr);
        hr = debug->QueryInterface(&info);
        AssertHR(hr);
        hr = info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        AssertHR(hr);
        hr = info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        AssertHR(hr);
        info->Release();
        debug->Release();
    }
#endif

    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = MAX_WIDTH * MAX_HEIGHT * sizeof(Vertex);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = device->CreateBuffer(&desc, NULL, &vbuffer);
        AssertHR(hr);
    }

    {
        // these must match vertex shader input layout
        D3D11_INPUT_ELEMENT_DESC desc[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex, pos),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, offsetof(Vertex, uv),    D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM,    0, offsetof(Vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BACKG", 0, DXGI_FORMAT_R8G8B8A8_UNORM,    0, offsetof(Vertex, backg), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        const char hlsl[] =
            "#line " STR(__LINE__) "                                    \n\n"
            "                                                             \n"
            "struct VS_INPUT                                              \n"
            "{                                                            \n"
            "     float2 pos   : POSITION;                                \n"
            "     float2 uv    : TEXCOORD;                                \n"
            "     float4 color : COLOR;                                   \n"
            "     float4 backg : BACKG;                                   \n"
            "};                                                           \n"
            "                                                             \n"
            "struct PS_INPUT                                              \n"
            "{                                                            \n"
            "  float4 pos   : SV_POSITION;                                \n"
            "  float2 uv    : TEXCOORD;                                   \n"
            "  float4 color : COLOR;                                      \n"
            "  float4 backg : BACKG;                                      \n"
            "};                                                           \n"
            "                                                             \n"
            "cbuffer cbuffer0 : register(b0)                              \n"
            "{                                                            \n"
            "    float4x4 uTransform;                                     \n"
            "}                                                            \n"
            "                                                             \n"
            "sampler sampler0 : register(s0);                             \n"
            "                                                             \n"
            "Texture2D<float3> texture0 : register(t0);                   \n"
            "                                                             \n"
            "PS_INPUT vs(VS_INPUT input)                                  \n"
            "{                                                            \n"
            "    PS_INPUT output;                                         \n"
            "    output.pos = mul(uTransform, float4(input.pos, 0, 1));   \n"
            "    output.uv = input.uv;                                    \n"
            "    output.color = input.color;                              \n"
            "    output.backg = input.backg;                              \n"
            "    return output;                                           \n"
            "}                                                            \n"
            "                                                             \n"
            "float4 ps(PS_INPUT input) : SV_TARGET                        \n"
            "{                                                            \n"
            "    float tex = texture0.Sample(sampler0, input.uv).r;       \n"
            "    return lerp(input.backg, input.color, tex);              \n"
            "}                                                            \n"
            ;

        UINT flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifndef NDEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ID3DBlob* error;

        ID3DBlob* vblob;
        hr = D3DCompile(hlsl, sizeof(hlsl), NULL, NULL, NULL, "vs", "vs_5_0", flags, 0, &vblob, &error);
        if (FAILED(hr))
        {
            const char* message = (const char*)error->GetBufferPointer();
            OutputDebugStringA(message);
            Assert(!"Failed to compile vertex shader!");
        }

        ID3DBlob* pblob;
        hr = D3DCompile(hlsl, sizeof(hlsl), NULL, NULL, NULL, "ps", "ps_5_0", flags, 0, &pblob, &error);
        if (FAILED(hr))
        {
            const char* message = (const char*)error->GetBufferPointer();
            OutputDebugStringA(message);
            Assert(!"Failed to compile pixel shader!");
        }

        hr = device->CreateVertexShader(vblob->GetBufferPointer(), vblob->GetBufferSize(), NULL, &vshader);
        AssertHR(hr);

        hr = device->CreatePixelShader(pblob->GetBufferPointer(), pblob->GetBufferSize(), NULL, &pshader);
        AssertHR(hr);

        hr = device->CreateInputLayout(desc, _countof(desc), vblob->GetBufferPointer(), vblob->GetBufferSize(), &layout);
        AssertHR(hr);

        pblob->Release();
        vblob->Release();
    }

    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = 4 * 4 * sizeof(float);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&desc, NULL, &ubuffer);
        AssertHR(hr);
    }

    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = 256 * 8;
        info.bmiHeader.biHeight = 16;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits;
        HDC dc = CreateCompatibleDC(0);
        HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(dc, bmp);

        HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH, "PxPlus IBM VGA8");
        SelectObject(dc, font);

        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, RGB(0, 0, 0));
        for (int i = 0; i < 256; i++)
        {
            RECT r = { i * 8, 0, i * 8 + 8, 16 };
            char ch = (char)i;
            ExtTextOutA(dc, i * 8, 0, ETO_OPAQUE, &r, &ch, 1, NULL);
        }

        {
            // cursor, doesn't look right in A version
            RECT r = { 0xdb * 8, 0, 0xdb * 8 + 8, 16 };
            wchar_t ch = 0x2588;
            ExtTextOutW(dc, 0xdb * 8, 0, ETO_OPAQUE, &r, &ch, 1, NULL);
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 256 * 8;
        desc.Height = 16;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = bits;
        data.SysMemPitch = 256 * 8 * 4;

        ID3D11Texture2D* texture;
        hr = device->CreateTexture2D(&desc, &data, &texture);
        AssertHR(hr);

        hr = device->CreateShaderResourceView(texture, NULL, &textureView);
        AssertHR(hr);

        texture->Release();
    }

    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;

        hr = device->CreateSamplerState(&desc, &sampler);
        AssertHR(hr);
    }

    // disable culling
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        hr = device->CreateRasterizerState(&desc, &rasterizerState);
        AssertHR(hr);
    }

    LARGE_INTEGER freq, time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&time);

    DWORD frames = 0;
    INT64 next = time.QuadPart + freq.QuadPart;

    static DWORD currentWidth = 0;
    static DWORD currentHeight = 0;
    bool events_added = false;
    MSG keydown_msg;

    console_mutex = CreateMutex(0, false, L"ConsoleMutex");
    Assert(console_mutex);

    // setup terminal dimensions here so that process already can write out correctly
    RECT rect;
    GetClientRect(window, &rect);
    DWORD width = rect.right - rect.left;
    DWORD height = rect.bottom - rect.top;
    FastFast_Resize((SHORT)(width / 8), (SHORT)(height / 16));

    SetupConsoleAndProcess();

    while(!console_exit)
    {
        RECT rect;
        GetClientRect(window, &rect);
        DWORD width = rect.right - rect.left;
        DWORD height = rect.bottom - rect.top;

        if (rtView == NULL || width != currentWidth || height != currentHeight)
        {
            if (rtView)
            {
                // release old swap chain buffers
                context->ClearState();
                rtView->Release();
                rtView = NULL;
            }

            if (width != 0 && height != 0)
            {
                hr = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
                AssertHR(hr);

                ID3D11Texture2D* backbuffer;
                hr = swapChain->GetBuffer(0, IID_ID3D11Texture2D, (void**)&backbuffer);
                AssertHR(hr);
                hr = device->CreateRenderTargetView(backbuffer, NULL, &rtView);
                AssertHR(hr);
                backbuffer->Release();
            }

            currentWidth = width;
            currentHeight = height;

            FastFast_LockTerminal();
            FastFast_Resize((SHORT)(currentWidth / 8), (SHORT)(currentHeight / 16));
            FastFast_UnlockTerminal();
        }

        if (rtView)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart > next)
            {
                show_cursor = !show_cursor;
                next = now.QuadPart + freq.QuadPart;

                float fps = float(double(frames) * freq.QuadPart / (now.QuadPart - time.QuadPart));
                time = now;
                frames = 0;

                WCHAR title[1024];
                _snwprintf(title, _countof(title), L"Handterm Size=%dx%d RenderFPS=%.2f", frontbuffer.size.X, frontbuffer.size.Y, fps);
                SetWindowTextW(window, title);
            }

            D3D11_VIEWPORT viewport = {};
            viewport.Width = (FLOAT)width;
            viewport.Height = (FLOAT)height;
            viewport.MinDepth = 0;
            viewport.MaxDepth = 1;

            {
                float l = 0;
                float r = (float)width;
                float t = 0;
                float b = (float)height;

                float x = 2.0f / (r - l);
                float y = 2.0f / (t - b);
                float z = 0.5f;

                float matrix[4 * 4] =
                {
                    x, 0, 0, (r + l) / (l - r),
                    0, y, 0, (t + b) / (b - t),
                    0, 0, z, 0.5f,
                    0, 0, 0, 1.0f,
                };

                D3D11_MAPPED_SUBRESOURCE mapped;
                hr = context->Map(ubuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                AssertHR(hr);
                memcpy(mapped.pData, matrix, sizeof(matrix));
                context->Unmap(ubuffer, 0);
            }

            UINT count = 0;
            {
                D3D11_MAPPED_SUBRESOURCE mapped;
                hr = context->Map(vbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                AssertHR(hr);

                Vertex* vtx = (Vertex*)mapped.pData;

#if 0
                static int backg = 1;
                for (int y = 0; y < termH; y++)
                {
                    for (int x = 0; x < termW; x++)
                    {
                        static char ch = 0;
                        vtx = AddChar(vtx, ch++, x, y, 0xffffff, backg++);
                    }
                }
#else
                // may set incorrect values since it's done without lock, 
                // but it can't be under lock since then we can deadlock with DefWindowProc
                SCROLLINFO si = { 0 };
                si.cbSize = sizeof(si);
                si.fMask = SIF_ALL;
                si.nMin = 0;
                si.nMax = frontbuffer.scrollback_available;
                si.nPos = frontbuffer.scrollback_available - frontbuffer.scrollback;
                SetScrollInfo(window, SB_VERT, &si, true);

                FastFast_LockTerminal();
                TermChar* t = check_wrap(&frontbuffer, frontbuffer.buffer - frontbuffer.size.X * frontbuffer.scrollback);
                TermChar* t_wrap = get_wrap_ptr(&frontbuffer);
                for (int y = 0; y < frontbuffer.size.Y; y++)
                {
                    for (int x = 0; x < frontbuffer.size.X; x++)
                    {
                        vtx = AddChar(vtx, (char)t->ch, x, y, t->attr.color, t->attr.backg);
                        t++;
                    }
                    if (t >= t_wrap) {
                        t = frontbuffer.buffer_start;
                    }
                }
                if (show_cursor) {
                    vtx = AddChar(vtx, 0xdb, frontbuffer.cursor_pos.X, frontbuffer.cursor_pos.Y + frontbuffer.scrollback, 0xffffff, 0);
                }
                FastFast_UnlockTerminal();
#endif
                count = (UINT)(vtx - (Vertex*)mapped.pData);

                context->Unmap(vbuffer, 0);
            }

            float clear_color[4] = { 0, 0, 0, 1 };
            context->ClearRenderTargetView(rtView, clear_color);
            context->IASetInputLayout(layout);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT stride = sizeof(struct Vertex);
            UINT offset = 0;
            context->IASetVertexBuffers(0, 1, &vbuffer, &stride, &offset);

            context->VSSetConstantBuffers(0, 1, &ubuffer);
            context->VSSetShader(vshader, NULL, 0);

            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizerState);

            context->PSSetSamplers(0, 1, &sampler);
            context->PSSetShaderResources(0, 1, &textureView);
            context->PSSetShader(pshader, NULL, 0);

            context->OMSetRenderTargets(1, &rtView, NULL);

            context->Draw(count, 0);

            frames++;
        }

        BOOL vsync = TRUE;
        hr = swapChain->Present(vsync ? 1 : 0, 0);
        if (hr == DXGI_STATUS_OCCLUDED)
        {
            // window is minimized, cannot vsync - instead sleep a bit
            if (vsync)
            {
                Sleep(10);
            }
        }
        AssertHR(hr);
    }
}