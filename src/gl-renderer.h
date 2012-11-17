/*
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

#include  "compositor.h"

#ifdef BUILD_OPENGL

#define OPENGL_ES_VER 0
#define GL_GLEXT_PROTOTYPES
#define GL_EGL_OPENGL_BIT EGL_OPENGL_BIT

#include <GL/gl.h>
#include <GL/glext.h>
#include <GLES2/gl2platform.h>

#else

#define OPENGL_ES
#define OPENGL_ES_VER 2
#define GL_EGL_OPENGL_BIT EGL_OPENGL_ES2_BIT
#include <GLES2/gl2.h>

#endif

#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

extern const EGLint gl_renderer_opaque_attribs[];
extern const EGLint gl_renderer_alpha_attribs[];

int
gl_renderer_create(struct weston_compositor *ec, EGLNativeDisplayType display,
	const EGLint *attribs, const EGLint *visual_id);
EGLDisplay
gl_renderer_display(struct weston_compositor *ec);
int
gl_renderer_output_create(struct weston_output *output,
				    EGLNativeWindowType window);
void
gl_renderer_output_destroy(struct weston_output *output);
EGLSurface
gl_renderer_output_surface(struct weston_output *output);
void
gl_renderer_set_border(struct weston_compositor *ec, int32_t width, int32_t height, void *data,
			  int32_t *edges);
void
gl_renderer_destroy(struct weston_compositor *ec);
