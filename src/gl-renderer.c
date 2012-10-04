/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2012 John Kåre Alsaker
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include "gl-internal.h"

static const char *
egl_error_string(EGLint code)
{
#define MYERRCODE(x) case x: return #x;
	switch (code) {
	MYERRCODE(EGL_SUCCESS)
	MYERRCODE(EGL_NOT_INITIALIZED)
	MYERRCODE(EGL_BAD_ACCESS)
	MYERRCODE(EGL_BAD_ALLOC)
	MYERRCODE(EGL_BAD_ATTRIBUTE)
	MYERRCODE(EGL_BAD_CONTEXT)
	MYERRCODE(EGL_BAD_CONFIG)
	MYERRCODE(EGL_BAD_CURRENT_SURFACE)
	MYERRCODE(EGL_BAD_DISPLAY)
	MYERRCODE(EGL_BAD_SURFACE)
	MYERRCODE(EGL_BAD_MATCH)
	MYERRCODE(EGL_BAD_PARAMETER)
	MYERRCODE(EGL_BAD_NATIVE_PIXMAP)
	MYERRCODE(EGL_BAD_NATIVE_WINDOW)
	MYERRCODE(EGL_CONTEXT_LOST)
	default:
		return "unknown";
	}
#undef MYERRCODE
}

static void
print_egl_error_state(void)
{
	EGLint code;

	code = eglGetError();
	weston_log("EGL error state: %s (0x%04lx)\n",
		egl_error_string(code), (long)code);
}

struct polygon8 {
	GLfloat x[8];
	GLfloat y[8];
	int n;
};

struct clip_context {
	struct {
		GLfloat x;
		GLfloat y;
	} prev;

	struct {
		GLfloat x1, y1;
		GLfloat x2, y2;
	} clip;

	struct {
		GLfloat *x;
		GLfloat *y;
	} vertices;
};

static GLfloat
float_difference(GLfloat a, GLfloat b)
{
	/* http://www.altdevblogaday.com/2012/02/22/comparing-floating-point-numbers-2012-edition/ */
	static const GLfloat max_diff = 4.0f * FLT_MIN;
	static const GLfloat max_rel_diff = 4.0e-5;
	GLfloat diff = a - b;
	GLfloat adiff = fabsf(diff);

	if (adiff <= max_diff)
		return 0.0f;

	a = fabsf(a);
	b = fabsf(b);
	if (adiff <= (a > b ? a : b) * max_rel_diff)
		return 0.0f;

	return diff;
}

/* A line segment (p1x, p1y)-(p2x, p2y) intersects the line x = x_arg.
 * Compute the y coordinate of the intersection.
 */
static GLfloat
clip_intersect_y(GLfloat p1x, GLfloat p1y, GLfloat p2x, GLfloat p2y,
		 GLfloat x_arg)
{
	GLfloat a;
	GLfloat diff = float_difference(p1x, p2x);

	/* Practically vertical line segment, yet the end points have already
	 * been determined to be on different sides of the line. Therefore
	 * the line segment is part of the line and intersects everywhere.
	 * Return the end point, so we use the whole line segment.
	 */
	if (diff == 0.0f)
		return p2y;

	a = (x_arg - p2x) / diff;
	return p2y + (p1y - p2y) * a;
}

/* A line segment (p1x, p1y)-(p2x, p2y) intersects the line y = y_arg.
 * Compute the x coordinate of the intersection.
 */
static GLfloat
clip_intersect_x(GLfloat p1x, GLfloat p1y, GLfloat p2x, GLfloat p2y,
		 GLfloat y_arg)
{
	GLfloat a;
	GLfloat diff = float_difference(p1y, p2y);

	/* Practically horizontal line segment, yet the end points have already
	 * been determined to be on different sides of the line. Therefore
	 * the line segment is part of the line and intersects everywhere.
	 * Return the end point, so we use the whole line segment.
	 */
	if (diff == 0.0f)
		return p2x;

	a = (y_arg - p2y) / diff;
	return p2x + (p1x - p2x) * a;
}

enum path_transition {
	PATH_TRANSITION_OUT_TO_OUT = 0,
	PATH_TRANSITION_OUT_TO_IN = 1,
	PATH_TRANSITION_IN_TO_OUT = 2,
	PATH_TRANSITION_IN_TO_IN = 3,
};

static void
clip_append_vertex(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	*ctx->vertices.x++ = x;
	*ctx->vertices.y++ = y;
}

static enum path_transition
path_transition_left_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.x >= ctx->clip.x1) << 1) | (x >= ctx->clip.x1);
}

static enum path_transition
path_transition_right_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.x < ctx->clip.x2) << 1) | (x < ctx->clip.x2);
}

static enum path_transition
path_transition_top_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.y >= ctx->clip.y1) << 1) | (y >= ctx->clip.y1);
}

static enum path_transition
path_transition_bottom_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.y < ctx->clip.y2) << 1) | (y < ctx->clip.y2);
}

static void
clip_polygon_leftright(struct clip_context *ctx,
		       enum path_transition transition,
		       GLfloat x, GLfloat y, GLfloat clip_x)
{
	GLfloat yi;

	switch (transition) {
	case PATH_TRANSITION_IN_TO_IN:
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_IN_TO_OUT:
		yi = clip_intersect_y(ctx->prev.x, ctx->prev.y, x, y, clip_x);
		clip_append_vertex(ctx, clip_x, yi);
		break;
	case PATH_TRANSITION_OUT_TO_IN:
		yi = clip_intersect_y(ctx->prev.x, ctx->prev.y, x, y, clip_x);
		clip_append_vertex(ctx, clip_x, yi);
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_OUT_TO_OUT:
		/* nothing */
		break;
	default:
		assert(0 && "bad enum path_transition");
	}

	ctx->prev.x = x;
	ctx->prev.y = y;
}

static void
clip_polygon_topbottom(struct clip_context *ctx,
		       enum path_transition transition,
		       GLfloat x, GLfloat y, GLfloat clip_y)
{
	GLfloat xi;

	switch (transition) {
	case PATH_TRANSITION_IN_TO_IN:
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_IN_TO_OUT:
		xi = clip_intersect_x(ctx->prev.x, ctx->prev.y, x, y, clip_y);
		clip_append_vertex(ctx, xi, clip_y);
		break;
	case PATH_TRANSITION_OUT_TO_IN:
		xi = clip_intersect_x(ctx->prev.x, ctx->prev.y, x, y, clip_y);
		clip_append_vertex(ctx, xi, clip_y);
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_OUT_TO_OUT:
		/* nothing */
		break;
	default:
		assert(0 && "bad enum path_transition");
	}

	ctx->prev.x = x;
	ctx->prev.y = y;
}

static void
clip_context_prepare(struct clip_context *ctx, const struct polygon8 *src,
		      GLfloat *dst_x, GLfloat *dst_y)
{
	ctx->prev.x = src->x[src->n - 1];
	ctx->prev.y = src->y[src->n - 1];
	ctx->vertices.x = dst_x;
	ctx->vertices.y = dst_y;
}

static int
clip_polygon_left(struct clip_context *ctx, const struct polygon8 *src,
		  GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_left_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_leftright(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.x1);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_right(struct clip_context *ctx, const struct polygon8 *src,
		   GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_right_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_leftright(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.x2);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_top(struct clip_context *ctx, const struct polygon8 *src,
		 GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_top_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_topbottom(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.y1);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_bottom(struct clip_context *ctx, const struct polygon8 *src,
		    GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_bottom_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_topbottom(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.y2);
	}
	return ctx->vertices.x - dst_x;
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))
#define clip(x, a, b)  min(max(x, a), b)

/*
 * Compute the boundary vertices of the intersection of the global coordinate
 * aligned rectangle 'rect', and an arbitrary quadrilateral produced from
 * 'surf_rect' when transformed from surface coordinates into global coordinates.
 * The vertices are written to 'ex' and 'ey', and the return value is the
 * number of vertices. Vertices are produced in clockwise winding order.
 * Guarantees to produce either zero vertices, or 3-8 vertices with non-zero
 * polygon area.
 */
static int
calculate_edges(struct weston_surface *es, pixman_box32_t *rect,
		pixman_box32_t *surf_rect, GLfloat *ex, GLfloat *ey)
{
	struct polygon8 polygon;
	struct clip_context ctx;
	int i, n;
	GLfloat min_x, max_x, min_y, max_y;
	struct polygon8 surf = {
		{ surf_rect->x1, surf_rect->x2, surf_rect->x2, surf_rect->x1 },
		{ surf_rect->y1, surf_rect->y1, surf_rect->y2, surf_rect->y2 },
		4
	};

	ctx.clip.x1 = rect->x1;
	ctx.clip.y1 = rect->y1;
	ctx.clip.x2 = rect->x2;
	ctx.clip.y2 = rect->y2;

	/* transform surface to screen space: */
	for (i = 0; i < surf.n; i++)
		weston_surface_to_global_float(es, surf.x[i], surf.y[i],
					       &surf.x[i], &surf.y[i]);

	/* find bounding box: */
	min_x = max_x = surf.x[0];
	min_y = max_y = surf.y[0];

	for (i = 1; i < surf.n; i++) {
		min_x = min(min_x, surf.x[i]);
		max_x = max(max_x, surf.x[i]);
		min_y = min(min_y, surf.y[i]);
		max_y = max(max_y, surf.y[i]);
	}

	/* First, simple bounding box check to discard early transformed
	 * surface rects that do not intersect with the clip region:
	 */
	if ((min_x >= ctx.clip.x2) || (max_x <= ctx.clip.x1) ||
	    (min_y >= ctx.clip.y2) || (max_y <= ctx.clip.y1))
		return 0;

	/* Simple case, bounding box edges are parallel to surface edges,
	 * there will be only four edges.  We just need to clip the surface
	 * vertices to the clip rect bounds:
	 */
	if (!es->transform.enabled) {
		for (i = 0; i < surf.n; i++) {
			ex[i] = clip(surf.x[i], ctx.clip.x1, ctx.clip.x2);
			ey[i] = clip(surf.y[i], ctx.clip.y1, ctx.clip.y2);
		}
		return surf.n;
	}

	/* Transformed case: use a general polygon clipping algorithm to
	 * clip the surface rectangle with each side of 'rect'.
	 * The algorithm is Sutherland-Hodgman, as explained in
	 * http://www.codeguru.com/cpp/misc/misc/graphics/article.php/c8965/Polygon-Clipping.htm
	 * but without looking at any of that code.
	 */
	polygon.n = clip_polygon_left(&ctx, &surf, polygon.x, polygon.y);
	surf.n = clip_polygon_right(&ctx, &polygon, surf.x, surf.y);
	polygon.n = clip_polygon_top(&ctx, &surf, polygon.x, polygon.y);
	surf.n = clip_polygon_bottom(&ctx, &polygon, surf.x, surf.y);

	/* Get rid of duplicate vertices */
	ex[0] = surf.x[0];
	ey[0] = surf.y[0];
	n = 1;
	for (i = 1; i < surf.n; i++) {
		if (float_difference(ex[n - 1], surf.x[i]) == 0.0f &&
		    float_difference(ey[n - 1], surf.y[i]) == 0.0f)
			continue;
		ex[n] = surf.x[i];
		ey[n] = surf.y[i];
		n++;
	}
	if (float_difference(ex[n - 1], surf.x[0]) == 0.0f &&
	    float_difference(ey[n - 1], surf.y[0]) == 0.0f)
		n--;

	if (n < 3)
		return 0;

	return n;
}

static int
texture_region(struct weston_surface *es, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	struct weston_compositor *ec = es->compositor;
	GLfloat *v, inv_width, inv_height;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects, *surf_rects;
	int i, j, k, nrects, nsurf;

	rects = pixman_region32_rectangles(region, &nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	/* worst case we can have 8 vertices per rect (ie. clipped into
	 * an octagon):
	 */
	v = wl_array_add(&ec->vertices, nrects * nsurf * 8 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&ec->vtxcnt, nrects * nsurf * sizeof *vtxcnt);

	inv_width = 1.0 / es->pitch;
	inv_height = 1.0 / es->geometry.height;

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		for (j = 0; j < nsurf; j++) {
			pixman_box32_t *surf_rect = &surf_rects[j];
			GLfloat sx, sy;
			GLfloat ex[8], ey[8];          /* edge points in screen space */
			int n;

			/* The transformed surface, after clipping to the clip region,
			 * can have as many as eight sides, emitted as a triangle-fan.
			 * The first vertex in the triangle fan can be chosen arbitrarily,
			 * since the area is guaranteed to be convex.
			 *
			 * If a corner of the transformed surface falls outside of the
			 * clip region, instead of emitting one vertex for the corner
			 * of the surface, up to two are emitted for two corresponding
			 * intersection point(s) between the surface and the clip region.
			 *
			 * To do this, we first calculate the (up to eight) points that
			 * form the intersection of the clip rect and the transformed
			 * surface.
			 */
			n = calculate_edges(es, rect, surf_rect, ex, ey);
			if (n < 3)
				continue;

			/* emit edge points: */
			for (k = 0; k < n; k++) {
				weston_surface_from_global_float(es, ex[k], ey[k], &sx, &sy);
				/* position: */
				*(v++) = ex[k];
				*(v++) = ey[k];
				/* texcoord: */
				*(v++) = sx * inv_width;
				*(v++) = sy * inv_height;
			}

			vtxcnt[nvtx++] = n;
		}
	}

	return nvtx;
}

static void
triangle_fan_debug(struct weston_surface *surface, int first, int count)
{
	struct weston_compositor *compositor = surface->compositor;
	struct gl_renderer *gr = get_renderer(compositor);
	int i;
	GLushort *buffer;
	GLushort *index;
	int nelems;
	static int color_idx = 0;
	static const GLfloat color[][4] = {
			{ 1.0, 0.0, 0.0, 1.0 },
			{ 0.0, 1.0, 0.0, 1.0 },
			{ 0.0, 0.0, 1.0, 1.0 },
			{ 1.0, 1.0, 1.0, 1.0 },
	};

	nelems = (count - 1 + count - 2) * 2;

	buffer = malloc(sizeof(GLushort) * nelems);
	index = buffer;

	for (i = 1; i < count; i++) {
		*index++ = first;
		*index++ = first + i;
	}

	for (i = 2; i < count; i++) {
		*index++ = first + i - 1;
		*index++ = first + i;
	}

	glUseProgram(gr->solid_shader->program);
	glUniform4fv(gr->solid_shader->color_uniform, 1,
			color[color_idx++ % ARRAY_LENGTH(color)]);
	glDrawElements(GL_LINES, nelems, GL_UNSIGNED_SHORT, buffer);
	glUseProgram(gr->current_shader->program);
	free(buffer);
}

static void
repaint_region(struct weston_compositor *ec, struct weston_surface *es,
		int nfans)
{
	GLfloat *v;
	unsigned int *vtxcnt;
	int i, first;

	v = ec->vertices.data;
	vtxcnt = ec->vtxcnt.data;

	/* position: */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glEnableVertexAttribArray(0);

	/* texcoord: */
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(1);

	for (i = 0, first = 0; i < nfans; i++) {
		glDrawArrays(GL_TRIANGLE_FAN, first, vtxcnt[i]);
		if (es && ec->fan_debug)
			triangle_fan_debug(es, first, vtxcnt[i]);
		first += vtxcnt[i];
	}

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	ec->vertices.size = 0;
	ec->vtxcnt.size = 0;
}

static int
use_output(struct weston_output *output)
{
	static int errored;
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	EGLBoolean ret;

	ret = eglMakeCurrent(gr->egl_display, go->egl_surface,
			     go->egl_surface, gr->egl_context);

	if (ret == EGL_FALSE) {
		if (errored)
			return -1;
		errored = 1;
		weston_log("Failed to make EGL context current.\n");
		print_egl_error_state();
		return -1;
	}

	return 0;
}

static void
repaint_surface(struct weston_surface *es, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	/* The final region to be painted is the intersection of
	 * 'region' and 'surf_region'. However, 'region' is in the global
	 * coordinates, and 'surf_region' is in the surface-local
	 * coordinates. texture_region() will iterate over all pairs of
	 * rectangles from both regions, compute the intersection
	 * polygon for each pair, and store it as a triangle fan if
	 * it has a non-zero area (at least 3 vertices, actually).
	 */
	int nfans = texture_region(es, region, surf_region);

	repaint_region(es->compositor, es, nfans);
}

static void
output_emit_vertex(struct weston_output *output, GLfloat **v, int32_t x, int32_t y)
{
	struct weston_vector vector;

	/* position: */
	*((*v)++) = x;
	*((*v)++) = y;

	/* texcoord: */

	vector.f[0] = x;
	vector.f[1] = y;
	vector.f[2] = 0.0f;
	vector.f[3] = 1.0f;

	weston_matrix_transform(&output->matrix, &vector);

	*((*v)++) = (vector.f[0] + 1.0f) * 0.5f;
	*((*v)++) = (vector.f[1] + 1.0f) * 0.5f;
}

static void
repaint_output(struct weston_output *output, pixman_region32_t *region)
{
	struct weston_compositor *ec = output->compositor;
	GLfloat *v;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects;
	int i, nrects;

	rects = pixman_region32_rectangles(region, &nrects);

	v = wl_array_add(&ec->vertices, nrects * 4 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&ec->vtxcnt, nrects * sizeof *vtxcnt);

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];

		output_emit_vertex(output, &v, rect->x1, rect->y1);
		output_emit_vertex(output, &v, rect->x2, rect->y1);
		output_emit_vertex(output, &v, rect->x2, rect->y2);
		output_emit_vertex(output, &v, rect->x1, rect->y2);

		vtxcnt[nvtx++] = 4;
	}

	repaint_region(ec, NULL, nvtx);
}

static void
create_indirect_texture(struct weston_output *output)
{
	struct gl_output_state *go = get_output_state(output);
	GLenum status;

	glGenTextures(1, &go->indirect_texture);

	glBindTexture(GL_TEXTURE_2D, go->indirect_texture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
		output->current->width,
		output->current->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_TEXTURE_2D, go->indirect_texture, 0);

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		weston_log("unable to create framebuffer for indirect rendering %d\n", (int)status);
		go->indirect_drawing = 0;
		go->indirect_disable = 1;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

static void
repaint_surfaces_start(struct weston_output *output, pixman_region32_t *damage)
{
	struct gl_output_state *go = get_output_state(output);

	go->indirect_drawing = output->compositor->color_managed;

	if (go->indirect_disable)
		go->indirect_drawing = 0;

	if (go->indirect_drawing) {
		glBindFramebuffer(GL_FRAMEBUFFER, go->indirect_fbo);

		if (!go->indirect_texture)
			create_indirect_texture(output);
	}
}

static void
repaint_surfaces_finish(struct weston_output *output, pixman_region32_t *damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_shader *shader;

	if (go->indirect_drawing) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		shader = gl_select_shader(gr,
			INPUT_RGBX,
			OUTPUT_TO_SRGB,
			CONVERSION_NONE);

		gl_use_shader(gr, shader);
		gl_shader_set_output(shader, output);

		glActiveTexture(GL_TEXTURE0 + MAX_PLANES);
		glBindTexture(GL_TEXTURE_2D, gr->srgb_encode_lut);

		glDisable(GL_BLEND);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, go->indirect_texture);

		repaint_output(output, damage);
	}
}

static void
draw_surface(struct weston_surface *es, struct weston_output *output,
	     pixman_region32_t *damage) /* in global coordinates */
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	struct gl_shader *shader;
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	pixman_region32_t *buffer_damage;
	GLint filter;
	int i, transparent;
	enum gl_output_attribute output_attribute;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &es->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &es->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	buffer_damage = &output->buffer_damage[output->current_buffer];
	pixman_region32_subtract(buffer_damage, buffer_damage, &repaint);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	if (ec->fan_debug) {
		gl_use_shader(gr, gr->solid_shader);
		gl_shader_setup(gr->solid_shader, es, output);
	}

	transparent = es->alpha < 1.0;
	output_attribute = transparent ? OUTPUT_TRANSPARENT : OUTPUT_BLEND;

	shader = gl_select_shader(gr, gs->input, output_attribute, gs->conversion);

	gl_use_shader(gr, shader);
	gl_shader_setup(shader, es, output);

	if (es->transform.enabled || output->zoom.active)
		filter = GL_LINEAR;
	else
		filter = GL_NEAREST;

	for (i = 0; i < gs->num_textures; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(gs->target, gs->textures[i]);
		glTexParameteri(gs->target, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(gs->target, GL_TEXTURE_MAG_FILTER, filter);
	}

	/* blended region is whole surface minus opaque region: */
	pixman_region32_init_rect(&surface_blend, 0, 0,
				  es->geometry.width, es->geometry.height);
	pixman_region32_subtract(&surface_blend, &surface_blend, &es->opaque);

	if (pixman_region32_not_empty(&es->opaque)) {
		if (gs->input == INPUT_RGBA) {
			/* Special case for RGBA textures with possibly
			 * bad data in alpha channel: use the shader
			 * that forces texture alpha = 1.0.
			 * Xwayland surfaces need this.
			 */

			struct gl_shader *rgbx_shader = gl_select_shader(gr,
				INPUT_RGBX,
				output_attribute,
				gs->conversion);
			gl_use_shader(gr, rgbx_shader);
			gl_shader_setup(rgbx_shader, es, output);
		}

		if (transparent)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);

		repaint_surface(es, &repaint, &es->opaque);
	}

	if (pixman_region32_not_empty(&surface_blend)) {
		gl_use_shader(gr, shader);
		glEnable(GL_BLEND);
		repaint_surface(es, &repaint, &surface_blend);
	}

	pixman_region32_fini(&surface_blend);

out:
	pixman_region32_fini(&repaint);
}

static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_surface *surface;

	repaint_surfaces_start(output, damage);

	wl_list_for_each_reverse(surface, &compositor->surface_list, link)
		if (surface->plane == &compositor->primary_plane)
			draw_surface(surface, output, damage);

	repaint_surfaces_finish(output, damage);
}


static int
texture_border(struct weston_output *output)
{
	struct weston_compositor *ec = output->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	GLfloat *d;
	unsigned int *p;
	int i, j, k, n;
	GLfloat x[4], y[4], u[4], v[4];

	x[0] = -gr->border.left;
	x[1] = 0;
	x[2] = output->current->width;
	x[3] = output->current->width + gr->border.right;

	y[0] = -gr->border.top;
	y[1] = 0;
	y[2] = output->current->height;
	y[3] = output->current->height + gr->border.bottom;

	u[0] = 0.0;
	u[1] = (GLfloat) gr->border.left / gr->border.width;
	u[2] = (GLfloat) (gr->border.width - gr->border.right) / gr->border.width;
	u[3] = 1.0;

	v[0] = 0.0;
	v[1] = (GLfloat) gr->border.top / gr->border.height;
	v[2] = (GLfloat) (gr->border.height - gr->border.bottom) / gr->border.height;
	v[3] = 1.0;

	n = 8;
	d = wl_array_add(&ec->vertices, n * 16 * sizeof *d);
	p = wl_array_add(&ec->indices, n * 6 * sizeof *p);

	k = 0;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {

			if (i == 1 && j == 1)
				continue;

			d[ 0] = x[i];
			d[ 1] = y[j];
			d[ 2] = u[i];
			d[ 3] = v[j];

			d[ 4] = x[i];
			d[ 5] = y[j + 1];
			d[ 6] = u[i];
			d[ 7] = v[j + 1];

			d[ 8] = x[i + 1];
			d[ 9] = y[j];
			d[10] = u[i + 1];
			d[11] = v[j];

			d[12] = x[i + 1];
			d[13] = y[j + 1];
			d[14] = u[i + 1];
			d[15] = v[j + 1];

			p[0] = k + 0;
			p[1] = k + 1;
			p[2] = k + 2;
			p[3] = k + 2;
			p[4] = k + 1;
			p[5] = k + 3;

			d += 16;
			p += 6;
			k += 4;
		}

	return k / 4;
}

static void
draw_border(struct weston_output *output)
{
	struct weston_compositor *ec = output->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_shader *shader;
	GLfloat *v;
	int n;

	shader = gl_select_shader(gr, INPUT_RGBA, OUTPUT_BLEND,
		CONVERSION_NONE);

	glDisable(GL_BLEND);
	gl_use_shader(gr, shader);

	gl_shader_set_output(shader, output);

	n = texture_border(output);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gr->border.texture);

	v = ec->vertices.data;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, n * 6,
		       GL_UNSIGNED_INT, ec->indices.data);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	ec->vertices.size = 0;
	ec->indices.size = 0;
}

static void
gl_renderer_repaint_output(struct weston_output *output,
			      pixman_region32_t *output_damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct weston_compositor *compositor = output->compositor;
	struct gl_renderer *gr = get_renderer(compositor);
	EGLBoolean ret;
	static int errored;
	int32_t width, height, i;

	width = output->current->width +
		output->border.left + output->border.right;
	height = output->current->height +
		output->border.top + output->border.bottom;

	glViewport(0, 0, width, height);

	if (use_output(output) < 0)
		return;

	/* if debugging, redraw everything outside the damage to clean up
	 * debug lines from the previous draw on this buffer:
	 */
	if (compositor->fan_debug) {
		pixman_region32_t undamaged;
		pixman_region32_init(&undamaged);
		pixman_region32_subtract(&undamaged, &output->region,
					 output_damage);
		compositor->fan_debug = 0;
		repaint_surfaces(output, &undamaged);
		compositor->fan_debug = 1;
		pixman_region32_fini(&undamaged);
	}

	for (i = 0; i < 2; i++)
		pixman_region32_union(&output->buffer_damage[i],
				      &output->buffer_damage[i],
				      output_damage);

	pixman_region32_union(output_damage, output_damage,
			      &output->buffer_damage[output->current_buffer]);

	repaint_surfaces(output, output_damage);

	if (gr->border.texture)
		draw_border(output);

	wl_signal_emit(&output->frame_signal, output);

	ret = eglSwapBuffers(gr->egl_display, go->egl_surface);
	if (ret == EGL_FALSE && !errored) {
		errored = 1;
		weston_log("Failed in eglSwapBuffers.\n");
		print_egl_error_state();
	}

	output->current_buffer ^= 1;

}

static int
gl_renderer_read_pixels(struct weston_output *output,
			       pixman_format_code_t format, void *pixels,
			       uint32_t x, uint32_t y,
			       uint32_t width, uint32_t height)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	GLenum gl_format;

	switch (format) {
	case PIXMAN_a8r8g8b8:
		gl_format = gr->bgra_format;
		break;
	case PIXMAN_a8b8g8r8:
		gl_format = GL_RGBA;
		break;
	default:
		return -1;
	}

	if (use_output(output) < 0)
		return -1;

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, width, height, gl_format,
		     GL_UNSIGNED_BYTE, pixels);

	return 0;
}

static void
gl_renderer_flush_damage(struct weston_surface *surface)
{
	struct gl_renderer *gr = get_renderer(surface->compositor);
	struct gl_surface_state *gs = get_surface_state(surface);

#ifdef GL_UNPACK_ROW_LENGTH
	pixman_box32_t *rectangles;
	void *data;
	int i, n;
#endif

	pixman_region32_union(&surface->texture_damage,
			      &surface->texture_damage, &surface->damage);

	/* Avoid upload, if the texture won't be used this time.
	 * We still accumulate the damage in texture_damage.
	 */
	if (surface->plane != &surface->compositor->primary_plane)
		return;

	if (!pixman_region32_not_empty(&surface->texture_damage))
		return;

	glBindTexture(GL_TEXTURE_2D, gs->textures[0]);

	if (!gr->has_unpack_subimage) {
		glTexImage2D(GL_TEXTURE_2D, 0, gr->bgra_internal_format,
			     surface->pitch, surface->buffer->height, 0,
			     gr->bgra_format, GL_UNSIGNED_BYTE,
			     wl_shm_buffer_get_data(surface->buffer));

		goto done;
	}

#ifdef GL_UNPACK_ROW_LENGTH
	/* Mesa does not define GL_EXT_unpack_subimage */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, surface->pitch);
	data = wl_shm_buffer_get_data(surface->buffer);
	rectangles = pixman_region32_rectangles(&surface->texture_damage, &n);
	for (i = 0; i < n; i++) {
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, rectangles[i].x1);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, rectangles[i].y1);
		glTexSubImage2D(GL_TEXTURE_2D, 0,
				rectangles[i].x1, rectangles[i].y1,
				rectangles[i].x2 - rectangles[i].x1,
				rectangles[i].y2 - rectangles[i].y1,
				gr->bgra_format, GL_UNSIGNED_BYTE, data);
	}
#endif

done:
	pixman_region32_fini(&surface->texture_damage);
	pixman_region32_init(&surface->texture_damage);
}

static void
ensure_textures(struct gl_surface_state *gs, int num_textures)
{
	int i;

	if (num_textures <= gs->num_textures)
		return;

	for (i = gs->num_textures; i < num_textures; i++) {
		glGenTextures(1, &gs->textures[i]);
		glBindTexture(gs->target, gs->textures[i]);
		glTexParameteri(gs->target,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(gs->target,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	gs->num_textures = num_textures;
	glBindTexture(gs->target, 0);
}

static void
gl_renderer_attach(struct weston_surface *es, struct wl_buffer *buffer)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	EGLint attribs[3], format;
	int i, num_planes;

	if (!buffer) {
		for (i = 0; i < gs->num_images; i++) {
			gr->destroy_image(gr->egl_display, gs->images[i]);
			gs->images[i] = NULL;
		}
		gs->num_images = 0;
		glDeleteTextures(gs->num_textures, gs->textures);
		gs->num_textures = 0;
		return;
	}

	if (wl_buffer_is_shm(buffer)) {
		es->pitch = wl_shm_buffer_get_stride(buffer) / 4;
		gs->target = GL_TEXTURE_2D;

		ensure_textures(gs, 1);
		glBindTexture(GL_TEXTURE_2D, gs->textures[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, gr->bgra_internal_format,
			     es->pitch, buffer->height, 0,
			     gr->bgra_format, GL_UNSIGNED_BYTE, NULL);
		if (wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_XRGB8888)
			gs->input = INPUT_RGBX;
		else
			gs->input = INPUT_RGBA;
	} else if (gr->query_buffer(gr->egl_display, buffer,
				    EGL_TEXTURE_FORMAT, &format)) {
		for (i = 0; i < gs->num_images; i++)
			gr->destroy_image(gr->egl_display, gs->images[i]);
		gs->num_images = 0;
		gs->target = GL_TEXTURE_2D;
		switch (format) {
		case EGL_TEXTURE_RGB:
		default:
			num_planes = 1;
			gs->input = INPUT_RGBX;
			break;
		case EGL_TEXTURE_RGBA:
			num_planes = 1;
			gs->input = INPUT_RGBA;
			break;
		case EGL_TEXTURE_EXTERNAL_WL:
			num_planes = 1;
			gs->target = GL_TEXTURE_EXTERNAL_OES;
			gs->input = INPUT_EGL_EXTERNAL;
			break;
		case EGL_TEXTURE_Y_UV_WL:
			num_planes = 2;
			gs->input = INPUT_Y_UV;
			break;
		case EGL_TEXTURE_Y_U_V_WL:
			num_planes = 3;
			gs->input = INPUT_Y_U_V;
			break;
		case EGL_TEXTURE_Y_XUXV_WL:
			num_planes = 2;
			gs->input = INPUT_Y_XUXV;
			break;
		}

		assert(num_planes <= MAX_PLANES);

		ensure_textures(gs, num_planes);
		for (i = 0; i < num_planes; i++) {
			attribs[0] = EGL_WAYLAND_PLANE_WL;
			attribs[1] = i;
			attribs[2] = EGL_NONE;
			gs->images[i] = gr->create_image(gr->egl_display,
							 NULL,
							 EGL_WAYLAND_BUFFER_WL,
							 buffer, attribs);
			if (!gs->images[i]) {
				weston_log("failed to create img for plane %d\n", i);
				continue;
			}
			gs->num_images++;

			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(gs->target, gs->textures[i]);
			gr->image_target_texture_2d(gs->target,
						    gs->images[i]);
		}

		es->pitch = buffer->width;
	} else {
		weston_log("unhandled buffer type!\n");
	}

	if (ec->color_managed)
		gs->conversion = CONVERSION_FROM_SRGB;
	else
		gs->conversion = CONVERSION_NONE;
}

static void
gl_renderer_surface_set_color(struct weston_surface *surface,
		 float red, float green, float blue, float alpha)
{
	struct gl_surface_state *gs = get_surface_state(surface);

	gs->color[0] = red;
	gs->color[1] = green;
	gs->color[2] = blue;
	gs->color[3] = alpha;

	gs->input = INPUT_SOLID;
	gs->conversion = CONVERSION_NONE;
}

static int
gl_renderer_create_surface(struct weston_surface *surface)
{
	struct gl_surface_state *gs;

	gs = calloc(1, sizeof *gs);

	if (!gs)
		return -1;

	surface->renderer_state = gs;

	return 0;
}

static void
gl_renderer_destroy_surface(struct weston_surface *surface)
{
	struct gl_surface_state *gs = get_surface_state(surface);
	struct gl_renderer *gr = get_renderer(surface->compositor);
	int i;

	glDeleteTextures(gs->num_textures, gs->textures);

	for (i = 0; i < gs->num_images; i++)
		gr->destroy_image(gr->egl_display, gs->images[i]);

	free(gs);
}

static void
log_extensions(const char *name, const char *extensions)
{
	const char *p, *end;
	int l;
	int len;

	l = weston_log("%s:", name);
	p = extensions;
	while (*p) {
		end = strchrnul(p, ' ');
		len = end - p;
		if (l + len > 78)
			l = weston_log_continue("\n" STAMP_SPACE "%.*s",
						len, p);
		else
			l += weston_log_continue(" %.*s", len, p);
		for (p = end; isspace(*p); p++)
			;
	}
	weston_log_continue("\n");
}

static void
log_egl_gl_info(EGLDisplay egldpy)
{
	const char *str;

	str = eglQueryString(egldpy, EGL_VERSION);
	weston_log("EGL version: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_VENDOR);
	weston_log("EGL vendor: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_CLIENT_APIS);
	weston_log("EGL client APIs: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_EXTENSIONS);
	log_extensions("EGL extensions", str ? str : "(null)");

	str = (char *)glGetString(GL_VERSION);
	weston_log("GL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	weston_log("GLSL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_VENDOR);
	weston_log("GL vendor: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_RENDERER);
	weston_log("GL renderer: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_EXTENSIONS);
	log_extensions("GL extensions", str ? str : "(null)");
}

static void
log_egl_config_info(EGLDisplay egldpy, EGLConfig eglconfig)
{
	EGLint r, g, b, a;

	weston_log("Chosen EGL config details:\n");

	weston_log_continue(STAMP_SPACE "RGBA bits");
	if (eglGetConfigAttrib(egldpy, eglconfig, EGL_RED_SIZE, &r) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_GREEN_SIZE, &g) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_BLUE_SIZE, &b) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_ALPHA_SIZE, &a))
		weston_log_continue(": %d %d %d %d\n", r, g, b, a);
	else
		weston_log_continue(" unknown\n");

	weston_log_continue(STAMP_SPACE "swap interval range");
	if (eglGetConfigAttrib(egldpy, eglconfig, EGL_MIN_SWAP_INTERVAL, &a) &&
	    eglGetConfigAttrib(egldpy, eglconfig, EGL_MAX_SWAP_INTERVAL, &b))
		weston_log_continue(": %d - %d\n", a, b);
	else
		weston_log_continue(" unknown\n");
}

static void
output_apply_border(struct weston_output *output, struct gl_renderer *gr)
{
	output->border.top = gr->border.top;
	output->border.bottom = gr->border.bottom;
	output->border.left = gr->border.left;
	output->border.right = gr->border.right;
}

WL_EXPORT void
gl_renderer_set_border(struct weston_compositor *ec, int32_t width, int32_t height, void *data,
			  int32_t *edges)
{
	struct gl_renderer *gr = get_renderer(ec);
	struct weston_output *output;

	gr->border.left = edges[0];
	gr->border.right = edges[1];
	gr->border.top = edges[2];
	gr->border.bottom = edges[3];

	gr->border.width = width;
	gr->border.height = height;

	glGenTextures(1, &gr->border.texture);
	glBindTexture(GL_TEXTURE_2D, gr->border.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(GL_TEXTURE_2D, 0, gr->bgra_internal_format,
		     width,
		     height,
		     0, gr->bgra_format, GL_UNSIGNED_BYTE,
		     data);

	wl_list_for_each(output, &ec->output_list, link)
		output_apply_border(output, gr);
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface);

WL_EXPORT int
gl_renderer_output_create(struct weston_output *output,
				    EGLNativeWindowType window)
{
	struct weston_compositor *ec = output->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_output_state *go = calloc(1, sizeof *go);

	if (!go)
		return -1;

	go->egl_surface =
		eglCreateWindowSurface(gr->egl_display,
				       gr->egl_config,
				       window, NULL);

	if (go->egl_surface == EGL_NO_SURFACE) {
		weston_log("failed to create egl surface\n");
		free(go);
		return -1;
	}

	if (gr->egl_context == NULL)
		if (gl_renderer_setup(ec, go->egl_surface) < 0) {
			free(go);
			return -1;
		}

	glGenFramebuffers(1, &go->indirect_fbo);

	output->renderer_state = go;

	output_apply_border(output, gr);

	return 0;
}

WL_EXPORT void
gl_renderer_output_destroy(struct weston_output *output)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_output_state *go = get_output_state(output);

	glDeleteTextures(1, &go->indirect_texture);
	glDeleteFramebuffers(1, &go->indirect_fbo);

	eglDestroySurface(gr->egl_display, go->egl_surface);

	free(go);
}

WL_EXPORT EGLSurface
gl_renderer_output_surface(struct weston_output *output)
{
	return get_output_state(output)->egl_surface;
}

WL_EXPORT void
gl_renderer_destroy(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);

	if (gr->has_bind_display)
		gr->unbind_display(gr->egl_display, ec->wl_display);

	gl_destroy_shaders(gr);

	/* Work around crash in egl_dri2.c's dri2_make_current() - when does this apply? */
	eglMakeCurrent(gr->egl_display,
		       EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglTerminate(gr->egl_display);
	eglReleaseThread();

	free(ec->renderer);
	ec->renderer = NULL;
}

static int
egl_choose_config(struct gl_renderer *gr, const EGLint *attribs,
	const EGLint *visual_id)
{
	EGLint count = 0;
	EGLint matched = 0;
	EGLConfig *configs;
	int i;

	if (!eglGetConfigs(gr->egl_display, NULL, 0, &count) || count < 1)
		return -1;

	configs = calloc(count, sizeof *configs);
	if (!configs)
		return -1;

	if (!eglChooseConfig(gr->egl_display, attribs, configs,
			      count, &matched))
		goto out;

	for (i = 0; i < matched; ++i) {
		EGLint id;

		if (visual_id) {
			if (!eglGetConfigAttrib(gr->egl_display,
					configs[i], EGL_NATIVE_VISUAL_ID,
					&id))
				continue;

			if (id != *visual_id)
				continue;
		}

		gr->egl_config = configs[i];

		free(configs);
		return 0;
	}

out:
	free(configs);
	return -1;
}

WL_EXPORT const EGLint gl_renderer_opaque_attribs[] = {
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	EGL_RED_SIZE, 1,
	EGL_GREEN_SIZE, 1,
	EGL_BLUE_SIZE, 1,
	EGL_ALPHA_SIZE, 0,
	EGL_RENDERABLE_TYPE, GL_EGL_OPENGL_BIT,
	EGL_NONE
};

WL_EXPORT const EGLint gl_renderer_alpha_attribs[] = {
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	EGL_RED_SIZE, 1,
	EGL_GREEN_SIZE, 1,
	EGL_BLUE_SIZE, 1,
	EGL_ALPHA_SIZE, 1,
	EGL_RENDERABLE_TYPE, GL_EGL_OPENGL_BIT,
	EGL_NONE
};

WL_EXPORT int
gl_renderer_create(struct weston_compositor *ec, EGLNativeDisplayType display,
	const EGLint *attribs, const EGLint *visual_id)
{
	struct gl_renderer *gr;
	EGLint major, minor;

	gr = calloc(1, sizeof *gr);

	if (gr == NULL)
		return -1;

	gr->base.read_pixels = gl_renderer_read_pixels;
	gr->base.repaint_output = gl_renderer_repaint_output;
	gr->base.flush_damage = gl_renderer_flush_damage;
	gr->base.attach = gl_renderer_attach;
	gr->base.create_surface = gl_renderer_create_surface;
	gr->base.surface_set_color = gl_renderer_surface_set_color;
	gr->base.destroy_surface = gl_renderer_destroy_surface;

	gr->egl_display = eglGetDisplay(display);
	if (gr->egl_display == EGL_NO_DISPLAY) {
		weston_log("failed to create display\n");
		goto err_egl;
	}

	if (!eglInitialize(gr->egl_display, &major, &minor)) {
		weston_log("failed to initialize display\n");
		goto err_egl;
	}

	if (egl_choose_config(gr, attribs, visual_id) < 0) {
		weston_log("failed to choose EGL config\n");
		goto err_egl;
	}

	ec->renderer = &gr->base;

	return 0;

err_egl:
	print_egl_error_state();
	free(gr);
	return -1;
}

WL_EXPORT EGLDisplay
gl_renderer_display(struct weston_compositor *ec)
{
	return get_renderer(ec)->egl_display;
}

static void
fragment_debug_binding(struct wl_seat *seat, uint32_t time, uint32_t key,
		       void *data)
{
	struct weston_compositor *ec = data;
	struct gl_renderer *gr = get_renderer(ec);

	gr->fragment_shader_debug ^= 1;

	gl_compile_shaders(gr);
	weston_compositor_damage_all(ec);
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface)
{
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;
	EGLBoolean ret;
	GLint param;

#ifdef BUILD_OPENGL
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, 2,
		EGL_CONTEXT_MINOR_VERSION_KHR, 0,
		EGL_NONE
	};

	gr->bgra_internal_format = GL_RGBA;
	gr->bgra_format = GL_BGRA;
	gr->short_type = GL_UNSIGNED_SHORT;
	gr->rgba16_internal_format = GL_RGBA16;
	gr->l16_internal_format = GL_LUMINANCE16;
#else
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	gr->bgra_internal_format = GL_BGRA_EXT;
	gr->bgra_format = GL_BGRA_EXT;
	gr->short_type = GL_UNSIGNED_BYTE;
	gr->rgba16_internal_format = GL_RGBA;
	gr->l16_internal_format = GL_LUMINANCE;
#endif

	if (!eglBindAPI(OPENGL_ES_VER ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) {
		weston_log("failed to bind OpenGL API");
		print_egl_error_state();
		return -1;
	}

	log_egl_config_info(gr->egl_display, gr->egl_config);

	extensions =
		(const char *) eglQueryString(gr->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	if (!OPENGL_ES_VER && !strstr(extensions, "EGL_KHR_create_context")) {
		weston_log("EGL_KHR_create_context required to create OpenGL context\n");
		return -1;
	}

	gr->egl_context = eglCreateContext(gr->egl_display, gr->egl_config,
					   EGL_NO_CONTEXT, context_attribs);
	if (gr->egl_context == NULL) {
		weston_log("failed to create context\n");
		print_egl_error_state();
		return -1;
	}

	ret = eglMakeCurrent(gr->egl_display, egl_surface,
			     egl_surface, gr->egl_context);
	if (ret == EGL_FALSE) {
		weston_log("Failed to make EGL context current.\n");
		print_egl_error_state();
		return -1;
	}

	log_egl_gl_info(gr->egl_display);

	gr->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	gr->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	gr->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	gr->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	gr->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");
	gr->query_buffer =
		(void *) eglGetProcAddress("eglQueryWaylandBufferWL");

	if (strstr(extensions, "EGL_WL_bind_wayland_display"))
		gr->has_bind_display = 1;

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving GL extension string failed.\n");
		return -1;
	}

	if (OPENGL_ES_VER && !strstr(extensions, "GL_EXT_texture_format_BGRA8888")) {
		weston_log("GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &param);

	if (ec->color_managed)
		param--;

	if (param < MAX_PLANES) {
		weston_log("Too few OpenGL texture units available\n");
		return -1;
	}

	if (strstr(extensions, "GL_EXT_read_format_bgra"))
		ec->read_format = PIXMAN_a8r8g8b8;
	else
		ec->read_format = PIXMAN_a8b8g8r8;

	if (strstr(extensions, "GL_EXT_unpack_subimage"))
		gr->has_unpack_subimage = 1;

	if (strstr(extensions, "GL_OES_EGL_image_external"))
		gr->has_egl_image_external = 1;

	if (gr->has_bind_display) {
		ret = gr->bind_display(gr->egl_display, ec->wl_display);
		if (!ret)
			gr->has_bind_display = 0;
	}

	if (gl_init_shaders(gr) < 0)
		return -1;

	weston_compositor_add_debug_binding(ec, KEY_S,
					    fragment_debug_binding, ec);

	weston_log("GL ES 2 renderer features:\n");
	weston_log_continue(STAMP_SPACE "read-back format: %s\n",
			    ec->read_format == GL_BGRA_EXT ? "BGRA" : "RGBA");
	weston_log_continue(STAMP_SPACE "wl_shm sub-image to texture: %s\n",
			    gr->has_unpack_subimage ? "yes" : "no");
	weston_log_continue(STAMP_SPACE "EGL Wayland extension: %s\n",
			    gr->has_bind_display ? "yes" : "no");


	return 0;
}
