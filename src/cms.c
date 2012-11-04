/*
 * Copyright © 2012 John Kåre Alsaker
 * Copyright © 2008-2011 Kristian Høgsberg
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

#include <stdlib.h>
#include "compositor.h"

struct weston_cms {
	struct wl_object base;
	struct weston_compositor *ec;
	struct wl_global *global;
	struct wl_listener destroy_listener;
};

static void cms_set_surface_gamma(struct wl_client *client,
			 struct wl_resource *resource,
			 struct wl_resource *surface_resource,
			 uint32_t gamma)
{
	struct weston_surface *surface;

	if (!surface_resource)
		return;

	surface = surface_resource->data;

	switch (gamma) {
	case WL_CMS_GAMMA_AUTO:
	case WL_CMS_GAMMA_LINEAR:
	case WL_CMS_GAMMA_SRGB:
		surface->gamma = gamma;
		break;
	default:
		break;
	}
}

struct wl_cms_interface wl_cms_implementation = {
	cms_set_surface_gamma
};


static void
send_compositing_info(struct weston_cms *cms, struct wl_resource *resource)
{
	wl_cms_send_compositing_gamma(resource, cms->ec->color_managed ?
		WL_CMS_GAMMA_LINEAR : WL_CMS_GAMMA_NATIVE);
}

static void
bind_color(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct weston_cms *cms = data;

	struct wl_resource *resource;

	resource = wl_client_add_object(client, &wl_cms_interface,
			     &wl_cms_implementation, id, data);

	wl_cms_send_compositing_gamma(resource, cms->ec->color_managed ?
		WL_CMS_GAMMA_LINEAR : WL_CMS_GAMMA_SRGB);
	send_compositing_info(cms, resource);
}

static void
cms_destroy(struct wl_listener *listener, void *data)
{
	struct weston_cms *cms =
		container_of(listener, struct weston_cms, destroy_listener);

	wl_display_remove_global(cms->ec->wl_display, cms->global);
	free(cms);
}

void
cms_create(struct weston_compositor *ec)
{
	struct weston_cms *cms;

	if (!ec->color_managed)
		return;

	cms = malloc(sizeof *cms);
	if (cms == NULL)
		return;

	cms->base.interface = &wl_cms_interface;
	cms->base.implementation =
		(void(**)(void)) &wl_cms_implementation;
	cms->ec = ec;

	cms->global = wl_display_add_global(ec->wl_display,
						&wl_cms_interface,
						cms, bind_color);

	cms->destroy_listener.notify = cms_destroy;
	wl_signal_add(&ec->destroy_signal, &cms->destroy_listener);
}
