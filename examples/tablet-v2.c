#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wlr/render/egl.h>
#include "tablet-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-util.h>

#include <linux/input-event-codes.h>

/**
 * Usage: tablet-v2
 */

static int width = 500, height = 300;

static struct wl_compositor *compositor = NULL;
static struct wl_seat *seat = NULL;
static struct xdg_wm_base *wm_base = NULL;
static struct zwp_tablet_manager_v2 *tablet_manager = NULL;
static struct zwp_tablet_seat_v2 *tablet_seat = NULL;

static struct wl_list tablets;
static struct wl_list tools;
static struct wl_list pads;

struct wlr_egl egl;
struct wl_egl_window *egl_window;
struct wlr_egl_surface *egl_surface;

/* tablet-v2 struct definitions */

struct tablet_path {
	struct wl_list link;
	char *path;
};

struct tablet {
	struct wl_list link;
	struct zwp_tablet_v2 *tablet;

	struct wl_list paths;
	char *name;
	uint32_t vid;
	uint32_t pid;
};

struct tablet_pad {
	struct wl_list link;
	struct zwp_tablet_pad_v2 *pad;
	struct tablet *tablet;

	struct wl_list paths;
	uint32_t buttons;

	size_t group;
	struct wl_list groups;

	bool entered;

	size_t rings;
	size_t strips;
};

struct tablet_pad_group {
	struct wl_list link;
	struct zwp_tablet_pad_group_v2 *group;
	struct tablet_pad *pad;
	size_t index;

	uint32_t num_modes;
	uint32_t mode;
	uint32_t mode_serial;
	size_t num_buttons;
	int *buttons;

	struct wl_list rings;
	struct wl_list strips;
};

struct tablet_pad_ring {
	struct wl_list link;
	struct zwp_tablet_pad_ring_v2 *ring;
	struct tablet_pad_group *group;

	size_t index;
	enum zwp_tablet_pad_ring_v2_source source;
	double angle;
	bool stopped;
};

struct tablet_pad_strip {
	struct wl_list link;
	struct zwp_tablet_pad_strip_v2 *strip;
	struct tablet_pad_group *group;

	size_t index;
	enum zwp_tablet_pad_strip_v2_source source;
	uint32_t position;
	bool stopped;
};

static void handle_pad_strip_position(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		uint32_t position) {
	struct tablet_pad_strip *strip = data;

	strip->position = position;
}

static void handle_pad_strip_source(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		uint32_t source) {
	struct tablet_pad_strip *strip = data;

	strip->source = source;
}

static void handle_pad_strip_stop(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2) {
	struct tablet_pad_strip *strip = data;

	strip->stopped = true;
}

static void handle_pad_strip_frame(void *data,
		struct zwp_tablet_pad_strip_v2 *zwp_tablet_pad_strip_v2,
		uint32_t time) {
	struct tablet_pad_strip *strip = data;

	if (strip->source == ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER) {
		fprintf(stderr, "This interaction was triggered by a finger\n");
	}
	if (strip->stopped) {
		fprintf(stderr, "strip interaction stopped\n");
	} else {
		fprintf(stderr, "Got strip frame at position: %u\n", strip->position);
	}

	strip->position = -1;
	strip->source = 0;
	strip->stopped = false;
}

static const struct zwp_tablet_pad_strip_v2_listener tablet_pad_strip_listener = {
	.source = handle_pad_strip_source,
	.position = handle_pad_strip_position,
	.stop = handle_pad_strip_stop,
	.frame = handle_pad_strip_frame,
};

static void handle_pad_ring_angle(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		wl_fixed_t degrees) {
	struct tablet_pad_ring *ring = data;

	ring->angle = wl_fixed_to_double(degrees);
}

static void handle_pad_ring_source(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		uint32_t source) {
	struct tablet_pad_ring *ring = data;

	ring->source = source;
}

static void handle_pad_ring_stop(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2) {
	struct tablet_pad_ring *ring = data;

	ring->stopped = true;
}

static void handle_pad_ring_frame(void *data,
		struct zwp_tablet_pad_ring_v2 *zwp_tablet_pad_ring_v2,
		uint32_t time) {
	struct tablet_pad_ring *ring = data;

	if (ring->source == ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER) {
		fprintf(stderr, "This interaction was triggered by a finger\n");
	}
	if (ring->stopped) {
		fprintf(stderr, "Ring interaction stopped\n");
	} else {
		fprintf(stderr, "Got ring frame at angle: %G\n", ring->angle);
	}

	ring->angle = -1;
	ring->source = 0;
	ring->stopped = false;
}

static const struct zwp_tablet_pad_ring_v2_listener tablet_pad_ring_listener = {
	.source = handle_pad_ring_source,
	.angle = handle_pad_ring_angle,
	.stop = handle_pad_ring_stop,
	.frame = handle_pad_ring_frame,
};

/* pad_group handlers */

static void handle_tablet_pad_group_buttons(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct wl_array *buttons) {
	struct tablet_pad_group *group = data;

	group->buttons = calloc(1, buttons->size);
	if (!group->buttons) {
		return;
	}

	memcpy(group->buttons, buttons->data, buttons->size);
	group->num_buttons = buttons->size / sizeof(int);
}

static void handle_tablet_pad_group_modes(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		uint32_t modes) {
	struct tablet_pad_group *group = data;

	group->num_modes = modes;
}

static void handle_tablet_pad_group_strip(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct zwp_tablet_pad_strip_v2 *strip) {
	struct tablet_pad_group *group = data;
	struct tablet_pad_strip *pad_strip = calloc(1, sizeof(struct tablet_pad_strip));
	if (!pad_strip) {
		return;
	}

	zwp_tablet_pad_strip_v2_add_listener(strip, &tablet_pad_strip_listener, pad_strip);
	pad_strip->index = group->pad->strips++;
	wl_list_insert(&group->strips, &pad_strip->link);
	pad_strip->group = group;
	pad_strip->strip = strip;
}

static void handle_tablet_pad_group_ring(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		struct zwp_tablet_pad_ring_v2 *ring) {
	struct tablet_pad_group *group = data;
	struct tablet_pad_ring *pad_ring = calloc(1, sizeof(struct tablet_pad_ring));
	if (!pad_ring) {
		return;
	}

	zwp_tablet_pad_ring_v2_add_listener(ring, &tablet_pad_ring_listener, pad_ring);
	pad_ring->index = group->pad->rings++;
	wl_list_insert(&group->rings, &pad_ring->link);
	pad_ring->group = group;
	pad_ring->ring = ring;
}

static void handle_tablet_pad_group_done(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2) {
	struct tablet_pad_group *group = data;

	fprintf(stderr, "Group %lu is done initialising\n", group->index);
}

static void handle_tablet_pad_group_mode_switch(void *data,
		struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
		uint32_t time, uint32_t serial, uint32_t mode) {
	struct tablet_pad_group *group = data;

	group->mode = mode;
	group->mode_serial = serial;

	if (mode>= group->num_modes) {
		fprintf(stderr, "WARNING: Got mode that shouldn't exist\n");
	}
	fprintf(stderr, "Group %lu switching to mode %u\n", group->index, mode);
}

static const struct zwp_tablet_pad_group_v2_listener tablet_pad_group_listener = {
	.buttons = handle_tablet_pad_group_buttons,
	.modes = handle_tablet_pad_group_modes,
	.ring = handle_tablet_pad_group_ring,
	.strip = handle_tablet_pad_group_strip,
	.done = handle_tablet_pad_group_done,
	.mode_switch = handle_tablet_pad_group_mode_switch,
};
/* pad_group handlers */

/* tablet_pad handlers */
static void handle_tablet_pad_done(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2) {
	//struct tablet_pad *pad = data;

	fprintf(stderr, "Tablet pad is fully initialised\n");
}

static void handle_tablet_pad_buttons(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2, uint32_t buttons) {
	struct tablet_pad *pad = data;

	pad->buttons = buttons;
}

static void handle_tablet_pad_path(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2, const char *path) {
	struct tablet_pad *pad = data;

	struct tablet_path *tpath = calloc(1, sizeof(struct tablet_path));
	if (!tpath) {
		return;
	}

	tpath->path = strdup(path);
	wl_list_insert(&pad->paths, &tpath->link);
}

static void handle_tablet_pad_enter(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t serial, struct zwp_tablet_v2 *tablet_p,
		struct wl_surface *surface) {
	struct tablet_pad *pad = data;
	struct tablet *tablet= zwp_tablet_v2_get_user_data(tablet_p);

	pad->entered = true;
	if (tablet) {
		fprintf(stderr, "Pad entered surface on tablet \"%s\"\n",
			tablet->name);
		pad->tablet = tablet;
	} else {
		fprintf(stderr, "Pad entered surface on unkown tablet\n");
	}
}

static void handle_tablet_pad_leave(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t serial, struct wl_surface *surface) {
	struct tablet_pad *pad = data;

	pad->entered = false;
	pad->tablet = NULL;

	fprintf(stderr, "Pad leaft surface\n");
}

static void handle_tablet_pad_removed(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad) {
	struct tablet_pad *pad = data;

	fprintf(stderr, "Tablet_pad got removed\n");

	struct tablet_path *path;
	struct tablet_path *tmp;
	wl_list_for_each_safe(path, tmp, &pad->paths, link) {
		wl_list_remove(&path->link);
		free(path->path);
		free(path);
	}

	zwp_tablet_pad_v2_destroy(pad->pad);
	wl_list_remove(&pad->link);
	free(pad);
}

static void handle_tablet_pad_group(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad,
		struct zwp_tablet_pad_group_v2 *pad_group) {
	struct tablet_pad *pad = data;
	struct tablet_pad_group *group = calloc(1, sizeof(struct tablet_pad_group));
	if (!group) {
		return;
	}

	zwp_tablet_pad_group_v2_add_listener(pad_group, &tablet_pad_group_listener, group);

	group->index = pad->group++;
	group->group = pad_group;
	group->pad = pad;
	wl_list_insert(&pad->groups, &group->link);
	wl_list_init(&group->strips);
	wl_list_init(&group->rings);


	fprintf(stderr, "Got tablet pad group\n");
}

static void handle_tablet_pad_button(void *data,
		struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
		uint32_t time, uint32_t button, uint32_t state) {
	fprintf(stderr, "Got tablet pad button: %u\n", button);
}

static const struct zwp_tablet_pad_v2_listener tablet_pad_listener = {
	.group = handle_tablet_pad_group,
	.path = handle_tablet_pad_path,
	.buttons = handle_tablet_pad_buttons,
	.button = handle_tablet_pad_button,
	.done = handle_tablet_pad_done,
	.enter = handle_tablet_pad_enter,
	.leave = handle_tablet_pad_leave,
	.removed = handle_tablet_pad_removed,
};
/* tablet_pad handlers */

/* tablet handlers */
static void handle_tablet_name(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
		const char *name) {
	struct tablet *tablet = data;

	tablet->name = strdup(name);
}

static void handle_tablet_id(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
		uint32_t vid, uint32_t pid) {
	struct tablet *tablet = data;

	tablet->vid = vid;
	tablet->pid = pid;
}

static void handle_tablet_path(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
		const char *path) {
	struct tablet *tablet = data;

	struct tablet_path *tpath = calloc(1, sizeof(struct tablet_path));
	if (!tpath) {
		return;
	}

	tpath->path = strdup(path);
	wl_list_insert(&tablet->paths, &tpath->link);
}

static void handle_tablet_done(void *data, struct zwp_tablet_v2 *zwp_tablet_v2) {
	struct tablet *tablet = data;

	fprintf(stderr, "Tablet \"%s\" is fully initialised\n", tablet->name);
}

static void handle_tablet_removed(void *data, struct zwp_tablet_v2 *zwp_tablet) {
	struct tablet *tablet = data;

	fprintf(stderr, "Tablet \"%s\" got removed\n", tablet->name);

	free(tablet->name);

	struct tablet_path *path;
	struct tablet_path *tmp;
	wl_list_for_each_safe(path, tmp, &tablet->paths, link) {
		wl_list_remove(&path->link);
		free(path->path);
		free(path);
	}

	zwp_tablet_v2_destroy(tablet->tablet);
	wl_list_remove(&tablet->link);
	free(tablet);
}

static const struct zwp_tablet_v2_listener tablet_listener = {
	.name = handle_tablet_name,
	.id = handle_tablet_id,
	.path = handle_tablet_path,
	.done = handle_tablet_done,
	.removed = handle_tablet_removed,
};

/* /tablet handlers */

static void handle_tablet_added(void *data,
		struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2,
		struct zwp_tablet_v2 *id) {
	fprintf(stderr, "Got a new tablet\n");
	struct tablet *tablet = calloc(1, sizeof(struct tablet));
	if (!tablet) {
		zwp_tablet_v2_destroy(id);
		return;
	}
	zwp_tablet_v2_set_user_data(id, tablet);
	zwp_tablet_v2_add_listener(id, &tablet_listener, tablet);

	wl_list_insert(&tablets, &tablet->link);
	tablet->tablet = id;
	wl_list_init(&tablet->paths);
}

static void handle_tool_added(void *data,
		struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2,
		struct zwp_tablet_tool_v2 *id) {

}

static void handle_pad_added(void *data,
		struct zwp_tablet_seat_v2 *zwp_tablet_seat_v2,
		struct zwp_tablet_pad_v2 *id) {

	fprintf(stderr, "Got a new tablet_pad\n");
	struct tablet_pad *pad = calloc(1, sizeof(struct tablet_pad));
	if (!pad) {
		zwp_tablet_pad_v2_destroy(id);
		return;
	}
	zwp_tablet_pad_v2_set_user_data(id, pad);
	zwp_tablet_pad_v2_add_listener(id, &tablet_pad_listener, pad);

	wl_list_insert(&pads, &pad->link);
	pad->pad = id;
	wl_list_init(&pad->paths);
	wl_list_init(&pad->groups);
}

static const struct zwp_tablet_seat_v2_listener seat_listener = {
	.tablet_added = handle_tablet_added,
	.tool_added = handle_tool_added,
	.pad_added = handle_pad_added,
};


static void draw(void) {
	eglMakeCurrent(egl.display, egl_surface, egl_surface, egl.context);

	float color[] = {1.0, 1.0, 0.0, 1.0};

	glViewport(0, 0, width, height);
	glClearColor(color[0], color[1], color[2], 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	eglSwapBuffers(egl.display, egl_surface);
}

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	xdg_surface_ack_configure(xdg_surface, serial);
	wl_egl_window_resize(egl_window, width, height, 0, 0);
	draw();
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data,
		struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
		struct wl_array *states) {
	width = w;
	height = h;
}

static void xdg_toplevel_handle_close(void *data,
		struct xdg_toplevel *xdg_toplevel) {
	exit(EXIT_SUCCESS);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, "wl_compositor") == 0) {
		compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0) {
		tablet_manager = wl_registry_bind(registry, name,
			&zwp_tablet_manager_v2_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	// who cares
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

int main(int argc, char **argv) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "Failed to create display\n");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	if (compositor == NULL) {
		fprintf(stderr, "wl-compositor not available\n");
		return EXIT_FAILURE;
	}
	if (wm_base == NULL) {
		fprintf(stderr, "xdg-shell not available\n");
		return EXIT_FAILURE;
	}
	if (tablet_manager == NULL) {
		fprintf(stderr, "tablet not available\n");
		return EXIT_FAILURE;
	}
	if (seat == NULL) {
		fprintf(stderr, "seat not available\n");
		return EXIT_FAILURE;
	}
	tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(tablet_manager, seat);
	if (!tablet_seat) {
		fprintf(stderr, "Failed to create tablet seat\n");
		return EXIT_FAILURE;
	}
	wl_list_init(&tablets);
	wl_list_init(&tools);
	wl_list_init(&pads);
	zwp_tablet_seat_v2_add_listener(tablet_seat, &seat_listener, NULL);

	wlr_egl_init(&egl, EGL_PLATFORM_WAYLAND_EXT, display, NULL,
		WL_SHM_FORMAT_ARGB8888);

	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface =
		xdg_wm_base_get_xdg_surface(wm_base, surface);
	struct xdg_toplevel *xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);

	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);

	wl_surface_commit(surface);

	egl_window = wl_egl_window_create(surface, width, height);
	egl_surface = wlr_egl_create_surface(&egl, egl_window);

	wl_display_roundtrip(display);

	draw();

	while (wl_display_dispatch(display) != -1) {
		// This space intentionally left blank
	}

	return EXIT_SUCCESS;
}
