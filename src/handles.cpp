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
#include "condefs.h"

/*++
Routine Description:
- This routine opens a handle to the console driver.

Arguments:
- Handle - Receives the handle.
- DeviceName - Supplies the name to be used to open the console driver.
- DesiredAccess - Supplies the desired access mask.
- Parent - Optionally supplies the parent object.
- Inheritable - Supplies a boolean indicating if the new handle is to be made inheritable.
- OpenOptions - Supplies the open options to be passed to NtOpenFile. A common
                option for clients is FILE_SYNCHRONOUS_IO_NONALERT, to make the handle
                synchronous.

Return Value:
- NTSTATUS indicating if the handle was successfully created.
--*/
NTSTATUS _CreateHandle(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR DeviceName,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ HANDLE Parent,
    _In_ BOOLEAN Inheritable,
    _In_ ULONG OpenOptions)

{
    ULONG Flags = OBJ_CASE_INSENSITIVE;

    if (Inheritable)
    {
        Flags |= OBJ_INHERIT;
    }

    UNICODE_STRING Name;
#pragma warning(suppress : 26492) // const_cast is prohibited, but we can't avoid it for filling UNICODE_STRING.
    Name.Buffer = (wchar_t*)DeviceName;
    Name.Length = (USHORT)((wcslen(DeviceName) * sizeof(wchar_t)));
    Name.MaximumLength = Name.Length;

    OBJECT_ATTRIBUTES ObjectAttributes;
#pragma warning(suppress : 26477) // The QOS part of this macro in the define is 0. Can't fix that.
    InitializeObjectAttributes(&ObjectAttributes,
        &Name,
        Flags,
        Parent,
        0);

    IO_STATUS_BLOCK IoStatus;
    return _NtOpenFile(Handle,
        DesiredAccess,
        &ObjectAttributes,
        &IoStatus,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OpenOptions);
}

/*++
Routine Description:
- This routine creates a handle to an input or output client of the given
  server. No control io is sent to the server as this request must be coming
  from the server itself.

Arguments:
- Handle - Receives a handle to the new client.
- ServerHandle - Supplies a handle to the server to which to attach the
                 newly created client.
- Name - Supplies the name of the client object.
- Inheritable - Supplies a flag indicating if the handle must be inheritable.

Return Value:
- NTSTATUS indicating if the client was successfully created.
--*/
NTSTATUS CreateClientHandle(
    _Out_ PHANDLE Handle,
    _In_ HANDLE ServerHandle,
    _In_ PCWSTR Name,
    _In_ BOOLEAN Inheritable)
{
    return _CreateHandle(Handle,
        Name,
        GENERIC_WRITE | GENERIC_READ | SYNCHRONIZE,
        ServerHandle,
        Inheritable,
        FILE_SYNCHRONOUS_IO_NONALERT);
}

/*++
Routine Description:
- This routine creates a new server on the driver and returns a handle to it.

Arguments:
- Handle - Receives a handle to the new server.
- Inheritable - Supplies a flag indicating if the handle must be inheritable.

Return Value:
- NTSTATUS indicating if the console was successfully created.
--*/
NTSTATUS CreateServerHandle(
    _Out_ PHANDLE Handle,
    _In_ BOOLEAN Inheritable)
{
    return _CreateHandle(Handle,
        L"\\Device\\ConDrv\\Server",
        GENERIC_ALL,
        0,
        Inheritable,
        0);
}