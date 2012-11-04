/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2012 John Kåre Alsaker
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <signal.h>

#include <linux/input.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

struct window;

struct display {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct {
		EGLDisplay dpy;
		EGLContext ctx;
		EGLConfig conf;
	} egl;
	uint32_t mask;
	struct window *window;
};

struct window {
	struct display *display;
	struct wl_egl_window *native;
	struct wl_surface *surface;
	EGLSurface egl_surface;
};

static void
init_egl(struct display *display)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	EGLint rgb_config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint rgba_config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n, v, i;
	EGLBoolean ret;
	EGLConfig *confs;


	display->egl.dpy = eglGetDisplay(display->display);
	assert(display->egl.dpy);

	ret = eglInitialize(display->egl.dpy, &major, &minor);
	assert(ret == EGL_TRUE);
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	assert(ret == EGL_TRUE);

	ret = eglChooseConfig(display->egl.dpy, rgb_config_attribs,
			      &display->egl.conf, 1, &n);
	assert(ret && n == 1);

	printf("Default RGB config: %p\n", display->egl.conf);

	ret = eglChooseConfig(display->egl.dpy, rgba_config_attribs,
			      &display->egl.conf, 1, &n);
	assert(ret && n == 1);

	printf("Default RGBA config: %p\n", display->egl.conf);

	ret = eglGetConfigs(display->egl.dpy, NULL, 0, &n);
	assert(ret == EGL_TRUE);

	confs = calloc(n, sizeof(*confs));

	ret = eglGetConfigs(display->egl.dpy, confs, n, &v);
	assert(ret == EGL_TRUE && n == v);

	for (i = 0; i < n; ++i) {
		printf("EGL config %p: ", confs[i]);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_RED_SIZE, &v) == EGL_TRUE)
			printf(" R%d", v);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_GREEN_SIZE, &v) == EGL_TRUE)
			printf(" G%d", v);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_BLUE_SIZE, &v) == EGL_TRUE)
			printf(" B%d", v);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_ALPHA_SIZE, &v) == EGL_TRUE)
			printf(" A%d", v);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_DEPTH_SIZE, &v) == EGL_TRUE)
			printf(" D%d", v);
		if (eglGetConfigAttrib(display->egl.dpy, confs[i], EGL_STENCIL_SIZE, &v) == EGL_TRUE)
			printf(" S%d", v);
		printf("\n");
	}

	display->egl.ctx = eglCreateContext(display->egl.dpy,
					    display->egl.conf,
					    EGL_NO_CONTEXT, context_attribs);
	assert(display->egl.ctx);
}

static void
fini_egl(struct display *display)
{
	/* Required, otherwise segfault in egl_dri2.c: dri2_make_current()
	 * on eglReleaseThread(). */
	eglMakeCurrent(display->egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	eglTerminate(display->egl.dpy);
	eglReleaseThread();
}

static void
create_surface(struct window *window)
{
	struct display *display = window->display;
	EGLBoolean ret;
	
	window->surface = wl_compositor_create_surface(display->compositor);

	window->native =
		wl_egl_window_create(window->surface, 1, 1);
	window->egl_surface =
		eglCreateWindowSurface(display->egl.dpy,
				       display->egl.conf,
				       window->native, NULL);

	ret = eglMakeCurrent(window->display->egl.dpy, window->egl_surface,
			     window->egl_surface, window->display->egl.ctx);
	assert(ret == EGL_TRUE);
}

static void
destroy_surface(struct window *window)
{
	wl_egl_window_destroy(window->native);

	wl_surface_destroy(window->surface);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
	struct display *d = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		d->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global
};

int
main(int argc, char **argv)
{
	struct display display = { 0 };
	struct window window  = { 0 };

	window.display = &display;
	display.window = &window;

	display.display = wl_display_connect(NULL);
	assert(display.display);

	display.registry = wl_display_get_registry(display.display);
	wl_registry_add_listener(display.registry,
				 &registry_listener, &display);

	wl_display_dispatch(display.display);

	init_egl(&display);
	create_surface(&window);
	destroy_surface(&window);
	fini_egl(&display);

	if (display.compositor)
		wl_compositor_destroy(display.compositor);

	wl_display_flush(display.display);
	wl_display_disconnect(display.display);

	return 0;
}
