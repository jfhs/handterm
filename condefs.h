/*
Copyright (c) Microsoft Corporation. All rights reserved.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#pragma once

#include <windows.h>
#include "condrv.h"
#include "conmsgl1.h"
#include "conmsgl2.h"
#include "conmsgl3.h"

typedef enum ControlType
{
    ConsoleSetVDMCursorBounds,
    ConsoleNotifyConsoleApplication,
    ConsoleFullscreenSwitch,
    ConsoleSetCaretInfo,
    ConsoleSetReserveKeys,
    ConsoleSetForeground,
    ConsoleSetWindowOwner,
    ConsoleEndTask,
} ControlType;

#define STATUS_SUCCESS ((DWORD)0x0)
#define STATUS_UNSUCCESSFUL ((DWORD)0xC0000001L)
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043L)
#define STATUS_INSUFFICIENT_RESOURCES ((DWORD)0xC000009AL)
#define STATUS_ILLEGAL_FUNCTION ((DWORD)0xC00000AFL)
#define STATUS_PIPE_DISCONNECTED ((DWORD)0xC00000B0L)
#define STATUS_BUFFER_TOO_SMALL ((DWORD)0xC0000023L)
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)


typedef NTSTATUS(NTAPI* PfnNtOpenFile)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
typedef NTSTATUS(WINAPI* PfnConsoleControl)(ControlType Command, PVOID Information, DWORD Length);

extern PfnNtOpenFile _NtOpenFile;
extern PfnConsoleControl _ConsoleControl;

/*
    * Console window startup optimization.
*/
#define CPI_NEWPROCESSWINDOW 0x0001

typedef struct _CONSOLE_PROCESS_INFO
{
    IN DWORD dwProcessID;
    IN DWORD dwFlags;
} CONSOLE_PROCESS_INFO, * PCONSOLE_PROCESS_INFO;

typedef struct _CONSOLEWINDOWOWNER
{
    HWND hwnd;
    ULONG ProcessId;
    ULONG ThreadId;
} CONSOLEWINDOWOWNER, * PCONSOLEWINDOWOWNER;

#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020

#define ProcThreadAttributeConsoleReference 10

#define PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE \
    ProcThreadAttributeValue(10, FALSE, TRUE, FALSE)

typedef struct CONSOLE_API_MSG {
    CD_IO_DESCRIPTOR Descriptor;
    union
    {
        struct
        {
            CD_CREATE_OBJECT_INFORMATION CreateObject;
            CONSOLE_CREATESCREENBUFFER_MSG CreateScreenBuffer;
        };
        struct
        {
            CONSOLE_MSG_HEADER msgHeader;
            union
            {
                CONSOLE_MSG_BODY_L1 consoleMsgL1;
                CONSOLE_MSG_BODY_L2 consoleMsgL2;
                CONSOLE_MSG_BODY_L3 consoleMsgL3;
            } u;
        };
    };
} CONSOLE_API_MSG, * PCONSOLE_API_MSG;

NTSTATUS _CreateHandle(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR DeviceName,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ HANDLE Parent,
    _In_ BOOLEAN Inheritable,
    _In_ ULONG OpenOptions);

NTSTATUS CreateClientHandle(
    _Out_ PHANDLE Handle,
    _In_ HANDLE ServerHandle,
    _In_ PCWSTR Name,
    _In_ BOOLEAN Inheritable);

NTSTATUS CreateServerHandle(
    _Out_ PHANDLE Handle,
    _In_ BOOLEAN Inheritable);

#define FG_ATTRS (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY)
#define BG_ATTRS (BACKGROUND_BLUE | BACKGROUND_GREEN | BACKGROUND_RED | BACKGROUND_INTENSITY)
#define META_ATTRS (COMMON_LVB_LEADING_BYTE | COMMON_LVB_TRAILING_BYTE | COMMON_LVB_GRID_HORIZONTAL | COMMON_LVB_GRID_LVERTICAL | COMMON_LVB_GRID_RVERTICAL | COMMON_LVB_REVERSE_VIDEO | COMMON_LVB_UNDERSCORE)


typedef struct _CONSOLEENDTASK
{
    HANDLE ProcessId;
    HWND hwnd;
    ULONG ConsoleEventCode;
    ULONG ConsoleFlags;
} CONSOLEENDTASK, * PCONSOLEENDTASK;

#define CONSOLE_CTRL_C_FLAG 0x00000001
#define CONSOLE_CTRL_BREAK_FLAG 0x00000002
#define CONSOLE_CTRL_CLOSE_FLAG 0x00000004

#define CONSOLE_CTRL_LOGOFF_FLAG 0x00000010
#define CONSOLE_CTRL_SHUTDOWN_FLAG 0x00000020