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
 * et ubegrenset antall firkantede flater. Hver flate har:
 *   - dst[4]: hvor flaten ligger i projektorbildet (geometri)
 *   - src[4]: hvilken del av innholdet flaten viser (utsnitt)
 * Alle punkter lagres normalisert (0..1), rekkefølge: øverst venstre,
 * øverst høyre, nederst høyre, nederst venstre.
 *
 * Redigering skjer ved å høyreklikke kilden -> "Samhandle" og dra hjørner.
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/darray.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <math.h>
#include <string.h>

#include <plugin-support.h>

#define PM_ID "projection_mapping_source"

enum pm_mode {
	MODE_SHOW = 0,     /* Ferdig: kun bildet, ingen hjelpelinjer */
	MODE_GEOMETRY = 1, /* Dra hjørnene på flatene */
	MODE_CROP = 2,     /* Velg utsnitt av innholdet for hver flate */
};

struct pm_surface {
	struct vec2 dst[4];
	struct vec2 src[4];
};

struct pm_source {
	obs_source_t *self;
	pthread_mutex_t mutex;

	DARRAY(struct pm_surface) surfaces;
	int selected;
	int mode;
	uint32_t width;
	uint32_t height;

	char *input_uuid;
	bool need_resolve;
	float retry_timer;
	obs_weak_source_t *input;

	gs_texrender_t *texrender;
	gs_effect_t *effect;
	bool rendered;

	bool dragging;
	int drag_surface;
	int drag_corner; /* 0-3 = hjørne, -1 = hele flaten */
	struct vec2 drag_last;
};

/* ------------------------------------------------------------------ */
/* Matematikk: homografi (perspektiv) mellom to firkanter              */

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

static void mat3_mul(const pm_mat3 a, const pm_mat3 b, pm_mat3 o)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			o[r * 3 + c] =
				a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
}

static bool point_in_quad(const struct vec2 q[4], float x, float y)
{
	bool inside = false;
	for (int i = 0, j = 3; i < 4; j = i++) {
		if (((q[i].y > y) != (q[j].y > y)) &&
		    (x < (q[j].x - q[i].x) * (y - q[i].y) / (q[j].y - q[i].y) + q[i].x))
			inside = !inside;
	}
	return inside;
}

/* ------------------------------------------------------------------ */
/* Lagring i OBS-prosjektet (kildens innstillinger)                    */

static void surface_set_default(struct pm_surface *s, size_t index)
{
	float off = 0.05f * (float)(index % 6);
	vec2_set(&s->dst[0], 0.25f + off, 0.25f + off);
	vec2_set(&s->dst[1], 0.65f + off, 0.25f + off);
	vec2_set(&s->dst[2], 0.65f + off, 0.65f + off);
	vec2_set(&s->dst[3], 0.25f + off, 0.65f + off);
	vec2_set(&s->src[0], 0.0f, 0.0f);
	vec2_set(&s->src[1], 1.0f, 0.0f);
	vec2_set(&s->src[2], 1.0f, 1.0f);
	vec2_set(&s->src[3], 0.0f, 1.0f);
}

static void load_surfaces(struct pm_source *pm, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, "surfaces");
	size_t count = arr ? obs_data_array_count(arr) : 0;
	char key[8];

	da_resize(pm->surfaces, 0);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		struct pm_surface s;
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
		da_push_back(pm->surfaces, &s);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void save_surfaces(struct pm_source *pm, obs_data_t *settings)
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
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "surfaces", arr);
	obs_data_set_int(settings, "selected", pm->selected);
	obs_data_array_release(arr);
}

/* Lagrer gjeldende flater. Kalles med mutex låst. */
static void save_to_source(struct pm_source *pm)
{
	obs_data_t *settings = obs_source_get_settings(pm->self);
	save_surfaces(pm, settings);
	obs_data_release(settings);
}

/* ------------------------------------------------------------------ */
/* Innholdskilde                                                       */

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

	if (!pm->dragging)
		load_surfaces(pm, settings);
	pm->selected = (int)obs_data_get_int(settings, "selected");
	if (pm->selected >= (int)pm->surfaces.num)
		pm->selected = (int)pm->surfaces.num - 1;
	if (pm->selected < 0)
		pm->selected = 0;

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
	da_init(pm->surfaces);

	obs_enter_graphics();
	pm->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	char *path = obs_module_file("effects/mapping.effect");
	pm->effect = gs_effect_create_from_file(path, NULL);
	bfree(path);
	obs_leave_graphics();

	if (!pm->effect)
		obs_log(LOG_ERROR, "Fant ikke / kunne ikke laste effects/mapping.effect");

	/* Ny kilde: start med én flate så man ser noe med en gang. */
	if (!obs_data_has_user_value(settings, "surfaces")) {
		struct pm_surface s;
		surface_set_default(&s, 0);
		da_push_back(pm->surfaces, &s);
		save_surfaces(pm, settings);
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
	gs_effect_destroy(pm->effect);
	obs_leave_graphics();

	da_free(pm->surfaces);
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
	obs_data_set_default_int(settings, "mode", MODE_GEOMETRY);
	obs_data_set_default_int(settings, "selected", 0);
	obs_data_set_default_string(settings, "input", "");
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
/* Tegning                                                              */

static float handle_size(const struct pm_source *pm)
{
	float m = (float)(pm->width > pm->height ? pm->width : pm->height);
	float s = m * 0.008f;
	return s < 6.0f ? 6.0f : s;
}

static void render_input_texture(struct pm_source *pm, obs_source_t *input)
{
	if (pm->rendered)
		return;
	pm->rendered = true;

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

static void draw_quad_strip(const struct vec2 p[4])
{
	gs_render_start(true);
	gs_vertex2f(p[0].x, p[0].y);
	gs_vertex2f(p[1].x, p[1].y);
	gs_vertex2f(p[3].x, p[3].y);
	gs_vertex2f(p[2].x, p[2].y);
	gs_render_stop(GS_TRISTRIP);
}

static void draw_surface(struct pm_source *pm, gs_texture_t *tex, const struct pm_surface *s)
{
	struct vec2 px[4];
	quad_to_pixels(pm, s->dst, px);

	pm_mat3 to_dst, to_src, inv_dst, hmat;
	square_to_quad(px, to_dst);
	square_to_quad(s->src, to_src);
	if (!mat3_inverse(to_dst, inv_dst))
		return;
	mat3_mul(to_src, inv_dst, hmat);

	struct vec4 r0, r1, r2;
	vec4_set(&r0, (float)hmat[0], (float)hmat[1], (float)hmat[2], 0.0f);
	vec4_set(&r1, (float)hmat[3], (float)hmat[4], (float)hmat[5], 0.0f);
	vec4_set(&r2, (float)hmat[6], (float)hmat[7], (float)hmat[8], 0.0f);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(pm->effect, "image"), tex);
	gs_effect_set_vec4(gs_effect_get_param_by_name(pm->effect, "hrow0"), &r0);
	gs_effect_set_vec4(gs_effect_get_param_by_name(pm->effect, "hrow1"), &r1);
	gs_effect_set_vec4(gs_effect_get_param_by_name(pm->effect, "hrow2"), &r2);

	while (gs_effect_loop(pm->effect, "Draw"))
		draw_quad_strip(px);
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

static void draw_overlay_quad(struct pm_source *pm, gs_eparam_t *color, const struct vec2 q[4], bool selected,
			      gs_technique_t *tech)
{
	struct vec2 px[4];
	quad_to_pixels(pm, q, px);
	float hs = handle_size(pm);
	float t = hs * 0.35f;
	if (t < 2.0f)
		t = 2.0f;

	struct vec4 line_col;
	vec4_from_rgba(&line_col, selected ? 0xFF00D0FF : 0xFFFFD000); /* ABGR: gul / cyan */
	gs_effect_set_vec4(color, &line_col);
	gs_technique_begin_pass(tech, 0);
	for (int i = 0; i < 4; i++)
		draw_line(px[i], px[(i + 1) % 4], t);
	gs_technique_end_pass(tech);

	for (int i = 0; i < 4; i++) {
		struct vec4 c;
		/* Øverste venstre hjørne markeres rødt, så man ser orienteringen. */
		vec4_from_rgba(&c, i == 0 ? 0xFF3030FF : 0xFFFFFFFF);
		gs_effect_set_vec4(color, &c);
		gs_technique_begin_pass(tech, 0);
		draw_box(px[i], selected ? hs : hs * 0.7f);
		gs_technique_end_pass(tech);
	}
}

static void draw_overlays(struct pm_source *pm)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_technique_begin(tech);
	for (size_t i = 0; i < pm->surfaces.num; i++) {
		if ((int)i == pm->selected)
			continue;
		const struct pm_surface *s = &pm->surfaces.array[i];
		draw_overlay_quad(pm, color, pm->mode == MODE_CROP ? s->src : s->dst, false, tech);
	}
	if (pm->selected >= 0 && pm->selected < (int)pm->surfaces.num) {
		const struct pm_surface *s = &pm->surfaces.array[pm->selected];
		draw_overlay_quad(pm, color, pm->mode == MODE_CROP ? s->src : s->dst, true, tech);
	}
	gs_technique_end(tech);
}

static void draw_placeholder(struct pm_source *pm, const struct pm_surface *s)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	vec4_from_rgba(&c, 0xFF404040);
	gs_effect_set_vec4(color, &c);
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
	gs_texture_t *tex = NULL;
	if (input) {
		render_input_texture(pm, input);
		tex = gs_texrender_get_texture(pm->texrender);
	}

	pthread_mutex_lock(&pm->mutex);

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	if (pm->mode == MODE_CROP) {
		/* Vis hele innholdet strukket over utgangen, med utsnittene oppå. */
		if (tex) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture_srgb(gs_effect_get_param_by_name(def, "image"), tex);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(tex, 0, pm->width, pm->height);
		}
	} else {
		for (size_t i = 0; i < pm->surfaces.num; i++) {
			const struct pm_surface *s = &pm->surfaces.array[i];
			if (tex)
				draw_surface(pm, tex, s);
			else if (pm->mode == MODE_GEOMETRY)
				draw_placeholder(pm, s);
		}
	}

	gs_enable_framebuffer_srgb(false);
	if (pm->mode != MODE_SHOW)
		draw_overlays(pm);
	gs_enable_framebuffer_srgb(prev_srgb);

	pthread_mutex_unlock(&pm->mutex);
	obs_source_release(input);
}

/* ------------------------------------------------------------------ */
/* Mus: dra hjørner / hele flater i "Samhandle"-vinduet                 */

static struct vec2 *edit_points(struct pm_source *pm, size_t idx)
{
	struct pm_surface *s = &pm->surfaces.array[idx];
	return pm->mode == MODE_CROP ? s->src : s->dst;
}

static bool hit_test(struct pm_source *pm, float px, float py, int *surface, int *corner)
{
	float r = handle_size(pm) * 2.0f;
	float w = (float)pm->width, h = (float)pm->height;
	int n = (int)pm->surfaces.num;

	/* Rekkefølge: valgt flate først, deretter øverste (sist tegnet) først. */
	for (int pass = 0; pass < 2; pass++) {
		for (int k = n - 1; k >= 0; k--) {
			int i = pass == 0 ? pm->selected : k;
			if (i < 0 || i >= n)
				continue;
			struct vec2 *p = edit_points(pm, i);
			for (int c = 0; c < 4; c++) {
				float dx = p[c].x * w - px, dy = p[c].y * h - py;
				if (dx * dx + dy * dy <= r * r) {
					*surface = i;
					*corner = c;
					return true;
				}
			}
			if (pass == 0)
				break;
		}
	}

	for (int pass = 0; pass < 2; pass++) {
		for (int k = n - 1; k >= 0; k--) {
			int i = pass == 0 ? pm->selected : k;
			if (i < 0 || i >= n)
				continue;
			struct vec2 *p = edit_points(pm, i);
			struct vec2 q[4];
			quad_to_pixels(pm, p, q);
			if (point_in_quad(q, px, py)) {
				*surface = i;
				*corner = -1;
				return true;
			}
			if (pass == 0)
				break;
		}
	}
	return false;
}

static void pm_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			   uint32_t click_count)
{
	UNUSED_PARAMETER(click_count);
	struct pm_source *pm = data;
	if (type != MOUSE_LEFT)
		return;

	pthread_mutex_lock(&pm->mutex);
	if (pm->mode == MODE_SHOW) {
		pthread_mutex_unlock(&pm->mutex);
		return;
	}

	if (!mouse_up) {
		int s, c;
		if (hit_test(pm, (float)event->x, (float)event->y, &s, &c)) {
			pm->selected = s;
			pm->dragging = true;
			pm->drag_surface = s;
			pm->drag_corner = c;
			vec2_set(&pm->drag_last, (float)event->x / (float)pm->width,
				 (float)event->y / (float)pm->height);
		}
	} else if (pm->dragging) {
		pm->dragging = false;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
}

static void pm_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	UNUSED_PARAMETER(mouse_leave);
	struct pm_source *pm = data;

	pthread_mutex_lock(&pm->mutex);
	if (pm->dragging && pm->drag_surface < (int)pm->surfaces.num) {
		struct vec2 cur;
		vec2_set(&cur, (float)event->x / (float)pm->width, (float)event->y / (float)pm->height);
		struct vec2 delta;
		vec2_sub(&delta, &cur, &pm->drag_last);
		pm->drag_last = cur;

		struct vec2 *p = edit_points(pm, pm->drag_surface);
		if (pm->drag_corner >= 0) {
			vec2_add(&p[pm->drag_corner], &p[pm->drag_corner], &delta);
		} else {
			for (int c = 0; c < 4; c++)
				vec2_add(&p[c], &p[c], &delta);
		}
	}
	pthread_mutex_unlock(&pm->mutex);
}

/* ------------------------------------------------------------------ */
/* Egenskaper (innstillingsvinduet)                                     */

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

static bool add_surface_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	struct pm_surface s;
	surface_set_default(&s, pm->surfaces.num);
	da_push_back(pm->surfaces, &s);
	pm->selected = (int)pm->surfaces.num - 1;
	save_to_source(pm);
	pthread_mutex_unlock(&pm->mutex);
	return false;
}

static bool remove_surface_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	if (pm->selected >= 0 && pm->selected < (int)pm->surfaces.num) {
		da_erase(pm->surfaces, pm->selected);
		if (pm->selected >= (int)pm->surfaces.num)
			pm->selected = (int)pm->surfaces.num - 1;
		if (pm->selected < 0)
			pm->selected = 0;
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
	return false;
}

static bool reset_surface_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct pm_source *pm = data;
	pthread_mutex_lock(&pm->mutex);
	if (pm->selected >= 0 && pm->selected < (int)pm->surfaces.num) {
		struct pm_surface *s = &pm->surfaces.array[pm->selected];
		struct pm_surface def;
		surface_set_default(&def, 0);
		if (pm->mode == MODE_CROP)
			memcpy(s->src, def.src, sizeof(def.src));
		else
			memcpy(s->dst, def.dst, sizeof(def.dst));
		save_to_source(pm);
	}
	pthread_mutex_unlock(&pm->mutex);
	return false;
}

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
	obs_property_list_add_int(mode, "Rediger flater (dra hjørnene)", MODE_GEOMETRY);
	obs_property_list_add_int(mode, "Velg utsnitt av innholdet", MODE_CROP);
	obs_property_list_add_int(mode, "Ferdig (skjul hjelpelinjer)", MODE_SHOW);

	obs_properties_add_button(props, "add_surface", "+ Legg til flate", add_surface_clicked);
	obs_properties_add_button(props, "remove_surface", "Fjern valgt flate", remove_surface_clicked);
	obs_properties_add_button(props, "reset_surface", "Nullstill valgt flate", reset_surface_clicked);

	obs_properties_add_int(props, "width", "Projektorbredde (px)", 16, 16384, 1);
	obs_properties_add_int(props, "height", "Projektorhøyde (px)", 16, 16384, 1);

	obs_properties_add_text(
		props, "help",
		"Slik redigerer du: trykk OK, høyreklikk kilden og velg «Samhandle». "
		"Dra hjørnene, eller dra midt i en flate for å flytte hele. Gul = valgt flate, rødt hjørne = øverst venstre.",
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
