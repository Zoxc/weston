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

#ifndef _GL_INTERNAL_H_
#define _GL_INTERNAL_H_

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>
#include <linux/input.h>

#include "gl-renderer.h"

#include <EGL/eglext.h>
#include "weston-egl-ext.h"

#define MAX_PLANES 3

enum gl_shader_attribute {
	ATTRIBUTE_INPUT,
	ATTRIBUTE_OUTPUT,
	ATTRIBUTE_CONVERSION,
	ATTRIBUTE_COUNT
};

enum gl_conversion_attribute {
	CONVERSION_NONE,
	CONVERSION_FROM_SRGB,
	CONVERSION_COUNT
};

enum gl_output_attribute {
	OUTPUT_BLEND,
	OUTPUT_TO_SRGB,
	OUTPUT_COUNT
};

enum gl_input_attribute {
	INPUT_RGBX,
	INPUT_RGBA,
	INPUT_EGL_EXTERNAL,
	INPUT_Y_UV,
	INPUT_Y_U_V,
	INPUT_Y_XUXV,
	INPUT_SOLID,
	INPUT_COUNT
};

struct gl_shader {
	GLuint program;
	GLint projection_uniform;
	GLint color_uniform;
	GLint alpha_uniform;
};

struct gl_output_state {
	EGLSurface egl_surface;

	int indirect_disable;
	int indirect_drawing;
	GLuint indirect_texture;
	GLuint indirect_fbo;
};

struct gl_surface_state {
	GLfloat color[4];
	enum gl_input_attribute input;
	enum gl_conversion_attribute conversion;

	GLuint textures[MAX_PLANES];
	int num_textures;

	EGLImageKHR images[MAX_PLANES];
	GLenum target;
	int num_images;
};

struct gl_renderer {
	struct weston_renderer base;
	int fragment_shader_debug;

	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;

	struct {
		int32_t top, bottom, left, right;
		GLuint texture;
		int32_t width, height;
	} border;

	GLuint srgb_decode_lut;
	GLuint srgb_encode_lut;

	GLenum bgra_internal_format, bgra_format;
	GLenum rgba16_internal_format;
	GLenum l16_internal_format;
	GLenum short_type;

	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;

	int has_unpack_subimage;

	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	int has_bind_display;

	int has_egl_image_external;

	struct gl_shader *solid_shader;
	struct gl_shader *current_shader;

	struct gl_shader **shaders;
	size_t shader_count;
};

static inline struct gl_output_state *
get_output_state(struct weston_output *output)
{
	return (struct gl_output_state *)output->renderer_state;
}

static inline struct gl_surface_state *
get_surface_state(struct weston_surface *surface)
{
	return (struct gl_surface_state *)surface->renderer_state;
}

static inline struct gl_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct gl_renderer *)ec->renderer;
}

int
gl_init_shaders(struct gl_renderer *gr);

int
gl_compile_shaders(struct gl_renderer *gr);

void
gl_destroy_shaders(struct gl_renderer *gr);

void
gl_shader_set_output(struct gl_shader *shader,
		     struct weston_output *output);

void
gl_use_shader(struct gl_renderer *gr,
			     struct gl_shader *shader);

struct gl_shader *
gl_select_shader(struct gl_renderer *gr,
			enum gl_input_attribute input,
			enum gl_output_attribute output,
			enum gl_conversion_attribute conversion);

void
gl_shader_setup(struct gl_shader *shader,
		       struct weston_surface *surface,
		       struct weston_output *output);

#endif
