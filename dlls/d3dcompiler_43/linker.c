/*
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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
#include "wine/debug.h"
#include "d3dcompiler_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dcompiler);

struct d3d11_function_linking_graph
{
    ID3D11FunctionLinkingGraph ID3D11FunctionLinkingGraph_iface;
    LONG refcount;
    struct flg_graph *graph;
};

static inline struct d3d11_function_linking_graph *impl_from_ID3D11FunctionLinkingGraph(ID3D11FunctionLinkingGraph *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_function_linking_graph, ID3D11FunctionLinkingGraph_iface);
}

static HRESULT WINAPI d3d11_function_linking_graph_QueryInterface(ID3D11FunctionLinkingGraph *iface, REFIID riid, void **object)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("graph %p, riid %s, object %p.\n", graph, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11FunctionLinkingGraph)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI d3d11_function_linking_graph_AddRef(ID3D11FunctionLinkingGraph *iface)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);
    ULONG refcount = InterlockedIncrement(&graph->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI d3d11_function_linking_graph_Release(ID3D11FunctionLinkingGraph *iface)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);
    ULONG refcount = InterlockedDecrement(&graph->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        flg_graph_release(graph->graph);
        free(graph);
    }

    return refcount;
}

static HRESULT WINAPI d3d11_function_linking_graph_CreateModuleInstance(ID3D11FunctionLinkingGraph *iface,
        ID3D11ModuleInstance **instance, ID3DBlob **error)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);
    HRESULT hr;

    TRACE("iface %p, instance %p, error %p.\n", iface, instance, error);

    if (!instance)
        return E_INVALIDARG;
    hr = flg_graph_create_instance(graph->graph, instance);
    flg_graph_get_messages(graph->graph, error);
    return hr;
}

static HRESULT WINAPI d3d11_function_linking_graph_SetInputSignature(ID3D11FunctionLinkingGraph *iface,
        const D3D11_PARAMETER_DESC *parameter_desc, UINT parameter_count, ID3D11LinkingNode **input_node)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, parameter_desc %p, parameter_count %u, input_node %p.\n", iface,
            parameter_desc, parameter_count, input_node);

    return flg_graph_set_signature(graph->graph, FALSE, parameter_desc, parameter_count, input_node);
}

static HRESULT WINAPI d3d11_function_linking_graph_SetOutputSignature(ID3D11FunctionLinkingGraph *iface,
        const D3D11_PARAMETER_DESC *parameter_desc, UINT parameter_count, ID3D11LinkingNode **output_node)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, parameter_desc %p, parameter_count %u, output_node %p.\n", iface,
            parameter_desc, parameter_count, output_node);

    return flg_graph_set_signature(graph->graph, TRUE, parameter_desc, parameter_count, output_node);
}

static HRESULT WINAPI d3d11_function_linking_graph_CallFunction(ID3D11FunctionLinkingGraph *iface,
        LPCSTR namespace, ID3D11Module *module, LPCSTR function_name, ID3D11LinkingNode **call_node)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, namespace %s, module %p, function_name %s, call_node %p.\n", iface,
            wine_dbgstr_a(namespace), module, wine_dbgstr_a(function_name), call_node);

    return flg_graph_call_function(graph->graph, namespace, module, function_name, call_node);
}

static HRESULT WINAPI d3d11_function_linking_graph_PassValue(ID3D11FunctionLinkingGraph *iface,
        ID3D11LinkingNode *src_node, INT src_parameter_index, ID3D11LinkingNode *dst_node,
        INT dst_parameter_index)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, src_node %p, src_parameter_index %d, dst_node %p, dst_parameter_index %d.\n",
            iface, src_node, src_parameter_index, dst_node, dst_parameter_index);

    return flg_graph_pass_value(graph->graph, src_node, src_parameter_index, NULL, dst_node, dst_parameter_index, NULL);
}

static HRESULT WINAPI d3d11_function_linking_graph_PassValueWithSwizzle(ID3D11FunctionLinkingGraph *iface,
        ID3D11LinkingNode *src_node, INT src_parameter_index, LPCSTR src_swizzle,
        ID3D11LinkingNode *dst_node, INT dst_parameter_index, LPCSTR dst_swizzle)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, src_node %p, src_parameter_index %d, src_swizzle %s, dst_node %p, "
          "dst_parameter_index %d, dst_swizzle %s.\n", iface, src_node, src_parameter_index,
          wine_dbgstr_a(src_swizzle), dst_node, dst_parameter_index, wine_dbgstr_a(dst_swizzle));

    return flg_graph_pass_value(graph->graph, src_node, src_parameter_index, src_swizzle, dst_node, dst_parameter_index,
            dst_swizzle);
}

static HRESULT WINAPI d3d11_function_linking_graph_GetLastError(ID3D11FunctionLinkingGraph *iface,
        ID3DBlob **error)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, error %p.\n", iface, error);

    if (!error)
        return E_INVALIDARG;
    return flg_graph_get_messages(graph->graph, error);
}

static HRESULT WINAPI d3d11_function_linking_graph_GenerateHlsl(ID3D11FunctionLinkingGraph *iface,
        UINT flags, ID3DBlob **buffer)
{
    struct d3d11_function_linking_graph *graph = impl_from_ID3D11FunctionLinkingGraph(iface);

    TRACE("iface %p, flags %#x, buffer %p.\n", iface, flags, buffer);

    if (!buffer)
        return E_INVALIDARG;
    return flg_graph_generate_hlsl(graph->graph, buffer);
}

static const struct ID3D11FunctionLinkingGraphVtbl d3d11_function_linking_graph_vtbl =
{
    d3d11_function_linking_graph_QueryInterface,
    d3d11_function_linking_graph_AddRef,
    d3d11_function_linking_graph_Release,
    d3d11_function_linking_graph_CreateModuleInstance,
    d3d11_function_linking_graph_SetInputSignature,
    d3d11_function_linking_graph_SetOutputSignature,
    d3d11_function_linking_graph_CallFunction,
    d3d11_function_linking_graph_PassValue,
    d3d11_function_linking_graph_PassValueWithSwizzle,
    d3d11_function_linking_graph_GetLastError,
    d3d11_function_linking_graph_GenerateHlsl,
};

HRESULT WINAPI D3DCreateFunctionLinkingGraph(UINT flags, ID3D11FunctionLinkingGraph **graph)
{
    struct d3d11_function_linking_graph *object;

    TRACE("flags %#x, graph %p.\n", flags, graph);

    if (flags != 0 || !graph)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11FunctionLinkingGraph_iface.lpVtbl = &d3d11_function_linking_graph_vtbl;
    object->refcount = 1;
    if (!(object->graph = flg_graph_create()))
    {
        free(object);
        return E_OUTOFMEMORY;
    }

    *graph = &object->ID3D11FunctionLinkingGraph_iface;
    return S_OK;
}

struct d3d11_linker
{
    ID3D11Linker ID3D11Linker_iface;
    LONG refcount;
    ID3D11ModuleInstance **libraries;
    unsigned int library_count;
    BOOL linked;
};

static inline struct d3d11_linker *impl_from_ID3D11Linker(ID3D11Linker *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_linker, ID3D11Linker_iface);
}

static HRESULT WINAPI d3d11_linker_QueryInterface(ID3D11Linker *iface, REFIID riid, void **object)
{
    struct d3d11_linker *linker = impl_from_ID3D11Linker(iface);

    TRACE("linker %p, riid %s, object %p.\n", linker, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11Linker)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI d3d11_linker_AddRef(ID3D11Linker *iface)
{
    struct d3d11_linker *linker = impl_from_ID3D11Linker(iface);
    ULONG refcount = InterlockedIncrement(&linker->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI d3d11_linker_Release(ID3D11Linker *iface)
{
    struct d3d11_linker *linker = impl_from_ID3D11Linker(iface);
    ULONG refcount = InterlockedDecrement(&linker->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        unsigned int i;

        for (i = 0; i < linker->library_count; ++i)
            linker->libraries[i]->lpVtbl->Release(linker->libraries[i]);
        free(linker->libraries);
        free(linker);
    }

    return refcount;
}

static HRESULT WINAPI d3d11_linker_Link(ID3D11Linker *iface, ID3D11ModuleInstance *instance,
        LPCSTR instance_name, LPCSTR target_name, UINT flags, ID3DBlob **shader, ID3DBlob **error)
{
    struct d3d11_linker *linker = impl_from_ID3D11Linker(iface);

    TRACE("iface %p, instance %p, instance_name %s, target_name %s, flags %#x, shader %p, error %p.\n",
            iface, instance, wine_dbgstr_a(instance_name), wine_dbgstr_a(target_name), flags, shader, error);

    if (flags)
        FIXME("Ignoring flags %#x.\n", flags);
    /* a linker links once (measured: error X9203 on the second Link) */
    if (linker->linked)
    {
        static const char message[] = "error X9203: ID3D11Linker::SetEntry: entry has already been set\n";

        if (shader)
            *shader = NULL;
        if (error && SUCCEEDED(D3DCreateBlob(sizeof(message), error)))
            memcpy(ID3D10Blob_GetBufferPointer(*error), message, sizeof(message));
        return E_FAIL;
    }
    linker->linked = TRUE;
    return d3d11_link(instance, linker->libraries, linker->library_count, target_name, shader, error);
}

static HRESULT WINAPI d3d11_linker_UseLibrary(ID3D11Linker *iface, ID3D11ModuleInstance *instance)
{
    struct d3d11_linker *linker = impl_from_ID3D11Linker(iface);
    ID3D11ModuleInstance **libraries;

    TRACE("iface %p, instance %p.\n", iface, instance);

    if (!instance || !d3d11_module_instance_is_library(instance))
        return E_INVALIDARG;
    if (!(libraries = realloc(linker->libraries, (linker->library_count + 1) * sizeof(*libraries))))
        return E_OUTOFMEMORY;
    linker->libraries = libraries;
    /* the instance itself: a binding made later counts too (measured) */
    linker->libraries[linker->library_count++] = instance;
    instance->lpVtbl->AddRef(instance);
    return S_OK;
}

static HRESULT WINAPI d3d11_linker_AddClipPlaneFromCBuffer(ID3D11Linker *iface, UINT buffer_slot,
        UINT buffer_entry)
{
    FIXME("iface %p, buffer_slot %u, buffer_entry %u stub!\n", iface, buffer_slot, buffer_entry);
    return E_NOTIMPL;
}

static const struct ID3D11LinkerVtbl d3d11_linker_vtbl =
{
    d3d11_linker_QueryInterface,
    d3d11_linker_AddRef,
    d3d11_linker_Release,
    d3d11_linker_Link,
    d3d11_linker_UseLibrary,
    d3d11_linker_AddClipPlaneFromCBuffer,
};

HRESULT WINAPI D3DCreateLinker(ID3D11Linker **linker)
{
    struct d3d11_linker *object;

    TRACE("linker %p.\n", linker);

    if (!linker)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11Linker_iface.lpVtbl = &d3d11_linker_vtbl;
    object->refcount = 1;

    *linker = &object->ID3D11Linker_iface;
    return S_OK;
}
