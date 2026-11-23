/*
 * Windows spell checking API: the spell checker factory
 *
 * Copyright 2026 WineExplorer contributors
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

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "rpcproxy.h"
#include "initguid.h"
#include "spellcheck.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(spellcheck);

/* Wine has no spelling dictionaries: the factory exists, as it does on Windows, and supports no language. Callers
 * (Notepad, the Windows App SDK text controls) ask it and then go without spell checking. */

static HRESULT WINAPI enum_string_QueryInterface( IEnumString *iface, REFIID iid, void **out )
{
    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &IID_IEnumString ))
    {
        *out = iface;
        IEnumString_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_string_AddRef( IEnumString *iface )
{
    return 2;
}

static ULONG WINAPI enum_string_Release( IEnumString *iface )
{
    return 1;
}

static HRESULT WINAPI enum_string_Next( IEnumString *iface, ULONG count, LPOLESTR *strings, ULONG *fetched )
{
    if (fetched) *fetched = 0;
    return count ? S_FALSE : S_OK;
}

static HRESULT WINAPI enum_string_Skip( IEnumString *iface, ULONG count )
{
    return count ? S_FALSE : S_OK;
}

static HRESULT WINAPI enum_string_Reset( IEnumString *iface )
{
    return S_OK;
}

static HRESULT WINAPI enum_string_Clone( IEnumString *iface, IEnumString **out )
{
    *out = iface;
    return S_OK;
}

static const IEnumStringVtbl enum_string_vtbl =
{
    enum_string_QueryInterface,
    enum_string_AddRef,
    enum_string_Release,
    enum_string_Next,
    enum_string_Skip,
    enum_string_Reset,
    enum_string_Clone,
};

static IEnumString empty_enum = { &enum_string_vtbl };

static HRESULT WINAPI factory_QueryInterface( ISpellCheckerFactory *iface, REFIID iid, void **out )
{
    TRACE( "%p, %s, %p\n", iface, debugstr_guid( iid ), out );

    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &IID_ISpellCheckerFactory ))
    {
        *out = iface;
        ISpellCheckerFactory_AddRef( iface );
        return S_OK;
    }
    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( ISpellCheckerFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( ISpellCheckerFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_get_SupportedLanguages( ISpellCheckerFactory *iface, IEnumString **value )
{
    TRACE( "%p, %p\n", iface, value );

    if (!value) return E_POINTER;
    *value = &empty_enum;
    return S_OK;
}

static HRESULT WINAPI factory_IsSupported( ISpellCheckerFactory *iface, LPCWSTR tag, BOOL *value )
{
    TRACE( "%p, %s, %p\n", iface, debugstr_w( tag ), value );

    if (!tag || !value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI factory_CreateSpellChecker( ISpellCheckerFactory *iface, LPCWSTR tag, ISpellChecker **value )
{
    TRACE( "%p, %s, %p\n", iface, debugstr_w( tag ), value );

    if (!tag || !value) return E_POINTER;
    *value = NULL;
    return E_INVALIDARG;
}

static const ISpellCheckerFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_get_SupportedLanguages,
    factory_IsSupported,
    factory_CreateSpellChecker,
};

static ISpellCheckerFactory factory = { &factory_vtbl };

static HRESULT WINAPI class_factory_QueryInterface( IClassFactory *iface, REFIID iid, void **out )
{
    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &IID_IClassFactory ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI class_factory_AddRef( IClassFactory *iface )
{
    return 2;
}

static ULONG WINAPI class_factory_Release( IClassFactory *iface )
{
    return 1;
}

static HRESULT WINAPI class_factory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID iid, void **out )
{
    TRACE( "%p, %p, %s, %p\n", iface, outer, debugstr_guid( iid ), out );

    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return ISpellCheckerFactory_QueryInterface( &factory, iid, out );
}

static HRESULT WINAPI class_factory_LockServer( IClassFactory *iface, BOOL lock )
{
    return S_OK;
}

static const IClassFactoryVtbl class_factory_vtbl =
{
    class_factory_QueryInterface,
    class_factory_AddRef,
    class_factory_Release,
    class_factory_CreateInstance,
    class_factory_LockServer,
};

static IClassFactory class_factory = { &class_factory_vtbl };

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID iid, void **out )
{
    TRACE( "%s, %s, %p\n", debugstr_guid( clsid ), debugstr_guid( iid ), out );

    if (!IsEqualCLSID( clsid, &CLSID_SpellCheckerFactory )) return CLASS_E_CLASSNOTAVAILABLE;
    return IClassFactory_QueryInterface( &class_factory, iid, out );
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    return __wine_register_resources();
}

HRESULT WINAPI DllUnregisterServer(void)
{
    return __wine_unregister_resources();
}
