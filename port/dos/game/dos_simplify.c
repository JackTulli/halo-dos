/*
DOS_SIMPLIFY.C

A mesh with fewer triangles and no holes, for the first-person weapon's
lighter copies (VIEW MODEL, dos_models.c) and the level's distant copies
(dos_level.c).

Edges are collapsed, cheapest first by the quadric error (Garland and
Heckbert): a vertex moves onto a neighbour (one of its own node, where the
mesh has nodes: they move apart in the animation otherwise), never one on
the mesh's outer edge (another mesh goes on from there: a level tile's
neighbours, drawn full or coarse, meet it exactly), never farther than
max_distance from the planes of its triangles, and never where a triangle
around it would turn over or fold flat (turned over, it would be culled: a
hole). A vertex on a texture's seam moves with its twins (the other
vertices in the same place): each onto the twin of its neighbour, all at
once or none, so the seam stays closed. Vertices only move onto others, so
the copy uses the mesh's own vertices, texture coordinates and colours.

Done in a few passes: every candidate collapse priced, then taken cheapest
first while none of its vertices has moved in that pass.
*/

#include "cseries.h"

#include "render/render.h"

#include "dos_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void *raw_malloc(size_t size) { return (malloc)(size); }
static void raw_free(void *pointer) { (free)(pointer); }

#define SIMPLIFY_PASSES 32
/* the most twins a place has (a seam's sides; a place with more is left as
it is) */
#define MAXIMUM_TWINS 8

struct collapse
{
	double cost;
	unsigned short from, to;
};

/* each vertex's corners (a triangle's index times 3, plus which), linked:
first[vertex], then next[corner], -1 at the end; a vertex that moves passes
its corners to where it went */
struct corner_lists
{
	int *first, *next;
};

struct mesh
{
	const float *positions;
	int stride;
	const unsigned char *nodes;
	int vertex_count, triangle_count;
};

#define POSITION(m, v) (&(m)->positions[(size_t)(v) * (m)->stride])
#define NODE(m, v) ((m)->nodes ? (m)->nodes[v] : 0)

static int collapse_order(const void *a, const void *b)
{
	double x = ((const struct collapse *)a)->cost, y = ((const struct collapse *)b)->cost;

	return x < y ? -1 : x > y;
}

static int unsigned_pair_order(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;

	return x < y ? -1 : x > y;
}

static const struct mesh *sort_mesh;

static int position_order(const void *a, const void *b)
{
	const float *p = POSITION(sort_mesh, *(const unsigned short *)a), *q = POSITION(sort_mesh, *(const unsigned short *)b);

	for (int k = 0; k < 3; k++)
	{
		if (p[k] != q[k])
			return p[k] < q[k] ? -1 : 1;
	}
	return 0;
}

static void triangle_normal(const float *a, const float *b, const float *c, double *n)
{
	double u[3], v[3];

	for (int k = 0; k < 3; k++)
	{
		u[k] = (double)b[k] - a[k];
		v[k] = (double)c[k] - a[k];
	}
	n[0] = u[1] * v[2] - u[2] * v[1];
	n[1] = u[2] * v[0] - u[0] * v[2];
	n[2] = u[0] * v[1] - u[1] * v[0];
}

/* a quadric (the 10 numbers of a symmetric 4x4) at a point */
static double quadric_error(const double *q, const float *p)
{
	double x = p[0], y = p[1], z = p[2];

	return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
		2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
}

/* whether moving from onto to keeps every triangle of from's (but those it
shares with to, which go) facing as it did, not folded flat, and to within
max_distance of its plane */
static int collapse_keeps_shape(const struct mesh *mesh, const unsigned short *corners, const unsigned char *alive,
	const struct corner_lists *lists, int from, int to, double max_distance)
{
	const float *destination = POSITION(mesh, to);

	for (int s = lists->first[from]; s >= 0; s = lists->next[s])
	{
		int t = s / 3;
		const unsigned short *c = &corners[t * 3];
		const float *p[3];
		double before[3], after[3], length_before, length_after, dot, distance;
		int has_from = -1, has_to = FALSE;

		if (!alive[t])
			continue;
		for (int k = 0; k < 3; k++)
		{
			if (c[k] == from)
				has_from = k;
			if (c[k] == to)
				has_to = TRUE;
		}
		if (has_from < 0)
			continue;
		for (int k = 0; k < 3; k++)
			p[k] = POSITION(mesh, c[k]);
		triangle_normal(p[0], p[1], p[2], before);
		length_before = sqrt(before[0] * before[0] + before[1] * before[1] + before[2] * before[2]);
		/* (to as far as max_distance from the triangle's plane: the copy's
		surface no farther than that from the mesh's) */
		if (length_before > 0.0)
		{
			distance = ((destination[0] - p[0][0]) * before[0] + (destination[1] - p[0][1]) * before[1] +
				(destination[2] - p[0][2]) * before[2]) / length_before;
			if (distance > max_distance || distance < -max_distance)
				return FALSE;
		}
		if (has_to)
			continue;
		p[has_from] = destination;
		triangle_normal(p[0], p[1], p[2], after);
		length_after = sqrt(after[0] * after[0] + after[1] * after[1] + after[2] * after[2]);
		dot = before[0] * after[0] + before[1] * after[1] + before[2] * after[2];
		/* (turned over, or more than about 60 degrees, or flattened) */
		if (length_after <= length_before * 0.02 || dot <= 0.5 * length_before * length_after)
			return FALSE;
	}
	return TRUE;
}

/* whether an alive triangle has both a and b */
static int edge_alive(const unsigned short *corners, const unsigned char *alive, const struct corner_lists *lists,
	int a, int b)
{
	for (int s = lists->first[a]; s >= 0; s = lists->next[s])
	{
		const unsigned short *c = &corners[s / 3 * 3];

		if (alive[s / 3] && (c[0] == b || c[1] == b || c[2] == b))
			return TRUE;
	}
	return FALSE;
}

/* from moved onto to: its triangles with to gone, the rest taking to (and
its corners joining to's list) */
static void collapse_apply(unsigned short *corners, unsigned char *alive, int *alive_count, unsigned char *touched,
	struct corner_lists *lists, int from, int to)
{
	int last = -1;

	for (int s = lists->first[from]; s >= 0; s = lists->next[s])
	{
		int t = s / 3;
		unsigned short *c = &corners[t * 3];

		last = s;
		if (!alive[t])
			continue;
		if (c[0] == to || c[1] == to || c[2] == to)
		{
			alive[t] = FALSE;
			(*alive_count)--;
			continue;
		}
		corners[s] = (unsigned short)to;
		for (int k = 0; k < 3; k++)
			touched[c[k]] = TRUE;
	}
	if (last >= 0)
	{
		lists->next[last] = lists->first[to];
		lists->first[to] = lists->first[from];
		lists->first[from] = -1;
	}
	touched[from] = touched[to] = TRUE;
}

int dos_simplify(const float *positions, int stride, const unsigned char *nodes, int vertex_count,
	const unsigned short *indices, int triangle_count, int target, double max_distance, unsigned short *kept)
{
	struct mesh mesh = { positions, stride, nodes, vertex_count, triangle_count };
	int alive_count = triangle_count, result = -1;
	unsigned short *corners = raw_malloc((size_t)triangle_count * 3 * sizeof(unsigned short));
	unsigned char *alive = raw_malloc((size_t)triangle_count);
	unsigned char *locked = raw_malloc((size_t)vertex_count);
	unsigned char *gone = raw_malloc((size_t)vertex_count);
	unsigned char *touched = raw_malloc((size_t)vertex_count);
	double *quadrics = raw_malloc((size_t)vertex_count * 10 * sizeof(double));
	unsigned long *edges = raw_malloc((size_t)triangle_count * 3 * sizeof(unsigned long));
	unsigned short *order = raw_malloc((size_t)vertex_count * sizeof(unsigned short));
	/* each vertex's place: the first of its twins, and where its twins are in
	order (their first) and how many */
	unsigned short *place = raw_malloc((size_t)vertex_count * sizeof(unsigned short));
	int *twins_first = raw_malloc((size_t)vertex_count * sizeof(int));
	unsigned char *twins_count = raw_malloc((size_t)vertex_count);
	struct collapse *collapses = raw_malloc((size_t)triangle_count * 6 * sizeof(struct collapse));
	struct corner_lists lists;

	lists.first = raw_malloc((size_t)vertex_count * sizeof(int));
	lists.next = raw_malloc((size_t)triangle_count * 3 * sizeof(int));
	if (!corners || !alive || !locked || !gone || !touched || !quadrics || !edges || !order || !place ||
		!twins_first || !twins_count || !collapses || !lists.first || !lists.next || vertex_count > 65535)
	{
		goto done;
	}
	memcpy(corners, indices, (size_t)triangle_count * 3 * sizeof(unsigned short));
	memset(alive, 1, (size_t)triangle_count);
	memset(locked, 0, (size_t)vertex_count);
	memset(gone, 0, (size_t)vertex_count);
	memset(quadrics, 0, (size_t)vertex_count * 10 * sizeof(double));
	for (int v = 0; v < vertex_count; v++)
		lists.first[v] = -1;
	for (int s = 0; s < triangle_count * 3; s++)
	{
		lists.next[s] = lists.first[corners[s]];
		lists.first[corners[s]] = s;
	}
	/* the places: twins sorted together */
	for (int v = 0; v < vertex_count; v++)
		order[v] = (unsigned short)v;
	sort_mesh = &mesh;
	qsort(order, (size_t)vertex_count, sizeof(unsigned short), position_order);
	for (int i = 0; i < vertex_count; )
	{
		int j = i + 1;

		while (j < vertex_count && !position_order(&order[i], &order[j]))
			j++;
		for (int k = i; k < j; k++)
		{
			place[order[k]] = order[i];
			twins_first[order[k]] = i;
			twins_count[order[k]] = (unsigned char)(j - i > 255 ? 255 : j - i);
			if (j - i > MAXIMUM_TWINS)
				locked[order[k]] = TRUE;
		}
		i = j;
	}
	/* the mesh's outer edge: the edges between two places that one triangle
	only has (a seam's edge, its sides apart in the indices, has two: one
	each side) */
	for (int t = 0; t < triangle_count; t++)
	{
		for (int k = 0; k < 3; k++)
		{
			unsigned long a = place[corners[t * 3 + k]], b = place[corners[t * 3 + (k + 1) % 3]];

			edges[t * 3 + k] = a < b ? (a << 16) | b : (b << 16) | a;
		}
	}
	qsort(edges, (size_t)triangle_count * 3, sizeof(unsigned long), unsigned_pair_order);
	for (int i = 0; i < triangle_count * 3; )
	{
		int j = i + 1;

		while (j < triangle_count * 3 && edges[j] == edges[i])
			j++;
		if (j - i == 1)
		{
			int ends[2] = { (int)(edges[i] >> 16), (int)(edges[i] & 0xFFFF) };

			for (int e = 0; e < 2; e++)
			{
				for (int k = 0; k < twins_count[ends[e]]; k++)
					locked[order[twins_first[ends[e]] + k]] = TRUE;
			}
		}
		i = j;
	}
	/* each place's quadric: its triangles' planes, by their areas */
	for (int t = 0; t < triangle_count; t++)
	{
		const float *a = POSITION(&mesh, corners[t * 3]);
		double n[3], length, d, q[10];

		triangle_normal(a, POSITION(&mesh, corners[t * 3 + 1]), POSITION(&mesh, corners[t * 3 + 2]), n);
		length = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length <= 0.0)
			continue;
		d = -(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]) / length;
		{
			double x = n[0] / length, y = n[1] / length, z = n[2] / length, w = length * 0.5;

			q[0] = w * x * x; q[1] = w * x * y; q[2] = w * x * z; q[3] = w * x * d;
			q[4] = w * y * y; q[5] = w * y * z; q[6] = w * y * d;
			q[7] = w * z * z; q[8] = w * z * d; q[9] = w * d * d;
		}
		for (int k = 0; k < 3; k++)
		{
			for (int e = 0; e < 10; e++)
				quadrics[place[corners[t * 3 + k]] * 10 + e] += q[e];
		}
	}
	for (int pass = 0; pass < SIMPLIFY_PASSES && alive_count > target; pass++)
	{
		int count = 0, taken = 0;

		for (int t = 0; t < triangle_count; t++)
		{
			if (!alive[t])
				continue;
			for (int k = 0; k < 6; k++)
			{
				int from = corners[t * 3 + k % 3], to = corners[t * 3 + (k % 3 + (k < 3 ? 1 : 2)) % 3];
				double q[10];

				if (locked[from] || NODE(&mesh, from) != NODE(&mesh, to) || place[from] == place[to])
					continue;
				for (int e = 0; e < 10; e++)
					q[e] = quadrics[place[from] * 10 + e] + quadrics[place[to] * 10 + e];
				collapses[count].cost = quadric_error(q, POSITION(&mesh, to));
				collapses[count].from = (unsigned short)from;
				collapses[count].to = (unsigned short)to;
				count++;
			}
		}
		if (!count)
			break;
		qsort(collapses, (size_t)count, sizeof(struct collapse), collapse_order);
		memset(touched, 0, (size_t)vertex_count);
		for (int i = 0; i < count && alive_count > target; i++)
		{
			int from = collapses[i].from, to = collapses[i].to;
			int pair_from[MAXIMUM_TWINS], pair_to[MAXIMUM_TWINS], pairs = 0, possible = TRUE;

			if (gone[from] || touched[from] || touched[to] || locked[from])
				continue;
			/* each twin of from (still a corner) onto a twin of to it has an
			edge with, of its own node */
			for (int k = 0; k < twins_count[from] && possible; k++)
			{
				int f = order[twins_first[from] + k], t = -1;

				if (gone[f])
					continue;
				for (int m = 0; m < twins_count[to] && t < 0; m++)
				{
					int candidate = order[twins_first[to] + m];

					if (!gone[candidate] && NODE(&mesh, candidate) == NODE(&mesh, f) &&
						edge_alive(corners, alive, &lists, f, candidate))
					{
						t = candidate;
					}
				}
				if (t < 0 || touched[f] || touched[t] || locked[f] ||
					!collapse_keeps_shape(&mesh, corners, alive, &lists, f, t, max_distance))
				{
					possible = FALSE;
					break;
				}
				pair_from[pairs] = f;
				pair_to[pairs] = t;
				pairs++;
			}
			if (!possible || !pairs)
				continue;
			for (int k = 0; k < pairs; k++)
			{
				collapse_apply(corners, alive, &alive_count, touched, &lists, pair_from[k], pair_to[k]);
				gone[pair_from[k]] = TRUE;
				locked[pair_from[k]] = TRUE;
			}
			for (int e = 0; e < 10; e++)
				quadrics[place[to] * 10 + e] += quadrics[place[from] * 10 + e];
			taken++;
		}
		if (!taken)
			break;
	}
	result = 0;
	for (int t = 0; t < triangle_count; t++)
	{
		if (alive[t])
		{
			memcpy(&kept[result * 3], &corners[t * 3], 3 * sizeof(unsigned short));
			result++;
		}
	}
done:
	raw_free(corners);
	raw_free(alive);
	raw_free(locked);
	raw_free(gone);
	raw_free(touched);
	raw_free(quadrics);
	raw_free(edges);
	raw_free(order);
	raw_free(place);
	raw_free(twins_first);
	raw_free(twins_count);
	raw_free(collapses);
	raw_free(lists.first);
	raw_free(lists.next);
	return result;
}
