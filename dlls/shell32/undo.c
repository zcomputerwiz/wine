/*
 * Undo of the shell's file operations, as Windows XP's Edit > Undo (rename, move, copy and delete to the
 * Recycle Bin).
 *
 * Copyright 2026 WineExplorer Desktop Environment Project
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
#include "winuser.h"
#include "shlobj.h"
#include "shellapi.h"
#include "shlwapi.h"

#include "shell32_main.h"
#include "shresdef.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

/* One operation: the items as (where it was, where it is now) pairs; for a delete, where it is now is the
 * Recycle Bin and only the first is kept.  The process keeps the last operations, the newest on top. */
struct undo_op
{
    enum undo_kind kind;
    WCHAR **from, **to;
    UINT count;
};

#define MAX_UNDO 10

static struct undo_op stack[MAX_UNDO];
static UINT depth;
static struct undo_op recording;
static int recording_level;   /* SHFileOperation calls itself for directories */
static BOOL undoing;          /* the undo's own operations are not recorded */

static CRITICAL_SECTION undo_cs;
static CRITICAL_SECTION_DEBUG undo_cs_debug =
{
    0, 0, &undo_cs,
    { &undo_cs_debug.ProcessLocksList, &undo_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": undo_cs") }
};
static CRITICAL_SECTION undo_cs = { &undo_cs_debug, -1, 0, 0, 0, 0 };

static void free_op(struct undo_op *op)
{
    UINT i;

    for (i = 0; i < op->count; i++)
    {
        free(op->from[i]);
        free(op->to[i]);
    }
    free(op->from);
    free(op->to);
    memset(op, 0, sizeof(*op));
}

void undo_begin(enum undo_kind kind)
{
    EnterCriticalSection(&undo_cs);
    if (!undoing && !recording_level++)
    {
        free_op(&recording);
        recording.kind = kind;
    }
    LeaveCriticalSection(&undo_cs);
}

void undo_add(const WCHAR *from, const WCHAR *to)
{
    WCHAR **new_from, **new_to;

    EnterCriticalSection(&undo_cs);
    if (!undoing && recording_level == 1 && recording.kind != UNDO_NONE &&
        (new_from = realloc(recording.from, (recording.count + 1) * sizeof(WCHAR *))))
    {
        recording.from = new_from;
        if ((new_to = realloc(recording.to, (recording.count + 1) * sizeof(WCHAR *))))
        {
            recording.to = new_to;
            recording.from[recording.count] = wcsdup(from);
            recording.to[recording.count] = to ? wcsdup(to) : NULL;
            recording.count++;
        }
    }
    LeaveCriticalSection(&undo_cs);
}

void undo_end(void)
{
    EnterCriticalSection(&undo_cs);
    if (!undoing && recording_level && !--recording_level && recording.count)
    {
        if (depth == MAX_UNDO)
        {
            free_op(&stack[0]);
            memmove(stack, stack + 1, (MAX_UNDO - 1) * sizeof(*stack));
            memset(&stack[MAX_UNDO - 1], 0, sizeof(*stack));
            depth--;
        }
        stack[depth++] = recording;
        memset(&recording, 0, sizeof(recording));
    }
    LeaveCriticalSection(&undo_cs);
}

/* the kind of the operation an undo would revert, UNDO_NONE when there is none */
enum undo_kind undo_top(void)
{
    enum undo_kind kind;

    EnterCriticalSection(&undo_cs);
    kind = depth ? stack[depth - 1].kind : UNDO_NONE;
    LeaveCriticalSection(&undo_cs);
    return kind;
}

/* "&Undo Rename" and so on, for the menus */
void undo_menu_text(WCHAR *text, UINT size)
{
    static const UINT ids[] = { IDS_UNDO, IDS_UNDO_RENAME, IDS_UNDO_MOVE, IDS_UNDO_COPY, IDS_UNDO_DELETE };

    LoadStringW(shell32_hInstance, ids[undo_top()], text, size);
}

static void move_back(const WCHAR *from, const WCHAR *to, HWND hwnd)
{
    WCHAR src[MAX_PATH + 1] = {0}, dst[MAX_PATH + 1] = {0};
    SHFILEOPSTRUCTW op = { hwnd, FO_MOVE, src, dst, FOF_NOCONFIRMMKDIR | FOF_SILENT };

    lstrcpynW(src, to, MAX_PATH);
    lstrcpynW(dst, from, MAX_PATH);
    SHFileOperationW(&op);
}

/* Revert the newest operation.  Windows asks before it deletes the copies of an undone copy. */
BOOL undo_run(HWND hwnd)
{
    struct undo_op op;
    WCHAR *list, *p;
    UINT i, len = 1;
    BOOL ret = TRUE;

    EnterCriticalSection(&undo_cs);
    if (!depth || undoing)
    {
        LeaveCriticalSection(&undo_cs);
        return FALSE;
    }
    op = stack[--depth];
    memset(&stack[depth], 0, sizeof(stack[depth]));
    undoing = TRUE;
    LeaveCriticalSection(&undo_cs);

    TRACE("undoing kind %u, %u items\n", op.kind, op.count);
    switch (op.kind)
    {
    case UNDO_RENAME:
    case UNDO_MOVE:
        for (i = op.count; i-- > 0;) move_back(op.from[i], op.to[i], hwnd);
        break;
    case UNDO_COPY:
        for (i = 0; i < op.count; i++) len += wcslen(op.to[i]) + 1;
        if ((p = list = calloc(len, sizeof(WCHAR))))
        {
            SHFILEOPSTRUCTW del = { hwnd, FO_DELETE, list, NULL, 0 };

            for (i = 0; i < op.count; i++) { wcscpy(p, op.to[i]); p += wcslen(p) + 1; }
            ret = !SHFileOperationW(&del) && !del.fAnyOperationsAborted;
            free(list);
        }
        break;
    case UNDO_DELETE:
        for (i = 0; i < op.count; i++)
            if (restore_trashed_file(op.from[i]))
                SHChangeNotify(PathIsDirectoryW(op.from[i]) ? SHCNE_MKDIR : SHCNE_CREATE, SHCNF_PATHW, op.from[i], NULL);
        break;
    default:
        break;
    }

    EnterCriticalSection(&undo_cs);
    undoing = FALSE;
    LeaveCriticalSection(&undo_cs);
    free_op(&op);
    return ret;
}
