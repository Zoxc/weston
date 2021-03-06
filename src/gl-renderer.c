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
gl_renderer_print_egl_error_state(void)
{
	EGLint code;

	code = eglGetError();
	weston_log("EGL error state: %s (0x%04lx)\n",
		egl_error_string(code), (long)code);
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))

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
calculate_edges(struct weston_view *ev, pixman_box32_t *rect,
		pixman_box32_t *surf_rect, GLfloat *ex, GLfloat *ey)
{

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
		weston_view_to_global_float(ev, surf.x[i], surf.y[i],
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
	if (!ev->transform.enabled)
		return clip_simple(&ctx, &surf, ex, ey);

	/* Transformed case: use a general polygon clipping algorithm to
	 * clip the surface rectangle with each side of 'rect'.
	 * The algorithm is Sutherland-Hodgman, as explained in
	 * http://www.codeguru.com/cpp/misc/misc/graphics/article.php/c8965/Polygon-Clipping.htm
	 * but without looking at any of that code.
	 */
	n = clip_transformed(&ctx, &surf, ex, ey);

	if (n < 3)
		return 0;

	return n;
}

static int
texture_region(struct weston_view *ev, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	struct gl_surface_state *gs = get_surface_state(ev->surface);
	struct weston_compositor *ec = ev->surface->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	GLfloat *v, inv_width, inv_height;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects, *surf_rects;
	int i, j, k, nrects, nsurf;

	rects = pixman_region32_rectangles(region, &nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	/* worst case we can have 8 vertices per rect (ie. clipped into
	 * an octagon):
	 */
	v = wl_array_add(&gr->vertices, nrects * nsurf * 8 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&gr->vtxcnt, nrects * nsurf * sizeof *vtxcnt);

	inv_width = 1.0 / gs->pitch;
        inv_height = 1.0 / gs->height;

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		for (j = 0; j < nsurf; j++) {
			pixman_box32_t *surf_rect = &surf_rects[j];
			GLfloat sx, sy, bx, by;
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
			n = calculate_edges(ev, rect, surf_rect, ex, ey);
			if (n < 3)
				continue;

			/* emit edge points: */
			for (k = 0; k < n; k++) {
				weston_view_from_global_float(ev, ex[k], ey[k],
							      &sx, &sy);
				/* position: */
				*(v++) = ex[k];
				*(v++) = ey[k];
				/* texcoord: */
				weston_surface_to_buffer_float(ev->surface,
							       sx, sy,
							       &bx, &by);
				*(v++) = bx * inv_width;
				if (gs->y_inverted) {
					*(v++) = by * inv_height;
				} else {
					*(v++) = (gs->height - by) * inv_height;
				}
			}

			vtxcnt[nvtx++] = n;
		}
	}

	return nvtx;
}

static void
triangle_fan_debug(struct weston_view *view, int first, int count)
{
	struct weston_compositor *compositor = view->surface->compositor;
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
repaint_region(struct weston_compositor *ec,
	       struct weston_view *ev, int nfans)
{
	struct gl_renderer *gr = get_renderer(ec);
	GLfloat *v;
	unsigned int *vtxcnt;
	int i, first;

	v = gr->vertices.data;
	vtxcnt = gr->vtxcnt.data;

	/* position: */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glEnableVertexAttribArray(0);

	/* texcoord: */
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(1);

	for (i = 0, first = 0; i < nfans; i++) {
		glDrawArrays(GL_TRIANGLE_FAN, first, vtxcnt[i]);

		if (ev && gr->fan_debug)
			triangle_fan_debug(ev, first, vtxcnt[i]);

		first += vtxcnt[i];
	}

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	gr->vertices.size = 0;
	gr->vtxcnt.size = 0;
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
		gl_renderer_print_egl_error_state();
		return -1;
	}

	return 0;
}

static void
repaint_view(struct weston_view *ev, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	/* The final region to be painted is the intersection of
	 * 'region' and 'surf_region'. However, 'region' is in the global
	 * coordinates, and 'surf_region' is in the surface-local
	 * coordinates. texture_region() will iterate over all pairs of
	 * rectangles from both regions, compute the intersection
	 * polygon for each pair, and store it as a triangle fan if
	 * it has a non-zero area (at least 3 vertices1, actually).
	 */
	int nfans = texture_region(ev, region, surf_region);

	repaint_region(ev->surface->compositor, ev, nfans);
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
	struct gl_renderer *gr = get_renderer(ec);
	GLfloat *v;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects;
	int i, nrects;

	rects = pixman_region32_rectangles(region, &nrects);

	v = wl_array_add(&gr->vertices, nrects * 4 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&gr->vtxcnt, nrects * sizeof *vtxcnt);

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
		output->current_mode->width,
		output->current_mode->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
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
repaint_views_start(struct weston_output *output)
{
	struct gl_output_state *go = get_output_state(output);

	go->indirect_drawing = get_renderer(output->compositor)->color_managed;

	if (go->indirect_disable)
		go->indirect_drawing = 0;

	if (go->indirect_drawing) {
		glBindFramebuffer(GL_FRAMEBUFFER, go->indirect_fbo);

		if (!go->indirect_texture)
			create_indirect_texture(output);
	}
}

static void
repaint_views_finish(struct weston_output *output,
		      pixman_region32_t *damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_shader *shader;

	if (go->indirect_drawing) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		// Viewport is set already by gl_renderer_repaint_output

		shader = gl_select_shader(gr,
			INPUT_RGBX,
			OUTPUT_TO_SRGB,
			CONVERSION_NONE);

		gl_use_shader(gr, shader);
		gl_shader_set_matrix(shader, &output->matrix);

		glActiveTexture(GL_TEXTURE0 + MAX_PLANES);
		glBindTexture(GL_TEXTURE_2D, gr->srgb_encode_lut);

		glDisable(GL_BLEND);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, go->indirect_texture);

		repaint_output(output, damage);
	}
}

static void
draw_view(struct weston_view *ev, struct weston_output *output,
	  pixman_region32_t *damage) /* in global coordinates */
{
	struct weston_compositor *ec = ev->surface->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(ev->surface);
	struct gl_shader *shader;
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	GLint filter;
	int i, transparent;
	enum gl_output_attribute output_attribute;

	/* In case of a runtime switch of renderers, we may not have received
	 * an attach for this surface since the switch. In that case we don't
	 * have a valid buffer or a proper shader set up so skip rendering. */
	if (gs->buffer_type == BUFFER_TYPE_NULL)
		return;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &ev->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &ev->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	if (gr->fan_debug) {
		gl_use_shader(gr, gr->solid_shader);
		gl_shader_setup(gr->solid_shader, ev, output);
	}

	transparent = ev->alpha < 1.0;
	output_attribute = transparent ? OUTPUT_TRANSPARENT : OUTPUT_BLEND;

	shader = gl_select_shader(gr, gs->input, output_attribute, gs->conversion);

	gl_use_shader(gr, shader);
	gl_shader_setup(shader, ev, output);

	if (ev->transform.enabled || output->zoom.active ||
	    output->current_scale != ev->surface->buffer_viewport.buffer.scale)
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
				  ev->surface->width, ev->surface->height);
	pixman_region32_subtract(&surface_blend, &surface_blend, &ev->surface->opaque);

	if (pixman_region32_not_empty(&surface_blend)) {
		glEnable(GL_BLEND);
		repaint_view(ev, &repaint, &surface_blend);
	}

	/* XXX: Should we be using ev->transform.opaque here? */
	if (pixman_region32_not_empty(&ev->surface->opaque)) {
		if (gs->input == INPUT_RGBA) {
			/* Special case for RGBA textures with possibly
			 * bad data in alpha channel: use the shader
			 * that forces texture alpha = 1.0.
			 * Xwayland surfaces need this.
			 */
			enum gl_conversion_attribute conversion_attribute = gs->conversion;
			struct gl_shader *rgbx_shader;

			/* Let OpenGL do sRGB decoding if it can */
			if(conversion_attribute == CONVERSION_FROM_SRGB && gs->srgb_image) {
				conversion_attribute = CONVERSION_NONE;
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(gs->target, gs->textures[1]);
			}

			rgbx_shader = gl_select_shader(gr,
				INPUT_RGBX,
				output_attribute,
				conversion_attribute);
			gl_use_shader(gr, rgbx_shader);
			gl_shader_setup(rgbx_shader, ev, output);
		}

		if (transparent)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);

		repaint_view(ev, &repaint, &ev->surface->opaque);
	}

	pixman_region32_fini(&surface_blend);

out:
	pixman_region32_fini(&repaint);
}

static void
repaint_views(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_view *view;

	repaint_views_start(output);

	wl_list_for_each_reverse(view, &compositor->view_list, link)
		if (view->plane == &compositor->primary_plane)
			draw_view(view, output, damage);

	repaint_views_finish(output, damage);
}

static void
draw_output_border_texture(struct gl_renderer *gr,
			   struct gl_output_state *go,
			   enum gl_renderer_border_side side,
			   int32_t x, int32_t y,
			   int32_t width, int32_t height)
{
	struct gl_border_image *img = &go->borders[side];
	static GLushort indices [] = { 0, 1, 3, 3, 1, 2 };

	if (!img->data) {
		if (img->tex) {
			glDeleteTextures(1, &img->tex);
			img->tex = 0;
		}

		return;
	}

	if (!img->tex) {
		glGenTextures(1, &img->tex);
		glBindTexture(GL_TEXTURE_2D, img->tex);

		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	} else {
		glBindTexture(GL_TEXTURE_2D, img->tex);
	}

	if (go->border_status & (1 << side)) {
#ifdef GL_EXT_unpack_subimage
		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
#endif
		glTexImage2D(GL_TEXTURE_2D, 0, gr->bgra_internal_format,
			     img->tex_width, img->height, 0,
			     gr->bgra_format, GL_UNSIGNED_BYTE, img->data);
	}

	GLfloat texcoord[] = {
		0.0f, 0.0f,
		(GLfloat)img->width / (GLfloat)img->tex_width, 0.0f,
		(GLfloat)img->width / (GLfloat)img->tex_width, 1.0f,
		0.0f, 1.0f,
	};

	GLfloat verts[] = {
		x, y,
		x + width, y,
		x + width, y + height,
		x, y + height
	};

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoord);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
}

static int
output_has_borders(struct weston_output *output)
{
	struct gl_output_state *go = get_output_state(output);

	return go->borders[GL_RENDERER_BORDER_TOP].data ||
	       go->borders[GL_RENDERER_BORDER_RIGHT].data ||
	       go->borders[GL_RENDERER_BORDER_BOTTOM].data ||
	       go->borders[GL_RENDERER_BORDER_LEFT].data;
}

static void
draw_output_borders(struct weston_output *output,
		    enum gl_border_status border_status)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_shader *shader;
	struct gl_border_image *top, *bottom, *left, *right;
	struct weston_matrix matrix;
	int full_width, full_height;

	if (border_status == BORDER_STATUS_CLEAN)
		return; /* Clean. Nothing to do. */

	shader = gl_select_shader(gr, INPUT_RGBA, OUTPUT_BLEND,
		CONVERSION_NONE);

	top = &go->borders[GL_RENDERER_BORDER_TOP];
	bottom = &go->borders[GL_RENDERER_BORDER_BOTTOM];
	left = &go->borders[GL_RENDERER_BORDER_LEFT];
	right = &go->borders[GL_RENDERER_BORDER_RIGHT];

	full_width = output->current_mode->width + left->width + right->width;
	full_height = output->current_mode->height + top->height + bottom->height;

	shader = gl_select_shader(gr, INPUT_RGBA, OUTPUT_BLEND, CONVERSION_NONE);

	glDisable(GL_BLEND);
	gl_use_shader(gr, shader);

	glViewport(0, 0, full_width, full_height);

	weston_matrix_init(&matrix);
	weston_matrix_translate(&matrix, -full_width/2.0, -full_height/2.0, 0);
	weston_matrix_scale(&matrix, 2.0/full_width, -2.0/full_height, 1);
	gl_shader_set_matrix(shader, &matrix);

	glUniform1f(shader->alpha_uniform, 1);
	glActiveTexture(GL_TEXTURE0);

	if (border_status & BORDER_TOP_DIRTY)
		draw_output_border_texture(gr, go, GL_RENDERER_BORDER_TOP,
					   0, 0,
					   full_width, top->height);
	if (border_status & BORDER_LEFT_DIRTY)
		draw_output_border_texture(gr, go, GL_RENDERER_BORDER_LEFT,
					   0, top->height,
					   left->width, output->current_mode->height);
	if (border_status & BORDER_RIGHT_DIRTY)
		draw_output_border_texture(gr, go, GL_RENDERER_BORDER_RIGHT,
					   full_width - right->width, top->height,
					   right->width, output->current_mode->height);
	if (border_status & BORDER_BOTTOM_DIRTY)
		draw_output_border_texture(gr, go, GL_RENDERER_BORDER_BOTTOM,
					   0, full_height - bottom->height,
					   full_width, bottom->height);
}

static void
output_get_border_damage(struct weston_output *output,
			 enum gl_border_status border_status,
			 pixman_region32_t *damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_border_image *top, *bottom, *left, *right;
	int full_width, full_height;

	if (border_status == BORDER_STATUS_CLEAN)
		return; /* Clean. Nothing to do. */

	top = &go->borders[GL_RENDERER_BORDER_TOP];
	bottom = &go->borders[GL_RENDERER_BORDER_BOTTOM];
	left = &go->borders[GL_RENDERER_BORDER_LEFT];
	right = &go->borders[GL_RENDERER_BORDER_RIGHT];

	full_width = output->current_mode->width + left->width + right->width;
	full_height = output->current_mode->height + top->height + bottom->height;
	if (border_status & BORDER_TOP_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, 0,
					   full_width, top->height);
	if (border_status & BORDER_LEFT_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, top->height,
					   left->width, output->current_mode->height);
	if (border_status & BORDER_RIGHT_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   full_width - right->width, top->height,
					   right->width, output->current_mode->height);
	if (border_status & BORDER_BOTTOM_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, full_height - bottom->height,
					   full_width, bottom->height);
}

static void
output_get_damage(struct weston_output *output,
		  pixman_region32_t *buffer_damage, uint32_t *border_damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	EGLint buffer_age = 0;
	EGLBoolean ret;
	int i;

	if (gr->has_egl_buffer_age) {
		ret = eglQuerySurface(gr->egl_display, go->egl_surface,
				      EGL_BUFFER_AGE_EXT, &buffer_age);
		if (ret == EGL_FALSE) {
			weston_log("buffer age query failed.\n");
			gl_renderer_print_egl_error_state();
		}
	}

	if (buffer_age == 0 || buffer_age - 1 > BUFFER_DAMAGE_COUNT) {
		pixman_region32_copy(buffer_damage, &output->region);
		*border_damage = BORDER_ALL_DIRTY;
	} else {
		for (i = 0; i < buffer_age - 1; i++)
			*border_damage |= go->border_damage[i];

		if (*border_damage & BORDER_SIZE_CHANGED) {
			/* If we've had a resize, we have to do a full
			 * repaint. */
			*border_damage |= BORDER_ALL_DIRTY;
			pixman_region32_copy(buffer_damage, &output->region);
		} else {
			for (i = 0; i < buffer_age - 1; i++)
				pixman_region32_union(buffer_damage,
						      buffer_damage,
						      &go->buffer_damage[i]);
		}
	}
}

static void
output_rotate_damage(struct weston_output *output,
		     pixman_region32_t *output_damage,
		     enum gl_border_status border_status)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	int i;

	if (!gr->has_egl_buffer_age)
		return;

	for (i = BUFFER_DAMAGE_COUNT - 1; i >= 1; i--) {
		go->border_damage[i] = go->border_damage[i - 1];
		pixman_region32_copy(&go->buffer_damage[i],
				     &go->buffer_damage[i - 1]);
	}

	go->border_damage[0] = border_status;
	pixman_region32_copy(&go->buffer_damage[0], output_damage);
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
#ifdef EGL_EXT_swap_buffers_with_damage
	int i, nrects, buffer_height;
	EGLint *egl_damage, *d;
	pixman_box32_t *rects;
#endif
	pixman_region32_t buffer_damage, total_damage;
	enum gl_border_status border_damage = BORDER_STATUS_CLEAN;

	/* Calculate the viewport */
	glViewport(go->borders[GL_RENDERER_BORDER_LEFT].width,
		   go->borders[GL_RENDERER_BORDER_BOTTOM].height,
		   output->current_mode->width,
		   output->current_mode->height);

	if (use_output(output) < 0)
		return;

	/* if debugging, redraw everything outside the damage to clean up
	 * debug lines from the previous draw on this buffer:
	 */
	if (gr->fan_debug) {
		pixman_region32_t undamaged;
		pixman_region32_init(&undamaged);
		pixman_region32_subtract(&undamaged, &output->region,
					 output_damage);
		gr->fan_debug = 0;
		repaint_views(output, &undamaged);
		gr->fan_debug = 1;
		pixman_region32_fini(&undamaged);
	}

	pixman_region32_init(&total_damage);
	pixman_region32_init(&buffer_damage);

	output_get_damage(output, &buffer_damage, &border_damage);
	output_rotate_damage(output, output_damage, go->border_status);

	pixman_region32_union(&total_damage, &buffer_damage, output_damage);
	border_damage |= go->border_status;

	repaint_views(output, &total_damage);

	pixman_region32_fini(&total_damage);
	pixman_region32_fini(&buffer_damage);

	draw_output_borders(output, border_damage);

	pixman_region32_copy(&output->previous_damage, output_damage);
	wl_signal_emit(&output->frame_signal, output);

#ifdef EGL_EXT_swap_buffers_with_damage
	if (gr->swap_buffers_with_damage) {
		pixman_region32_init(&buffer_damage);
		weston_transformed_region(output->width, output->height,
					  output->transform,
					  output->current_scale,
					  output_damage, &buffer_damage);

		if (output_has_borders(output)) {
			pixman_region32_translate(&buffer_damage,
						  go->borders[GL_RENDERER_BORDER_LEFT].width,
						  go->borders[GL_RENDERER_BORDER_TOP].height);
			output_get_border_damage(output, go->border_status,
						 &buffer_damage);
		}

		rects = pixman_region32_rectangles(&buffer_damage, &nrects);
		egl_damage = malloc(nrects * 4 * sizeof(EGLint));

		buffer_height = go->borders[GL_RENDERER_BORDER_TOP].height +
				output->current_mode->height +
				go->borders[GL_RENDERER_BORDER_BOTTOM].height;

		d = egl_damage;
		for (i = 0; i < nrects; ++i) {
			*d++ = rects[i].x1;
			*d++ = buffer_height - rects[i].y2;
			*d++ = rects[i].x2 - rects[i].x1;
			*d++ = rects[i].y2 - rects[i].y1;
		}
		ret = gr->swap_buffers_with_damage(gr->egl_display,
						   go->egl_surface,
						   egl_damage, nrects);
		free(egl_damage);
		pixman_region32_fini(&buffer_damage);
	} else {
		ret = eglSwapBuffers(gr->egl_display, go->egl_surface);
	}
#else /* ! defined EGL_EXT_swap_buffers_with_damage */
	ret = eglSwapBuffers(gr->egl_display, go->egl_surface);
#endif

	if (ret == EGL_FALSE && !errored) {
		errored = 1;
		weston_log("Failed in eglSwapBuffers.\n");
		gl_renderer_print_egl_error_state();
	}

	go->border_status = BORDER_STATUS_CLEAN;
}

static int
gl_renderer_read_pixels(struct weston_output *output,
			       pixman_format_code_t format, void *pixels,
			       uint32_t x, uint32_t y,
			       uint32_t width, uint32_t height)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	GLenum gl_format;
	struct gl_output_state *go = get_output_state(output);

	x += go->borders[GL_RENDERER_BORDER_LEFT].width;
	y += go->borders[GL_RENDERER_BORDER_BOTTOM].height;

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
	struct weston_buffer *buffer = gs->buffer_ref.buffer;
	struct weston_view *view;
	int texture_used;

#ifdef GL_EXT_unpack_subimage
	pixman_box32_t *rectangles;
	void *data;
	int i, n;
#endif

	pixman_region32_union(&gs->texture_damage,
			      &gs->texture_damage, &surface->damage);

	if (!buffer)
		return;

	/* Avoid upload, if the texture won't be used this time.
	 * We still accumulate the damage in texture_damage, and
	 * hold the reference to the buffer, in case the surface
	 * migrates back to the primary plane.
	 */
	texture_used = 0;
	wl_list_for_each(view, &surface->views, surface_link) {
		if (view->plane == &surface->compositor->primary_plane) {
			texture_used = 1;
			break;
		}
	}
	if (!texture_used)
		return;

	if (!pixman_region32_not_empty(&gs->texture_damage) &&
	    !gs->needs_full_upload)
		goto done;

	glBindTexture(GL_TEXTURE_2D, gs->textures[0]);

	if (!gr->has_unpack_subimage) {
		wl_shm_buffer_begin_access(buffer->shm_buffer);
		glTexImage2D(GL_TEXTURE_2D, 0, gs->gl_internal_format,
			     gs->pitch, buffer->height, 0,
			     gs->gl_format, gs->gl_pixel_type,
			     wl_shm_buffer_get_data(buffer->shm_buffer));
		wl_shm_buffer_end_access(buffer->shm_buffer);

		goto done;
	}

#ifdef GL_EXT_unpack_subimage
	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, gs->pitch);
	data = wl_shm_buffer_get_data(buffer->shm_buffer);

	if (gs->needs_full_upload) {
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
		wl_shm_buffer_begin_access(buffer->shm_buffer);
		glTexImage2D(GL_TEXTURE_2D, 0, gs->gl_internal_format,
			     gs->pitch, buffer->height, 0,
			     gs->gl_format, gs->gl_pixel_type, data);
		wl_shm_buffer_end_access(buffer->shm_buffer);
		goto done;
	}

	rectangles = pixman_region32_rectangles(&gs->texture_damage, &n);
	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < n; i++) {
		pixman_box32_t r;

		r = weston_surface_to_buffer_rect(surface, rectangles[i]);

		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, r.x1);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, r.y1);
		glTexSubImage2D(GL_TEXTURE_2D, 0, r.x1, r.y1,
				r.x2 - r.x1, r.y2 - r.y1,
				gs->gl_format, gs->gl_pixel_type, data);
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);
#endif

done:
	pixman_region32_fini(&gs->texture_damage);
	pixman_region32_init(&gs->texture_damage);
	gs->needs_full_upload = 0;

	weston_buffer_reference(&gs->buffer_ref, NULL);
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
destroy_images(struct gl_renderer *gr, struct gl_surface_state *gs)
{
	int i;

	for (i = 0; i < gs->num_images; i++)
		gr->destroy_image(gr->egl_display, gs->images[i]);

	gs->num_images = 0;
}

static void
destroy_textures(struct gl_surface_state *gs)
{
	glDeleteTextures(gs->num_textures, gs->textures);
	gs->num_textures = 0;
}

static const EGLint image_gamma_linear_attribs[3] = {EGL_GAMMA_MESA, EGL_COLORSPACE_LINEAR, EGL_NONE};
static const EGLint image_gamma_srgb_attribs[3] = {EGL_GAMMA_MESA, EGL_COLORSPACE_sRGB, EGL_NONE};

static int
create_texture_images(struct weston_surface *es, struct wl_resource *buffer)
{
	struct gl_surface_state *gs = get_surface_state(es);
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	EGLImageKHR image, srgb_image = NULL;
	const EGLint *attribs = NULL;

	if (gr->color_managed && gr->has_image_srgb) {
		attribs = image_gamma_linear_attribs;

		/* try to get an sRGB EGL image,
		 * skip this path if we don't want sRGB decoding */
		srgb_image = gr->create_image(gr->egl_display,
					 NULL,
					 EGL_WAYLAND_BUFFER_WL,
					 buffer, image_gamma_srgb_attribs);
		if(srgb_image)
			gs->srgb_image = 1;
	}

	if (srgb_image && gl_input_type_opaque(gs->input)) {
		image = srgb_image;
		srgb_image = NULL;
	} else
		image = gr->create_image(gr->egl_display,
					 NULL,
					 EGL_WAYLAND_BUFFER_WL,
					 buffer, attribs);

	if (!image) {
		if(srgb_image)
			gr->destroy_image(gr->egl_display, srgb_image);

		weston_log("failed to create img\n");
		return -1;
	}

	gs->num_images = srgb_image ? 2 : 1;

	ensure_textures(gs, gs->num_images);

	gs->images[0] = image;
	glBindTexture(gs->target, gs->textures[0]);
	gr->image_target_texture_2d(gs->target,
				    image);

	if (srgb_image) {
		gs->images[1] = srgb_image;
		glBindTexture(gs->target, gs->textures[1]);
		gr->image_target_texture_2d(gs->target,
					    srgb_image);
	}

	return 0;
}

static void
gl_renderer_attach_shm(struct weston_surface *es, struct weston_buffer *buffer,
		       struct wl_shm_buffer *shm_buffer)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	GLenum gl_format, gl_internal_format, gl_pixel_type;
	int pitch;

	buffer->shm_buffer = shm_buffer;
	buffer->width = wl_shm_buffer_get_width(shm_buffer);
	buffer->height = wl_shm_buffer_get_height(shm_buffer);

	switch (wl_shm_buffer_get_format(shm_buffer)) {
	case WL_SHM_FORMAT_XRGB8888:
		gs->input = INPUT_RGBX;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
		gl_internal_format = gr->bgra_internal_format;
		gl_format = gr->bgra_format;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		break;
	case WL_SHM_FORMAT_ARGB8888:
		gs->input = INPUT_RGBA;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
		gl_internal_format = gr->bgra_internal_format;
		gl_format = gr->bgra_format;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		break;
	case WL_SHM_FORMAT_RGB565:
		gs->input = INPUT_RGBX;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 2;
		gl_internal_format = GL_RGB;
		gl_format = GL_RGB;
		gl_pixel_type = GL_UNSIGNED_SHORT_5_6_5;
		break;
	default:
		weston_log("warning: unknown shm buffer format: %08x\n",
			   wl_shm_buffer_get_format(shm_buffer));
		return;
	}

	/* Only allocate a texture if it doesn't match existing one.
	 * If a switch from DRM allocated buffer to a SHM buffer is
	 * happening, we need to allocate a new texture buffer. */
	if (pitch != gs->pitch ||
	    buffer->height != gs->height ||
	    gl_internal_format != gs->gl_internal_format ||
	    gl_format != gs->gl_format ||
	    gl_pixel_type != gs->gl_pixel_type ||
	    gs->buffer_type != BUFFER_TYPE_SHM) {
		gs->pitch = pitch;
		gs->height = buffer->height;
		gs->target = GL_TEXTURE_2D;
		gs->gl_internal_format = gl_internal_format;
		gs->gl_format = gl_format;
		gs->gl_pixel_type = gl_pixel_type;
		gs->buffer_type = BUFFER_TYPE_SHM;
		gs->needs_full_upload = 1;
		gs->y_inverted = 1;

		gs->surface = es;

		ensure_textures(gs, 1);
	}
}

static void
gl_renderer_attach_egl(struct weston_surface *es, struct weston_buffer *buffer,
		       uint32_t format)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	EGLint attribs[3];
	int i, num_planes;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_WIDTH, &buffer->width);
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_HEIGHT, &buffer->height);
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_WAYLAND_Y_INVERTED_WL, &buffer->y_inverted);

	gs->target = GL_TEXTURE_2D;
	switch (format) {
	case EGL_TEXTURE_RGB:
		num_planes = 1;
		gs->input = INPUT_RGBX;
		break;
	case EGL_TEXTURE_RGBA:
	default:
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

	gs->pitch = buffer->width;
	gs->height = buffer->height;
	gs->buffer_type = BUFFER_TYPE_EGL;
	gs->y_inverted = buffer->y_inverted;

	if (num_planes == 1) {
		if (create_texture_images(es, buffer->resource) < 0)
			return;
	}

	ensure_textures(gs, num_planes);
	for (i = 0; i < num_planes; i++) {
		attribs[0] = EGL_WAYLAND_PLANE_WL;
		attribs[1] = i;
		attribs[2] = EGL_NONE;
		gs->images[i] = gr->create_image(gr->egl_display,
						 NULL,
						 EGL_WAYLAND_BUFFER_WL,
						 buffer->legacy_buffer,
						 attribs);
		if (!gs->images[i]) {
 			gs->num_images = i;
 			destroy_images(gr, gs);
			weston_log("failed to create img for plane %d\n", i);
			return;
		}

		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(gs->target, gs->textures[i]);
		gr->image_target_texture_2d(gs->target,
					    gs->images[i]);
	}

	gs->num_images = num_planes;
}

static void
gl_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	EGLint format;

	weston_buffer_reference(&gs->buffer_ref, buffer);
	destroy_images(gr, gs);

	gs->srgb_image = 0;
	gs->conversion = CONVERSION_NONE;

	if (!buffer) {
		destroy_textures(gs);
		gs->buffer_type = BUFFER_TYPE_NULL;
		gs->y_inverted = 1;
		return;
	}

	shm_buffer = wl_shm_buffer_get(buffer->resource);

	if (shm_buffer)
		gl_renderer_attach_shm(es, buffer, shm_buffer);
	else if (gr->query_buffer(gr->egl_display, (void *) buffer->resource,
				  EGL_TEXTURE_FORMAT, &format))
		gl_renderer_attach_egl(es, buffer, format);
	else {
		weston_log("unhandled buffer type!\n");
		weston_buffer_reference(&gs->buffer_ref, NULL);
		gs->buffer_type = BUFFER_TYPE_NULL;
		gs->y_inverted = 1;
	}

	if (!gr->color_managed) {
		gs->conversion = CONVERSION_NONE;
		return;
	}

	if (gl_input_type_opaque(gs->input) && gs->srgb_image)
		gs->conversion = CONVERSION_NONE;
	else
		gs->conversion = CONVERSION_FROM_SRGB;
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

static void
surface_state_destroy(struct gl_surface_state *gs, struct gl_renderer *gr)
{
	wl_list_remove(&gs->surface_destroy_listener.link);
	wl_list_remove(&gs->renderer_destroy_listener.link);

	gs->surface->renderer_state = NULL;

	destroy_textures(gs);
	destroy_images(gr, gs);

	weston_buffer_reference(&gs->buffer_ref, NULL);
	pixman_region32_fini(&gs->texture_damage);
	free(gs);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr;

	gs = container_of(listener, struct gl_surface_state,
			  surface_destroy_listener);

	gr = get_renderer(gs->surface->compositor);

	surface_state_destroy(gs, gr);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr;

	gr = data;

	gs = container_of(listener, struct gl_surface_state,
			  renderer_destroy_listener);

	surface_state_destroy(gs, gr);
}

int
gl_renderer_create_surface(struct weston_surface *surface)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr = get_renderer(surface->compositor);

	gs = calloc(1, sizeof *gs);
	if (!gs)
		return -1;

	/* A buffer is never attached to solid color surfaces, yet
	 * they still go through texcoord computations. Do not divide
	 * by zero there.
	 */
	gs->pitch = 1;
	gs->y_inverted = 1;

	gs->surface = surface;

	pixman_region32_init(&gs->texture_damage);
	surface->renderer_state = gs;

	gs->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &gs->surface_destroy_listener);

	gs->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&gr->destroy_signal,
		      &gs->renderer_destroy_listener);

	if (surface->buffer_ref.buffer) {
		gl_renderer_attach(surface, surface->buffer_ref.buffer);
		gl_renderer_flush_damage(surface);
	}

	return 0;
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

static int
egl_choose_config(struct gl_renderer *gr, const EGLint *attribs,
		  const EGLint *visual_id,
		  EGLConfig *config_out)
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

			if (id != 0 && id != *visual_id)
				continue;
		}

		*config_out = configs[i];

		free(configs);
		return 0;
	}

out:
	free(configs);
	return -1;
}

static void
gl_renderer_output_set_border(struct weston_output *output,
			      enum gl_renderer_border_side side,
			      int32_t width, int32_t height,
			      int32_t tex_width, unsigned char *data)
{
	struct gl_output_state *go = get_output_state(output);

	if (go->borders[side].width != width ||
	    go->borders[side].height != height)
		/* In this case, we have to blow everything and do a full
		 * repaint. */
		go->border_status |= BORDER_SIZE_CHANGED | BORDER_ALL_DIRTY;

	if (data == NULL) {
		width = 0;
		height = 0;
	}

	go->borders[side].width = width;
	go->borders[side].height = height;
	go->borders[side].tex_width = tex_width;
	go->borders[side].data = data;
	go->border_status |= 1 << side;
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface);

static int
gl_renderer_output_create(struct weston_output *output,
			  EGLNativeWindowType window,
			  const EGLint *attribs,
			  const EGLint *visual_id)
{
	struct weston_compositor *ec = output->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_output_state *go;
	EGLConfig egl_config;
	int i;

	if (egl_choose_config(gr, attribs, visual_id, &egl_config) == -1) {
		weston_log("failed to choose EGL config for output\n");
		return -1;
	}

	if (egl_config != gr->egl_config &&
	    !gr->has_configless_context) {
		weston_log("attempted to use a different EGL config for an "
			   "output but EGL_MESA_configless_context is not "
			   "supported\n");
		return -1;
	}

	go = calloc(1, sizeof *go);

	if (!go)
		return -1;

	go->egl_surface =
		eglCreateWindowSurface(gr->egl_display,
				       egl_config,
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

	for (i = 0; i < BUFFER_DAMAGE_COUNT; i++)
		pixman_region32_init(&go->buffer_damage[i]);

	glGenFramebuffers(1, &go->indirect_fbo);

	output->renderer_state = go;

	log_egl_config_info(gr->egl_display, egl_config);

	return 0;
}

static void
gl_renderer_output_destroy(struct weston_output *output)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_output_state *go = get_output_state(output);
	int i;

	for (i = 0; i < 2; i++)
		pixman_region32_fini(&go->buffer_damage[i]);

	glDeleteTextures(1, &go->indirect_texture);
	glDeleteFramebuffers(1, &go->indirect_fbo);

	eglDestroySurface(gr->egl_display, go->egl_surface);

	free(go);
}

static EGLSurface
gl_renderer_output_surface(struct weston_output *output)
{
	return get_output_state(output)->egl_surface;
}

static void
gl_renderer_destroy(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);

	wl_signal_emit(&gr->destroy_signal, gr);

	if (gr->has_bind_display)
		gr->unbind_display(gr->egl_display, ec->wl_display);

	gl_destroy_shaders(gr);

	/* Work around crash in egl_dri2.c's dri2_make_current() - when does this apply? */
	eglMakeCurrent(gr->egl_display,
		       EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglTerminate(gr->egl_display);
	eglReleaseThread();

	wl_array_release(&gr->vertices);
	wl_array_release(&gr->vtxcnt);

	if (gr->fragment_binding)
		weston_binding_destroy(gr->fragment_binding);
	if (gr->fan_binding)
		weston_binding_destroy(gr->fan_binding);

	free(gr);
	ec->renderer = NULL;
}

static int
gl_renderer_setup_egl_extensions(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;
	EGLBoolean ret;

	gr->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	gr->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	gr->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	gr->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");
	gr->query_buffer =
		(void *) eglGetProcAddress("eglQueryWaylandBufferWL");

	extensions =
		(const char *) eglQueryString(gr->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	if (strstr(extensions, "EGL_WL_bind_wayland_display"))
		gr->has_bind_display = 1;
	if (gr->has_bind_display) {
		ret = gr->bind_display(gr->egl_display, ec->wl_display);
		if (!ret)
			gr->has_bind_display = 0;
	}

	if (strstr(extensions, "EGL_EXT_buffer_age"))
		gr->has_egl_buffer_age = 1;
	else
		weston_log("warning: EGL_EXT_buffer_age not supported. "
			   "Performance could be affected.\n");

#ifdef EGL_EXT_swap_buffers_with_damage
	if (strstr(extensions, "EGL_EXT_swap_buffers_with_damage"))
		gr->swap_buffers_with_damage =
			(void *) eglGetProcAddress("eglSwapBuffersWithDamageEXT");
	else
		weston_log("warning: EGL_EXT_swap_buffers_with_damage not "
			   "supported. Performance could be affected.\n");
#endif

#ifdef EGL_MESA_configless_context
	if (strstr(extensions, "EGL_MESA_configless_context"))
		gr->has_configless_context = 1;
#endif

	return 0;
}

static const EGLint gl_renderer_opaque_attribs[] = {
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	EGL_RED_SIZE, 1,
	EGL_GREEN_SIZE, 1,
	EGL_BLUE_SIZE, 1,
	EGL_ALPHA_SIZE, 0,
	EGL_RENDERABLE_TYPE, GL_RENDERER_EGL_OPENGL_BIT,
	EGL_NONE
};

static const EGLint gl_renderer_alpha_attribs[] = {
	EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	EGL_RED_SIZE, 1,
	EGL_GREEN_SIZE, 1,
	EGL_BLUE_SIZE, 1,
	EGL_ALPHA_SIZE, 1,
	EGL_RENDERABLE_TYPE, GL_RENDERER_EGL_OPENGL_BIT,
	EGL_NONE
};

static int
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
	gr->base.surface_set_color = gl_renderer_surface_set_color;
	gr->base.destroy = gl_renderer_destroy;

	gr->egl_display = eglGetDisplay(display);
	if (gr->egl_display == EGL_NO_DISPLAY) {
		weston_log("failed to create display\n");
		goto err_egl;
	}

	if (!eglInitialize(gr->egl_display, &major, &minor)) {
		weston_log("failed to initialize display\n");
		goto err_egl;
	}

	if (egl_choose_config(gr, attribs, visual_id, &gr->egl_config) < 0) {
		weston_log("failed to choose EGL config\n");
		goto err_egl;
	}

	if (!OPENGL_ES_VER)
		gr->color_managed = ec->color_managed;

	ec->renderer = &gr->base;
	ec->capabilities |= WESTON_CAP_ROTATION_ANY;
	ec->capabilities |= WESTON_CAP_CAPTURE_YFLIP;

	if (gl_renderer_setup_egl_extensions(ec) < 0)
		goto err_egl;

	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_RGB565);

	wl_signal_init(&gr->destroy_signal);

	return 0;

err_egl:
	gl_renderer_print_egl_error_state();
	free(gr);
	return -1;
}

static EGLDisplay
gl_renderer_display(struct weston_compositor *ec)
{
	return get_renderer(ec)->egl_display;
}

static void
fragment_debug_binding(struct weston_seat *seat, uint32_t time, uint32_t key,
		       void *data)
{
	struct weston_compositor *ec = data;
	struct gl_renderer *gr = get_renderer(ec);

	gr->fragment_shader_debug ^= 1;

	gl_compile_shaders(gr);

	weston_compositor_damage_all(ec);
}

static void
fan_debug_repaint_binding(struct weston_seat *seat, uint32_t time, uint32_t key,
		      void *data)
{
	struct weston_compositor *compositor = data;
	struct gl_renderer *gr = get_renderer(compositor);

	gr->fan_debug = !gr->fan_debug;
	weston_compositor_damage_all(compositor);
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface)
{
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;
	EGLConfig context_config;
	EGLBoolean ret;
	GLint param;

#if !OPENGL_ES_VER
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
		weston_log("failed to bind EGL_OPENGL_ES_API\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

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

	context_config = gr->egl_config;

#ifdef EGL_MESA_configless_context
	if (gr->has_configless_context)
		context_config = EGL_NO_CONFIG_MESA;
#endif

	gr->egl_context = eglCreateContext(gr->egl_display, context_config,
					   EGL_NO_CONTEXT, context_attribs);
	if (gr->egl_context == NULL) {
		weston_log("failed to create context\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	ret = eglMakeCurrent(gr->egl_display, egl_surface,
			     egl_surface, gr->egl_context);
	if (ret == EGL_FALSE) {
		weston_log("Failed to make EGL context current.\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	log_egl_gl_info(gr->egl_display);

	gr->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");

	if (strstr(extensions, "EGL_MESA_image_sRGB"))
		gr->has_image_srgb = 1;

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

	if (gr->color_managed)
		param--;

	if (param < MAX_PLANES) {
		weston_log("Too few OpenGL texture units available\n");
		return -1;
	}

	if (strstr(extensions, "GL_EXT_read_format_bgra"))
		ec->read_format = PIXMAN_a8r8g8b8;
	else
		ec->read_format = PIXMAN_a8b8g8r8;

#ifdef GL_EXT_unpack_subimage
	if (strstr(extensions, "GL_EXT_unpack_subimage"))
		gr->has_unpack_subimage = 1;
#endif

	if (strstr(extensions, "GL_OES_EGL_image_external"))
		gr->has_egl_image_external = 1;

	if (gl_init_shaders(gr) < 0)
		return -1;

	gr->fragment_binding =
		weston_compositor_add_debug_binding(ec, KEY_S,
						    fragment_debug_binding,
						    ec);
	gr->fan_binding =
		weston_compositor_add_debug_binding(ec, KEY_F,
						    fan_debug_repaint_binding,
						    ec);

	weston_log("GL renderer features:\n");
	weston_log_continue(STAMP_SPACE "read-back format: %s\n",
		ec->read_format == PIXMAN_a8r8g8b8 ? "BGRA" : "RGBA");
	weston_log_continue(STAMP_SPACE "wl_shm sub-image to texture: %s\n",
			    gr->has_unpack_subimage ? "yes" : "no");
	weston_log_continue(STAMP_SPACE "EGL Wayland extension: %s\n",
			    gr->has_bind_display ? "yes" : "no");


	return 0;
}

WL_EXPORT struct gl_renderer_interface gl_renderer_interface = {
	.opaque_attribs = gl_renderer_opaque_attribs,
	.alpha_attribs = gl_renderer_alpha_attribs,

	.create = gl_renderer_create,
	.display = gl_renderer_display,
	.output_create = gl_renderer_output_create,
	.output_destroy = gl_renderer_output_destroy,
	.output_surface = gl_renderer_output_surface,
	.output_set_border = gl_renderer_output_set_border,
	.print_egl_error_state = gl_renderer_print_egl_error_state
};
