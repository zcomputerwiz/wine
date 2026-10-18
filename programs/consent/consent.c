/*
 * WineExplorer User Account Control Consent Dialog (consent.exe)
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <commctrl.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>

#include "resource.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(consent);

struct consent_data
{
    HWND hwnd_parent;
    WCHAR cmdline[2048];
    WCHAR exepath[MAX_PATH];
    WCHAR progname[256];
    WCHAR publisher[256];
    WCHAR origin[128];
    BOOL is_verified;
    BOOL credentials_mode;
    BOOL details_visible;
};

/* Draws authentic 4-quadrant Windows Vista UAC shield with golden border */
static void draw_shield(HDC hdc, int x, int y, int w, int h)
{
    POINT pts[7];
    POINT q_tl[6], q_tr[6], q_bl[5], q_br[5];
    HBRUSH br_gold, br_blue_light, br_blue_dark;
    HPEN pen_rim, pen_gold, pen_cross;
    HGDIOBJ old_brush, old_pen;
    int mid_x = x + w / 2;
    int mid_y = y + h / 2;

    br_gold = CreateSolidBrush(RGB(246, 185, 25));
    br_blue_light = CreateSolidBrush(RGB(35, 140, 235));
    br_blue_dark = CreateSolidBrush(RGB(20, 95, 195));
    pen_rim = CreatePen(PS_SOLID, 2, RGB(255, 235, 130));
    pen_gold = CreatePen(PS_SOLID, 1, RGB(220, 160, 20));
    pen_cross = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));

    pts[0].x = x + 2;     pts[0].y = y;
    pts[1].x = x + w - 2; pts[1].y = y;
    pts[2].x = x + w;     pts[2].y = y + 4;
    pts[3].x = x + w;     pts[3].y = y + h * 5 / 9;
    pts[4].x = mid_x;     pts[4].y = y + h;
    pts[5].x = x;         pts[5].y = y + h * 5 / 9;
    pts[6].x = x;         pts[6].y = y + 4;

    q_tl[0].x = x + 2; q_tl[0].y = y;
    q_tl[1].x = mid_x; q_tl[1].y = y;
    q_tl[2].x = mid_x; q_tl[2].y = mid_y;
    q_tl[3].x = x;     q_tl[3].y = mid_y;
    q_tl[4].x = x;     q_tl[4].y = y + 4;
    q_tl[5].x = x + 2; q_tl[5].y = y;
    old_brush = SelectObject(hdc, br_gold);
    old_pen = SelectObject(hdc, pen_gold);
    Polygon(hdc, q_tl, 6);

    q_tr[0].x = mid_x;     q_tr[0].y = y;
    q_tr[1].x = x + w - 2; q_tr[1].y = y;
    q_tr[2].x = x + w;     q_tr[2].y = y + 4;
    q_tr[3].x = x + w;     q_tr[3].y = mid_y;
    q_tr[4].x = mid_x;     q_tr[4].y = mid_y;
    q_tr[5].x = mid_x;     q_tr[5].y = y;
    SelectObject(hdc, br_blue_light);
    Polygon(hdc, q_tr, 6);

    q_bl[0].x = x;     q_bl[0].y = mid_y;
    q_bl[1].x = mid_x; q_bl[1].y = mid_y;
    q_bl[2].x = mid_x; q_bl[2].y = y + h;
    q_bl[3].x = x;     q_bl[3].y = y + h * 5 / 9;
    q_bl[4].x = x;     q_bl[4].y = mid_y;
    SelectObject(hdc, br_blue_dark);
    Polygon(hdc, q_bl, 5);

    q_br[0].x = mid_x; q_br[0].y = mid_y;
    q_br[1].x = x + w; q_br[1].y = mid_y;
    q_br[2].x = x + w; q_br[2].y = y + h * 5 / 9;
    q_br[3].x = mid_x; q_br[3].y = y + h;
    q_br[4].x = mid_x; q_br[4].y = mid_y;
    SelectObject(hdc, br_gold);
    Polygon(hdc, q_br, 5);

    SelectObject(hdc, pen_cross);
    MoveToEx(hdc, mid_x, y + 1, NULL);
    LineTo(hdc, mid_x, y + h - 1);
    MoveToEx(hdc, x + 1, mid_y, NULL);
    LineTo(hdc, x + w - 1, mid_y);

    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    SelectObject(hdc, pen_rim);
    Polygon(hdc, pts, 7);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen_rim);
    DeleteObject(pen_gold);
    DeleteObject(pen_cross);
    DeleteObject(br_gold);
    DeleteObject(br_blue_light);
    DeleteObject(br_blue_dark);
}

static void draw_banner(const DRAWITEMSTRUCT *dis, BOOL is_verified)
{
    HDC hdc = dis->hDC;
    COLORREF bg_color = is_verified ? RGB(12, 80, 140) : RGB(195, 130, 10);
    COLORREF line_color = is_verified ? RGB(8, 55, 95) : RGB(160, 100, 5);
    HBRUSH bg = CreateSolidBrush(bg_color);
    HPEN pen_line = CreatePen(PS_SOLID, 1, line_color);
    HFONT font, old_font;
    LOGFONTW lf;
    RECT text = dis->rcItem;
    HGDIOBJ old_pen;
    const WCHAR *msg;

    FillRect(hdc, &dis->rcItem, bg);
    DeleteObject(bg);

    old_pen = SelectObject(hdc, pen_line);
    MoveToEx(hdc, dis->rcItem.left, dis->rcItem.bottom - 1, NULL);
    LineTo(hdc, dis->rcItem.right, dis->rcItem.bottom - 1);
    SelectObject(hdc, old_pen);
    DeleteObject(pen_line);

    draw_shield(hdc, 14, 5, 24, 26);

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -13;
    lf.lfWeight = FW_SEMIBOLD;
    lstrcpyW(lf.lfFaceName, L"Tahoma");
    font = CreateFontIndirectW(&lf);
    old_font = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    text.left = 46;
    text.right -= 10;

    msg = is_verified ?
          L"A program needs your permission to continue" :
          L"An unidentified program wants access to your computer";

    DrawTextW(hdc, msg, -1, &text, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    SelectObject(hdc, old_font);
    DeleteObject(font);
}

static void draw_user_tile(const DRAWITEMSTRUCT *dis)
{
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    HPEN pen_avatar = CreatePen(PS_SOLID, 1, RGB(165, 178, 192));
    HBRUSH bg_avatar = CreateSolidBrush(RGB(226, 233, 242));
    HBRUSH br_skin = CreateSolidBrush(RGB(225, 185, 145));
    HBRUSH br_hair = CreateSolidBrush(RGB(90, 65, 45));
    HBRUSH br_shirt = CreateSolidBrush(RGB(95, 150, 218));
    HGDIOBJ old_brush, old_pen;
    POINT shoulder_pts[5];

    old_brush = SelectObject(hdc, bg_avatar);
    old_pen = SelectObject(hdc, pen_avatar);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 4, 4);

    shoulder_pts[0].x = rc.left + 3;  shoulder_pts[0].y = rc.bottom - 1;
    shoulder_pts[1].x = rc.left + 8;  shoulder_pts[1].y = rc.top + (rc.bottom - rc.top) * 5 / 9;
    shoulder_pts[2].x = rc.right - 8; shoulder_pts[2].y = rc.top + (rc.bottom - rc.top) * 5 / 9;
    shoulder_pts[3].x = rc.right - 3; shoulder_pts[3].y = rc.bottom - 1;
    shoulder_pts[4].x = rc.left + 3;  shoulder_pts[4].y = rc.bottom - 1;
    SelectObject(hdc, br_shirt);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, shoulder_pts, 5);

    SelectObject(hdc, br_skin);
    Ellipse(hdc, rc.left + 9, rc.top + 5, rc.right - 9, rc.top + (rc.bottom - rc.top) * 6 / 10);

    SelectObject(hdc, br_hair);
    Chord(hdc, rc.left + 9, rc.top + 4, rc.right - 9, rc.top + (rc.bottom - rc.top) * 5 / 10,
          rc.right - 9, rc.top + 10, rc.left + 9, rc.top + 10);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen_avatar);
    DeleteObject(bg_avatar);
    DeleteObject(br_skin);
    DeleteObject(br_hair);
    DeleteObject(br_shirt);
}

static void parse_command_line(const WCHAR *cmd, struct consent_data *data)
{
    const WCHAR *p = cmd;
    WCHAR *dst;

    memset(data, 0, sizeof(*data));
    lstrcpyW(data->origin, L"Hard drive on this computer");

    while (*p)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (!wcsnicmp(p, L"/hwnd", 5) || !wcsnicmp(p, L"-hwnd", 5))
        {
            p += 5;
            while (*p == ' ' || *p == '\t') p++;
            data->hwnd_parent = (HWND)(ULONG_PTR)wcstoul(p, (WCHAR **)&p, 16);
        }
        else if (!wcsnicmp(p, L"/cmd", 4) || !wcsnicmp(p, L"-cmd", 4))
        {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            lstrcpynW(data->cmdline, p, ARRAY_SIZE(data->cmdline));
            break;
        }
        else
        {
            p++;
        }
    }

    if (!data->cmdline[0])
        lstrcpyW(data->cmdline, L"setup.exe");

    /* Extract executable path from cmdline */
    p = data->cmdline;
    while (*p == ' ' || *p == '\t') p++;
    dst = data->exepath;

    if (*p == '"')
    {
        p++;
        while (*p && *p != '"' && (dst - data->exepath) < (MAX_PATH - 1))
            *dst++ = *p++;
    }
    else
    {
        while (*p && *p != ' ' && *p != '\t' && (dst - data->exepath) < (MAX_PATH - 1))
            *dst++ = *p++;
    }
    *dst = 0;

    /* Extract friendly program name */
    {
        DWORD ver_size, handle = 0;
        BOOL got_name = FALSE;

        ver_size = GetFileVersionInfoSizeW(data->exepath, &handle);
        if (ver_size)
        {
            void *ver_data = malloc(ver_size);
            if (ver_data && GetFileVersionInfoW(data->exepath, 0, ver_size, ver_data))
            {
                WCHAR *desc = NULL;
                UINT desc_len = 0;

                if (VerQueryValueW(ver_data, L"\\StringFileInfo\\040904b0\\FileDescription", (void **)&desc, &desc_len) && desc_len > 1)
                {
                    lstrcpynW(data->progname, desc, ARRAY_SIZE(data->progname));
                    got_name = TRUE;
                }
                else if (VerQueryValueW(ver_data, L"\\StringFileInfo\\040904e4\\FileDescription", (void **)&desc, &desc_len) && desc_len > 1)
                {
                    lstrcpynW(data->progname, desc, ARRAY_SIZE(data->progname));
                    got_name = TRUE;
                }
            }
            free(ver_data);
        }

        if (!got_name)
        {
            const WCHAR *base = wcsrchr(data->exepath, '\\');
            lstrcpynW(data->progname, base ? base + 1 : data->exepath, ARRAY_SIZE(data->progname));
        }
    }

    /* Verify Authenticode signature */
    {
        WINTRUST_FILE_INFO file_info = { sizeof(file_info), data->exepath };
        WINTRUST_DATA trust_data = { sizeof(trust_data) };
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG status;

        trust_data.dwUIChoice = WTD_UI_NONE;
        trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust_data.dwUnionChoice = WTD_CHOICE_FILE;
        trust_data.pFile = &file_info;
        trust_data.dwStateAction = WTD_STATEACTION_VERIFY;

        status = WinVerifyTrust(NULL, &action, &trust_data);
        trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(NULL, &action, &trust_data);

        if (status == ERROR_SUCCESS)
        {
            data->is_verified = TRUE;
            lstrcpyW(data->publisher, L"Verified publisher");
        }
        else
        {
            data->is_verified = FALSE;
            lstrcpyW(data->publisher, L"Unknown publisher");
        }
    }
}

static BOOL is_credentials_mode_required(void)
{
    TOKEN_ELEVATION_TYPE type;
    DWORD size, mode = 2; /* 2 = consent, 1 = credentials */
    HKEY hkey;

    if (GetTokenInformation(GetCurrentThreadEffectiveToken(), TokenElevationType, &type, sizeof(type), &size))
    {
        if (type == TokenElevationTypeDefault)
            return TRUE; /* Standard user always requires administrator credentials */
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS)
    {
        DWORD val_size = sizeof(mode);
        RegQueryValueExW(hkey, L"ConsentPromptBehaviorAdmin", NULL, NULL, (BYTE *)&mode, &val_size);
        RegCloseKey(hkey);
    }
    return (mode == 1);
}

static INT_PTR CALLBACK consent_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct consent_data *data = (struct consent_data *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HFONT font_bold;
        LOGFONTW lf;

        data = (struct consent_data *)lparam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)data);

        SetDlgItemTextW(hwnd, IDC_UAC_PROGRAM, data->progname);
        SetDlgItemTextW(hwnd, IDC_UAC_PUBLISHER, data->publisher);
        SetDlgItemTextW(hwnd, IDC_UAC_ORIGIN, data->origin);
        SetDlgItemTextW(hwnd, IDC_UAC_DETAILS_TEXT, data->cmdline);

        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -12;
        lf.lfWeight = FW_BOLD;
        lstrcpyW(lf.lfFaceName, L"Tahoma");
        if ((font_bold = CreateFontIndirectW(&lf)))
            SendDlgItemMessageW(hwnd, IDC_UAC_PROGRAM, WM_SETFONT, (WPARAM)font_bold, TRUE);

        SetFocus(GetDlgItem(hwnd, IDYES));
        return FALSE;
    }

    case WM_DRAWITEM:
        if (wparam == IDC_UAC_BANNER)
        {
            draw_banner((const DRAWITEMSTRUCT *)lparam, data->is_verified);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_UAC_DETAILS_BTN:
            data->details_visible = !data->details_visible;
            ShowWindow(GetDlgItem(hwnd, IDC_UAC_DETAILS_TEXT), data->details_visible ? SW_SHOW : SW_HIDE);
            SetDlgItemTextW(hwnd, IDC_UAC_DETAILS_BTN, data->details_visible ? L"&Hide" : L"&Details");
            return TRUE;

        case IDYES:
            EndDialog(hwnd, IDYES);
            return TRUE;

        case IDNO:
        case IDCANCEL:
            EndDialog(hwnd, IDNO);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwnd, IDNO);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK credentials_dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct consent_data *data = (struct consent_data *)GetWindowLongPtrW(hwnd, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HFONT font_bold;
        LOGFONTW lf;
        WCHAR user[128];
        DWORD user_len = ARRAY_SIZE(user);

        data = (struct consent_data *)lparam;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)data);

        SetDlgItemTextW(hwnd, IDC_UAC_PROGRAM, data->progname);
        SetDlgItemTextW(hwnd, IDC_UAC_PUBLISHER, data->publisher);

        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -12;
        lf.lfWeight = FW_BOLD;
        lstrcpyW(lf.lfFaceName, L"Tahoma");
        if ((font_bold = CreateFontIndirectW(&lf)))
            SendDlgItemMessageW(hwnd, IDC_UAC_PROGRAM, WM_SETFONT, (WPARAM)font_bold, TRUE);

        if (GetUserNameW(user, &user_len))
            SendDlgItemMessageW(hwnd, IDC_UAC_ACCOUNT, CB_ADDSTRING, 0, (LPARAM)user);
        else
            SendDlgItemMessageW(hwnd, IDC_UAC_ACCOUNT, CB_ADDSTRING, 0, (LPARAM)L"Administrator");
        SendDlgItemMessageW(hwnd, IDC_UAC_ACCOUNT, CB_SETCURSEL, 0, 0);

        SetFocus(GetDlgItem(hwnd, IDC_UAC_PASSWORD));
        return FALSE;
    }

    case WM_DRAWITEM:
        if (wparam == IDC_UAC_BANNER)
        {
            draw_banner((const DRAWITEMSTRUCT *)lparam, data->is_verified);
            return TRUE;
        }
        if (wparam == IDC_UAC_USERTILE)
        {
            draw_user_tile((const DRAWITEMSTRUCT *)lparam);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDYES:
        {
            WCHAR pass[128];
            GetDlgItemTextW(hwnd, IDC_UAC_PASSWORD, pass, ARRAY_SIZE(pass));
            if (!pass[0])
            {
                SetDlgItemTextW(hwnd, IDC_UAC_ERROR, L"Please enter a password.");
                SetFocus(GetDlgItem(hwnd, IDC_UAC_PASSWORD));
                return TRUE;
            }
            /* Password entered */
            EndDialog(hwnd, IDYES);
            return TRUE;
        }

        case IDNO:
        case IDCANCEL:
            EndDialog(hwnd, IDNO);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwnd, IDNO);
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    struct consent_data data;
    HWND hdim = NULL;
    INT_PTR res;

    /* Diagnostic / Automated Test Overrides */
    if (GetEnvironmentVariableW(L"WINE_CONSENT_AUTO_APPROVE", NULL, 0))
    {
        TRACE("WINE_CONSENT_AUTO_APPROVE set: auto-granting elevation\n");
        return 0;
    }
    if (GetEnvironmentVariableW(L"WINE_CONSENT_AUTO_DENY", NULL, 0))
    {
        TRACE("WINE_CONSENT_AUTO_DENY set: auto-cancelling elevation\n");
        return 1223;
    }

    InitCommonControls();
    parse_command_line(cmdline, &data);
    data.credentials_mode = is_credentials_mode_required();

    /* Secure Desktop Dimmer */
    hdim = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                           L"STATIC", L"", WS_POPUP | WS_VISIBLE,
                           GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                           NULL, NULL, inst, NULL);
    if (hdim)
        SetLayeredWindowAttributes(hdim, 0, 160, LWA_ALPHA);

    if (data.credentials_mode)
        res = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CREDENTIALS), hdim ? hdim : data.hwnd_parent,
                              credentials_dlg_proc, (LPARAM)&data);
    else
        res = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_CONSENT), hdim ? hdim : data.hwnd_parent,
                              consent_dlg_proc, (LPARAM)&data);

    if (hdim)
        DestroyWindow(hdim);

    TRACE("Consent dialog finished with result %Id\n", res);
    return (res == IDYES) ? 0 : 1223;
}
