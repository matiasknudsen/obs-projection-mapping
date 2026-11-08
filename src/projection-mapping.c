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
	DRAG_HOLE_POINT, /* et punkt i et hull */
};

#define PM_PROGRAM "__program__"
#define PM_RESTART_CHECK 0.5f

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
	DARRAY(struct pm_stroke) holes;   /* hull klikket ut midt i flaten */
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
	bool program_input; /* følg OBS sitt programbilde */
	bool need_resolve;
	float retry_timer;
	float media_timer;
	obs_weak_source_t *input;
	bool input_error; /* innholdet inneholder mapping-kilden selv */
	char *error_name;

	/* Egen "grep" om innholdet så videoer spiller selv om de er skjult. */
	pthread_mutex_t hold_mutex;
	bool is_active;
	bool is_showing;
	obs_source_t *held;

	bool snap;
	int color_mode;          /* 0 = nøyaktig kopi, 1 = lineær (sRGB), 2 = lysere (som 0.4) */
	gs_texture_t *white_tex; /* brukes når en flate ikke har maske */
	bool created;            /* ferdig opprettet (skiller lasting fra brukervalg) */
	bool fit_pending;        /* tilpass flater til innholdets størrelse når det dukker opp */
	uint32_t input_w, input_h;

	gs_texrender_t *texrender;
	gs_effect_t *effect;
	bool rendered;
	DARRAY(gs_texrender_t *) mask_tr;
	gs_texrender_t *tmp_tr;

	int drag;
	int drag_surface;
	int drag_corner; /* 0-3 hjørne, 4-7 kant, -1 hele flaten; punkt-indeks ved maske/hull */
	int drag_hole;
	struct vec2 drag_start;   /* musens startpunkt (normalisert) */
	struct vec2 drag_orig[4]; /* flatens punkter da dra startet */
	bool lasso_subtract;
	DARRAY(struct vec2) lasso;

	bool drawing_hole;
	DARRAY(struct vec2) hole_draft;
	struct vec2 hover;

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
static void proc_set_snap(void *data, calldata_t *cd);
static void proc_flip(void *data, calldata_t *cd);
static void proc_select_at(void *data, calldata_t *cd);
static void proc_finish_hole(void *data, calldata_t *cd);
static void proc_get_status(void *data, calldata_t *cd);
static void proc_fit_content(void *data, calldata_t *cd);

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

static void strokes_free(void *da_ptr)
{
	DARRAY(struct pm_stroke) *da = da_ptr;
	for (size_t i = 0; i < da->num; i++)
		da_free(da->array[i].pts);
	da_free(*da);
}

static void surface_free(struct pm_surface *s)
{
	da_free(s->mask);
	strokes_free(&s->strokes);
	strokes_free(&s->holes);
}

static void surfaces_clear(struct pm_source *pm)
{
	for (size_t i = 0; i < pm->surfaces.num; i++)
		surface_free(&pm->surfaces.array[i]);
	da_resize(pm->surfaces, 0);
}

static bool surface_has_mask(const struct pm_surface *s)
{
	return s->mask.num >= 3 || s->strokes.num > 0 || s->holes.num > 0;
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

		obs_data_array_t *holes = obs_data_get_array(item, "holes");
		size_t nh = holes ? obs_data_array_count(holes) : 0;
		for (size_t k = 0; k < nh; k++) {
			obs_data_t *ho = obs_data_array_item(holes, k);
			struct pm_stroke hole;
			memset(&hole, 0, sizeof(hole));
			hole.subtract = true;
			string_to_points(obs_data_get_string(ho, "pts"), &hole.pts);
			if (hole.pts.num >= 3)
				da_push_back(s.holes, &hole);
			else
				da_free(hole.pts);
			obs_data_release(ho);
		}
		obs_data_array_release(holes);

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

		obs_data_array_t *holes = obs_data_array_create();
		for (size_t k = 0; k < s->holes.num; k++) {
			obs_data_t *ho = obs_data_create();
			points_to_string(&str, s->holes.array[k].pts.array, s->holes.array[k].pts.num);
			obs_data_set_string(ho, "pts", str.array ? str.array : "");
			dstr_free(&str);
			obs_data_array_push_back(holes, ho);
			obs_data_release(ho);
		}
		obs_data_set_array(item, "holes", holes);
		obs_data_array_release(holes);

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
		pm->drawing_hole = false;
		da_resize(pm->hole_draft, 0);
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

/* Gi/ta tilbake vår egen aktiv/vises-referanse på innholdet. */
static void hold_release(struct pm_source *pm)
{
	pthread_mutex_lock(&pm->hold_mutex);
	if (pm->held) {
		if (pm->is_active)
			obs_source_dec_active(pm->held);
		if (pm->is_showing)
			obs_source_dec_showing(pm->held);
		obs_source_release(pm->held);
		pm->held = NULL;
	}
	pthread_mutex_unlock(&pm->hold_mutex);
}

static void hold_take(struct pm_source *pm, obs_source_t *src)
{
	pthread_mutex_lock(&pm->hold_mutex);
	pm->held = obs_source_get_ref(src);
	if (pm->held) {
		if (pm->is_active)
			obs_source_inc_active(pm->held);
		if (pm->is_showing)
			obs_source_inc_showing(pm->held);
	}
	pthread_mutex_unlock(&pm->hold_mutex);
}

static void set_input(struct pm_source *pm, obs_source_t *src)
{
	pthread_mutex_lock(&pm->mutex);
	obs_weak_source_t *old_weak = pm->input;
	pm->input = NULL;
	pthread_mutex_unlock(&pm->mutex);

	hold_release(pm);
	if (old_weak) {
		obs_source_t *old = obs_weak_source_get_source(old_weak);
		if (old) {
			obs_source_remove_active_child(pm->self, old);
			obs_source_release(old);
		}
		obs_weak_source_release(old_weak);
	}

	if (!src || src == pm->self) {
		pthread_mutex_lock(&pm->mutex);
		pm->input_error = src == pm->self;
		pthread_mutex_unlock(&pm->mutex);
		return;
	}

	if (!obs_source_add_active_child(pm->self, src)) {
		pthread_mutex_lock(&pm->mutex);
		if (!pm->input_error)
			obs_log(LOG_WARNING, "Kan ikke bruke '%s' som innhold (den inneholder selve mapping-kilden).",
				obs_source_get_name(src));
		pm->input_error = true;
		bfree(pm->error_name);
		pm->error_name = bstrdup(obs_source_get_name(src));
		pthread_mutex_unlock(&pm->mutex);
		return;
	}

	hold_take(pm, src);
	pthread_mutex_lock(&pm->mutex);
	pm->input = obs_source_get_weak_source(src);
	pm->input_error = false;
	pthread_mutex_unlock(&pm->mutex);
}

static void pm_activate(void *data)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->hold_mutex);
	pm->is_active = true;
	if (pm->held)
		obs_source_inc_active(pm->held);
	pthread_mutex_unlock(&pm->hold_mutex);
}

static void pm_deactivate(void *data)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->hold_mutex);
	pm->is_active = false;
	if (pm->held)
		obs_source_dec_active(pm->held);
	pthread_mutex_unlock(&pm->hold_mutex);
}

static void pm_show(void *data)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->hold_mutex);
	pm->is_showing = true;
	if (pm->held)
		obs_source_inc_showing(pm->held);
	pthread_mutex_unlock(&pm->hold_mutex);
}

static void pm_hide(void *data)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->hold_mutex);
	pm->is_showing = false;
	if (pm->held)
		obs_source_dec_showing(pm->held);
	pthread_mutex_unlock(&pm->hold_mutex);
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
	pm->snap = obs_data_get_bool(settings, "snap");
	pm->color_mode = (int)obs_data_get_int(settings, "color_mode");

	if (pm->drag == DRAG_NONE)
		load_surfaces_from(pm, settings);
	pm->selected = (int)obs_data_get_int(settings, "selected");
	clamp_selected(pm);

	const char *uuid = obs_data_get_string(settings, "input");
	if (!pm->input_uuid || strcmp(pm->input_uuid, uuid) != 0) {
		bfree(pm->input_uuid);
		pm->input_uuid = bstrdup(uuid);
		pm->program_input = strcmp(uuid, PM_PROGRAM) == 0;
		pm->need_resolve = true;
		if (pm->created)
			pm->fit_pending = true;
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void *pm_create(obs_data_t *settings, obs_source_t *source)
{
	struct pm_source *pm = bzalloc(sizeof(struct pm_source));
	pm->self = source;
	pthread_mutex_init(&pm->mutex, NULL);
	pthread_mutex_init(&pm->hold_mutex, NULL);

	obs_enter_graphics();
	pm->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	pm->tmp_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	{
		const uint32_t white = 0xFFFFFFFF;
		const uint8_t *data = (const uint8_t *)&white;
		pm->white_tex = gs_texture_create(1, 1, GS_RGBA, 1, &data, 0);
	}
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
	proc_handler_add(ph, "void pm_set_snap(in bool enabled)", proc_set_snap, pm);
	proc_handler_add(ph, "void pm_fit_content()", proc_fit_content, pm);
	proc_handler_add(ph, "void pm_flip(in int kind, in bool all)", proc_flip, pm);
	proc_handler_add(ph, "void pm_select_at(in int x, in int y, out bool hit)", proc_select_at, pm);
	proc_handler_add(ph, "void pm_finish_hole(in bool cancel, out bool handled)", proc_finish_hole, pm);
	proc_handler_add(ph, "void pm_get_status(out string error, out bool drawing, out bool snap)", proc_get_status,
			 pm);

	/* Ny kilde: start med én flate så man ser noe med en gang. */
	if (!obs_data_has_user_value(settings, "surfaces")) {
		struct pm_surface s;
		surface_set_default(&s, 0, SHAPE_QUAD, 1.0f);
		da_push_back(pm->surfaces, &s);
		save_surfaces_to(pm, settings);
		pm->fit_pending = true;
	}

	pm_update(pm, settings);
	pm->created = true;
	return pm;
}

static void pm_destroy(void *data)
{
	struct pm_source *pm = data;

	set_input(pm, NULL);

	obs_enter_graphics();
	gs_texrender_destroy(pm->texrender);
	gs_texrender_destroy(pm->tmp_tr);
	gs_texture_destroy(pm->white_tex);
	for (size_t i = 0; i < pm->mask_tr.num; i++)
		gs_texrender_destroy(pm->mask_tr.array[i]);
	gs_effect_destroy(pm->effect);
	obs_leave_graphics();

	da_free(pm->mask_tr);
	surfaces_clear(pm);
	da_free(pm->surfaces);
	da_free(pm->lasso);
	da_free(pm->hole_draft);
	bfree(pm->error_name);
	pthread_mutex_destroy(&pm->hold_mutex);
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
	obs_data_set_default_bool(settings, "snap", false);
	obs_data_set_default_bool(settings, "color_tags", true);
	obs_data_set_default_int(settings, "color_mode", 0);
}

/* Gjør en flate til et rett rektangel med innholdets ekte pikselstørrelse
 * (krymper hvis innholdet er større enn projektorbildet). Kalles med mutex låst. */
static void fit_surface(struct pm_source *pm, struct pm_surface *s, bool keep_center)
{
	if (!pm->input_w || !pm->input_h)
		return;
	float w = (float)pm->input_w / (float)pm->width;
	float h = (float)pm->input_h / (float)pm->height;
	float big = fmaxf(w, h);
	if (big > 0.95f) {
		w *= 0.95f / big;
		h *= 0.95f / big;
	}
	float cx = 0.5f, cy = 0.5f;
	if (keep_center) {
		cx = 0.25f * (s->dst[0].x + s->dst[1].x + s->dst[2].x + s->dst[3].x);
		cy = 0.25f * (s->dst[0].y + s->dst[1].y + s->dst[2].y + s->dst[3].y);
	}
	vec2_set(&s->dst[0], cx - w * 0.5f, cy - h * 0.5f);
	vec2_set(&s->dst[1], cx + w * 0.5f, cy - h * 0.5f);
	vec2_set(&s->dst[2], cx + w * 0.5f, cy + h * 0.5f);
	vec2_set(&s->dst[3], cx - w * 0.5f, cy + h * 0.5f);
	vec2_set(&s->src[0], 0.0f, 0.0f);
	vec2_set(&s->src[1], 1.0f, 0.0f);
	vec2_set(&s->src[2], 1.0f, 1.0f);
	vec2_set(&s->src[3], 0.0f, 1.0f);
}

/* Står flaten fortsatt der den ble lagt til (ikke flyttet av brukeren)? */
static bool surface_untouched(struct pm_source *pm, const struct pm_surface *s)
{
	for (size_t i = 0; i < 6; i++) {
		struct pm_surface def;
		surface_set_default(&def, i, s->shape, s->shape == SHAPE_ROUND ? pm_aspect(pm) : 1.0f);
		bool same = true;
		for (int c = 0; c < 4 && same; c++)
			same = fabsf(def.dst[c].x - s->dst[c].x) < 1e-4f && fabsf(def.dst[c].y - s->dst[c].y) < 1e-4f;
		if (same)
			return true;
	}
	return false;
}

static void auto_fit(struct pm_source *pm)
{
	bool changed = false;
	for (size_t i = 0; i < pm->surfaces.num; i++) {
		struct pm_surface *s = &pm->surfaces.array[i];
		if (s->shape == SHAPE_QUAD && surface_untouched(pm, s)) {
			if (!changed)
				push_undo(pm);
			fit_surface(pm, s, false);
			changed = true;
		}
	}
	if (changed)
		save_to_source(pm);
}

/* Sikkerhetsnett: start en video på nytt hvis den skal loope men har stoppet. */
static void keep_media_looping(obs_source_t *input)
{
	if (!(obs_source_get_output_flags(input) & OBS_SOURCE_CONTROLLABLE_MEDIA))
		return;
	if (obs_source_media_get_state(input) != OBS_MEDIA_STATE_ENDED)
		return;
	obs_data_t *settings = obs_source_get_settings(input);
	bool loop = obs_data_get_bool(settings, "looping") || obs_data_get_bool(settings, "loop");
	obs_data_release(settings);
	if (loop)
		obs_source_media_restart(input);
}

static void pm_video_tick(void *data, float seconds)
{
	struct pm_source *pm = data;
	pm->rendered = false;

	pthread_mutex_lock(&pm->mutex);
	bool need = pm->need_resolve;
	bool program = pm->program_input;
	char *uuid = bstrdup(pm->input_uuid ? pm->input_uuid : "");
	bool have_weak = pm->input != NULL;
	pm->need_resolve = false;
	pthread_mutex_unlock(&pm->mutex);

	obs_source_t *cur = get_input(pm);

	if (program) {
		/* Kanal 0 = OBS sitt programbilde (inkludert overganger). */
		obs_source_t *prog = obs_get_output_source(0);
		if (prog != cur) {
			pm->retry_timer += seconds;
			if (need || cur || pm->retry_timer >= 1.0f) {
				pm->retry_timer = 0.0f;
				set_input(pm, prog);
			}
		}
		obs_source_release(prog);
	} else {
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
	}

	if (cur) {
		uint32_t iw = obs_source_get_width(cur), ih = obs_source_get_height(cur);
		pthread_mutex_lock(&pm->mutex);
		pm->input_w = iw;
		pm->input_h = ih;
		if (pm->fit_pending && iw && ih) {
			pm->fit_pending = false;
			auto_fit(pm);
		}
		pthread_mutex_unlock(&pm->mutex);

		pm->media_timer += seconds;
		if (pm->media_timer >= PM_RESTART_CHECK) {
			pm->media_timer = 0.0f;
			keep_media_looping(cur);
		}
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
		/* Nøyaktig kopi: be innholdet tegne uten sRGB-omregning, slik OBS gjør med
		 * blandemetoden «SRGB av». Da blir pikslene identiske med originalen. */
		const bool prev_linear = gs_set_linear_srgb(pm->color_mode == 1);
		obs_source_video_render(input);
		gs_set_linear_srgb(prev_linear);
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
/* Legg et polygon til (eller trekk det fra) en maske som allerede er tegnet. */
static void composite_polygon(struct pm_source *pm, gs_texrender_t *tr, const struct vec2 *pts, size_t n, bool subtract)
{
	uint32_t w = pm->width, h = pm->height;
	struct vec4 clear;

	gs_texrender_reset(pm->tmp_tr);
	if (!gs_texrender_begin(pm->tmp_tr, w, h))
		return;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, 1.0f, 0.0f, 1.0f, -100.0f, 100.0f);
	fill_polygon_xor(pts, n);
	gs_texrender_end(pm->tmp_tr);

	/* begin uten clear beholder masken som allerede ligger der. */
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(true);
	if (subtract)
		gs_blend_function(GS_BLEND_ZERO, GS_BLEND_INVSRCCOLOR);
	else
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(pm->tmp_tr);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr);
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

	for (size_t k = 0; k < s->strokes.num; k++) {
		const struct pm_stroke *st = &s->strokes.array[k];
		composite_polygon(pm, tr, st->pts.array, st->pts.num, st->subtract);
	}
	/* Hull trekkes alltid fra til slutt. */
	for (size_t k = 0; k < s->holes.num; k++)
		composite_polygon(pm, tr, s->holes.array[k].pts.array, s->holes.array[k].pts.num, true);
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
	if (pm->color_mode == 1)
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"), tex);
	else
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), tex);
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
	/* Viktig: ikke bind innholdsteksturen to ganger (sRGB-innstillingen følger teksturen),
	 * ellers blir bildet for lyst. Bruk en hvit 1x1-tekstur når det ikke er noen maske. */
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "masktex"), mtex ? mtex : pm->white_tex);
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

/* Rotasjonshåndtaket: en stilk ut fra midten av øverste kant, bort fra flatens sentrum. */
static bool rotation_handle(const struct pm_source *pm, const struct vec2 px[4], struct vec2 *base, struct vec2 *pos)
{
	struct vec2 center;
	vec2_set(&center, 0.25f * (px[0].x + px[1].x + px[2].x + px[3].x),
		 0.25f * (px[0].y + px[1].y + px[2].y + px[3].y));
	vec2_add(base, &px[0], &px[1]);
	vec2_mulf(base, base, 0.5f);
	struct vec2 dir;
	vec2_sub(&dir, base, &center);
	float len = vec2_len(&dir);
	if (len < 1.0f)
		return false;
	vec2_mulf(&dir, &dir, 1.0f / len);
	float stalk = handle_size(pm) * 5.0f;
	vec2_mulf(&dir, &dir, stalk);
	vec2_add(pos, base, &dir);
	return true;
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

	/* Rotasjonshåndtak over øverste kant (bare valgt flate i Move). */
	struct vec2 rot_base, rot_pos;
	bool rot = selected && !crop && rotation_handle(pm, px, &rot_base, &rot_pos);
	if (rot) {
		gs_technique_begin_pass(tech, 0);
		draw_line(rot_base, rot_pos, fmaxf(1.0f, t * 0.5f));
		const int segs = 20;
		struct vec2 prev;
		vec2_set(&prev, rot_pos.x + hs * 1.1f, rot_pos.y);
		for (int i = 1; i <= segs; i++) {
			float a = (float)i / (float)segs * 6.2831853f;
			struct vec2 cur;
			vec2_set(&cur, rot_pos.x + hs * 1.1f * cosf(a), rot_pos.y + hs * 1.1f * sinf(a));
			draw_line(prev, cur, fmaxf(1.5f, t * 0.6f));
			prev = cur;
		}
		gs_technique_end_pass(tech);
	}

	/* Hjørnehåndtak. Øverste venstre er en rombe, så man ser orienteringen. */
	set_color(color, 0xFFFFFFFF);
	gs_technique_begin_pass(tech, 0);
	if (rot)
		draw_box(rot_pos, hs * 0.45f);
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

	/* Hull: røde omriss med punkter. */
	for (size_t k = 0; k < s->holes.num; k++) {
		const struct pm_stroke *ho = &s->holes.array[k];
		draw_points_px(pm, color, tech, ho->pts.array, ho->pts.num, true, t, red);
		set_color(color, 0xFFFFFFFF);
		gs_technique_begin_pass(tech, 0);
		for (size_t i = 0; i < ho->pts.num; i++)
			draw_box(to_px(pm, ho->pts.array[i]), hs * 0.7f);
		gs_technique_end_pass(tech);
		set_color(color, red);
		gs_technique_begin_pass(tech, 0);
		for (size_t i = 0; i < ho->pts.num; i++)
			draw_box(to_px(pm, ho->pts.array[i]), hs * 0.4f);
		gs_technique_end_pass(tech);
	}

	/* Hull som tegnes akkurat nå: linjer, strikk til musa og stort startpunkt. */
	if (pm->drawing_hole && pm->hole_draft.num) {
		draw_points_px(pm, color, tech, pm->hole_draft.array, pm->hole_draft.num, false, t, red);
		struct vec2 last = to_px(pm, pm->hole_draft.array[pm->hole_draft.num - 1]);
		struct vec2 first = to_px(pm, pm->hole_draft.array[0]);
		set_color(color, 0xFFFFFFFF);
		gs_technique_begin_pass(tech, 0);
		draw_line(last, to_px(pm, pm->hover), fmaxf(1.0f, t * 0.5f));
		draw_box(first, hs * 1.3f);
		for (size_t i = 1; i < pm->hole_draft.num; i++)
			draw_box(to_px(pm, pm->hole_draft.array[i]), hs * 0.6f);
		gs_technique_end_pass(tech);
		set_color(color, red);
		gs_technique_begin_pass(tech, 0);
		draw_box(first, hs * 0.8f);
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
	gs_enable_framebuffer_srgb(pm->color_mode != 0);

	if (pm->mode == MODE_CROP) {
		if (tex) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			if (pm->color_mode == 1)
				gs_effect_set_texture_srgb(gs_effect_get_param_by_name(def, "image"), tex);
			else
				gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
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

	/* Rotasjonshåndtaket på valgt flate (bare i Move) */
	if (pm->mode == MODE_MOVE && pm->selected >= 0 && pm->selected < n) {
		struct vec2 q[4], base, pos;
		quad_to_pixels(pm, pm->surfaces.array[pm->selected].dst, q);
		if (rotation_handle(pm, q, &base, &pos)) {
			float dx = pos.x - px, dy = pos.y - py;
			if (dx * dx + dy * dy <= r * r) {
				*surface = pm->selected;
				*corner = 8;
				return true;
			}
		}
	}

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

static bool hit_hole_point(struct pm_source *pm, const struct pm_surface *s, struct vec2 p, int *hole, int *pt)
{
	float r = handle_size(pm) * 2.0f;
	for (size_t k = 0; k < s->holes.num; k++) {
		for (size_t i = 0; i < s->holes.array[k].pts.num; i++) {
			struct vec2 q = to_px(pm, s->holes.array[k].pts.array[i]);
			if (vec2_dist(&q, &p) <= r) {
				*hole = (int)k;
				*pt = (int)i;
				return true;
			}
		}
	}
	return false;
}

static bool hit_hole_edge(struct pm_source *pm, const struct pm_surface *s, struct vec2 p, int *hole, int *edge)
{
	float r = handle_size(pm) * 1.5f;
	for (size_t k = 0; k < s->holes.num; k++) {
		const struct pm_stroke *ho = &s->holes.array[k];
		for (size_t i = 0; i < ho->pts.num; i++) {
			struct vec2 a = to_px(pm, ho->pts.array[i]);
			struct vec2 b = to_px(pm, ho->pts.array[(i + 1) % ho->pts.num]);
			if (dist_to_segment(p, a, b) <= r) {
				*hole = (int)k;
				*edge = (int)i;
				return true;
			}
		}
	}
	return false;
}

static int surface_under(struct pm_source *pm, struct vec2 p)
{
	int n = (int)pm->surfaces.num;
	struct vec2 q[4];
	bool crop = pm->mode == MODE_CROP;
	if (pm->selected >= 0 && pm->selected < n) {
		const struct pm_surface *s = &pm->surfaces.array[pm->selected];
		quad_to_pixels(pm, crop ? s->src : s->dst, q);
		if (point_in_poly(q, 4, p.x, p.y))
			return pm->selected;
	}
	for (int i = n - 1; i >= 0; i--) {
		const struct pm_surface *s = &pm->surfaces.array[i];
		quad_to_pixels(pm, crop ? s->src : s->dst, q);
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
static bool in_bounds(struct pm_source *pm, float x, float y)
{
	return x >= 0.0f && y >= 0.0f && x <= (float)pm->width && y <= (float)pm->height;
}

static bool wants_click(struct pm_source *pm, float x, float y, uint32_t mods)
{
	if (pm->mode == MODE_SHOW)
		return false;
	struct vec2 p;
	vec2_set(&p, x, y);
	if (pm->mode == MODE_MASK) {
		if (pm->drawing_hole)
			return in_bounds(pm, x, y);
		if (mods & INTERACT_ALT_KEY)
			return selected_surface(pm) && in_bounds(pm, x, y);
		struct pm_surface *s = selected_surface(pm);
		int a, b;
		if (s && (hit_mask_point(pm, s, p) >= 0 || hit_mask_edge(pm, s, p) >= 0 ||
			  hit_hole_point(pm, s, p, &a, &b) || hit_hole_edge(pm, s, p, &a, &b)))
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

static void finish_hole(struct pm_source *pm, bool cancel)
{
	struct pm_surface *s = selected_surface(pm);
	if (!cancel && s && pm->hole_draft.num >= 3) {
		push_undo(pm);
		struct pm_stroke hole;
		memset(&hole, 0, sizeof(hole));
		hole.subtract = true;
		da_copy(hole.pts, pm->hole_draft);
		da_push_back(s->holes, &hole);
		save_to_source(pm);
	}
	pm->drawing_hole = false;
	da_resize(pm->hole_draft, 0);
}

static void start_drag(struct pm_source *pm, int kind, int surface, int corner, struct vec2 p)
{
	pm->drag = kind;
	pm->drag_surface = surface;
	pm->drag_corner = corner;
	pm->drag_start = norm_point(pm, p);
	if (kind == DRAG_WARP)
		memcpy(pm->drag_orig, edit_points(pm, (size_t)surface), sizeof(pm->drag_orig));
}

static void mask_mouse_down(struct pm_source *pm, struct vec2 p, uint32_t mods, uint32_t clicks)
{
	struct pm_surface *s = selected_surface(pm);
	struct vec2 n = norm_point(pm, p);
	float r = handle_size(pm) * 2.0f;

	/* Tegner et hull: hvert klikk er et nytt punkt. Klikk på startpunktet,
	 * dobbeltklikk eller Enter lukker hullet. */
	if (pm->drawing_hole) {
		if (clicks >= 2) {
			finish_hole(pm, false);
			return;
		}
		if (pm->hole_draft.num >= 3) {
			struct vec2 first = to_px(pm, pm->hole_draft.array[0]);
			if (vec2_dist(&first, &p) <= r) {
				finish_hole(pm, false);
				return;
			}
		}
		da_push_back(pm->hole_draft, &n);
		pm->hover = n;
		return;
	}

	/* Alt = tegn ny maske (legg til), Alt+Shift = visk ut. */
	if ((mods & INTERACT_ALT_KEY) && s) {
		pm->drag = DRAG_LASSO;
		pm->lasso_subtract = (mods & INTERACT_SHIFT_KEY) != 0;
		da_resize(pm->lasso, 0);
		da_push_back(pm->lasso, &n);
		return;
	}

	if (s) {
		int hole, pt;
		/* Punkter i hull */
		if (hit_hole_point(pm, s, p, &hole, &pt)) {
			if (clicks >= 2) {
				push_undo(pm);
				struct pm_stroke *ho = &s->holes.array[hole];
				if (ho->pts.num > 3) {
					da_erase(ho->pts, (size_t)pt);
				} else {
					da_free(ho->pts);
					da_erase(s->holes, (size_t)hole);
				}
				save_to_source(pm);
				pm->drag = DRAG_NONE;
				return;
			}
			push_undo(pm);
			start_drag(pm, DRAG_HOLE_POINT, pm->selected, pt, p);
			pm->drag_hole = hole;
			return;
		}
		/* Dobbeltklikk på kanten av et hull = nytt punkt */
		if (clicks >= 2 && hit_hole_edge(pm, s, p, &hole, &pt)) {
			push_undo(pm);
			da_insert(s->holes.array[hole].pts, (size_t)pt + 1, &n);
			save_to_source(pm);
			start_drag(pm, DRAG_HOLE_POINT, pm->selected, pt + 1, p);
			pm->drag_hole = hole;
			return;
		}
	}

	if (s && clicks >= 2) {
		/* Dobbeltklikk på punkt i ytterkanten = fjern det. */
		int mp = hit_mask_point(pm, s, p);
		if (mp >= 0) {
			if (s->mask.num > 3) {
				push_undo(pm);
				da_erase(s->mask, (size_t)mp);
				save_to_source(pm);
			}
			pm->drag = DRAG_NONE;
			return;
		}
		/* Dobbeltklikk på den røde linjen = nytt punkt. */
		int edge = hit_mask_edge(pm, s, p);
		if (edge >= 0) {
			push_undo(pm);
			if (s->mask.num < 3) {
				da_resize(s->mask, 0);
				for (int c = 0; c < 4; c++)
					da_push_back(s->mask, &s->dst[c]);
			}
			da_insert(s->mask, (size_t)edge + 1, &n);
			save_to_source(pm);
			start_drag(pm, DRAG_MASK_POINT, pm->selected, edge + 1, p);
			return;
		}
		/* Dobbeltklikk midt i flaten = start et hull. */
		struct vec2 q[4];
		quad_to_pixels(pm, s->dst, q);
		if (point_in_poly(q, 4, p.x, p.y)) {
			pm->drawing_hole = true;
			da_resize(pm->hole_draft, 0);
			da_push_back(pm->hole_draft, &n);
			pm->hover = n;
			pm->drag = DRAG_NONE;
			return;
		}
	}

	if (s) {
		int mp = hit_mask_point(pm, s, p);
		if (mp >= 0) {
			push_undo(pm);
			start_drag(pm, DRAG_MASK_POINT, pm->selected, mp, p);
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
				start_drag(pm, DRAG_WARP, s, c, p);
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

/* ---- Snapping ---------------------------------------------------- */

struct snap_best {
	float dist;
	struct vec2 corr; /* korreksjon i piksler */
};

static void snap_consider(struct snap_best *b, struct vec2 from, struct vec2 to)
{
	float d = vec2_dist(&from, &to);
	if (d < b->dist) {
		b->dist = d;
		vec2_sub(&b->corr, &to, &from);
	}
}

/* Finn minste korreksjon som får ett av punktene (piksler) til å treffe
 * et hjørne/kant på en annen flate eller kanten av bildet. */
static struct vec2 snap_correction(struct pm_source *pm, const struct vec2 *pts, size_t n, int skip_surface)
{
	float w = (float)pm->width, h = (float)pm->height;
	float limit = fmaxf(8.0f, handle_size(pm) * 1.5f);
	struct snap_best best = {limit, {0.0f, 0.0f}};
	struct snap_best axis_x = {limit, {0.0f, 0.0f}}, axis_y = {limit, {0.0f, 0.0f}};
	bool crop = pm->mode == MODE_CROP;

	for (size_t i = 0; i < n; i++) {
		struct vec2 p = pts[i];

		/* Bildets hjørner og kanter */
		const float cx[2] = {0.0f, w}, cy[2] = {0.0f, h};
		for (int a = 0; a < 2; a++) {
			for (int b = 0; b < 2; b++) {
				struct vec2 c;
				vec2_set(&c, cx[a], cy[b]);
				snap_consider(&best, p, c);
			}
			if (fabsf(p.x - cx[a]) < axis_x.dist) {
				axis_x.dist = fabsf(p.x - cx[a]);
				vec2_set(&axis_x.corr, cx[a] - p.x, 0.0f);
			}
			if (fabsf(p.y - cy[a]) < axis_y.dist) {
				axis_y.dist = fabsf(p.y - cy[a]);
				vec2_set(&axis_y.corr, 0.0f, cy[a] - p.y);
			}
		}

		/* Andre flater: hjørner og kanter */
		if (crop)
			continue;
		for (size_t k = 0; k < pm->surfaces.num; k++) {
			if ((int)k == skip_surface)
				continue;
			struct vec2 q[4];
			quad_to_pixels(pm, pm->surfaces.array[k].dst, q);
			for (int c = 0; c < 4; c++)
				snap_consider(&best, p, q[c]);
			for (int c = 0; c < 4; c++) {
				struct vec2 a = q[c], b = q[(c + 1) % 4];
				float dx = b.x - a.x, dy = b.y - a.y;
				float len2 = dx * dx + dy * dy;
				if (len2 <= 0.0f)
					continue;
				float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
				if (t < 0.0f || t > 1.0f)
					continue;
				struct vec2 on;
				vec2_set(&on, a.x + t * dx, a.y + t * dy);
				/* Kanter teller litt mindre enn hjørner. */
				float d = vec2_dist(&p, &on) * 1.25f;
				if (d < best.dist) {
					best.dist = d;
					vec2_sub(&best.corr, &on, &p);
				}
			}
		}
	}

	if (best.dist < limit)
		return best.corr;
	struct vec2 corr;
	vec2_set(&corr, axis_x.dist < limit ? axis_x.corr.x : 0.0f, axis_y.dist < limit ? axis_y.corr.y : 0.0f);
	return corr;
}

static struct vec2 snap_single(struct pm_source *pm, struct vec2 n, int skip_surface)
{
	if (!pm->snap)
		return n;
	struct vec2 p = to_px(pm, n);
	struct vec2 corr = snap_correction(pm, &p, 1, skip_surface);
	vec2_add(&p, &p, &corr);
	return norm_point(pm, p);
}

/* ---- Dra ------------------------------------------------------------ */

static void drag_warp(struct pm_source *pm, struct vec2 cur, uint32_t mods)
{
	struct vec2 *pts = edit_points(pm, (size_t)pm->drag_surface);
	struct vec2 delta;
	vec2_sub(&delta, &cur, &pm->drag_start);

	/* Rotasjon rundt flatens sentrum. Shift = trinn på 15 grader. */
	if (pm->drag_corner == 8) {
		struct vec2 orig_px[4];
		quad_to_pixels(pm, pm->drag_orig, orig_px);
		struct vec2 c;
		vec2_set(&c, 0.25f * (orig_px[0].x + orig_px[1].x + orig_px[2].x + orig_px[3].x),
			 0.25f * (orig_px[0].y + orig_px[1].y + orig_px[2].y + orig_px[3].y));
		struct vec2 a = to_px(pm, pm->drag_start), b = to_px(pm, cur);
		float ang = atan2f(b.y - c.y, b.x - c.x) - atan2f(a.y - c.y, a.x - c.x);
		if (mods & INTERACT_SHIFT_KEY) {
			const float step = 0.2617994f; /* 15 grader */
			ang = roundf(ang / step) * step;
		}
		float cs = cosf(ang), sn = sinf(ang);
		for (int k = 0; k < 4; k++) {
			float x = orig_px[k].x - c.x, y = orig_px[k].y - c.y;
			struct vec2 r;
			vec2_set(&r, c.x + x * cs - y * sn, c.y + x * sn + y * cs);
			pts[k] = norm_point(pm, r);
		}
		return;
	}

	/* Shift + hjørne: skaler proporsjonalt, motsatt hjørne står fast. */
	if ((mods & INTERACT_SHIFT_KEY) && pm->drag_corner >= 0 && pm->drag_corner < 4) {
		struct vec2 orig_px[4];
		quad_to_pixels(pm, pm->drag_orig, orig_px);
		struct vec2 anchor = orig_px[(pm->drag_corner + 2) % 4];
		struct vec2 corner = orig_px[pm->drag_corner];
		struct vec2 mouse = to_px(pm, cur);
		struct vec2 start = to_px(pm, pm->drag_start);
		struct vec2 target;
		/* Hjørnets nye plass = der hjørnet var + hvor langt musa har flyttet seg. */
		vec2_sub(&target, &mouse, &start);
		vec2_add(&target, &target, &corner);
		struct vec2 v, m;
		vec2_sub(&v, &corner, &anchor);
		vec2_sub(&m, &target, &anchor);
		float vv = vec2_dot(&v, &v);
		float sc = vv > 0.0f ? vec2_dot(&m, &v) / vv : 1.0f;
		if (sc < 0.02f)
			sc = 0.02f;
		for (int c = 0; c < 4; c++) {
			struct vec2 d;
			vec2_sub(&d, &orig_px[c], &anchor);
			vec2_mulf(&d, &d, sc);
			vec2_add(&d, &d, &anchor);
			pts[c] = norm_point(pm, d);
		}
		return;
	}

	/* Hvilke punkter flyttes? */
	int idx[4], n = 0;
	if (pm->drag_corner >= 4) {
		idx[n++] = pm->drag_corner - 4;
		idx[n++] = (pm->drag_corner - 3) % 4;
	} else if (pm->drag_corner >= 0) {
		idx[n++] = pm->drag_corner;
	} else {
		for (int c = 0; c < 4; c++)
			idx[n++] = c;
	}

	struct vec2 moved[4];
	for (int c = 0; c < 4; c++)
		vec2_add(&moved[c], &pm->drag_orig[c], &delta);

	if (pm->snap) {
		struct vec2 px[4];
		for (int i = 0; i < n; i++)
			px[i] = to_px(pm, moved[idx[i]]);
		struct vec2 corr = snap_correction(pm, px, (size_t)n, pm->drag_surface);
		corr.x /= (float)pm->width;
		corr.y /= (float)pm->height;
		for (int c = 0; c < 4; c++)
			vec2_add(&moved[c], &moved[c], &corr);
	}

	for (int i = 0; i < n; i++)
		pts[idx[i]] = moved[idx[i]];
}

static void pm_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	UNUSED_PARAMETER(mouse_leave);
	struct pm_source *pm = data;

	pthread_mutex_lock(&pm->mutex);
	struct vec2 p;
	vec2_set(&p, (float)event->x, (float)event->y);
	struct vec2 cur = norm_point(pm, p);
	pm->hover = cur;

	if (pm->drag == DRAG_LASSO && pm->lasso.num) {
		struct vec2 last = pm->lasso.array[pm->lasso.num - 1];
		float dx = (cur.x - last.x) * (float)pm->width, dy = (cur.y - last.y) * (float)pm->height;
		if (dx * dx + dy * dy >= 4.0f && pm->lasso.num < PM_MAX_LASSO)
			da_push_back(pm->lasso, &cur);
	} else if (pm->drag == DRAG_MASK_POINT && pm->drag_surface < (int)pm->surfaces.num) {
		struct pm_surface *s = &pm->surfaces.array[pm->drag_surface];
		if (pm->drag_corner >= 0 && pm->drag_corner < (int)s->mask.num)
			s->mask.array[pm->drag_corner] = snap_single(pm, cur, -1);
	} else if (pm->drag == DRAG_HOLE_POINT && pm->drag_surface < (int)pm->surfaces.num) {
		struct pm_surface *s = &pm->surfaces.array[pm->drag_surface];
		if (pm->drag_hole >= 0 && pm->drag_hole < (int)s->holes.num) {
			struct pm_stroke *ho = &s->holes.array[pm->drag_hole];
			if (pm->drag_corner >= 0 && pm->drag_corner < (int)ho->pts.num)
				ho->pts.array[pm->drag_corner] = snap_single(pm, cur, -1);
		}
	} else if (pm->drag == DRAG_WARP && pm->drag_surface < (int)pm->surfaces.num) {
		drag_warp(pm, cur, event->modifiers);
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
		strokes_free(&s->strokes);
		strokes_free(&s->holes);
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
	pm->drawing_hole = false;
	da_resize(pm->hole_draft, 0);
	obs_data_t *settings = obs_source_get_settings(pm->self);
	obs_data_set_int(settings, "mode", mode);
	obs_data_release(settings);
	pthread_mutex_unlock(&pm->mutex);
}

/* Snu innholdet i en flate (hjørnene og masken står stille). */
static void flip_surface(struct pm_surface *s, int kind)
{
	struct vec2 o[4];
	memcpy(o, s->src, sizeof(o));
	/* 0 = horisontalt, 1 = vertikalt, 2 = 180 grader */
	static const int map[3][4] = {{1, 0, 3, 2}, {3, 2, 1, 0}, {2, 3, 0, 1}};
	if (kind < 0 || kind > 2)
		return;
	for (int c = 0; c < 4; c++)
		s->src[c] = o[map[kind][c]];
}

static void action_flip(struct pm_source *pm, int kind, bool all)
{
	pthread_mutex_lock(&pm->mutex);
	if (pm->surfaces.num) {
		push_undo(pm);
		if (all) {
			for (size_t i = 0; i < pm->surfaces.num; i++)
				flip_surface(&pm->surfaces.array[i], kind);
		} else if (selected_surface(pm)) {
			flip_surface(selected_surface(pm), kind);
		}
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_flip(void *data, calldata_t *cd)
{
	action_flip(data, (int)calldata_int(cd, "kind"), calldata_bool(cd, "all"));
}

static void proc_select_at(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	struct vec2 p;
	vec2_set(&p, (float)calldata_int(cd, "x"), (float)calldata_int(cd, "y"));
	int under = surface_under(pm, p);
	if (under >= 0 && under != pm->selected) {
		pm->selected = under;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
	calldata_set_bool(cd, "hit", under >= 0);
}

static void proc_finish_hole(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	bool handled = pm->drawing_hole;
	if (handled)
		finish_hole(pm, calldata_bool(cd, "cancel"));
	pthread_mutex_unlock(&pm->mutex);
	calldata_set_bool(cd, "handled", handled);
}

static void proc_get_status(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	calldata_set_string(cd, "error", pm->input_error && pm->error_name ? pm->error_name : "");
	calldata_set_bool(cd, "drawing", pm->drawing_hole);
	calldata_set_bool(cd, "snap", pm->snap);
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_fit_content(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface *s = selected_surface(pm);
	if (s && pm->input_w && pm->input_h) {
		push_undo(pm);
		fit_surface(pm, s, true);
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void proc_set_snap(void *data, calldata_t *cd)
{
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	pm->snap = calldata_bool(cd, "enabled");
	obs_data_t *settings = obs_source_get_settings(pm->self);
	obs_data_set_bool(settings, "snap", pm->snap);
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
BUTTON_CB(flip_h_clicked, action_flip(data, 0, false))
BUTTON_CB(flip_v_clicked, action_flip(data, 1, false))
BUTTON_CB(rotate_clicked, action_flip(data, 2, false))
BUTTON_CB(fit_clicked, proc_fit_content(data, NULL))

static obs_properties_t *pm_get_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, "input", "Innhold (scene eller kilde)",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, "— Velg —", "");
	obs_property_list_add_string(list, "★ Aktiv scene (programbildet)", PM_PROGRAM);
	obs_enum_scenes(add_source_to_list, list);
	obs_enum_sources(add_source_to_list, list);

	obs_property_t *mode =
		obs_properties_add_list(props, "mode", "Modus", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "Move (dra hjørner og kanter)", MODE_MOVE);
	obs_property_list_add_int(mode, "Mask (tegn masker)", MODE_MASK);
	obs_property_list_add_int(mode, "Utsnitt (velg del av innholdet)", MODE_CROP);
	obs_property_list_add_int(mode, "Ferdig (skjul hjelpelinjer)", MODE_SHOW);

	obs_property_t *cm =
		obs_properties_add_list(props, "color_mode", "Farger", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cm, "Nøyaktig som originalen (anbefalt)", 0);
	obs_property_list_add_int(cm, "Lineær (OBS-standard)", 1);
	obs_property_list_add_int(cm, "Lysere", 2);
	obs_properties_add_bool(props, "guides_on_output", "Vis hjelpelinjer på projektor");
	obs_properties_add_bool(props, "snap", "Snap (fest til andre flater og bildekanten)");

	obs_properties_add_button(props, "add_surface", "+ Legg til firkantet flate", add_surface_clicked);
	obs_properties_add_button(props, "add_round", "+ Legg til rund flate", add_round_clicked);
	obs_properties_add_button(props, "toggle_shape", "Gjør valgt flate rund / firkantet", toggle_shape_clicked);
	obs_properties_add_button(props, "remove_surface", "Fjern valgt flate", remove_surface_clicked);
	obs_properties_add_button(props, "reset_surface", "Nullstill valgt flate", reset_surface_clicked);
	obs_properties_add_button(props, "clear_mask", "Fjern maske på valgt flate", clear_mask_clicked);
	obs_properties_add_button(props, "fit", "Original størrelse (innholdets pikselstørrelse)", fit_clicked);
	obs_properties_add_button(props, "flip_h", "Flip innhold horisontalt", flip_h_clicked);
	obs_properties_add_button(props, "flip_v", "Flip innhold vertikalt", flip_v_clicked);
	obs_properties_add_button(props, "rotate", "Roter innhold 180°", rotate_clicked);

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
	.activate = pm_activate,
	.deactivate = pm_deactivate,
	.show = pm_show,
	.hide = pm_hide,
	.mouse_click = pm_mouse_click,
	.mouse_move = pm_mouse_move,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
};
