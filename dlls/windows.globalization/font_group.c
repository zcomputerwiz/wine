/* WinRT Windows.Globalization.Fonts.LanguageFontGroup implementation
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

#include "private.h"

#include "initguid.h"
#define WIDL_using_Windows_Globalization_Fonts
#define WIDL_using_Windows_UI_Text
#include "windows.globalization.fonts.h"

WINE_DEFAULT_DEBUG_CHANNEL(locale);

/* The recommended fonts of a language, as Windows 11 (64-bit) gives them, by script. Stretch and style are Normal
 * throughout; a font without a family is null (Windows has none for that use and script). */
struct font_entry
{
    const WCHAR *family;
    UINT16 weight;
    double scale;
};

enum font_use
{
    UI_TEXT, UI_HEADING, UI_TITLE, UI_CAPTION, UI_NOTIFICATION_HEADING, TRADITIONAL_DOCUMENT, MODERN_DOCUMENT,
    DOCUMENT_HEADING, FIXED_WIDTH_TEXT, DOCUMENT_ALTERNATE1, DOCUMENT_ALTERNATE2, FONT_USE_COUNT
};

struct font_set
{
    const WCHAR *languages;   /* primary language subtags, each followed by a space; NULL for the default */
    struct font_entry fonts[FONT_USE_COUNT];
};

#define SEGOE_UI_FONTS {L"Segoe UI", 350, 100}, {L"Segoe UI", 300, 100}, {L"Segoe UI", 300, 100}, \
                       {L"Segoe UI", 400, 100}, {L"Segoe UI", 600, 100}

static const struct font_set default_fonts =
{
    NULL,
    {SEGOE_UI_FONTS, {L"Cambria", 400, 100}, {L"Calibri", 400, 100}, {L"Cambria", 400, 100}, {L"Consolas", 400, 100},
     {L"Verdana", 400, 100}, {L"Georgia", 400, 100}}
};

static const struct font_set chinese_traditional_fonts =
{
    NULL,
    {{L"Microsoft JhengHei UI", 400, 100}, {L"Microsoft JhengHei UI", 400, 100}, {L"Microsoft JhengHei UI", 400, 93},
     {L"Microsoft JhengHei UI", 400, 100}, {L"Microsoft JhengHei UI", 700, 100}, {L"MingLiU", 400, 100},
     {L"Microsoft JhengHei", 400, 90}, {L"Microsoft JhengHei", 400, 90}, {L"MingLiU", 400, 110},
     {L"DFKai-SB", 400, 100}, {NULL}}
};

static const struct font_set script_fonts[] =
{
    {L"ja ", {{L"Meiryo UI", 400, 95}, {L"Meiryo UI", 400, 95}, {L"Meiryo UI", 400, 95}, {L"Meiryo UI", 400, 95},
              {L"Meiryo UI", 700, 95}, {L"MS Mincho", 400, 100}, {L"Meiryo", 400, 90}, {L"Meiryo", 400, 90},
              {L"MS Mincho", 400, 110}, {L"MS Gothic", 400, 100}, {NULL}}},
    {L"ko ", {{L"Malgun Gothic", 400, 96}, {L"Malgun Gothic", 400, 96}, {L"Malgun Gothic", 400, 96},
              {L"Malgun Gothic", 400, 96}, {L"Malgun Gothic", 700, 96}, {L"Batang", 400, 100},
              {L"Malgun Gothic", 400, 90}, {L"Batang", 400, 100}, {L"BatangChe", 400, 110}, {L"Dotum", 400, 100},
              {L"Gungsuh", 400, 100}}},
    {L"zh ", {{L"Microsoft YaHei UI", 400, 90}, {L"Microsoft YaHei UI", 400, 90}, {L"Microsoft YaHei UI", 400, 90},
              {L"Microsoft YaHei UI", 400, 90}, {L"Microsoft YaHei UI", 700, 90}, {L"SimSun", 400, 100},
              {L"Microsoft YaHei", 400, 90}, {L"Microsoft YaHei", 400, 90}, {L"NSimsun", 400, 110},
              {L"KaiTi", 400, 100}, {L"FangSong", 400, 100}}},
    {L"ar ", {SEGOE_UI_FONTS, {L"Sakkal Majalla", 400, 127}, {L"Arial", 400, 90}, {L"Sakkal Majalla", 400, 127},
              {L"Courier New", 400, 92}, {L"Arabic Typesetting", 400, 100}, {L"Traditional Arabic", 400, 100}}},
    {L"he ", {SEGOE_UI_FONTS, {L"Times New Roman", 400, 100}, {L"Arial", 400, 90}, {L"Times New Roman", 400, 100},
              {L"Courier New", 400, 92}, {L"Segoe UI", 400, 100}, {L"David", 400, 100}}},
    {L"th ", {{L"Leelawadee", 400, 109}, {L"Leelawadee", 400, 109}, {L"Leelawadee", 400, 100},
              {L"Leelawadee", 400, 109}, {L"Leelawadee", 700, 109}, {L"Angsana New", 400, 145},
              {L"Browallia New", 400, 141}, {L"Angsana New", 400, 145}, {NULL}, {L"Cordia New", 400, 100},
              {L"Leelawadee", 400, 100}}},
    {L"hi ", {{L"Nirmala UI", 400, 100}, {L"Nirmala UI", 400, 100}, {L"Nirmala UI", 400, 100},
              {L"Nirmala UI", 400, 100}, {L"Nirmala UI", 700, 100}, {L"Kokila", 400, 124}, {L"Utsaah", 400, 122},
              {L"Aparajita", 400, 125}, {NULL}, {L"Nirmala UI", 400, 100}, {L"Mangal", 400, 100}}},
    {L"ka ", {SEGOE_UI_FONTS, {L"Sylfaen", 400, 100}, {L"Segoe UI", 400, 90}, {L"Sylfaen", 400, 100}, {NULL}, {NULL},
              {NULL}}},
    {L"am ", {{L"Ebrima", 400, 100}, {L"Ebrima", 400, 100}, {L"Ebrima", 400, 100}, {L"Ebrima", 400, 100},
              {L"Ebrima", 700, 100}, {L"Nyala", 400, 103}, {L"Ebrima", 400, 90}, {L"Nyala", 400, 103}, {NULL},
              {NULL}, {NULL}}},
};

/* the fonts of a language tag, or NULL when it names no language Windows knows */
static const struct font_set *font_set_for_tag( const WCHAR *tag )
{
    WCHAR primary[9], lower[LOCALE_NAME_MAX_LENGTH];
    unsigned int i, len;

    if (!tag[0] || wcslen( tag ) >= ARRAY_SIZE(lower)) return NULL;
    if (!LocaleNameToLCID( tag, LOCALE_ALLOW_NEUTRAL_NAMES )) return NULL;

    for (i = 0; tag[i]; i++) lower[i] = towlower( tag[i] );
    lower[i] = 0;
    for (len = 0; lower[len] && lower[len] != '-' && lower[len] != '_' && len < ARRAY_SIZE(primary) - 2; len++)
        primary[len] = lower[len];
    primary[len++] = ' ';
    primary[len] = 0;

    if (!wcscmp( primary, L"zh " ) && (wcsstr( lower, L"-hant" ) || wcsstr( lower, L"-tw" ) || wcsstr( lower, L"-hk" ) ||
                                       wcsstr( lower, L"-mo" )))
        return &chinese_traditional_fonts;
    for (i = 0; i < ARRAY_SIZE(script_fonts); i++)
        if (wcsstr( script_fonts[i].languages, primary )) return &script_fonts[i];
    return &default_fonts;
}

struct language_font
{
    ILanguageFont ILanguageFont_iface;
    const struct font_entry *entry;
    LONG ref;
};

static inline struct language_font *impl_from_ILanguageFont( ILanguageFont *iface )
{
    return CONTAINING_RECORD( iface, struct language_font, ILanguageFont_iface );
}

static HRESULT WINAPI language_font_QueryInterface( ILanguageFont *iface, REFIID iid, void **out )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILanguageFont ))
    {
        *out = &impl->ILanguageFont_iface;
        ILanguageFont_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI language_font_AddRef( ILanguageFont *iface )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI language_font_Release( ILanguageFont *iface )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI language_font_GetIids( ILanguageFont *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI language_font_GetRuntimeClassName( ILanguageFont *iface, HSTRING *class_name )
{
    const WCHAR *name = RuntimeClass_Windows_Globalization_Fonts_LanguageFont;
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI language_font_GetTrustLevel( ILanguageFont *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI language_font_get_FontFamily( ILanguageFont *iface, HSTRING *value )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( impl->entry->family, wcslen( impl->entry->family ), value );
}

static HRESULT WINAPI language_font_get_FontWeight( ILanguageFont *iface, FontWeight *weight )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );
    TRACE( "iface %p, weight %p.\n", iface, weight );
    if (!weight) return E_POINTER;
    weight->Weight = impl->entry->weight;
    return S_OK;
}

static HRESULT WINAPI language_font_get_FontStretch( ILanguageFont *iface, FontStretch *stretch )
{
    TRACE( "iface %p, stretch %p.\n", iface, stretch );
    if (!stretch) return E_POINTER;
    *stretch = FontStretch_Normal;
    return S_OK;
}

static HRESULT WINAPI language_font_get_FontStyle( ILanguageFont *iface, FontStyle *style )
{
    TRACE( "iface %p, style %p.\n", iface, style );
    if (!style) return E_POINTER;
    *style = FontStyle_Normal;
    return S_OK;
}

static HRESULT WINAPI language_font_get_ScaleFactor( ILanguageFont *iface, DOUBLE *scale )
{
    struct language_font *impl = impl_from_ILanguageFont( iface );
    TRACE( "iface %p, scale %p.\n", iface, scale );
    if (!scale) return E_POINTER;
    *scale = impl->entry->scale;
    return S_OK;
}

static const ILanguageFontVtbl language_font_vtbl =
{
    language_font_QueryInterface,
    language_font_AddRef,
    language_font_Release,
    /* IInspectable methods */
    language_font_GetIids,
    language_font_GetRuntimeClassName,
    language_font_GetTrustLevel,
    /* ILanguageFont methods */
    language_font_get_FontFamily,
    language_font_get_FontWeight,
    language_font_get_FontStretch,
    language_font_get_FontStyle,
    language_font_get_ScaleFactor,
};

struct language_font_group
{
    ILanguageFontGroup ILanguageFontGroup_iface;
    const struct font_set *set;
    LONG ref;
};

static inline struct language_font_group *impl_from_ILanguageFontGroup( ILanguageFontGroup *iface )
{
    return CONTAINING_RECORD( iface, struct language_font_group, ILanguageFontGroup_iface );
}

static HRESULT WINAPI group_QueryInterface( ILanguageFontGroup *iface, REFIID iid, void **out )
{
    struct language_font_group *impl = impl_from_ILanguageFontGroup( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_ILanguageFontGroup ))
    {
        *out = &impl->ILanguageFontGroup_iface;
        ILanguageFontGroup_AddRef( iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI group_AddRef( ILanguageFontGroup *iface )
{
    struct language_font_group *impl = impl_from_ILanguageFontGroup( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI group_Release( ILanguageFontGroup *iface )
{
    struct language_font_group *impl = impl_from_ILanguageFontGroup( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI group_GetIids( ILanguageFontGroup *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI group_GetRuntimeClassName( ILanguageFontGroup *iface, HSTRING *class_name )
{
    const WCHAR *name = RuntimeClass_Windows_Globalization_Fonts_LanguageFontGroup;
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI group_GetTrustLevel( ILanguageFontGroup *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

/* a new LanguageFont for one use, or NULL (with S_OK) when the script has no font for it */
static HRESULT group_font( ILanguageFontGroup *iface, enum font_use use, ILanguageFont **value )
{
    struct language_font_group *impl = impl_from_ILanguageFontGroup( iface );
    const struct font_entry *entry = &impl->set->fonts[use];
    struct language_font *font;

    if (!value) return E_POINTER;
    *value = NULL;
    if (!entry->family) return S_OK;
    if (!(font = calloc( 1, sizeof(*font) ))) return E_OUTOFMEMORY;
    font->ILanguageFont_iface.lpVtbl = &language_font_vtbl;
    font->entry = entry;
    font->ref = 1;
    *value = &font->ILanguageFont_iface;
    return S_OK;
}

#define GROUP_FONT_GETTER( name, use )                                                       \
    static HRESULT WINAPI group_get_##name( ILanguageFontGroup *iface, ILanguageFont **value ) \
    {                                                                                       \
        TRACE( "iface %p, value %p.\n", iface, value );                                     \
        return group_font( iface, use, value );                                             \
    }

GROUP_FONT_GETTER( UITextFont, UI_TEXT )
GROUP_FONT_GETTER( UIHeadingFont, UI_HEADING )
GROUP_FONT_GETTER( UITitleFont, UI_TITLE )
GROUP_FONT_GETTER( UICaptionFont, UI_CAPTION )
GROUP_FONT_GETTER( UINotificationHeadingFont, UI_NOTIFICATION_HEADING )
GROUP_FONT_GETTER( TraditionalDocumentFont, TRADITIONAL_DOCUMENT )
GROUP_FONT_GETTER( ModernDocumentFont, MODERN_DOCUMENT )
GROUP_FONT_GETTER( DocumentHeadingFont, DOCUMENT_HEADING )
GROUP_FONT_GETTER( FixedWidthTextFont, FIXED_WIDTH_TEXT )
GROUP_FONT_GETTER( DocumentAlternate1Font, DOCUMENT_ALTERNATE1 )
GROUP_FONT_GETTER( DocumentAlternate2Font, DOCUMENT_ALTERNATE2 )

static const ILanguageFontGroupVtbl group_vtbl =
{
    group_QueryInterface,
    group_AddRef,
    group_Release,
    /* IInspectable methods */
    group_GetIids,
    group_GetRuntimeClassName,
    group_GetTrustLevel,
    /* ILanguageFontGroup methods */
    group_get_UITextFont,
    group_get_UIHeadingFont,
    group_get_UITitleFont,
    group_get_UICaptionFont,
    group_get_UINotificationHeadingFont,
    group_get_TraditionalDocumentFont,
    group_get_ModernDocumentFont,
    group_get_DocumentHeadingFont,
    group_get_FixedWidthTextFont,
    group_get_DocumentAlternate1Font,
    group_get_DocumentAlternate2Font,
};

struct font_group_factory
{
    IActivationFactory IActivationFactory_iface;
    ILanguageFontGroupFactory ILanguageFontGroupFactory_iface;
    LONG ref;
};

static inline struct font_group_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct font_group_factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct font_group_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_ILanguageFontGroupFactory ))
        *out = &impl->ILanguageFontGroupFactory_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct font_group_factory *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct font_group_factory *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

/* as on Windows: the class has no default constructor */
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( group_factory, ILanguageFontGroupFactory, struct font_group_factory, IActivationFactory_iface )

static HRESULT WINAPI group_factory_CreateLanguageFontGroup( ILanguageFontGroupFactory *iface, HSTRING language_tag,
                                                             ILanguageFontGroup **value )
{
    struct language_font_group *group;
    const struct font_set *set;

    TRACE( "iface %p, language_tag %s, value %p.\n", iface, debugstr_hstring( language_tag ), value );

    if (!value) return E_POINTER;
    *value = NULL;
    if (!(set = font_set_for_tag( WindowsGetStringRawBuffer( language_tag, NULL ) )))
        return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    if (!(group = calloc( 1, sizeof(*group) ))) return E_OUTOFMEMORY;
    group->ILanguageFontGroup_iface.lpVtbl = &group_vtbl;
    group->set = set;
    group->ref = 1;
    *value = &group->ILanguageFontGroup_iface;
    return S_OK;
}

static const ILanguageFontGroupFactoryVtbl group_factory_vtbl =
{
    group_factory_QueryInterface,
    group_factory_AddRef,
    group_factory_Release,
    /* IInspectable methods */
    group_factory_GetIids,
    group_factory_GetRuntimeClassName,
    group_factory_GetTrustLevel,
    /* ILanguageFontGroupFactory methods */
    group_factory_CreateLanguageFontGroup,
};

static struct font_group_factory font_group_factory =
{
    {&factory_vtbl},
    {&group_factory_vtbl},
    1,
};

IActivationFactory *language_font_group_factory = &font_group_factory.IActivationFactory_iface;
