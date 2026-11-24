/*
 * Shader libraries (D3DLoadModule), module instances and the linking of function linking graphs
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

/* The format of a shader library, as compiled for lib_4_0*, lib_5_0 (measured black box on libraries of our own, compiled by
 * d3dcompiler_47 on Windows 11; WineExplorer tests/uwp/alpc/flg-probe.cpp):
 * - one LIBF chunk per exported function and a LIBH chunk;
 * - a LIBF chunk is a whole DXBC container: SHDR or SHEX (the code, program type 0xfff0), LFS0 (the parameters), RDEF (the
 *   resources and constant buffers of the function, at the slots of the library), STAT, and Aon9 for the 10_level_9 targets;
 * - LFS0: the number of entries, the offset of the first entry, then per entry the twelve fields of D3D11_PARAMETER_DESC with
 *   the two strings as offsets into the chunk (0 is no string); entry 0 is the return value (flags 0 for a void function);
 * - LIBH: a version (1), the offset of the creator string, flags, the number of functions, the offset of a table of
 *   {0, offset of the name} in the order of the LIBF chunks.
 * In the code of a function the parameters that come in are v registers and the return value and the parameters that go out are o
 * registers, at the registers and components named by LFS0.
 *
 * Linking (ID3D11Linker::Link of the module instance of a function linking graph) inlines the called functions in the order they
 * were added to the graph: every input register of a function becomes a temporary register that is written with mov from where
 * the graph passes the value from, every output register becomes a temporary register, the temporary registers of the function
 * are moved past those of the functions before it, and the resources, samplers, constant buffers and UAVs go to the slots that
 * the module instance binds them to. The operand layout of the shader tokens is the one that vkd3d-shader (tpf.c) reads. */

#define COBJMACROS
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include "wine/debug.h"
#include "d3dcompiler_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dcompiler);

#define TAG_LIBF MAKE_TAG('L', 'I', 'B', 'F')
#define TAG_LIBH MAKE_TAG('L', 'I', 'B', 'H')
#define TAG_LFS0 MAKE_TAG('L', 'F', 'S', '0')
#define TAG_SHDR MAKE_TAG('S', 'H', 'D', 'R')
#define TAG_SHEX MAKE_TAG('S', 'H', 'E', 'X')

/* shader tokens */
#define SM4_OPCODE(t)            ((t) & 0x7ff)
#define SM4_INSTRUCTION_LENGTH(t) (((t) >> 24) & 0x7f)
#define SM4_EXTENDED             (1u << 31)
#define SM4_OP_CUSTOMDATA        0x35
#define SM4_OP_MOV               0x36
#define SM4_OP_RET               0x3e
#define SM4_OP_RETC              0x3f
#define SM4_OP_DCL_RESOURCE      0x58
#define SM4_OP_DCL_CONSTANT_BUFFER 0x59
#define SM4_OP_DCL_SAMPLER       0x5a
#define SM4_OP_DCL_INPUT         0x5f
#define SM4_OP_DCL_INPUT_SGV     0x60
#define SM4_OP_DCL_INPUT_SIV     0x61
#define SM4_OP_DCL_INPUT_PS      0x62
#define SM4_OP_DCL_INPUT_PS_SGV  0x63
#define SM4_OP_DCL_INPUT_PS_SIV  0x64
#define SM4_OP_DCL_OUTPUT        0x65
#define SM4_OP_DCL_OUTPUT_SGV    0x66
#define SM4_OP_DCL_OUTPUT_SIV    0x67
#define SM4_OP_DCL_TEMPS         0x68
#define SM4_OP_DCL_INDEXABLE_TEMP 0x69
#define SM4_OP_DCL_GLOBAL_FLAGS  0x6a
#define SM5_OP_DCL_UAV_TYPED     0x9c
#define SM5_OP_DCL_UAV_RAW       0x9d
#define SM5_OP_DCL_UAV_STRUCTURED 0x9e
#define SM5_OP_DCL_RESOURCE_RAW  0xa1
#define SM5_OP_DCL_RESOURCE_STRUCTURED 0xa2

#define SM4_RT_TEMP              0x00
#define SM4_RT_INPUT             0x01
#define SM4_RT_OUTPUT            0x02
#define SM4_RT_INDEXABLE_TEMP    0x03
#define SM4_RT_IMMCONST          0x04
#define SM4_RT_IMMCONST64        0x05
#define SM4_RT_SAMPLER           0x06
#define SM4_RT_RESOURCE          0x07
#define SM4_RT_CONSTBUFFER       0x08
#define SM4_RT_IMMCONSTBUFFER    0x09
#define SM4_RT_DEPTHOUT          0x0c
#define SM5_RT_UAV               0x1e

#define SM4_INTERPOLATION_LINEAR 2

struct token_buffer
{
    uint32_t *data;
    size_t count, capacity;
    BOOL failed;
};

static void put_token(struct token_buffer *b, uint32_t t)
{
    if (b->failed) return;
    if (b->count == b->capacity)
    {
        size_t capacity = max(64, b->capacity * 2);
        uint32_t *data = realloc(b->data, capacity * sizeof(*data));

        if (!data) { b->failed = TRUE; return; }
        b->data = data;
        b->capacity = capacity;
    }
    b->data[b->count++] = t;
}

struct library_parameter
{
    char *name, *semantic;
    D3D_SHADER_VARIABLE_TYPE type;
    D3D_SHADER_VARIABLE_CLASS class;
    UINT rows, columns;
    D3D_INTERPOLATION_MODE interpolation;
    D3D_PARAMETER_FLAGS flags;
    UINT in_reg, in_comp, out_reg, out_comp;
};

struct library_binding
{
    char *name;
    D3D_SHADER_INPUT_TYPE type;
    UINT bind_point, bind_count;
};

struct library_function
{
    char *name;
    struct library_parameter *params;   /* [0] is the return value */
    unsigned int param_count;           /* with [0] */
    uint32_t *code;                     /* with the version and length tokens */
    unsigned int code_count;
    struct library_binding *bindings;
    unsigned int binding_count;
};

struct d3d11_module
{
    ID3D11Module ID3D11Module_iface;
    LONG refcount;
    struct library_function *functions;
    unsigned int function_count;
};

static void free_parameter(struct library_parameter *p)
{
    free(p->name);
    free(p->semantic);
}

static void free_function(struct library_function *f)
{
    unsigned int i;

    free(f->name);
    for (i = 0; i < f->param_count; ++i) free_parameter(&f->params[i]);
    free(f->params);
    free(f->code);
    for (i = 0; i < f->binding_count; ++i) free(f->bindings[i].name);
    free(f->bindings);
}

static char *chunk_string(const char *data, size_t size, uint32_t offset)
{
    size_t len = 0;

    if (!offset || offset >= size) return NULL;
    while (offset + len < size && data[offset + len]) ++len;
    if (offset + len >= size) return NULL;
    return strdup(data + offset);
}

/* vkd3d_shader_parse_dxbc() reads the chunk offsets before it checks that the table of them fits in the data: check here */
static BOOL dxbc_chunk_table_fits(const void *data, size_t size)
{
    const uint32_t *u = data;

    return size >= 8 * sizeof(uint32_t) && u[7] <= (size - 8 * sizeof(uint32_t)) / sizeof(uint32_t);
}

/* a register holds at most four rows of four components */
static BOOL parameter_shape_valid(UINT rows, UINT columns)
{
    return rows <= 4 && columns <= 4;
}

static BOOL parse_lfs0(struct library_function *f, const char *data, size_t size)
{
    const uint32_t *u = (const uint32_t *)data;
    uint32_t count, offset, i;

    if (f->params) return FALSE;   /* a second LFS0 chunk */
    if (size < 8) return FALSE;
    count = u[0];
    offset = u[1];
    if (offset > size || count > (size - offset) / (12 * sizeof(uint32_t))) return FALSE;
    if (!(f->params = calloc(max(count, 1), sizeof(*f->params)))) return FALSE;
    f->param_count = count;
    for (i = 0; i < count; ++i)
    {
        const uint32_t *e = (const uint32_t *)(data + offset) + i * 12;
        struct library_parameter *p = &f->params[i];

        p->name = chunk_string(data, size, e[0]);
        p->semantic = chunk_string(data, size, e[1]);
        p->type = e[2];
        p->class = e[3];
        p->rows = e[4];
        p->columns = e[5];
        p->interpolation = e[6];
        p->flags = e[7];
        p->in_reg = e[8];
        p->in_comp = e[9];
        p->out_reg = e[10];
        p->out_comp = e[11];
        if (!parameter_shape_valid(p->rows, p->columns)) return FALSE;
    }
    return TRUE;
}

/* the resource bindings of RDEF (D3D10/11: eight UINTs per binding), for the Bind*ByName methods */
static BOOL parse_rdef_bindings(struct library_function *f, const char *data, size_t size)
{
    const uint32_t *u = (const uint32_t *)data;
    uint32_t count, offset, i, stride;

    if (f->bindings) return FALSE;   /* a second RDEF chunk */
    if (size < 7 * sizeof(uint32_t)) return FALSE;
    count = u[2];
    offset = u[3];
    stride = 8 * sizeof(uint32_t);
    /* 5.1 targets have two more fields per binding */
    if (((u[4] >> 4) & 0xf) == 5 && (u[4] & 0xf) == 1) stride = 10 * sizeof(uint32_t);
    if (offset > size || count > (size - offset) / stride) return FALSE;
    if (!(f->bindings = calloc(max(count, 1), sizeof(*f->bindings)))) return FALSE;
    f->binding_count = count;
    for (i = 0; i < count; ++i)
    {
        const uint32_t *e = (const uint32_t *)(data + offset + i * stride);

        f->bindings[i].name = chunk_string(data, size, e[0]);
        f->bindings[i].type = e[1];
        f->bindings[i].bind_point = e[5];
        f->bindings[i].bind_count = e[6];
    }
    return TRUE;
}

static HRESULT parse_library_function(struct library_function *f, const void *data, size_t size)
{
    struct vkd3d_shader_code code = {data, size};
    struct vkd3d_shader_dxbc_desc desc;
    BOOL have_code = FALSE, have_params = FALSE, duplicate = FALSE;
    unsigned int i;
    int ret;

    if (!dxbc_chunk_table_fits(data, size)) return E_FAIL;
    if ((ret = vkd3d_shader_parse_dxbc(&code, VKD3D_SHADER_PARSE_DXBC_IGNORE_CHECKSUM, &desc, NULL)) < 0)
    {
        WARN("Failed to parse a library function, ret %d.\n", ret);
        return E_FAIL;
    }
    for (i = 0; i < desc.section_count; ++i)
    {
        const struct vkd3d_shader_dxbc_section_desc *s = &desc.sections[i];

        switch (s->tag)
        {
            case TAG_SHDR:
            case TAG_SHEX:
            {
                const uint32_t *tokens = s->data.code;

                if (f->code) { duplicate = TRUE; break; }
                if (s->data.size < 8 || tokens[1] < 2 || tokens[1] > s->data.size / sizeof(uint32_t))
                    break;
                if (!(f->code = malloc(tokens[1] * sizeof(uint32_t)))) break;
                memcpy(f->code, tokens, tokens[1] * sizeof(uint32_t));
                f->code_count = tokens[1];
                have_code = TRUE;
                break;
            }
            case TAG_LFS0:
                if (f->params) duplicate = TRUE;
                else have_params = parse_lfs0(f, s->data.code, s->data.size);
                break;
            case TAG_RDEF:
                if (f->bindings) duplicate = TRUE;
                else parse_rdef_bindings(f, s->data.code, s->data.size);
                break;
        }
    }
    vkd3d_shader_free_dxbc(&desc);
    if (!have_code || !have_params || !f->param_count || duplicate)
    {
        WARN("Library function without code (%d) or parameters (%d), or with a chunk twice (%d).\n", have_code, have_params,
                duplicate);
        return E_FAIL;
    }
    return S_OK;
}

static inline struct d3d11_module *impl_from_ID3D11Module(ID3D11Module *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_module, ID3D11Module_iface);
}

static const struct ID3D11ModuleVtbl d3d11_module_vtbl;

static struct d3d11_module *unsafe_impl_from_ID3D11Module(ID3D11Module *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_module_vtbl) return NULL;
    return impl_from_ID3D11Module(iface);
}

static HRESULT WINAPI d3d11_module_QueryInterface(ID3D11Module *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11Module) || IsEqualGUID(riid, &IID_IUnknown))
    {
        iface->lpVtbl->AddRef(iface);
        *object = iface;
        return S_OK;
    }
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI d3d11_module_AddRef(ID3D11Module *iface)
{
    struct d3d11_module *module = impl_from_ID3D11Module(iface);
    ULONG refcount = InterlockedIncrement(&module->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI d3d11_module_Release(ID3D11Module *iface)
{
    struct d3d11_module *module = impl_from_ID3D11Module(iface);
    ULONG refcount = InterlockedDecrement(&module->refcount);
    unsigned int i;

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);
    if (!refcount)
    {
        for (i = 0; i < module->function_count; ++i) free_function(&module->functions[i]);
        free(module->functions);
        free(module);
    }
    return refcount;
}

static HRESULT WINAPI d3d11_module_CreateInstance(ID3D11Module *iface, const char *namespace, ID3D11ModuleInstance **instance)
{
    struct d3d11_module *module = impl_from_ID3D11Module(iface);

    TRACE("iface %p, namespace %s, instance %p.\n", iface, debugstr_a(namespace), instance);

    if (!instance) return E_INVALIDARG;
    return d3d11_module_instance_create(module, NULL, namespace, instance);
}

static const struct ID3D11ModuleVtbl d3d11_module_vtbl =
{
    d3d11_module_QueryInterface,
    d3d11_module_AddRef,
    d3d11_module_Release,
    d3d11_module_CreateInstance,
};

HRESULT WINAPI D3DLoadModule(const void *data, SIZE_T size, ID3D11Module **module)
{
    struct vkd3d_shader_code code = {data, size};
    struct vkd3d_shader_dxbc_desc desc;
    const char *libh = NULL;
    struct d3d11_module *object;
    size_t libh_size = 0;
    unsigned int i, n;
    HRESULT hr = S_OK;
    int ret;

    TRACE("data %p, size %Iu, module %p.\n", data, size, module);

    if (!data || !module) return E_INVALIDARG;
    *module = NULL;
    if (!dxbc_chunk_table_fits(data, size)) return E_FAIL;
    if ((ret = vkd3d_shader_parse_dxbc(&code, 0, &desc, NULL)) < 0)
    {
        WARN("Failed to parse the library, ret %d.\n", ret);
        return E_FAIL;
    }
    if (!(object = calloc(1, sizeof(*object))))
    {
        vkd3d_shader_free_dxbc(&desc);
        return E_OUTOFMEMORY;
    }
    object->ID3D11Module_iface.lpVtbl = &d3d11_module_vtbl;
    object->refcount = 1;

    for (i = 0, n = 0; i < desc.section_count; ++i)
    {
        if (desc.sections[i].tag == TAG_LIBF) ++n;
        else if (desc.sections[i].tag == TAG_LIBH) { libh = desc.sections[i].data.code; libh_size = desc.sections[i].data.size; }
    }
    if (!n || !(object->functions = calloc(n, sizeof(*object->functions))))
    {
        WARN("No library functions (%u).\n", n);
        hr = n ? E_OUTOFMEMORY : E_FAIL;
    }
    for (i = 0; SUCCEEDED(hr) && i < desc.section_count; ++i)
    {
        struct library_function *f;

        if (desc.sections[i].tag != TAG_LIBF) continue;
        f = &object->functions[object->function_count++];
        hr = parse_library_function(f, desc.sections[i].data.code, desc.sections[i].data.size);
    }
    if (SUCCEEDED(hr))
    {
        /* the names: LIBH, in the order of the LIBF chunks; the name of the return value of LFS0 is the same */
        const uint32_t *u = (const uint32_t *)libh;

        for (i = 0; i < object->function_count; ++i)
        {
            struct library_function *f = &object->functions[i];

            if (libh && libh_size >= 5 * sizeof(uint32_t) && i < u[3] && u[4] <= libh_size
                    && (libh_size - u[4]) / 8 > i)
                f->name = chunk_string(libh, libh_size, ((const uint32_t *)(libh + u[4]))[i * 2 + 1]);
            if (!f->name && f->params[0].name) f->name = strdup(f->params[0].name);
            if (!f->name)
            {
                WARN("Library function %u has no name.\n", i);
                hr = E_FAIL;
                break;
            }
            TRACE("Function %u %s: %u parameters, %u tokens, %u bindings.\n", i, debugstr_a(f->name), f->param_count - 1,
                    f->code_count, f->binding_count);
        }
    }
    vkd3d_shader_free_dxbc(&desc);
    if (FAILED(hr))
    {
        (&object->ID3D11Module_iface)->lpVtbl->Release(&object->ID3D11Module_iface);
        return hr;
    }
    *module = &object->ID3D11Module_iface;
    return S_OK;
}

static const struct library_function *module_find_function(struct d3d11_module *module, const char *name)
{
    unsigned int i;

    for (i = 0; i < module->function_count; ++i)
        if (module->functions[i].name && !strcmp(module->functions[i].name, name)) return &module->functions[i];
    return NULL;
}

/* module instances */

struct slot_binding
{
    UINT src, dst, count;
};

struct slot_bindings
{
    struct slot_binding *b;
    unsigned int count, capacity;
};

enum binding_kind
{
    BINDING_CBUFFER,
    BINDING_RESOURCE,
    BINDING_SAMPLER,
    BINDING_UAV,
    BINDING_RESOURCE_AS_UAV,
    BINDING_KIND_COUNT,
};

struct d3d11_module_instance
{
    ID3D11ModuleInstance ID3D11ModuleInstance_iface;
    LONG refcount;
    struct d3d11_module *module;     /* the instance of a library */
    struct flg_graph *graph;         /* the instance of a function linking graph */
    char *namespace;
    struct slot_bindings bindings[BINDING_KIND_COUNT];
};

static inline struct d3d11_module_instance *impl_from_ID3D11ModuleInstance(ID3D11ModuleInstance *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_module_instance, ID3D11ModuleInstance_iface);
}

static const struct ID3D11ModuleInstanceVtbl d3d11_module_instance_vtbl;

static struct d3d11_module_instance *unsafe_impl_from_ID3D11ModuleInstance(ID3D11ModuleInstance *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_module_instance_vtbl) return NULL;
    return impl_from_ID3D11ModuleInstance(iface);
}

static HRESULT WINAPI d3d11_module_instance_QueryInterface(ID3D11ModuleInstance *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11ModuleInstance) || IsEqualGUID(riid, &IID_IUnknown))
    {
        iface->lpVtbl->AddRef(iface);
        *object = iface;
        return S_OK;
    }
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI d3d11_module_instance_AddRef(ID3D11ModuleInstance *iface)
{
    struct d3d11_module_instance *instance = impl_from_ID3D11ModuleInstance(iface);
    ULONG refcount = InterlockedIncrement(&instance->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI d3d11_module_instance_Release(ID3D11ModuleInstance *iface)
{
    struct d3d11_module_instance *instance = impl_from_ID3D11ModuleInstance(iface);
    ULONG refcount = InterlockedDecrement(&instance->refcount);
    unsigned int i;

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);
    if (!refcount)
    {
        if (instance->module) (&instance->module->ID3D11Module_iface)->lpVtbl->Release(&instance->module->ID3D11Module_iface);
        if (instance->graph) flg_graph_release(instance->graph);
        for (i = 0; i < BINDING_KIND_COUNT; ++i) free(instance->bindings[i].b);
        free(instance->namespace);
        free(instance);
    }
    return refcount;
}

static HRESULT add_binding(struct d3d11_module_instance *instance, enum binding_kind kind, UINT src, UINT dst, UINT count)
{
    struct slot_bindings *s = &instance->bindings[kind];
    unsigned int i;

    if (!instance->module) return E_FAIL;
    for (i = 0; i < s->count; ++i)
    {
        if (s->b[i].src == src)
        {
            s->b[i].dst = dst;
            s->b[i].count = count;
            return S_OK;
        }
    }
    if (s->count == s->capacity)
    {
        unsigned int capacity = max(4, s->capacity * 2);
        struct slot_binding *b = realloc(s->b, capacity * sizeof(*b));

        if (!b) return E_OUTOFMEMORY;
        s->b = b;
        s->capacity = capacity;
    }
    s->b[s->count].src = src;
    s->b[s->count].dst = dst;
    s->b[s->count].count = count;
    ++s->count;
    return S_OK;
}

static BOOL binding_type_matches(enum binding_kind kind, D3D_SHADER_INPUT_TYPE type)
{
    switch (kind)
    {
        case BINDING_CBUFFER: return type == D3D_SIT_CBUFFER || type == D3D_SIT_TBUFFER;
        case BINDING_SAMPLER: return type == D3D_SIT_SAMPLER;
        case BINDING_UAV:
            return type == D3D_SIT_UAV_RWTYPED || type == D3D_SIT_UAV_RWSTRUCTURED || type == D3D_SIT_UAV_RWBYTEADDRESS
                    || type == D3D_SIT_UAV_APPEND_STRUCTURED || type == D3D_SIT_UAV_CONSUME_STRUCTURED
                    || type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
        default: return type == D3D_SIT_TEXTURE || type == D3D_SIT_STRUCTURED || type == D3D_SIT_BYTEADDRESS;
    }
}

static HRESULT add_binding_by_name(struct d3d11_module_instance *instance, enum binding_kind kind, const char *name, UINT dst,
        UINT count)
{
    unsigned int i, j;

    if (!instance->module || !name) return E_FAIL;
    for (i = 0; i < instance->module->function_count; ++i)
    {
        const struct library_function *f = &instance->module->functions[i];

        for (j = 0; j < f->binding_count; ++j)
            if (f->bindings[j].name && !strcmp(f->bindings[j].name, name) && binding_type_matches(kind, f->bindings[j].type))
                return add_binding(instance, kind, f->bindings[j].bind_point, dst, count);
    }
    WARN("No binding named %s.\n", debugstr_a(name));
    return E_FAIL;
}

static HRESULT WINAPI d3d11_module_instance_BindConstantBuffer(ID3D11ModuleInstance *iface, UINT src, UINT dst, UINT offset)
{
    TRACE("iface %p, src %u, dst %u, offset %u.\n", iface, src, dst, offset);

    if (offset) FIXME("Ignoring offset %u.\n", offset);
    return add_binding(impl_from_ID3D11ModuleInstance(iface), BINDING_CBUFFER, src, dst, 1);
}

static HRESULT WINAPI d3d11_module_instance_BindConstantBufferByName(ID3D11ModuleInstance *iface, const char *name, UINT dst,
        UINT offset)
{
    TRACE("iface %p, name %s, dst %u, offset %u.\n", iface, debugstr_a(name), dst, offset);

    if (offset) FIXME("Ignoring offset %u.\n", offset);
    return add_binding_by_name(impl_from_ID3D11ModuleInstance(iface), BINDING_CBUFFER, name, dst, 1);
}

static HRESULT WINAPI d3d11_module_instance_BindResource(ID3D11ModuleInstance *iface, UINT src, UINT dst, UINT count)
{
    TRACE("iface %p, src %u, dst %u, count %u.\n", iface, src, dst, count);
    return add_binding(impl_from_ID3D11ModuleInstance(iface), BINDING_RESOURCE, src, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindResourceByName(ID3D11ModuleInstance *iface, const char *name, UINT dst, UINT count)
{
    TRACE("iface %p, name %s, dst %u, count %u.\n", iface, debugstr_a(name), dst, count);
    return add_binding_by_name(impl_from_ID3D11ModuleInstance(iface), BINDING_RESOURCE, name, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindSampler(ID3D11ModuleInstance *iface, UINT src, UINT dst, UINT count)
{
    TRACE("iface %p, src %u, dst %u, count %u.\n", iface, src, dst, count);
    return add_binding(impl_from_ID3D11ModuleInstance(iface), BINDING_SAMPLER, src, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindSamplerByName(ID3D11ModuleInstance *iface, const char *name, UINT dst, UINT count)
{
    TRACE("iface %p, name %s, dst %u, count %u.\n", iface, debugstr_a(name), dst, count);
    return add_binding_by_name(impl_from_ID3D11ModuleInstance(iface), BINDING_SAMPLER, name, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindUnorderedAccessView(ID3D11ModuleInstance *iface, UINT src, UINT dst, UINT count)
{
    TRACE("iface %p, src %u, dst %u, count %u.\n", iface, src, dst, count);
    return add_binding(impl_from_ID3D11ModuleInstance(iface), BINDING_UAV, src, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindUnorderedAccessViewByName(ID3D11ModuleInstance *iface, const char *name, UINT dst,
        UINT count)
{
    TRACE("iface %p, name %s, dst %u, count %u.\n", iface, debugstr_a(name), dst, count);
    return add_binding_by_name(impl_from_ID3D11ModuleInstance(iface), BINDING_UAV, name, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindResourceAsUnorderedAccessView(ID3D11ModuleInstance *iface, UINT src, UINT dst,
        UINT count)
{
    FIXME("iface %p, src %u, dst %u, count %u semi-stub.\n", iface, src, dst, count);
    return add_binding(impl_from_ID3D11ModuleInstance(iface), BINDING_RESOURCE_AS_UAV, src, dst, count);
}

static HRESULT WINAPI d3d11_module_instance_BindResourceAsUnorderedAccessViewByName(ID3D11ModuleInstance *iface, const char *name,
        UINT dst, UINT count)
{
    FIXME("iface %p, name %s, dst %u, count %u semi-stub.\n", iface, debugstr_a(name), dst, count);
    return add_binding_by_name(impl_from_ID3D11ModuleInstance(iface), BINDING_RESOURCE_AS_UAV, name, dst, count);
}

static const struct ID3D11ModuleInstanceVtbl d3d11_module_instance_vtbl =
{
    d3d11_module_instance_QueryInterface,
    d3d11_module_instance_AddRef,
    d3d11_module_instance_Release,
    d3d11_module_instance_BindConstantBuffer,
    d3d11_module_instance_BindConstantBufferByName,
    d3d11_module_instance_BindResource,
    d3d11_module_instance_BindResourceByName,
    d3d11_module_instance_BindSampler,
    d3d11_module_instance_BindSamplerByName,
    d3d11_module_instance_BindUnorderedAccessView,
    d3d11_module_instance_BindUnorderedAccessViewByName,
    d3d11_module_instance_BindResourceAsUnorderedAccessView,
    d3d11_module_instance_BindResourceAsUnorderedAccessViewByName,
};

HRESULT d3d11_module_instance_create(struct d3d11_module *module, struct flg_graph *graph, const char *namespace,
        ID3D11ModuleInstance **instance)
{
    struct d3d11_module_instance *object;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    if (!(object->namespace = strdup(namespace ? namespace : "")))
    {
        free(object);
        return E_OUTOFMEMORY;
    }
    object->ID3D11ModuleInstance_iface.lpVtbl = &d3d11_module_instance_vtbl;
    object->refcount = 1;
    if ((object->module = module)) (&module->ID3D11Module_iface)->lpVtbl->AddRef(&module->ID3D11Module_iface);
    if ((object->graph = graph)) flg_graph_addref(graph);
    *instance = &object->ID3D11ModuleInstance_iface;
    return S_OK;
}

/* function linking graphs */
enum flg_node_kind
{
    FLG_NODE_INPUT,
    FLG_NODE_OUTPUT,
    FLG_NODE_CALL,
};

struct flg_node
{
    ID3D11LinkingNode ID3D11LinkingNode_iface;
    LONG refcount;
    struct flg_graph *graph;   /* the graph that owns the node; NULL once that graph is gone */
    enum flg_node_kind kind;
    unsigned int index;
    struct d3d11_module *module;
    char *namespace;
    const struct library_function *function;
    struct library_parameter *params;   /* the signature of an input or output node */
    unsigned int param_count;
};

struct flg_edge
{
    struct flg_node *src, *dst;
    int src_index, dst_index;
    char src_swizzle[5], dst_swizzle[5];
};

struct flg_graph
{
    LONG refcount;
    struct flg_node **nodes;
    unsigned int node_count, node_capacity;
    struct flg_edge *edges;
    unsigned int edge_count, edge_capacity;
    struct flg_node *input, *output;
    char *messages;
    BOOL failed;   /* after a call that failed, the graph refuses everything (measured) */
    BOOL sealed;   /* after CreateModuleInstance the graph cannot be changed (measured) */
};

static inline struct flg_node *impl_from_ID3D11LinkingNode(ID3D11LinkingNode *iface)
{
    return CONTAINING_RECORD(iface, struct flg_node, ID3D11LinkingNode_iface);
}

static const struct ID3D11LinkingNodeVtbl flg_node_vtbl;

static struct flg_node *unsafe_impl_from_ID3D11LinkingNode(ID3D11LinkingNode *iface)
{
    if (!iface || iface->lpVtbl != &flg_node_vtbl) return NULL;
    return impl_from_ID3D11LinkingNode(iface);
}

static HRESULT WINAPI flg_node_QueryInterface(ID3D11LinkingNode *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11LinkingNode) || IsEqualGUID(riid, &IID_IUnknown))
    {
        iface->lpVtbl->AddRef(iface);
        *object = iface;
        return S_OK;
    }
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));
    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI flg_node_AddRef(ID3D11LinkingNode *iface)
{
    struct flg_node *node = impl_from_ID3D11LinkingNode(iface);
    ULONG refcount = InterlockedIncrement(&node->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);
    return refcount;
}

static ULONG WINAPI flg_node_Release(ID3D11LinkingNode *iface)
{
    struct flg_node *node = impl_from_ID3D11LinkingNode(iface);
    ULONG refcount = InterlockedDecrement(&node->refcount);
    unsigned int i;

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);
    if (!refcount)
    {
        if (node->module) (&node->module->ID3D11Module_iface)->lpVtbl->Release(&node->module->ID3D11Module_iface);
        for (i = 0; i < node->param_count; ++i) free_parameter(&node->params[i]);
        free(node->params);
        free(node->namespace);
        free(node);
    }
    return refcount;
}

static const struct ID3D11LinkingNodeVtbl flg_node_vtbl =
{
    flg_node_QueryInterface,
    flg_node_AddRef,
    flg_node_Release,
};

static void WINAPIV flg_message(struct flg_graph *graph, const char *format, ...)
{
    size_t len = graph->messages ? strlen(graph->messages) : 0;
    char buffer[512], *m;
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n < 0) return;
    WARN("%s", buffer);
    if (!(m = realloc(graph->messages, len + strlen(buffer) + 1))) return;
    strcpy(m + len, buffer);
    graph->messages = m;
}

struct flg_graph *flg_graph_create(void)
{
    struct flg_graph *graph = calloc(1, sizeof(*graph));

    if (graph) graph->refcount = 1;
    return graph;
}

void flg_graph_addref(struct flg_graph *graph)
{
    InterlockedIncrement(&graph->refcount);
}

void flg_graph_release(struct flg_graph *graph)
{
    unsigned int i;

    if (InterlockedDecrement(&graph->refcount)) return;
    for (i = 0; i < graph->edge_count; ++i)
    {
        ID3D11LinkingNode *src = &graph->edges[i].src->ID3D11LinkingNode_iface, *dst = &graph->edges[i].dst->ID3D11LinkingNode_iface;

        src->lpVtbl->Release(src);
        dst->lpVtbl->Release(dst);
    }
    for (i = 0; i < graph->node_count; ++i)
    {
        ID3D11LinkingNode *node = &graph->nodes[i]->ID3D11LinkingNode_iface;

        /* a node the application still holds must not name a freed graph */
        graph->nodes[i]->graph = NULL;
        node->lpVtbl->Release(node);
    }
    free(graph->nodes);
    free(graph->edges);
    free(graph->messages);
    free(graph);
}

HRESULT flg_graph_get_messages(struct flg_graph *graph, ID3DBlob **blob)
{
    size_t len;
    HRESULT hr;

    if (!blob) return S_OK;
    *blob = NULL;
    if (!graph->messages || !(len = strlen(graph->messages))) return S_OK;
    if (FAILED(hr = D3DCreateBlob(len + 1, blob))) return hr;
    memcpy(ID3D10Blob_GetBufferPointer(*blob), graph->messages, len + 1);
    return S_OK;
}

static struct flg_node *flg_add_node(struct flg_graph *graph, enum flg_node_kind kind)
{
    struct flg_node *node;

    if (graph->node_count == graph->node_capacity)
    {
        unsigned int capacity = max(8, graph->node_capacity * 2);
        struct flg_node **nodes = realloc(graph->nodes, capacity * sizeof(*nodes));

        if (!nodes) return NULL;
        graph->nodes = nodes;
        graph->node_capacity = capacity;
    }
    if (!(node = calloc(1, sizeof(*node)))) return NULL;
    node->ID3D11LinkingNode_iface.lpVtbl = &flg_node_vtbl;
    node->refcount = 1;
    node->graph = graph;
    node->kind = kind;
    node->index = graph->node_count;
    graph->nodes[graph->node_count++] = node;
    return node;
}

static BOOL copy_parameter(struct library_parameter *dst, const D3D11_PARAMETER_DESC *src)
{
    dst->name = src->Name ? strdup(src->Name) : NULL;
    dst->semantic = src->SemanticName ? strdup(src->SemanticName) : NULL;
    dst->type = src->Type;
    dst->class = src->Class;
    dst->rows = src->Rows;
    dst->columns = src->Columns;
    dst->interpolation = src->InterpolationMode;
    dst->flags = src->Flags;
    dst->in_reg = src->FirstInRegister;
    dst->in_comp = src->FirstInComponent;
    dst->out_reg = src->FirstOutRegister;
    dst->out_comp = src->FirstOutComponent;
    return (!src->Name || dst->name) && (!src->SemanticName || dst->semantic);
}

static HRESULT flg_graph_set_signature_unchecked(struct flg_graph *graph, BOOL output, const D3D11_PARAMETER_DESC *desc, UINT count,
        ID3D11LinkingNode **node)
{
    struct flg_node *n, *other = output ? graph->input : graph->output;
    unsigned int i, j;

    if (!desc && count) return E_INVALIDARG;
    for (i = 0; i < count; ++i)
        if (!parameter_shape_valid(desc[i].Rows, desc[i].Columns)) return E_INVALIDARG;
    if (output ? graph->output : graph->input)
    {
        flg_message(graph, "error X9017: ID3D11FunctionLinkingGraph::Set%sSignature: the signature is already set\n",
                output ? "Output" : "Input");
        return E_FAIL;
    }
    /* the names of the parameters of the input and the output signature must differ (measured: error X9018) */
    for (i = 0; other && i < count; ++i)
        for (j = 0; j < other->param_count; ++j)
            if (desc[i].Name && other->params[j].name && !strcmp(desc[i].Name, other->params[j].name))
            {
                flg_message(graph, "error X9018: ID3D11FunctionLinkingGraph::SetOutputSignature: "
                        "input and output signatures have duplicate parameter names\n");
                return E_FAIL;
            }
    if (!(n = flg_add_node(graph, output ? FLG_NODE_OUTPUT : FLG_NODE_INPUT))) return E_OUTOFMEMORY;
    if (count && !(n->params = calloc(count, sizeof(*n->params)))) return E_OUTOFMEMORY;
    n->param_count = count;
    for (i = 0; i < count; ++i)
        if (!copy_parameter(&n->params[i], &desc[i])) return E_OUTOFMEMORY;
    if (output) graph->output = n;
    else graph->input = n;
    if (node)
    {
        *node = &n->ID3D11LinkingNode_iface;
        (*node)->lpVtbl->AddRef(*node);
    }
    return S_OK;
}

static HRESULT flg_graph_call_function_unchecked(struct flg_graph *graph, const char *namespace, ID3D11Module *module_iface, const char *name,
        ID3D11LinkingNode **node)
{
    struct d3d11_module *module = unsafe_impl_from_ID3D11Module(module_iface);
    const struct library_function *function;
    struct flg_node *n;

    if (!module || !name) return E_INVALIDARG;
    if (!(function = module_find_function(module, name)))
    {
        flg_message(graph, "error X9002: ID3D11FunctionLinkingGraph::CallFunction: function '%s' is not in the module\n", name);
        return E_FAIL;
    }
    if (!(n = flg_add_node(graph, FLG_NODE_CALL))) return E_OUTOFMEMORY;
    n->module = module;
    module_iface->lpVtbl->AddRef(module_iface);
    n->function = function;
    if (!(n->namespace = strdup(namespace ? namespace : ""))) return E_OUTOFMEMORY;   /* the graph is failed from now on */
    if (node)
    {
        *node = &n->ID3D11LinkingNode_iface;
        (*node)->lpVtbl->AddRef(*node);
    }
    return S_OK;
}

/* the parameter of a node, -1 being the return value of a call */
static const struct library_parameter *node_parameter(const struct flg_node *node, int index)
{
    if (node->kind == FLG_NODE_CALL)
    {
        /* in 64 bits: index + 1 must not overflow for INT_MAX */
        if (index < -1 || (INT64)index + 1 >= (INT64)node->function->param_count) return NULL;
        return &node->function->params[index + 1];
    }
    if (index < 0 || (INT64)index >= (INT64)node->param_count) return NULL;
    return &node->params[index];
}

static BOOL valid_swizzle(const char *s)
{
    size_t i, len;

    if (!s) return TRUE;
    if ((len = strlen(s)) > 4) return FALSE;
    for (i = 0; i < len; ++i)
        if (!strchr("xyzwrgba", s[i])) return FALSE;
    return TRUE;
}

static HRESULT flg_graph_pass_value_unchecked(struct flg_graph *graph, ID3D11LinkingNode *src_iface, int src_index, const char *src_swizzle,
        ID3D11LinkingNode *dst_iface, int dst_index, const char *dst_swizzle)
{
    struct flg_node *src = unsafe_impl_from_ID3D11LinkingNode(src_iface), *dst = unsafe_impl_from_ID3D11LinkingNode(dst_iface);
    struct flg_edge *e;

    if (!src || !dst) return E_INVALIDARG;
    /* a value cannot come from a call added after the call it goes to (measured: E_FAIL). A node of another graph is
     * accepted (measured); the edge keeps a reference to both nodes, and Link refuses such an edge. */
    if (src->kind == FLG_NODE_CALL && dst->kind == FLG_NODE_CALL && src->graph == graph && dst->graph == graph
            && src->index > dst->index)
    {
        flg_message(graph, "error X9006: ID3D11FunctionLinkingGraph::PassValue: the source is called after the destination\n");
        return E_FAIL;
    }
    if (!node_parameter(src, src_index) || !node_parameter(dst, dst_index) || dst->kind == FLG_NODE_INPUT
            || src->kind == FLG_NODE_OUTPUT || !valid_swizzle(src_swizzle) || !valid_swizzle(dst_swizzle))
    {
        flg_message(graph, "error X9005: ID3D11FunctionLinkingGraph::PassValue: invalid parameter %d -> %d\n", src_index, dst_index);
        return E_FAIL;
    }
    if (graph->edge_count == graph->edge_capacity)
    {
        unsigned int capacity = max(8, graph->edge_capacity * 2);
        struct flg_edge *edges = realloc(graph->edges, capacity * sizeof(*edges));

        if (!edges) return E_OUTOFMEMORY;
        graph->edges = edges;
        graph->edge_capacity = capacity;
    }
    e = &graph->edges[graph->edge_count++];
    memset(e, 0, sizeof(*e));
    e->src = src;
    src_iface->lpVtbl->AddRef(src_iface);
    e->src_index = src_index;
    e->dst = dst;
    dst_iface->lpVtbl->AddRef(dst_iface);
    e->dst_index = dst_index;
    if (src_swizzle) strcpy(e->src_swizzle, src_swizzle);
    if (dst_swizzle) strcpy(e->dst_swizzle, dst_swizzle);
    return S_OK;
}

static const char *hlsl_type(const struct library_parameter *p, char *buffer, size_t size)
{
    const char *base;

    switch (p->type)
    {
        case D3D_SVT_BOOL: base = "bool"; break;
        case D3D_SVT_INT: base = "int"; break;
        case D3D_SVT_UINT: base = "uint"; break;
        case D3D_SVT_DOUBLE: base = "double"; break;
        default: base = "float"; break;
    }
    if (p->class == D3D_SVC_MATRIX_ROWS || p->class == D3D_SVC_MATRIX_COLUMNS)
        snprintf(buffer, size, "%s%ux%u", base, p->rows, p->columns);
    else if (p->class == D3D_SVC_VECTOR)
        snprintf(buffer, size, "%s%u", base, p->columns);
    else
        snprintf(buffer, size, "%s", base);
    return buffer;
}

/* append formatted text of any length (the names come from the application's libraries and descriptions) */
static void WINAPIV append(char **text, size_t *len, const char *format, ...)
{
    va_list args;
    char *t;
    int n;

    va_start(args, format);
    n = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (n < 0 || !(t = realloc(*text, *len + n + 1))) return;
    va_start(args, format);
    vsnprintf(t + *len, n + 1, format, args);
    va_end(args);
    *text = t;
    *len += n;
}

/* the name of a parameter of a signature, or a made-up one when the description has none */
static const char *signature_param_name(const struct flg_node *node, int index, char *buffer, size_t size)
{
    const char *name = node->params[index].name;

    if (name) return name;
    snprintf(buffer, size, "%s%d", node->kind == FLG_NODE_OUTPUT ? "out" : "in", index);
    return buffer;
}

/* the HLSL of the graph, in the form of the one of Windows (the declarations of the functions, then main) */
HRESULT flg_graph_generate_hlsl(struct flg_graph *graph, ID3DBlob **blob)
{
    char *text = NULL, type[32], name[32], name2[32];
    unsigned int i, j, k;
    size_t len = 0;
    HRESULT hr;

    for (i = 0; i < graph->node_count; ++i)
    {
        const struct flg_node *n = graph->nodes[i];
        const struct library_function *f = n->function;

        if (n->kind != FLG_NODE_CALL) continue;
        append(&text, &len, "%s %s(", f->params[0].flags ? hlsl_type(&f->params[0], type, sizeof(type)) : "void", f->name);
        for (j = 1; j < f->param_count; ++j)
            append(&text, &len, "%s%s %s %s", j > 1 ? ", " : "", (f->params[j].flags & D3D_PF_OUT)
                    ? ((f->params[j].flags & D3D_PF_IN) ? "inout" : "out") : "in",
                    hlsl_type(&f->params[j], type, sizeof(type)), f->params[j].name ? f->params[j].name : "");
        append(&text, &len, ");\n");
    }
    append(&text, &len, "\nvoid main(");
    for (k = 0; k < 2; ++k)
    {
        const struct flg_node *n = k ? graph->output : graph->input;

        for (j = 0; n && j < n->param_count; ++j)
            append(&text, &len, "%s%s %s %s : %s", (k || j) ? ", " : "", k ? "out" : "in",
                    hlsl_type(&n->params[j], type, sizeof(type)), signature_param_name(n, j, name, sizeof(name)),
                    n->params[j].semantic ? n->params[j].semantic : "");
    }
    append(&text, &len, ")\n{\n");
    for (i = 0; i < graph->node_count; ++i)
    {
        const struct flg_node *n = graph->nodes[i];
        const struct library_function *f = n->function;

        if (n->kind != FLG_NODE_CALL) continue;
        for (j = 0; j < f->param_count; ++j)
        {
            if (!j && !f->params[0].flags) continue;
            append(&text, &len, "    %s %s_n%u_%u;\n", hlsl_type(&f->params[j], type, sizeof(type)), f->name, n->index, j);
        }
        for (k = 0; k < graph->edge_count; ++k)
        {
            const struct flg_edge *e = &graph->edges[k];
            const char *src_name;

            if (e->dst != n) continue;
            if (e->src->kind == FLG_NODE_INPUT)
                src_name = signature_param_name(e->src, e->src_index, name, sizeof(name));
            else
                src_name = NULL;
            if (src_name)
                append(&text, &len, "    %s_n%u_%d%s%s = %s%s%s;\n", f->name, n->index, e->dst_index + 1, *e->dst_swizzle ? "." : "",
                        e->dst_swizzle, src_name, *e->src_swizzle ? "." : "", e->src_swizzle);
            else
                append(&text, &len, "    %s_n%u_%d%s%s = %s_n%u_%d%s%s;\n", f->name, n->index, e->dst_index + 1,
                        *e->dst_swizzle ? "." : "", e->dst_swizzle, e->src->function->name, e->src->index, e->src_index + 1,
                        *e->src_swizzle ? "." : "", e->src_swizzle);
        }
        if (f->params[0].flags)
            append(&text, &len, "    %s_n%u_0 = ::%s(", f->name, n->index, f->name);
        else
            append(&text, &len, "    ::%s(", f->name);
        for (j = 1; j < f->param_count; ++j)
            append(&text, &len, "%s%s_n%u_%u", j > 1 ? ", " : "", f->name, n->index, j);
        append(&text, &len, ");\n");
    }
    for (k = 0; graph->output && k < graph->edge_count; ++k)
    {
        const struct flg_edge *e = &graph->edges[k];

        if (e->dst != graph->output) continue;
        if (e->src->kind == FLG_NODE_INPUT)
            append(&text, &len, "    %s%s%s = %s%s%s;\n", signature_param_name(graph->output, e->dst_index, name, sizeof(name)),
                    *e->dst_swizzle ? "." : "", e->dst_swizzle,
                    signature_param_name(e->src, e->src_index, name2, sizeof(name2)), *e->src_swizzle ? "." : "", e->src_swizzle);
        else
            append(&text, &len, "    %s%s%s = %s_n%u_%d%s%s;\n", signature_param_name(graph->output, e->dst_index, name, sizeof(name)),
                    *e->dst_swizzle ? "." : "", e->dst_swizzle, e->src->function->name, e->src->index, e->src_index + 1,
                    *e->src_swizzle ? "." : "", e->src_swizzle);
    }
    append(&text, &len, "}\n");
    if (!text) return E_OUTOFMEMORY;
    if (SUCCEEDED(hr = D3DCreateBlob(len + 1, blob)))
        memcpy(ID3D10Blob_GetBufferPointer(*blob), text, len + 1);
    free(text);
    return hr;
}

static HRESULT flg_graph_create_instance_unchecked(struct flg_graph *graph, ID3D11ModuleInstance **instance)
{
    if (!graph->input && !graph->output)
    {
        flg_message(graph, "error X9019: ID3D11FunctionLinkingGraph::CreateModuleInstance: the graph has no signature\n");
        return E_FAIL;
    }
    return d3d11_module_instance_create(NULL, graph, "", instance);
}

/* the public entry points: a graph that refused a call refuses all later ones with E_FAIL (measured on Windows) */
#define FLG_CHECKED(call) \
    do { HRESULT hr_; if (graph->failed) return E_FAIL; if (FAILED(hr_ = (call))) graph->failed = TRUE; return hr_; } while (0)
#define FLG_CHANGE(call) \
    do { if (graph->sealed) { graph->failed = TRUE; return E_FAIL; } FLG_CHECKED(call); } while (0)

HRESULT flg_graph_set_signature(struct flg_graph *graph, BOOL output, const D3D11_PARAMETER_DESC *desc, UINT count,
        ID3D11LinkingNode **node)
{
    FLG_CHANGE(flg_graph_set_signature_unchecked(graph, output, desc, count, node));
}

HRESULT flg_graph_call_function(struct flg_graph *graph, const char *namespace, ID3D11Module *module, const char *name,
        ID3D11LinkingNode **node)
{
    FLG_CHANGE(flg_graph_call_function_unchecked(graph, namespace, module, name, node));
}

HRESULT flg_graph_pass_value(struct flg_graph *graph, ID3D11LinkingNode *src, int src_index, const char *src_swizzle,
        ID3D11LinkingNode *dst, int dst_index, const char *dst_swizzle)
{
    FLG_CHANGE(flg_graph_pass_value_unchecked(graph, src, src_index, src_swizzle, dst, dst_index, dst_swizzle));
}

HRESULT flg_graph_create_instance(struct flg_graph *graph, ID3D11ModuleInstance **instance)
{
    HRESULT hr;

    if (graph->failed) return E_FAIL;
    if (FAILED(hr = flg_graph_create_instance_unchecked(graph, instance))) graph->failed = TRUE;
    else graph->sealed = TRUE;
    return hr;
}

/* linking */

struct node_outputs
{
    unsigned int out[32];   /* o register of the function -> temporary register */
    BOOL linked;            /* the call is inlined already: its outputs are written */
};

struct link_context
{
    struct flg_graph *graph;
    struct d3d11_module_instance **libraries;
    unsigned int library_count;
    unsigned int shader_type;            /* D3D10_SB_TOKENIZED_PROGRAM_TYPE: 0 pixel, 1 vertex, ... */
    unsigned int major, minor;
    struct token_buffer decls, body, icb;   /* icb: the immediate constant buffer of a function, if any */
    unsigned int temp_count, indexable_temp_count;
    uint32_t global_flags;
    BOOL have_icb;
    struct node_outputs *outputs;             /* per node: where the output registers of an inlined call went */
    unsigned int *input_regs, *output_regs;   /* of the signature parameters of the shader */
    BYTE input_used[32];                      /* the components read, per input register */
    char *messages;
    HRESULT hr;
};

static void WINAPIV link_error(struct link_context *ctx, const char *format, ...)
{
    size_t len = ctx->messages ? strlen(ctx->messages) : 0;
    char buffer[512], *m;
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n < 0) return;
    WARN("%s", buffer);
    if (SUCCEEDED(ctx->hr)) ctx->hr = E_FAIL;
    if (!(m = realloc(ctx->messages, len + strlen(buffer) + 1))) return;
    strcpy(m + len, buffer);
    ctx->messages = m;
}

/* where the registers of one inlined function go */
struct callee_map
{
    unsigned int in[32], out[32];          /* v and o register -> temporary register, ~0u if none */
    unsigned int temp_base, indexable_base;
    const struct d3d11_module_instance *instance;
    const struct library_function *function;
};

static BOOL map_slot(struct link_context *ctx, const struct callee_map *map, enum binding_kind kind, uint32_t *slot)
{
    const struct slot_bindings *s = &map->instance->bindings[kind];
    static const char *names[] = {"constant buffer", "resource", "sampler", "UAV", "resource as UAV"};
    unsigned int i;

    for (i = 0; i < s->count; ++i)
    {
        if (*slot >= s->b[i].src && *slot < s->b[i].src + max(s->b[i].count, 1))
        {
            *slot = s->b[i].dst + (*slot - s->b[i].src);
            return TRUE;
        }
    }
    /* measured: error X9210 for a constant buffer, X9207 for the others */
    if (kind == BINDING_CBUFFER)
    {
        link_error(ctx, "error X9210: ID3D11Linker::Link: remapping has not been specified for cbuffer slot %u in function '%s::%s'\n",
                *slot, map->instance->namespace, map->function->name);
        return FALSE;
    }
    link_error(ctx, "error X9207: ID3D11Linker::Link: illegal remapping specified for %s slot range [%u, %u] in function '%s::%s'\n",
            names[kind], *slot, *slot, map->instance->namespace, map->function->name);
    return FALSE;
}

static const uint32_t *rewrite_operand(struct link_context *ctx, uint32_t *p, const uint32_t *end, const struct callee_map *map)
{
    uint32_t token, type, order, i, *type_token;

    if (p >= end) { link_error(ctx, "error: truncated operand\n"); return NULL; }
    type_token = p;
    token = *p++;
    type = (token >> 12) & 0xff;
    order = (token >> 20) & 0x3;
    if (token & SM4_EXTENDED)
    {
        uint32_t ext;

        do
        {
            if (p >= end) { link_error(ctx, "error: truncated operand\n"); return NULL; }
            ext = *p++;
        } while (ext & SM4_EXTENDED);
    }
    for (i = 0; i < order; ++i)
    {
        uint32_t repr = (token >> (22 + 3 * i)) & 0x7, *imm = NULL;

        switch (repr)
        {
            case 0: /* immediate32 */
                imm = p++;
                break;
            case 1: /* immediate64 */
                p += 2;
                break;
            case 2: /* relative */
                if (i == 0 && (type == SM4_RT_INPUT || type == SM4_RT_OUTPUT))
                {
                    link_error(ctx, "error: relative addressing of a parameter register is not supported\n");
                    return NULL;
                }
                if (!(p = (uint32_t *)rewrite_operand(ctx, p, end, map))) return NULL;
                break;
            case 3: /* immediate32 + relative */
                if (i == 0 && (type == SM4_RT_INPUT || type == SM4_RT_OUTPUT))
                {
                    link_error(ctx, "error: relative addressing of a parameter register is not supported\n");
                    return NULL;
                }
                imm = p++;
                if (!(p = (uint32_t *)rewrite_operand(ctx, p, end, map))) return NULL;
                break;
            case 4: /* immediate64 + relative */
                p += 2;
                if (!(p = (uint32_t *)rewrite_operand(ctx, p, end, map))) return NULL;
                break;
            default:
                link_error(ctx, "error: unknown index representation %u\n", repr);
                return NULL;
        }
        if (p > end) { link_error(ctx, "error: truncated operand\n"); return NULL; }
        if (i || !imm) continue;

        switch (type)
        {
            case SM4_RT_TEMP:
                *imm += map->temp_base;
                break;
            case SM4_RT_INDEXABLE_TEMP:
                *imm += map->indexable_base;
                break;
            case SM4_RT_INPUT:
            case SM4_RT_OUTPUT:
            {
                const unsigned int *m = type == SM4_RT_INPUT ? map->in : map->out;

                if (*imm >= 32 || m[*imm] == ~0u)
                {
                    link_error(ctx, "error: function '%s' uses %c%u, which is not a parameter\n", map->function->name,
                            type == SM4_RT_INPUT ? 'v' : 'o', *imm);
                    return NULL;
                }
                *imm = m[*imm];
                *type_token = (*type_token & ~(0xffu << 12)) | (SM4_RT_TEMP << 12);
                break;
            }
            case SM4_RT_SAMPLER:
                if (!map_slot(ctx, map, BINDING_SAMPLER, imm)) return NULL;
                break;
            case SM4_RT_RESOURCE:
                if (!map_slot(ctx, map, BINDING_RESOURCE, imm)) return NULL;
                break;
            case SM4_RT_CONSTBUFFER:
                if (!map_slot(ctx, map, BINDING_CBUFFER, imm)) return NULL;
                break;
            case SM5_RT_UAV:
                if (!map_slot(ctx, map, BINDING_UAV, imm)) return NULL;
                break;
        }
    }
    if (type == SM4_RT_IMMCONST || type == SM4_RT_IMMCONST64)
    {
        unsigned int count = (token & 0x3) == 2 ? 4 : (type == SM4_RT_IMMCONST64 ? 2 : 1);

        p += count;
        if (p > end) { link_error(ctx, "error: truncated immediate\n"); return NULL; }
    }
    return p;
}

/* a declaration of the shader for a resource, sampler, constant buffer or UAV of a function, at the slot it is bound to; one
 * declaration per slot (the largest constant buffer) */
static void add_resource_declaration(struct link_context *ctx, const uint32_t *ins, unsigned int len)
{
    uint32_t opcode = SM4_OPCODE(ins[0]);
    size_t i, step;

    if (len < 3)
    {
        link_error(ctx, "error: a declaration of %u tokens is too short\n", len);
        return;
    }
    /* ctx->decls holds only declarations with an instruction length (the immediate constant buffer is kept apart) */
    for (i = 0; i < ctx->decls.count; i += step)
    {
        uint32_t *d = &ctx->decls.data[i];

        if (!(step = SM4_INSTRUCTION_LENGTH(d[0])) || i + step > ctx->decls.count) break;
        /* the operand token and its first index have the same position in both */
        if (step < 3 || SM4_OPCODE(d[0]) != opcode || ((d[1] >> 12) & 0xff) != ((ins[1] >> 12) & 0xff) || d[2] != ins[2])
            continue;
        if (opcode == SM4_OP_DCL_CONSTANT_BUFFER && len > 3 && d[3] < ins[3]) d[3] = ins[3];
        return;
    }
    for (i = 0; i < len; ++i) put_token(&ctx->decls, ins[i]);
}

static void inline_function(struct link_context *ctx, const struct callee_map *map)
{
    const struct library_function *f = map->function;
    const uint32_t *end = f->code + f->code_count;
    uint32_t *code, *p, *code_end;

    if (!(code = malloc(f->code_count * sizeof(*code)))) { ctx->hr = E_OUTOFMEMORY; return; }
    memcpy(code, f->code, f->code_count * sizeof(*code));
    code_end = code + (end - f->code);
    p = code + 2;
    while (p < code_end && SUCCEEDED(ctx->hr))
    {
        uint32_t token = p[0], opcode = SM4_OPCODE(token), len;
        uint32_t *operands, *ins_end;

        if (opcode == SM4_OP_CUSTOMDATA)
        {
            if (p + 1 >= code_end || p[1] < 2) { link_error(ctx, "error: bad custom data\n"); break; }
            len = p[1];
            if ((token >> 11) == 3) /* the immediate constant buffer */
            {
                if (ctx->have_icb)
                    link_error(ctx, "error: more than one function with an immediate constant buffer is not supported\n");
                else if (len > (size_t)(code_end - p))
                    link_error(ctx, "error: truncated immediate constant buffer in function '%s'\n", f->name);
                else
                {
                    unsigned int i;

                    ctx->have_icb = TRUE;
                    for (i = 0; i < len; ++i) put_token(&ctx->icb, p[i]);
                }
            }
            if (len > (size_t)(code_end - p)) break;
            p += len;
            continue;
        }
        len = SM4_INSTRUCTION_LENGTH(token);
        if (!len || p + len > code_end) { link_error(ctx, "error: bad instruction length in function '%s'\n", f->name); break; }
        ins_end = p + len;

        switch (opcode)
        {
            case SM4_OP_DCL_INPUT: case SM4_OP_DCL_INPUT_SGV: case SM4_OP_DCL_INPUT_SIV: case SM4_OP_DCL_INPUT_PS:
            case SM4_OP_DCL_INPUT_PS_SGV: case SM4_OP_DCL_INPUT_PS_SIV: case SM4_OP_DCL_OUTPUT: case SM4_OP_DCL_OUTPUT_SGV:
            case SM4_OP_DCL_OUTPUT_SIV: case SM4_OP_DCL_TEMPS:
                break;

            case SM4_OP_DCL_GLOBAL_FLAGS:
                ctx->global_flags |= (token >> 11) & 0x1ff;
                break;

            case SM4_OP_DCL_INDEXABLE_TEMP:
                if (len >= 4)
                {
                    put_token(&ctx->decls, token);
                    put_token(&ctx->decls, p[1] + map->indexable_base);
                    put_token(&ctx->decls, p[2]);
                    put_token(&ctx->decls, p[3]);
                }
                break;

            case SM4_OP_DCL_RESOURCE: case SM4_OP_DCL_CONSTANT_BUFFER: case SM4_OP_DCL_SAMPLER: case SM5_OP_DCL_UAV_TYPED:
            case SM5_OP_DCL_UAV_RAW: case SM5_OP_DCL_UAV_STRUCTURED: case SM5_OP_DCL_RESOURCE_RAW:
            case SM5_OP_DCL_RESOURCE_STRUCTURED:
                if (!rewrite_operand(ctx, p + 1, ins_end, map)) break;
                add_resource_declaration(ctx, p, len);
                break;

            case SM4_OP_RET:
                if (len != 1 || ins_end != code_end)
                    link_error(ctx, "error: a return before the end of function '%s' is not supported\n", f->name);
                break;

            case SM4_OP_RETC:
                link_error(ctx, "error: a conditional return in function '%s' is not supported\n", f->name);
                break;

            default:
                if (opcode >= SM4_OP_DCL_RESOURCE && opcode <= SM4_OP_DCL_GLOBAL_FLAGS)
                {
                    FIXME("Skipping declaration %#x in function %s.\n", opcode, debugstr_a(f->name));
                    break;
                }
                operands = p + 1;
                if (token & SM4_EXTENDED)
                {
                    while (operands < ins_end && (*operands & SM4_EXTENDED)) ++operands;
                    ++operands;
                }
                while (operands < ins_end && SUCCEEDED(ctx->hr))
                    if (!(operands = (uint32_t *)rewrite_operand(ctx, operands, ins_end, map))) break;
                if (SUCCEEDED(ctx->hr))
                {
                    uint32_t *q;

                    for (q = p; q < ins_end; ++q) put_token(&ctx->body, *q);
                }
                break;
        }
        p = ins_end;
    }
    free(code);
}

static unsigned int temp_count_of(const struct library_function *f, unsigned int *indexable)
{
    const uint32_t *p = f->code + 2, *end = f->code + f->code_count;
    unsigned int temps = 0;

    *indexable = 0;
    while (p < end)
    {
        uint32_t opcode = SM4_OPCODE(p[0]), len = opcode == SM4_OP_CUSTOMDATA ? (p + 1 < end ? p[1] : 0) : SM4_INSTRUCTION_LENGTH(p[0]);

        /* a zero or truncated length ends the scan (inline_function reports the error) */
        if (!len || len > (size_t)(end - p)) break;
        if (opcode == SM4_OP_DCL_TEMPS && len >= 2) temps = max(temps, min(p[1], 4096));
        if (opcode == SM4_OP_DCL_INDEXABLE_TEMP && len >= 2) *indexable = max(*indexable, min(p[1], 4095) + 1);
        p += len;
    }
    return temps;
}

/* the components of a parameter in its register(s) */
static unsigned int parameter_mask(const struct library_parameter *p, BOOL in)
{
    unsigned int comp = in ? p->in_comp : p->out_comp, cols = p->class == D3D_SVC_SCALAR ? 1 : max(p->columns, 1);

    return ((1u << min(cols, 4)) - 1) << min(comp, 3);
}

static int swizzle_component(char c)
{
    switch (c)
    {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        default: return 3;
    }
}

/* where a value lives: a register type and index and the first component */
struct value_location
{
    uint32_t type, reg, comp;
};

/* sel[j]: the source component that goes to component j of the destination, -1 for none */
static void emit_mov(struct link_context *ctx, uint32_t dst_type, uint32_t dst_reg, const int *sel, const struct value_location *src)
{
    unsigned int swizzle = 0, dst_mask = 0, j, first = 0;

    for (j = 0; j < 4; ++j)
        if (sel[j] >= 0) { first = sel[j]; break; }
    for (j = 0; j < 4; ++j)
    {
        if (sel[j] >= 0) dst_mask |= 1u << j;
        swizzle |= ((sel[j] >= 0 ? sel[j] : first) & 3) << (2 * j);
    }
    if (!dst_mask) return;
    put_token(&ctx->body, SM4_OP_MOV | (5u << 24));
    /* dst: four components, mask mode, the mask, the type, one immediate index */
    put_token(&ctx->body, 0x2 | (0u << 2) | (dst_mask << 4) | (dst_type << 12) | (1u << 20));
    put_token(&ctx->body, dst_reg);
    /* src: four components, swizzle mode */
    put_token(&ctx->body, 0x2 | (1u << 2) | (swizzle << 4) | (src->type << 12) | (1u << 20));
    put_token(&ctx->body, src->reg);
    if (src->type == SM4_RT_INPUT && src->reg < ARRAY_SIZE(ctx->input_used))
        for (j = 0; j < 4; ++j)
            if (sel[j] >= 0) ctx->input_used[src->reg] |= 1u << sel[j];
}

/* emit the movs of the edges into one parameter */
static void pass_values(struct link_context *ctx, const struct flg_node *dst_node, int dst_index, uint32_t dst_type,
        uint32_t dst_reg, const struct library_parameter *dst_param, BOOL dst_in, struct value_location (*node_out)(struct link_context *,
        const struct flg_node *, int))
{
    unsigned int k, j, rows = dst_param->class == D3D_SVC_MATRIX_ROWS || dst_param->class == D3D_SVC_MATRIX_COLUMNS
            ? max(dst_param->rows, 1) : 1;

    for (k = 0; k < ctx->graph->edge_count; ++k)
    {
        const struct flg_edge *e = &ctx->graph->edges[k];
        const struct library_parameter *src_param;
        unsigned int comps[4], dst_comps[4], count = 0, dst_count = 0, r, base_dst;
        struct value_location src;
        int sel[4] = {-1, -1, -1, -1};

        if (e->dst != dst_node || e->dst_index != dst_index) continue;
        src_param = node_parameter(e->src, e->src_index);
        src = node_out(ctx, e->src, e->src_index);
        base_dst = dst_in ? dst_param->in_comp : dst_param->out_comp;
        /* the components written, in the order of the destination swizzle (component k of the source swizzle goes to component
         * k of the destination swizzle, measured), else the components of the parameter in order */
        if (*e->dst_swizzle)
        {
            for (j = 0; e->dst_swizzle[j] && dst_count < 4; ++j)
                dst_comps[dst_count++] = min(base_dst + swizzle_component(e->dst_swizzle[j]), 3);
        }
        else
        {
            unsigned int dst_mask = parameter_mask(dst_param, dst_in);

            for (j = 0; j < 4; ++j)
                if (dst_mask & (1u << j)) dst_comps[dst_count++] = j;
        }
        if (*e->src_swizzle)
        {
            for (j = 0; e->src_swizzle[j] && count < 4; ++j) comps[count++] = min(src.comp + swizzle_component(e->src_swizzle[j]), 3);
        }
        else
        {
            /* a parameter of the input signature starts at component 0 of its register */
            unsigned int src_mask = e->src->kind == FLG_NODE_INPUT ? parameter_mask(src_param, TRUE) >> src_param->in_comp
                    : parameter_mask(src_param, FALSE);

            for (j = 0; j < 4 && count < 4; ++j)
                if (src_mask & (1u << j)) comps[count++] = j;
        }
        if (!count || !dst_count) continue;
        /* a source with fewer components than the destination repeats its last one */
        for (j = 0; j < dst_count; ++j) sel[dst_comps[j]] = comps[min(j, count - 1)];
        for (r = 0; r < rows; ++r)
        {
            struct value_location row = src;

            row.reg += r;
            emit_mov(ctx, dst_type, dst_reg + r, sel, &row);
        }
    }
}

static struct value_location node_output_location(struct link_context *ctx, const struct flg_node *node, int index)
{
    const struct library_parameter *p = node_parameter(node, index);
    struct value_location l = {SM4_RT_TEMP, 0, 0};

    if (node->graph != ctx->graph)
    {
        link_error(ctx, "error X9006: ID3D11Linker::Link: a value is passed from a node of another graph\n");
        return l;
    }
    if (node->kind == FLG_NODE_INPUT)
    {
        l.type = SM4_RT_INPUT;
        l.reg = ctx->input_regs[index];
        l.comp = 0;
        return l;
    }
    if (!ctx->outputs[node->index].linked)
    {
        /* a value passed from a call that comes later in the graph */
        link_error(ctx, "error: function '%s' is called after the function it passes a value to\n", node->function->name);
        return l;
    }
    l.reg = ctx->outputs[node->index].out[min(p->out_reg, 31)];
    l.comp = p->out_comp;
    if (l.reg == ~0u) link_error(ctx, "error: the value of function '%s' parameter %d is not available\n", node->function->name, index);
    return l;
}

static const struct d3d11_module_instance *find_library(struct link_context *ctx, const struct flg_node *node)
{
    unsigned int i;

    for (i = 0; i < ctx->library_count; ++i)
        if (ctx->libraries[i]->module == node->module && !strcmp(ctx->libraries[i]->namespace, node->namespace))
            return ctx->libraries[i];
    return NULL;
}

static void link_call(struct link_context *ctx, const struct flg_node *node)
{
    const struct library_function *f = node->function;
    struct callee_map map;
    unsigned int i, r, temps, indexable;

    memset(map.in, 0xff, sizeof(map.in));
    memset(map.out, 0xff, sizeof(map.out));
    map.function = f;
    if (!(map.instance = find_library(ctx, node)))
    {
        link_error(ctx, "error X9201: ID3D11Linker::Link: the module of function '%s' is not used by the linker (UseLibrary)\n",
                f->name);
        return;
    }
    /* a temporary register for every input and output register of the function (the rows of a matrix in consecutive ones) */
    for (i = 0; i < f->param_count; ++i)
    {
        const struct library_parameter *p = &f->params[i];
        unsigned int rows = p->class == D3D_SVC_MATRIX_ROWS || p->class == D3D_SVC_MATRIX_COLUMNS ? max(p->rows, 1) : 1;

        for (r = 0; (p->flags & D3D_PF_IN) && r < rows; ++r)
            if (p->in_reg + r < 32 && map.in[p->in_reg + r] == ~0u) map.in[p->in_reg + r] = ctx->temp_count++;
    }
    for (i = 0; i < f->param_count; ++i)
    {
        const struct library_parameter *p = &f->params[i];
        unsigned int rows = p->class == D3D_SVC_MATRIX_ROWS || p->class == D3D_SVC_MATRIX_COLUMNS ? max(p->rows, 1) : 1;

        for (r = 0; (p->flags & D3D_PF_OUT) && r < rows; ++r)
            if (p->out_reg + r < 32 && map.out[p->out_reg + r] == ~0u) map.out[p->out_reg + r] = ctx->temp_count++;
    }
    /* the values passed in */
    for (i = 1; i < f->param_count; ++i)
        if (f->params[i].flags & D3D_PF_IN)
            pass_values(ctx, node, (int)i - 1, SM4_RT_TEMP, map.in[min(f->params[i].in_reg, 31)], &f->params[i], TRUE,
                    node_output_location);
    temps = temp_count_of(f, &indexable);
    map.temp_base = ctx->temp_count;
    map.indexable_base = ctx->indexable_temp_count;
    ctx->temp_count += temps;
    ctx->indexable_temp_count += indexable;
    inline_function(ctx, &map);
    memcpy(ctx->outputs[node->index].out, map.out, sizeof(map.out));
    ctx->outputs[node->index].linked = TRUE;
}

static void put_operand_reg(struct token_buffer *b, uint32_t type, uint32_t reg, unsigned int mask)
{
    put_token(b, 0x2 | (mask << 4) | (type << 12) | (1u << 20));
    put_token(b, reg);
}

/* D3D_NAME of a system value semantic, 0 for none */
static unsigned int system_value(const char *semantic, BOOL output, unsigned int shader_type)
{
    char name[64];
    size_t len;

    if (!semantic) return 0;
    snprintf(name, sizeof(name), "%s", semantic);
    for (len = strlen(name); len && isdigit((unsigned char)name[len - 1]); --len) name[len - 1] = 0;
    if (!_stricmp(name, "SV_POSITION")) return (shader_type == 0 && output) ? 0 : D3D_NAME_POSITION;
    if (!_stricmp(name, "SV_TARGET")) return output ? D3D_NAME_TARGET : 0;
    if (!_stricmp(name, "SV_DEPTH")) return output ? D3D_NAME_DEPTH : 0;
    if (!_stricmp(name, "SV_VERTEXID")) return D3D_NAME_VERTEX_ID;
    if (!_stricmp(name, "SV_INSTANCEID")) return D3D_NAME_INSTANCE_ID;
    if (!_stricmp(name, "SV_PRIMITIVEID")) return D3D_NAME_PRIMITIVE_ID;
    if (!_stricmp(name, "SV_ISFRONTFACE")) return D3D_NAME_IS_FRONT_FACE;
    if (!_stricmp(name, "SV_SAMPLEINDEX")) return D3D_NAME_SAMPLE_INDEX;
    if (!_stricmp(name, "SV_CLIPDISTANCE")) return D3D_NAME_CLIP_DISTANCE;
    if (!_stricmp(name, "SV_CULLDISTANCE")) return D3D_NAME_CULL_DISTANCE;
    if (!_stricmp(name, "SV_COVERAGE")) return output ? D3D_NAME_COVERAGE : 0;
    return 0;
}

static unsigned int semantic_index(const char *semantic)
{
    size_t len;

    if (!semantic) return 0;
    for (len = strlen(semantic); len && isdigit((unsigned char)semantic[len - 1]); --len);
    return atoi(semantic + len);
}

static D3D_REGISTER_COMPONENT_TYPE component_type(D3D_SHADER_VARIABLE_TYPE type)
{
    switch (type)
    {
        case D3D_SVT_INT: return D3D_REGISTER_COMPONENT_SINT32;
        case D3D_SVT_UINT: case D3D_SVT_BOOL: return D3D_REGISTER_COMPONENT_UINT32;
        default: return D3D_REGISTER_COMPONENT_FLOAT32;
    }
}

/* ISGN / OSGN */
static BOOL write_signature(struct token_buffer *b, const struct flg_node *node, const unsigned int *regs, const BYTE *used,
        BOOL output, unsigned int shader_type)
{
    unsigned int count = node ? node->param_count : 0, i, string_offset = 8 + 24 * count;
    struct token_buffer strings = {0};

    put_token(b, count);
    put_token(b, 8);
    for (i = 0; i < count; ++i)
    {
        const struct library_parameter *p = &node->params[i];
        const char *semantic = p->semantic ? p->semantic : "";
        unsigned int mask = parameter_mask(p, !output) >> (output ? p->out_comp : p->in_comp), sv;
        char name[64];
        size_t len, k;

        snprintf(name, sizeof(name), "%s", semantic);
        for (len = strlen(name); len && isdigit((unsigned char)name[len - 1]); --len) name[len - 1] = 0;
        sv = system_value(semantic, output, shader_type);
        put_token(b, string_offset + strings.count * 4);
        put_token(b, semantic_index(semantic));
        put_token(b, sv);
        put_token(b, component_type(p->type));
        put_token(b, regs[i]);
        /* the mask and, for an input, the components read; for an output the components never written (none) */
        put_token(b, mask | ((output ? 0 : (used && regs[i] < 32 ? used[regs[i]] & mask : mask)) << 8));
        /* the name, padded to a dword */
        for (k = 0; k <= len; k += 4)
        {
            uint32_t t = 0;
            size_t c;

            for (c = 0; c < 4 && k + c <= len; ++c) t |= (uint32_t)(k + c < len ? (BYTE)name[k + c] : 0) << (8 * c);
            put_token(&strings, t);
        }
    }
    for (i = 0; i < strings.count; ++i) put_token(b, strings.data[i]);
    free(strings.data);
    return !b->failed && !strings.failed;
}

static BOOL parse_target(const char *target, unsigned int *type, unsigned int *major, unsigned int *minor)
{
    static const struct { const char *prefix; unsigned int type; } types[] =
    {
        {"ps_", 0}, {"vs_", 1}, {"gs_", 2}, {"hs_", 3}, {"ds_", 4}, {"cs_", 5},
    };
    unsigned int i;

    if (!target) return FALSE;
    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        if (strncmp(target, types[i].prefix, 3)) continue;
        if (!isdigit((unsigned char)target[3]) || target[4] != '_' || !isdigit((unsigned char)target[5])) return FALSE;
        *type = types[i].type;
        *major = target[3] - '0';
        *minor = target[5] - '0';
        return *major >= 4 && *major <= 5;
    }
    return FALSE;
}

HRESULT d3d11_link(ID3D11ModuleInstance *entry_iface, ID3D11ModuleInstance **library_ifaces, unsigned int library_count,
        const char *target, ID3DBlob **shader, ID3DBlob **error)
{
    struct d3d11_module_instance *entry = unsafe_impl_from_ID3D11ModuleInstance(entry_iface);
    struct vkd3d_shader_dxbc_section_desc sections[3];
    struct token_buffer program = {0}, isgn = {0}, osgn = {0};
    struct vkd3d_shader_code dxbc = {0};
    struct link_context ctx;
    struct flg_graph *graph;
    unsigned int i, j;
    int ret;

    if (error) *error = NULL;
    if (!shader) return E_INVALIDARG;
    *shader = NULL;
    if (!entry || !(graph = entry->graph))
    {
        WARN("The entry instance is not the instance of a function linking graph.\n");
        return E_INVALIDARG;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.graph = graph;
    ctx.library_count = library_count;
    if (!(ctx.libraries = calloc(max(library_count, 1), sizeof(*ctx.libraries)))) return E_OUTOFMEMORY;
    for (i = 0; i < library_count; ++i) ctx.libraries[i] = unsafe_impl_from_ID3D11ModuleInstance(library_ifaces[i]);
    if (!parse_target(target, &ctx.shader_type, &ctx.major, &ctx.minor))
        link_error(&ctx, "error X9204: ID3D11Linker::Link: invalid target '%s'\n", debugstr_a(target));

    /* the registers of the signature parameters: in order (measured; the registers of the descriptions are not used) */
    ctx.input_regs = calloc(max(graph->input ? graph->input->param_count : 0, 1), sizeof(*ctx.input_regs));
    ctx.output_regs = calloc(max(graph->output ? graph->output->param_count : 0, 1), sizeof(*ctx.output_regs));
    if (!ctx.input_regs || !ctx.output_regs) ctx.hr = E_OUTOFMEMORY;
    for (i = 0, j = 0; SUCCEEDED(ctx.hr) && graph->input && i < graph->input->param_count; ++i)
    {
        const struct library_parameter *p = &graph->input->params[i];

        ctx.input_regs[i] = j;
        j += p->class == D3D_SVC_MATRIX_ROWS || p->class == D3D_SVC_MATRIX_COLUMNS ? max(p->rows, 1) : 1;
    }
    for (i = 0, j = 0; SUCCEEDED(ctx.hr) && graph->output && i < graph->output->param_count; ++i)
    {
        const struct library_parameter *p = &graph->output->params[i];
        unsigned int sv = system_value(p->semantic, TRUE, ctx.shader_type);

        /* SV_TargetN is oN */
        ctx.output_regs[i] = sv == D3D_NAME_TARGET ? semantic_index(p->semantic) : j;
        j = max(j, ctx.output_regs[i] + 1);
    }

    if (SUCCEEDED(ctx.hr) && !(ctx.outputs = calloc(max(graph->node_count, 1), sizeof(*ctx.outputs)))) ctx.hr = E_OUTOFMEMORY;
    for (i = 0; SUCCEEDED(ctx.hr) && i < graph->node_count; ++i)
        if (graph->nodes[i]->kind == FLG_NODE_CALL) link_call(&ctx, graph->nodes[i]);
    /* the outputs of the shader */
    for (i = 0; SUCCEEDED(ctx.hr) && graph->output && i < graph->output->param_count; ++i)
    {
        const struct library_parameter *p = &graph->output->params[i];
        uint32_t type = SM4_RT_OUTPUT, reg = ctx.output_regs[i];
        struct library_parameter q = *p;

        /* the parameter as it lands in the register: from component 0 */
        q.in_comp = q.out_comp = 0;
        if (system_value(p->semantic, TRUE, ctx.shader_type) == D3D_NAME_DEPTH)
        {
            type = SM4_RT_DEPTHOUT;
            reg = 0;
        }
        pass_values(&ctx, graph->output, i, type, reg, &q, TRUE, node_output_location);
    }

    if (SUCCEEDED(ctx.hr))
    {
        /* the program: version, length, declarations, the code, ret */
        put_token(&program, (ctx.shader_type << 16) | (ctx.major << 4) | ctx.minor);
        put_token(&program, 0);
        if (ctx.global_flags) put_token(&program, SM4_OP_DCL_GLOBAL_FLAGS | (1u << 24) | (ctx.global_flags << 11));
        for (i = 0; i < ctx.icb.count; ++i) put_token(&program, ctx.icb.data[i]);
        for (i = 0; i < ctx.decls.count; ++i) put_token(&program, ctx.decls.data[i]);
        for (i = 0; graph->input && i < graph->input->param_count; ++i)
        {
            const struct library_parameter *p = &graph->input->params[i];
            unsigned int sv = system_value(p->semantic, FALSE, ctx.shader_type), mask = parameter_mask(p, TRUE) >> p->in_comp;
            unsigned int rows = p->class == D3D_SVC_MATRIX_ROWS || p->class == D3D_SVC_MATRIX_COLUMNS ? max(p->rows, 1) : 1, r;

            for (r = 0; r < rows; ++r)
            {
                if (ctx.shader_type == 0)
                {
                    unsigned int interp = p->interpolation;

                    if (component_type(p->type) != D3D_REGISTER_COMPONENT_FLOAT32) interp = D3D_INTERPOLATION_CONSTANT;
                    else if (interp == D3D_INTERPOLATION_UNDEFINED)
                        interp = sv == D3D_NAME_POSITION ? D3D_INTERPOLATION_LINEAR_NOPERSPECTIVE : SM4_INTERPOLATION_LINEAR;
                    if (sv == D3D_NAME_POSITION)
                    {
                        put_token(&program, SM4_OP_DCL_INPUT_PS_SIV | (4u << 24) | (interp << 11));
                        put_operand_reg(&program, SM4_RT_INPUT, ctx.input_regs[i] + r, mask);
                        put_token(&program, sv);
                    }
                    else if (sv)
                    {
                        put_token(&program, SM4_OP_DCL_INPUT_PS_SGV | (4u << 24));
                        put_operand_reg(&program, SM4_RT_INPUT, ctx.input_regs[i] + r, mask);
                        put_token(&program, sv);
                    }
                    else
                    {
                        put_token(&program, SM4_OP_DCL_INPUT_PS | (3u << 24) | (interp << 11));
                        put_operand_reg(&program, SM4_RT_INPUT, ctx.input_regs[i] + r, mask);
                    }
                }
                else if (sv)
                {
                    put_token(&program, SM4_OP_DCL_INPUT_SGV | (4u << 24));
                    put_operand_reg(&program, SM4_RT_INPUT, ctx.input_regs[i] + r, mask);
                    put_token(&program, sv);
                }
                else
                {
                    put_token(&program, SM4_OP_DCL_INPUT | (3u << 24));
                    put_operand_reg(&program, SM4_RT_INPUT, ctx.input_regs[i] + r, mask);
                }
            }
        }
        for (i = 0; graph->output && i < graph->output->param_count; ++i)
        {
            const struct library_parameter *p = &graph->output->params[i];
            unsigned int sv = system_value(p->semantic, TRUE, ctx.shader_type), mask = parameter_mask(p, FALSE) >> p->out_comp;

            if (sv == D3D_NAME_DEPTH)
            {
                put_token(&program, SM4_OP_DCL_OUTPUT | (2u << 24));
                put_token(&program, 0x0 | (SM4_RT_DEPTHOUT << 12));
            }
            else if (sv && sv != D3D_NAME_TARGET)
            {
                put_token(&program, SM4_OP_DCL_OUTPUT_SIV | (4u << 24));
                put_operand_reg(&program, SM4_RT_OUTPUT, ctx.output_regs[i], mask);
                put_token(&program, sv);
            }
            else
            {
                put_token(&program, SM4_OP_DCL_OUTPUT | (3u << 24));
                put_operand_reg(&program, SM4_RT_OUTPUT, ctx.output_regs[i], mask);
            }
        }
        if (ctx.temp_count)
        {
            put_token(&program, SM4_OP_DCL_TEMPS | (2u << 24));
            put_token(&program, ctx.temp_count);
        }
        for (i = 0; i < ctx.body.count; ++i) put_token(&program, ctx.body.data[i]);
        put_token(&program, SM4_OP_RET | (1u << 24));
        if (program.failed || ctx.decls.failed || ctx.body.failed || ctx.icb.failed) ctx.hr = E_OUTOFMEMORY;
        else program.data[1] = program.count;
    }
    if (SUCCEEDED(ctx.hr))
    {
        if (!write_signature(&isgn, graph->input, ctx.input_regs, ctx.input_used, FALSE, ctx.shader_type)
                || !write_signature(&osgn, graph->output, ctx.output_regs, NULL, TRUE, ctx.shader_type))
            ctx.hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(ctx.hr))
    {
        sections[0].tag = TAG_ISGN;
        sections[0].data.code = isgn.data;
        sections[0].data.size = isgn.count * sizeof(uint32_t);
        sections[1].tag = TAG_OSGN;
        sections[1].data.code = osgn.data;
        sections[1].data.size = osgn.count * sizeof(uint32_t);
        sections[2].tag = TAG_SHEX;
        sections[2].data.code = program.data;
        sections[2].data.size = program.count * sizeof(uint32_t);
        if ((ret = vkd3d_shader_serialize_dxbc(ARRAY_SIZE(sections), sections, &dxbc, NULL)) < 0)
        {
            WARN("Failed to serialise the shader, ret %d.\n", ret);
            ctx.hr = E_FAIL;
        }
        else
        {
            if (SUCCEEDED(ctx.hr = D3DCreateBlob(dxbc.size, shader)))
                memcpy(ID3D10Blob_GetBufferPointer(*shader), dxbc.code, dxbc.size);
            vkd3d_shader_free_shader_code(&dxbc);
            TRACE("Linked %s: %u temps, %Iu tokens of code.\n", debugstr_a(target), ctx.temp_count, program.count);
        }
    }
    if (ctx.messages && error)
    {
        size_t len = strlen(ctx.messages);

        if (SUCCEEDED(D3DCreateBlob(len + 1, error))) memcpy(ID3D10Blob_GetBufferPointer(*error), ctx.messages, len + 1);
    }
    free(ctx.messages);
    free(ctx.outputs);
    free(ctx.libraries);
    free(ctx.input_regs);
    free(ctx.output_regs);
    free(ctx.decls.data);
    free(ctx.icb.data);
    free(ctx.body.data);
    free(program.data);
    free(isgn.data);
    free(osgn.data);
    return ctx.hr;
}

BOOL d3d11_module_instance_is_library(ID3D11ModuleInstance *iface)
{
    struct d3d11_module_instance *instance = unsafe_impl_from_ID3D11ModuleInstance(iface);

    return instance && instance->module;
}
