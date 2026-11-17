/* WinRT Windows.System.Threading.ThreadPoolTimer Implementation
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

/* A timer of the system thread pool. The pool keeps the timer alive while it can fire: a periodic timer until Cancel,
 * a one-shot one until it has fired, so an application may drop its reference. */
struct timer
{
    IThreadPoolTimer IThreadPoolTimer_iface;
    LONG ref;
    LONG pool_ref;          /* 1 while the pool holds a reference */
    PTP_TIMER tp;
    ITimerElapsedHandler *handler;
    ITimerDestroyedHandler *destroyed;
    TimeSpan period;        /* zero for a one-shot timer */
    TimeSpan delay;
    LONG cancelled;
};

static inline struct timer *impl_from_IThreadPoolTimer(IThreadPoolTimer *iface)
{
    return CONTAINING_RECORD(iface, struct timer, IThreadPoolTimer_iface);
}

static HRESULT STDMETHODCALLTYPE timer_QueryInterface(IThreadPoolTimer *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IInspectable) || IsEqualIID(iid, &IID_IAgileObject)
            || IsEqualIID(iid, &IID_IThreadPoolTimer))
    {
        *out = iface;
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    *out = NULL;
    WARN("Unsupported interface %s.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE timer_AddRef(IThreadPoolTimer *iface)
{
    struct timer *timer = impl_from_IThreadPoolTimer(iface);
    ULONG ref = InterlockedIncrement(&timer->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE timer_Release(IThreadPoolTimer *iface)
{
    struct timer *timer = impl_from_IThreadPoolTimer(iface);
    ULONG ref = InterlockedDecrement(&timer->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (timer->tp)
        {
            SetThreadpoolTimer(timer->tp, NULL, 0, 0);
            WaitForThreadpoolTimerCallbacks(timer->tp, TRUE);
            CloseThreadpoolTimer(timer->tp);
        }
        if (timer->handler) ITimerElapsedHandler_Release(timer->handler);
        if (timer->destroyed) ITimerDestroyedHandler_Release(timer->destroyed);
        free(timer);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE timer_GetIids(IThreadPoolTimer *iface, ULONG *iid_count, IID **iids)
{
    IID *list;

    TRACE("iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids);

    if (!(list = CoTaskMemAlloc(sizeof(*list)))) return E_OUTOFMEMORY;
    *list = IID_IThreadPoolTimer;
    *iid_count = 1;
    *iids = list;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_GetRuntimeClassName(IThreadPoolTimer *iface, HSTRING *class_name)
{
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    return WindowsCreateString(L"Windows.System.Threading.ThreadPoolTimer", 40, class_name);
}

static HRESULT STDMETHODCALLTYPE timer_GetTrustLevel(IThreadPoolTimer *iface, TrustLevel *trust_level)
{
    TRACE("iface %p, trust_level %p.\n", iface, trust_level);
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_get_Period(IThreadPoolTimer *iface, TimeSpan *value)
{
    struct timer *timer = impl_from_IThreadPoolTimer(iface);

    TRACE("iface %p, value %p.\n", iface, value);
    *value = timer->period;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE timer_get_Delay(IThreadPoolTimer *iface, TimeSpan *value)
{
    struct timer *timer = impl_from_IThreadPoolTimer(iface);

    TRACE("iface %p, value %p.\n", iface, value);
    *value = timer->delay;
    return S_OK;
}

/* the pool no longer fires the timer: the first call stops it, tells the destroyed handler and drops the pool's reference */
static void timer_stop(struct timer *timer, BOOL wait)
{
    if (InterlockedExchange(&timer->cancelled, 1)) return;
    SetThreadpoolTimer(timer->tp, NULL, 0, 0);
    if (wait) WaitForThreadpoolTimerCallbacks(timer->tp, TRUE);   /* never from the callback itself */
    if (timer->destroyed) ITimerDestroyedHandler_Invoke(timer->destroyed, &timer->IThreadPoolTimer_iface);
    if (InterlockedExchange(&timer->pool_ref, 0)) IThreadPoolTimer_Release(&timer->IThreadPoolTimer_iface);
}

static HRESULT STDMETHODCALLTYPE timer_Cancel(IThreadPoolTimer *iface)
{
    struct timer *timer = impl_from_IThreadPoolTimer(iface);

    TRACE("iface %p.\n", iface);

    timer_stop(timer, FALSE);
    return S_OK;
}

static const IThreadPoolTimerVtbl timer_vtbl =
{
    timer_QueryInterface,
    timer_AddRef,
    timer_Release,
    /* IInspectable methods */
    timer_GetIids,
    timer_GetRuntimeClassName,
    timer_GetTrustLevel,
    /* IThreadPoolTimer methods */
    timer_get_Period,
    timer_get_Delay,
    timer_Cancel,
};

static void CALLBACK timer_callback(TP_CALLBACK_INSTANCE *instance, void *context, TP_TIMER *tp_timer)
{
    struct timer *timer = context;

    if (timer->cancelled) return;
    IThreadPoolTimer_AddRef(&timer->IThreadPoolTimer_iface);
    ITimerElapsedHandler_Invoke(timer->handler, &timer->IThreadPoolTimer_iface);
    /* a one-shot timer is done once it has fired */
    if (!timer->period.Duration) timer_stop(timer, FALSE);
    IThreadPoolTimer_Release(&timer->IThreadPoolTimer_iface);
}

HRESULT threadpool_timer_create(ITimerElapsedHandler *handler, TimeSpan span, BOOL periodic,
        ITimerDestroyedHandler *destroyed, IThreadPoolTimer **ret)
{
    struct timer *timer;
    FILETIME due;
    DWORD period_ms = 0;
    LONGLONG ticks;

    *ret = NULL;
    if (!handler) return E_INVALIDARG;
    if (span.Duration < 0 || (periodic && !span.Duration)) return E_INVALIDARG;

    if (!(timer = calloc(1, sizeof(*timer)))) return E_OUTOFMEMORY;
    timer->IThreadPoolTimer_iface.lpVtbl = &timer_vtbl;
    timer->ref = 1;
    timer->handler = handler;
    ITimerElapsedHandler_AddRef(handler);
    if ((timer->destroyed = destroyed)) ITimerDestroyedHandler_AddRef(destroyed);
    if (periodic) timer->period = span;
    else timer->delay = span;

    if (!(timer->tp = CreateThreadpoolTimer(timer_callback, timer, NULL)))
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        IThreadPoolTimer_Release(&timer->IThreadPoolTimer_iface);
        return hr;
    }

    /* the first expiry is one period (or the delay) from now; a delay of zero fires at once */
    timer->pool_ref = 1;
    IThreadPoolTimer_AddRef(&timer->IThreadPoolTimer_iface);
    ticks = -span.Duration;
    due.dwLowDateTime = (DWORD)ticks;
    due.dwHighDateTime = (DWORD)(ticks >> 32);
    if (periodic) period_ms = (DWORD)min((span.Duration + 9999) / 10000, 0x7fffffff);
    SetThreadpoolTimer(timer->tp, &due, period_ms, 0);

    *ret = &timer->IThreadPoolTimer_iface;
    return S_OK;
}
