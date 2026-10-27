/*
OBS Projection Mapping
Copyright (C) 2026 Matias Knudsen <matiasknuds@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Kilden "Projection Mapping".
 *
 * Henter bildet fra en valgt scene/kilde ("innhold") og tegner det på
 * et ubegrenset antall flater. Hver flate har:
 *   - dst[4]   hvor flaten ligger i projektorbildet (geometri / Move)
 *   - src[4]   hvilken del av innholdet flaten viser (Utsnitt)
 *   - shape    firkant eller rund (ellipse)
 *   - radius   avrundede hjørner for firkanter (0..1)
 *   - maske    et redigerbart polygon + frihåndsstrøk (legg til / visk ut)
 * Alle punkter lagres normalisert (0..1) i projektorbildet.
 * Hjørnerekkefølge: øverst venstre, øverst høyre, nederst høyre, nederst venstre.
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <plugin-support.h>

#define PM_ID "projection_mapping_source"
#define PM_MAX_UNDO 100
#define PM_MAX_LASSO 4000

enum pm_mode {
	MODE_SHOW = 0, /* Ferdig: kun bildet */
	MODE_MOVE = 1, /* Dra hjørner, kanter og flater */
	MODE_CROP = 2, /* Velg utsnitt av innholdet */
	MODE_MASK = 3, /* Rediger masker */
};

enum pm_shape {
	SHAPE_QUAD = 0,
	SHAPE_ROUND = 1,
};

enum pm_drag {
	DRAG_NONE = 0,
	DRAG_WARP,       /* hjørne/kant/hele flaten (Move eller Utsnitt) */
	DRAG_MASK_POINT, /* et punkt i maskepolygonet */
	DRAG_LASSO,      /* frihåndstegning av maske */
};

struct pm_stroke {
	bool subtract;
	DARRAY(struct vec2) pts;
};

struct pm_surface {
	struct vec2 dst[4];
	struct vec2 src[4];
	int shape;
	float radius;
	DARRAY(struct vec2) mask;         /* tomt = ingen polygon-maske */
	DARRAY(struct pm_stroke) strokes; /* frihåndsstrøk i rekkefølge */
};

/* Satt av panelet mens det tegner, så hjelpelinjer alltid vises der. */
static volatile bool pm_editor_render = false;

void pm_set_editor_render(bool enabled)
{
	pm_editor_render = enabled;
}

struct pm_source {
	obs_source_t *self;
	pthread_mutex_t mutex;

	DARRAY(struct pm_surface) surfaces;
	int selected;
	int mode;
	bool guides_on_output;
	uint32_t width;
	uint32_t height;

	char *input_uuid;
	bool need_resolve;
	float retry_timer;
	obs_weak_source_t *input;

	gs_texrender_t *texrender;
	gs_effect_t *effect;
	bool rendered;
	DARRAY(gs_texrender_t *) mask_tr;
	gs_texrender_t *tmp_tr;

	int drag;
	int drag_surface;
	int drag_corner; /* 0-3 hjørne, 4-7 kant, -1 hele flaten; maskepunkt-indeks ved DRAG_MASK_POINT */
	struct vec2 drag_last;
	bool lasso_subtract;
	DARRAY(struct vec2) lasso;

	DARRAY(char *) undo;
	DARRAY(char *) redo;
	uint64_t last_edit;
};

static void proc_add_surface(void *data, calldata_t *cd);
static void proc_remove_surface(void *data, calldata_t *cd);
static void proc_toggle_shape(void *data, calldata_t *cd);
static void proc_reset_surface(void *data, calldata_t *cd);
static void proc_set_mode(void *data, calldata_t *cd);
static void proc_get_mode(void *data, calldata_t *cd);
static void proc_get_selected(void *data, calldata_t *cd);
static void proc_set_radius(void *data, calldata_t *cd);
static void proc_push_undo(void *data, calldata_t *cd);
static void proc_undo(void *data, calldata_t *cd);
static void proc_redo(void *data, calldata_t *cd);
static void proc_clear_mask(void *data, calldata_t *cd);
static void proc_wants_click(void *data, calldata_t *cd);
static void proc_get_last_edit(void *data, calldata_t *cd);
static void proc_set_guides(void *data, calldata_t *cd);

/* ------------------------------------------------------------------ */
/* Matematikk                                                           */

typedef double pm_mat3[9];

/* Enhetskvadrat (0,0)(1,0)(1,1)(0,1) -> firkant q (Heckbert). */
static void square_to_quad(const struct vec2 q[4], pm_mat3 m)
{
	double x0 = q[0].x, y0 = q[0].y, x1 = q[1].x, y1 = q[1].y;
	double x2 = q[2].x, y2 = q[2].y, x3 = q[3].x, y3 = q[3].y;
	double sx = x0 - x1 + x2 - x3;
	double sy = y0 - y1 + y2 - y3;
	double g = 0.0, h = 0.0;

	if (fabs(sx) > 1e-12 || fabs(sy) > 1e-12) {
		double dx1 = x1 - x2, dx2 = x3 - x2;
		double dy1 = y1 - y2, dy2 = y3 - y2;
		double den = dx1 * dy2 - dx2 * dy1;
		if (fabs(den) > 1e-12) {
			g = (sx * dy2 - dx2 * sy) / den;
			h = (dx1 * sy - sx * dy1) / den;
		}
	}

	m[0] = x1 - x0 + g * x1;
	m[1] = x3 - x0 + h * x3;
	m[2] = x0;
	m[3] = y1 - y0 + g * y1;
	m[4] = y3 - y0 + h * y3;
	m[5] = y0;
	m[6] = g;
	m[7] = h;
	m[8] = 1.0;
}

static bool mat3_inverse(const pm_mat3 m, pm_mat3 o)
{
	double a = m[0], b = m[1], c = m[2];
	double d = m[3], e = m[4], f = m[5];
	double g = m[6], h = m[7], i = m[8];
	double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	double det = a * A + b * B + c * C;
	if (fabs(det) < 1e-12)
		return false;
	double inv = 1.0 / det;
	o[0] = A * inv;
	o[1] = -(b * i - c * h) * inv;
	o[2] = (b * f - c * e) * inv;
	o[3] = B * inv;
	o[4] = (a * i - c * g) * inv;
	o[5] = -(a * f - c * d) * inv;
	o[6] = C * inv;
	o[7] = -(a * h - b * g) * inv;
	o[8] = (a * e - b * d) * inv;
	return true;
}

static bool point_in_poly(const struct vec2 *q, size_t n, float x, float y)
{
	bool inside = false;
	for (size_t i = 0, j = n - 1; i < n; j = i++) {
		if (((q[i].y > y) != (q[j].y > y)) &&
		    (x < (q[j].x - q[i].x) * (y - q[i].y) / (q[j].y - q[i].y) + q[i].x))
			inside = !inside;
	}
	return inside;
}

static float dist_to_segment(struct vec2 p, struct vec2 a, struct vec2 b)
{
	float dx = b.x - a.x, dy = b.y - a.y;
	float len2 = dx * dx + dy * dy;
	float t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
	return sqrtf(ex * ex + ey * ey);
}

/* Punkt (s, t) i flatens enhetskvadrat -> piksel. */
static struct vec2 map_unit(const pm_mat3 m, float u, float v)
{
	double x = m[0] * u + m[1] * v + m[2];
	double y = m[3] * u + m[4] * v + m[5];
	double w = m[6] * u + m[7] * v + m[8];
	struct vec2 r;
	vec2_set(&r, (float)(x / w), (float)(y / w));
	return r;
}

/* Omtrentlig størrelse på flaten i piksler (snitt av motstående kanter). */
static void quad_size(const struct vec2 px[4], float *w, float *h)
{
	*w = 0.5f * (vec2_dist(&px[0], &px[1]) + vec2_dist(&px[3], &px[2]));
	*h = 0.5f * (vec2_dist(&px[0], &px[3]) + vec2_dist(&px[1], &px[2]));
	if (*w < 1.0f)
		*w = 1.0f;
	if (*h < 1.0f)
		*h = 1.0f;
}

/* ------------------------------------------------------------------ */
/* Flater: minne og standardverdier                                     */

static void surface_free(struct pm_surface *s)
{
	da_free(s->mask);
	for (size_t i = 0; i < s->strokes.num; i++)
		da_free(s->strokes.array[i].pts);
	da_free(s->strokes);
}

static void surfaces_clear(struct pm_source *pm)
{
	for (size_t i = 0; i < pm->surfaces.num; i++)
		surface_free(&pm->surfaces.array[i]);
	da_resize(pm->surfaces, 0);
}

static bool surface_has_mask(const struct pm_surface *s)
{
	return s->mask.num >= 3 || s->strokes.num > 0;
}

/* aspect = utgangens bredde / høyde, slik at runde flater blir sirkler. */
static void surface_set_default(struct pm_surface *s, size_t index, int shape, float aspect)
{
	memset(s, 0, sizeof(*s));
	float off = 0.05f * (float)(index % 6);
	float h = 0.4f;
	float w = shape == SHAPE_ROUND && aspect > 0.0f ? h / aspect : h;
	vec2_set(&s->dst[0], 0.25f + off, 0.25f + off);
	vec2_set(&s->dst[1], 0.25f + off + w, 0.25f + off);
	vec2_set(&s->dst[2], 0.25f + off + w, 0.25f + off + h);
	vec2_set(&s->dst[3], 0.25f + off, 0.25f + off + h);
	vec2_set(&s->src[0], 0.0f, 0.0f);
	vec2_set(&s->src[1], 1.0f, 0.0f);
	vec2_set(&s->src[2], 1.0f, 1.0f);
	vec2_set(&s->src[3], 0.0f, 1.0f);
	s->shape = shape;
	s->radius = 0.0f;
}

static float pm_aspect(const struct pm_source *pm)
{
	return pm->height ? (float)pm->width / (float)pm->height : 1.0f;
}

/* ------------------------------------------------------------------ */
/* Lagring i OBS-prosjektet                                             */

static void points_to_string(struct dstr *out, const struct vec2 *p, size_t n)
{
	dstr_init(out);
	for (size_t i = 0; i < n; i++)
		dstr_catf(out, "%s%.5f %.5f", i ? " " : "", p[i].x, p[i].y);
}

static void string_to_points(const char *str, void *da_ptr)
{
	DARRAY(struct vec2) *da = da_ptr;
	da_resize(*da, 0);
	if (!str)
		return;
	const char *p = str;
	char *end;
	for (;;) {
		float x = strtof(p, &end);
		if (end == p)
			break;
		p = end;
		float y = strtof(p, &end);
		if (end == p)
			break;
		p = end;
		struct vec2 v;
		vec2_set(&v, x, y);
		da_push_back(*da, &v);
	}
}

static void load_surfaces_from(struct pm_source *pm, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, "surfaces");
	size_t count = arr ? obs_data_array_count(arr) : 0;
	char key[8];

	surfaces_clear(pm);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		struct pm_surface s;
		memset(&s, 0, sizeof(s));
		for (int c = 0; c < 4; c++) {
			snprintf(key, sizeof(key), "d%dx", c);
			s.dst[c].x = (float)obs_data_get_double(item, key);
			snprintf(key, sizeof(key), "d%dy", c);
			s.dst[c].y = (float)obs_data_get_double(item, key);
			snprintf(key, sizeof(key), "s%dx", c);
			s.src[c].x = (float)obs_data_get_double(item, key);
			snprintf(key, sizeof(key), "s%dy", c);
			s.src[c].y = (float)obs_data_get_double(item, key);
		}
		s.shape = (int)obs_data_get_int(item, "shape");
		s.radius = (float)obs_data_get_double(item, "radius");
		string_to_points(obs_data_get_string(item, "mask"), &s.mask);

		obs_data_array_t *strokes = obs_data_get_array(item, "strokes");
		size_t ns = strokes ? obs_data_array_count(strokes) : 0;
		for (size_t k = 0; k < ns; k++) {
			obs_data_t *st = obs_data_array_item(strokes, k);
			struct pm_stroke stroke;
			memset(&stroke, 0, sizeof(stroke));
			stroke.subtract = obs_data_get_bool(st, "sub");
			string_to_points(obs_data_get_string(st, "pts"), &stroke.pts);
			if (stroke.pts.num >= 3)
				da_push_back(s.strokes, &stroke);
			else
				da_free(stroke.pts);
			obs_data_release(st);
		}
		obs_data_array_release(strokes);

		da_push_back(pm->surfaces, &s);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void save_surfaces_to(struct pm_source *pm, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_array_create();
	char key[8];

	for (size_t i = 0; i < pm->surfaces.num; i++) {
		const struct pm_surface *s = &pm->surfaces.array[i];
		obs_data_t *item = obs_data_create();
		for (int c = 0; c < 4; c++) {
			snprintf(key, sizeof(key), "d%dx", c);
			obs_data_set_double(item, key, s->dst[c].x);
			snprintf(key, sizeof(key), "d%dy", c);
			obs_data_set_double(item, key, s->dst[c].y);
			snprintf(key, sizeof(key), "s%dx", c);
			obs_data_set_double(item, key, s->src[c].x);
			snprintf(key, sizeof(key), "s%dy", c);
			obs_data_set_double(item, key, s->src[c].y);
		}
		obs_data_set_int(item, "shape", s->shape);
		obs_data_set_double(item, "radius", s->radius);

		struct dstr str;
		points_to_string(&str, s->mask.array, s->mask.num);
		obs_data_set_string(item, "mask", str.array ? str.array : "");
		dstr_free(&str);

		obs_data_array_t *strokes = obs_data_array_create();
		for (size_t k = 0; k < s->strokes.num; k++) {
			obs_data_t *st = obs_data_create();
			obs_data_set_bool(st, "sub", s->strokes.array[k].subtract);
			points_to_string(&str, s->strokes.array[k].pts.array, s->strokes.array[k].pts.num);
			obs_data_set_string(st, "pts", str.array ? str.array : "");
			dstr_free(&str);
			obs_data_array_push_back(strokes, st);
			obs_data_release(st);
		}
		obs_data_set_array(item, "strokes", strokes);
		obs_data_array_release(strokes);

		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "surfaces", arr);
	obs_data_set_int(settings, "selected", pm->selected);
	obs_data_array_release(arr);
}

/* Lagrer gjeldende flater i kildens innstillinger. Kalles med mutex låst. */
static void save_to_source(struct pm_source *pm)
{
	obs_data_t *settings = obs_source_get_settings(pm->self);
	save_surfaces_to(pm, settings);
	obs_data_release(settings);
	pm->last_edit = os_gettime_ns();
}

/* ------------------------------------------------------------------ */
/* Angre / gjør om                                                      */

typedef DARRAY(char *) pm_string_stack;

static char *snapshot(struct pm_source *pm)
{
	obs_data_t *d = obs_data_create();
	save_surfaces_to(pm, d);
	char *json = bstrdup(obs_data_get_json(d));
	obs_data_release(d);
	return json;
}

static void clamp_selected(struct pm_source *pm)
{
	if (pm->selected >= (int)pm->surfaces.num)
		pm->selected = (int)pm->surfaces.num - 1;
	if (pm->selected < 0)
		pm->selected = 0;
}

static void restore(struct pm_source *pm, const char *json)
{
	obs_data_t *d = obs_data_create_from_json(json);
	if (!d)
		return;
	load_surfaces_from(pm, d);
	pm->selected = (int)obs_data_get_int(d, "selected");
	clamp_selected(pm);
	obs_data_release(d);
	save_to_source(pm);
}

static void clear_stack(pm_string_stack *st)
{
	for (size_t i = 0; i < st->num; i++)
		bfree(st->array[i]);
	da_resize(*st, 0);
}

/* Lagre tilstanden før en endring. Kalles med mutex låst. */
static void push_undo(struct pm_source *pm)
{
	pm_string_stack *undo = (pm_string_stack *)&pm->undo;
	char *s = snapshot(pm);
	if (undo->num && strcmp(undo->array[undo->num - 1], s) == 0) {
		bfree(s);
		return;
	}
	if (undo->num >= PM_MAX_UNDO) {
		bfree(undo->array[0]);
		da_erase(*undo, 0);
	}
	da_push_back(*undo, &s);
	clear_stack((pm_string_stack *)&pm->redo);
}

static bool do_undo(struct pm_source *pm, bool redo)
{
	bool done = false;
	pthread_mutex_lock(&pm->mutex);
	pm_string_stack *from = (pm_string_stack *)(redo ? (void *)&pm->redo : (void *)&pm->undo);
	pm_string_stack *to = (pm_string_stack *)(redo ? (void *)&pm->undo : (void *)&pm->redo);
	if (from->num) {
		char *cur = snapshot(pm);
		da_push_back(*to, &cur);
		char *state = from->array[from->num - 1];
		da_pop_back(*from);
		restore(pm, state);
		bfree(state);
		pm->drag = DRAG_NONE;
		done = true;
	}
	pthread_mutex_unlock(&pm->mutex);
	return done;
}

/* ------------------------------------------------------------------ */
/* Innholdskilde                                                        */

static obs_source_t *get_input(struct pm_source *pm)
{
	obs_source_t *src = NULL;
	pthread_mutex_lock(&pm->mutex);
	if (pm->input)
		src = obs_weak_source_get_source(pm->input);
	pthread_mutex_unlock(&pm->mutex);
	return src;
}

static void set_input(struct pm_source *pm, obs_source_t *src)
{
	pthread_mutex_lock(&pm->mutex);
	obs_weak_source_t *old_weak = pm->input;
	pm->input = NULL;
	pthread_mutex_unlock(&pm->mutex);

	if (old_weak) {
		obs_source_t *old = obs_weak_source_get_source(old_weak);
		if (old) {
			obs_source_remove_active_child(pm->self, old);
			obs_source_release(old);
		}
		obs_weak_source_release(old_weak);
	}

	if (!src || src == pm->self)
		return;

	if (!obs_source_add_active_child(pm->self, src)) {
		obs_log(LOG_WARNING, "Kan ikke bruke '%s' som innhold (den inneholder selve mapping-kilden).",
			obs_source_get_name(src));
		return;
	}

	pthread_mutex_lock(&pm->mutex);
	pm->input = obs_source_get_weak_source(src);
	pthread_mutex_unlock(&pm->mutex);
}

/* ------------------------------------------------------------------ */
/* OBS-kildefunksjoner                                                  */

static const char *pm_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Projection Mapping";
}

static void pm_update(void *data, obs_data_t *settings)
{
	struct pm_source *pm = data;

	pthread_mutex_lock(&pm->mutex);
	pm->width = (uint32_t)obs_data_get_int(settings, "width");
	pm->height = (uint32_t)obs_data_get_int(settings, "height");
	if (pm->width < 16)
		pm->width = 16;
	if (pm->height < 16)
		pm->height = 16;
	pm->mode = (int)obs_data_get_int(settings, "mode");
	pm->guides_on_output = obs_data_get_bool(settings, "guides_on_output");

	if (pm->drag == DRAG_NONE)
		load_surfaces_from(pm, settings);
	pm->selected = (int)obs_data_get_int(settings, "selected");
	clamp_selected(pm);

	const char *uuid = obs_data_get_string(settings, "input");
	if (!pm->input_uuid || strcmp(pm->input_uuid, uuid) != 0) {
		bfree(pm->input_uuid);
		pm->input_uuid = bstrdup(uuid);
		pm->need_resolve = true;
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void *pm_create(obs_data_t *settings, obs_source_t *source)
{
	struct pm_source *pm = bzalloc(sizeof(struct pm_source));
	pm->self = source;
	pthread_mutex_init(&pm->mutex, NULL);

	obs_enter_graphics();
	pm->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	pm->tmp_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	char *path = obs_module_file("effects/mapping.effect");
	pm->effect = gs_effect_create_from_file(path, NULL);
	bfree(path);
	obs_leave_graphics();

	if (!pm->effect)
		obs_log(LOG_ERROR, "Fant ikke / kunne ikke laste effects/mapping.effect");

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void pm_add_surface(in int shape)", proc_add_surface, pm);
	proc_handler_add(ph, "void pm_remove_surface()", proc_remove_surface, pm);
	proc_handler_add(ph, "void pm_toggle_shape()", proc_toggle_shape, pm);
	proc_handler_add(ph, "void pm_reset_surface()", proc_reset_surface, pm);
	proc_handler_add(ph, "void pm_set_mode(in int mode)", proc_set_mode, pm);
	proc_handler_add(ph, "void pm_get_mode(out int mode)", proc_get_mode, pm);
	proc_handler_add(ph,
			 "void pm_get_selected(out int index, out int count, out int shape, out int radius, "
			 "out bool has_mask)",
			 proc_get_selected, pm);
	proc_handler_add(ph, "void pm_set_radius(in int radius, in bool push_undo)", proc_set_radius, pm);
	proc_handler_add(ph, "void pm_push_undo()", proc_push_undo, pm);
	proc_handler_add(ph, "void pm_undo(out bool done)", proc_undo, pm);
	proc_handler_add(ph, "void pm_redo(out bool done)", proc_redo, pm);
	proc_handler_add(ph, "void pm_clear_mask()", proc_clear_mask, pm);
	proc_handler_add(ph, "void pm_wants_click(in int x, in int y, in int mods, out bool hit)", proc_wants_click,
			 pm);
	proc_handler_add(ph, "void pm_get_last_edit(out int time, out int undo_count, out int redo_count)",
			 proc_get_last_edit, pm);
	proc_handler_add(ph, "void pm_set_guides(in bool enabled)", proc_set_guides, pm);

	/* Ny kilde: start med én flate så man ser noe med en gang. */
	if (!obs_data_has_user_value(settings, "surfaces")) {
		struct pm_surface s;
		surface_set_default(&s, 0, SHAPE_QUAD, 1.0f);
		da_push_back(pm->surfaces, &s);
		save_surfaces_to(pm, settings);
	}

	pm_update(pm, settings);
	return pm;
}

static void pm_destroy(void *data)
{
	struct pm_source *pm = data;

	set_input(pm, NULL);

	obs_enter_graphics();
	gs_texrender_destroy(pm->texrender);
	gs_texrender_destroy(pm->tmp_tr);
	for (size_t i = 0; i < pm->mask_tr.num; i++)
		gs_texrender_destroy(pm->mask_tr.array[i]);
	gs_effect_destroy(pm->effect);
	obs_leave_graphics();

	da_free(pm->mask_tr);
	surfaces_clear(pm);
	da_free(pm->surfaces);
	da_free(pm->lasso);
	clear_stack((pm_string_stack *)&pm->undo);
	clear_stack((pm_string_stack *)&pm->redo);
	da_free(pm->undo);
	da_free(pm->redo);
	bfree(pm->input_uuid);
	pthread_mutex_destroy(&pm->mutex);
	bfree(pm);
}

static uint32_t pm_get_width(void *data)
{
	struct pm_source *pm = data;
	return pm->width;
}

static uint32_t pm_get_height(void *data)
{
	struct pm_source *pm = data;
	return pm->height;
}

static void pm_get_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	uint32_t w = 1920, h = 1080;
	if (obs_get_video_info(&ovi)) {
		w = ovi.base_width;
		h = ovi.base_height;
	}
	obs_data_set_default_int(settings, "width", w);
	obs_data_set_default_int(settings, "height", h);
	obs_data_set_default_int(settings, "mode", MODE_MOVE);
	obs_data_set_default_int(settings, "selected", 0);
	obs_data_set_default_string(settings, "input", "");
	obs_data_set_default_bool(settings, "guides_on_output", true);
}

static void pm_video_tick(void *data, float seconds)
{
	struct pm_source *pm = data;
	pm->rendered = false;

	pthread_mutex_lock(&pm->mutex);
	bool need = pm->need_resolve;
	char *uuid = bstrdup(pm->input_uuid ? pm->input_uuid : "");
	bool have_weak = pm->input != NULL;
	pm->need_resolve = false;
	pthread_mutex_unlock(&pm->mutex);

	obs_source_t *cur = get_input(pm);

	/* Innholdet finnes kanskje ikke ennå når prosjektet lastes: prøv igjen hvert sekund. */
	if (!cur && *uuid) {
		pm->retry_timer += seconds;
		if (pm->retry_timer >= 1.0f) {
			pm->retry_timer = 0.0f;
			need = true;
		}
	}
	if (!cur && have_weak)
		need = true;

	if (need) {
		obs_source_t *src = *uuid ? obs_get_source_by_uuid(uuid) : NULL;
		if (src != cur || !src)
			set_input(pm, src);
		obs_source_release(src);
	}

	obs_source_release(cur);
	bfree(uuid);
}

static void pm_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct pm_source *pm = data;
	obs_source_t *src = get_input(pm);
	if (src) {
		enum_callback(pm->self, src, param);
		obs_source_release(src);
	}
}

/* ------------------------------------------------------------------ */
/* Tegning: innhold og masker                                           */

static float handle_size(const struct pm_source *pm)
{
	float m = (float)(pm->width > pm->height ? pm->width : pm->height);
	float s = m * 0.008f;
	return s < 6.0f ? 6.0f : s;
}

static void render_input_texture(struct pm_source *pm, obs_source_t *input)
{
	uint32_t cx = obs_source_get_width(input);
	uint32_t cy = obs_source_get_height(input);
	gs_texrender_reset(pm->texrender);
	if (!cx || !cy)
		return;

	if (gs_texrender_begin(pm->texrender, cx, cy)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(input);
		gs_blend_state_pop();

		gs_texrender_end(pm->texrender);
	}
}

static void quad_to_pixels(const struct pm_source *pm, const struct vec2 in[4], struct vec2 out[4])
{
	for (int i = 0; i < 4; i++)
		vec2_set(&out[i], in[i].x * (float)pm->width, in[i].y * (float)pm->height);
}

static struct vec2 to_px(const struct pm_source *pm, struct vec2 p)
{
	struct vec2 r;
	vec2_set(&r, p.x * (float)pm->width, p.y * (float)pm->height);
	return r;
}

static void draw_quad_strip(const struct vec2 p[4])
{
	gs_render_start(true);
	gs_vertex2f(p[0].x, p[0].y);
	gs_vertex2f(p[1].x, p[1].y);
	gs_vertex2f(p[3].x, p[3].y);
	gs_vertex2f(p[2].x, p[2].y);
	gs_render_stop(GS_TRISTRIP);
}

/* Fyller et (gjerne konkavt) polygon med partall/oddetall-regelen:
 * hver trekant i viften inverterer målet, så overlapp oppheves. */
static void fill_polygon_xor(const struct vec2 *p, size_t n)
{
	if (n < 3)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &white);

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_INVDSTCOLOR, GS_BLEND_ZERO);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(true);
		for (size_t i = 1; i + 1 < n; i++) {
			gs_vertex2f(p[0].x, p[0].y);
			gs_vertex2f(p[i].x, p[i].y);
			gs_vertex2f(p[i + 1].x, p[i + 1].y);
		}
		gs_render_stop(GS_TRIS);
	}
	gs_blend_state_pop();
}

/* Tegner maske-teksturen for én flate (hvit = synlig). */
static void render_mask(struct pm_source *pm, gs_texrender_t *tr, const struct pm_surface *s)
{
	uint32_t w = pm->width, h = pm->height;
	bool any_add = s->mask.num >= 3;
	for (size_t i = 0; i < s->strokes.num; i++)
		if (!s->strokes.array[i].subtract)
			any_add = true;

	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return;
	struct vec4 clear;
	float v = any_add ? 0.0f : 1.0f;
	vec4_set(&clear, v, v, v, v);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);
	if (s->mask.num >= 3)
		fill_polygon_xor(s->mask.array, s->mask.num);
	gs_texrender_end(tr);

	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	for (size_t k = 0; k < s->strokes.num; k++) {
		const struct pm_stroke *st = &s->strokes.array[k];

		gs_texrender_reset(pm->tmp_tr);
		if (!gs_texrender_begin(pm->tmp_tr, w, h))
			continue;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);
		fill_polygon_xor(st->pts.array, st->pts.num);
		gs_texrender_end(pm->tmp_tr);

		/* Legg strøket oppå masken (begin uten clear beholder innholdet). */
		gs_texrender_reset(tr);
		if (!gs_texrender_begin(tr, w, h))
			continue;
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_enable_blending(true);
		if (st->subtract)
			gs_blend_function(GS_BLEND_ZERO, GS_BLEND_INVSRCCOLOR);
		else
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		gs_texture_t *tex = gs_texrender_get_texture(pm->tmp_tr);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(tex, 0, w, h);
		gs_blend_state_pop();
		gs_texrender_end(tr);
	}
}

static void render_all_masks(struct pm_source *pm)
{
	while (pm->mask_tr.num < pm->surfaces.num) {
		gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		da_push_back(pm->mask_tr, &tr);
	}
	for (size_t i = 0; i < pm->surfaces.num; i++) {
		const struct pm_surface *s = &pm->surfaces.array[i];
		if (surface_has_mask(s))
			render_mask(pm, pm->mask_tr.array[i], s);
	}
}

static void set_rows(gs_effect_t *effect, const char *prefix, const pm_mat3 m)
{
	char name[8];
	for (int r = 0; r < 3; r++) {
		struct vec4 v;
		vec4_set(&v, (float)m[r * 3 + 0], (float)m[r * 3 + 1], (float)m[r * 3 + 2], 0.0f);
		snprintf(name, sizeof(name), "%srow%d", prefix, r);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, name), &v);
	}
}

static void draw_surface(struct pm_source *pm, gs_texture_t *tex, size_t index)
{
	const struct pm_surface *s = &pm->surfaces.array[index];
	struct vec2 px[4];
	quad_to_pixels(pm, s->dst, px);

	pm_mat3 to_dst, to_src, inv_dst;
	square_to_quad(px, to_dst);
	square_to_quad(s->src, to_src);
	if (!mat3_inverse(to_dst, inv_dst))
		return;

	float qw, qh;
	quad_size(px, &qw, &qh);
	float span = qw > qh ? qw : qh;

	gs_effect_t *e = pm->effect;
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"), tex);
	set_rows(e, "d", inv_dst);
	set_rows(e, "s", to_src);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "shape"), (float)s->shape);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "feather"), 3.0f / span);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "radius"), s->radius);
	struct vec2 qs, os;
	vec2_set(&qs, qw, qh);
	vec2_set(&os, (float)pm->width, (float)pm->height);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "qsize"), &qs);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, "out_size"), &os);

	bool masked = surface_has_mask(s) && index < pm->mask_tr.num;
	gs_texture_t *mtex = masked ? gs_texrender_get_texture(pm->mask_tr.array[index]) : NULL;
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "masktex"), mtex ? mtex : tex);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "use_mask"), mtex ? 1.0f : 0.0f);

	while (gs_effect_loop(e, "Draw"))
		draw_quad_strip(px);
}

/* ------------------------------------------------------------------ */
/* Tegning: hjelpelinjer                                                */

static void set_color(gs_eparam_t *color, uint32_t abgr)
{
	struct vec4 c;
	vec4_from_rgba(&c, abgr);
	gs_effect_set_vec4(color, &c);
}

static void draw_line(struct vec2 a, struct vec2 b, float t)
{
	float dx = b.x - a.x, dy = b.y - a.y;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f)
		return;
	float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
	gs_render_start(true);
	gs_vertex2f(a.x + nx, a.y + ny);
	gs_vertex2f(b.x + nx, b.y + ny);
	gs_vertex2f(a.x - nx, a.y - ny);
	gs_vertex2f(b.x - nx, b.y - ny);
	gs_render_stop(GS_TRISTRIP);
}

static void draw_box(struct vec2 c, float half)
{
	gs_render_start(true);
	gs_vertex2f(c.x - half, c.y - half);
	gs_vertex2f(c.x + half, c.y - half);
	gs_vertex2f(c.x - half, c.y + half);
	gs_vertex2f(c.x + half, c.y + half);
	gs_render_stop(GS_TRISTRIP);
}

static void draw_diamond(struct vec2 c, float half)
{
	gs_render_start(true);
	gs_vertex2f(c.x, c.y - half);
	gs_vertex2f(c.x + half, c.y);
	gs_vertex2f(c.x - half, c.y);
	gs_vertex2f(c.x, c.y + half);
	gs_render_stop(GS_TRISTRIP);
}

static void draw_polyline(const struct vec2 *p, size_t n, bool closed, float t)
{
	for (size_t i = 0; i + 1 < n; i++)
		draw_line(p[i], p[i + 1], t);
	if (closed && n > 2)
		draw_line(p[n - 1], p[0], t);
}

/* Omrisset av flatens form (rund eller avrundet) gjennom perspektivet. */
static void draw_shape_outline(const struct pm_surface *s, const struct vec2 px[4], float t)
{
	pm_mat3 m;
	square_to_quad(px, m);

	if (s->shape == SHAPE_ROUND) {
		const int segs = 64;
		struct vec2 prev = map_unit(m, 1.0f, 0.5f);
		for (int i = 1; i <= segs; i++) {
			float a = (float)i / (float)segs * 6.2831853f;
			struct vec2 cur = map_unit(m, 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a));
			draw_line(prev, cur, t);
			prev = cur;
		}
		return;
	}

	/* Avrundet rektangel i "pikselaktige" flatekoordinater (samme som i shaderen). */
	float qw, qh;
	quad_size(px, &qw, &qh);
	float r = s->radius * 0.5f * (qw < qh ? qw : qh);
	/* Hjørnesentre: øverst høyre, øverst venstre, nederst venstre, nederst høyre. */
	const float cx[4] = {qw - r, r, r, qw - r};
	const float cy[4] = {r, r, qh - r, qh - r};
	const float start[4] = {0.0f, 1.5707963f, 3.1415926f, 4.7123890f};
	struct vec2 first = {0}, prev = {0};
	bool have = false;
	for (int c = 0; c < 4; c++) {
		for (int i = 0; i <= 12; i++) {
			/* y peker nedover, så vinkelen går "baklengs" for å følge klokka. */
			float a = start[c] + (float)i / 12.0f * 1.5707963f;
			float x = cx[c] + r * cosf(a);
			float y = cy[c] - r * sinf(a);
			struct vec2 cur = map_unit(m, x / qw, y / qh);
			if (have)
				draw_line(prev, cur, t);
			else
				first = cur;
			prev = cur;
			have = true;
		}
	}
	if (have)
		draw_line(prev, first, t);
}

static void draw_move_overlay(struct pm_source *pm, gs_eparam_t *color, gs_technique_t *tech, size_t index,
			      bool selected, bool crop)
{
	const struct pm_surface *s = &pm->surfaces.array[index];
	struct vec2 px[4];
	quad_to_pixels(pm, crop ? s->src : s->dst, px);
	float hs = handle_size(pm);
	float t = fmaxf(2.0f, hs * 0.35f);
	bool shaped = !crop && (s->shape == SHAPE_ROUND || s->radius > 0.001f);

	/* ABGR: gul (Move), cyan (Utsnitt), grå (ikke valgt) */
	set_color(color, selected ? (crop ? 0xFFFFD000 : 0xFF00D0FF) : 0xFFA0A0A0);
	gs_technique_begin_pass(tech, 0);
	draw_polyline(px, 4, true, shaped ? fmaxf(1.0f, t * 0.4f) : t);
	if (shaped)
		draw_shape_outline(s, px, t);
	/* Kanthåndtak */
	for (int i = 0; i < 4; i++) {
		struct vec2 mid;
		vec2_add(&mid, &px[i], &px[(i + 1) % 4]);
		vec2_mulf(&mid, &mid, 0.5f);
		draw_box(mid, selected ? hs * 0.75f : hs * 0.5f);
	}
	gs_technique_end_pass(tech);

	/* Hjørnehåndtak. Øverste venstre er en rombe, så man ser orienteringen. */
	set_color(color, 0xFFFFFFFF);
	gs_technique_begin_pass(tech, 0);
	for (int i = 0; i < 4; i++) {
		float half = selected ? hs : hs * 0.7f;
		if (i == 0)
			draw_diamond(px[i], half * 1.5f);
		else
			draw_box(px[i], half);
	}
	gs_technique_end_pass(tech);
}

static void draw_points_px(struct pm_source *pm, gs_eparam_t *color, gs_technique_t *tech, const struct vec2 *pts,
			   size_t n, bool closed, float t, uint32_t abgr)
{
	if (n < 2)
		return;
	struct vec2 *tmp = bmalloc(sizeof(struct vec2) * n);
	for (size_t i = 0; i < n; i++)
		tmp[i] = to_px(pm, pts[i]);
	set_color(color, abgr);
	gs_technique_begin_pass(tech, 0);
	draw_polyline(tmp, n, closed, t);
	gs_technique_end_pass(tech);
	bfree(tmp);
}

static void draw_mask_overlay(struct pm_source *pm, gs_eparam_t *color, gs_technique_t *tech, size_t index,
			      bool selected)
{
	const struct pm_surface *s = &pm->surfaces.array[index];
	struct vec2 px[4];
	quad_to_pixels(pm, s->dst, px);
	float hs = handle_size(pm);
	float t = fmaxf(2.0f, hs * 0.35f);
	const uint32_t red = 0xFF2828FF, pink = 0xFFB070FF;

	/* Flatens ramme tynt i grått, så man ser hva masken hører til. */
	set_color(color, selected ? 0xFFA0A0A0 : 0xFF707070);
	gs_technique_begin_pass(tech, 0);
	draw_polyline(px, 4, true, fmaxf(1.0f, t * 0.4f));
	gs_technique_end_pass(tech);
	if (!selected)
		return;

	/* Frihåndsstrøk: rød = lagt til, rosa = visket ut. */
	for (size_t k = 0; k < s->strokes.num; k++) {
		const struct pm_stroke *st = &s->strokes.array[k];
		draw_points_px(pm, color, tech, st->pts.array, st->pts.num, true, fmaxf(1.0f, t * 0.5f),
			       st->subtract ? pink : red);
	}

	/* Polygon-masken, eller flatens ramme i rødt hvis det ikke finnes noen ennå. */
	if (s->mask.num >= 3) {
		draw_points_px(pm, color, tech, s->mask.array, s->mask.num, true, t, red);
		set_color(color, 0xFFFFFFFF);
		gs_technique_begin_pass(tech, 0);
		for (size_t i = 0; i < s->mask.num; i++)
			draw_box(to_px(pm, s->mask.array[i]), hs * 0.8f);
		gs_technique_end_pass(tech);
		set_color(color, red);
		gs_technique_begin_pass(tech, 0);
		for (size_t i = 0; i < s->mask.num; i++)
			draw_box(to_px(pm, s->mask.array[i]), hs * 0.45f);
		gs_technique_end_pass(tech);
	} else if (s->strokes.num == 0) {
		set_color(color, red);
		gs_technique_begin_pass(tech, 0);
		draw_polyline(px, 4, true, t);
		gs_technique_end_pass(tech);
	}

	/* Strøket som tegnes akkurat nå. */
	if (pm->drag == DRAG_LASSO)
		draw_points_px(pm, color, tech, pm->lasso.array, pm->lasso.num, false, t,
			       pm->lasso_subtract ? pink : red);
}

static void draw_overlays(struct pm_source *pm)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_technique_begin(tech);
	/* Valgt flate tegnes sist, så den ligger øverst. */
	for (int pass = 0; pass < 2; pass++) {
		for (size_t i = 0; i < pm->surfaces.num; i++) {
			bool sel = (int)i == pm->selected;
			if (sel != (pass == 1))
				continue;
			if (pm->mode == MODE_MASK)
				draw_mask_overlay(pm, color, tech, i, sel);
			else
				draw_move_overlay(pm, color, tech, i, sel, pm->mode == MODE_CROP);
		}
	}
	gs_technique_end(tech);
}

static void draw_placeholder(struct pm_source *pm, const struct pm_surface *s)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF404040);
	struct vec2 px[4];
	quad_to_pixels(pm, s->dst, px);
	while (gs_effect_loop(solid, "Solid"))
		draw_quad_strip(px);
}

static void pm_video_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct pm_source *pm = data;
	if (!pm->effect)
		return;

	obs_source_t *input = get_input(pm);

	pthread_mutex_lock(&pm->mutex);

	/* Innhold og masker tegnes bare én gang per bilde, selv om kilden vises flere steder. */
	if (!pm->rendered) {
		pm->rendered = true;
		if (input)
			render_input_texture(pm, input);
		else
			gs_texrender_reset(pm->texrender);
		render_all_masks(pm);
	}
	gs_texture_t *tex = input ? gs_texrender_get_texture(pm->texrender) : NULL;

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	if (pm->mode == MODE_CROP) {
		if (tex) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(def, "image"), tex);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(tex, 0, pm->width, pm->height);
		}
	} else {
		for (size_t i = 0; i < pm->surfaces.num; i++) {
			if (tex)
				draw_surface(pm, tex, i);
			else if (pm->mode != MODE_SHOW)
				draw_placeholder(pm, &pm->surfaces.array[i]);
		}
	}

	gs_enable_framebuffer_srgb(false);

	/* Hjelpelinjer: alltid i panelet og i OBS sitt eget bilde (forhåndsvisning/program).
	 * I projektorvinduer bare hvis «Vis hjelpelinjer på projektor» er på. */
	bool to_window = gs_get_render_target() == NULL;
	bool guides = pm->mode != MODE_SHOW && (pm_editor_render || !to_window || pm->guides_on_output);
	if (guides)
		draw_overlays(pm);

	gs_enable_framebuffer_srgb(prev_srgb);

	pthread_mutex_unlock(&pm->mutex);
	obs_source_release(input);
}

/* ------------------------------------------------------------------ */
/* Mus                                                                  */

static struct vec2 *edit_points(struct pm_source *pm, size_t idx)
{
	struct pm_surface *s = &pm->surfaces.array[idx];
	return pm->mode == MODE_CROP ? s->src : s->dst;
}

/* Treff for Move/Utsnitt: hjørner, så kanter, så innsiden. Valgt flate først. */
static bool hit_test_warp(struct pm_source *pm, float px, float py, int *surface, int *corner)
{
	float r = handle_size(pm) * 2.0f;
	int n = (int)pm->surfaces.num;

	for (int kind = 0; kind < 3; kind++) {
		for (int pass = 0; pass < 2; pass++) {
			for (int k = n - 1; k >= 0; k--) {
				int i = pass == 0 ? pm->selected : k;
				if (i < 0 || i >= n)
					continue;
				struct vec2 q[4];
				quad_to_pixels(pm, edit_points(pm, i), q);

				if (kind < 2) {
					for (int c = 0; c < 4; c++) {
						struct vec2 p = q[c];
						if (kind == 1) {
							vec2_add(&p, &q[c], &q[(c + 1) % 4]);
							vec2_mulf(&p, &p, 0.5f);
						}
						float dx = p.x - px, dy = p.y - py;
						if (dx * dx + dy * dy <= r * r) {
							*surface = i;
							*corner = kind == 0 ? c : 4 + c;
							return true;
						}
					}
				} else if (point_in_poly(q, 4, px, py)) {
					*surface = i;
					*corner = -1;
					return true;
				}
				if (pass == 0)
					break;
			}
		}
	}
	return false;
}

static int hit_mask_point(struct pm_source *pm, const struct pm_surface *s, struct vec2 p)
{
	float r = handle_size(pm) * 2.0f;
	for (size_t i = 0; i < s->mask.num; i++) {
		struct vec2 q = to_px(pm, s->mask.array[i]);
		if (vec2_dist(&q, &p) <= r)
			return (int)i;
	}
	return -1;
}

/* Kantsegment i maskepolygonet, eller i flatens ramme hvis det ikke finnes noe polygon. */
static int hit_mask_edge(struct pm_source *pm, const struct pm_surface *s, struct vec2 p)
{
	float r = handle_size(pm) * 1.5f;
	bool poly = s->mask.num >= 3;
	size_t n = poly ? s->mask.num : 4;
	for (size_t i = 0; i < n; i++) {
		struct vec2 a = to_px(pm, poly ? s->mask.array[i] : s->dst[i]);
		struct vec2 b = to_px(pm, poly ? s->mask.array[(i + 1) % n] : s->dst[(i + 1) % n]);
		if (dist_to_segment(p, a, b) <= r)
			return (int)i;
	}
	return -1;
}

static int surface_under(struct pm_source *pm, struct vec2 p)
{
	int n = (int)pm->surfaces.num;
	struct vec2 q[4];
	if (pm->selected >= 0 && pm->selected < n) {
		quad_to_pixels(pm, pm->surfaces.array[pm->selected].dst, q);
		if (point_in_poly(q, 4, p.x, p.y))
			return pm->selected;
	}
	for (int i = n - 1; i >= 0; i--) {
		quad_to_pixels(pm, pm->surfaces.array[i].dst, q);
		if (point_in_poly(q, 4, p.x, p.y))
			return i;
	}
	return -1;
}

static struct pm_surface *selected_surface(struct pm_source *pm)
{
	if (pm->selected < 0 || pm->selected >= (int)pm->surfaces.num)
		return NULL;
	return &pm->surfaces.array[pm->selected];
}

/* Vil et klikk her gjøre noe? Forhåndsvisningen bruker dette for å avgjøre
 * om klikket går til mappingen eller til OBS. Kalles med mutex låst. */
static bool wants_click(struct pm_source *pm, float x, float y, uint32_t mods)
{
	if (pm->mode == MODE_SHOW)
		return false;
	struct vec2 p;
	vec2_set(&p, x, y);
	if (pm->mode == MODE_MASK) {
		if (mods & INTERACT_ALT_KEY)
			return selected_surface(pm) && x >= 0.0f && y >= 0.0f && x <= (float)pm->width &&
			       y <= (float)pm->height;
		struct pm_surface *s = selected_surface(pm);
		if (s && (hit_mask_point(pm, s, p) >= 0 || hit_mask_edge(pm, s, p) >= 0))
			return true;
		return surface_under(pm, p) >= 0;
	}
	int si, c;
	return hit_test_warp(pm, x, y, &si, &c);
}

static struct vec2 norm_point(struct pm_source *pm, struct vec2 p)
{
	struct vec2 n;
	vec2_set(&n, p.x / (float)pm->width, p.y / (float)pm->height);
	return n;
}

static void mask_mouse_down(struct pm_source *pm, struct vec2 p, uint32_t mods, uint32_t clicks)
{
	struct pm_surface *s = selected_surface(pm);

	/* Alt = tegn ny maske (legg til), Alt+Shift = visk ut. */
	if ((mods & INTERACT_ALT_KEY) && s) {
		pm->drag = DRAG_LASSO;
		pm->lasso_subtract = (mods & INTERACT_SHIFT_KEY) != 0;
		da_resize(pm->lasso, 0);
		struct vec2 n = norm_point(pm, p);
		da_push_back(pm->lasso, &n);
		return;
	}

	if (s && clicks >= 2) {
		/* Dobbeltklikk på punkt = fjern det. */
		int pt = hit_mask_point(pm, s, p);
		if (pt >= 0) {
			if (s->mask.num > 3) {
				push_undo(pm);
				da_erase(s->mask, (size_t)pt);
				save_to_source(pm);
			}
			pm->drag = DRAG_NONE;
			return;
		}
		/* Dobbeltklikk på linjen = nytt punkt. */
		int edge = hit_mask_edge(pm, s, p);
		if (edge >= 0) {
			push_undo(pm);
			if (s->mask.num < 3) {
				da_resize(s->mask, 0);
				for (int c = 0; c < 4; c++)
					da_push_back(s->mask, &s->dst[c]);
			}
			struct vec2 n = norm_point(pm, p);
			da_insert(s->mask, (size_t)edge + 1, &n);
			save_to_source(pm);
			pm->drag = DRAG_MASK_POINT;
			pm->drag_surface = pm->selected;
			pm->drag_corner = edge + 1;
			pm->drag_last = n;
			return;
		}
	}

	if (s) {
		int pt = hit_mask_point(pm, s, p);
		if (pt >= 0) {
			push_undo(pm);
			pm->drag = DRAG_MASK_POINT;
			pm->drag_surface = pm->selected;
			pm->drag_corner = pt;
			pm->drag_last = norm_point(pm, p);
			return;
		}
	}

	int under = surface_under(pm, p);
	if (under >= 0 && under != pm->selected) {
		pm->selected = under;
		save_to_source(pm);
	}
}

static void pm_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			   uint32_t click_count)
{
	struct pm_source *pm = data;
	if (type != MOUSE_LEFT)
		return;

	pthread_mutex_lock(&pm->mutex);
	if (pm->mode == MODE_SHOW) {
		pthread_mutex_unlock(&pm->mutex);
		return;
	}

	struct vec2 p;
	vec2_set(&p, (float)event->x, (float)event->y);

	if (!mouse_up) {
		if (pm->mode == MODE_MASK) {
			mask_mouse_down(pm, p, event->modifiers, click_count);
		} else {
			int s, c;
			if (hit_test_warp(pm, p.x, p.y, &s, &c)) {
				push_undo(pm);
				pm->selected = s;
				pm->drag = DRAG_WARP;
				pm->drag_surface = s;
				pm->drag_corner = c;
				pm->drag_last = norm_point(pm, p);
			}
		}
	} else if (pm->drag == DRAG_LASSO) {
		struct pm_surface *s = selected_surface(pm);
		if (s && pm->lasso.num >= 3) {
			push_undo(pm);
			struct pm_stroke st;
			memset(&st, 0, sizeof(st));
			st.subtract = pm->lasso_subtract;
			da_copy(st.pts, pm->lasso);
			da_push_back(s->strokes, &st);
			save_to_source(pm);
		}
		da_resize(pm->lasso, 0);
		pm->drag = DRAG_NONE;
	} else if (pm->drag != DRAG_NONE) {
		pm->drag = DRAG_NONE;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void pm_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	UNUSED_PARAMETER(mouse_leave);
	struct pm_source *pm = data;

	pthread_mutex_lock(&pm->mutex);
	struct vec2 p;
	vec2_set(&p, (float)event->x, (float)event->y);
	struct vec2 cur = norm_point(pm, p);

	if (pm->drag == DRAG_LASSO && pm->lasso.num) {
		struct vec2 last = pm->lasso.array[pm->lasso.num - 1];
		float dx = (cur.x - last.x) * (float)pm->width, dy = (cur.y - last.y) * (float)pm->height;
		if (dx * dx + dy * dy >= 4.0f && pm->lasso.num < PM_MAX_LASSO)
			da_push_back(pm->lasso, &cur);
	} else if (pm->drag == DRAG_MASK_POINT && pm->drag_surface < (int)pm->surfaces.num) {
		struct pm_surface *s = &pm->surfaces.array[pm->drag_surface];
		if (pm->drag_corner >= 0 && pm->drag_corner < (int)s->mask.num)
			s->mask.array[pm->drag_corner] = cur;
	} else if (pm->drag == DRAG_WARP && pm->drag_surface < (int)pm->surfaces.num) {
		struct vec2 delta;
		vec2_sub(&delta, &cur, &pm->drag_last);
		pm->drag_last = cur;

		struct vec2 *pts = edit_points(pm, pm->drag_surface);
		if (pm->drag_corner >= 4) {
			int a = pm->drag_corner - 4, b = (a + 1) % 4;
			vec2_add(&pts[a], &pts[a], &delta);
			vec2_add(&pts[b], &pts[b], &delta);
		} else if (pm->drag_corner >= 0) {
			vec2_add(&pts[pm->drag_corner], &pts[pm->drag_corner], &delta);
		} else {
			for (int c = 0; c < 4; c++)
				vec2_add(&pts[c], &pts[c], &delta);
		}
	}
	pthread_mutex_unlock(&pm->mutex);
}

/* ------------------------------------------------------------------ */
/* Handlinger (knapper, panel, hurtigtaster)                            */

static void action_add_surface(struct pm_source *pm, int shape)
{
	pthread_mutex_lock(&pm->mutex);
	push_undo(pm);
	struct pm_surface s;
	surface_set_default(&s, pm->surfaces.num, shape, pm_aspect(pm));
	da_push_back(pm->surfaces, &s);
	pm->selected = (int)pm->surfaces.num - 1;
	save_to_source(pm);
	pthread_mutex_unlock(&pm->mutex);
}

static void action_remove_surface(struct pm_source *pm)
{
	pthread_mutex_lock(&pm->mutex);
	if (pm->selected >= 0 && pm->selected < (int)pm->surfaces.num) {
		push_undo(pm);
		surface_free(&pm->surfaces.array[pm->selected]);
		da_erase(pm->surfaces, pm->selected);
		clamp_selected(pm);
		pm->drag = DRAG_NONE;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void action_toggle_shape(struct pm_source *pm)
{
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	if (s) {
		push_undo(pm);
		s->shape = s->shape == SHAPE_ROUND ? SHAPE_QUAD : SHAPE_ROUND;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void action_reset_surface(struct pm_source *pm)
{
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	if (s) {
		push_undo(pm);
		struct pm_surface def;
		surface_set_default(&def, 0, s->shape, pm_aspect(pm));
		if (pm->mode == MODE_CROP)
			memcpy(s->src, def.src, sizeof(def.src));
		else
			memcpy(s->dst, def.dst, sizeof(def.dst));
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void action_clear_mask(struct pm_source *pm)
{
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	if (s && surface_has_mask(s)) {
		push_undo(pm);
		da_resize(s->mask, 0);
		for (size_t i = 0; i < s->strokes.num; i++)
			da_free(s->strokes.array[i].pts);
		da_resize(s->strokes, 0);
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void action_set_mode(struct pm_source *pm, int mode)
{
	pthread_mutex_lock(&pm->mutex);
	pm->mode = mode;
	pm->drag = DRAG_NONE;
	da_resize(pm->lasso, 0);
	obs_data_t *settings = obs_source_get_settings(pm->self);
	obs_data_set_int(settings, "mode", mode);
	obs_data_release(settings);
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_add_surface(void *data, calldata_t *cd)
{
	action_add_surface(data, (int)calldata_int(cd, "shape"));
}

static void proc_remove_surface(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	action_remove_surface(data);
}

static void proc_toggle_shape(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	action_toggle_shape(data);
}

static void proc_reset_surface(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	action_reset_surface(data);
}

static void proc_clear_mask(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	action_clear_mask(data);
}

static void proc_set_mode(void *data, calldata_t *cd)
{
	action_set_mode(data, (int)calldata_int(cd, "mode"));
}

static void proc_get_mode(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	calldata_set_int(cd, "mode", pm->mode);
}

static void proc_get_selected(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	calldata_set_int(cd, "index", s ? pm->selected : -1);
	calldata_set_int(cd, "count", (long long)pm->surfaces.num);
	calldata_set_int(cd, "shape", s ? s->shape : 0);
	calldata_set_int(cd, "radius", s ? (long long)lroundf(s->radius * 100.0f) : 0);
	calldata_set_bool(cd, "has_mask", s ? surface_has_mask(s) : false);
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_set_radius(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	if (s) {
		if (calldata_bool(cd, "push_undo"))
			push_undo(pm);
		float r = (float)calldata_int(cd, "radius") / 100.0f;
		s->radius = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_push_undo(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	push_undo(pm);
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_undo(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "done", do_undo(data, false));
}

static void proc_redo(void *data, calldata_t *cd)
{
	calldata_set_bool(cd, "done", do_undo(data, true));
}

static void proc_wants_click(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	bool hit = wants_click(pm, (float)calldata_int(cd, "x"), (float)calldata_int(cd, "y"),
			       (uint32_t)calldata_int(cd, "mods"));
	pthread_mutex_unlock(&pm->mutex);
	calldata_set_bool(cd, "hit", hit);
}

static void proc_get_last_edit(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	calldata_set_int(cd, "time", (long long)(pm->last_edit / 1000000ULL));
	calldata_set_int(cd, "undo_count", (long long)pm->undo.num);
	calldata_set_int(cd, "redo_count", (long long)pm->redo.num);
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_set_guides(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	pm->guides_on_output = calldata_bool(cd, "enabled");
	obs_data_t *settings = obs_source_get_settings(pm->self);
	obs_data_set_bool(settings, "guides_on_output", pm->guides_on_output);
	obs_data_release(settings);
	pthread_mutex_unlock(&pm->mutex);
}

/* ------------------------------------------------------------------ */
/* Egenskaper                                                           */

static bool add_source_to_list(void *param, obs_source_t *src)
{
	obs_property_t *list = param;
	uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & OBS_SOURCE_VIDEO))
		return true;
	if (strcmp(obs_source_get_unversioned_id(src), PM_ID) == 0)
		return true;
	const char *name = obs_source_get_name(src);
	if (!name || !*name)
		return true;
	obs_property_list_add_string(list, name, obs_source_get_uuid(src));
	return true;
}

#define BUTTON_CB(name, call)                                                    \
	static bool name(obs_properties_t *props, obs_property_t *p, void *data) \
	{                                                                        \
		UNUSED_PARAMETER(props);                                         \
		UNUSED_PARAMETER(p);                                             \
		call;                                                            \
		return false;                                                    \
	}

BUTTON_CB(add_surface_clicked, action_add_surface(data, SHAPE_QUAD))
BUTTON_CB(add_round_clicked, action_add_surface(data, SHAPE_ROUND))
BUTTON_CB(toggle_shape_clicked, action_toggle_shape(data))
BUTTON_CB(remove_surface_clicked, action_remove_surface(data))
BUTTON_CB(reset_surface_clicked, action_reset_surface(data))
BUTTON_CB(clear_mask_clicked, action_clear_mask(data))

static obs_properties_t *pm_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, "input", "Innhold (scene eller kilde)",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, "— Velg —", "");
	obs_enum_scenes(add_source_to_list, list);
	obs_enum_sources(add_source_to_list, list);

	obs_property_t *mode =
		obs_properties_add_list(props, "mode", "Modus", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "Move (dra hjørner og kanter)", MODE_MOVE);
	obs_property_list_add_int(mode, "Mask (tegn masker)", MODE_MASK);
	obs_property_list_add_int(mode, "Utsnitt (velg del av innholdet)", MODE_CROP);
	obs_property_list_add_int(mode, "Ferdig (skjul hjelpelinjer)", MODE_SHOW);

	obs_properties_add_bool(props, "guides_on_output", "Vis hjelpelinjer på projektor");

	obs_properties_add_button(props, "add_surface", "+ Legg til firkantet flate", add_surface_clicked);
	obs_properties_add_button(props, "add_round", "+ Legg til rund flate", add_round_clicked);
	obs_properties_add_button(props, "toggle_shape", "Gjør valgt flate rund / firkantet", toggle_shape_clicked);
	obs_properties_add_button(props, "remove_surface", "Fjern valgt flate", remove_surface_clicked);
	obs_properties_add_button(props, "reset_surface", "Nullstill valgt flate", reset_surface_clicked);
	obs_properties_add_button(props, "clear_mask", "Fjern maske på valgt flate", clear_mask_clicked);

	obs_properties_add_int(props, "width", "Projektorbredde (px)", 16, 16384, 1);
	obs_properties_add_int(props, "height", "Projektorhøyde (px)", 16, 16384, 1);

	obs_properties_add_text(props, "help",
				"Rediger direkte i forhåndsvisningen eller i panelet «Projection Mapping». "
				"Tab bytter mellom Move, Mask og Ferdig. Cmd/Ctrl+Z angrer.",
				OBS_TEXT_INFO);

	return props;
}

struct obs_source_info projection_mapping_source_info = {
	.id = PM_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.get_name = pm_get_name,
	.create = pm_create,
	.destroy = pm_destroy,
	.update = pm_update,
	.get_defaults = pm_get_defaults,
	.get_properties = pm_get_properties,
	.get_width = pm_get_width,
	.get_height = pm_get_height,
	.video_tick = pm_video_tick,
	.video_render = pm_video_render,
	.enum_active_sources = pm_enum_active_sources,
	.mouse_click = pm_mouse_click,
	.mouse_move = pm_mouse_move,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
};
