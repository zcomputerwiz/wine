/* WinRT Windows.UI.Colors implementation
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

WINE_DEFAULT_DEBUG_CHANNEL(xaml);

/* the named colours, ARGB, in the order of IColorsStatics */
#define NAMED_COLORS \
    X(AliceBlue, 0xfff0f8ff) X(AntiqueWhite, 0xfffaebd7) X(Aqua, 0xff00ffff) X(Aquamarine, 0xff7fffd4) \
    X(Azure, 0xfff0ffff) X(Beige, 0xfff5f5dc) X(Bisque, 0xffffe4c4) X(Black, 0xff000000) \
    X(BlanchedAlmond, 0xffffebcd) X(Blue, 0xff0000ff) X(BlueViolet, 0xff8a2be2) X(Brown, 0xffa52a2a) \
    X(BurlyWood, 0xffdeb887) X(CadetBlue, 0xff5f9ea0) X(Chartreuse, 0xff7fff00) X(Chocolate, 0xffd2691e) \
    X(Coral, 0xffff7f50) X(CornflowerBlue, 0xff6495ed) X(Cornsilk, 0xfffff8dc) X(Crimson, 0xffdc143c) \
    X(Cyan, 0xff00ffff) X(DarkBlue, 0xff00008b) X(DarkCyan, 0xff008b8b) X(DarkGoldenrod, 0xffb8860b) \
    X(DarkGray, 0xffa9a9a9) X(DarkGreen, 0xff006400) X(DarkKhaki, 0xffbdb76b) X(DarkMagenta, 0xff8b008b) \
    X(DarkOliveGreen, 0xff556b2f) X(DarkOrange, 0xffff8c00) X(DarkOrchid, 0xff9932cc) X(DarkRed, 0xff8b0000) \
    X(DarkSalmon, 0xffe9967a) X(DarkSeaGreen, 0xff8fbc8f) X(DarkSlateBlue, 0xff483d8b) X(DarkSlateGray, 0xff2f4f4f) \
    X(DarkTurquoise, 0xff00ced1) X(DarkViolet, 0xff9400d3) X(DeepPink, 0xffff1493) X(DeepSkyBlue, 0xff00bfff) \
    X(DimGray, 0xff696969) X(DodgerBlue, 0xff1e90ff) X(Firebrick, 0xffb22222) X(FloralWhite, 0xfffffaf0) \
    X(ForestGreen, 0xff228b22) X(Fuchsia, 0xffff00ff) X(Gainsboro, 0xffdcdcdc) X(GhostWhite, 0xfff8f8ff) \
    X(Gold, 0xffffd700) X(Goldenrod, 0xffdaa520) X(Gray, 0xff808080) X(Green, 0xff008000) \
    X(GreenYellow, 0xffadff2f) X(Honeydew, 0xfff0fff0) X(HotPink, 0xffff69b4) X(IndianRed, 0xffcd5c5c) \
    X(Indigo, 0xff4b0082) X(Ivory, 0xfffffff0) X(Khaki, 0xfff0e68c) X(Lavender, 0xffe6e6fa) \
    X(LavenderBlush, 0xfffff0f5) X(LawnGreen, 0xff7cfc00) X(LemonChiffon, 0xfffffacd) X(LightBlue, 0xffadd8e6) \
    X(LightCoral, 0xfff08080) X(LightCyan, 0xffe0ffff) X(LightGoldenrodYellow, 0xfffafad2) X(LightGreen, 0xff90ee90) \
    X(LightGray, 0xffd3d3d3) X(LightPink, 0xffffb6c1) X(LightSalmon, 0xffffa07a) X(LightSeaGreen, 0xff20b2aa) \
    X(LightSkyBlue, 0xff87cefa) X(LightSlateGray, 0xff778899) X(LightSteelBlue, 0xffb0c4de) X(LightYellow, 0xffffffe0) \
    X(Lime, 0xff00ff00) X(LimeGreen, 0xff32cd32) X(Linen, 0xfffaf0e6) X(Magenta, 0xffff00ff) \
    X(Maroon, 0xff800000) X(MediumAquamarine, 0xff66cdaa) X(MediumBlue, 0xff0000cd) X(MediumOrchid, 0xffba55d3) \
    X(MediumPurple, 0xff9370db) X(MediumSeaGreen, 0xff3cb371) X(MediumSlateBlue, 0xff7b68ee) \
    X(MediumSpringGreen, 0xff00fa9a) X(MediumTurquoise, 0xff48d1cc) X(MediumVioletRed, 0xffc71585) \
    X(MidnightBlue, 0xff191970) X(MintCream, 0xfff5fffa) X(MistyRose, 0xffffe4e1) X(Moccasin, 0xffffe4b5) \
    X(NavajoWhite, 0xffffdead) X(Navy, 0xff000080) X(OldLace, 0xfffdf5e6) X(Olive, 0xff808000) \
    X(OliveDrab, 0xff6b8e23) X(Orange, 0xffffa500) X(OrangeRed, 0xffff4500) X(Orchid, 0xffda70d6) \
    X(PaleGoldenrod, 0xffeee8aa) X(PaleGreen, 0xff98fb98) X(PaleTurquoise, 0xffafeeee) X(PaleVioletRed, 0xffdb7093) \
    X(PapayaWhip, 0xffffefd5) X(PeachPuff, 0xffffdab9) X(Peru, 0xffcd853f) X(Pink, 0xffffc0cb) \
    X(Plum, 0xffdda0dd) X(PowderBlue, 0xffb0e0e6) X(Purple, 0xff800080) X(Red, 0xffff0000) \
    X(RosyBrown, 0xffbc8f8f) X(RoyalBlue, 0xff4169e1) X(SaddleBrown, 0xff8b4513) X(Salmon, 0xfffa8072) \
    X(SandyBrown, 0xfff4a460) X(SeaGreen, 0xff2e8b57) X(SeaShell, 0xfffff5ee) X(Sienna, 0xffa0522d) \
    X(Silver, 0xffc0c0c0) X(SkyBlue, 0xff87ceeb) X(SlateBlue, 0xff6a5acd) X(SlateGray, 0xff708090) \
    X(Snow, 0xfffffafa) X(SpringGreen, 0xff00ff7f) X(SteelBlue, 0xff4682b4) X(Tan, 0xffd2b48c) \
    X(Teal, 0xff008080) X(Thistle, 0xffd8bfd8) X(Tomato, 0xffff6347) X(Transparent, 0x00ffffff) \
    X(Turquoise, 0xff40e0d0) X(Violet, 0xffee82ee) X(Wheat, 0xfff5deb3) X(White, 0xffffffff) \
    X(WhiteSmoke, 0xfff5f5f5) X(Yellow, 0xffffff00) X(YellowGreen, 0xff9acd32)

struct colors_statics
{
    IActivationFactory IActivationFactory_iface;
    IColorsStatics IColorsStatics_iface;
    LONG ref;
};

static inline struct colors_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct colors_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct colors_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IColorsStatics ))
    {
        *out = &impl->IColorsStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return InterlockedIncrement( &impl_from_IActivationFactory( iface )->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return InterlockedDecrement( &impl_from_IActivationFactory( iface )->ref );
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

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
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

DEFINE_IINSPECTABLE( colors_statics, IColorsStatics, struct colors_statics, IActivationFactory_iface )

static HRESULT named_color( UINT32 argb, Color *value )
{
    if (!value) return E_INVALIDARG;
    value->A = argb >> 24;
    value->R = argb >> 16;
    value->G = argb >> 8;
    value->B = argb;
    return S_OK;
}

#define X( name, argb ) \
    static HRESULT WINAPI colors_statics_get_##name( IColorsStatics *iface, Color *value ) \
    {                                                                                      \
        TRACE( "iface %p, value %p.\n", iface, value );                                    \
        return named_color( argb, value );                                                 \
    }
NAMED_COLORS
#undef X

static const struct IColorsStaticsVtbl colors_statics_vtbl =
{
    colors_statics_QueryInterface,
    colors_statics_AddRef,
    colors_statics_Release,
    /* IInspectable methods */
    colors_statics_GetIids,
    colors_statics_GetRuntimeClassName,
    colors_statics_GetTrustLevel,
    /* IColorsStatics methods */
#define X( name, argb ) colors_statics_get_##name,
    NAMED_COLORS
#undef X
};

static struct colors_statics colors_statics =
{
    {&factory_vtbl},
    {&colors_statics_vtbl},
    1,
};

IActivationFactory *colors_factory = &colors_statics.IActivationFactory_iface;
