/*
 * msstyles v4+ visual style format reader (Windows Vista through 11)
 *
 * Copyright (C) 2026 zcomputerwiz
 *
 * Portions derived from libmsstyle (https://github.com/nptr/msstyleEditor)
 * Copyright (c) 2017-2022 nptr
 * Copyright (c) 2019 Jakob K.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * The Wine implementation is distributed under the following license:
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
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "vfwmsgs.h"
#include "uxtheme.h"
#include "vssym32.h"

#include "msstyles.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

static const WCHAR normalColor[] = L"NormalColor\0";
static const WCHAR normalSize[]  = L"NormalSize\0";

/* libmsstyle primitive type not yet declared in vssym32.h. */
#define MSSTYLES_HIGHCONTRASTCOLORTYPE 241

struct v4_class_record {
    int class_id;
    WCHAR raw_name[MAX_THEME_APP_NAME + MAX_THEME_CLASS_NAME];
    WCHAR app_name[MAX_THEME_APP_NAME];
    WCHAR class_name[MAX_THEME_CLASS_NAME];
    PTHEME_CLASS cls;
};

/***********************************************************************
 *      MSSTYLES_OpenThemeFile_V4
 *
 * Load and parse a Vista through 11 v4 msstyles visual style file.
 */
HRESULT MSSTYLES_OpenThemeFile_V4(HMODULE hTheme, LPCWSTR lpThemeFile,
                                  LPCWSTR pszColorName, LPCWSTR pszSizeName,
                                  PTHEME_FILE *tf)
{
    HRSRC hrsc_cmap;
    HRSRC hrsc_var;
    DWORD cmap_bytes;
    DWORD var_bytes;
    const WCHAR *cmap_data;
    const BYTE *var_data;
    DWORD total_wchars;
    DWORD pos;
    int class_count = 0;
    int idx = 0;
    int i;
    struct v4_class_record *v4_classes = NULL;
    DWORD cursor = 0;
    PTHEME_CLASS globals;
    PTHEME_CLASS cur_cls;

    TRACE("Opening v4 theme file %s\n", debugstr_w(lpThemeFile));

    /* Validate color and size schemes */
    if (pszColorName && lstrcmpiW(pszColorName, normalColor)) {
        TRACE("Requested color (%s) not found in theme\n", debugstr_w(pszColorName));
        FreeLibrary(hTheme);
        *tf = NULL;
        return E_PROP_ID_UNSUPPORTED;
    }
    if (pszSizeName && lstrcmpiW(pszSizeName, normalSize)) {
        TRACE("Requested size (%s) not found in theme\n", debugstr_w(pszSizeName));
        FreeLibrary(hTheme);
        *tf = NULL;
        return E_PROP_ID_UNSUPPORTED;
    }

    /* Locate and validate CMAP resource */
    hrsc_cmap = FindResourceW(hTheme, L"CMAP", L"CMAP");
    if (!hrsc_cmap)
        hrsc_cmap = FindResourceW(hTheme, MAKEINTRESOURCEW(1), L"CMAP");
    if (!hrsc_cmap) {
        TRACE("CMAP resource not found\n");
        FreeLibrary(hTheme);
        *tf = NULL;
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    cmap_bytes = SizeofResource(hTheme, hrsc_cmap);
    cmap_data = LoadResource(hTheme, hrsc_cmap);
    if (!cmap_data || cmap_bytes < sizeof(WCHAR) * 2) {
        TRACE("CMAP resource empty or invalid size: %lu\n", (unsigned long)cmap_bytes);
        FreeLibrary(hTheme);
        *tf = NULL;
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    total_wchars = cmap_bytes / sizeof(WCHAR);

    /* First pass: count non-empty strings in CMAP */
    pos = 0;
    while (pos < total_wchars) {
        DWORD start = pos;
        while (pos < total_wchars && cmap_data[pos] != 0)
            pos++;
        if (pos > start)
            class_count++;
        if (pos < total_wchars && cmap_data[pos] == 0)
            pos++;
    }

    if (class_count == 0) {
        TRACE("CMAP contains no valid classes\n");
        FreeLibrary(hTheme);
        *tf = NULL;
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    v4_classes = calloc(class_count, sizeof(*v4_classes));
    if (!v4_classes) {
        free(v4_classes);
        FreeLibrary(hTheme);
        *tf = NULL;
        return E_OUTOFMEMORY;
    }

    /* Second pass: record classes */
    pos = 0;
    idx = 0;
    while (pos < total_wchars && idx < class_count) {
        DWORD start = pos;
        while (pos < total_wchars && cmap_data[pos] != 0)
            pos++;
        if (pos > start) {
            DWORD len = pos - start;
            v4_classes[idx].class_id = idx;
            lstrcpynW(v4_classes[idx].raw_name, &cmap_data[start], min(len + 1, ARRAY_SIZE(v4_classes[idx].raw_name)));
            parse_app_class_name(v4_classes[idx].raw_name, v4_classes[idx].app_name, v4_classes[idx].class_name);
            idx++;
        }
        if (pos < total_wchars && cmap_data[pos] == 0)
            pos++;
    }

    TRACE("Read %d classes\n", class_count);

    /* Allocate THEME_FILE structure */
    *tf = calloc(1, sizeof(THEME_FILE));
    if (!*tf) {
        free(v4_classes);
        FreeLibrary(hTheme);
        return E_OUTOFMEMORY;
    }
    (*tf)->hTheme = hTheme;
    GetFullPathNameW(lpThemeFile, MAX_PATH, (*tf)->szThemeFile, NULL);
    (*tf)->pszAvailColors = (LPWSTR)normalColor;
    (*tf)->pszAvailSizes = (LPWSTR)normalSize;
    (*tf)->pszSelectedColor = (LPWSTR)normalColor;
    (*tf)->pszSelectedSize = (LPWSTR)normalSize;
    (*tf)->refcount = 1;

    /* Add all classes to THEME_FILE */
    for (i = 0; i < class_count; i++) {
        v4_classes[i].cls = MSSTYLES_AddClass(*tf, v4_classes[i].app_name, v4_classes[i].class_name);
    }

    /* Locate and validate VARIANT resource */
    hrsc_var = FindResourceW(hTheme, L"NORMAL", L"VARIANT");
    if (!hrsc_var)
        hrsc_var = FindResourceW(hTheme, MAKEINTRESOURCEW(1), L"VARIANT");
    if (!hrsc_var) {
        TRACE("VARIANT resource NORMAL not found\n");
        MSSTYLES_CloseThemeFile(*tf);
        *tf = NULL;
        free(v4_classes);
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    var_bytes = SizeofResource(hTheme, hrsc_var);
    var_data = LoadResource(hTheme, hrsc_var);
    if (!var_data || var_bytes < 32) {
        TRACE("VARIANT resource empty or too small: %lu\n", (unsigned long)var_bytes);
        MSSTYLES_CloseThemeFile(*tf);
        *tf = NULL;
        free(v4_classes);
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    /* Parse binary property stream */
    cursor = 0;
    while (cursor + 32 <= var_bytes) {
        const int32_t *hdr = (const int32_t *)(var_data + cursor);
        int32_t name_id = hdr[0];
        int32_t type_id = hdr[1];
        int32_t class_id = hdr[2];
        int32_t part_id = hdr[3];
        int32_t state_id = hdr[4];
        int32_t short_flag = hdr[5];
        int32_t reserved = hdr[6];
        int32_t size_in_bytes = hdr[7];
        PTHEME_CLASS cls;
        PTHEME_PARTSTATE ps;
        PTHEME_PROPERTY prop;
        PTHEME_PROPERTY existing;

        (void)reserved;

        cursor += 32;

        /* Bounds check on header fields */
        if (part_id < 0 || part_id > 199 ||
            state_id < 0 || state_id > 199 ||
            class_id < 0 || class_id >= class_count ||
            type_id < 200 || type_id > 241 ||
            name_id < 0 || name_id > 25000)
        {
            WARN("Malformed property header: name=%d type=%d class=%d part=%d state=%d\n",
                 name_id, type_id, class_id, part_id, state_id);
            goto malformed;
        }

        cls = v4_classes[class_id].cls;
        ps = MSSTYLES_AddPartState(cls, part_id, state_id);

        prop = calloc(1, sizeof(THEME_PROPERTY));
        if (!prop) goto oom;

        prop->iPropertyId = name_id;
        prop->iPrimitiveType = type_id;
        prop->is_typed = TRUE;

        if (state_id)
            prop->origin = PO_STATE;
        else if (part_id)
            prop->origin = PO_PART;
        else if (!lstrcmpiW(cls->szClassName, L"globals") || !lstrcmpiW(cls->szClassName, L"sysmetrics"))
            prop->origin = PO_GLOBAL;
        else
            prop->origin = PO_CLASS;

        switch (type_id) {
        case TMT_ENUM:
        case TMT_INT:
        case TMT_SIZE:
        case MSSTYLES_HIGHCONTRASTCOLORTYPE:
            if (short_flag == 0) {
                if (cursor + 4 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.int_val = *(const int32_t *)(var_data + cursor);
                cursor += 8;
            } else {
                prop->v.int_val = 0;
            }
            break;

        case TMT_BOOL:
            if (short_flag == 0) {
                if (cursor + 4 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.bool_val = (*(const int32_t *)(var_data + cursor) != 0);
                cursor += 8;
            } else {
                prop->v.bool_val = FALSE;
            }
            break;

        case TMT_COLOR:
            if (short_flag == 0) {
                const BYTE *col;
                if (cursor + 4 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                col = var_data + cursor;
                prop->v.color_val = RGB(col[0], col[1], col[2]);
                cursor += 8;
            } else {
                prop->v.color_val = RGB(0, 0, 0);
            }
            break;

        case TMT_POSITION:
            if (short_flag == 0) {
                if (cursor + 8 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.point_val.x = *(const int32_t *)(var_data + cursor);
                prop->v.point_val.y = *(const int32_t *)(var_data + cursor + 4);
                cursor += 8;
            } else {
                prop->v.point_val.x = 0;
                prop->v.point_val.y = 0;
            }
            break;

        case TMT_RECT:
            if (short_flag == 0) {
                if (cursor + 16 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.rect_val.left   = *(const int32_t *)(var_data + cursor);
                prop->v.rect_val.top    = *(const int32_t *)(var_data + cursor + 4);
                prop->v.rect_val.right  = *(const int32_t *)(var_data + cursor + 8);
                prop->v.rect_val.bottom = *(const int32_t *)(var_data + cursor + 12);
                cursor += 16;
            }
            break;

        case TMT_MARGINS:
            if (short_flag == 0) {
                if (cursor + 16 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.margins_val.cxLeftWidth    = *(const int32_t *)(var_data + cursor);
                prop->v.margins_val.cxRightWidth   = *(const int32_t *)(var_data + cursor + 4);
                prop->v.margins_val.cyTopHeight    = *(const int32_t *)(var_data + cursor + 8);
                prop->v.margins_val.cyBottomHeight = *(const int32_t *)(var_data + cursor + 12);
                cursor += 16;
            }
            break;

        case TMT_INTLIST:
            if (size_in_bytes != 0) {
                int32_t num;
                int k;
                if (cursor + 4 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                num = *(const int32_t *)(var_data + cursor);
                cursor += 4;
                if (num < 0 || cursor + (DWORD)num * 4 > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                prop->v.intlist_val.iValueCount = min(num, MAX_INTLIST_COUNT);
                for (k = 0; k < prop->v.intlist_val.iValueCount; k++) {
                    prop->v.intlist_val.iValues[k] = *(const int32_t *)(var_data + cursor + k * 4);
                }
                cursor += num * 4;
            }
            break;

        case TMT_STRING:
            if (size_in_bytes > 0) {
                DWORD wchars;
                WCHAR *str;
                if (cursor + size_in_bytes > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                wchars = size_in_bytes / sizeof(WCHAR);
                str = malloc((wchars + 1) * sizeof(WCHAR));
                if (!str) {
                    free(prop);
                    goto oom;
                }
                memcpy(str, var_data + cursor, size_in_bytes);
                str[wchars] = 0;
                if (wchars > 0 && str[wchars - 1] == 0)
                    prop->dwValueLen = wchars - 1;
                else
                    prop->dwValueLen = wchars;
                prop->lpValue = str;
                cursor += size_in_bytes;
            }
            break;

        case TMT_FILENAME:
        case TMT_DISKSTREAM:
        case TMT_STREAM:
            {
                WCHAR *res_name = malloc(16 * sizeof(WCHAR));
                if (!res_name) {
                    free(prop);
                    goto oom;
                }
                prop->v.res_id = (DWORD)short_flag;
                swprintf(res_name, 16, L"#%u", (unsigned int)short_flag);
                prop->lpValue = res_name;
                prop->dwValueLen = wcslen(res_name);
            }
            break;

        case TMT_FONT:
            {
                WCHAR font_buf[128] = {0};
                prop->v.res_id = (DWORD)short_flag;
                if (LoadStringW(hTheme, short_flag, font_buf, ARRAY_SIZE(font_buf)) > 0) {
                    prop->lpValue = wcsdup(font_buf);
                } else {
                    prop->lpValue = wcsdup(L"Segoe UI, 9");
                }
                if (!prop->lpValue) {
                    free(prop);
                    goto oom;
                }
                prop->dwValueLen = wcslen(prop->lpValue);
            }
            break;

        default:
            if (short_flag == 0 && size_in_bytes > 0) {
                if (cursor + size_in_bytes > var_bytes) {
                    free(prop);
                    goto malformed;
                }
                cursor += size_in_bytes;
            }
            break;
        }

        /* Align to 8 bytes */
        if (cursor % 8 != 0)
            cursor = (cursor + 7) & ~7;

        /* Insert property into part/state, ignoring duplicate definitions */
        existing = ps->properties;
        while (existing && existing->iPropertyId != prop->iPropertyId)
            existing = existing->next;

        if (!existing) {
            prop->next = ps->properties;
            ps->properties = prop;
        } else {
            if (prop->lpValue) free((void *)prop->lpValue);
            free(prop);
            prop = existing;
        }

        /* If this belongs to sysmetrics, also copy to tf->metrics */
        if (!lstrcmpiW(cls->szClassName, L"sysmetrics")) {
            PTHEME_PROPERTY m = calloc(1, sizeof(THEME_PROPERTY));
            if (m) {
                *m = *prop;
                if (prop->lpValue)
                    m->lpValue = wcsdup(prop->lpValue);
                m->origin = PO_GLOBAL;
                m->next = (*tf)->metrics;
                (*tf)->metrics = m;
            }
        }
    }

    /* Build class override chain */
    globals = MSSTYLES_FindClass(*tf, NULL, L"globals");
    cur_cls = (*tf)->classes;
    while (cur_cls) {
        if (*cur_cls->szAppName) {
            PTHEME_CLASS base = MSSTYLES_FindClass(*tf, NULL, cur_cls->szClassName);
            cur_cls->overrides = base ? base : globals;
        } else if (cur_cls != globals) {
            cur_cls->overrides = globals;
        }
        cur_cls = cur_cls->next;
    }

    free(v4_classes);
    return S_OK;

malformed:
    TRACE("Malformed stream encountered at offset %lu\n", (unsigned long)cursor);
    MSSTYLES_CloseThemeFile(*tf);
    *tf = NULL;
    free(v4_classes);
    return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);

oom:
    MSSTYLES_CloseThemeFile(*tf);
    *tf = NULL;
    free(v4_classes);
    return E_OUTOFMEMORY;
}
