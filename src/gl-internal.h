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

#include "config.h"

#ifdef BUILD_DESKTOP_OPENGL

#define OPENGL_ES_VER 0
#define GL_GLEXT_PROTOTYPES
#define GL_RENDERER_EGL_OPENGL_BIT EGL_OPENGL_BIT

#include <GL/gl.h>
#include <GL/glext.h>
#include <GLES2/gl2platform.h>

#else

#define OPENGL_ES_VER 2
#define GL_RENDERER_EGL_OPENGL_BIT EGL_OPENGL_ES2_BIT
#include <GLES2/gl2.h>

#endif

#include <GLES2/gl2ext.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>
#include <linux/input.h>

#include "gl-renderer.h"
#include "vertex-clipping.h"

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
	OUTPUT_TRANSPARENT,
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
	size_t index;
	GLuint program;
	GLint projection_uniform;
	GLint color_uniform;
	GLint alpha_uniform;
};

#define BUFFER_DAMAGE_COUNT 2

enum gl_border_status {
	BORDER_STATUS_CLEAN = 0,
	BORDER_TOP_DIRTY = 1 << GL_RENDERER_BORDER_TOP,
	BORDER_LEFT_DIRTY = 1 << GL_RENDERER_BORDER_LEFT,
	BORDER_RIGHT_DIRTY = 1 << GL_RENDERER_BORDER_RIGHT,
	BORDER_BOTTOM_DIRTY = 1 << GL_RENDERER_BORDER_BOTTOM,
	BORDER_ALL_DIRTY = 0xf,
	BORDER_SIZE_CHANGED = 0x10
};

struct gl_border_image {
	GLuint tex;
	int32_t width, height;
	int32_t tex_width;
	void *data;
};

struct gl_output_state {
	EGLSurface egl_surface;
	pixman_region32_t buffer_damage[BUFFER_DAMAGE_COUNT];
	enum gl_border_status border_damage[BUFFER_DAMAGE_COUNT];
	struct gl_border_image borders[4];
	enum gl_border_status border_status;

	int indirect_disable;
	int indirect_drawing;
	GLuint indirect_texture;
	GLuint indirect_fbo;
};

enum buffer_type {
	BUFFER_TYPE_NULL,
	BUFFER_TYPE_SHM,
	BUFFER_TYPE_EGL
};

struct gl_surface_state {
	GLfloat color[4];
	enum gl_input_attribute input;
	enum gl_conversion_attribute conversion;

	GLuint textures[MAX_PLANES];
	int num_textures;
	int needs_full_upload;
	pixman_region32_t texture_damage;

	/* These are only used by SHM surfaces to detect when we need
	 * to do a full upload to specify a new internal texture
	 * format */
	GLenum gl_internal_format;
	GLenum gl_format;
	GLenum gl_pixel_type;

	EGLImageKHR images[MAX_PLANES];
	GLenum target;
	int num_images;

	int srgb_image;

	struct weston_buffer_reference buffer_ref;
	enum buffer_type buffer_type;
	int pitch; /* in pixels */
	int height; /* in pixels */
	int y_inverted;

	struct weston_surface *surface;

	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
};

struct gl_renderer {
	struct weston_renderer base;
	int fragment_shader_debug;
	int fan_debug;
	struct weston_binding *fragment_binding;
	struct weston_binding *fan_binding;

	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;

	struct wl_array vertices;
	struct wl_array vtxcnt;

	GLuint srgb_decode_lut;
	GLuint srgb_encode_lut;

	GLenum bgra_internal_format, bgra_format;
	GLenum rgba16_internal_format;
	GLenum l16_internal_format;
	GLenum short_type;

	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_2d;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;

#ifdef EGL_EXT_swap_buffers_with_damage
	PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC swap_buffers_with_damage;
#endif

	int color_managed;

	int has_unpack_subimage;

	PFNEGLBINDWAYLANDDISPLAYWL bind_display;
	PFNEGLUNBINDWAYLANDDISPLAYWL unbind_display;
	PFNEGLQUERYWAYLANDBUFFERWL query_buffer;
	int has_bind_display;

	int has_egl_image_external;

	int has_egl_buffer_age;

	int has_configless_context;

	int has_image_srgb;

	struct gl_shader *solid_shader;
	struct gl_shader *current_shader;

	GLuint vertex_shader;
	struct gl_shader **shaders;
	size_t shader_count;

	struct wl_signal destroy_signal;
};

static inline struct gl_output_state *
get_output_state(struct weston_output *output)
{
	return (struct gl_output_state *)output->renderer_state;
}

int
gl_renderer_create_surface(struct weston_surface *surface);

static inline struct gl_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		gl_renderer_create_surface(surface);

	return (struct gl_surface_state *)surface->renderer_state;
}

static inline struct gl_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct gl_renderer *)ec->renderer;
}

int
gl_input_type_opaque(enum gl_input_attribute input);

int
gl_init_shaders(struct gl_renderer *gr);

int
gl_compile_shaders(struct gl_renderer *gr);

void
gl_destroy_shaders(struct gl_renderer *gr);

void
gl_shader_set_matrix(struct gl_shader *shader,
		     struct weston_matrix *matrix);

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
		       struct weston_view *view,
		       struct weston_output *output);

#endif
