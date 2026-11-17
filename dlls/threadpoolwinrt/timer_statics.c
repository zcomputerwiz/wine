/* WinRT Windows.System.Threading.ThreadPoolTimer factory
 *
 * Copyright (C) 2026 WineExplorer contributors
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "wine/debug.h"
#include "objbase.h"

#include "activation.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_System_Threading
#include "windows.system.threading.h"

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(threadpool);

struct timer_factory
{
    IActivationFactory IActivationFactory_iface;
    IThreadPoolTimerStatics IThreadPoolTimerStatics_iface;
    LONG ref;
};

static inline struct timer_factory *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct timer_factory, IActivationFactory_iface);
}

static inline struct timer_factory *impl_from_IThreadPoolTimerStatics(IThreadPoolTimerStatics *iface)
{
    return CONTAINING_RECORD(iface, struct timer_factory, IThreadPoolTimerStatics_iface);
}

static HRESULT STDMETHODCALLTYPE timer_factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    struct timer_factory *factory = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject)
            || IsEqualIID(iid, &IID_IActivationFactory))
        *out = &factory->IActivationFactory_iface;
    else if (IsEqualIID(iid, &IID_IThreadPoolTimerStatics))
        *out = &factory->IThreadPoolTimerStatics_iface;
    else
    {
        *out = NULL;
        WARN("Unsupported interface %s.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE timer_factory_AddRef(IActivationFactory *iface)
{
    struct timer_factory *factory = impl_from_IActivationFactory(iface);
    return InterlockedIncrement(&factory->ref);
}

static ULONG STDMETHODCALLTYPE timer_factory_Release(IActivationFactory *iface)
{
    struct timer_factory *factory = impl_from_IActivationFactory(iface);
    return InterlockedDecrement(&factory->ref);
}

static HRESULT STDMETHODCALLTYPE timer_factory_GetIids(IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    IID *list;

    if (!(list = CoTaskMemAlloc(sizeof(*list)))) return E_OUTOFMEMORY;
    *list = IID_IThreadPoolTimerStatics;
    *iid_count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *class_name)
{
    return WindowsCreateString(L"Windows.System.Threading.ThreadPoolTimer", 40, class_name);
}

static HRESULT STDMETHODCALLTYPE timer_factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *trust_level)
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    TRACE("iface %p, instance %p: not activatable.\n", iface, instance);
    *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl timer_factory_vtbl =
{
    timer_factory_QueryInterface,
    timer_factory_AddRef,
    timer_factory_Release,
    /* IInspectable methods */
    timer_factory_GetIids,
    timer_factory_GetRuntimeClassName,
    timer_factory_GetTrustLevel,
    /* IActivationFactory methods */
    timer_factory_ActivateInstance,
};

static HRESULT STDMETHODCALLTYPE timer_statics_QueryInterface(IThreadPoolTimerStatics *iface, REFIID iid, void **out)
{
    return timer_factory_QueryInterface(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE timer_statics_AddRef(IThreadPoolTimerStatics *iface)
{
    return timer_factory_AddRef(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface);
}

static ULONG STDMETHODCALLTYPE timer_statics_Release(IThreadPoolTimerStatics *iface)
{
    return timer_factory_Release(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE timer_statics_GetIids(IThreadPoolTimerStatics *iface, ULONG *iid_count, IID **iids)
{
    return timer_factory_GetIids(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface, iid_count, iids);
}

static HRESULT STDMETHODCALLTYPE timer_statics_GetRuntimeClassName(IThreadPoolTimerStatics *iface, HSTRING *class_name)
{
    return timer_factory_GetRuntimeClassName(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface, class_name);
}

static HRESULT STDMETHODCALLTYPE timer_statics_GetTrustLevel(IThreadPoolTimerStatics *iface, TrustLevel *trust_level)
{
    return timer_factory_GetTrustLevel(&impl_from_IThreadPoolTimerStatics(iface)->IActivationFactory_iface, trust_level);
}

static HRESULT STDMETHODCALLTYPE timer_statics_CreatePeriodicTimer(IThreadPoolTimerStatics *iface,
        ITimerElapsedHandler *handler, TimeSpan period, IThreadPoolTimer **timer)
{
    TRACE("iface %p, handler %p, period %I64d, timer %p.\n", iface, handler, period.Duration, timer);
    return threadpool_timer_create(handler, period, TRUE, NULL, timer);
}

static HRESULT STDMETHODCALLTYPE timer_statics_CreateTimer(IThreadPoolTimerStatics *iface,
        ITimerElapsedHandler *handler, TimeSpan delay, IThreadPoolTimer **timer)
{
    TRACE("iface %p, handler %p, delay %I64d, timer %p.\n", iface, handler, delay.Duration, timer);
    return threadpool_timer_create(handler, delay, FALSE, NULL, timer);
}

static HRESULT STDMETHODCALLTYPE timer_statics_CreatePeriodicTimerWithCompletion(IThreadPoolTimerStatics *iface,
        ITimerElapsedHandler *handler, TimeSpan period, ITimerDestroyedHandler *destroyed, IThreadPoolTimer **timer)
{
    TRACE("iface %p, handler %p, period %I64d, destroyed %p, timer %p.\n", iface, handler, period.Duration, destroyed, timer);
    return threadpool_timer_create(handler, period, TRUE, destroyed, timer);
}

static HRESULT STDMETHODCALLTYPE timer_statics_CreateTimerWithCompletion(IThreadPoolTimerStatics *iface,
        ITimerElapsedHandler *handler, TimeSpan delay, ITimerDestroyedHandler *destroyed, IThreadPoolTimer **timer)
{
    TRACE("iface %p, handler %p, delay %I64d, destroyed %p, timer %p.\n", iface, handler, delay.Duration, destroyed, timer);
    return threadpool_timer_create(handler, delay, FALSE, destroyed, timer);
}

static const IThreadPoolTimerStaticsVtbl timer_statics_vtbl =
{
    timer_statics_QueryInterface,
    timer_statics_AddRef,
    timer_statics_Release,
    /* IInspectable methods */
    timer_statics_GetIids,
    timer_statics_GetRuntimeClassName,
    timer_statics_GetTrustLevel,
    /* IThreadPoolTimerStatics methods */
    timer_statics_CreatePeriodicTimer,
    timer_statics_CreateTimer,
    timer_statics_CreatePeriodicTimerWithCompletion,
    timer_statics_CreateTimerWithCompletion,
};

static struct timer_factory timer_factory =
{
    .IActivationFactory_iface.lpVtbl = &timer_factory_vtbl,
    .IThreadPoolTimerStatics_iface.lpVtbl = &timer_statics_vtbl,
    .ref = 1,
};

IActivationFactory *threadpool_timer_factory = &timer_factory.IActivationFactory_iface;
