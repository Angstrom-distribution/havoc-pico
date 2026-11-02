/* A just-enough Wayland compositor to check havoc's text-input-v3 use without
 * a real compositor. It serves wl_compositor, wl_shm, xdg_wm_base, wl_seat
 * with a keyboard and, unless --no-text-input is given, text-input-v3. It
 * runs havoc as a child, moves the keyboard focus to and from its window and
 * types into it, and checks what havoc sent:
 *
 *  - nothing is enabled before the focus arrives
 *  - the focus enter is answered by exactly one enable of purpose terminal
 *    with a cursor rectangle, followed by a commit
 *  - typing does not enable (or commit) again
 *  - the focus leave is answered by a disable and a commit
 *  - the next enter enables again, once
 *  - the typed text reaches the shell in the terminal in every case
 *
 * With --no-text-input the same typing must still work.
 *
 * With --driver it is instead a test bench for havoc's damage reporting and is
 * run by damage.py: it takes commands on stdin (see driver_line) and, besides
 * serving the protocols, checks on every commit that the compositor's own
 * idea of the window, kept by applying only the damage rectangles it was told,
 * equals the committed buffer, that nothing writes to a buffer it still holds,
 * and that a new buffer size comes with damage for all of it. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-server.h"
#include "text-input-unstable-v3-server.h"
#include "viewporter-server.h"
#include "fractional-scale-v1-server.h"
#ifdef WITH_C8
/* built by c8-run.sh from the protocol XML of the picowl repository */
#include "picowl-palette-v1-server.h"
#endif

#define FORMAT_C8 0x20203843

#define PURPOSE_TERMINAL 13

static struct {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_event_source *timer;
	bool with_ti;
	pid_t child;
	const char *outfile;

	struct wl_resource *surface, *xdg_surface, *toplevel;
	struct wl_resource *keyboard, *ti;
	bool mapped;
	uint32_t serial;
	int stage, waited;

	/* text-input state as havoc sent it */
	bool p_enable, p_disable, enabled;
	uint32_t hint, purpose;
	int32_t rx, ry, rw, rh;
	int enables, disables, commits;
	int enables_at_typing;
	int commits_at_typing;

	int failed;
} C;

static void fail(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	C.failed = 1;
}

#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

/* wl_surface */
struct rect {
	int x, y, w, h;
};

#define MAX_RECTS 256
#define MAX_REGION 8

struct opaque {
	bool set;
	int n;
	struct rect r[MAX_REGION];
};

struct region {
	struct opaque o;
};

struct surf {
	struct wl_resource *buf;	/* attached, not yet committed */
	struct wl_resource *cb;
	int ndmg;
	struct rect dmg[MAX_RECTS];
	bool dmg_overflow, legacy_damage;
	bool opaque_pending;
	struct opaque opaque;
};

static void s_nop(struct wl_client *c, struct wl_resource *r) {}
static void s_attach(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *buf, int32_t x, int32_t y)
{
	struct surf *s = wl_resource_get_user_data(r);

	s->buf = buf;
}
static void s_damage(struct wl_client *c, struct wl_resource *r,
		     int32_t x, int32_t y, int32_t w, int32_t h)
{
	struct surf *s = wl_resource_get_user_data(r);

	s->legacy_damage = true;
}
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct surf *s = wl_resource_get_user_data(r);

	s->cb = wl_resource_create(c, &wl_callback_interface, 1, id);
}
static void s_opaque(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *region)
{
	struct surf *s = wl_resource_get_user_data(r);

	s->opaque_pending = true;
	memset(&s->opaque, 0, sizeof s->opaque);
	if (region)
		s->opaque = ((struct region *)
			     wl_resource_get_user_data(region))->o;
}
static void s_input_region(struct wl_client *c, struct wl_resource *r,
			   struct wl_resource *region) {}
static void s_int(struct wl_client *c, struct wl_resource *r, int32_t v) {}
static void s_dmg_buf(struct wl_client *c, struct wl_resource *r,
		      int32_t x, int32_t y, int32_t w, int32_t h)
{
	struct surf *s = wl_resource_get_user_data(r);

	if (s->ndmg < MAX_RECTS)
		s->dmg[s->ndmg++] = (struct rect){ x, y, w, h };
	else
		s->dmg_overflow = true;
}

static void s_commit(struct wl_client *c, struct wl_resource *r);

static const struct wl_surface_interface surface_impl = {
	.destroy = s_nop, .attach = s_attach, .damage = s_damage,
	.frame = s_frame, .set_opaque_region = s_opaque,
	.set_input_region = s_input_region, .commit = s_commit,
	.set_buffer_transform = s_int, .set_buffer_scale = s_int,
	.damage_buffer = s_dmg_buf,
};

static void comp_create_surface(struct wl_client *c, struct wl_resource *r,
				uint32_t id)
{
	struct wl_resource *s = wl_resource_create(c, &wl_surface_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(s, &surface_impl,
				       calloc(1, sizeof(struct surf)), NULL);
}

/* wl_region, kept only as far as havoc's use of it goes: rectangles added */
static void rg_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void rg_add(struct wl_client *c, struct wl_resource *r, int32_t x,
		   int32_t y, int32_t w, int32_t h)
{
	struct region *g = wl_resource_get_user_data(r);

	g->o.set = true;
	if (g->o.n < MAX_REGION)
		g->o.r[g->o.n++] = (struct rect){ x, y, w, h };
}
static void rg_sub(struct wl_client *c, struct wl_resource *r, int32_t x,
		   int32_t y, int32_t w, int32_t h)
{
	fail("havoc subtracts from a region");
}
static const struct wl_region_interface region_impl = {
	.destroy = rg_destroy, .add = rg_add, .subtract = rg_sub,
};
static void comp_create_region(struct wl_client *c, struct wl_resource *r,
			       uint32_t id)
{
	struct wl_resource *g = wl_resource_create(c, &wl_region_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(g, &region_impl,
				       calloc(1, sizeof(struct region)), NULL);
}
static const struct wl_compositor_interface comp_impl = {
	.create_surface = comp_create_surface,
	.create_region = comp_create_region,
};

/* What the compositor does with the window's commits. The compositor keeps its
 * own copy of the window (img) and updates it from the committed buffer only
 * where damage says it changed, which is all a compositor that composites from
 * a copy, or uploads to a display, can do. If that copy ever differs from the
 * committed buffer, damage was missing. */
enum { REL_IMMEDIATE, REL_NEXT, REL_TIMER, REL_NEVER };

#define MAX_LOG 512
#define LOG_RECTS 64
#define HOLD_MS 40

struct tbuf {
	struct wl_resource *res;
	struct wl_listener destroy;
	uint8_t *snap;
	size_t size;
	bool held;
	long long since;
	struct tbuf *next;
};

struct commit_rec {
	int w, h;
	uint32_t format;
	int n;
	struct rect r[LOG_RECTS];
	long long union_px, sum_px;
	bool full;
};

static struct {
	bool driver;
	/* what the terminal runs under /bin/sh -c; HAVOC_CTL is the fifo the
	 * driver feeds */
	const char *child_cmd;
	int release;
	struct tbuf *bufs;
	struct wl_event_source *release_timer;

	uint8_t *img;
	int w, h;
	uint32_t format;
	/* bytes per pixel of img and of the buffers: 4, or 1 for C8 */
	int bpp;

	/* C8 (--c8): the palette the surface has (applied at commit), the one
	 * it has had since, how often it was set, and what the compositor says
	 * about the window: -1 no palette global, 0 composited, 1 exclusive */
	int c8_mode;
	struct wl_resource *pal_res;
	uint32_t pal_pending[256], pal_cur[256];
	bool pal_is_pending;
	int pal_sets;
	struct opaque opaque;
	int vp_w, vp_h;
	struct wl_resource *pointer, *fs;

	struct commit_rec log[MAX_LOG];
	int nlog;
	int commits, stale, held_writes, no_full, lost;

	struct wl_event_source *wait_timer;
	bool wait_pending;
	int quiet_ms;
} D;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void tbuf_gone(struct wl_listener *l, void *data)
{
	struct tbuf *t = wl_container_of(l, t, destroy), **p;

	for (p = &D.bufs; *p; p = &(*p)->next)
		if (*p == t) {
			*p = t->next;
			break;
		}
	free(t->snap);
	free(t);
}

static struct tbuf *tbuf_get(struct wl_resource *res)
{
	struct tbuf *t;

	for (t = D.bufs; t; t = t->next)
		if (t->res == res)
			return t;
	t = calloc(1, sizeof *t);
	t->res = res;
	t->destroy.notify = tbuf_gone;
	wl_resource_add_destroy_listener(res, &t->destroy);
	t->next = D.bufs;
	D.bufs = t;
	return t;
}

/* whether what the compositor holds was left alone */
static void tbuf_check(struct tbuf *t)
{
	struct wl_shm_buffer *shm = wl_shm_buffer_get(t->res);

	if (!shm || !t->held)
		return;
	wl_shm_buffer_begin_access(shm);
	if (memcmp(wl_shm_buffer_get_data(shm), t->snap, t->size)) {
		fail("havoc wrote to a buffer the compositor still holds");
		D.held_writes++;
		memcpy(t->snap, wl_shm_buffer_get_data(shm), t->size);
	}
	wl_shm_buffer_end_access(shm);
}

static void tbuf_release(struct tbuf *t)
{
	tbuf_check(t);
	t->held = false;
	free(t->snap);
	t->snap = NULL;
	wl_buffer_send_release(t->res);
}

static int release_tick(void *data)
{
	struct tbuf *t;
	long long n = now_ms();

	wl_event_source_timer_update(D.release_timer, 10);
	if (D.release != REL_TIMER)
		return 0;
	for (t = D.bufs; t; t = t->next)
		if (t->held && n - t->since >= HOLD_MS) {
			tbuf_release(t);
			break;
		}
	return 0;
}

static void poke_wait(void)
{
	if (D.wait_pending)
		wl_event_source_timer_update(D.wait_timer, D.quiet_ms);
}

static void main_commit(struct surf *s)
{
	struct commit_rec *lr;
	struct wl_shm_buffer *shm;
	struct tbuf *t, *o;
	uint8_t *data, *mask;
	int w, h, stride, i, x, y, bpp;
	bool resized;

	if (!s->buf)
		return;
	C.mapped = true;
	shm = wl_shm_buffer_get(s->buf);
	if (!shm) {
		fail("the window's buffer is not a shm buffer");
		return;
	}
	w = wl_shm_buffer_get_width(shm);
	h = wl_shm_buffer_get_height(shm);
	stride = wl_shm_buffer_get_stride(shm);
	bpp = wl_shm_buffer_get_format(shm) == FORMAT_C8 ? 1 : 4;
	CHECK(stride == (bpp == 1 ? (w + 3) & ~3 : w * 4),
	      "stride %d for width %d", stride, w);

	t = tbuf_get(s->buf);
	CHECK(!t->held, "havoc attached a buffer that was not released");
	for (o = D.bufs; o; o = o->next)
		if (o != t)
			tbuf_check(o);

	wl_shm_buffer_begin_access(shm);
	data = wl_shm_buffer_get_data(shm);

	/* the damage, as rectangles and as the pixels they cover */
	mask = calloc(w * h, 1);
	lr = &D.log[D.nlog < MAX_LOG ? D.nlog++ : MAX_LOG - 1];
	memset(lr, 0, sizeof *lr);
	lr->w = w;
	lr->h = h;
	lr->format = wl_shm_buffer_get_format(shm);
	lr->n = s->ndmg;
	CHECK(!s->dmg_overflow && !s->legacy_damage,
	      "damage the harness cannot follow");
	for (i = 0; i < s->ndmg; ++i) {
		struct rect r = s->dmg[i];

		if (i < LOG_RECTS)
			lr->r[i] = r;
		lr->sum_px += (long long)r.w * r.h;
		CHECK(r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 &&
		      r.x + r.w <= w && r.y + r.h <= h,
		      "damage %d,%d %dx%d outside the %dx%d buffer",
		      r.x, r.y, r.w, r.h, w, h);
		for (y = r.y < 0 ? 0 : r.y; y < r.y + r.h && y < h; ++y)
			for (x = r.x < 0 ? 0 : r.x; x < r.x + r.w && x < w; ++x)
				mask[y * w + x] = 1;
	}
	for (i = 0; i < w * h; ++i)
		lr->union_px += mask[i];
	lr->full = lr->union_px == (long long)w * h;

	resized = !D.img || D.w != w || D.h != h ||
		  D.format != wl_shm_buffer_get_format(shm);
	if (resized) {
		if (!lr->full) {
			fail("a %dx%d buffer after %dx%d came with damage for %lld of %d pixels",
			     w, h, D.w, D.h, lr->union_px, w * h);
			D.no_full++;
		}
		free(D.img);
		D.img = malloc((size_t)stride * h);
		memcpy(D.img, data, (size_t)stride * h);
		D.w = w;
		D.h = h;
		D.format = lr->format;
		D.bpp = bpp;
	} else {
		int bad = 0, fx = -1, fy = -1;

		for (y = 0; y < h; ++y)
			for (x = 0; x < w; ++x)
				if (mask[y * w + x])
					memcpy(D.img + (size_t)y * stride + x * bpp,
					       data + (size_t)y * stride + x * bpp,
					       bpp);
		for (y = 0; y < h; ++y)
			for (x = 0; x < w; ++x)
				if (memcmp(D.img + (size_t)y * stride + x * bpp,
					   data + (size_t)y * stride + x * bpp,
					   bpp)) {
					if (!bad++) {
						fx = x;
						fy = y;
					}
				}
		if (bad) {
			fail("commit %d: %d pixels differ from the buffer outside the damage, the first at %d,%d",
			     D.commits, bad, fx, fy);
			D.stale++;
			memcpy(D.img, data, (size_t)stride * h);
		}
	}
	free(mask);

	if (s->opaque_pending) {
		D.opaque = s->opaque;
		s->opaque_pending = false;
	}
	D.commits++;
#ifdef WITH_C8
	if (D.pal_is_pending) {
		memcpy(D.pal_cur, D.pal_pending, sizeof(D.pal_cur));
		D.pal_is_pending = false;
	}
#endif

	if (D.release == REL_IMMEDIATE) {
		wl_shm_buffer_end_access(shm);
		wl_buffer_send_release(s->buf);
	} else {
		/* hold it, and what it had when we got it, until it is time */
		t->size = (size_t)stride * h;
		t->snap = malloc(t->size);
		memcpy(t->snap, data, t->size);
		t->held = true;
		t->since = now_ms();
		wl_shm_buffer_end_access(shm);
		if (D.release == REL_NEXT)
			for (o = D.bufs; o; o = o->next)
				if (o != t && o->held)
					tbuf_release(o);
	}
	s->ndmg = 0;
	s->buf = NULL;
	poke_wait();
}

static void s_commit(struct wl_client *c, struct wl_resource *r)
{
	struct surf *s = wl_resource_get_user_data(r);

	if (r == C.surface) {
		main_commit(s);
	} else if (s->buf) {
		wl_buffer_send_release(s->buf);
		s->buf = NULL;
	}
	if (s->cb) {
		wl_callback_send_done(s->cb, 0);
		wl_resource_destroy(s->cb);
		s->cb = NULL;
	}
}

static void comp_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface,
						   v, id);
	wl_resource_set_implementation(r, &comp_impl, NULL, NULL);
}

/* xdg-shell */
static void x_nop(struct wl_client *c, struct wl_resource *r) {}
static void x_set_str(struct wl_client *c, struct wl_resource *r,
		      const char *s) {}
static void x_set_res(struct wl_client *c, struct wl_resource *r,
		      struct wl_resource *o) {}
static void x_move(struct wl_client *c, struct wl_resource *r,
		   struct wl_resource *seat, uint32_t serial) {}
static void x_resize(struct wl_client *c, struct wl_resource *r,
		     struct wl_resource *seat, uint32_t serial, uint32_t e) {}
static void x_size(struct wl_client *c, struct wl_resource *r,
		   int32_t w, int32_t h) {}
static void x_menu(struct wl_client *c, struct wl_resource *r,
		   struct wl_resource *seat, uint32_t serial, int32_t x,
		   int32_t y) {}
static void x_fs(struct wl_client *c, struct wl_resource *r,
		 struct wl_resource *o) {}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = x_nop, .set_parent = x_set_res, .set_title = x_set_str,
	.set_app_id = x_set_str, .show_window_menu = x_menu, .move = x_move,
	.resize = x_resize, .set_max_size = x_size, .set_min_size = x_size,
	.set_maximized = x_nop, .unset_maximized = x_nop,
	.set_fullscreen = x_fs, .unset_fullscreen = x_nop,
	.set_minimized = x_nop,
};

static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r,
			    uint32_t id)
{
	struct wl_array states;

	C.toplevel = wl_resource_create(c, &xdg_toplevel_interface,
					wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.toplevel, &toplevel_impl, NULL, NULL);
	wl_array_init(&states);
	xdg_toplevel_send_configure(C.toplevel, 0, 0, &states);
	wl_array_release(&states);
	xdg_surface_send_configure(r, ++C.serial);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r,
			 uint32_t id, struct wl_resource *parent,
			 struct wl_resource *pos) {}
static void xs_geometry(struct wl_client *c, struct wl_resource *r,
			int32_t x, int32_t y, int32_t w, int32_t h) {}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t s) {}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = x_nop, .get_toplevel = xs_get_toplevel,
	.get_popup = xs_get_popup, .set_window_geometry = xs_geometry,
	.ack_configure = xs_ack,
};

static void wm_create_positioner(struct wl_client *c, struct wl_resource *r,
				 uint32_t id) {}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r,
			       uint32_t id, struct wl_resource *surf)
{
	/* the window is the surface that gets a role, not the cursor's */
	C.surface = surf;
	C.xdg_surface = wl_resource_create(c, &xdg_surface_interface,
					   wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.xdg_surface, &xdg_surface_impl, NULL,
				       NULL);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t s) {}

static const struct xdg_wm_base_interface wm_impl = {
	.destroy = x_nop, .create_positioner = wm_create_positioner,
	.get_xdg_surface = wm_get_xdg_surface, .pong = wm_pong,
};
static void wm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface,
						   v, id);
	wl_resource_set_implementation(r, &wm_impl, NULL, NULL);
}

/* wl_seat with a keyboard that has a real keymap, so havoc's key handling
 * runs for real */
static void send_keymap(struct wl_resource *kbd)
{
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, NULL, 0);
	char *s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(s) + 1;
	int fd = memfd_create("keymap", 0);

	if (!km || fd < 0 || write(fd, s, len) != (ssize_t)len) {
		fprintf(stderr, "cannot build a keymap\n");
		exit(2);
	}
	wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
				len);
	close(fd);
	free(s);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
}

static void kbd_release(struct wl_client *c, struct wl_resource *r)
{
	if (C.keyboard == r)
		C.keyboard = NULL;
	wl_resource_destroy(r);
}
static const struct wl_keyboard_interface kbd_impl = { .release = kbd_release };

static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r,
			   uint32_t serial, struct wl_resource *surf,
			   int32_t x, int32_t y) {}
static void ptr_release(struct wl_client *c, struct wl_resource *r)
{
	if (D.pointer == r)
		D.pointer = NULL;
	wl_resource_destroy(r);
}
static const struct wl_pointer_interface ptr_impl = {
	.set_cursor = ptr_set_cursor, .release = ptr_release,
};
static void seat_get_pointer(struct wl_client *c, struct wl_resource *r,
			     uint32_t id)
{
	D.pointer = wl_resource_create(c, &wl_pointer_interface,
				       wl_resource_get_version(r), id);
	wl_resource_set_implementation(D.pointer, &ptr_impl, NULL, NULL);
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r,
			      uint32_t id)
{
	struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(k, &kbd_impl, NULL, NULL);
	send_keymap(k);
	wl_keyboard_send_repeat_info(k, 25, 600);
	C.keyboard = k;
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r,
			   uint32_t id) {}
static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = x_nop,
};
static void seat_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);

	wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_KEYBOARD |
				 (D.driver ? WL_SEAT_CAPABILITY_POINTER : 0));
	wl_seat_send_name(r, "seat0");
}

/* text-input-v3, with the double buffered state of the protocol */
static void ti_destroy(struct wl_client *c, struct wl_resource *r)
{
	if (C.ti == r)
		C.ti = NULL;
	wl_resource_destroy(r);
}
static void ti_enable(struct wl_client *c, struct wl_resource *r)
{
	C.p_enable = true;
	C.p_disable = false;
}
static void ti_disable(struct wl_client *c, struct wl_resource *r)
{
	C.p_disable = true;
	C.p_enable = false;
}
static void ti_surrounding(struct wl_client *c, struct wl_resource *r,
			   const char *t, int32_t cur, int32_t anchor) {}
static void ti_cause(struct wl_client *c, struct wl_resource *r, uint32_t v) {}
static void ti_content(struct wl_client *c, struct wl_resource *r,
		       uint32_t hint, uint32_t purpose)
{
	C.hint = hint;
	C.purpose = purpose;
}
static void ti_rect(struct wl_client *c, struct wl_resource *r,
		    int32_t x, int32_t y, int32_t w, int32_t h)
{
	C.rx = x;
	C.ry = y;
	C.rw = w;
	C.rh = h;
}
static void ti_commit(struct wl_client *c, struct wl_resource *r)
{
	C.commits++;
	if (C.p_enable) {
		C.enables++;
		C.enabled = true;
	}
	if (C.p_disable) {
		C.disables++;
		C.enabled = false;
	}
	C.p_enable = C.p_disable = false;
}
static const struct zwp_text_input_v3_interface ti_impl = {
	.destroy = ti_destroy, .enable = ti_enable, .disable = ti_disable,
	.set_surrounding_text = ti_surrounding,
	.set_text_change_cause = ti_cause, .set_content_type = ti_content,
	.set_cursor_rectangle = ti_rect, .commit = ti_commit,
};
static void tim_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void tim_get(struct wl_client *c, struct wl_resource *r, uint32_t id,
		    struct wl_resource *seat)
{
	C.ti = wl_resource_create(c, &zwp_text_input_v3_interface,
				  wl_resource_get_version(r), id);
	wl_resource_set_implementation(C.ti, &ti_impl, NULL, NULL);
}
static const struct zwp_text_input_manager_v3_interface tim_impl = {
	.destroy = tim_destroy, .get_text_input = tim_get,
};
static void tim_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c,
		&zwp_text_input_manager_v3_interface, v, id);

	wl_resource_set_implementation(r, &tim_impl, NULL, NULL);
}

static void focus(bool in)
{
	struct wl_array keys;

	wl_array_init(&keys);
	if (in) {
		wl_keyboard_send_enter(C.keyboard, ++C.serial, C.surface,
				       &keys);
		if (C.ti)
			zwp_text_input_v3_send_enter(C.ti, C.surface);
	} else {
		if (C.ti)
			zwp_text_input_v3_send_leave(C.ti, C.surface);
		wl_keyboard_send_leave(C.keyboard, ++C.serial, C.surface);
	}
	wl_array_release(&keys);
}

static void type(uint32_t key)
{
	wl_keyboard_send_key(C.keyboard, ++C.serial, 0, key,
			     WL_KEYBOARD_KEY_STATE_PRESSED);
	wl_keyboard_send_key(C.keyboard, ++C.serial, 0, key,
			     WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* viewporter and fractional scale, enough for havoc to render at another
 * scale: the scale is whatever the driver says and the viewport is recorded */
static void vp_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void vp_source(struct wl_client *c, struct wl_resource *r,
		      wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {}
static void vp_dest(struct wl_client *c, struct wl_resource *r, int32_t w,
		    int32_t h)
{
	D.vp_w = w;
	D.vp_h = h;
}
static const struct wp_viewport_interface vp_impl = {
	.destroy = vp_destroy, .set_source = vp_source,
	.set_destination = vp_dest,
};
static void vpm_get(struct wl_client *c, struct wl_resource *r, uint32_t id,
		    struct wl_resource *surf)
{
	struct wl_resource *v = wl_resource_create(c, &wp_viewport_interface,
						   wl_resource_get_version(r),
						   id);

	wl_resource_set_implementation(v, &vp_impl, NULL, NULL);
}
static const struct wp_viewporter_interface vpm_impl = {
	.destroy = vp_destroy, .get_viewport = vpm_get,
};
static void vpm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wp_viewporter_interface,
						   v, id);

	wl_resource_set_implementation(r, &vpm_impl, NULL, NULL);
}

static const struct wp_fractional_scale_v1_interface fs_impl = {
	.destroy = vp_destroy,
};
static void fsm_get(struct wl_client *c, struct wl_resource *r, uint32_t id,
		    struct wl_resource *surf)
{
	D.fs = wl_resource_create(c, &wp_fractional_scale_v1_interface,
				  wl_resource_get_version(r), id);
	wl_resource_set_implementation(D.fs, &fs_impl, NULL, NULL);
}
static const struct wp_fractional_scale_manager_v1_interface fsm_impl = {
	.destroy = vp_destroy, .get_fractional_scale = fsm_get,
};
static void fsm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c,
		&wp_fractional_scale_manager_v1_interface, v, id);

	wl_resource_set_implementation(r, &fsm_impl, NULL, NULL);
}

/* the commands of --driver, one per line on stdin, answered on stdout:
 *
 *   focus                      keyboard (and text input) focus into the window
 *   feed HEX                   bytes the terminal gets as program output
 *   key CODE                   press and release an evdev key
 *   mods N                     modifiers held from now on (xkb mask: 1 shift,
 *                              4 control)
 *   ptr enter|motion X Y       pointer at surface coordinates
 *   ptr button 0|1             left button released or pressed
 *   ptr axis N                 scroll by N / 3 lines (negative is up)
 *   configure W H              xdg_toplevel configure with this size
 *   scale N                    preferred fractional scale N / 120
 *   wait                       answered once nothing happened for a while, with
 *                              a line per commit since the last wait
 *   snap PATH                  the compositor's copy of the window to PATH
 *   stats                      totals, the checks' counters and the opaque region
 *   quit
 */
static int ctl_fd = -1;

static void reply(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static int wait_done(void *data)
{
	int i, j;

	for (i = 0; i < D.nlog; ++i) {
		struct commit_rec *l = &D.log[i];

		printf("C %d %d %u %d %lld %lld %d", l->w, l->h, l->format,
		       l->n, l->union_px, l->sum_px, l->full);
		for (j = 0; j < l->n && j < LOG_RECTS; ++j)
			printf(" %d,%d,%d,%d", l->r[j].x, l->r[j].y, l->r[j].w,
			       l->r[j].h);
		printf("\n");
	}
	D.nlog = 0;
	D.wait_pending = false;
	reply("END");
	return 0;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

static void driver_line(char *l)
{
	char *arg = strchr(l, ' ');
	int a, b;
	uint8_t buf[8192];

	if (arg)
		*arg++ = 0;
	else
		arg = "";

	if (!strcmp(l, "focus")) {
		focus(true);
		reply("OK");
	} else if (!strcmp(l, "feed")) {
		size_t n = 0;

		for (; arg[0] && arg[1] && n < sizeof buf; arg += 2)
			buf[n++] = hexval(arg[0]) << 4 | hexval(arg[1]);
		if (write(ctl_fd, buf, n) != (ssize_t)n)
			fail("cannot feed the terminal");
		reply("OK");
	} else if (!strcmp(l, "key")) {
		type(atoi(arg));
		reply("OK");
	} else if (!strcmp(l, "mods")) {
		wl_keyboard_send_modifiers(C.keyboard, ++C.serial, atoi(arg),
					   0, 0, 0);
		reply("OK");
	} else if (!strcmp(l, "ptr")) {
		char *w = strsep(&arg, " ");

		if (!D.pointer) {
			fail("no pointer");
		} else if (sscanf(arg ? arg : "", "%d %d", &a, &b) == 2 &&
			   !strcmp(w, "enter")) {
			wl_pointer_send_enter(D.pointer, ++C.serial, C.surface,
					      wl_fixed_from_int(a),
					      wl_fixed_from_int(b));
		} else if (sscanf(arg ? arg : "", "%d %d", &a, &b) == 2 &&
			   !strcmp(w, "motion")) {
			wl_pointer_send_motion(D.pointer, 0,
					       wl_fixed_from_int(a),
					       wl_fixed_from_int(b));
		} else if (!strcmp(w, "axis")) {
			wl_pointer_send_axis(D.pointer, 0,
					     WL_POINTER_AXIS_VERTICAL_SCROLL,
					     wl_fixed_from_int(atoi(arg)));
		} else if (!strcmp(w, "button")) {
			wl_pointer_send_button(D.pointer, ++C.serial, 0, 0x110,
					       atoi(arg)
					       ? WL_POINTER_BUTTON_STATE_PRESSED
					       : WL_POINTER_BUTTON_STATE_RELEASED);
		}
		reply("OK");
	} else if (!strcmp(l, "configure")) {
		struct wl_array states;

		wl_array_init(&states);
		sscanf(arg, "%d %d", &a, &b);
		xdg_toplevel_send_configure(C.toplevel, a, b, &states);
		xdg_surface_send_configure(C.xdg_surface, ++C.serial);
		wl_array_release(&states);
		reply("OK");
	} else if (!strcmp(l, "scale")) {
		if (D.fs)
			wp_fractional_scale_v1_send_preferred_scale(D.fs,
								    atoi(arg));
		else
			fail("no fractional scale");
		reply("OK");
	} else if (!strcmp(l, "wait")) {
		D.wait_pending = true;
		wl_event_source_timer_update(D.wait_timer, D.quiet_ms);
	} else if (!strcmp(l, "snap")) {
		FILE *f = fopen(arg, "w");

		if (f && D.img && D.bpp == 1) {
			/* shown through the palette of the last commit; the
			 * same bytes as the XRGB8888 window would hold */
			int stride = (D.w + 3) & ~3, x, y;

			for (y = 0; y < D.h; ++y)
				for (x = 0; x < D.w; ++x) {
					uint32_t p = 0xff000000 |
						D.pal_cur[D.img[y * stride + x]];

					fwrite(&p, 4, 1, f);
				}
			fclose(f);
			reply("SNAP %d %d", D.w, D.h);
		} else if (f && D.img) {
			fwrite(D.img, 4, (size_t)D.w * D.h, f);
			fclose(f);
			reply("SNAP %d %d", D.w, D.h);
		} else {
			reply("SNAP 0 0");
		}
#ifdef WITH_C8
	} else if (!strcmp(l, "presentation")) {
		D.c8_mode = atoi(arg);
		if (D.pal_res)
			picowl_surface_palette_v1_send_presentation(D.pal_res,
								     D.c8_mode);
		else
			fail("no surface palette");
		reply("OK");
	} else if (!strcmp(l, "palette")) {
		/* the palette in force: index=RRGGBB for the indices asked */
		char out[512] = "";
		int idx, off = 0, n;

		while (arg && sscanf(arg, "%d%n", &idx, &n) == 1 &&
		       idx >= 0 && idx < 256) {
			off += snprintf(out + off, sizeof(out) - off, " %d=%06x",
					idx, D.pal_cur[idx] & 0xffffff);
			arg += n;
		}
		reply("PAL%s", out);
#endif
	} else if (!strcmp(l, "stats")) {
		struct opaque *o = &D.opaque;

		reply("S commits=%d stale=%d held_writes=%d no_full=%d failed=%d w=%d h=%d format=%u vp=%dx%d opaque=%d,%d:%d,%d,%d,%d palsets=%d",
		      D.commits, D.stale, D.held_writes, D.no_full, C.failed,
		      D.w, D.h, D.format, D.vp_w, D.vp_h, o->set, o->n,
		      o->n ? o->r[0].x : 0, o->n ? o->r[0].y : 0,
		      o->n ? o->r[0].w : 0, o->n ? o->r[0].h : 0, D.pal_sets);
		reply("END");
	} else if (!strcmp(l, "quit")) {
		kill(C.child, SIGTERM);
		waitpid(C.child, NULL, 0);
		exit(C.failed);
	}
}

static int stdin_ready(int fd, uint32_t mask, void *data)
{
	static char line[16384];
	static size_t n;
	ssize_t r = read(fd, line + n, sizeof line - n - 1);
	char *nl;

	if (r <= 0) {
		kill(C.child, SIGTERM);
		exit(C.failed);
	}
	n += r;
	while ((nl = memchr(line, '\n', n))) {
		*nl = 0;
		driver_line(line);
		n -= nl + 1 - line;
		memmove(line, nl + 1, n);
	}
	return 0;
}

static void driver_start(const char *havoc, const char *cfg, const char *dir)
{
	char path[512];

	snprintf(path, sizeof path, "%s/ctl", dir);
	unlink(path);
	if (mkfifo(path, 0600) < 0 ||
	    (ctl_fd = open(path, O_RDWR)) < 0) {
		perror("fifo");
		exit(2);
	}
	setenv("HAVOC_CTL", path, 1);
	/* the terminal only echoes what is typed, everything else is what the
	 * driver feeds through the fifo */
	C.child = fork();
	if (C.child == 0) {
		execl(havoc, havoc, "-c", cfg, "/bin/sh", "-c",
		      D.child_cmd ? D.child_cmd : "exec cat \"$HAVOC_CTL\"",
		      (char *)NULL);
		_exit(127);
	}
	D.wait_timer = wl_event_loop_add_timer(C.loop, wait_done, NULL);
	D.release_timer = wl_event_loop_add_timer(C.loop, release_tick, NULL);
	wl_event_source_timer_update(D.release_timer, 10);
	wl_event_loop_add_fd(C.loop, 0, WL_EVENT_READABLE, stdin_ready, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, SIG_DFL);
	alarm(600);
}

static void finish(void)
{
	char buf[64] = "";
	FILE *f = fopen(C.outfile, "r");
	int status;

	if (f) {
		if (!fgets(buf, sizeof buf, f))
			buf[0] = 0;
		fclose(f);
	}
	CHECK(!strcmp(buf, "abc"), "the shell read '%s' instead of 'abc'",
	      buf);
	if (waitpid(C.child, &status, WNOHANG) != 0)
		fail("havoc exited early");
	kill(C.child, SIGTERM);
	waitpid(C.child, &status, 0);
	printf("%s: %d enables, %d disables, %d commits\n",
	       C.failed ? "FAILED" : "PASSED", C.enables, C.disables,
	       C.commits);
	exit(C.failed);
}

static int tick(void *data)
{
	wl_event_source_timer_update(C.timer, 150);

	switch (C.stage) {
	case 0:
		if (!C.mapped || !C.keyboard || (C.with_ti && !C.ti)) {
			if (++C.waited > 40) {
				fail("havoc never got ready: mapped %d, keyboard %d, text input %d",
				     C.mapped, !!C.keyboard, !!C.ti);
				finish();
			}
			return 0;
		}
		CHECK(C.commits == 0 && !C.enabled,
		      "text input touched before the focus arrived");
		focus(true);
		break;
	case 1:
		if (C.with_ti) {
			CHECK(C.enables == 1 && C.enabled && C.commits == 1,
			      "after enter: %d enables, %d commits, enabled %d",
			      C.enables, C.commits, C.enabled);
			CHECK(C.purpose == PURPOSE_TERMINAL,
			      "purpose %u is not terminal", C.purpose);
			CHECK(C.rw > 0 && C.rh > 0 && C.rx >= 0 && C.ry >= 0,
			      "bad cursor rectangle %d,%d %dx%d", C.rx, C.ry,
			      C.rw, C.rh);
		}
		C.enables_at_typing = C.enables;
		C.commits_at_typing = C.commits;
		type(30);
		type(48);
		type(46);
		type(28);
		break;
	case 2:
		CHECK(C.enables == C.enables_at_typing &&
		      C.commits == C.commits_at_typing,
		      "typing changed the text input state");
		focus(false);
		break;
	case 3:
		if (C.with_ti)
			CHECK(!C.enabled && C.disables == 1 && C.commits == 2,
			      "after leave: %d disables, %d commits, enabled %d",
			      C.disables, C.commits, C.enabled);
		focus(true);
		break;
	case 4:
		if (C.with_ti)
			CHECK(C.enabled && C.enables == 2 && C.commits == 3,
			      "after re-enter: %d enables, %d commits",
			      C.enables, C.commits);
		finish();
	}
	C.stage++;
	return 0;
}

#ifdef WITH_C8
/* picowl-palette-v1 (section 2.1 of the C8 design): a palette per surface,
 * applied at commit, and the presentation event */
static void sp_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void sp_set_palette(struct wl_client *c, struct wl_resource *r,
			   struct wl_array *a)
{
	if (a->size == 0 || a->size > 256 * 4 || a->size % 4) {
		wl_resource_post_error(r, PICOWL_SURFACE_PALETTE_V1_ERROR_INVALID_SIZE,
				       "palette of %zu bytes", a->size);
		return;
	}
	memset(D.pal_pending, 0, sizeof(D.pal_pending));
	memcpy(D.pal_pending, a->data, a->size);
	D.pal_is_pending = true;
	D.pal_sets++;
}
static const struct picowl_surface_palette_v1_interface sp_impl = {
	.destroy = sp_destroy, .set_palette = sp_set_palette,
};
static void sp_gone(struct wl_resource *r)
{
	D.pal_res = NULL;
}
static void pm_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}
static void pm_get_surface_palette(struct wl_client *c, struct wl_resource *r,
				   uint32_t id, struct wl_resource *surf)
{
	struct wl_resource *p;

	if (D.pal_res) {
		wl_resource_post_error(r, PICOWL_PALETTE_MANAGER_V1_ERROR_ALREADY_EXISTS,
				       "the surface has a palette");
		return;
	}
	p = wl_resource_create(c, &picowl_surface_palette_v1_interface,
			       wl_resource_get_version(r), id);
	wl_resource_set_implementation(p, &sp_impl, NULL, sp_gone);
	D.pal_res = p;
	picowl_surface_palette_v1_send_presentation(p, D.c8_mode);
}
static const struct picowl_palette_manager_v1_interface pm_impl = {
	.destroy = pm_destroy, .get_surface_palette = pm_get_surface_palette,
};
static void pm_bind(struct wl_client *c, void *d, uint32_t v, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c,
		&picowl_palette_manager_v1_interface, v, id);
	struct wl_array a;
	uint32_t pal[256] = { 0 };

	wl_resource_set_implementation(r, &pm_impl, NULL, NULL);
	a.size = a.alloc = sizeof(pal);
	a.data = pal;
	picowl_palette_manager_v1_send_converter_palette(r, &a);
}
#endif

int main(int argc, char *argv[])
{
	const char *sock;
	int i = 1;

	bool fractional = false;

	C.with_ti = true;
	D.quiet_ms = 150;
	D.bpp = 4;
	D.c8_mode = -1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--no-text-input")) {
			C.with_ti = false;
		} else if (!strcmp(argv[i], "--driver")) {
			D.driver = true;
		} else if (!strcmp(argv[i], "--c8") && i + 1 < argc) {
			const char *m = argv[++i];

			D.c8_mode = !strcmp(m, "exclusive") ? 1 :
				    !strcmp(m, "composited") ? 0 : -1;
		} else if (!strcmp(argv[i], "--child") && i + 1 < argc) {
			D.child_cmd = argv[++i];
		} else if (!strcmp(argv[i], "--fractional")) {
			fractional = true;
		} else if (!strcmp(argv[i], "--quiet") && i + 1 < argc) {
			D.quiet_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--release") && i + 1 < argc) {
			const char *m = argv[++i];

			D.release = !strcmp(m, "immediate") ? REL_IMMEDIATE :
				    !strcmp(m, "next") ? REL_NEXT :
				    !strcmp(m, "timer") ? REL_TIMER :
				    !strcmp(m, "never") ? REL_NEVER : -1;
			if (D.release < 0) {
				fprintf(stderr, "unknown release policy %s\n", m);
				return 2;
			}
		} else {
			fprintf(stderr, "unknown option %s\n", argv[i]);
			return 2;
		}
	}
	if (argc - i != 2) {
		fprintf(stderr, "usage: %s [--no-text-input] HAVOC OUTFILE\n"
			"       %s --driver [--release immediate|next|timer|never]"
			" [--fractional] [--quiet MS] [--child CMD] HAVOC CONFIG\n",
			argv[0], argv[0]);
		return 2;
	}
	C.outfile = argv[i + 1];
	if (!D.driver)
		unlink(C.outfile);

	C.display = wl_display_create();
	C.loop = wl_display_get_event_loop(C.display);
	sock = wl_display_add_socket_auto(C.display);
	wl_display_init_shm(C.display);
#ifdef WITH_C8
	if (D.c8_mode >= 0) {
		wl_display_add_shm_format(C.display, FORMAT_C8);
		wl_global_create(C.display, &picowl_palette_manager_v1_interface,
				 1, NULL, pm_bind);
	}
#endif
	wl_global_create(C.display, &wl_compositor_interface, 4, NULL,
			 comp_bind);
	wl_global_create(C.display, &xdg_wm_base_interface, 1, NULL, wm_bind);
	wl_global_create(C.display, &wl_seat_interface, 5, NULL, seat_bind);
	if (C.with_ti)
		wl_global_create(C.display,
				 &zwp_text_input_manager_v3_interface, 1, NULL,
				 tim_bind);

	if (fractional) {
		wl_global_create(C.display, &wp_viewporter_interface, 1, NULL,
				 vpm_bind);
		wl_global_create(C.display,
				 &wp_fractional_scale_manager_v1_interface, 1,
				 NULL, fsm_bind);
	}

	setenv("WAYLAND_DISPLAY", sock, 1);
	if (D.driver) {
		driver_start(argv[i], C.outfile, getenv("XDG_RUNTIME_DIR"));
		wl_display_run(C.display);
		return 1;
	}

	C.child = fork();
	if (C.child == 0) {
		setenv("HAVOC_OUT", C.outfile, 1);
		/* -c "" skips the user's configuration */
		execl(argv[i], argv[i], "-c", "", "/bin/sh", "-c",
		      "IFS= read -r l; printf %s \"$l\" > \"$HAVOC_OUT\"; "
		      "sleep 30", (char *)NULL);
		_exit(127);
	}

	C.timer = wl_event_loop_add_timer(C.loop, tick, NULL);
	wl_event_source_timer_update(C.timer, 150);
	signal(SIGALRM, SIG_DFL);
	alarm(30);
	wl_display_run(C.display);
	return 1;
}
