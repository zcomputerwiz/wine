/*
 * Task Manager: the Networking page
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * As on XP: one history graph per network adapter (in its own frame, named after the connection), the share of the
 * link's speed used in each interval, and below them a list of the adapters with the columns chosen in
 * View > Select Columns. The figures come from GetIfTable2; loopback interfaces are left out.
 */

#include <stdio.h>
#include <stdlib.h>

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "taskmgr.h"

HWND hNetworkPage;

static HWND hAdapterList, hGraphArea, hNoAdapters;
static int nPageWidth, nPageHeight;

#define HISTORY_SIZE   1024
#define GRAPH_STEP     2        /* pixels between two samples */
#define GRID_X         12       /* XP's grid: a line every 12 pixels across, ... */
#define GRID_Y         31       /* ... and every 31 pixels up from the bottom */
#define SCALE_WIDTH    37       /* the scale's labels, left of the plot */
#define GRAPH_CLASS    L"TaskManagerNetGraph"

#define GRID_COLOR     RGB(0, 128, 64)
#define SCALE_COLOR    RGB(255, 255, 0)
#define SENT_COLOR     RGB(255, 0, 0)
#define RECEIVED_COLOR RGB(255, 255, 0)
#define TOTAL_COLOR    RGB(0, 255, 0)

struct adapter
{
    NET_LUID luid;
    WCHAR    name[IF_MAX_STRING_SIZE + 1];
    WCHAR    description[IF_MAX_STRING_SIZE + 1];
    ULONG64  speed;             /* bits per second */
    IF_OPER_STATUS status;
    BOOL     present;           /* seen in the last refresh */

    /* counters now, at the last refresh, and when the history was reset */
    ULONG64  out_octets, in_octets, out_ucast, in_ucast, out_nucast, in_nucast;
    ULONG64  last_out_octets, last_in_octets, last_out_ucast, last_in_ucast, last_out_nucast, last_in_nucast;
    ULONG64  base_out_octets, base_in_octets, base_out_ucast, base_in_ucast, base_out_nucast, base_in_nucast;
    double   util_sent, util_received;      /* percent of the link in the last interval */
    BOOL     have_last;

    /* history in percent of the link: sent, received */
    float    hist_sent[HISTORY_SIZE], hist_received[HISTORY_SIZE];
    unsigned hist_count, hist_pos;

    HWND     frame, graph;
};

static struct adapter *adapters;
static unsigned adapter_count;
static ULONGLONG last_tick;
static unsigned grid_scroll;    /* the grid moves left with the samples */

/* the columns, in the order of the Select Columns dialog */
enum
{
    NETCOL_NAME, NETCOL_DESCRIPTION, NETCOL_UTILIZATION, NETCOL_SPEED, NETCOL_STATE,
    NETCOL_SENT_THROUGHPUT, NETCOL_RECEIVED_THROUGHPUT, NETCOL_THROUGHPUT,
    NETCOL_BYTES_SENT, NETCOL_BYTES_RECEIVED, NETCOL_BYTES,
    NETCOL_BYTES_SENT_INTERVAL, NETCOL_BYTES_RECEIVED_INTERVAL, NETCOL_BYTES_INTERVAL,
    NETCOL_UCAST_SENT, NETCOL_UCAST_RECEIVED, NETCOL_UCAST,
    NETCOL_UCAST_SENT_INTERVAL, NETCOL_UCAST_RECEIVED_INTERVAL, NETCOL_UCAST_INTERVAL,
    NETCOL_NUCAST_SENT, NETCOL_NUCAST_RECEIVED, NETCOL_NUCAST,
    NETCOL_NUCAST_SENT_INTERVAL, NETCOL_NUCAST_RECEIVED_INTERVAL, NETCOL_NUCAST_INTERVAL,
};

static int column_map[NET_COLUMN_COUNT];    /* list column -> NETCOL_* */
static int column_count;

void NetworkPage_DefaultSettings(void)
{
    static const int widths[NET_COLUMN_COUNT] =
        { 96, 110, 101, 56, 92, 90, 90, 90, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80 };
    int i;

    TaskManagerSettings.NetTabAlwaysActive = FALSE;
    TaskManagerSettings.NetShowCumulative = FALSE;
    TaskManagerSettings.NetAutoScale = TRUE;
    TaskManagerSettings.NetShowScale = TRUE;
    TaskManagerSettings.NetHistory = NET_HISTORY_TOTAL;
    for (i = 0; i < NET_COLUMN_COUNT; i++)
    {
        TaskManagerSettings.NetColumns[i] = i == NETCOL_NAME || i == NETCOL_UTILIZATION || i == NETCOL_SPEED ||
                                            i == NETCOL_STATE;
        TaskManagerSettings.NetColumnWidths[i] = widths[i];
    }
}

/* ---- data ---- */

static struct adapter *find_adapter(const NET_LUID *luid)
{
    unsigned i;

    for (i = 0; i < adapter_count; i++)
        if (adapters[i].luid.Value == luid->Value) return &adapters[i];
    return NULL;
}

static void reset_history(struct adapter *a)
{
    a->hist_count = a->hist_pos = 0;
    a->base_out_octets = a->out_octets;
    a->base_in_octets = a->in_octets;
    a->base_out_ucast = a->out_ucast;
    a->base_in_ucast = a->in_ucast;
    a->base_out_nucast = a->out_nucast;
    a->base_in_nucast = a->in_nucast;
}

static void create_adapter_windows(struct adapter *a);
static void layout_graphs(void);

/* Read the adapters' counters; returns TRUE when the set of adapters changed. */
static BOOL update_adapters(void)
{
    MIB_IF_TABLE2 *table;
    ULONGLONG tick = GetTickCount64();
    double seconds = last_tick ? (tick - last_tick) / 1000.0 : 0;
    BOOL changed = FALSE;
    unsigned i, j;

    if (GetIfTable2(&table) != NO_ERROR) return FALSE;
    last_tick = tick;

    for (i = 0; i < adapter_count; i++) adapters[i].present = FALSE;

    for (i = 0; i < table->NumEntries; i++)
    {
        MIB_IF_ROW2 *row = &table->Table[i];
        struct adapter *a;

        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK || row->Type == IF_TYPE_TUNNEL) continue;
        if (!(a = find_adapter(&row->InterfaceLuid)))
        {
            struct adapter *new = realloc(adapters, (adapter_count + 1) * sizeof(*adapters));
            if (!new) continue;
            adapters = new;
            a = &adapters[adapter_count++];
            memset(a, 0, sizeof(*a));
            a->luid = row->InterfaceLuid;
            changed = TRUE;
        }
        a->present = TRUE;
        lstrcpynW(a->name, row->Alias[0] ? row->Alias : row->Description, ARRAY_SIZE(a->name));
        lstrcpynW(a->description, row->Description, ARRAY_SIZE(a->description));
        a->speed = max(row->TransmitLinkSpeed, row->ReceiveLinkSpeed);
        if (a->speed == ~(ULONG64)0) a->speed = 0;
        a->status = row->OperStatus;
        a->out_octets = row->OutOctets;
        a->in_octets = row->InOctets;
        a->out_ucast = row->OutUcastPkts;
        a->in_ucast = row->InUcastPkts;
        a->out_nucast = row->OutNUcastPkts;
        a->in_nucast = row->InNUcastPkts;

        if (!a->have_last)
        {
            reset_history(a);
            a->util_sent = a->util_received = 0;
        }
        else if (seconds > 0 && a->speed)
        {
            a->util_sent = (a->out_octets - a->last_out_octets) * 8.0 * 100.0 / (seconds * a->speed);
            a->util_received = (a->in_octets - a->last_in_octets) * 8.0 * 100.0 / (seconds * a->speed);
            a->util_sent = min(a->util_sent, 100.0);
            a->util_received = min(a->util_received, 100.0);
        }
        else a->util_sent = a->util_received = 0;
    }
    FreeMibTable(table);

    /* adapters that went away */
    for (i = j = 0; i < adapter_count; i++)
    {
        if (!adapters[i].present)
        {
            if (adapters[i].frame) DestroyWindow(adapters[i].frame);
            if (adapters[i].graph) DestroyWindow(adapters[i].graph);
            changed = TRUE;
            continue;
        }
        if (i != j)
        {
            adapters[j] = adapters[i];
            /* the graph window points at its adapter */
            if (adapters[j].graph) SetWindowLongPtrW(adapters[j].graph, GWLP_USERDATA, j);
        }
        j++;
    }
    adapter_count = j;
    return changed;
}

static void append_samples(void)
{
    unsigned i;

    for (i = 0; i < adapter_count; i++)
    {
        struct adapter *a = &adapters[i];

        if (a->have_last)
        {
            a->hist_sent[a->hist_pos] = a->util_sent;
            a->hist_received[a->hist_pos] = a->util_received;
            a->hist_pos = (a->hist_pos + 1) % HISTORY_SIZE;
            if (a->hist_count < HISTORY_SIZE) a->hist_count++;
        }
        a->last_out_octets = a->out_octets;
        a->last_in_octets = a->in_octets;
        a->last_out_ucast = a->out_ucast;
        a->last_in_ucast = a->in_ucast;
        a->last_out_nucast = a->out_nucast;
        a->last_in_nucast = a->in_nucast;
        a->have_last = TRUE;
    }
    grid_scroll = (grid_scroll + GRAPH_STEP) % GRID_X;
}

/* ---- the graphs ---- */

/* XP's automatic scale: the smallest of these that holds every sample in view */
static double graph_scale(const struct adapter *a, unsigned samples)
{
    static const double scales[] = { 1, 5, 10, 25, 50, 100 };
    double top = 0;
    unsigned i, n = min(samples, a->hist_count);

    if (!TaskManagerSettings.NetAutoScale) return 100;
    for (i = 0; i < n; i++)
    {
        unsigned k = (a->hist_pos + HISTORY_SIZE - 1 - i) % HISTORY_SIZE;
        double v = 0;
        if (TaskManagerSettings.NetHistory & NET_HISTORY_SENT) v = max(v, a->hist_sent[k]);
        if (TaskManagerSettings.NetHistory & NET_HISTORY_RECEIVED) v = max(v, a->hist_received[k]);
        if (TaskManagerSettings.NetHistory & NET_HISTORY_TOTAL) v = max(v, a->hist_sent[k] + a->hist_received[k]);
        top = max(top, v);
    }
    for (i = 0; i < ARRAY_SIZE(scales); i++)
        if (top <= scales[i]) return scales[i];
    return 100;
}

static void format_percent(WCHAR *buf, size_t size, double value)
{
    if (value == (int)value) swprintf(buf, size, L"%d %%", (int)value);
    else swprintf(buf, size, L"%.1f %%", value);
}

static void draw_line(HDC hdc, const struct adapter *a, RECT *plot, double scale, int which, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 1, color), old = SelectObject(hdc, pen);
    int height = plot->bottom - plot->top - 1, x = plot->right - 1;
    unsigned i;

    for (i = 0; i < a->hist_count && x >= plot->left; i++, x -= GRAPH_STEP)
    {
        unsigned k = (a->hist_pos + HISTORY_SIZE - 1 - i) % HISTORY_SIZE;
        double v = which == NET_HISTORY_SENT ? a->hist_sent[k] :
                   which == NET_HISTORY_RECEIVED ? a->hist_received[k] : a->hist_sent[k] + a->hist_received[k];
        int y = plot->bottom - 1 - (int)(min(v, scale) * height / scale + 0.5);

        if (!i) MoveToEx(hdc, x, y, NULL);
        else LineTo(hdc, x, y);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void paint_graph(HWND hwnd, HDC hdc)
{
    const struct adapter *a = &adapters[GetWindowLongPtrW(hwnd, GWLP_USERDATA)];
    RECT rc, plot;
    HDC mem;
    HBITMAP bitmap, old_bitmap;
    HPEN grid, old_pen;
    HFONT old_font;
    double scale;
    int x, y;

    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(hdc);
    bitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    old_bitmap = SelectObject(mem, bitmap);
    FillSolidRect(mem, &rc, RGB(0, 0, 0));

    plot = rc;
    if (TaskManagerSettings.NetShowScale) plot.left = SCALE_WIDTH;
    scale = graph_scale(a, (plot.right - plot.left) / GRAPH_STEP + 1);

    grid = CreatePen(PS_SOLID, 1, GRID_COLOR);
    old_pen = SelectObject(mem, grid);
    for (y = plot.bottom - 1 - GRID_Y; y > plot.top; y -= GRID_Y)
    {
        MoveToEx(mem, plot.left, y, NULL);
        LineTo(mem, plot.right, y);
    }
    for (x = plot.right - 1 - (GRID_X - 1 - grid_scroll) % GRID_X; x > plot.left; x -= GRID_X)
    {
        MoveToEx(mem, x, plot.top, NULL);
        LineTo(mem, x, plot.bottom);
    }
    SelectObject(mem, old_pen);
    DeleteObject(grid);

    if (TaskManagerSettings.NetShowScale)
    {
        WCHAR text[32];
        RECT label;
        HPEN axis = CreatePen(PS_SOLID, 1, SCALE_COLOR);
        int line_height;
        TEXTMETRICW tm;

        old_font = SelectObject(mem, (HFONT)SendMessageW(hNetworkPage, WM_GETFONT, 0, 0));
        GetTextMetricsW(mem, &tm);
        line_height = tm.tmHeight;
        old_pen = SelectObject(mem, axis);
        MoveToEx(mem, plot.left, plot.top, NULL);
        LineTo(mem, plot.left, plot.bottom);
        SelectObject(mem, old_pen);
        DeleteObject(axis);

        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, SCALE_COLOR);
        SetRect(&label, 0, plot.top + 1, plot.left - 4, plot.top + 1 + line_height);
        format_percent(text, ARRAY_SIZE(text), scale);
        DrawTextW(mem, text, -1, &label, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        SetRect(&label, 0, (plot.top + plot.bottom - line_height) / 2, plot.left - 4,
                (plot.top + plot.bottom + line_height) / 2);
        format_percent(text, ARRAY_SIZE(text), scale / 2);
        DrawTextW(mem, text, -1, &label, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        SetRect(&label, 0, plot.bottom - 1 - line_height, plot.left - 4, plot.bottom - 1);
        format_percent(text, ARRAY_SIZE(text), 0);
        DrawTextW(mem, text, -1, &label, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, old_font);
        plot.left++;
    }

    if (TaskManagerSettings.NetHistory & NET_HISTORY_TOTAL)
        draw_line(mem, a, &plot, scale, NET_HISTORY_TOTAL, TOTAL_COLOR);
    if (TaskManagerSettings.NetHistory & NET_HISTORY_RECEIVED)
        draw_line(mem, a, &plot, scale, NET_HISTORY_RECEIVED, RECEIVED_COLOR);
    if (TaskManagerSettings.NetHistory & NET_HISTORY_SENT)
        draw_line(mem, a, &plot, scale, NET_HISTORY_SENT, SENT_COLOR);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

static LRESULT CALLBACK graph_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) < adapter_count) paint_graph(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void create_adapter_windows(struct adapter *a)
{
    a->frame = CreateWindowExW(WS_EX_TRANSPARENT, L"BUTTON", a->name, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                               0, 0, 0, 0, hNetworkPage, NULL, hInst, NULL);
    SendMessageW(a->frame, WM_SETFONT, SendMessageW(hNetworkPage, WM_GETFONT, 0, 0), FALSE);
    a->graph = CreateWindowExW(WS_EX_CLIENTEDGE, GRAPH_CLASS, NULL, WS_CHILD | WS_VISIBLE,
                               0, 0, 0, 0, hNetworkPage, NULL, hInst, NULL);
    SetWindowLongPtrW(a->graph, GWLP_USERDATA, a - adapters);
}

/* Share the area above the list between the adapters' frames, one above the other. */
static void layout_graphs(void)
{
    RECT area, unit = { 4, 14, 0, 0 };   /* the graph's place in its frame (dialog units): left and top */
    RECT margin = { 4, 4, 0, 0 };        /* and the frame's margin right and below it */
    int i, height, top;

    GetWindowRect(hGraphArea, &area);
    MapWindowPoints(NULL, hNetworkPage, (POINT *)&area, 2);
    MapDialogRect(hNetworkPage, &unit);
    MapDialogRect(hNetworkPage, &margin);

    ShowWindow(hNoAdapters, adapter_count ? SW_HIDE : SW_SHOW);
    if (!adapter_count) return;

    height = (area.bottom - area.top) / adapter_count;
    for (i = 0; i < adapter_count; i++)
    {
        struct adapter *a = &adapters[i];

        if (!a->frame) create_adapter_windows(a);
        SetWindowTextW(a->frame, a->name);
        top = area.top + i * height;
        SetWindowPos(a->frame, NULL, area.left, top, area.right - area.left, height - margin.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(a->graph, NULL, area.left + unit.left, top + unit.top,
                     area.right - area.left - 2 * unit.left, height - margin.top - unit.top - margin.left,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---- the list ---- */

static void add_columns(void)
{
    WCHAR text[64];
    LVCOLUMNW column;
    int i;

    while (SendMessageW(hAdapterList, LVM_DELETECOLUMN, 0, 0));
    column_count = 0;
    for (i = 0; i < NET_COLUMN_COUNT; i++)
    {
        if (i != NETCOL_NAME && !TaskManagerSettings.NetColumns[i]) continue;
        LoadStringW(hInst, IDS_NET_COL_FIRST + i, text, ARRAY_SIZE(text));
        column.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        column.fmt = (i == NETCOL_NAME || i == NETCOL_DESCRIPTION) ? LVCFMT_LEFT : LVCFMT_RIGHT;
        column.cx = max(TaskManagerSettings.NetColumnWidths[i], TaskManager_HeaderWidth(hAdapterList, text));
        column.pszText = text;
        SendMessageW(hAdapterList, LVM_INSERTCOLUMNW, column_count, (LPARAM)&column);
        column_map[column_count++] = i;
    }
}

static void save_column_widths(void)
{
    int i;

    for (i = 0; i < column_count; i++)
        TaskManagerSettings.NetColumnWidths[column_map[i]] =
            SendMessageW(hAdapterList, LVM_GETCOLUMNWIDTH, i, 0);
}

static void format_number(WCHAR *buf, size_t size, ULONG64 value)
{
    WCHAR digits[32];
    NUMBERFMTW fmt = { 0, 0, 3, (WCHAR *)L".", (WCHAR *)L",", 0 };

    swprintf(digits, ARRAY_SIZE(digits), L"%I64u", value);
    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, digits, &fmt, buf, size))
        lstrcpynW(buf, digits, size);
}

static void format_speed(WCHAR *buf, size_t size, ULONG64 bps)
{
    if (bps >= 1000000000 && !(bps % 1000000000)) swprintf(buf, size, L"%I64u Gbps", bps / 1000000000);
    else if (bps >= 1000000) swprintf(buf, size, L"%I64u Mbps", bps / 1000000);
    else if (bps >= 1000) swprintf(buf, size, L"%I64u Kbps", bps / 1000);
    else swprintf(buf, size, L"%I64u bps", bps);
}

static void format_cell(const struct adapter *a, int column, WCHAR *buf, size_t size)
{
    BOOL cumulative = TaskManagerSettings.NetShowCumulative;
    UINT id;

    buf[0] = 0;
    switch (column)
    {
    case NETCOL_NAME:        lstrcpynW(buf, a->name, size); break;
    case NETCOL_DESCRIPTION: lstrcpynW(buf, a->description, size); break;
    case NETCOL_UTILIZATION:
        swprintf(buf, size, L"%.2f %%", a->util_sent + a->util_received);
        if (a->util_sent + a->util_received == 0) lstrcpynW(buf, L"0 %", size);
        break;
    case NETCOL_SPEED:       format_speed(buf, size, a->speed); break;
    case NETCOL_STATE:
        switch (a->status)
        {
        case IfOperStatusUp:             id = IDS_NET_OPERATIONAL; break;
        case IfOperStatusDown:
        case IfOperStatusNotPresent:     id = IDS_NET_DISCONNECTED; break;
        case IfOperStatusDormant:        id = IDS_NET_CONNECTING; break;
        case IfOperStatusLowerLayerDown: id = IDS_NET_UNREACHABLE; break;
        default:                         id = IDS_NET_NONOPERATIONAL; break;
        }
        LoadStringW(hInst, id, buf, size);
        break;
    case NETCOL_SENT_THROUGHPUT:     swprintf(buf, size, L"%.2f %%", a->util_sent); break;
    case NETCOL_RECEIVED_THROUGHPUT: swprintf(buf, size, L"%.2f %%", a->util_received); break;
    case NETCOL_THROUGHPUT:          swprintf(buf, size, L"%.2f %%", a->util_sent + a->util_received); break;
#define TOTAL(field) (cumulative ? a->field : a->field - a->base_##field)
#define DELTA(field) (a->field - a->last_##field)
    case NETCOL_BYTES_SENT:     format_number(buf, size, TOTAL(out_octets)); break;
    case NETCOL_BYTES_RECEIVED: format_number(buf, size, TOTAL(in_octets)); break;
    case NETCOL_BYTES:          format_number(buf, size, TOTAL(out_octets) + TOTAL(in_octets)); break;
    case NETCOL_BYTES_SENT_INTERVAL:     format_number(buf, size, DELTA(out_octets)); break;
    case NETCOL_BYTES_RECEIVED_INTERVAL: format_number(buf, size, DELTA(in_octets)); break;
    case NETCOL_BYTES_INTERVAL:          format_number(buf, size, DELTA(out_octets) + DELTA(in_octets)); break;
    case NETCOL_UCAST_SENT:     format_number(buf, size, TOTAL(out_ucast)); break;
    case NETCOL_UCAST_RECEIVED: format_number(buf, size, TOTAL(in_ucast)); break;
    case NETCOL_UCAST:          format_number(buf, size, TOTAL(out_ucast) + TOTAL(in_ucast)); break;
    case NETCOL_UCAST_SENT_INTERVAL:     format_number(buf, size, DELTA(out_ucast)); break;
    case NETCOL_UCAST_RECEIVED_INTERVAL: format_number(buf, size, DELTA(in_ucast)); break;
    case NETCOL_UCAST_INTERVAL:          format_number(buf, size, DELTA(out_ucast) + DELTA(in_ucast)); break;
    case NETCOL_NUCAST_SENT:     format_number(buf, size, TOTAL(out_nucast)); break;
    case NETCOL_NUCAST_RECEIVED: format_number(buf, size, TOTAL(in_nucast)); break;
    case NETCOL_NUCAST:          format_number(buf, size, TOTAL(out_nucast) + TOTAL(in_nucast)); break;
    case NETCOL_NUCAST_SENT_INTERVAL:     format_number(buf, size, DELTA(out_nucast)); break;
    case NETCOL_NUCAST_RECEIVED_INTERVAL: format_number(buf, size, DELTA(in_nucast)); break;
    case NETCOL_NUCAST_INTERVAL:          format_number(buf, size, DELTA(out_nucast) + DELTA(in_nucast)); break;
#undef TOTAL
#undef DELTA
    }
}

static void fill_list(void)
{
    WCHAR text[IF_MAX_STRING_SIZE + 1];
    LVITEMW item;
    unsigned i;
    int c;

    SendMessageW(hAdapterList, WM_SETREDRAW, FALSE, 0);
    while (SendMessageW(hAdapterList, LVM_GETITEMCOUNT, 0, 0) > adapter_count)
        SendMessageW(hAdapterList, LVM_DELETEITEM, adapter_count, 0);
    for (i = 0; i < adapter_count; i++)
    {
        for (c = 0; c < column_count; c++)
        {
            format_cell(&adapters[i], column_map[c], text, ARRAY_SIZE(text));
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.iSubItem = c;
            item.pszText = text;
            if (!c && i >= SendMessageW(hAdapterList, LVM_GETITEMCOUNT, 0, 0))
                SendMessageW(hAdapterList, LVM_INSERTITEMW, 0, (LPARAM)&item);
            else
                SendMessageW(hAdapterList, LVM_SETITEMTEXTW, i, (LPARAM)&item);
        }
    }
    SendMessageW(hAdapterList, WM_SETREDRAW, TRUE, 0);
}

void RefreshNetworkPage(void)
{
    unsigned i;

    if (!hNetworkPage) return;
    if (!TaskManagerSettings.NetTabAlwaysActive && !IsWindowVisible(hNetworkPage) && adapter_count) return;

    if (update_adapters()) layout_graphs();
    append_samples();
    fill_list();
    for (i = 0; i < adapter_count; i++)
        InvalidateRect(adapters[i].graph, NULL, FALSE);
}

/* ---- Select Columns ---- */

static INT_PTR CALLBACK columns_dlg_proc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    int i;

    switch (msg)
    {
    case WM_INITDIALOG:
        for (i = 0; i < NET_COLUMN_COUNT; i++)
            CheckDlgButton(hDlg, IDC_NETCOL_FIRST + i,
                           i == NETCOL_NAME || TaskManagerSettings.NetColumns[i] ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            save_column_widths();
            for (i = 0; i < NET_COLUMN_COUNT; i++)
                TaskManagerSettings.NetColumns[i] = IsDlgButtonChecked(hDlg, IDC_NETCOL_FIRST + i) == BST_CHECKED;
            EndDialog(hDlg, IDOK);
        }
        else if (LOWORD(wParam) == IDCANCEL) EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* ---- menus ---- */

static void append_string(HMENU menu, UINT flags, UINT_PTR id, UINT string)
{
    WCHAR text[64];

    LoadStringW(hInst, string, text, ARRAY_SIZE(text));
    AppendMenuW(menu, flags, id, text);
}

void NetworkPage_InitMenus(HMENU hOptionsMenu, HMENU hViewMenu)
{
    HMENU history = CreatePopupMenu();

    AppendMenuW(hOptionsMenu, MF_SEPARATOR, 0, NULL);
    append_string(hOptionsMenu, MF_STRING | (TaskManagerSettings.NetTabAlwaysActive ? MF_CHECKED : 0),
                  ID_OPTIONS_NET_TABALWAYSACTIVE, IDS_OPT_NET_TABALWAYSACTIVE);
    append_string(hOptionsMenu, MF_STRING | (TaskManagerSettings.NetShowCumulative ? MF_CHECKED : 0),
                  ID_OPTIONS_NET_SHOWCUMULATIVE, IDS_OPT_NET_SHOWCUMULATIVE);
    append_string(hOptionsMenu, MF_STRING | (TaskManagerSettings.NetAutoScale ? MF_CHECKED : 0),
                  ID_OPTIONS_NET_AUTOSCALE, IDS_OPT_NET_AUTOSCALE);
    append_string(hOptionsMenu, MF_STRING, ID_OPTIONS_NET_RESET, IDS_OPT_NET_RESET);
    append_string(hOptionsMenu, MF_STRING | (TaskManagerSettings.NetShowScale ? MF_CHECKED : 0),
                  ID_OPTIONS_NET_SHOWSCALE, IDS_OPT_NET_SHOWSCALE);

    append_string(history, MF_STRING | (TaskManagerSettings.NetHistory & NET_HISTORY_SENT ? MF_CHECKED : 0),
                  ID_VIEW_NETHISTORY_SENT, IDS_NETHISTORY_SENT);
    append_string(history, MF_STRING | (TaskManagerSettings.NetHistory & NET_HISTORY_RECEIVED ? MF_CHECKED : 0),
                  ID_VIEW_NETHISTORY_RECEIVED, IDS_NETHISTORY_RECEIVED);
    append_string(history, MF_STRING | (TaskManagerSettings.NetHistory & NET_HISTORY_TOTAL ? MF_CHECKED : 0),
                  ID_VIEW_NETHISTORY_TOTAL, IDS_NETHISTORY_TOTAL);
    append_string(hViewMenu, MF_STRING | MF_POPUP, (UINT_PTR)history, IDS_VIEW_NETHISTORY);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, NULL);
    append_string(hViewMenu, MF_STRING, ID_VIEW_SELECTCOLUMNS, IDS_VIEW_SELECTCOLUMNS);
}

static void toggle(BOOL *setting, UINT id)
{
    *setting = !*setting;
    CheckMenuItem(GetMenu(hMainWnd), id, MF_BYCOMMAND | (*setting ? MF_CHECKED : MF_UNCHECKED));
}

static void toggle_history(DWORD flag, UINT id)
{
    /* one of the three stays on */
    if ((TaskManagerSettings.NetHistory & ~flag) || !(TaskManagerSettings.NetHistory & flag))
        TaskManagerSettings.NetHistory ^= flag;
    CheckMenuItem(GetMenu(hMainWnd), id,
                  MF_BYCOMMAND | (TaskManagerSettings.NetHistory & flag ? MF_CHECKED : MF_UNCHECKED));
}

void NetworkPage_OnCommand(UINT id)
{
    unsigned i;

    switch (id)
    {
    case ID_OPTIONS_NET_TABALWAYSACTIVE: toggle(&TaskManagerSettings.NetTabAlwaysActive, id); break;
    case ID_OPTIONS_NET_SHOWCUMULATIVE:  toggle(&TaskManagerSettings.NetShowCumulative, id); fill_list(); break;
    case ID_OPTIONS_NET_AUTOSCALE:       toggle(&TaskManagerSettings.NetAutoScale, id); break;
    case ID_OPTIONS_NET_SHOWSCALE:       toggle(&TaskManagerSettings.NetShowScale, id); break;
    case ID_OPTIONS_NET_RESET:
        for (i = 0; i < adapter_count; i++) reset_history(&adapters[i]);
        fill_list();
        break;
    case ID_VIEW_NETHISTORY_SENT:     toggle_history(NET_HISTORY_SENT, id); break;
    case ID_VIEW_NETHISTORY_RECEIVED: toggle_history(NET_HISTORY_RECEIVED, id); break;
    case ID_VIEW_NETHISTORY_TOTAL:    toggle_history(NET_HISTORY_TOTAL, id); break;
    case ID_VIEW_SELECTCOLUMNS:
        if (DialogBoxW(hInst, MAKEINTRESOURCEW(IDD_NET_COLUMNS_DIALOG), hMainWnd, columns_dlg_proc) == IDOK)
        {
            add_columns();
            fill_list();
        }
        return;
    }
    for (i = 0; i < adapter_count; i++)
        InvalidateRect(adapters[i].graph, NULL, FALSE);
}

/* ---- the page ---- */

static void move_control(HWND hDlg, HWND ctl, int dx, int dy, int dcx, int dcy)
{
    RECT rc;

    GetWindowRect(ctl, &rc);
    MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
    SetWindowPos(ctl, NULL, rc.left + dx, rc.top + dy, rc.right - rc.left + dcx, rc.bottom - rc.top + dcy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK NetworkPageWndProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    RECT rc;
    int dx, dy;

    switch (message)
    {
    case WM_INITDIALOG:
    {
        WNDCLASSW cls = { 0 };

        hNetworkPage = hDlg;
        GetClientRect(hDlg, &rc);
        nPageWidth = rc.right;
        nPageHeight = rc.bottom;
        SetWindowPos(hDlg, NULL, 15, 30, 0, 0, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOZORDER);

        cls.lpfnWndProc = graph_proc;
        cls.hInstance = hInst;
        cls.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        cls.lpszClassName = GRAPH_CLASS;
        RegisterClassW(&cls);

        hAdapterList = GetDlgItem(hDlg, IDC_NETWORK_ADAPTERS);
        hGraphArea = GetDlgItem(hDlg, IDC_NETWORK_GRAPHAREA);
        hNoAdapters = GetDlgItem(hDlg, IDC_NETWORK_NOADAPTERS);
        SendMessageW(hAdapterList, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
        add_columns();
        update_adapters();
        append_samples();
        layout_graphs();
        fill_list();
        return TRUE;
    }

    case WM_DESTROY:
        save_column_widths();
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        dx = LOWORD(lParam) - nPageWidth;
        dy = HIWORD(lParam) - nPageHeight;
        nPageWidth = LOWORD(lParam);
        nPageHeight = HIWORD(lParam);
        /* the graphs take the new height, the list keeps its own */
        move_control(hDlg, hGraphArea, 0, 0, dx, dy);
        move_control(hDlg, hNoAdapters, 0, 0, dx, dy);
        move_control(hDlg, hAdapterList, 0, dy, dx, 0);
        layout_graphs();
        InvalidateRect(hDlg, NULL, TRUE);
        break;
    }
    return 0;
}
