/* WinRT Windows.ApplicationModel Implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
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

#ifndef __WINE_WINDOWS_APPLICATIONMODEL_PRIVATE_H
#define __WINE_WINDOWS_APPLICATIONMODEL_PRIVATE_H

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_ApplicationModel
#define WIDL_using_Windows_Storage
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_UI
#include "windows.applicationmodel.h"

extern IActivationFactory *package_factory;
extern IActivationFactory *design_mode_factory;

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )
#define DEFINE_IINSPECTABLE_OUTER( pfx, iface_type, impl_type, outer_iface )                       \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, impl->outer_iface )

/* package resources (WineExplorer UWP U1.6) */
#define WIDL_using_Windows_ApplicationModel_Resources
#define WIDL_using_Windows_ApplicationModel_Resources_Core
#include "windows.applicationmodel.resources.h"
#include "windows.applicationmodel.resources.core.h"

extern IActivationFactory *resource_loader_factory;
extern IActivationFactory *resource_manager_factory;
extern IActivationFactory *resource_context_factory;

/* pri.c */
#define PRI_QUALIFIER_TYPES 12
#define PRI_QUALIFIER_LANGUAGE 0
#define PRI_QUALIFIER_SCALE 2
enum pri_kind { PRI_KIND_STRING, PRI_KIND_FILE, PRI_KIND_EMBEDDED };

struct pri_qualifier
{
    UINT16 type;
    UINT16 priority;
    UINT16 fallback; /* 0 to 1000; 1000 is the default */
    WCHAR *value;
};

struct pri_candidate
{
    int kind;
    WCHAR *value;            /* the text, or the path relative to the package */
    UINT32 *qualifiers;      /* indexes into pri->qualifiers */
    UINT32 qualifier_count;
};

struct pri_item
{
    WCHAR *name;             /* "Resources/Greeting", "Files/Assets/pic.png" */
    UINT32 first;            /* in pri->candidates */
    UINT32 count;
};

struct pri
{
    WCHAR *root;             /* the folder of the package */
    WCHAR *name;             /* the name of the resource map (the package name) */
    struct pri_item *items;
    UINT32 item_count;
    struct pri_candidate *candidates;
    UINT32 candidate_count;
    struct pri_qualifier *qualifiers;
    UINT32 qualifier_count;
    UINT32 *set_qualifiers;
};

extern const WCHAR *const pri_qualifier_names[PRI_QUALIFIER_TYPES];
HRESULT pri_load( const WCHAR *root, struct pri **out );
void pri_free( struct pri *pri );
const struct pri_item *pri_find( const struct pri *pri, const WCHAR *name, UINT32 length );
void pri_order( const struct pri *pri, const struct pri_item *item, const WCHAR *const *context, UINT32 *order );
BOOL pri_candidate_matches( const struct pri *pri, const struct pri_candidate *candidate, const WCHAR *const *context );

/* map.c and vector.c (copies of dlls/wintypes/map.c and dlls/windows.media.speech/vector.c) */
struct map_iids
{
    const GUID *map;
    const GUID *view;
    const GUID *iterable;
    const GUID *iterator;
    const GUID *pair;
};
HRESULT single_threaded_map_create( const struct map_iids *iids, IInspectable *outer, IInspectable **out );

struct vector_iids
{
    const GUID *iterable;
    const GUID *iterator;
    const GUID *vector;
    const GUID *view;
};
HRESULT vector_hstring_create( IVector_HSTRING **out );
HRESULT vector_inspectable_create( const struct vector_iids *iids, IVector_IInspectable **out );

#endif
