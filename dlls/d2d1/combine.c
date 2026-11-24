/*
 * Boolean operations on geometries (ID2D1Geometry::CombineWithGeometry)
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

/* Both geometries are flattened to polygons (Simplify with lines), their points snapped to a grid of 1/256 unit, so that the
 * computations on edges are exact in integers (the approach of Clipper2, Boost licence, which served as the reference for the
 * semantics; the algorithm here is a different one). Every edge is split where it meets another one; each piece is classified
 * by testing on both of its sides whether the point is inside each geometry, under the fill mode of that geometry; a piece is
 * an edge of the result when the combine mode gives different results on its two sides. The pieces are then linked into closed
 * figures with the filled side on the left of the direction of travel (in the y-down coordinates of Direct2D: outlines clockwise
 * on screen, holes counter-clockwise), as Windows writes them.
 *
 * Output as on Windows 11 (measured, WineExplorer tests/uwp/alpc/d2d-combine-probe.cpp): fill mode ALTERNATE, filled closed
 * figures, each line in its own AddLines call and the start point repeated at the end, a figure starting at its top-most then
 * left-most point, figures in descending order of that point, no Close. Windows keeps Bézier segments (split at intersections);
 * here curves come out flattened with the given tolerance. Hollow figures do not take part, open ones are closed. */

#include "d2d1_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

#define COMBINE_SCALE 256.0

struct combine_point
{
    INT64 x, y;
};

struct combine_edge
{
    struct combine_point a, b;
};

struct combine_shape
{
    struct combine_edge *edges;
    size_t count, size;
    D2D1_FILL_MODE fill_mode;
};

/* collects the flattened figures of a geometry as edges */
struct combine_sink
{
    ID2D1SimplifiedGeometrySink ID2D1SimplifiedGeometrySink_iface;
    struct combine_shape *shape;
    struct combine_point first, last;
    BOOL in_figure, hollow, failed;
};

static inline struct combine_sink *impl_from_ID2D1SimplifiedGeometrySink(ID2D1SimplifiedGeometrySink *iface)
{
    return CONTAINING_RECORD(iface, struct combine_sink, ID2D1SimplifiedGeometrySink_iface);
}

static struct combine_point combine_snap(D2D1_POINT_2F p)
{
    struct combine_point ret;

    ret.x = llround(p.x * COMBINE_SCALE);
    ret.y = llround(p.y * COMBINE_SCALE);
    return ret;
}

static BOOL combine_point_equal(const struct combine_point *a, const struct combine_point *b)
{
    return a->x == b->x && a->y == b->y;
}

static BOOL combine_shape_add_edge(struct combine_shape *shape, const struct combine_point *a, const struct combine_point *b)
{
    if (combine_point_equal(a, b)) return TRUE;
    if (!d2d_array_reserve((void **)&shape->edges, &shape->size, shape->count + 1, sizeof(*shape->edges))) return FALSE;
    shape->edges[shape->count].a = *a;
    shape->edges[shape->count].b = *b;
    ++shape->count;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE combine_sink_QueryInterface(ID2D1SimplifiedGeometrySink *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_ID2D1SimplifiedGeometrySink) || IsEqualGUID(iid, &IID_IUnknown))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE combine_sink_AddRef(ID2D1SimplifiedGeometrySink *iface)
{
    return 2;
}

static ULONG STDMETHODCALLTYPE combine_sink_Release(ID2D1SimplifiedGeometrySink *iface)
{
    return 1;
}

static void STDMETHODCALLTYPE combine_sink_SetFillMode(ID2D1SimplifiedGeometrySink *iface, D2D1_FILL_MODE mode)
{
    impl_from_ID2D1SimplifiedGeometrySink(iface)->shape->fill_mode = mode;
}

static void STDMETHODCALLTYPE combine_sink_SetSegmentFlags(ID2D1SimplifiedGeometrySink *iface, D2D1_PATH_SEGMENT flags)
{
}

static void STDMETHODCALLTYPE combine_sink_BeginFigure(ID2D1SimplifiedGeometrySink *iface, D2D1_POINT_2F p,
        D2D1_FIGURE_BEGIN begin)
{
    struct combine_sink *sink = impl_from_ID2D1SimplifiedGeometrySink(iface);

    sink->first = sink->last = combine_snap(p);
    sink->in_figure = TRUE;
    sink->hollow = begin == D2D1_FIGURE_BEGIN_HOLLOW;
}

static void STDMETHODCALLTYPE combine_sink_AddLines(ID2D1SimplifiedGeometrySink *iface, const D2D1_POINT_2F *points,
        UINT32 count)
{
    struct combine_sink *sink = impl_from_ID2D1SimplifiedGeometrySink(iface);
    struct combine_point p;
    UINT32 i;

    if (!sink->in_figure) return;
    for (i = 0; i < count; ++i)
    {
        p = combine_snap(points[i]);
        if (!sink->hollow && !combine_shape_add_edge(sink->shape, &sink->last, &p)) sink->failed = TRUE;
        sink->last = p;
    }
}

static void STDMETHODCALLTYPE combine_sink_AddBeziers(ID2D1SimplifiedGeometrySink *iface,
        const D2D1_BEZIER_SEGMENT *beziers, UINT32 count)
{
    UINT32 i;

    /* not written by Simplify with D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES; the end points keep the figure closed */
    WARN("Unexpected Bézier segments.\n");
    for (i = 0; i < count; ++i)
        combine_sink_AddLines(iface, &beziers[i].point3, 1);
}

static void STDMETHODCALLTYPE combine_sink_EndFigure(ID2D1SimplifiedGeometrySink *iface, D2D1_FIGURE_END end)
{
    struct combine_sink *sink = impl_from_ID2D1SimplifiedGeometrySink(iface);

    /* an open figure is filled as if it were closed */
    if (sink->in_figure && !sink->hollow && !combine_shape_add_edge(sink->shape, &sink->last, &sink->first))
        sink->failed = TRUE;
    sink->in_figure = FALSE;
}

static HRESULT STDMETHODCALLTYPE combine_sink_Close(ID2D1SimplifiedGeometrySink *iface)
{
    return S_OK;
}

static const ID2D1SimplifiedGeometrySinkVtbl combine_sink_vtbl =
{
    combine_sink_QueryInterface,
    combine_sink_AddRef,
    combine_sink_Release,
    combine_sink_SetFillMode,
    combine_sink_SetSegmentFlags,
    combine_sink_BeginFigure,
    combine_sink_AddLines,
    combine_sink_AddBeziers,
    combine_sink_EndFigure,
    combine_sink_Close,
};

static HRESULT combine_collect(ID2D1Geometry *geometry, const D2D1_MATRIX_3X2_F *transform, float tolerance,
        struct combine_shape *shape)
{
    struct combine_sink sink = {{&combine_sink_vtbl}};
    HRESULT hr;

    shape->fill_mode = D2D1_FILL_MODE_ALTERNATE;
    sink.shape = shape;
    hr = ID2D1Geometry_Simplify(geometry, D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES, transform, tolerance,
            &sink.ID2D1SimplifiedGeometrySink_iface);
    if (SUCCEEDED(hr) && sink.failed) hr = E_OUTOFMEMORY;
    return hr;
}

/* Inside test of a point given in real coordinates (doubles in grid units), by the fill mode of the shape. */
static BOOL combine_shape_contains(const struct combine_shape *shape, double x, double y)
{
    int winding = 0, crossings = 0;
    size_t i;

    for (i = 0; i < shape->count; ++i)
    {
        double ax = shape->edges[i].a.x, ay = shape->edges[i].a.y;
        double bx = shape->edges[i].b.x, by = shape->edges[i].b.y;
        double cross;

        if ((ay <= y) == (by <= y)) continue;   /* the edge does not span the horizontal line through the point */
        cross = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        if (by > ay)
        {
            if (cross > 0.0) { ++winding; ++crossings; }
        }
        else
        {
            if (cross < 0.0) { --winding; ++crossings; }
        }
    }
    return shape->fill_mode == D2D1_FILL_MODE_WINDING ? winding != 0 : (crossings & 1);
}

static BOOL combine_result(D2D1_COMBINE_MODE mode, BOOL a, BOOL b)
{
    switch (mode)
    {
        case D2D1_COMBINE_MODE_UNION:     return a || b;
        case D2D1_COMBINE_MODE_INTERSECT: return a && b;
        case D2D1_COMBINE_MODE_XOR:       return a != b;
        case D2D1_COMBINE_MODE_EXCLUDE:   return a && !b;
        default:                          return FALSE;
    }
}

/* the points at which an edge is split, as distances along it (any monotonic measure) with the point */
struct combine_split
{
    double t;
    struct combine_point p;
};

struct combine_splits
{
    struct combine_split *s;
    size_t count, size;
};

static BOOL combine_add_split(struct combine_splits *splits, const struct combine_edge *e, const struct combine_point *p)
{
    double dx = (double)(e->b.x - e->a.x), dy = (double)(e->b.y - e->a.y);

    if (combine_point_equal(p, &e->a) || combine_point_equal(p, &e->b)) return TRUE;
    if (!d2d_array_reserve((void **)&splits->s, &splits->size, splits->count + 1, sizeof(*splits->s))) return FALSE;
    splits->s[splits->count].t = ((p->x - e->a.x) * dx + (p->y - e->a.y) * dy) / (dx * dx + dy * dy);
    splits->s[splits->count].p = *p;
    ++splits->count;
    return TRUE;
}

static INT64 combine_cross(const struct combine_point *o, const struct combine_point *a, const struct combine_point *b)
{
    return (a->x - o->x) * (b->y - o->y) - (a->y - o->y) * (b->x - o->x);
}

/* whether p, known to be on the line of e, lies strictly between its end points */
static BOOL combine_on_segment(const struct combine_edge *e, const struct combine_point *p)
{
    if (combine_point_equal(p, &e->a) || combine_point_equal(p, &e->b)) return FALSE;
    return min(e->a.x, e->b.x) <= p->x && p->x <= max(e->a.x, e->b.x)
            && min(e->a.y, e->b.y) <= p->y && p->y <= max(e->a.y, e->b.y);
}

/* records where the two edges meet: a crossing, a touching end point, or the end points of an overlap */
static BOOL combine_intersect(const struct combine_edge *e1, struct combine_splits *s1,
        const struct combine_edge *e2, struct combine_splits *s2)
{
    INT64 d1 = combine_cross(&e1->a, &e1->b, &e2->a), d2 = combine_cross(&e1->a, &e1->b, &e2->b);
    INT64 d3 = combine_cross(&e2->a, &e2->b, &e1->a), d4 = combine_cross(&e2->a, &e2->b, &e1->b);
    BOOL ok = TRUE;

    if (max(e1->a.x, e1->b.x) < min(e2->a.x, e2->b.x) || max(e2->a.x, e2->b.x) < min(e1->a.x, e1->b.x)
            || max(e1->a.y, e1->b.y) < min(e2->a.y, e2->b.y) || max(e2->a.y, e2->b.y) < min(e1->a.y, e1->b.y))
        return TRUE;

    if (!d1 && !d2)
    {
        /* collinear: each end point inside the other edge splits it */
        if (combine_on_segment(e1, &e2->a)) ok &= combine_add_split(s1, e1, &e2->a);
        if (combine_on_segment(e1, &e2->b)) ok &= combine_add_split(s1, e1, &e2->b);
        if (combine_on_segment(e2, &e1->a)) ok &= combine_add_split(s2, e2, &e1->a);
        if (combine_on_segment(e2, &e1->b)) ok &= combine_add_split(s2, e2, &e1->b);
        return ok;
    }

    /* an end point of one edge on the other one */
    if (!d1 && combine_on_segment(e1, &e2->a)) ok &= combine_add_split(s1, e1, &e2->a);
    if (!d2 && combine_on_segment(e1, &e2->b)) ok &= combine_add_split(s1, e1, &e2->b);
    if (!d3 && combine_on_segment(e2, &e1->a)) ok &= combine_add_split(s2, e2, &e1->a);
    if (!d4 && combine_on_segment(e2, &e1->b)) ok &= combine_add_split(s2, e2, &e1->b);

    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
    {
        /* a proper crossing, at a point snapped to the grid */
        double t = (double)d3 / (double)(d3 - d4);
        struct combine_point p;

        p.x = llround(e1->a.x + t * (double)(e1->b.x - e1->a.x));
        p.y = llround(e1->a.y + t * (double)(e1->b.y - e1->a.y));
        ok &= combine_add_split(s1, e1, &p);
        ok &= combine_add_split(s2, e2, &p);
    }
    return ok;
}

static int __cdecl combine_split_compare(const void *a, const void *b)
{
    const struct combine_split *sa = a, *sb = b;

    return sa->t < sb->t ? -1 : sa->t > sb->t ? 1 : 0;
}

static int combine_point_compare(const struct combine_point *a, const struct combine_point *b)
{
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return 0;
}

static int __cdecl combine_edge_compare(const void *a, const void *b)
{
    const struct combine_edge *ea = a, *eb = b;
    int r;

    if ((r = combine_point_compare(&ea->a, &eb->a))) return r;
    return combine_point_compare(&ea->b, &eb->b);
}

/* the result: directed edges with the filled side on their left (in y-down coordinates: to the right on screen) */
struct combine_result_edge
{
    struct combine_point a, b;
    BOOL used;
};

struct combine_figure
{
    struct combine_point *points;
    size_t count;
};

static int __cdecl combine_figure_compare(const void *a, const void *b)
{
    const struct combine_figure *fa = a, *fb = b;

    /* descending by the start point, as Windows writes them */
    return -combine_point_compare(&fa->points[0], &fb->points[0]);
}

static double combine_turn(const struct combine_point *a, const struct combine_point *b, const struct combine_point *c)
{
    double dx1 = (double)(b->x - a->x), dy1 = (double)(b->y - a->y);
    double dx2 = (double)(c->x - b->x), dy2 = (double)(c->y - b->y);

    return atan2(dx1 * dy2 - dy1 * dx2, dx1 * dx2 + dy1 * dy2);
}

static int __cdecl combine_result_edge_compare(const void *a, const void *b)
{
    const struct combine_result_edge *ea = a, *eb = b;

    return combine_point_compare(&ea->a, &eb->a);
}

static size_t combine_first_edge_at(const struct combine_result_edge *edges, size_t count, const struct combine_point *p)
{
    size_t lo = 0, hi = count;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (combine_point_compare(&edges[mid].a, p) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void combine_write_figure(ID2D1SimplifiedGeometrySink *sink, const struct combine_figure *figure)
{
    D2D1_POINT_2F p;
    size_t i;

    p.x = figure->points[0].x / COMBINE_SCALE;
    p.y = figure->points[0].y / COMBINE_SCALE;
    ID2D1SimplifiedGeometrySink_BeginFigure(sink, p, D2D1_FIGURE_BEGIN_FILLED);
    for (i = 1; i <= figure->count; ++i)
    {
        const struct combine_point *q = &figure->points[i % figure->count];

        p.x = q->x / COMBINE_SCALE;
        p.y = q->y / COMBINE_SCALE;
        ID2D1SimplifiedGeometrySink_AddLines(sink, &p, 1);
    }
    ID2D1SimplifiedGeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
}

HRESULT d2d_geometry_combine(ID2D1Geometry *geometry1, ID2D1Geometry *geometry2, D2D1_COMBINE_MODE mode,
        const D2D1_MATRIX_3X2_F *transform, float tolerance, ID2D1SimplifiedGeometrySink *sink)
{
    struct combine_shape shapes[2] = {{0}};
    struct combine_splits *splits = NULL;
    struct combine_edge *all = NULL, *pieces = NULL;
    struct combine_result_edge *result = NULL;
    struct combine_figure *figures = NULL;
    size_t all_count, piece_count = 0, piece_size = 0, result_count = 0, result_size = 0;
    size_t figure_count = 0, figure_size = 0, i, j;
    HRESULT hr = E_OUTOFMEMORY;

    if ((unsigned int)mode > D2D1_COMBINE_MODE_EXCLUDE) return E_INVALIDARG;
    if (!(tolerance > 0.0f)) tolerance = D2D1_DEFAULT_FLATTENING_TOLERANCE;

    /* the transform applies to the second geometry only */
    if (FAILED(hr = combine_collect(geometry1, NULL, tolerance, &shapes[0]))) goto done;
    if (FAILED(hr = combine_collect(geometry2, transform, tolerance, &shapes[1]))) goto done;
    hr = E_OUTOFMEMORY;

    /* split every edge where it meets another one */
    all_count = shapes[0].count + shapes[1].count;
    if (all_count && (!(all = malloc(all_count * sizeof(*all))) || !(splits = calloc(all_count, sizeof(*splits)))))
        goto done;
    if (shapes[0].count) memcpy(all, shapes[0].edges, shapes[0].count * sizeof(*all));
    if (shapes[1].count) memcpy(all + shapes[0].count, shapes[1].edges, shapes[1].count * sizeof(*all));
    for (i = 0; i < all_count; ++i)
        for (j = i + 1; j < all_count; ++j)
            if (!combine_intersect(&all[i], &splits[i], &all[j], &splits[j])) goto done;

    for (i = 0; i < all_count; ++i)
    {
        struct combine_point prev = all[i].a;

        if (splits[i].count > 1) qsort(splits[i].s, splits[i].count, sizeof(*splits[i].s), combine_split_compare);
        for (j = 0; j <= splits[i].count; ++j)
        {
            struct combine_point next = j < splits[i].count ? splits[i].s[j].p : all[i].b;
            struct combine_edge *e;

            if (combine_point_equal(&prev, &next)) continue;
            if (!d2d_array_reserve((void **)&pieces, &piece_size, piece_count + 1, sizeof(*pieces))) goto done;
            e = &pieces[piece_count++];
            /* undirected: the smaller point first, so that the same piece from both geometries is found twice */
            if (combine_point_compare(&prev, &next) < 0) { e->a = prev; e->b = next; }
            else { e->a = next; e->b = prev; }
            prev = next;
        }
    }
    if (piece_count > 1) qsort(pieces, piece_count, sizeof(*pieces), combine_edge_compare);

    /* keep the pieces that have the result on one side only, directed with the filled side on their left */
    for (i = 0; i < piece_count; ++i)
    {
        const struct combine_edge *e = &pieces[i];
        double mx, my, nx, ny, len;
        BOOL left, right;

        if (i && !combine_edge_compare(e, &pieces[i - 1])) continue;
        mx = (e->a.x + e->b.x) / 2.0;
        my = (e->a.y + e->b.y) / 2.0;
        nx = -(double)(e->b.y - e->a.y);
        ny = (double)(e->b.x - e->a.x);
        len = sqrt(nx * nx + ny * ny);
        /* a quarter of a grid unit to each side */
        nx = nx / len * 0.25;
        ny = ny / len * 0.25;
        left = combine_result(mode, combine_shape_contains(&shapes[0], mx + nx, my + ny),
                combine_shape_contains(&shapes[1], mx + nx, my + ny));
        right = combine_result(mode, combine_shape_contains(&shapes[0], mx - nx, my - ny),
                combine_shape_contains(&shapes[1], mx - nx, my - ny));
        if (left == right) continue;
        if (!d2d_array_reserve((void **)&result, &result_size, result_count + 1, sizeof(*result))) goto done;
        if (left) { result[result_count].a = e->a; result[result_count].b = e->b; }
        else { result[result_count].a = e->b; result[result_count].b = e->a; }
        result[result_count].used = FALSE;
        ++result_count;
    }
    if (result_count > 1) qsort(result, result_count, sizeof(*result), combine_result_edge_compare);

    /* link the edges into figures; at a point with several ways on, take the one that turns most towards the filled side */
    for (i = 0; i < result_count; ++i)
    {
        struct combine_figure figure = {0};
        size_t size = 0, cur = i, k, start, best;

        if (result[i].used) continue;
        for (;;)
        {
            result[cur].used = TRUE;
            if (!d2d_array_reserve((void **)&figure.points, &size, figure.count + 1, sizeof(*figure.points)))
            {
                free(figure.points);
                goto done;
            }
            figure.points[figure.count++] = result[cur].a;
            if (combine_point_equal(&result[cur].b, &result[i].a)) break;
            best = result_count;
            for (k = combine_first_edge_at(result, result_count, &result[cur].b);
                    k < result_count && combine_point_equal(&result[k].a, &result[cur].b); ++k)
            {
                if (result[k].used) continue;
                if (best == result_count || combine_turn(&result[cur].a, &result[cur].b, &result[k].b)
                        > combine_turn(&result[cur].a, &result[cur].b, &result[best].b))
                    best = k;
            }
            if (best == result_count) break;   /* should not happen: an unclosed chain */
            cur = best;
        }

        /* drop the points where the figure goes straight on */
        for (k = 0; k < figure.count && figure.count > 2;)
        {
            const struct combine_point *p0 = &figure.points[(k + figure.count - 1) % figure.count];
            const struct combine_point *p1 = &figure.points[k], *p2 = &figure.points[(k + 1) % figure.count];

            if (!combine_cross(p0, p1, p2) && (p1->x - p0->x) * (p2->x - p1->x) + (p1->y - p0->y) * (p2->y - p1->y) > 0)
            {
                memmove(&figure.points[k], &figure.points[k + 1], (figure.count - k - 1) * sizeof(*figure.points));
                --figure.count;
                if (k) --k;
            }
            else ++k;
        }
        if (figure.count < 3)
        {
            free(figure.points);
            continue;
        }

        /* start at the top-most, then left-most point */
        for (k = 1, start = 0; k < figure.count; ++k)
            if (combine_point_compare(&figure.points[k], &figure.points[start]) < 0) start = k;
        if (start)
        {
            struct combine_point *rotated = malloc(figure.count * sizeof(*rotated));

            if (!rotated)
            {
                free(figure.points);
                goto done;
            }
            for (k = 0; k < figure.count; ++k) rotated[k] = figure.points[(start + k) % figure.count];
            free(figure.points);
            figure.points = rotated;
        }

        if (!d2d_array_reserve((void **)&figures, &figure_size, figure_count + 1, sizeof(*figures)))
        {
            free(figure.points);
            goto done;
        }
        figures[figure_count++] = figure;
    }

    if (figure_count > 1) qsort(figures, figure_count, sizeof(*figures), combine_figure_compare);
    ID2D1SimplifiedGeometrySink_SetFillMode(sink, D2D1_FILL_MODE_ALTERNATE);
    for (i = 0; i < figure_count; ++i)
        combine_write_figure(sink, &figures[i]);
    hr = S_OK;

done:
    for (i = 0; i < figure_count; ++i) free(figures[i].points);
    free(figures);
    free(result);
    free(pieces);
    if (splits)
    {
        for (i = 0; i < all_count; ++i) free(splits[i].s);
        free(splits);
    }
    free(all);
    free(shapes[0].edges);
    free(shapes[1].edges);
    return hr;
}
