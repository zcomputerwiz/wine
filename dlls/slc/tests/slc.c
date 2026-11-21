/*
 * Copyright 2014 Sebastian Lackner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "slpublic.h"
#include "slerror.h"

#include <wine/test.h>

static void test_SLGetWindowsInformationDWORD(void)
{
    DWORD value;
    HRESULT res;

    res = SLGetWindowsInformationDWORD(L"Nonexistent-License-Value", NULL);
    ok(res == E_INVALIDARG, "expected E_INVALIDARG, got %08lx\n", res);

    res = SLGetWindowsInformationDWORD(NULL, &value);
    ok(res == E_INVALIDARG, "expected E_INVALIDARG, got %08lx\n", res);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Nonexistent-License-Value", &value);
    ok(res == SL_E_VALUE_NOT_FOUND, "expected SL_E_VALUE_NOT_FOUND, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"", &value);
    ok(res == SL_E_RIGHT_NOT_GRANTED || broken(res == 0xd000000d) /* Win 8 */,
       "expected SL_E_RIGHT_NOT_GRANTED, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Kernel-MUI-Language-Allowed", &value);
    ok(res == SL_E_DATATYPE_MISMATCHED, "expected SL_E_DATATYPE_MISMATCHED, got %08lx\n", res);
    ok(value == 0xdeadbeef, "expected value = 0xdeadbeef, got %lu\n", value);

    value = 0xdeadbeef;
    res = SLGetWindowsInformationDWORD(L"Kernel-MUI-Number-Allowed", &value);
    ok(res == S_OK, "expected S_OK, got %lu\n", res);
    ok(value != 0xdeadbeef, "expected value != 0xdeadbeef\n");
}

static void test_SLRegisterEvent(void)
{
    static const SLID windows_app = {0x55c92734, 0xd682, 0x4d71, {0x98, 0x3e, 0xd6, 0xec, 0x3f, 0x16, 0x05, 0x9f}};
    static const SLID other_app = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    HANDLE event, event2;
    HSLC slc;
    HRESULT hr;

    hr = SLOpen(&slc);
    ok(hr == S_OK, "SLOpen returned %08lx\n", hr);
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    event2 = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = SLRegisterEvent(NULL, SL_EVENT_USER_NOTIFICATION, &windows_app, event2);
    ok(hr == S_OK, "no session handle: got %08lx\n", hr);
    hr = SLUnregisterEvent(NULL, SL_EVENT_USER_NOTIFICATION, &windows_app, event2);
    ok(hr == S_OK, "no session handle: got %08lx\n", hr);

    hr = SLRegisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, event);
    /* 32-bit slc on 64-bit Windows 11 refuses a registration through an SLOpen handle */
    ok(hr == S_OK || broken(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)), "got %08lx\n", hr);
    if (hr != S_OK)
    {
        win_skip("SLRegisterEvent with a session handle is not available\n");
        CloseHandle(event);
        CloseHandle(event2);
        SLClose(slc);
        return;
    }
    hr = SLRegisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, event);
    ok(hr == S_OK, "second registration: got %08lx\n", hr);
    hr = SLRegisterEvent(slc, SL_EVENT_POLICY_CHANGED, &other_app, event2);
    ok(hr == S_OK, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, SL_EVENT_USER_NOTIFICATION, &windows_app, event2);
    ok(hr == S_OK, "got %08lx\n", hr);

    hr = SLRegisterEvent(slc, L"msft:rm/event/nonexistent", &windows_app, event);
    ok(hr == SL_E_INVALID_EVENT_ID, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, L"MSFT:RM/EVENT/LICENSINGSTATECHANGED", &windows_app, event);
    ok(hr == SL_E_INVALID_EVENT_ID, "upper case: got %08lx\n", hr);
    hr = SLRegisterEvent(slc, L"", &windows_app, event);
    ok(hr == SL_E_INVALID_EVENT_ID, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, NULL, &windows_app, event);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, NULL, event);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, NULL);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLRegisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, (HANDLE)0xdead);
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), "got %08lx\n", hr);

    hr = SLUnregisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, event);
    ok(hr == S_OK, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, event);
    ok(hr == SL_E_EVENT_NOT_REGISTERED, "second unregistration: got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_POLICY_CHANGED, &windows_app, event2);
    ok(hr == SL_E_EVENT_NOT_REGISTERED, "other application: got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_POLICY_CHANGED, &other_app, event2);
    ok(hr == S_OK, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_USER_NOTIFICATION, &windows_app, event2);
    ok(hr == S_OK, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, L"msft:rm/event/nonexistent", &windows_app, event);
    ok(hr == SL_E_INVALID_EVENT_ID, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, NULL, &windows_app, event);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, NULL, event);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLUnregisterEvent(slc, SL_EVENT_LICENSING_STATE_CHANGED, &windows_app, NULL);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);

    CloseHandle(event);
    CloseHandle(event2);
    SLClose(slc);
}

/* Wine-only: the license is kept in the prefix and nothing checks its signature. Windows'
 * licensing service rejects an unsigned document. */
static void test_SLInstallLicense(void)
{
    static const char license[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><r:license xmlns:r=\"urn:mpeg:mpeg21:2003:01-REL-R-NS\" "
        "licenseId=\"{7CBEB5E5-2BF9-4F2E-9E0B-1F0A8F2B4B77}\"></r:license>";
    static const GUID expected = {0x7cbeb5e5, 0x2bf9, 0x4f2e, {0x9e, 0x0b, 0x1f, 0x0a, 0x8f, 0x2b, 0x4b, 0x77}};
    WCHAR path[MAX_PATH];
    HSLC slc;
    SLID id, *ids;
    UINT count, i;
    BOOL found;
    HRESULT hr;

    if (!GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version"))
    {
        skip("Wine's license store\n");
        return;
    }

    hr = SLOpen(&slc);
    ok(hr == S_OK, "got %08lx\n", hr);

    hr = SLInstallLicense(slc, sizeof(license) - 1, (const BYTE *)license, NULL);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLInstallLicense(slc, sizeof(license) - 1, NULL, &id);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLInstallLicense(slc, 0, (const BYTE *)license, &id);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLInstallLicense(NULL, sizeof(license) - 1, (const BYTE *)license, &id);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);
    hr = SLInstallLicense(slc, 4, (const BYTE *)"junk", &id);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);

    memset(&id, 0, sizeof(id));
    hr = SLInstallLicense(slc, sizeof(license) - 1, (const BYTE *)license, &id);
    ok(hr == S_OK, "got %08lx\n", hr);
    ok(IsEqualGUID(&id, &expected), "got %s\n", wine_dbgstr_guid(&id));

    memset(&id, 0, sizeof(id));
    hr = SLGetLicenseFileId(slc, sizeof(license) - 1, (const BYTE *)license, &id);
    ok(hr == S_OK, "got %08lx\n", hr);
    ok(IsEqualGUID(&id, &expected), "got %s\n", wine_dbgstr_guid(&id));

    count = 0;
    ids = NULL;
    hr = SLGetSLIDList(slc, SL_ID_ALL_LICENSE_FILES, NULL, SL_ID_LICENSE_FILE, &count, &ids);
    ok(hr == S_OK, "got %08lx\n", hr);
    for (i = 0, found = FALSE; i < count; i++) if (IsEqualGUID(&ids[i], &expected)) found = TRUE;
    ok(found, "installed license not listed (%u ids)\n", count);
    LocalFree(ids);

    hr = SLGetSLIDList(slc, SL_ID_ALL_LICENSE_FILES, NULL, SL_ID_LICENSE_FILE, NULL, &ids);
    ok(hr == E_INVALIDARG, "got %08lx\n", hr);

    if (!GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH - 80)) GetWindowsDirectoryW(path, MAX_PATH - 80);
    lstrcatW(path, L"\\SPPWine\\Licenses\\{7CBEB5E5-2BF9-4F2E-9E0B-1F0A8F2B4B77}.xrm-ms");
    ok(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, "license file not in the store: %lu\n", GetLastError());

    hr = SLUninstallLicense(slc, &expected);
    ok(hr == S_OK, "got %08lx\n", hr);
    ok(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES, "license file still in the store\n");
    hr = SLUninstallLicense(slc, &expected);
    ok(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got %08lx\n", hr);

    SLClose(slc);
}

START_TEST(slc)
{
    test_SLInstallLicense();
    test_SLGetWindowsInformationDWORD();
    test_SLRegisterEvent();
}
