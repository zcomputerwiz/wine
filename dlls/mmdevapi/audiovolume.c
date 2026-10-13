/*
 * Copyright 2010 Maarten Lankhorst for CodeWeavers
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
#include <math.h>
#include <assert.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"

#include "ole2.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "dsound.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"

#include "mmdevapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

typedef struct AEVImpl {
    IAudioEndpointVolumeEx IAudioEndpointVolumeEx_iface;
    LONG ref;
    GUID devguid;
} AEVImpl;

static inline AEVImpl *impl_from_IAudioEndpointVolumeEx(IAudioEndpointVolumeEx *iface)
{
    return CONTAINING_RECORD(iface, AEVImpl, IAudioEndpointVolumeEx_iface);
}

static void AudioEndpointVolume_Destroy(AEVImpl *This)
{
    free(This);
}

static HRESULT WINAPI AEV_QueryInterface(IAudioEndpointVolumeEx *iface, REFIID riid, void **ppv)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    TRACE("(%p)->(%s,%p)\n", This, debugstr_guid(riid), ppv);
    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IAudioEndpointVolume) ||
        IsEqualIID(riid, &IID_IAudioEndpointVolumeEx)) {
        *ppv = &This->IAudioEndpointVolumeEx_iface;
    }
    else
        return E_NOINTERFACE;
    IUnknown_AddRef((IUnknown *)*ppv);
    return S_OK;
}

static ULONG WINAPI AEV_AddRef(IAudioEndpointVolumeEx *iface)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI AEV_Release(IAudioEndpointVolumeEx *iface)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    if (!ref)
        AudioEndpointVolume_Destroy(This);
    return ref;
}

static HRESULT WINAPI AEV_RegisterControlChangeNotify(IAudioEndpointVolumeEx *iface, IAudioEndpointVolumeCallback *notify)
{
    TRACE("(%p)->(%p)\n", iface, notify);
    if (!notify)
        return E_POINTER;
    FIXME("stub\n");
    return S_OK;
}

static HRESULT WINAPI AEV_UnregisterControlChangeNotify(IAudioEndpointVolumeEx *iface, IAudioEndpointVolumeCallback *notify)
{
    TRACE("(%p)->(%p)\n", iface, notify);
    if (!notify)
        return E_POINTER;
    FIXME("stub\n");
    return S_OK;
}

/* The endpoint volume is the sound server's volume for the physical device behind this endpoint; the
 * driver reads/writes it (winepulse: PulseAudio/PipeWire via libpulse).  Drivers without support return
 * E_NOTIMPL and callers fall back to their own handling.  The scalar uses PulseAudio's scale, i.e. the
 * numbers pavucontrol and wpctl show; dB values follow its cubic curve. */
static HRESULT aev_driver_call( AEVImpl *This, unsigned int code, float *volume, BOOL *mute )
{
    struct endpoint_volume_params params;
    char *name;
    EDataFlow flow;
    NTSTATUS status;

    if (!get_device_name_from_guid( &This->devguid, &name, &flow )) return E_FAIL;
    params.device = name;
    params.flow = flow;
    params.volume = volume;
    params.mute = mute;
    params.result = E_NOTIMPL;
    __wine_unix_call( drvs.module_unixlib, main_loop_start, NULL );   /* idempotent; the pulse driver needs its loop */
    status = __wine_unix_call( drvs.module_unixlib, code, &params );
    free( name );
    return status ? E_NOTIMPL : params.result;
}

static float scalar_to_db( float scalar )
{
    return scalar <= 0.0f ? -100.0f : max( -100.0f, 60.0f * log10f( scalar ) );
}

static float db_to_scalar( float db )
{
    return powf( 10.0f, db / 60.0f );
}

static HRESULT WINAPI AEV_GetChannelCount(IAudioEndpointVolumeEx *iface, UINT *count)
{
    TRACE("(%p)->(%p)\n", iface, count);
    if (!count)
        return E_POINTER;
    *count = 2;
    return S_OK;
}

static HRESULT WINAPI AEV_SetMasterVolumeLevel(IAudioEndpointVolumeEx *iface, float leveldb, const GUID *ctx)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    float scalar;

    TRACE("(%p)->(%f,%s)\n", iface, leveldb, debugstr_guid(ctx));

    if (leveldb < -100.f || leveldb > 0.f)
        return E_INVALIDARG;

    scalar = db_to_scalar( leveldb );
    return aev_driver_call( This, set_endpoint_volume, &scalar, NULL );
}

static HRESULT WINAPI AEV_SetMasterVolumeLevelScalar(IAudioEndpointVolumeEx *iface, float level, const GUID *ctx)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);

    TRACE("(%p)->(%f,%s)\n", iface, level, debugstr_guid(ctx));

    if (level < 0.0f || level > 1.0f)
        return E_INVALIDARG;
    return aev_driver_call( This, set_endpoint_volume, &level, NULL );
}

static HRESULT WINAPI AEV_GetMasterVolumeLevel(IAudioEndpointVolumeEx *iface, float *leveldb)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    float scalar;
    HRESULT hr;

    TRACE("(%p)->(%p)\n", iface, leveldb);

    if (!leveldb)
        return E_POINTER;
    if (SUCCEEDED(hr = aev_driver_call( This, get_endpoint_volume, &scalar, NULL )))
        *leveldb = scalar_to_db( scalar );
    return hr;
}

static HRESULT WINAPI AEV_GetMasterVolumeLevelScalar(IAudioEndpointVolumeEx *iface, float *level)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);

    TRACE("(%p)->(%p)\n", iface, level);
    if (!level)
        return E_POINTER;
    return aev_driver_call( This, get_endpoint_volume, level, NULL );
}

static HRESULT WINAPI AEV_SetChannelVolumeLevel(IAudioEndpointVolumeEx *iface, UINT chan, float leveldb, const GUID *ctx)
{
    TRACE("(%p)->(%f,%s)\n", iface, leveldb, debugstr_guid(ctx));
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AEV_SetChannelVolumeLevelScalar(IAudioEndpointVolumeEx *iface, UINT chan, float level, const GUID *ctx)
{
    TRACE("(%p)->(%u,%f,%s)\n", iface, chan, level, debugstr_guid(ctx));
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AEV_GetChannelVolumeLevel(IAudioEndpointVolumeEx *iface, UINT chan, float *leveldb)
{
    TRACE("(%p)->(%u,%p)\n", iface, chan, leveldb);
    if (!leveldb)
        return E_POINTER;
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AEV_GetChannelVolumeLevelScalar(IAudioEndpointVolumeEx *iface, UINT chan, float *level)
{
    TRACE("(%p)->(%u,%p)\n", iface, chan, level);
    if (!level)
        return E_POINTER;
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AEV_SetMute(IAudioEndpointVolumeEx *iface, BOOL mute, const GUID *ctx)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    BOOL current;
    HRESULT hr;

    TRACE("(%p)->(%u,%s)\n", iface, mute, debugstr_guid(ctx));

    if (FAILED(hr = aev_driver_call( This, get_endpoint_volume, NULL, &current ))) return hr;
    if (!current == !mute) return S_FALSE;
    return aev_driver_call( This, set_endpoint_volume, NULL, &mute );
}

static HRESULT WINAPI AEV_GetMute(IAudioEndpointVolumeEx *iface, BOOL *mute)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);

    TRACE("(%p)->(%p)\n", iface, mute);

    if (!mute)
        return E_POINTER;
    return aev_driver_call( This, get_endpoint_volume, NULL, mute );
}

/* 101 steps of 1% on the scalar */
static HRESULT WINAPI AEV_GetVolumeStepInfo(IAudioEndpointVolumeEx *iface, UINT *stepsize, UINT *stepcount)
{
    AEVImpl *This = impl_from_IAudioEndpointVolumeEx(iface);
    float scalar;
    HRESULT hr;

    TRACE("(%p)->(%p,%p)\n", iface, stepsize, stepcount);
    if (!stepsize && !stepcount)
        return E_POINTER;
    if (FAILED(hr = aev_driver_call( This, get_endpoint_volume, &scalar, NULL ))) return hr;
    if (stepsize) *stepsize = (UINT)(scalar * 100.0f + 0.5f);   /* current step */
    if (stepcount) *stepcount = 101;
    return S_OK;
}

static HRESULT aev_step( AEVImpl *This, float delta )
{
    float scalar;
    HRESULT hr;

    if (FAILED(hr = aev_driver_call( This, get_endpoint_volume, &scalar, NULL ))) return hr;
    scalar = min( 1.0f, max( 0.0f, floorf( scalar * 100.0f + 0.5f ) / 100.0f + delta ) );
    return aev_driver_call( This, set_endpoint_volume, &scalar, NULL );
}

static HRESULT WINAPI AEV_VolumeStepUp(IAudioEndpointVolumeEx *iface, const GUID *ctx)
{
    TRACE("(%p)->(%s)\n", iface, debugstr_guid(ctx));
    return aev_step( impl_from_IAudioEndpointVolumeEx(iface), 0.01f );
}

static HRESULT WINAPI AEV_VolumeStepDown(IAudioEndpointVolumeEx *iface, const GUID *ctx)
{
    TRACE("(%p)->(%s)\n", iface, debugstr_guid(ctx));
    return aev_step( impl_from_IAudioEndpointVolumeEx(iface), -0.01f );
}

static HRESULT WINAPI AEV_QueryHardwareSupport(IAudioEndpointVolumeEx *iface, DWORD *mask)
{
    TRACE("(%p)->(%p)\n", iface, mask);
    if (!mask)
        return E_POINTER;
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI AEV_GetVolumeRange(IAudioEndpointVolumeEx *iface, float *mindb, float *maxdb, float *inc)
{
    TRACE("(%p)->(%p,%p,%p)\n", iface, mindb, maxdb, inc);

    if (!mindb || !maxdb || !inc)
        return E_POINTER;

    *mindb = -100.f;
    *maxdb = 0.f;
    *inc = 1.f;

    return S_OK;
}

static HRESULT WINAPI AEV_GetVolumeRangeChannel(IAudioEndpointVolumeEx *iface, UINT chan, float *mindb, float *maxdb, float *inc)
{
    TRACE("(%p)->(%p,%p,%p)\n", iface, mindb, maxdb, inc);
    if (!mindb || !maxdb || !inc)
        return E_POINTER;
    FIXME("stub\n");
    return E_NOTIMPL;
}

static const IAudioEndpointVolumeExVtbl AEVImpl_Vtbl = {
    AEV_QueryInterface,
    AEV_AddRef,
    AEV_Release,
    AEV_RegisterControlChangeNotify,
    AEV_UnregisterControlChangeNotify,
    AEV_GetChannelCount,
    AEV_SetMasterVolumeLevel,
    AEV_SetMasterVolumeLevelScalar,
    AEV_GetMasterVolumeLevel,
    AEV_GetMasterVolumeLevelScalar,
    AEV_SetChannelVolumeLevel,
    AEV_SetChannelVolumeLevelScalar,
    AEV_GetChannelVolumeLevel,
    AEV_GetChannelVolumeLevelScalar,
    AEV_SetMute,
    AEV_GetMute,
    AEV_GetVolumeStepInfo,
    AEV_VolumeStepUp,
    AEV_VolumeStepDown,
    AEV_QueryHardwareSupport,
    AEV_GetVolumeRange,
    AEV_GetVolumeRangeChannel
};

HRESULT AudioEndpointVolume_Create(MMDevice *parent, IAudioEndpointVolumeEx **ppv)
{
    AEVImpl *This;

    *ppv = NULL;
    This = calloc(1, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->IAudioEndpointVolumeEx_iface.lpVtbl = &AEVImpl_Vtbl;
    This->ref = 1;
    This->devguid = parent->devguid;

    *ppv = &This->IAudioEndpointVolumeEx_iface;
    return S_OK;
}
