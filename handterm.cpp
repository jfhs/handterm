#define _CRT_SECURE_NO_DEPRECATE
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <intrin.h>
#include <winternl.h>
#include <stdlib.h>
#include <stdbool.h>
#include "condefs.h"

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
HANDLE console_mutex;
HANDLE ServerHandle, ReferenceHandle, InputEventHandle;
#define MAX_WIDTH 4096
#define MAX_HEIGHT 4096

static int termColor = 0xffffff;
static int termBackg = 0x000000;
static int termH = 1;
static int termW = 1;

static int termPosX = 0;
static int termPosY = 0;

ULONG input_console_mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION;
ULONG output_console_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
COORD cursor_pos = { 0 };
COORD size = { 100, 20 };

typedef struct TermChar
{
    int ch;
    int color;
    int backg;
} TermChar;
static TermChar terminal[MAX_WIDTH * MAX_HEIGHT];

INPUT_RECORD pending_events[256];
volatile PINPUT_RECORD pending_events_write_ptr = pending_events;
volatile PINPUT_RECORD pending_events_read_ptr = pending_events;
PINPUT_RECORD pending_events_wrap_ptr = pending_events + sizeof(pending_events)/sizeof(pending_events[0]);

bool console_exit = false;

wchar_t console_title[256] = L"Handterm";

CONSOLE_API_MSG delayed_io_msg;
bool has_delayed_io_msg = false;
HANDLE delayed_io_event;

COORD line_input_saved_cursor = { 0 };
bool show_cursor = true;

// END GLOBALS

HRESULT PushEvent(INPUT_RECORD ev) {
    // buffer full
    if (pending_events_write_ptr + 1 == pending_events_read_ptr) {
        return STATUS_UNSUCCESSFUL;
    }
    *pending_events_write_ptr = ev;
    if (++pending_events_write_ptr == pending_events_wrap_ptr)
        pending_events_write_ptr = pending_events;
    SetEvent(InputEventHandle);
    return STATUS_SUCCESS;
}

void FastFast_LockTerminal() {
    WaitForSingleObject(console_mutex, INFINITE);
}
void FastFast_UnlockTerminal() {
    ReleaseMutex(console_mutex);
}

void FastFast_Resize(SHORT width, SHORT height) {
    FastFast_LockTerminal();
    size.X = width;
    size.Y = height;
    INPUT_RECORD resize_event = {};
    resize_event.EventType = WINDOW_BUFFER_SIZE_EVENT;
    resize_event.Event.WindowBufferSizeEvent.dwSize = size;
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

void scroll_up(UINT lines) {
    cursor_pos.Y -= lines;
    memmove(terminal, terminal + lines * size.X, size.X * (size.Y - lines) * sizeof(terminal[0]));
    ZeroMemory(terminal + size.X * (size.Y - lines), lines * size.X * sizeof(terminal[0]));
}

void FastFast_UpdateTerminalW(const wchar_t* str, SIZE_T count)
{
    str = str;
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
                    termColor = r | (g << 8) | (b << 16);
                }
                else {
                    termBackg = r | (g << 8) | (b << 16);
                }
            }
            str += pos + 1;
            count -= pos + 1;
        }
        else
        {
            // todo: also do that if cursor moves beyond and WRAP_AT_EOL_OUTPUT is enabled
            if (cursor_pos.Y >= size.Y) {
                scroll_up(cursor_pos.Y - size.Y + 1);
            }
            if (cursor_pos.X >= 0 && cursor_pos.X < size.X && cursor_pos.Y >= 0 && cursor_pos.Y < size.Y)
            {
                TermChar* t = &terminal[cursor_pos.Y * size.X + cursor_pos.X];
                t->ch = *str;
                t->color = termColor;
                t->backg = termBackg;

                cursor_pos.X++;
                if (cursor_pos.X == size.X || t->ch == L'\n')
                {
                    cursor_pos.X = 0;
                    cursor_pos.Y++;
                }
            }
            str++;
            count--;
        }
    }
}

void FastFast_UpdateTerminalA(const char* str, SIZE_T count)
{
    str = str;
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
            }
            else if (str[pos] == 'm')
            {
                BOOL fg = str[1] == L'3';
                const char* s = str + 6;
                int r = num(s);
                s++;
                int g = num(s);
                s++;
                int b = num(s);

                if (fg) {
                    termColor = r | (g << 8) | (b << 16);
                }
                else {
                    termBackg = r | (g << 8) | (b << 16);
                }
            }
            str += pos + 1;
            count -= pos + 1;
        }
        else
        {
            // todo: also do that if cursor moves beyond and WRAP_AT_EOL_OUTPUT is enabled
            if (cursor_pos.Y >= size.Y) {
                scroll_up(cursor_pos.Y - size.Y + 1);
            }
            if (cursor_pos.X >= 0 && cursor_pos.X < size.X && cursor_pos.Y >= 0 && cursor_pos.Y < size.Y)
            {
                TermChar* t = &terminal[cursor_pos.Y * size.X + cursor_pos.X];
                t->ch = *str;
                t->color = termColor;
                t->backg = termBackg;

                cursor_pos.X++;
                if (cursor_pos.X == size.X || t->ch == '\n')
                {
                    cursor_pos.X = 0;
                    cursor_pos.Y++;
                }
            }
            str++;
            count--;
        }
    }
}

thread_local char* buf = 0;
thread_local size_t buf_size = 0;

HRESULT HandleReadMessage(PCONSOLE_API_MSG ReceiveMsg, PCD_IO_COMPLETE io_complete) {
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;

    // todo: these can now be modified/read by 3 threads, probably should lock :|
    PINPUT_RECORD write_ptr = pending_events_write_ptr;
    PINPUT_RECORD read_ptr = pending_events_read_ptr;
    if (read_ptr != write_ptr) {
        PCONSOLE_READCONSOLE_MSG read_msg = &ReceiveMsg->u.consoleMsgL1.ReadConsoleW;

        if (read_msg->InitialNumBytes) {
            // not supported
            return STATUS_UNSUCCESSFUL;
        }

        size_t bytes = 0;
        size_t buf_idx = 0;
        bool has_cr = false;
        wchar_t* wbuf = (wchar_t*)buf;
        bool line_mode_enabled = input_console_mode & ENABLE_LINE_INPUT;
        while (read_ptr != write_ptr) {
            // it's expected that in this mode we just skip other events
            if (read_ptr->EventType == KEY_EVENT && read_ptr->Event.KeyEvent.uChar.UnicodeChar) {
                // todo: support repeat
                Assert(read_ptr->Event.KeyEvent.wRepeatCount == 1);

                bool is_cr;
                if (read_msg->Unicode) {
                    is_cr = read_ptr->Event.KeyEvent.uChar.UnicodeChar == L'\r';
                } else {
                    is_cr = read_ptr->Event.KeyEvent.uChar.AsciiChar == '\r';
                }
                has_cr |= is_cr;
                bool will_add_lf = is_cr && line_mode_enabled;

                bytes += (will_add_lf ? 2 : 1) * (read_msg->Unicode ? 2 : 1);
                if (bytes > buf_size) {
                    buf_size = max(buf_size, 128) * 2;
                    buf = (char*)realloc(buf, buf_size);
                    wbuf = (wchar_t*)buf;
                }
                if (read_msg->Unicode) {
                    wbuf[buf_idx++] = read_ptr->Event.KeyEvent.uChar.UnicodeChar;
                    if (will_add_lf) {
                        wbuf[buf_idx++] = L'\n';
                    }
                } else {
                    buf[buf_idx++] = read_ptr->Event.KeyEvent.uChar.AsciiChar;
                    if (will_add_lf) {
                        buf[buf_idx++] = '\n';
                    }
                }
            }
            if (++read_ptr == pending_events_wrap_ptr) {
                read_ptr = pending_events;
            }
        }

        if (bytes && (input_console_mode & ENABLE_ECHO_INPUT)) {
            FastFast_LockTerminal();
            cursor_pos = line_input_saved_cursor;
            if (read_msg->Unicode) {
                FastFast_UpdateTerminalW(wbuf, buf_idx);
            }
            else {
                FastFast_UpdateTerminalA(buf, buf_idx);
            }
            FastFast_UnlockTerminal();
        }

        if (!bytes || (!has_cr && input_console_mode & ENABLE_LINE_INPUT)) {
            return STATUS_TIMEOUT;
        }

        CD_IO_OPERATION write_op = { 0 };
        write_op.Identifier = ReceiveMsg->Descriptor.Identifier;
        write_op.Buffer.Offset = ReceiveMsg->msgHeader.ApiDescriptorSize;
        write_op.Buffer.Data = buf;
        write_op.Buffer.Size = bytes;

        ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_WRITE_OUTPUT, &write_op, sizeof(write_op), 0, 0, &BytesRead, 0);
        hr = ok ? S_OK : GetLastError();
        Assert(SUCCEEDED(hr));

        pending_events_read_ptr = read_ptr;
        // todo: this is most likely not thread safe
        if (pending_events_read_ptr == pending_events_write_ptr) {
            ResetEvent(InputEventHandle);
        }

        io_complete->IoStatus.Information = read_msg->NumBytes = bytes;
    } else {
        return STATUS_TIMEOUT;
    }
}

DWORD WINAPI DelayedIoThread(LPVOID lpParameter)
{
    DWORD BytesRead = 0;
    HRESULT hr;
    BOOL ok;
    CD_IO_COMPLETE io_complete;

    while (!console_exit) {
        WaitForSingleObject(delayed_io_event, INFINITE);
        if (!has_delayed_io_msg) {
            continue;
        }
        ZeroMemory(&io_complete, sizeof(io_complete));
        io_complete.Identifier = delayed_io_msg.Descriptor.Identifier;
        io_complete.Write.Data = &delayed_io_msg.u;
        io_complete.Write.Size = delayed_io_msg.msgHeader.ApiDescriptorSize;

        switch (delayed_io_msg.Descriptor.Function) {
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
                    PCONSOLE_WRITECONSOLE_MSG write_msg = &ReceiveMsg.u.consoleMsgL1.WriteConsoleW; // this WriteConsole macro...
                    size_t read_offset = ReceiveMsg.msgHeader.ApiDescriptorSize + sizeof(CONSOLE_MSG_HEADER);
                    size_t bytes = ReceiveMsg.Descriptor.InputSize - read_offset;
                    if (buf_size < bytes) {
                        buf = (char*)realloc(buf, bytes);
                        buf_size = bytes;
                    }
                    CD_IO_OPERATION read_op = { 0 };
                    read_op.Identifier = ReceiveMsg.Descriptor.Identifier;
                    read_op.Buffer.Offset = read_offset;
                    read_op.Buffer.Data = buf;
                    read_op.Buffer.Size = bytes;
                    ok = DeviceIoControl(ServerHandle, IOCTL_CONDRV_READ_INPUT, &read_op, sizeof(read_op), 0, 0, &BytesRead, 0);
                    hr = ok ? S_OK : GetLastError();
                    Assert(SUCCEEDED(hr));
                    FastFast_LockTerminal();
                    if (write_msg->Unicode) {
                        FastFast_UpdateTerminalW((wchar_t*)buf, bytes / 2);
                    } else {
                        FastFast_UpdateTerminalA(buf, bytes);
                    }
                    FastFast_UnlockTerminal();

                    io_complete.IoStatus.Status = STATUS_SUCCESS;
                    io_complete.IoStatus.Information = write_msg->NumBytes = bytes;

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
                        line_input_saved_cursor = cursor_pos;
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
                    buf_info_msg->CursorPosition = cursor_pos;
                    buf_info_msg->MaximumWindowSize = size;
                    buf_info_msg->Size = size;
                    buf_info_msg->CurrentWindowSize = size;
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
                default:
                    sprintf(dbg_buf, "Unhandled L2 request %d\n", ApiNumber);
                    OutputDebugStringA(dbg_buf);
                }
            } else {
                sprintf(dbg_buf, "Unhandled L3 request %d\n", ApiNumber);
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

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wparam, lparam);
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
    console_mutex = CreateMutex(0, false, L"ConsoleMutex");
    Assert(console_mutex);

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

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
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
    DWORD style = WS_OVERLAPPEDWINDOW;

    HWND window = CreateWindowExW(exstyle, wc.lpszClassName, L"Handterm", style, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, wc.hInstance, NULL);
    Assert(window);

    HRESULT hr;

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

    // show the window
    ShowWindow(window, SW_SHOWDEFAULT);

    LARGE_INTEGER freq, time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&time);

    DWORD frames = 0;
    INT64 next = time.QuadPart + freq.QuadPart;

    static DWORD currentWidth = 0;
    static DWORD currentHeight = 0;
    bool events_added = false;
    MSG keydown_msg;

    // setup terminal dimensions here so that process already can write out correctly
    RECT rect;
    GetClientRect(window, &rect);
    DWORD width = rect.right - rect.left;
    DWORD height = rect.bottom - rect.top;
    termW = width / 8;
    termH = height / 16;
    FastFast_Resize((SHORT)termW, (SHORT)termH);

    SetupConsoleAndProcess();

    while(!console_exit)
    {
        MSG msg;
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
            continue;
        }
        if (events_added) {
            SetEvent(delayed_io_event);
        }

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

            termPosX = 0;
            termPosY = 0;
            termW = currentWidth / 8;
            termH = currentHeight / 16;
            // memset(terminal, 0, sizeof(terminal));

            FastFast_LockTerminal();
            FastFast_Resize((SHORT)termW, (SHORT)termH);
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
                _snwprintf(title, _countof(title), L"Handterm Size=%dx%d RenderFPS=%.2f", termW, termH, fps);
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
                FastFast_LockTerminal();
                TermChar* t = terminal;
                for (int y = 0; y < termH; y++)
                {
                    for (int x = 0; x < termW; x++)
                    {
                        vtx = AddChar(vtx, (char)t->ch, x, y, t->color, t->backg);
                        t++;
                    }
                }
                if (show_cursor) {
                    vtx = AddChar(vtx, 0xdb, cursor_pos.X, cursor_pos.Y, 0xffffff, 0);
                }
                FastFast_UnlockTerminal();
#endif
                count = (UINT)(vtx - (Vertex*)mapped.pData);

                context->Unmap(vbuffer, 0);
            }

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