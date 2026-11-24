/*
 * Win32 5.1 Theme properties
 *
 * Copyright (C) 2003 Kevin Koltzau
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
#include "wingdi.h"
#include "vfwmsgs.h"
#include "uxtheme.h"
#include "vssym32.h"

#include "msstyles.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

/***********************************************************************
 *      GetThemeBool                                        (UXTHEME.@)
 */
HRESULT WINAPI GetThemeBool(HTHEME hTheme, int iPartId, int iStateId,
                            int iPropId, BOOL *pfVal)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_BOOL, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyBool(tp, pfVal);
}

/***********************************************************************
 *      GetThemeColor                                       (UXTHEME.@)
 */
HRESULT WINAPI GetThemeColor(HTHEME hTheme, int iPartId, int iStateId,
                             int iPropId, COLORREF *pColor)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_COLOR, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyColor(tp, pColor);
}

/***********************************************************************
 *      GetThemeEnumValue                                   (UXTHEME.@)
 */
HRESULT WINAPI GetThemeEnumValue(HTHEME hTheme, int iPartId, int iStateId,
                                 int iPropId, int *piVal)
{
    HRESULT hr;
    WCHAR val[60];
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_ENUM, iPropId)))
        return E_PROP_ID_UNSUPPORTED;

    if (tp->is_typed) {
        *piVal = tp->v.int_val;
        return S_OK;
    }

    hr = MSSTYLES_GetPropertyString(tp, val, ARRAY_SIZE(val));
    if(FAILED(hr))
        return hr;
    if(!MSSTYLES_LookupEnum(val, iPropId, piVal))
        return E_PROP_ID_UNSUPPORTED;
    return S_OK;
}

/***********************************************************************
 *      GetThemeFilename                                    (UXTHEME.@)
 */
HRESULT WINAPI GetThemeFilename(HTHEME hTheme, int iPartId, int iStateId,
                                int iPropId, LPWSTR pszThemeFilename,
                                int cchMaxBuffChars)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_FILENAME, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyString(tp, pszThemeFilename, cchMaxBuffChars);
}

/***********************************************************************
 *      GetThemeFont                                        (UXTHEME.@)
 */
HRESULT WINAPI GetThemeFont(HTHEME hTheme, HDC hdc, int iPartId,
                            int iStateId, int iPropId, LOGFONTW *pFont)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_FONT, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyFont(tp, hdc, pFont);
}

/***********************************************************************
 *      GetThemeInt                                         (UXTHEME.@)
 */
HRESULT WINAPI GetThemeInt(HTHEME hTheme, int iPartId, int iStateId,
                           int iPropId, int *piVal)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_INT, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyInt(tp, piVal);
}

/***********************************************************************
 *      GetThemeIntList                                     (UXTHEME.@)
 */
HRESULT WINAPI GetThemeIntList(HTHEME hTheme, int iPartId, int iStateId,
                               int iPropId, INTLIST *pIntList)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_INTLIST, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyIntList(tp, pIntList);
}

/***********************************************************************
 *      GetThemePosition                                    (UXTHEME.@)
 */
HRESULT WINAPI GetThemePosition(HTHEME hTheme, int iPartId, int iStateId,
                                int iPropId, POINT *pPoint)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_POSITION, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyPosition(tp, pPoint);
}

/***********************************************************************
 *      GetThemeRect                                        (UXTHEME.@)
 */
HRESULT WINAPI GetThemeRect(HTHEME hTheme, int iPartId, int iStateId,
                            int iPropId, RECT *pRect)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_RECT, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyRect(tp, pRect);
}

/***********************************************************************
 *      GetThemeString                                      (UXTHEME.@)
 */
HRESULT WINAPI GetThemeString(HTHEME hTheme, int iPartId, int iStateId,
                              int iPropId, LPWSTR pszBuff, int cchMaxBuffChars)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_STRING, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyString(tp, pszBuff, cchMaxBuffChars);
}

/***********************************************************************
 *      GetThemeMargins                                     (UXTHEME.@)
 */
HRESULT WINAPI GetThemeMargins(HTHEME hTheme, HDC hdc, int iPartId,
                               int iStateId, int iPropId, RECT *prc,
                               MARGINS *pMargins)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    memset (pMargins, 0, sizeof (MARGINS));
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, TMT_MARGINS, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    return MSSTYLES_GetPropertyMargins(tp, prc, pMargins);
}

/***********************************************************************
 *      GetThemeMetric                                      (UXTHEME.@)
 */
HRESULT WINAPI GetThemeMetric(HTHEME hTheme, HDC hdc, int iPartId,
                              int iStateId, int iPropId, int *piVal)
{
    PTHEME_PROPERTY tp;
    WCHAR val[60];
    HRESULT hr;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, 0, iPropId)))
        return E_PROP_ID_UNSUPPORTED;
    switch(tp->iPrimitiveType) {
        case TMT_POSITION: /* Only the X coord is retrieved */
        case TMT_MARGINS: /* Only the cxLeftWidth member is retrieved */
        case TMT_INTLIST: /* Only the first int is retrieved */
        case TMT_SIZE:
        case TMT_INT:
            return MSSTYLES_GetPropertyInt(tp, piVal);
        case TMT_BOOL:
            return MSSTYLES_GetPropertyBool(tp, piVal);
        case TMT_COLOR:
            return MSSTYLES_GetPropertyColor(tp, (COLORREF*)piVal);
        case TMT_ENUM:
            if (tp->is_typed) {
                *piVal = tp->v.int_val;
                return S_OK;
            }
            hr = MSSTYLES_GetPropertyString(tp, val, ARRAY_SIZE(val));
            if(FAILED(hr))
                return hr;
            if(!MSSTYLES_LookupEnum(val, iPropId, piVal))
                return E_PROP_ID_UNSUPPORTED;
            return S_OK;
         case TMT_FILENAME:
             /* Windows does return a value for this, but its value doesn't make sense */
             FIXME("Filename\n");
             break;
    }
    return E_PROP_ID_UNSUPPORTED;
}

/***********************************************************************
 *      GetThemePropertyOrigin                              (UXTHEME.@)
 */
HRESULT WINAPI GetThemePropertyOrigin(HTHEME hTheme, int iPartId, int iStateId,
                                      int iPropId, PROPERTYORIGIN *pOrigin)
{
    PTHEME_PROPERTY tp;

    TRACE("(%d, %d, %d)\n", iPartId, iStateId, iPropId);
    if(!hTheme)
        return E_HANDLE;

    if(!(tp = MSSTYLES_FindProperty(hTheme, iPartId, iStateId, 0, iPropId))) {
        *pOrigin = PO_NOTFOUND;
        return S_OK;
    }
    *pOrigin = tp->origin;
    return S_OK;
}

/* The themes of Wine carry no animation data (the storyboards of the Windows themes), so the animation queries find nothing. The
 * results for missing data and bad arguments are those of Windows 11. */

/***********************************************************************
 *      GetThemeAnimationProperty                           (UXTHEME.@)
 */
HRESULT WINAPI GetThemeAnimationProperty(HTHEME theme, int storyboard, int target, TA_PROPERTY property, void *value,
                                         DWORD size, DWORD *size_out)
{
    TRACE("(%p, %d, %d, %d, %p, %lu, %p)\n", theme, storyboard, target, property, value, size, size_out);

    if (!theme || !value || !size_out) return E_INVALIDARG;
    if (property < TAP_FLAGS || property > TAP_ZORDER)
    {
        *size_out = 0;
        return E_INVALIDARG;
    }
    /* every property is a DWORD (or a float of the same size) */
    *size_out = sizeof(DWORD);
    if (size < sizeof(DWORD)) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

/***********************************************************************
 *      GetThemeAnimationTransform                          (UXTHEME.@)
 */
HRESULT WINAPI GetThemeAnimationTransform(HTHEME theme, int storyboard, int target, DWORD index, TA_TRANSFORM *transform,
                                          DWORD size, DWORD *size_out)
{
    TRACE("(%p, %d, %d, %lu, %p, %lu, %p)\n", theme, storyboard, target, index, transform, size, size_out);

    if (!theme) return E_INVALIDARG;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

/***********************************************************************
 *      GetThemeTimingFunction                              (UXTHEME.@)
 */
HRESULT WINAPI GetThemeTimingFunction(HTHEME theme, int id, TA_TIMINGFUNCTION *function, DWORD size, DWORD *size_out)
{
    TRACE("(%p, %d, %p, %lu, %p)\n", theme, id, function, size, size_out);

    if (!theme) return E_INVALIDARG;
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}
