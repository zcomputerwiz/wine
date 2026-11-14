/*
 * Package resource index (resources.pri) reader
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

/* The layout of the file follows the description in "Package Resource Index File Format" of PriTools
 * (https://github.com/chausner/PriTools, written from analysis of files), checked against files made by MakePri.
 * Every offset is checked against the size of the file: the file comes from a package and is not trusted. */

#include <stdlib.h>
#include "private.h"
#include "winnls.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(model);

const WCHAR *const pri_qualifier_names[PRI_QUALIFIER_TYPES] =
{
    L"Language", L"Contrast", L"Scale", L"HomeRegion", L"TargetSize", L"LayoutDirection", L"Theme", L"AlternateForm",
    L"DXFeatureLevel", L"Configuration", L"DeviceFamily", L"Custom",
};

enum value_type { VALUE_STRING, VALUE_PATH, VALUE_EMBEDDED, VALUE_ASCII_STRING, VALUE_UTF8_STRING, VALUE_ASCII_PATH, VALUE_UTF8_PATH };

struct reader
{
    const BYTE *data;
    UINT32 size;
};

static BOOL in_file( const struct reader *r, UINT64 offset, UINT64 length )
{
    return offset <= r->size && length <= r->size - offset;
}

static UINT16 get16( const struct reader *r, UINT64 offset )
{
    return in_file( r, offset, 2 ) ? r->data[offset] | (r->data[offset + 1] << 8) : 0;
}

static UINT32 get32( const struct reader *r, UINT64 offset )
{
    return in_file( r, offset, 4 ) ? get16( r, offset ) | ((UINT32)get16( r, offset + 2 ) << 16) : 0;
}

struct section
{
    char id[17];
    UINT32 offset; /* of the section data, after its header */
    UINT32 length;
};

/* a string of the file: UTF-16 or 8-bit, without the zeros at its end */
static WCHAR *read_string( const struct reader *r, UINT64 offset, UINT32 bytes, BOOL wide, BOOL utf8 )
{
    WCHAR *text;
    UINT32 i, count;

    if (!in_file( r, offset, bytes )) return NULL;
    if (wide)
    {
        count = bytes / 2;
        if (!(text = malloc( (count + 1) * sizeof(WCHAR) ))) return NULL;
        for (i = 0; i < count; i++) text[i] = get16( r, offset + 2 * i );
    }
    else if (utf8)
    {
        count = bytes ? MultiByteToWideChar( CP_UTF8, 0, (const char *)r->data + offset, bytes, NULL, 0 ) : 0;
        if (!(text = malloc( (count + 1) * sizeof(WCHAR) ))) return NULL;
        if (count) MultiByteToWideChar( CP_UTF8, 0, (const char *)r->data + offset, bytes, text, count );
    }
    else
    {
        count = bytes;
        if (!(text = malloc( (count + 1) * sizeof(WCHAR) ))) return NULL;
        for (i = 0; i < count; i++) text[i] = r->data[offset + i];
    }
    text[count] = 0;
    while (count && !text[count - 1]) text[--count] = 0;
    return text;
}

/* a zero-terminated string of a name block */
static WCHAR *read_terminated( const struct reader *r, UINT64 offset, UINT64 end, BOOL wide )
{
    UINT64 p = offset;

    if (wide) while (p + 2 <= end && in_file( r, p, 2 ) && get16( r, p )) p += 2;
    else while (p < end && in_file( r, p, 1 ) && r->data[p]) p++;
    return read_string( r, offset, p - offset, wide, FALSE );
}

static int find_section( const struct section *sections, UINT32 count, const char *prefix, int from )
{
    UINT32 i;
    for (i = from; i < count; i++) if (!strncmp( sections[i].id, prefix, strlen( prefix ) )) return i;
    return -1;
}

static void pri_free_parts( struct pri *pri )
{
    UINT32 i;

    for (i = 0; i < pri->item_count; i++) free( pri->items[i].name );
    for (i = 0; i < pri->candidate_count; i++) free( pri->candidates[i].value );
    for (i = 0; i < pri->qualifier_count; i++) free( pri->qualifiers[i].value );
    free( pri->items );
    free( pri->candidates );
    free( pri->qualifiers );
    free( pri->set_qualifiers );
    free( pri->root );
    free( pri->name );
}

void pri_free( struct pri *pri )
{
    if (!pri) return;
    pri_free_parts( pri );
    free( pri );
}

/* the names of the items of a hierarchical schema section, by the index of the item */
static HRESULT read_schema( const struct reader *r, const struct section *s, struct pri *pri, WCHAR ***names, UINT32 *count )
{
    BOOL extended = !strncmp( s->id, "[mrm_hschemaex]", 15 );
    UINT64 p = s->offset, end = (UINT64)s->offset + s->length, entries, unicode, ascii;
    UINT32 unique_length, name_length, total, scopes, items, unicode_length, ascii_length = 0, i;
    BOOL hnamesx = FALSE;
    struct entry { UINT16 parent; BOOL scope; UINT16 index; WCHAR *name; } *table;

    *names = NULL;
    *count = 0;
    if (s->length < 8) return S_OK; /* an empty schema */
    unique_length = get16( r, p + 2 );
    name_length = get16( r, p + 4 );
    if (extended)
    {
        hnamesx = in_file( r, p + 8, 13 ) && !memcmp( r->data + p + 8, "[def_hnamesx]", 13 );
        p += 16;
    }
    p += 8 + 20;
    free( pri->name );
    pri->name = read_string( r, p + 2 * unique_length, 2 * name_length, TRUE, FALSE );
    p += 2 * (UINT64)(unique_length + name_length) + 6;
    total = get32( r, p );
    scopes = get32( r, p + 4 );
    items = get32( r, p + 8 );
    unicode_length = get32( r, p + 12 );
    p += 20;
    if (hnamesx) ascii_length = get32( r, p ), p += 4;
    if (total > 0xffff || items > total || scopes > total) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );

    entries = p;
    unicode = entries + 12 * (UINT64)total + 8 * (UINT64)scopes + 2 * (UINT64)items;
    ascii = unicode + 2 * (UINT64)unicode_length;
    if (ascii + ascii_length > end || !in_file( r, entries, ascii + ascii_length - entries ))
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );

    if (!(table = calloc( total ? total : 1, sizeof(*table) ))) return E_OUTOFMEMORY;
    for (i = 0; i < total; i++)
    {
        UINT64 e = entries + 12 * (UINT64)i;
        BYTE length = r->data[e + 6], flags = r->data[e + 7];
        UINT32 offset = get16( r, e + 8 ) | ((UINT32)(flags & 0xf) << 16);

        table[i].parent = get16( r, e );
        table[i].scope = !!(flags & 0x10);
        table[i].index = get16( r, e + 10 );
        if (flags & 0x20)
            table[i].name = length ? read_string( r, ascii + offset, length, FALSE, FALSE ) : read_terminated( r, ascii + offset, ascii + ascii_length, FALSE );
        else
            table[i].name = length ? read_string( r, unicode + 2 * (UINT64)offset, 2 * length, TRUE, FALSE )
                                   : read_terminated( r, unicode + 2 * (UINT64)offset, ascii, TRUE );
        if (!table[i].name) table[i].name = calloc( 1, sizeof(WCHAR) );
    }

    if (!(*names = calloc( items ? items : 1, sizeof(**names) )))
    {
        for (i = 0; i < total; i++) free( table[i].name );
        free( table );
        return E_OUTOFMEMORY;
    }
    *count = items;
    for (i = 0; i < total; i++)
    {
        WCHAR path[1024];
        UINT32 k = i, depth = 0, length = 0, chain[64], j;

        if (table[i].scope || table[i].index >= items) continue;
        /* the names from the root (entry 0, not part of the path) down to the item */
        while (k && depth < ARRAY_SIZE(chain) && table[k].parent != k && table[k].parent < total)
        {
            chain[depth++] = k;
            k = table[k].parent;
        }
        path[0] = 0;
        for (j = depth; j-- > 0;)
        {
            size_t n = wcslen( table[chain[j]].name );
            if (!n) continue;
            if (length + n + 2 >= ARRAY_SIZE(path)) break;
            if (length) path[length++] = '/';
            memcpy( path + length, table[chain[j]].name, n * sizeof(WCHAR) );
            length += n;
            path[length] = 0;
        }
        free( (*names)[table[i].index] );
        (*names)[table[i].index] = wcsdup( path );
    }
    for (i = 0; i < total; i++) free( table[i].name );
    free( table );
    return S_OK;
}

/* the qualifiers and qualifier sets of the decision info section */
struct decisions
{
    UINT32 decision_count, set_count, index_count;
    UINT64 decisions, sets, index;
};

static HRESULT read_decisions( const struct reader *r, const struct section *s, struct pri *pri, struct decisions *d )
{
    UINT64 p = s->offset, qualifiers, distinct, values;
    UINT32 distinct_count = get16( r, p ), qualifier_count = get16( r, p + 2 ), value_length = get16( r, p + 10 ), i;

    d->set_count = get16( r, p + 4 );
    d->decision_count = get16( r, p + 6 );
    d->index_count = get16( r, p + 8 );
    d->decisions = p + 12;
    d->sets = d->decisions + 4 * (UINT64)d->decision_count;
    qualifiers = d->sets + 4 * (UINT64)d->set_count;
    distinct = qualifiers + 8 * (UINT64)qualifier_count;
    d->index = distinct + 12 * (UINT64)distinct_count;
    values = d->index + 2 * (UINT64)d->index_count;
    if (values + 2 * (UINT64)value_length > (UINT64)s->offset + s->length || !in_file( r, values, 2 * (UINT64)value_length ))
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );

    if (!(pri->qualifiers = calloc( qualifier_count ? qualifier_count : 1, sizeof(*pri->qualifiers) ))) return E_OUTOFMEMORY;
    pri->qualifier_count = qualifier_count;
    for (i = 0; i < qualifier_count; i++)
    {
        UINT64 q = qualifiers + 8 * (UINT64)i, dq;
        UINT32 which = get16( r, q ), offset;

        pri->qualifiers[i].priority = get16( r, q + 2 );
        pri->qualifiers[i].fallback = get16( r, q + 4 );
        if (which >= distinct_count) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        dq = distinct + 12 * (UINT64)which;
        pri->qualifiers[i].type = get16( r, dq + 2 );
        if (pri->qualifiers[i].type >= PRI_QUALIFIER_TYPES) pri->qualifiers[i].type = PRI_QUALIFIER_TYPES - 1;
        offset = get32( r, dq + 8 );
        if (offset > value_length) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
        pri->qualifiers[i].value = read_terminated( r, values + 2 * (UINT64)offset, values + 2 * (UINT64)value_length, TRUE );
        if (!pri->qualifiers[i].value) return E_OUTOFMEMORY;
    }
    return S_OK;
}

/* the bytes of a data item */
static BOOL data_item( const struct reader *r, const struct section *s, UINT32 index, UINT64 *offset, UINT32 *length )
{
    UINT64 p = s->offset, data;
    UINT32 strings = get16( r, p + 4 ), blobs = get16( r, p + 6 ), total = get32( r, p + 8 );

    data = p + 12 + 4 * (UINT64)strings + 8 * (UINT64)blobs;
    if (index < strings)
    {
        *offset = data + get16( r, p + 12 + 4 * (UINT64)index );
        *length = get16( r, p + 12 + 4 * (UINT64)index + 2 );
    }
    else if (index - strings < blobs)
    {
        UINT64 b = p + 12 + 4 * (UINT64)strings + 8 * (UINT64)(index - strings);
        *offset = data + get32( r, b );
        *length = get32( r, b + 4 );
    }
    else return FALSE;
    return *offset + *length <= data + total && in_file( r, *offset, *length );
}

static WCHAR *decode_value( const struct reader *r, UINT32 type, UINT64 offset, UINT32 length, int *kind )
{
    switch (type)
    {
    case VALUE_STRING: *kind = PRI_KIND_STRING; return read_string( r, offset, length, TRUE, FALSE );
    case VALUE_PATH: *kind = PRI_KIND_FILE; return read_string( r, offset, length, TRUE, FALSE );
    case VALUE_ASCII_STRING: *kind = PRI_KIND_STRING; return read_string( r, offset, length, FALSE, FALSE );
    case VALUE_UTF8_STRING: *kind = PRI_KIND_STRING; return read_string( r, offset, length, FALSE, TRUE );
    case VALUE_ASCII_PATH: *kind = PRI_KIND_FILE; return read_string( r, offset, length, FALSE, FALSE );
    case VALUE_UTF8_PATH: *kind = PRI_KIND_FILE; return read_string( r, offset, length, FALSE, TRUE );
    default: *kind = PRI_KIND_EMBEDDED; return calloc( 1, sizeof(WCHAR) );
    }
}

static HRESULT read_map( const struct reader *r, const struct section *sections, UINT32 section_count, const struct section *s,
                         struct pri *pri, WCHAR **names, UINT32 name_count, const struct decisions *d )
{
    UINT64 p = s->offset, types, item_groups, groups, infos, candidates, embedded;
    UINT32 env_length = get16( r, p ), schema_ref_length = get16( r, p + 6 ), type_count = get16( r, p + 10 );
    UINT32 item_group_count = get16( r, p + 12 ), group_count = get16( r, p + 14 ), info_count = get32( r, p + 16 );
    UINT32 candidate_count = get32( r, p + 20 ), embedded_length = get32( r, p + 24 ), extension_length = get32( r, p + 28 );
    UINT32 *info_of_item, i, j, done = 0, sets_used = 0;

    types = p + 32 + env_length + schema_ref_length;
    item_groups = types + 8 * (UINT64)type_count;
    groups = item_groups + 4 * (UINT64)item_group_count;
    infos = groups + 4 * (UINT64)group_count;
    candidates = infos + 4 * (UINT64)info_count + extension_length;
    embedded = candidates + 8 * (UINT64)candidate_count;
    if (embedded + embedded_length > (UINT64)s->offset + s->length || !in_file( r, types, embedded + embedded_length - types ))
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    if (extension_length) FIXME( "table extension block not supported\n" );

    if (!(info_of_item = malloc( (name_count ? name_count : 1) * sizeof(*info_of_item) ))) return E_OUTOFMEMORY;
    for (i = 0; i < name_count; i++) info_of_item[i] = ~0u;
    for (i = 0; i < item_group_count; i++)
    {
        UINT32 first = get16( r, item_groups + 4 * (UINT64)i ), group = get16( r, item_groups + 4 * (UINT64)i + 2 ), count, info;

        if (group >= group_count) count = 1, info = group - group_count;
        else
        {
            count = get16( r, groups + 4 * (UINT64)group );
            info = get16( r, groups + 4 * (UINT64)group + 2 );
        }
        for (j = 0; j < count && first + j < name_count; j++) info_of_item[first + j] = info + j;
    }

    pri->items = calloc( name_count ? name_count : 1, sizeof(*pri->items) );
    pri->candidates = calloc( candidate_count ? candidate_count : 1, sizeof(*pri->candidates) );
    pri->set_qualifiers = calloc( d->index_count ? d->index_count : 1, sizeof(*pri->set_qualifiers) );
    if (!pri->items || !pri->candidates || !pri->set_qualifiers)
    {
        free( info_of_item );
        return E_OUTOFMEMORY;
    }

    for (i = 0; i < name_count; i++)
    {
        struct pri_item *item = &pri->items[pri->item_count];
        UINT32 info = info_of_item[i], decision, first, set_first, set_count;

        if (!names[i] || info >= info_count) continue;
        decision = get16( r, infos + 4 * (UINT64)info );
        first = get16( r, infos + 4 * (UINT64)info + 2 );
        if (decision >= d->decision_count) continue;
        set_first = get16( r, d->decisions + 4 * (UINT64)decision );
        set_count = get16( r, d->decisions + 4 * (UINT64)decision + 2 );
        if ((UINT64)first + set_count > candidate_count || (UINT64)set_first + set_count > d->index_count) continue;

        item->name = names[i];
        names[i] = NULL;
        item->first = done;
        for (j = 0; j < set_count; j++)
        {
            struct pri_candidate *candidate = &pri->candidates[done];
            UINT64 c = candidates + 8 * (UINT64)(first + j), offset = 0;
            UINT32 set = get16( r, d->index + 2 * (UINT64)(set_first + j) ), type_index = r->data[c + 1], type, length = 0;
            UINT32 q_first, q_count, k;
            BOOL found = FALSE;

            if (type_index >= type_count) continue;
            type = get32( r, types + 8 * (UINT64)type_index + 4 );
            if (r->data[c] == 0) /* in the embedded data block */
            {
                length = get16( r, c + 2 );
                offset = embedded + get32( r, c + 4 );
                found = offset + length <= embedded + embedded_length;
            }
            else if (!get16( r, c + 2 )) /* a data item of this file */
            {
                UINT32 section = get16( r, c + 6 );
                if (section < section_count && !strncmp( sections[section].id, "[mrm_dataitem]", 14 ))
                    found = data_item( r, &sections[section], get16( r, c + 4 ), &offset, &length );
            }
            else FIXME( "candidate in another resource file not supported\n" );
            if (!found) continue;

            if (!(candidate->value = decode_value( r, type, offset, length, &candidate->kind ))) continue;
            /* the qualifiers of the set, kept as indexes into pri->qualifiers */
            if (set >= d->set_count) q_first = q_count = 0;
            else
            {
                q_first = get16( r, d->sets + 4 * (UINT64)set );
                q_count = get16( r, d->sets + 4 * (UINT64)set + 2 );
                if ((UINT64)q_first + q_count > d->index_count) q_first = q_count = 0;
            }
            candidate->qualifiers = &pri->set_qualifiers[sets_used];
            for (k = 0; k < q_count && sets_used < d->index_count; k++)
            {
                UINT32 q = get16( r, d->index + 2 * (UINT64)(q_first + k) );
                if (q >= pri->qualifier_count) continue;
                pri->set_qualifiers[sets_used++] = q;
                candidate->qualifier_count++;
            }
            done++;
            item->count++;
        }
        pri->item_count++;
    }
    pri->candidate_count = done;
    free( info_of_item );
    return S_OK;
}

HRESULT pri_load( const WCHAR *root, struct pri **out )
{
    struct section *sections = NULL;
    struct decisions decisions = {0};
    struct reader r = {0};
    WCHAR **names = NULL, *path;
    UINT32 name_count = 0, count, first, toc, i;
    struct pri *pri;
    LARGE_INTEGER size;
    HANDLE file;
    DWORD got;
    HRESULT hr;
    int index;

    *out = NULL;
    if (!(path = malloc( (wcslen( root ) + 16) * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    swprintf( path, wcslen( root ) + 16, L"%s\\resources.pri", root );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    free( path );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > 0x10000000 || size.QuadPart < 32 ||
        !(r.data = malloc( size.QuadPart )) || !ReadFile( file, (BYTE *)r.data, size.QuadPart, &got, NULL ) || got != size.QuadPart)
    {
        CloseHandle( file );
        free( (BYTE *)r.data );
        return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    }
    CloseHandle( file );
    r.size = size.QuadPart;

    hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    if (!(pri = calloc( 1, sizeof(*pri) ))) { hr = E_OUTOFMEMORY; goto done; }
    if (memcmp( r.data, "mrm_pri", 7 )) goto done;
    toc = get32( &r, 16 );
    first = get32( &r, 20 );
    count = get16( &r, 24 );
    if (!count || !in_file( &r, toc, 32 * (UINT64)count )) goto done;
    if (!(sections = calloc( count, sizeof(*sections) ))) { hr = E_OUTOFMEMORY; goto done; }
    for (i = 0; i < count; i++)
    {
        UINT64 start = (UINT64)first + get32( &r, toc + 32 * (UINT64)i + 24 );
        UINT32 length = get32( &r, toc + 32 * (UINT64)i + 28 );

        memcpy( sections[i].id, r.data + toc + 32 * (UINT64)i, 16 );
        if (length < 40 || !in_file( &r, start, length )) goto done;
        sections[i].offset = start + 32;
        sections[i].length = length - 40; /* without the header and the footer */
    }

    if ((index = find_section( sections, count, "[mrm_hschema", 0 )) < 0) goto done;
    if (FAILED(hr = read_schema( &r, &sections[index], pri, &names, &name_count ))) goto done;
    hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    if ((index = find_section( sections, count, "[mrm_decn_info]", 0 )) < 0) goto done;
    if (FAILED(hr = read_decisions( &r, &sections[index], pri, &decisions ))) goto done;
    hr = HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    if ((index = find_section( sections, count, "[mrm_res_map", 0 )) < 0) goto done;
    if (FAILED(hr = read_map( &r, sections, count, &sections[index], pri, names, name_count, &decisions ))) goto done;
    if (!(pri->root = wcsdup( root ))) { hr = E_OUTOFMEMORY; goto done; }
    if (!pri->name) pri->name = calloc( 1, sizeof(WCHAR) );
    TRACE( "%s: %u items, %u candidates\n", debugstr_w( root ), pri->item_count, pri->candidate_count );
    hr = S_OK;

done:
    if (names) for (i = 0; i < name_count; i++) free( names[i] );
    free( names );
    free( sections );
    free( (BYTE *)r.data );
    if (FAILED(hr))
    {
        WARN( "cannot read the resources of %s, hr %#lx\n", debugstr_w( root ), hr );
        pri_free( pri );
        return hr;
    }
    *out = pri;
    return S_OK;
}

const struct pri_item *pri_find( const struct pri *pri, const WCHAR *name, UINT32 length )
{
    UINT32 i;

    for (i = 0; i < pri->item_count; i++)
        if (wcslen( pri->items[i].name ) == length && CompareStringOrdinal( pri->items[i].name, length, name, length, TRUE ) == CSTR_EQUAL)
            return &pri->items[i];
    return NULL;
}

/* ---- choosing a candidate -------------------------------------------------------------------------------- */

/* the part of a language tag before the first '-' */
static size_t primary_length( const WCHAR *tag, size_t length )
{
    size_t i;
    for (i = 0; i < length; i++) if (tag[i] == '-') return i;
    return length;
}

/* how well a qualifier of a candidate matches the context: a negative score is no match */
static double qualifier_score( const struct pri_qualifier *q, const WCHAR *const *context )
{
    const WCHAR *wanted = context[q->type] ? context[q->type] : L"";

    if (q->type == PRI_QUALIFIER_LANGUAGE)
    {
        const WCHAR *p = wanted, *end;
        size_t value_length = wcslen( q->value ), value_primary = primary_length( q->value, value_length );
        unsigned int position = 0;

        while (*p)
        {
            size_t length;
            end = p;
            while (*end && *end != ';' && *end != ',') end++;
            length = end - p;
            if (length == value_length && !wcsnicmp( p, q->value, length )) return 1.0 - position * 0.01;
            if (primary_length( p, length ) == value_primary && !wcsnicmp( p, q->value, value_primary )) return 0.9 - position * 0.01;
            position++;
            p = *end ? end + 1 : end;
        }
        return -1;
    }
    if (q->type == PRI_QUALIFIER_SCALE && *wanted)
    {
        double scale = _wtoi( wanted ), value = _wtoi( q->value );
        /* the smallest scale that is not smaller than the wanted one, else the biggest */
        if (value >= scale) return 1.0 - (value - scale) / 10000.0;
        return value / 10000.0 - 1.0;
    }
    if (*wanted && !wcsicmp( wanted, q->value )) return 1.0;
    return -2;
}

/* compares two candidates for a context: the qualifier types are taken by priority; for each, a match beats no
 * match, then the better score wins; a candidate without the qualifier counts as a weak match */
static int compare_candidates( const struct pri *pri, const struct pri_candidate *a, const struct pri_candidate *b,
                               const WCHAR *const *context )
{
    static const UINT16 default_priority[PRI_QUALIFIER_TYPES] = { 700, 500, 200, 300, 200, 400, 400, 300, 200, 200, 600, 100 };
    UINT16 priorities[PRI_QUALIFIER_TYPES];
    int order[PRI_QUALIFIER_TYPES], i, j, t;

    for (t = 0; t < PRI_QUALIFIER_TYPES; t++) priorities[t] = default_priority[t], order[t] = t;
    for (i = 0; i < a->qualifier_count; i++) priorities[pri->qualifiers[a->qualifiers[i]].type] = pri->qualifiers[a->qualifiers[i]].priority;
    for (i = 0; i < PRI_QUALIFIER_TYPES; i++)
        for (j = i + 1; j < PRI_QUALIFIER_TYPES; j++)
            if (priorities[order[j]] > priorities[order[i]]) t = order[i], order[i] = order[j], order[j] = t;

    for (i = 0; i < PRI_QUALIFIER_TYPES; i++)
    {
        double score[2];
        int matched[2], side;
        const struct pri_candidate *c[2] = { a, b };

        for (side = 0; side < 2; side++)
        {
            matched[side] = 1;
            score[side] = 0.0; /* no qualifier of this type: a weak match */
            for (j = 0; j < c[side]->qualifier_count; j++)
            {
                const struct pri_qualifier *q = &pri->qualifiers[c[side]->qualifiers[j]];
                double s;

                if (q->type != order[i]) continue;
                if ((s = qualifier_score( q, context )) >= -1.0 && (q->type == PRI_QUALIFIER_SCALE || s >= 0)) score[side] = s + 1.0;
                else
                {
                    matched[side] = 0;
                    score[side] = q->fallback / 1000.0;
                }
            }
        }
        if (matched[0] != matched[1]) return matched[0] ? -1 : 1;
        if (score[0] != score[1]) return score[0] > score[1] ? -1 : 1;
    }
    return 0;
}

/* the candidates of an item, best first (stable) */
void pri_order( const struct pri *pri, const struct pri_item *item, const WCHAR *const *context, UINT32 *order )
{
    UINT32 i, j, t;

    for (i = 0; i < item->count; i++) order[i] = item->first + i;
    for (i = 1; i < item->count; i++)
        for (j = i; j > 0 && compare_candidates( pri, &pri->candidates[order[j]], &pri->candidates[order[j - 1]], context ) < 0; j--)
            t = order[j], order[j] = order[j - 1], order[j - 1] = t;
}

BOOL pri_candidate_matches( const struct pri *pri, const struct pri_candidate *candidate, const WCHAR *const *context )
{
    UINT32 i;
    for (i = 0; i < candidate->qualifier_count; i++)
    {
        const struct pri_qualifier *q = &pri->qualifiers[candidate->qualifiers[i]];
        double s = qualifier_score( q, context );
        if (s < -1.0 || (s < 0 && q->type != PRI_QUALIFIER_SCALE)) return FALSE;
    }
    return TRUE;
}
