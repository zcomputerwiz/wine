/* Windows.Internal.StateRepository.Package (a logging stand-in)
 *
 * The package state repository of Windows is internal and undocumented. The Windows App Runtime asks the activation factory of
 * Windows.Internal.StateRepository.Package for an interface (4408fb32-c420-41c5-b6c1-edce489bb84d) while it starts, and
 * the application ends soon after when the class is missing. The methods of that interface are not known. This stand-in answers
 * the factory request with an object whose methods all log their number and arguments and fail, which shows what the caller
 * wants; it is a probe, to be replaced by the real behaviour once that is known.
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

#include <stdio.h>

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(model);

#define SLOT_COUNT 64   /* methods after the six of IInspectable */

/* what an argument looks like in the log: a number, or a pointer with its first eight words, and the wide string any of them
 * points to */
static const char *describe( ULONG_PTR arg )
{
    static char buffer[8][600];
    static LONG next;
    char *out = buffer[InterlockedIncrement( &next ) & 7], *p = out;
    ULONG_PTR *words = (ULONG_PTR *)arg;
    unsigned int i;

    if (arg < 0x10000 || IsBadReadPtr( words, 8 * sizeof(*words) ))
        return (snprintf( out, 600, "%#Ix", arg ), out);
    p += snprintf( p, 600, "%p {", words );
    for (i = 0; i < 8; i++)
    {
        const WCHAR *str = (const WCHAR *)words[i];

        if (words[i] >= 0x10000 && !IsBadReadPtr( str, 4 ) && str[0] >= 0x20 && str[0] < 0x7f && str[1] >= 0x20 && str[1] < 0x7f)
            p += snprintf( p, 600 - (p - out), " %#Ix=%s", words[i], debugstr_wn( str, 48 ) );
        else
            p += snprintf( p, 600 - (p - out), " %#Ix", words[i] );
    }
    snprintf( p, 600 - (p - out), " }" );
    return out;
}

static HRESULT slot_log( unsigned int slot, void *iface, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d )
{
    FIXME( "Windows.Internal.StateRepository.Package statics: method %u (vtable slot %u) called, iface %p\n"
           "  arg1 %s\n  arg2 %s\n  arg3 %s\n  arg4 %s\n", slot, slot + 6, iface, describe( a ), describe( b ), describe( c ),
           describe( d ) );
    return E_NOTIMPL;
}

#define SLOT(n) \
    static HRESULT WINAPI slot_##n( void *iface, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d ) { return slot_log( n, iface, a, b, c, d ); }
#define SLOT8(n) SLOT(n##0) SLOT(n##1) SLOT(n##2) SLOT(n##3) SLOT(n##4) SLOT(n##5) SLOT(n##6) SLOT(n##7)
SLOT8(0) SLOT8(1) SLOT8(2) SLOT8(3) SLOT8(4) SLOT8(5) SLOT8(6) SLOT8(7)

struct statics
{
    const void *const *vtbl;
    LONG ref;
};

static HRESULT WINAPI statics_QueryInterface( void *iface, REFIID iid, void **out );
static ULONG WINAPI statics_AddRef( void *iface );
static ULONG WINAPI statics_Release( void *iface );

static HRESULT WINAPI statics_GetIids( void *iface, ULONG *count, IID **iids )
{
    IID *list;

    if (!(list = CoTaskMemAlloc( sizeof(*list) ))) return E_OUTOFMEMORY;
    *list = IID_IStateRepositoryPackageStatics;
    *count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT WINAPI statics_GetRuntimeClassName( void *iface, HSTRING *name )
{
    return WindowsCreateString( L"Windows.Internal.StateRepository.Package",
                                ARRAY_SIZE(L"Windows.Internal.StateRepository.Package") - 1, name );
}

static HRESULT WINAPI statics_GetTrustLevel( void *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

#define S(n) slot_##n
#define S8(n) S(n##0), S(n##1), S(n##2), S(n##3), S(n##4), S(n##5), S(n##6), S(n##7)
static const void *const statics_vtbl[6 + SLOT_COUNT] =
{
    statics_QueryInterface, statics_AddRef, statics_Release, statics_GetIids, statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    S8(0), S8(1), S8(2), S8(3), S8(4), S8(5), S8(6), S8(7),
};

static struct statics statics_object = { statics_vtbl, 1 };

struct factory
{
    IActivationFactory IActivationFactory_iface;
};

static HRESULT WINAPI statics_QueryInterface( void *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IStateRepositoryPackageStatics ))
    {
        *out = &statics_object;
        return S_OK;
    }
    FIXME( "Windows.Internal.StateRepository.Package statics: interface %s asked for.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI statics_AddRef( void *iface )
{
    return InterlockedIncrement( &statics_object.ref );
}

static ULONG WINAPI statics_Release( void *iface )
{
    return InterlockedDecrement( &statics_object.ref );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IStateRepositoryPackageStatics )) return statics_QueryInterface( &statics_object, iid, out );
    FIXME( "Windows.Internal.StateRepository.Package factory: interface %s asked for.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids )
{
    return statics_GetIids( NULL, count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name )
{
    return statics_GetRuntimeClassName( NULL, name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "Windows.Internal.StateRepository.Package: ActivateInstance called.\n" );
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

static struct factory state_repository_package = { {&factory_vtbl} };

IActivationFactory *state_repository_package_factory = &state_repository_package.IActivationFactory_iface;
