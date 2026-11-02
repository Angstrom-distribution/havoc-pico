#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include "ptyutil.h"

#include <xkbcommon/xkbcommon-compose.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-cursor.h>

#include "tsm/libtsm.h"
#include "xdg-shell.h"
#include "primary-selection-unstable-v1.h"
#include "xdg-decoration-unstable-v1.h"
#include "fractional-scale-v1.h"
#include "viewporter.h"
#include "text-input-unstable-v3.h"
#include "emoji.h"
#include "pixfont.h"
#include "c8.h"
#include "graphics.h"
#ifdef HAVE_PICOWL_PALETTE
#include "picowl-palette-v1.h"
#else
struct picowl_palette_manager_v1;
struct picowl_surface_palette_v1;
#endif

#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

int font_init(char *);
void font_scale(int, int *, int *);
void font_deinit(void);
unsigned char *get_glyph(uint32_t, uint32_t, int);

enum deco {
	DECO_AUTO,
	DECO_SERVER,
	DECO_NONE
};

enum evfd {
	EV_DISPLAY,
	EV_TTY,
	EV_PASTE,
	NUM_POLLFDS,
};

/* Everything that decides which pixels a cell gets, with the colours as
 * paint_cell uses them (inverse applied) and no padding bytes, so that two
 * cells paint the same pixels exactly when their keys are memcmp equal.
 * libtsm's own ages cannot say that: it stamps the whole screen on every
 * erase, scroll, selection and colour change. */
struct cell_key {
	uint32_t id;
	uint32_t fg, bg;
	uint8_t width;
	/* drawn from an emoji pack: whether it is depends on the pack and the
	 * cell size, not only on the character */
	uint8_t picture;
#if HAVOC_SIXEL
	/* only on a cell with a sixel tile: KF_*, and which picture it is
	 * (a slot is reused for another one) */
	uint8_t kflags;
	uint8_t pad;
	uint32_t pic, pgen;
#else
	uint8_t pad[2];
#endif
};

#if HAVOC_SIXEL
#define KF_INV 0x01	/* the tile is drawn inverted (cursor, selection) */
_Static_assert(sizeof(struct cell_key) == 24, "cell_key has padding");
#elif !HAVE_GRAPHICS
_Static_assert(sizeof(struct cell_key) == 16, "cell_key has padding");
#endif

enum {
	CELL_PAINT = 1,
	CELL_DAMAGE = 2,
};

static struct {
	bool die;
	bool configured;
	bool need_redraw;
	bool can_redraw;

	int master_fd;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *cp;
	struct wl_shm *shm;
	bool shm_argb;
	bool shm_c8;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct zwp_text_input_manager_v3 *tim;
	struct zwp_text_input_v3 *ti;

	struct wl_surface *surf;
	struct xdg_surface *xdgsurf;
	struct xdg_toplevel *toplvl;
	struct wp_viewporter *vpm;
	struct wp_viewport *vp;
	struct wp_fractional_scale_manager_v1 *fsm;
	struct wp_fractional_scale_v1 *fs;

	struct buffer {
		struct wl_buffer *b;
		void *data;
		int size;
		/* 8 bit indices (C8) instead of 32 bit pixels, and the bytes
		 * from one row to the next */
		bool c8;
		int stride;
		int w, h, cols, rows;
		bool busy;
		/* whether shadow holds what is painted in data */
		bool valid;
		struct cell_key *shadow;
		uint32_t margin;
	} buf[2];
	struct wl_callback *cb;

	/* C8 windows (patch 0005): the palette protocol, whether the
	 * compositor shows the window alone on a C8 output, whether the
	 * buffers are C8 now, and the colours in them. */
	struct picowl_palette_manager_v1 *palm;
	struct picowl_surface_palette_v1 *pal;
	bool exclusive;
	bool c8_on;
	struct c8_table colours;

	/* The cells as of the redraw being made, and as of the last committed
	 * frame (what the compositor shows), sized col * row. */
	struct cell_key *cur, *shown;
	uint32_t *cur_ch;
	uint8_t *flags;
	bool shown_valid;
	uint32_t shown_margin;
	bool full_redraw;

	int col, row;
	int cwidth, cheight;
	int width, height, scale;
	int pendingwidth, pendingheight;
	int confwidth, confheight;
	struct {
		int top, left;
	} margin;

	struct tsm_screen *screen;
	struct tsm_vte *vte;
#if HAVE_GRAPHICS
	struct gfx *gfx;
	/* the pixel size last given to the pty */
	unsigned int ws_xpixel, ws_ypixel;
#endif
	unsigned int mods;

	struct xkb_context *xkb_ctx;
	struct xkb_state *xkb_state;
	struct xkb_keymap *xkb_keymap;
	struct xkb_compose_table *xkb_compose_table;
	struct xkb_compose_state *xkb_compose_state;
	xkb_mod_index_t xkb_alt;
	xkb_mod_index_t xkb_ctrl;
	xkb_mod_index_t xkb_shift;

	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	wl_fixed_t ptr_x, ptr_y;

	enum {
		SS_RESET,
		SS_ANCHORED,
		SS_DRAGGING,
		SS_ACTIVE
	} selection;

	struct {
		int count;
		int x, y;
	} click;

	struct {
		struct wl_cursor_theme *theme;
		struct wl_cursor *text;
		struct wl_cursor *current;
		long long anim_start;
		struct wl_surface *surface;
		struct wl_callback *callback;
		uint32_t enter_serial;
		struct wp_viewport *viewport;
	} cursor;

	struct {
		int delay, interval;
		int timeout;
		long long start;
		uint32_t key;
		xkb_keysym_t sym;
		uint32_t unicode;
		void (*action)(void);
	} repeat;

	struct wl_data_device_manager *d_dm;
	struct wl_data_device *d_d;
	struct zwp_primary_selection_device_manager_v1 *ps_dm;
	struct zwp_primary_selection_device_v1 *ps_d;

	struct {
		struct wl_data_source *source;
		char *data;
	} d_copy;

	struct {
		struct zwp_primary_selection_source_v1 *source;
		char *data;
	} ps_copy;

	struct {
		struct wl_data_offer *d_offer;
		struct zwp_primary_selection_offer_v1 *ps_offer;
		char *d_mime;
		char *ps_mime;

		int fd[2];
		char buf[200];
		size_t len;
		bool active;
	} paste;

	struct {
		struct zxdg_decoration_manager_v1 *manager;
		struct zxdg_toplevel_decoration_v1 *deco;
	} deco;

	struct {
		bool linger;
		char *config;
		char *display;
		char *app_id;
	} opt;

	struct binding {
		struct binding *next;
		unsigned int mods;
		xkb_keysym_t sym;
		void (*action)(void);
	} *binding;

	struct {
		char shell[32];
		int col, row;
		int scrollback;
		bool scroll_to_bottom_on_input;
		bool margin;
		int padding;
		unsigned char opacity;
		bool c8;
		enum deco decorations;
		int font_size;
		char font_path[512];
		char font_pixel[16];
#if HAVE_GRAPHICS
		struct gfx_config gfx;
#endif
	} cfg;
} term = {
	.scale = 120,

	.cfg.shell = "/bin/sh",
	.cfg.col = 80,
	.cfg.row = 24,
	.cfg.scrollback = 0,
	.cfg.scroll_to_bottom_on_input = false,
	.cfg.margin = false,
	.cfg.padding = 0,
	.cfg.opacity = 0xff,
	.cfg.decorations = DECO_AUTO,
	.cfg.font_size = 18,
	.cfg.font_path = "",
	.cfg.font_pixel = "",
#if HAVE_GRAPHICS
	.cfg.gfx.max_pixels = 524288,
#endif
	.opt.app_id = "havoc"
};

#define error(s) { fprintf(stderr, s ": %s\n", strerror(errno)); }

static void wcb(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	assert(len <= PIPE_BUF);
	if (term.master_fd >= 0 && write(term.master_fd, u8, len) < 0)
		error("could not write to pty master");
}

static void handle_display(int ev)
{
	if (ev & POLLHUP) {
		term.die = true;
	} else if (ev & POLLIN) {
		if (wl_display_dispatch(term.display) < 0) {
			error("could not dispatch events");
			exit(EXIT_FAILURE);
		}
	}
}

static void handle_tty(int ev)
{
	int len = 0;

	if (ev & POLLIN) {
		/* a picture is many bytes; fewer reads, fewer wake-ups */
#if HAVE_GRAPHICS
		char data[4096];
#else
		char data[256];
#endif

		term.need_redraw = true;
		len = read(term.master_fd, data, sizeof(data));

		if (len < 0) {
			error("could not read from pty");
		} else {
			tsm_vte_input(term.vte, data, len);
		}
	}

	if (ev & POLLHUP && len == 0) {
		close(term.master_fd);
		term.master_fd = -1;
		if (!term.opt.linger)
			term.die = true;
	}
}

static long long now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void handle_repeat(void)
{
	int diff;

	if (term.repeat.timeout < 0)
		return;

	diff = now() - term.repeat.start;
	term.repeat.start += diff;

	if (diff >= term.repeat.timeout) {
		term.repeat.timeout += term.repeat.interval - diff;
		if (term.repeat.timeout < 0)
			term.repeat.timeout = 0;

		if (term.repeat.action) {
			term.repeat.action();
			return;
		}

		tsm_vte_handle_keyboard(term.vte, term.repeat.sym,
					XKB_KEY_NoSymbol, term.mods,
					term.repeat.unicode);
	} else {
		term.repeat.timeout -= diff;
	}
}

static inline int destination(int bufsize)
{
	/* This does not work for all scales and buffer sizes because
	 * wayland fractional scaling is currently broken unfortunately. */
	return ((bufsize * 120) + (term.scale / 2)) / term.scale;
}

static void cursor_draw(int frame)
{
	struct wl_buffer *buffer;
	struct wl_cursor_image *image;

	if ((int)term.cursor.current->image_count <= frame) {
		fprintf(stderr, "cursor frame index out of range\n");
		return;
	}

	image = term.cursor.current->images[frame];
	buffer = wl_cursor_image_get_buffer(image);
	if (term.cursor.viewport)
		wp_viewport_set_destination(term.cursor.viewport,
					    destination(image->width),
					    destination(image->height));
	wl_surface_attach(term.cursor.surface, buffer, 0, 0);
	wl_surface_damage_buffer(term.cursor.surface, 0, 0,
				 image->width, image->height);
	wl_surface_commit(term.cursor.surface);
	wl_pointer_set_cursor(term.ptr, term.cursor.enter_serial,
			      term.cursor.surface,
			      destination(image->hotspot_x),
			      destination(image->hotspot_y));
}

static void cursor_request_frame_callback(void);

static void cursor_frame_callback(void *data, struct wl_callback *cb,
				  uint32_t time)
{
	int frame = wl_cursor_frame(term.cursor.current,
				    now() - term.cursor.anim_start);

	assert(cb == term.cursor.callback);
	wl_callback_destroy(term.cursor.callback);
	cursor_request_frame_callback();
	cursor_draw(frame);
}

static const struct wl_callback_listener cursor_frame_listener = {
	.done = cursor_frame_callback,
};

static void cursor_request_frame_callback(void)
{
	term.cursor.callback = wl_surface_frame(term.cursor.surface);
	wl_callback_add_listener(term.cursor.callback, &cursor_frame_listener,
				 NULL);
}

static void cursor_unset(void)
{
	if (term.cursor.callback) {
		wl_callback_destroy(term.cursor.callback);
		term.cursor.callback = NULL;
	}
	term.cursor.current = NULL;
}

static void cursor_set(struct wl_cursor *cursor)
{
	uint32_t duration;
	int frame;

	cursor_unset();

	if (term.ptr == NULL)
		return;

	if (cursor == NULL)
		goto hide;

	term.cursor.current = cursor;

	frame = wl_cursor_frame_and_duration(term.cursor.current, 0, &duration);
	if (duration) {
		term.cursor.anim_start = now();
		cursor_request_frame_callback();
	}
	cursor_draw(frame);

	return;
hide:
	wl_pointer_set_cursor(term.ptr, term.cursor.enter_serial, NULL, 0, 0);
}

static void cursor_load(void)
{
	int size = 32;
	char *size_str = getenv("XCURSOR_SIZE");
	struct wl_cursor *text = NULL;

	if (term.cursor.surface == NULL)
		return;

	if (size_str && *size_str) {
		char *end;
		long s;

		errno = 0;
		s = strtol(size_str, &end, 10);
		if (errno == 0 && *end == '\0' && s > 0)
			size = s;
	}

	if (term.cursor.theme)
		wl_cursor_theme_destroy(term.cursor.theme);

	size = (size * term.scale + 60) / 120;
	term.cursor.theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size,
						 term.shm);
	if (term.cursor.theme == NULL)
		return;

	text = wl_cursor_theme_get_cursor(term.cursor.theme, "text");
	if (text == NULL)
		text = wl_cursor_theme_get_cursor(term.cursor.theme, "ibeam");
	if (text == NULL)
		text = wl_cursor_theme_get_cursor(term.cursor.theme, "xterm");

	term.cursor.text = text;
}

static void cursor_init(void)
{
	term.cursor.surface = wl_compositor_create_surface(term.cp);
	if (term.cursor.surface && term.vpm && term.fsm)
		term.cursor.viewport = wp_viewporter_get_viewport(
			term.vpm, term.cursor.surface);
}

static void cursor_free(void)
{
	if (term.cursor.callback)
		wl_callback_destroy(term.cursor.callback);
	if (term.cursor.surface)
		wl_surface_destroy(term.cursor.surface);
	if (term.cursor.theme)
		wl_cursor_theme_destroy(term.cursor.theme);
}

#define REPLACEMENT_CHAR 0x0000fffd

static int utf8_to_utf32(char const **utf8, size_t *len, uint32_t *r)
{
	static unsigned char const tail_len[128] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0
	};
	unsigned char tail, c = *(*utf8)++;

	--(*len);
	if (c < 0x80) {
		*r = c;
		return 0;
	}

	tail = tail_len[c - 0x80];

	if (!tail) {
		/* middle of a character or tail too long */
		*r = REPLACEMENT_CHAR;
		return 0;
	} else if (tail > *len) {
		/* need more input for a complete character */
		++(*len), --(*utf8);
		return 1;
	}

	/* remove length specification bits */
	*r = c & 0x3f >> tail;

	while (tail--) {
		c = *(*utf8)++;
		--(*len);
		if ((c & 0xc0) != 0x80) {
			*r = REPLACEMENT_CHAR;
			++(*len), --(*utf8);
			return 0;
		}
		*r = (*r << 6) + (c & 0x3f);
	}
	return 0;
}

static void end_paste(void)
{
	tsm_vte_paste_end(term.vte);
	close(term.paste.fd[0]);
	term.paste.fd[0] = -1;
	term.paste.len = 0;
	term.paste.active = false;
}

static void handle_paste(int ev)
{
	if (ev & POLLIN) {
		uint32_t code;
		ssize_t len;
		char const *p = &term.paste.buf[0];

		len = read(term.paste.fd[0],
			   term.paste.buf + term.paste.len,
			   sizeof term.paste.buf - term.paste.len);

		if (len <= 0) {
			end_paste();
			return;
		}

		if (term.cfg.scroll_to_bottom_on_input)
			tsm_screen_sb_reset(term.screen);

		term.need_redraw = true;
		term.paste.len += len;
		while (term.paste.len > 0) {
			if (utf8_to_utf32(&p, &term.paste.len, &code)) {
				memcpy(&term.paste.buf, p, term.paste.len);
				break;
			}
			tsm_vte_handle_keyboard(term.vte, XKB_KEY_NoSymbol,
						XKB_KEY_NoSymbol, 0, code);
		}
	} else if (ev & POLLHUP) {
		end_paste();
	}
}

static void (*pcb[NUM_POLLFDS])(int) = {
	[EV_DISPLAY] = handle_display,
	[EV_TTY] = handle_tty,
	[EV_PASTE] = handle_paste,
};

static void buffer_release(void *data, struct wl_buffer *b)
{
	struct buffer *buffer = data;

	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static int buffer_init(struct buffer *buf)
{
	struct wl_shm_pool *pool;
	char shm_name[14];
	int fd, stride;
	int max = 100;

	assert(!buf->busy);

	buf->c8 = term.c8_on;
	/* a row of indices is a multiple of 4 bytes, like a row of pixels */
	stride = buf->c8 ? (term.width + 3) & ~3 : term.width * 4;
	buf->stride = stride;
	buf->size = stride * term.height;

	srand(time(NULL));
	do {
		sprintf(shm_name, "/havoc-%d", rand() % 1000000);
		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	} while (fd < 0 && errno == EEXIST && --max);

	if (fd < 0) {
		error("shm_open failed");
		return -1;
	}
	shm_unlink(shm_name);

	if (ftruncate(fd, buf->size) < 0) {
		error("ftruncate failed");
		close(fd);
		return -1;
	}

	buf->data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, 0);

	if (buf->data == MAP_FAILED) {
		error("mmap failed");
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(term.shm, fd, buf->size);
	/* Nothing is blended onto an opaque window, so say so */
	buf->b = wl_shm_pool_create_buffer(pool, 0, term.width, term.height,
					   stride,
					   buf->c8 ? WL_SHM_FORMAT_C8 :
					   term.cfg.opacity == 255
					   ? WL_SHM_FORMAT_XRGB8888
					   : WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buf->b, &buffer_listener, buf);
	wl_shm_pool_destroy(pool);
	close(fd);

	buf->w = term.width;
	buf->h = term.height;
	buf->valid = false;

	return 0;
}

static void buffer_unmap(struct buffer *buf)
{
	if (buf->b) {
		wl_buffer_destroy(buf->b);
		munmap(buf->data, buf->size);
	}
	free(buf->shadow);
	buf->b = NULL;
	buf->data = NULL;
	buf->shadow = NULL;
	buf->cols = buf->rows = 0;
	buf->valid = false;
}

static struct buffer *swap_buffers(void)
{
	struct buffer *buf;

	assert(term.configured);

	if (!term.buf[0].busy) {
		buf = &term.buf[0];
	} else if (!term.buf[1].busy) {
		buf = &term.buf[1];
	} else {
		fprintf(stderr, "both surface content buffers are busy\n");
		return NULL;
	}

	/* Each buffer is checked on its own: a buffer that was busy while the
	 * window was resized keeps its old size, and one that was never used
	 * has no memory yet. */
	if (!buf->b || buf->w != term.width || buf->h != term.height ||
	    buf->c8 != term.c8_on) {
		buffer_unmap(buf);
		if (buffer_init(buf) < 0)
			abort();
	}

	if (buf->cols != term.col || buf->rows != term.row) {
		free(buf->shadow);
		buf->shadow = calloc(term.col * term.row,
				     sizeof(*buf->shadow));
		if (buf->shadow == NULL)
			abort();
		buf->cols = term.col;
		buf->rows = term.row;
		buf->valid = false;
	}

	return buf;
}

typedef uint8_t u8;
typedef uint32_t u32;

#define mul(a, b) (((u32)(a) * (u32)(b) + 255) >> 8)
#define join(a, r, g, b) ((u32)(a) << 24 | (u32)(r) << 16 | (u32)(g) << 8 | (u32)(b))

static void blank(uint32_t *dst, int w, u8 br, u8 bg, u8 bb, u8 ba)
{
	int i;
	uint32_t b;
	int h = term.cheight;

	b = join(ba, mul(br, ba), mul(bg, ba), mul(bb, ba));
	w *= term.cwidth;

	while (h--) {
		for (i = 0; i < w; ++i)
			dst[i] = b;
		dst += term.width;
	}
}

/* Only vis of the w cells of the glyph fit before the right edge of the grid;
 * writing the rest would land on the next row of pixels. */
static void print(uint32_t *dst, int w, int vis,
		  u8 br, u8 bg, u8 bb,
		  u8 fr, u8 fg, u8 fb,
		  u8 ba, unsigned char *glyph)
{
	int i;
	int h = term.cheight;

	w *= term.cwidth;
	vis *= term.cwidth;

	br = mul(br, ba);
	bg = mul(bg, ba);
	bb = mul(bb, ba);
	while (h--) {
		for (i = 0; i < vis; ++i) {
			u8 fa = glyph[i];
			if (fa == 0) {
				dst[i] = join(ba, br, bg, bb);
			} else if (fa == 0xff) {
				dst[i] = join(0xff, fr, fg, fb);
			} else {
				u8 ca = 255 - fa;
				dst[i] = join(fa + mul(ba, ca),
					      mul(fr, fa) + mul(br, ca),
					      mul(fg, fa) + mul(bg, ca),
					      mul(fb, fa) + mul(bb, ca));
			}
		}

		glyph += w;
		dst += term.width;
	}
}

/* A cell of a bitmap font: the foreground where a bit of the glyph is set and
 * the background where it is not, so a cell holds those two colours and no
 * blend. A pixel of the font is a k x k block of pixels. */
static void print1(uint32_t *dst, int vis, int k,
		   u8 br, u8 bg, u8 bb,
		   u8 fr, u8 fg, u8 fb,
		   u8 ba, const uint8_t *rows, int bpr)
{
	uint32_t bgc = join(ba, mul(br, ba), mul(bg, ba), mul(bb, ba));
	uint32_t fgc = join(0xff, fr, fg, fb);
	int h = term.cheight;
	int y, x;

	vis *= term.cwidth;
	for (y = 0; y < h; ++y) {
		const uint8_t *r = rows + (y / k) * bpr;

		if (k == 1) {
			for (x = 0; x < vis; ++x)
				dst[x] = r[x >> 3] & 0x80 >> (x & 7) ? fgc : bgc;
		} else {
			for (x = 0; x < vis; ++x)
				dst[x] = r[x / k >> 3] & 0x80 >> (x / k & 7) ?
					 fgc : bgc;
		}
		dst += term.width;
	}
}

static bool picture_cb(uint32_t ucs4)
{
	return emoji_wide(ucs4, term.cwidth, term.cheight);
}

static void collect_cell(struct tsm_screen *tsm, uint32_t id,
			 const uint32_t *ch, size_t len, int char_width,
			 int x, int y, const struct tsm_screen_attr *a,
			 tsm_age_t age, void *data)
{
	int i = y * term.col + x;
	struct cell_key *k = &term.cur[i];
	u8 inv = a->inverse ? 0xff : 0;

	memset(k, 0, sizeof(*k));
	k->width = char_width;
#if HAVOC_SIXEL
	if (a->pic) {
		/* a tile is composed with the plain colours and inverted as a
		 * whole, so that the picture inverts too */
		k->pic = a->pic;
		k->pgen = gfx_pic_gen(term.gfx, a->pic);
		k->kflags = a->inverse ? KF_INV : 0;
		inv = 0;
	}
#endif
	k->bg = join(0, a->br ^ inv, a->bg ^ inv, a->bb ^ inv);
	/* a blank cell has no foreground, so the colour must not tell two
	 * of them apart; the second half of a wide cell is blank for the
	 * same reason, whatever character it was left holding */
	if (len && char_width) {
		unsigned px;

		k->id = id;
		k->fg = join(0, a->fr ^ inv, a->fg ^ inv, a->fb ^ inv);
		term.cur_ch[i] = ch[0];
		k->picture = char_width == 2 &&
			     emoji_image(ch[0], term.cwidth, term.cheight, &px);
	}
}

/* The same cells into a C8 buffer: an index for each colour, found in the
 * table of colours in use (c8.h). Only the bitmap font is drawn this way: a
 * cell holds two colours and no blend. */
static void blank8(uint8_t *dst, int stride, int w, uint8_t c)
{
	int h = term.cheight;

	w *= term.cwidth;
	while (h--) {
		memset(dst, c, w);
		dst += stride;
	}
}

static void print18(uint8_t *dst, int stride, int vis, int k, uint8_t bgi,
		    uint8_t fgi, const uint8_t *rows, int bpr)
{
	int h = term.cheight;
	int y, x;

	vis *= term.cwidth;
	for (y = 0; y < h; ++y) {
		const uint8_t *r = rows + (y / k) * bpr;

		for (x = 0; x < vis; ++x)
			dst[x] = r[x / k >> 3] & 0x80 >> (x / k & 7) ? fgi : bgi;
		dst += stride;
	}
}

/* A picture is blended in colours of its own: a colour the table has is
 * used, any other is the nearest of the cube, so pictures never take a slot
 * from the text. */
static void picture8(uint8_t *dst, int stride, const uint8_t *rec,
		     unsigned px, uint32_t bgl, uint32_t bgr)
{
	static uint32_t *tmp;
	static size_t tmplen;
	int w = 2 * term.cwidth, h = term.cheight;
	int x, y;

	if (tmplen < (size_t)w * h) {
		free(tmp);
		tmplen = (size_t)w * h;
		tmp = malloc(tmplen * sizeof(*tmp));
		if (tmp == NULL)
			abort();
	}
	emoji_paint(tmp, w, term.cwidth, term.cheight, bgl, bgr,
		    term.cfg.opacity, rec, px);
	for (y = 0; y < h; ++y)
		for (x = 0; x < w; ++x)
			dst[y * stride + x] = c8_find(&term.colours,
						      tmp[y * w + x] & 0xffffff);
}

static void paint_cell8(struct buffer *buffer, int x, int y, int w, int vis)
{
	int i = y * term.col + x;
	const struct cell_key *k = &term.cur[i];
	uint8_t *dst = buffer->data;
	int stride = buffer->stride;
	uint8_t bgi = c8_get(&term.colours, k->bg);

	dst += term.margin.top * stride + term.margin.left;
	dst = &dst[y * term.cheight * stride + x * term.cwidth];

	if (k->picture) {
		const uint8_t *rec;
		unsigned px;

		if (x + 1 >= term.col || k[1].width != 0) {
			blank8(dst, stride, 1, bgi);
			return;
		}
		rec = emoji_image(term.cur_ch[i], term.cwidth, term.cheight,
				  &px);
		if (rec) {
			/* the second half's colour is looked up too, so that
			 * a plain background there has its slot */
			c8_get(&term.colours, k[1].bg);
			picture8(dst, stride, rec, px, k->bg, k[1].bg);
			return;
		}
	}

	if (k->id == 0) {
		blank8(dst, stride, vis, bgi);
	} else {
		int bpr;
		const uint8_t *g = pixfont_glyph(term.cur_ch[i], w, &bpr);

		print18(dst, stride, vis, pixfont_multiple(), bgi,
			c8_get(&term.colours, k->fg), g, bpr);
	}
}

#if HAVOC_SIXEL
/* A tile of a sixel picture over the cell's background; unset pixels of the
 * picture show the background. Inverse inverts what is composed: with the
 * alpha a (premultiplied colours), c becomes a - c. */
static void paint_tile(struct buffer *buffer, int x, int y)
{
	const struct cell_key *k = &term.cur[y * term.col + x];
	uint32_t *dst = buffer->data;
	struct gfx_target t;
	int px, py;

	dst += term.margin.top * term.width + term.margin.left;
	dst = &dst[y * term.cheight * term.width + x * term.cwidth];
	blank(dst, 1, k->bg >> 16, k->bg >> 8, k->bg, term.cfg.opacity);
	t.px = dst;
	t.stride = term.width;
	t.w = term.cwidth;
	t.h = term.cheight;
	gfx_paint(term.gfx, GFX_TILE, k->pic, &t);

	if (!(k->kflags & KF_INV))
		return;
	for (py = 0; py < term.cheight; ++py) {
		uint32_t *r = dst + py * term.width;

		for (px = 0; px < term.cwidth; ++px)
			r[px] = (r[px] & 0xff000000) |
				((r[px] >> 24) * 0x010101 - (r[px] & 0xffffff));
	}
}
#endif

static void paint_cell(struct buffer *buffer, int x, int y)
{
	int i = y * term.col + x;
	const struct cell_key *k = &term.cur[i];
	uint32_t *dst = buffer->data;
	int w = k->width;
	int vis;
	u8 br = k->bg >> 16, bg = k->bg >> 8, bb = k->bg;

	if (w == 0) {
		/* The second half of a wide cell is drawn with the first.
		 * Anything else (the first half was overwritten) would
		 * otherwise keep the old half glyph on screen. */
		if (x > 0 && term.cur[i - 1].width == 2)
			return;
		w = 1;
	}
	vis = x + w > term.col ? term.col - x : w;

	if (buffer->c8) {
		paint_cell8(buffer, x, y, w, vis);
		return;
	}

#if HAVOC_SIXEL
	if (k->pic) {
		paint_tile(buffer, x, y);
		return;
	}
#endif

	dst += term.margin.top * term.width + term.margin.left;
	dst = &dst[y * term.cheight * term.width + x * term.cwidth];

	if (k->picture) {
		const uint8_t *rec;
		unsigned px;

		/* half a picture says nothing: its second half was written
		 * over, or the right edge cut it off (a narrower window,
		 * characters inserted before it) */
		if (x + 1 >= term.col || k[1].width != 0) {
			blank(dst, 1, br, bg, bb, term.cfg.opacity);
			return;
		}
		rec = emoji_image(term.cur_ch[i], term.cwidth, term.cheight,
				  &px);
		if (rec) {
			emoji_paint(dst, term.width, term.cwidth, term.cheight,
				    k->bg, k[1].bg, term.cfg.opacity, rec, px);
			return;
		}
	}

	if (k->id == 0) {
		blank(dst, vis, br, bg, bb, term.cfg.opacity);
	} else if (pixfont_active()) {
		int bpr;
		const uint8_t *g = pixfont_glyph(term.cur_ch[i], w, &bpr);

		print1(dst, vis, pixfont_multiple(),
		       br, bg, bb, k->fg >> 16, k->fg >> 8, k->fg,
		       term.cfg.opacity, g, bpr);
	} else {
		/* todo, combining marks */
		unsigned char *g = get_glyph(k->id, term.cur_ch[i], w);

		print(dst, w, vis, br, bg, bb,
		      k->fg >> 16, k->fg >> 8, k->fg,
		      term.cfg.opacity, g);
	}
}

static uint32_t margin_colour(void)
{
	uint8_t a = term.cfg.opacity;
	uint8_t *rgb = tsm_screen_get_background(term.screen);

	return join(a, mul(rgb[0], a), mul(rgb[1], a), mul(rgb[2], a));
}

static void draw_margin8(struct buffer *buffer, uint32_t c)
{
	uint8_t *dst = buffer->data;
	uint8_t i8 = c8_get(&term.colours, c);
	int s = buffer->stride;
	int inw = term.col * term.cwidth;
	int inh = term.row * term.cheight;
	int y;

	for (y = 0; y < term.height; ++y) {
		if (y < term.margin.top || y >= term.margin.top + inh) {
			memset(dst + y * s, i8, term.width);
		} else {
			memset(dst + y * s, i8, term.margin.left);
			memset(dst + y * s + term.margin.left + inw, i8,
			       term.width - term.margin.left - inw);
		}
	}
}

static void draw_margin(struct buffer *buffer, uint32_t c)
{
	uint32_t *dst = buffer->data;
	int inw = term.col * term.cwidth;
	int inh = term.row * term.cheight;
	int i, j;

	if (buffer->c8) {
		draw_margin8(buffer, c);
		return;
	}

	for (i = 0; i < term.width * term.margin.top; ++i)
		dst[i] = c;

	for (i = (term.margin.top + inh) * term.width;
	     i < term.height * term.width;
	     ++i)
		dst[i] = c;

	for (i = term.margin.top; i < term.margin.top + inh; ++i) {
		for (j = 0; j < term.margin.left; ++j)
			dst[i * term.width + j] = c;

		for (j = term.margin.left + inw; j < term.width; ++j)
			dst[i * term.width + j] = c;
	}
}

static void frame_callback(void *data, struct wl_callback *cb, uint32_t time)
{
	assert(term.cb == cb);
	wl_callback_destroy(cb);
	term.cb = NULL;
	term.can_redraw = true;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_callback,
};

/* Spread a flag over the cells that share pixels with a flagged wide cell, the
 * same way before and after the change (old is what the flag is relative to):
 * painting either half of a wide cell touches both. A picture is also drawn
 * differently when the cell after it changes (its background, or a character
 * written over its second half). */
static void spread_wide(uint8_t bit, const struct cell_key *old)
{
	int x, y;

	for (y = 0; y < term.row; ++y) {
		const struct cell_key *c = &term.cur[y * term.col];
		const struct cell_key *o = &old[y * term.col];
		uint8_t *f = &term.flags[y * term.col];

		for (x = term.col - 1; x > 0; --x)
			if ((f[x] & bit) && (c[x].width == 0 ||
					      c[x - 1].picture ||
					      o[x - 1].picture))
				f[x - 1] |= bit;
		for (x = 0; x < term.col - 1; ++x)
			if ((f[x] & bit) &&
			    (c[x].width == 2 || o[x].width == 2 ||
			     c[x + 1].width == 0))
				f[x + 1] |= bit;
	}
}

struct rect {
	int x0, x1, y0, y1;
};

#define MAX_RECTS 8

/* The flagged cells as rectangles of cells, row spans that overlap the span
 * above joined into one. Returns -1 when there would be more than MAX_RECTS. */
static int damage_rects(struct rect *r)
{
	int x, y, n = 0;
	bool open = false;

	for (y = 0; y < term.row; ++y) {
		uint8_t *f = &term.flags[y * term.col];
		int x0 = -1, x1 = -1;

		for (x = 0; x < term.col; ++x) {
			if (!(f[x] & CELL_DAMAGE))
				continue;
			if (x0 < 0)
				x0 = x;
			x1 = x;
		}

		if (x0 < 0) {
			open = false;
			continue;
		}

		if (open && x0 <= r[n - 1].x1 && x1 >= r[n - 1].x0) {
			if (x0 < r[n - 1].x0)
				r[n - 1].x0 = x0;
			if (x1 > r[n - 1].x1)
				r[n - 1].x1 = x1;
			r[n - 1].y1 = y;
		} else {
			if (n == MAX_RECTS)
				return -1;
			r[n++] = (struct rect){ x0, x1, y, y };
		}
		open = true;
	}

	return n;
}

/* Tell the compositor what differs from the frame it shows, and nothing more.
 * Many small rectangles cost requests, and a box that is mostly changed
 * anyway is no cheaper than the whole surface. */
static void send_damage(bool full)
{
	struct rect r[MAX_RECTS];
	int i, n = 0, area = 0;

	if (!full) {
		n = damage_rects(r);
		if (n < 0) {
			int x0 = term.col, x1 = -1, y0 = term.row, y1 = -1;
			int x, y;

			for (y = 0; y < term.row; ++y) {
				for (x = 0; x < term.col; ++x) {
					if (!(term.flags[y * term.col + x] &
					      CELL_DAMAGE))
						continue;
					x0 = x < x0 ? x : x0;
					x1 = x > x1 ? x : x1;
					y0 = y < y0 ? y : y0;
					y1 = y > y1 ? y : y1;
				}
			}
			r[0] = (struct rect){ x0, x1, y0, y1 };
			n = 1;
		}
		for (i = 0; i < n; ++i)
			area += (r[i].x1 - r[i].x0 + 1) *
				(r[i].y1 - r[i].y0 + 1);
		if (area * 4 > term.col * term.row * 3)
			full = true;
	}

	if (full) {
		wl_surface_damage_buffer(term.surf, 0, 0, term.width,
					 term.height);
		return;
	}

	for (i = 0; i < n; ++i) {
		int x = term.margin.left + r[i].x0 * term.cwidth;
		int y = term.margin.top + r[i].y0 * term.cheight;
		int x2 = x + (r[i].x1 - r[i].x0 + 1) * term.cwidth;
		int y2 = y + (r[i].y1 - r[i].y0 + 1) * term.cheight;

		/* A viewport maps the buffer to the surface with rounding, so
		 * a compositor may take a pixel at the edge for part of its
		 * neighbour and leave a seam. */
		if (term.scale != 120) {
			x = x > 0 ? x - 1 : 0;
			y = y > 0 ? y - 1 : 0;
			x2 = x2 < term.width ? x2 + 1 : term.width;
			y2 = y2 < term.height ? y2 + 1 : term.height;
		}
		wl_surface_damage_buffer(term.surf, x, y, x2 - x, y2 - y);
	}
}

/* Whether the window is drawn in C8 now (patch 0005): the user asked for it,
 * the compositor has the palette protocol and the C8 shm format and says that
 * this window alone is on a C8 output, nothing is blended (opacity 255) and
 * the text comes from a bitmap font. Anything else is XRGB8888 as before. */
static bool c8_wanted(void)
{
	return term.cfg.c8 && term.palm && term.pal && term.shm_c8 &&
	       term.exclusive && term.cfg.opacity == 255 && pixfont_active();
}

/* Called at the start of a redraw. A change of format, or colours that did
 * not fit, start from nothing: the colours are chosen again and both
 * buffers and the compositor's picture count as unknown. */
static void c8_update(void)
{
	bool want = c8_wanted();

	if (want != term.c8_on || (want && term.colours.full)) {
		term.c8_on = want;
		c8_reset(&term.colours);
		term.buf[0].valid = false;
		term.buf[1].valid = false;
		term.shown_valid = false;
	}
}

/* The palette goes with the commit that first needs it. */
static void c8_send_palette(void)
{
#ifdef HAVE_PICOWL_PALETTE
	uint32_t pal[256];
	struct wl_array a;

	if (!term.c8_on || !term.colours.dirty)
		return;
	c8_palette(&term.colours, pal);
	a.size = a.alloc = sizeof(pal);
	a.data = pal;
	picowl_surface_palette_v1_set_palette(term.pal, &a);
#endif
	term.colours.dirty = false;
}

static void redraw(void)
{
	struct buffer *buffer;
	int i, x, y, n = term.col * term.row;
	bool margins = term.cfg.margin || term.cfg.padding;
	uint32_t mc = margins ? margin_colour() : 0;
	bool full;

	c8_update();
	tsm_screen_draw(term.screen, collect_cell, NULL);

	/* What the compositor shows is only known to be what we last
	 * committed. Without that, or when the margins change, it all goes. */
	full = term.full_redraw || !term.shown_valid || mc != term.shown_margin;
	for (i = 0; i < n; ++i)
		term.flags[i] = full || memcmp(&term.cur[i], &term.shown[i],
					       sizeof(*term.cur))
				? CELL_DAMAGE : 0;
	if (!full) {
		for (i = 0; i < n && !term.flags[i]; ++i)
			;
		if (i == n) {
			/* nothing to show, so no commit that would only cost
			 * the compositor a frame */
			term.need_redraw = false;
			return;
		}
		spread_wide(CELL_DAMAGE, term.shown);
	}

	buffer = swap_buffers();

	if (buffer == NULL) {
		fprintf(stderr, "no buffer available, cannot redraw\n");
		return;
	}

	if (term.full_redraw) {
		/* so that nothing left in the buffer can hide a cell that was
		 * not painted */
		memset(buffer->data, 0, buffer->size);
		buffer->valid = false;
	}

	for (i = 0; i < n; ++i)
		term.flags[i] &= ~CELL_PAINT;
	for (i = 0; i < n; ++i)
		if (!buffer->valid || memcmp(&term.cur[i], &buffer->shadow[i],
					     sizeof(*term.cur)))
			term.flags[i] |= CELL_PAINT;
	if (buffer->valid)
		spread_wide(CELL_PAINT, buffer->shadow);

	wl_surface_attach(term.surf, buffer->b, 0, 0);
	if (margins && (!buffer->valid || buffer->margin != mc))
		draw_margin(buffer, mc);
	for (y = 0; y < term.row; ++y)
		for (x = 0; x < term.col; ++x)
			if (term.flags[y * term.col + x] & CELL_PAINT)
				paint_cell(buffer, x, y);
	memcpy(buffer->shadow, term.cur, n * sizeof(*term.cur));
	buffer->margin = mc;
	buffer->valid = true;

	c8_send_palette();
	send_damage(full);
	memcpy(term.shown, term.cur, n * sizeof(*term.cur));
	term.shown_margin = mc;
	term.shown_valid = true;

	term.cb = wl_surface_frame(term.surf);
	wl_callback_add_listener(term.cb, &frame_listener, NULL);
	wl_surface_commit(term.surf);

	buffer->busy = true;
	term.can_redraw = false;
	term.need_redraw = false;
}

static void paste(bool primary)
{
	if (primary) {
		if (!term.ps_dm || !term.paste.ps_mime)
			return;
	} else {
		if (!term.d_dm || !term.paste.d_mime)
			return;
	}

	if (term.paste.active)
		end_paste();

	if (pipe(term.paste.fd) < 0)
		return;

	if (primary) {
		zwp_primary_selection_offer_v1_receive(term.paste.ps_offer,
						       term.paste.ps_mime,
						       term.paste.fd[1]);
	} else {
		wl_data_offer_receive(term.paste.d_offer, term.paste.d_mime,
				      term.paste.fd[1]);
	}
	close(term.paste.fd[1]);

	term.paste.active = true;
	tsm_vte_paste_begin(term.vte);
}

static void action_paste(void)
{
	paste(false);
}

static void ds_target(void *d, struct wl_data_source *ds, const char *mt)
{
}

static void ds_send(void *data, struct wl_data_source *ds,
		    const char *mime_type, int32_t fd)
{
	write(fd, term.d_copy.data, strlen(term.d_copy.data));
	close(fd);
}

static void ds_cancelled(void *data, struct wl_data_source *source)
{
	wl_data_source_destroy(term.d_copy.source);
	term.d_copy.source = NULL;
	free(term.d_copy.data);
}

static void ds_dnd_drop_performed(void *data, struct wl_data_source *ds)
{
}

static void ds_dnd_finished(void *data, struct wl_data_source *ds)
{
}

static void ds_action(void *data, struct wl_data_source *ds, uint32_t a)
{
}

static struct wl_data_source_listener ds_listener = {
	.target = ds_target,
	.send = ds_send,
	.cancelled = ds_cancelled,
	.dnd_drop_performed = ds_dnd_drop_performed,
	.dnd_finished = ds_dnd_finished,
	.action = ds_action,
};

static void d_copy(uint32_t serial)
{
	if (!term.d_dm)
		return;

	if (tsm_screen_selection_copy(term.screen, &term.d_copy.data) < 0)
		return;

	term.d_copy.source =
		wl_data_device_manager_create_data_source(term.d_dm);
	wl_data_source_offer(term.d_copy.source, "UTF8_STRING");
	wl_data_source_offer(term.d_copy.source, "text/plain");
	wl_data_source_add_listener(term.d_copy.source, &ds_listener, NULL);
	wl_data_device_set_selection(term.d_d, term.d_copy.source, serial);
}

static void d_uncopy(void)
{
	if (!term.d_dm)
		return;

	if (!term.d_copy.source)
		return;

	ds_cancelled(NULL, term.d_copy.source);
}

static uint32_t action_copy_serial;
static void action_copy(void)
{
	d_uncopy();
	if (term.selection == SS_ACTIVE)
		d_copy(action_copy_serial);
}

static void reset_repeat(void)
{
	term.repeat.timeout = -1;
}

static void setup_compose(void)
{
	struct xkb_compose_table *compose_table;
	struct xkb_compose_state *compose_state;
	char *lang = getenv("LANG");

	if (lang == NULL)
		return;

	compose_table =
		xkb_compose_table_new_from_locale(term.xkb_ctx,
						  lang,
						  XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!compose_table) {
		fprintf(stderr, "could not create XKB compose table "
				"for locale '%s'.\n", lang);
		return;
	}

	compose_state = xkb_compose_state_new(compose_table,
					      XKB_COMPOSE_STATE_NO_FLAGS);
	if (!compose_state) {
		fprintf(stderr, "could not create XKB compose state. "
				"Disabling compose.\n");
		xkb_compose_table_unref(compose_table);
		return;
	}

	xkb_compose_table_unref(term.xkb_compose_table);
	xkb_compose_state_unref(term.xkb_compose_state);
	term.xkb_compose_table = compose_table;
	term.xkb_compose_state = compose_state;
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt,
		       int32_t fd, uint32_t size)
{
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	char *map;

	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}

	keymap = xkb_keymap_new_from_string(term.xkb_ctx, map,
					    XKB_KEYMAP_FORMAT_TEXT_V1,
					    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);

	if (!keymap) {
		fprintf(stderr, "failed to compile keymap\n");
		return;
	}

	state = xkb_state_new(keymap);
	if (!state) {
		fprintf(stderr, "failed to create XKB state\n");
		xkb_keymap_unref(keymap);
		return;
	}

	xkb_keymap_unref(term.xkb_keymap);
	xkb_state_unref(term.xkb_state);
	term.xkb_keymap = keymap;
	term.xkb_state = state;

	setup_compose();

	term.xkb_ctrl = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	term.xkb_alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
	term.xkb_shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf, struct wl_array *keys)
{
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial,
		      struct wl_surface *surf)
{
	reset_repeat();
}

/* Text input exists only so that the compositor knows a text field has the
 * focus and can show an on-screen keyboard. Keys, including the ones from an
 * on-screen keyboard, keep arriving through wl_keyboard, so the text and
 * preedit events are ignored. The compositor sends enter and leave when the
 * keyboard focus moves to or from the surface; enabling there means once per
 * focus, never per key, so an on-screen keyboard that was hidden by hand
 * stays hidden until the focus comes back. */
static void ti_enter(void *data, struct zwp_text_input_v3 *ti,
		     struct wl_surface *surf)
{
	int x = destination(term.margin.left);
	int y = destination(term.margin.top);
	int w = destination(term.cwidth);
	int h = destination(term.cheight);

	if (term.screen) {
		x += destination(term.cwidth *
				 tsm_screen_get_cursor_x(term.screen));
		y += destination(term.cheight *
				 tsm_screen_get_cursor_y(term.screen));
	}

	zwp_text_input_v3_enable(ti);
	zwp_text_input_v3_set_content_type(ti,
		ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
		ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL);
	zwp_text_input_v3_set_cursor_rectangle(ti, x, y, w, h);
	zwp_text_input_v3_commit(ti);
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti,
		     struct wl_surface *surf)
{
	zwp_text_input_v3_disable(ti);
	zwp_text_input_v3_commit(ti);
}

static void ti_preedit_string(void *data, struct zwp_text_input_v3 *ti,
			      const char *text, int32_t begin, int32_t end)
{
}

static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
			     const char *text)
{
}

static void ti_delete_surrounding_text(void *data,
				       struct zwp_text_input_v3 *ti,
				       uint32_t before, uint32_t after)
{
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
}

static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = ti_enter,
	.leave = ti_leave,
	.preedit_string = ti_preedit_string,
	.commit_string = ti_commit_string,
	.delete_surrounding_text = ti_delete_surrounding_text,
	.done = ti_done,
};

static xkb_keysym_t compose(xkb_keysym_t sym)
{
	if (!term.xkb_compose_state)
		return sym;
	if (sym == XKB_KEY_NoSymbol)
		return sym;
	if (xkb_compose_state_feed(term.xkb_compose_state,
				   sym) != XKB_COMPOSE_FEED_ACCEPTED)
		return sym;

	switch (xkb_compose_state_get_status(term.xkb_compose_state)) {
	case XKB_COMPOSE_COMPOSED:
		return xkb_compose_state_get_one_sym(term.xkb_compose_state);
	case XKB_COMPOSE_COMPOSING:
	case XKB_COMPOSE_CANCELLED:
		return XKB_KEY_NoSymbol;
	case XKB_COMPOSE_NOTHING:
	default:
		return sym;
	}
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial,
		    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym, lsym;
	uint32_t unicode;
	struct binding *b;
	void (*action)(void) = NULL;

	if (!term.xkb_keymap || !term.xkb_state)
		return;

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (term.repeat.key == key)
			reset_repeat();
		return;
	}

	cursor_set(NULL);

	sym = compose(xkb_state_key_get_one_sym(term.xkb_state, key + 8));

	unicode = xkb_keysym_to_utf32(sym);
	if (unicode == 0)
		unicode = TSM_VTE_INVALID;

	lsym = xkb_keysym_to_lower(sym);
	action_copy_serial = serial;
	b = term.binding;
	while (b) {
		if (term.mods == b->mods && lsym == b->sym) {
			b->action();
			action = b->action;
			break;
		}
		b = b->next;
	}

	if (!action) {
		if (tsm_vte_handle_keyboard(term.vte, sym, XKB_KEY_NoSymbol,
					    term.mods, unicode) &&
		    term.cfg.scroll_to_bottom_on_input &&
		    tsm_screen_sb_reset(term.screen))
			term.need_redraw = true;
	}

	if (xkb_keymap_key_repeats(term.xkb_keymap, key + 8)) {
		term.repeat.key = key;
		term.repeat.sym = sym;
		term.repeat.unicode = unicode;
		term.repeat.action = action;
		term.repeat.timeout = term.repeat.delay;
		term.repeat.start = now();
	}
}

static void kbd_mods(void *data, struct wl_keyboard *k, uint32_t serial,
		     uint32_t depressed, uint32_t latched, uint32_t locked,
		     uint32_t group)
{
	if (!term.xkb_keymap || !term.xkb_state)
		return;

	xkb_state_update_mask(term.xkb_state, depressed, latched, locked,
			      0, 0, group);

	term.mods = 0;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_alt,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_ALT_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_ctrl,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_CONTROL_MASK;
	if (xkb_state_mod_index_is_active(term.xkb_state, term.xkb_shift,
					  XKB_STATE_MODS_EFFECTIVE) == 1)
		term.mods |= TSM_SHIFT_MASK;

	reset_repeat();
}

static void kbd_repeat(void *data, struct wl_keyboard *k,
		       int32_t rate, int32_t delay)
{
	if (rate > 1000)
		rate = 1000;

	term.repeat.interval = rate > 0 ? 1000 / rate : -1;
	term.repeat.delay = rate > 0 ? delay : -1;
}

static struct wl_keyboard_listener kbd_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_mods,
	.repeat_info = kbd_repeat,
};

static void pss_send(void *data,
		     struct zwp_primary_selection_source_v1 *source,
		     const char *mime_type,
		     int32_t fd)
{
	write(fd, term.ps_copy.data, strlen(term.ps_copy.data));
	close(fd);
}

static void pss_cancelled(void *data,
			  struct zwp_primary_selection_source_v1 *source)
{
	zwp_primary_selection_source_v1_destroy(term.ps_copy.source);
	term.ps_copy.source = NULL;
	free(term.ps_copy.data);
}

static struct zwp_primary_selection_source_v1_listener pss_listener = {
	.send = pss_send,
	.cancelled = pss_cancelled,
};


static void ps_copy(uint32_t serial)
{
	if (!term.ps_dm)
		return;

	if (tsm_screen_selection_copy(term.screen, &term.ps_copy.data) < 0)
		return;

	term.ps_copy.source =
		zwp_primary_selection_device_manager_v1_create_source(term.ps_dm);
	zwp_primary_selection_source_v1_offer(term.ps_copy.source, "UTF8_STRING");
	zwp_primary_selection_source_v1_offer(term.ps_copy.source, "text/plain");
	zwp_primary_selection_source_v1_add_listener(term.ps_copy.source,
						     &pss_listener, NULL);
	zwp_primary_selection_device_v1_set_selection(term.ps_d,
						      term.ps_copy.source,
						      serial);
}

static void ps_uncopy(void)
{
	if (!term.ps_dm)
		return;

	if (!term.ps_copy.source)
		return;

	pss_cancelled(NULL, term.ps_copy.source);
}

static inline int grid_x(void)
{
	double px = (wl_fixed_to_double(term.ptr_x) * term.scale) / 120;
	int x = (px - term.margin.left) / term.cwidth;

	if (x < 0)
		return 0;

	if (x >= term.col)
		return term.col - 1;

	return x;
}

static inline int grid_y(void)
{
	double py = (wl_fixed_to_double(term.ptr_y) * term.scale) / 120;
	int y = (py - term.margin.top) / term.cheight;

	if (y < 0)
		return 0;

	if (y >= term.row)
		return term.row - 1;

	return y;
}

static void selection_start(enum tsm_screen_selection_mode mode)
{
	tsm_screen_selection_start(term.screen, mode, grid_x(), grid_y());
}

static void ptr_enter(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;

	term.cursor.enter_serial = serial;
	cursor_set(term.cursor.text);
}

static void ptr_leave(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, struct wl_surface *surface)
{
	cursor_unset();
}

static void ptr_motion(void *data, struct wl_pointer *wl_pointer,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	term.ptr_x = x;
	term.ptr_y = y;

	switch (term.selection) {
	case SS_ANCHORED:
		ps_uncopy();
		term.selection = SS_DRAGGING;
		selection_start(TSM_SM_CHAR);
		term.need_redraw = true;
		break;
	case SS_DRAGGING:
		tsm_screen_selection_target(term.screen, grid_x(), grid_y());
		term.need_redraw = true;
		break;
	case SS_RESET:
	case SS_ACTIVE:
		break;
	}

	if (term.cursor.current == NULL && term.cursor.text)
		cursor_set(term.cursor.text);
}

static void ptr_button(void *data, struct wl_pointer *wl_pointer,
		       uint32_t serial, uint32_t time, uint32_t button,
		       uint32_t state)
{
	if (button == 0x110) {
		switch (state) {
		case WL_POINTER_BUTTON_STATE_PRESSED:
			if (term.selection == SS_ACTIVE) {
				tsm_screen_selection_reset(term.screen);
				term.need_redraw = true;
			}
			term.selection = SS_ANCHORED;

			if (term.click.count > 0
			    && term.click.x == grid_x()
			    && term.click.y == grid_y()) {
				++term.click.count;
			} else {
				term.click.count = 1;
				term.click.x = grid_x();
				term.click.y = grid_y();
			}

			if (term.click.count >= 2) {
				ps_uncopy();
				if (term.click.count == 2) {
					selection_start(TSM_SM_WORD);
				}
				if (term.click.count == 3) {
					selection_start(TSM_SM_LINE);
					term.click.count = 0;
				}
				term.selection = SS_DRAGGING;
				term.need_redraw = true;
			}
			break;
		case WL_POINTER_BUTTON_STATE_RELEASED:
			if (term.selection == SS_ANCHORED) {
				term.selection = SS_RESET;
			}

			if (term.selection == SS_DRAGGING) {
				ps_copy(serial);
				tsm_screen_selection_finish(term.screen);
				term.selection = SS_ACTIVE;
			}
		}
	} else if (button == 0x112 &&
		   state == WL_POINTER_BUTTON_STATE_RELEASED) {
		paste(true);
	}

	if (term.cursor.current == NULL && term.cursor.text)
		cursor_set(term.cursor.text);
}

static void ptr_axis(void *data, struct wl_pointer *wl_pointer,
		     uint32_t time, uint32_t axis, wl_fixed_t value)
{
	int v = wl_fixed_to_double(value) / 3;

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;

	if (v > 0)
		tsm_screen_sb_down(term.screen, v);
	else
		tsm_screen_sb_up(term.screen, -v);

	term.need_redraw = true;
}

static void ptr_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void ptr_axis_source(void *data, struct wl_pointer *wl_pointer,
			    uint32_t axis_source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *wl_pointer,
			  uint32_t time, uint32_t axis)
{
}

static void ptr_axis_discrete(void *data, struct wl_pointer *wl_pointer,
			      uint32_t axis, int32_t discrete)
{
}

static struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !term.kbd) {
		term.kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(term.kbd, &kbd_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && term.kbd) {
		wl_keyboard_release(term.kbd);
		term.kbd = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !term.ptr) {
		term.ptr = wl_seat_get_pointer(term.seat);
		wl_pointer_add_listener(term.ptr, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && term.ptr) {
		wl_pointer_release(term.ptr);
		term.ptr = NULL;
		cursor_unset();
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void do_offer(void *d, struct wl_data_offer *o, const char *mime_type)
{
	if (strcmp(mime_type, "UTF8_STRING") == 0)
		term.paste.d_mime = "UTF8_STRING";
	else if(term.paste.d_mime == NULL &&
		strcmp(mime_type, "text/plain") == 0)
		term.paste.d_mime = "text/plain";
}

static void do_source_actions(void *d, struct wl_data_offer *o, uint32_t sa)
{
}

static void do_action(void *d, struct wl_data_offer *o, uint32_t dnd_action)
{
}

static const struct wl_data_offer_listener do_listener = {
	.offer = do_offer,
	.source_actions = do_source_actions,
	.action = do_action,
};

static void dd_data_offer(void *data, struct wl_data_device *wl_data_device,
			  struct wl_data_offer *offer)
{
	if (term.paste.d_offer)
		wl_data_offer_destroy(term.paste.d_offer);
	term.paste.d_offer = offer;
	term.paste.d_mime = NULL;
	wl_data_offer_add_listener(offer, &do_listener, NULL);
}

static void dd_enter(void *data, struct wl_data_device *wl_data_device,
		     uint32_t serial, struct wl_surface *surface,
		     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *id)
{
}

static void dd_leave(void *data, struct wl_data_device *wl_data_device)
{
}

static void dd_motion(void *data, struct wl_data_device *wl_data_device,
		      uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}

static void dd_drop(void *data, struct wl_data_device *wl_data_device)
{
}

static void dd_selection(void *data, struct wl_data_device *wl_data_device,
			 struct wl_data_offer *id)
{
	if (id == NULL && term.paste.d_offer) {
		wl_data_offer_destroy(term.paste.d_offer);
		term.paste.d_offer = NULL;
		term.paste.d_mime = NULL;
	}
}

static const struct wl_data_device_listener dd_listener = {
	.data_offer = dd_data_offer,
	.enter = dd_enter,
	.leave = dd_leave,
	.motion = dd_motion,
	.drop = dd_drop,
	.selection = dd_selection,
};

static void pso_offer(void *data,
		      struct zwp_primary_selection_offer_v1 *offer,
		      const char *mime_type)
{
	if (strcmp(mime_type, "UTF8_STRING") == 0)
		term.paste.ps_mime = "UTF8_STRING";
	else if(term.paste.ps_mime == NULL &&
		strcmp(mime_type, "text/plain") == 0)
		term.paste.ps_mime = "text/plain";
}

static const struct zwp_primary_selection_offer_v1_listener pso_listener = {
	.offer = pso_offer,
};

static void psd_data_offer(void *data,
			   struct zwp_primary_selection_device_v1 *ps_d,
			   struct zwp_primary_selection_offer_v1 *offer)
{
	if (term.paste.ps_offer)
		zwp_primary_selection_offer_v1_destroy(term.paste.ps_offer);
	term.paste.ps_offer = offer;
	term.paste.ps_mime = NULL;
	zwp_primary_selection_offer_v1_add_listener(offer, &pso_listener, NULL);
}

static void psd_selection(void *data,
			  struct zwp_primary_selection_device_v1 *ps_d,
			  struct zwp_primary_selection_offer_v1 *id)
{
	if (id == NULL && term.paste.ps_offer) {
		zwp_primary_selection_offer_v1_destroy(term.paste.ps_offer);
		term.paste.ps_offer = NULL;
		term.paste.ps_mime = NULL;
	}
}

static const struct zwp_primary_selection_device_v1_listener psd_listener = {
	.data_offer = psd_data_offer,
	.selection = psd_selection,
};

static void do_configure(void)
{
	int scaledwidth, scaledheight;
	int newwidth, newheight;
	int padding_x, padding_y;
	int col, row;
	int padding = (term.cfg.padding * term.scale + 60) / 120;
	int logicalwidth, logicalheight;
	bool pty_size = false;

	if (term.confwidth)
		scaledwidth = (term.confwidth * term.scale + 60) / 120;
	else
		scaledwidth = term.cfg.col * term.cwidth + 2 * padding;

	if (term.confheight)
		scaledheight = (term.confheight * term.scale + 60) / 120;
	else
		scaledheight = term.cfg.row * term.cheight + 2 * padding;

	if (scaledwidth < term.cwidth)
		scaledwidth = term.cwidth;
	if (scaledheight < term.cheight)
		scaledheight = term.cheight;

	/* Keep at least one cell visible if the compositor makes the window small. */
	padding_x = padding < (scaledwidth - term.cwidth) / 2
		? padding : (scaledwidth - term.cwidth) / 2;
	padding_y = padding < (scaledheight - term.cheight) / 2
		? padding : (scaledheight - term.cheight) / 2;

	col = (scaledwidth - 2 * padding_x) / term.cwidth;
	row = (scaledheight - 2 * padding_y) / term.cheight;

	if (term.cfg.margin) {
		newwidth = scaledwidth;
		newheight = scaledheight;
		term.margin.left = (newwidth - col * term.cwidth) / 2;
		term.margin.top = (newheight - row * term.cheight) / 2;

		logicalwidth = term.confwidth ? term.confwidth
					      : destination(newwidth);
		logicalheight = term.confheight ? term.confheight
						: destination(newheight);
		if (term.vp && term.fs)
			wp_viewport_set_destination(term.vp, logicalwidth,
						    logicalheight);
	} else {
		newwidth = col * term.cwidth + 2 * padding_x;
		newheight = row * term.cheight + 2 * padding_y;
		term.margin.left = padding_x;
		term.margin.top = padding_y;

		logicalwidth = destination(newwidth);
		logicalheight = destination(newheight);
		if (term.vp && term.fs)
			wp_viewport_set_destination(term.vp, logicalwidth,
						    logicalheight);
	}

	/* Every pixel is opaque only when the opacity says so, and then the
	 * compositor need not blend or even look at what is below. The region
	 * is in surface coordinates, like the viewport destination. */
	if (term.cfg.opacity == 255) {
		struct wl_region *region = wl_compositor_create_region(term.cp);

		wl_region_add(region, 0, 0, logicalwidth, logicalheight);
		wl_surface_set_opaque_region(term.surf, region);
		wl_region_destroy(region);
	}

	if (term.width != newwidth || term.height != newheight) {
		term.width = newwidth;
		term.height = newheight;

		term.shown_valid = false;
		term.need_redraw = true;
	}

	if (term.col != col || term.row != row) {
		term.col = col;
		term.row = row;
		tsm_screen_resize(term.screen, col, row);

		free(term.cur);
		free(term.shown);
		free(term.cur_ch);
		free(term.flags);
		term.cur = calloc(col * row, sizeof(*term.cur));
		term.shown = calloc(col * row, sizeof(*term.shown));
		term.cur_ch = calloc(col * row, sizeof(*term.cur_ch));
		term.flags = calloc(col * row, sizeof(*term.flags));
		if (!term.cur || !term.shown || !term.cur_ch || !term.flags)
			abort();
		term.shown_valid = false;
		pty_size = true;
		term.need_redraw = true;
	}

#if HAVE_GRAPHICS
	if (gfx_enabled(term.gfx)) {
		unsigned long xp = (unsigned long)col * term.cwidth;
		unsigned long yp = (unsigned long)row * term.cheight;

		gfx_set_geometry(term.gfx, term.cwidth, term.cheight, col, row);
		/* the pixel fields change with the cell size alone too */
		if (xp > 65535)
			xp = 65535;
		if (yp > 65535)
			yp = 65535;
		if (xp != term.ws_xpixel || yp != term.ws_ypixel) {
			term.ws_xpixel = xp;
			term.ws_ypixel = yp;
			pty_size = true;
		}
	}
#endif

	if (pty_size && term.master_fd >= 0) {
		struct winsize ws = {
			row, col, 0, 0
		};

#if HAVE_GRAPHICS
		ws.ws_xpixel = term.ws_xpixel;
		ws.ws_ypixel = term.ws_ypixel;
#endif
		if (ioctl(term.master_fd, TIOCSWINSZ, &ws) < 0)
			error("could not resize pty");
	}
}

static void rescale_font(void)
{
	font_scale((term.cfg.font_size * term.scale + 60) / 120,
		   &term.cwidth, &term.cheight);

	if (term.configured) {
		do_configure();
		/* the glyphs are new, and the cell keys do not say so */
		term.buf[0].valid = term.buf[1].valid = false;
		term.shown_valid = false;
		term.need_redraw = true;
	}
}

static void fs_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs,
			       uint32_t scale)
{
	int iscale = scale;

	if (term.vp && iscale > 0 && term.scale != iscale) {
		term.scale = iscale;
		rescale_font();

		if (term.cursor.theme)
			cursor_load();
	}
}

static const struct wp_fractional_scale_v1_listener fs_listener = {
	.preferred_scale = fs_preferred_scale,
};

static void toplvl_configure(void *data, struct xdg_toplevel *xdg_toplevel,
			     int32_t width, int32_t height,
			     struct wl_array *state)
{
	term.pendingwidth = width;
	term.pendingheight = height;
}

static void toplvl_close(void *data, struct xdg_toplevel *t)
{
	term.die = true;
}

static const struct xdg_toplevel_listener toplvl_listener = {
	.configure = toplvl_configure,
	.close = toplvl_close,
};

static void configure(void *d, struct xdg_surface *surf, uint32_t serial)
{
	xdg_surface_ack_configure(surf, serial);

	if (term.pendingwidth)
		term.confwidth = term.pendingwidth;

	if (term.pendingheight)
		term.confheight = term.pendingheight;

	do_configure();
	term.configured = true;
}

static const struct xdg_surface_listener surf_listener = {
	.configure = configure,
};

static void ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = ping,
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
	if (format == WL_SHM_FORMAT_ARGB8888)
		term.shm_argb = true;
	else if (format == WL_SHM_FORMAT_C8)
		term.shm_c8 = true;
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

#ifdef HAVE_PICOWL_PALETTE
static void palm_converter_palette(void *data,
				   struct picowl_palette_manager_v1 *m,
				   struct wl_array *entries)
{
}

static void palm_output_format(void *data,
			       struct picowl_palette_manager_v1 *m,
			       struct wl_output *output, uint32_t format)
{
}

static const struct picowl_palette_manager_v1_listener palm_listener = {
	.converter_palette = palm_converter_palette,
	.output_format = palm_output_format,
};

static void pal_presentation(void *data,
			     struct picowl_surface_palette_v1 *p,
			     uint32_t mode)
{
	bool exclusive = mode == PICOWL_SURFACE_PALETTE_V1_MODE_EXCLUSIVE;

	if (exclusive != term.exclusive) {
		term.exclusive = exclusive;
		term.need_redraw = true;
	}
}

static const struct picowl_surface_palette_v1_listener pal_listener = {
	.presentation = pal_presentation,
};
#endif

static void registry_get(void *data, struct wl_registry *r, uint32_t id,
			 const char *i, uint32_t version)
{
	if (strcmp(i, "wl_compositor") == 0) {
		term.cp = wl_registry_bind(r, id, &wl_compositor_interface, 4);
	} else if (strcmp(i, "wl_shm") == 0) {
		term.shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
		wl_shm_add_listener(term.shm, &shm_listener, NULL);
#ifdef HAVE_PICOWL_PALETTE
	} else if (strcmp(i, "picowl_palette_manager_v1") == 0) {
		if (term.cfg.c8) {
			term.palm = wl_registry_bind(r, id,
				&picowl_palette_manager_v1_interface, 1);
			picowl_palette_manager_v1_add_listener(term.palm,
				&palm_listener, NULL);
		}
#endif
	} else if (strcmp(i, "xdg_wm_base") == 0) {
		term.wm_base = wl_registry_bind(r, id, &xdg_wm_base_interface,
						1);
		xdg_wm_base_add_listener(term.wm_base, &wm_base_listener, NULL);
	} else if (strcmp(i, "wl_seat") == 0) {
		term.seat = wl_registry_bind(r, id, &wl_seat_interface, 5);
		wl_seat_add_listener(term.seat, &seat_listener, NULL);
	} else if (strcmp(i, "wl_data_device_manager") == 0) {
		term.d_dm = wl_registry_bind(r, id,
			&wl_data_device_manager_interface, 2);
	} else if (strcmp(i, "zwp_primary_selection_device_manager_v1") == 0) {
		term.ps_dm = wl_registry_bind(r, id,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	} else if (strcmp(i, "zxdg_decoration_manager_v1") == 0) {
		term.deco.manager = wl_registry_bind(r, id,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(i, "zwp_text_input_manager_v3") == 0) {
		term.tim = wl_registry_bind(r, id,
			&zwp_text_input_manager_v3_interface, 1);
	} else if (strcmp(i, "wp_viewporter") == 0) {
		term.vpm = wl_registry_bind(r, id, &wp_viewporter_interface, 1);
	} else if (strcmp(i, "wp_fractional_scale_manager_v1") == 0) {
		term.fsm = wl_registry_bind(r, id,
			&wp_fractional_scale_manager_v1_interface, 1);
	}
}

static void setup_deco(void)
{
	if (term.deco.manager) {
		term.deco.deco =
			zxdg_decoration_manager_v1_get_toplevel_decoration(
				term.deco.manager, term.toplvl);

		if (term.cfg.decorations == DECO_AUTO)
			zxdg_toplevel_decoration_v1_unset_mode(term.deco.deco);
		else
			zxdg_toplevel_decoration_v1_set_mode(term.deco.deco,
				term.cfg.decorations == DECO_NONE
				? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
				: ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void registry_loose(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener reg_listener = {
	.global = registry_get,
	.global_remove = registry_loose,
};

static void setup_pty(char *argv[])
{
	pid_t pid = forkpty(&term.master_fd, NULL, NULL, NULL);

	if (pid < 0) {
		error("forkpty failed");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		char *prog;
		setenv("TERM", "xterm-256color", 1);
		if (*argv) {
			execvp(*argv, argv);
			prog = *argv;
		} else {
			execlp(term.cfg.shell, term.cfg.shell, (char *) NULL);
			prog = term.cfg.shell;
		}
		fprintf(stderr, "could not execute %s: %s\n", prog,
			strerror(errno));
		fprintf(stderr, "running /bin/sh...\n");
		execl("/bin/sh", "/bin/sh", (char *) NULL);
		fprintf(stderr, "could not execute /bin/sh either: %s\n",
			strerror(errno));
		pause();
		exit(EXIT_FAILURE);
	}
	fcntl(term.master_fd, F_SETFL, O_NONBLOCK);
}

static void action_reset(void)
{
	tsm_vte_reset(term.vte);
}

static void action_hard_reset(void)
{
	tsm_vte_hard_reset(term.vte);
	term.need_redraw = true;
}

static void action_scroll_up(void)
{
	tsm_screen_sb_up(term.screen, 1);
	term.need_redraw = true;
}

static void action_scroll_down(void)
{
	tsm_screen_sb_down(term.screen, 1);
	term.need_redraw = true;
}

static void action_scroll_up_page(void)
{
	tsm_screen_sb_page_up(term.screen, 1);
	term.need_redraw = true;
}

static void action_scroll_down_page(void)
{
	tsm_screen_sb_page_down(term.screen, 1);
	term.need_redraw = true;
}

static void action_scroll_to_top(void)
{
	tsm_screen_sb_up(term.screen, term.cfg.scrollback);
	term.need_redraw = true;
}

static void action_scroll_to_bottom(void)
{
	tsm_screen_sb_reset(term.screen);
	term.need_redraw = true;
}

static void action_zoom_in(void)
{
	term.cfg.font_size += 1;
	rescale_font();
}

static void action_zoom_out(void)
{
	term.cfg.font_size -= 1;
	if (term.cfg.font_size < 2)
		term.cfg.font_size = 2;
	rescale_font();
}

static struct {
	char *name;
	void (*f)(void);
} actions[] = {
	{ "copy", &action_copy },
	{ "paste", &action_paste },
	{ "reset", &action_reset },
	{ "hard reset", &action_hard_reset },
	{ "scroll up", &action_scroll_up },
	{ "scroll down", &action_scroll_down },
	{ "scroll up page", &action_scroll_up_page },
	{ "scroll down page", &action_scroll_down_page },
	{ "scroll to top", &action_scroll_to_top },
	{ "scroll to bottom", &action_scroll_to_bottom },
	{ "zoom in", &action_zoom_in },
	{ "zoom out", &action_zoom_out },
};

#define CONF_FILE "havoc.cfg"

static long long cfg_num(const char *nptr, int base,
			 long long min, long long max)
{
	long long n;

	n = strtoll(nptr, NULL, base);
	return n < min ? min : n > max ? max : n;
}

static void child_config(char *key, char *val)
{
	if (strcmp(key, "program") == 0)
		strncpy(term.cfg.shell, val, sizeof(term.cfg.shell) - 1);
}

static void window_config(char *key, char *val)
{
	if (strcmp(key, "opacity") == 0)
		term.cfg.opacity = cfg_num(val, 10, 0, 255);
	else if (strcmp(key, "c8") == 0)
		term.cfg.c8 = strcmp(val, "yes") == 0;
	else if (strcmp(key, "margin") == 0)
		term.cfg.margin = strcmp(val, "yes") == 0;
	else if (strcmp(key, "padding") == 0)
		term.cfg.padding = cfg_num(val, 10, 0, 1000);
	else if (strcmp(key, "decorations") == 0)
		term.cfg.decorations = strcmp(val, "yes") == 0 ? DECO_SERVER
			: (strcmp(val, "no") == 0 ? DECO_NONE : DECO_AUTO);
}

static void terminal_config(char *key, char *val)
{
	if (strcmp(key, "rows") == 0)
		term.cfg.row = cfg_num(val, 10, 1, 1000);
	else if (strcmp(key, "columns") == 0)
		term.cfg.col = cfg_num(val, 10, 1, 1000);
	else if (strcmp(key, "scrollback") == 0)
		term.cfg.scrollback = cfg_num(val, 10, 0, INT_MAX);
	else if (strcmp(key, "scroll to bottom on input") == 0)
		term.cfg.scroll_to_bottom_on_input = strcmp(val, "yes") == 0;
}

static void font_config(char *key, char *val)
{
	if (strcmp(key, "size") == 0)
		term.cfg.font_size = cfg_num(val, 10, 6, 300);
	else if (strcmp(key, "path") == 0)
		strncpy(term.cfg.font_path, val,
			sizeof(term.cfg.font_path) - 1);
	else if (strcmp(key, "pixel") == 0)
		strncpy(term.cfg.font_pixel, strcmp(val, "no") == 0 ? "" : val,
			sizeof(term.cfg.font_pixel) - 1);
}

static void graphics_config(char *key, char *val)
{
	if (strcmp(key, "sixel") == 0) {
#if HAVOC_SIXEL
		term.cfg.gfx.sixel = strcmp(val, "yes") == 0;
#else
		if (strcmp(val, "yes") == 0)
			fprintf(stderr, "[graphics] sixel is not built in\n");
#endif
	} else if (strcmp(key, "shm") == 0) {
#if HAVE_GRAPHICS
		term.cfg.gfx.shm = strcmp(val, "yes") == 0;
#endif
	} else if (strcmp(key, "max_pixels") == 0) {
#if HAVE_GRAPHICS
		term.cfg.gfx.max_pixels = cfg_num(val, 10, 0, 16777216);
#endif
	}
}

static void bind_config(char *key, char *val)
{
	unsigned int mods = 0;
	xkb_keysym_t k;
	struct binding *b;
	size_t i;

	if (key[0] == '\0')
		return;

	while (key[1] == '-') {
		switch (key[0]) {
		case 'S':
			mods |= TSM_SHIFT_MASK;
			break;
		case 'C':
			mods |= TSM_CONTROL_MASK;
			break;
		case 'A':
			mods |= TSM_ALT_MASK;
			break;
		}
		key += 2;
	}

	k = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	k = xkb_keysym_to_lower(k);
	if (k == XKB_KEY_NoSymbol)
		return;

	b = malloc(sizeof *b);
	if (b == NULL)
		return;

	b->mods = mods;
	b->sym = k;

	for (i = 0; i < ARRAY_LENGTH(actions); ++i) {
		if (strcmp(val, actions[i].name) == 0)
			b->action = actions[i].f;
	}
	b->next = term.binding;
	term.binding = b;
}

static void color_config(char *key, char *val)
{
	uint32_t c = 0;

	if (*val == '#')
		c = cfg_num(++val, 16, 0, 0xffffff);

	if (strcmp(key, "foreground") == 0) {
		tsm_set_default_foreground(c >> 16, c >> 8, c);
	} else if (strcmp(key, "background") == 0) {
		tsm_set_default_background(c >> 16, c >> 8, c);
	} else if (strstr(key, "color") == key && *(key + 5) != '\0') {
		char *p;
		long i = strtol(key + 5, &p, 10);
		if (*p == '\0' && i >= 0 && i < 256)
			tsm_set_default_color(i, c >> 16, c >> 8, c);
	}
}

static FILE *open_config(void)
{
	char *dir;
	char path[512];
	FILE *f;

	if (term.opt.config) {
		if (*term.opt.config == '\0')
			return NULL;

		f = fopen(term.opt.config, "r");
		if (f == NULL)
			fprintf(stderr, "could not open '%s': %s, "
				"using default configuration\n",
				term.opt.config, strerror(errno));
		return f;
	}

	dir = getenv("XDG_CONFIG_HOME");
	if (dir && *dir != '\0') {
		snprintf(path, sizeof path, "%s/havoc/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;

		snprintf(path, sizeof(path), "%s/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;
	}

	dir = getenv("HOME");
	if (dir && *dir != '\0') {
		snprintf(path, sizeof(path), "%s/.config/havoc/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;

		snprintf(path, sizeof(path), "%s/.config/%s", dir, CONF_FILE);
		f = fopen(path, "r");
		if (f)
			return f;
	}

	f = fopen(CONF_FILE, "r");
	return f;
}

static void read_config(void)
{
	FILE *f = open_config();
	char *key, *val, *p, line[512];
	void (*section)(char *, char *) = NULL;

	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f)) {
		key = line;
		while (isblank(*key))
			++key;

		switch (*key) {
		case '\n':
		case '#':
			continue;
		case '[':
			p = strchr(key, ']');
			if (p == NULL)
				continue;
			*p = '\0';
			++key;

			if (strcmp(key, "child") == 0)
				section = &child_config;
			else if (strcmp(key, "window") == 0)
				section = &window_config;
			else if (strcmp(key, "terminal") == 0)
				section = &terminal_config;
			else if (strcmp(key, "font") == 0)
				section = &font_config;
			else if (strcmp(key, "graphics") == 0)
				section = &graphics_config;
			else if (strcmp(key, "bind") == 0)
				section = &bind_config;
			else if (strcmp(key, "colors") == 0)
				section = &color_config;
			else
				section = NULL;

			continue;
		default:
			val = strchr(key, '=');
			if (val == NULL)
				continue;

			p = val - 1;
			while (isblank(*p) && p > key) {
				*p = '\0';
				--p;
			}

			val[0] = '\0';
			++val;
			while (isblank(*val))
				++val;

			p = val + strlen(val) - 1;
			while (isspace(*p) && p > val) {
				*p = '\0';
				--p;
			}

			if (section)
				section(key, val);
		}
	}

	fclose(f);
}

static void usage(void)
{
	printf("usage: havoc [option...] [program [args...]]\n\n"
	       "  -c <file>  Specify configuration file."
			     " Use empty string for defaults.\n"
	       "  -l         Keep window open after the child process exits.\n"
	       "  -s <name>  Wayland display server to connect to.\n"
	       "  -i <id>    Wayland app ID to use instead of \"havoc\".\n"
	       "  -E <dir>   Directory of the emoji packs"
			     " (default " EMOJI_DIR ").\n"
	       "  -v         Show version information.\n"
	       "  -h         Show this help.\n");
}

#define take(s) (*(argv+1) \
	? *++argv \
	: (fprintf(stderr, "missing " s " after option '%s'\n", *argv), \
	  exit(EXIT_FAILURE), NULL))

int main(int argc, char *argv[])
{
	int n, i, ret = 1;
	struct binding *b;

	while (++argv, *argv && **argv == '-') {
retry:
		switch (*++*argv) {
		case 'c':
			term.opt.config = take("config file path");
			break;
		case 'l':
			term.opt.linger = true;
			break;
		case 's':
			term.opt.display = take("display name or socket");
			break;
		case 'i':
			term.opt.app_id = take("wayland app id");
			break;
		case 'E':
			emoji_set_dir(take("emoji pack directory"));
			break;
		case 'v':
			printf("havoc " VERSION "\n");
			return 0;
		case 'h':
			usage();
			return 0;
		case '-':
			goto retry;
		default:
			fprintf(stderr, "unrecognized command line option "
				"'%s'\n", *argv);
			exit(EXIT_FAILURE);
		}
	}
	read_config();
	/* for tests: paint everything into a cleared buffer and damage it all
	 * on every redraw, which is what the incremental result must equal */
	term.full_redraw = getenv("HAVOC_FULL_REDRAW") != NULL;
	setup_pty(argv);

#define fail(e, s) { fprintf(stderr, s "\n"); goto e; }

	if (term.cfg.font_pixel[0] &&
	    pixfont_select(term.cfg.font_pixel) < 0)
		fprintf(stderr, "no bitmap font '%s', using the outline font\n",
			term.cfg.font_pixel);
	if (font_init(term.cfg.font_path) < 0)
		fail(efont, "could not load font");
	font_scale(term.cfg.font_size, &term.cwidth, &term.cheight);

	term.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (term.xkb_ctx == NULL)
		fail(exkb, "failed to create xkb context");

	term.display = wl_display_connect(term.opt.display);
	if (term.display == NULL)
		fail(econnect, "could not connect to display");

	term.registry = wl_display_get_registry(term.display);
	wl_registry_add_listener(term.registry, &reg_listener, NULL);

	wl_display_roundtrip(term.display);
	if (!term.cp || !term.shm)
		fail(eglobals, "missing required globals");
	if (!term.wm_base)
		fail(eglobals, "your compositor does not support xdg_wm_base,"
			       " make sure you have the latest version");

	wl_display_roundtrip(term.display);
	if (term.shm_argb == false)
		fail(eglobals, "missing required ARGB8888 shm format");

	cursor_init();
	cursor_load();

	if (tsm_screen_new(&term.screen) < 0)
		fail(etsm, "failed to create tsm screen");
	tsm_screen_set_max_sb(term.screen, term.cfg.scrollback);
	tsm_screen_set_picture_cb(term.screen, picture_cb);

	if (tsm_vte_new(&term.vte, term.screen, wcb, NULL) < 0)
		fail(evte, "failed to create tsm vte");
#if HAVE_GRAPHICS
	term.gfx = gfx_new(term.screen, term.vte, &term.cfg.gfx);
	gfx_set_geometry(term.gfx, term.cwidth, term.cheight, term.cfg.col,
			 term.cfg.row);
#endif

	term.surf = wl_compositor_create_surface(term.cp);
	if (term.surf == NULL)
		fail(esurf, "could not create surface");
#ifdef HAVE_PICOWL_PALETTE
	/* the compositor says through it whether the window is alone on a C8
	 * output, which decides the format of the buffers */
	if (term.palm) {
		term.pal = picowl_palette_manager_v1_get_surface_palette(
			term.palm, term.surf);
		picowl_surface_palette_v1_add_listener(term.pal,
						       &pal_listener, NULL);
	}
#endif

	term.xdgsurf = xdg_wm_base_get_xdg_surface(term.wm_base, term.surf);
	if (term.xdgsurf == NULL)
		fail(exdgsurf, "could not create xdg_surface");
	xdg_surface_add_listener(term.xdgsurf, &surf_listener, NULL);

	term.toplvl = xdg_surface_get_toplevel(term.xdgsurf);
	if (term.toplvl == NULL)
		fail(etoplvl, "could not create xdg_toplevel");
	xdg_toplevel_add_listener(term.toplvl, &toplvl_listener, NULL);
	xdg_toplevel_set_title(term.toplvl, "havoc");
	xdg_toplevel_set_app_id(term.toplvl, term.opt.app_id);

	if (term.vpm && term.fsm) {
		term.vp = wp_viewporter_get_viewport(term.vpm, term.surf);
		term.fs = wp_fractional_scale_manager_v1_get_fractional_scale(
			term.fsm, term.surf);
		if (term.vp && term.fs)
			wp_fractional_scale_v1_add_listener(term.fs,
							    &fs_listener, NULL);
	}

	setup_deco();

	wl_surface_commit(term.surf);
	term.can_redraw = true;

	term.repeat.interval = -1;
	term.repeat.delay = -1;
	term.repeat.timeout = -1;

	if (term.d_dm && term.seat) {
		term.d_d = wl_data_device_manager_get_data_device(
			term.d_dm, term.seat);
		wl_data_device_add_listener(term.d_d, &dd_listener, NULL);
	}

	if (term.ps_dm && term.seat) {
		term.ps_d = zwp_primary_selection_device_manager_v1_get_device(
			term.ps_dm, term.seat);
		zwp_primary_selection_device_v1_add_listener(term.ps_d,
							     &psd_listener,
							     NULL);
	}

	if (term.tim && term.seat) {
		term.ti = zwp_text_input_manager_v3_get_text_input(term.tim,
								   term.seat);
		zwp_text_input_v3_add_listener(term.ti, &ti_listener, NULL);
	}

	term.paste.fd[0] = -1;

	struct pollfd pollfds[NUM_POLLFDS] = {
		[EV_DISPLAY] = {
			.fd = wl_display_get_fd(term.display),
			.events = POLLIN,
		},
		[EV_TTY] = {
			.fd = term.master_fd,
			.events = POLLIN,
		},
		[EV_PASTE] = {
			.fd = term.paste.fd[0],
			.events = POLLIN,
		},
	};

	while (!term.die) {
		if (term.can_redraw && term.need_redraw && term.configured)
			redraw();

		wl_display_flush(term.display);

		pollfds[EV_PASTE].fd = term.paste.fd[0];
		n = poll(pollfds, NUM_POLLFDS, term.repeat.timeout);
		if (n < 0) {
			error("poll error");
			abort();
		}

		for (i = 0; i < NUM_POLLFDS; i++) {
			void (*f)(int) = pcb[i];

			f(pollfds[i].revents);
		}
		handle_repeat();
	}

	ret = 0;

	buffer_unmap(&term.buf[0]);
	buffer_unmap(&term.buf[1]);
	free(term.cur);
	free(term.shown);
	free(term.cur_ch);
	free(term.flags);
	if (term.cb)
		wl_callback_destroy(term.cb);

	if (term.d_d)
		wl_data_device_release(term.d_d);
	if (term.ps_d)
		zwp_primary_selection_device_v1_destroy(term.ps_d);
	if (term.ti)
		zwp_text_input_v3_destroy(term.ti);
	if (term.ptr)
		wl_pointer_release(term.ptr);
	if (term.kbd)
		wl_keyboard_release(term.kbd);

	if (term.deco.deco)
		zxdg_toplevel_decoration_v1_destroy(term.deco.deco);

	if (term.deco.manager)
		zxdg_decoration_manager_v1_destroy(term.deco.manager);

	xdg_toplevel_destroy(term.toplvl);
etoplvl:
	xdg_surface_destroy(term.xdgsurf);
exdgsurf:
	wl_surface_destroy(term.surf);
esurf:
#if HAVE_GRAPHICS
	gfx_free(term.gfx);
#endif
	tsm_vte_unref(term.vte);
evte:
	tsm_screen_unref(term.screen);
etsm:
	cursor_free();
eglobals:
	if (term.vp)
		wp_viewport_destroy(term.vp);
	if (term.vpm)
		wp_viewporter_destroy(term.vpm);
	if (term.fs)
		wp_fractional_scale_v1_destroy(term.fs);
	if (term.fsm)
		wp_fractional_scale_manager_v1_destroy(term.fsm);
	if (term.tim)
		zwp_text_input_manager_v3_destroy(term.tim);
	if (term.ps_dm)
		zwp_primary_selection_device_manager_v1_destroy(term.ps_dm);
	if (term.d_dm)
		wl_data_device_manager_destroy(term.d_dm);
	if (term.seat)
		wl_seat_destroy(term.seat);
	if (term.wm_base)
		xdg_wm_base_destroy(term.wm_base);
#ifdef HAVE_PICOWL_PALETTE
	if (term.pal)
		picowl_surface_palette_v1_destroy(term.pal);
	if (term.palm)
		picowl_palette_manager_v1_destroy(term.palm);
#endif
	if (term.shm)
		wl_shm_destroy(term.shm);
	if (term.cp)
		wl_compositor_destroy(term.cp);

	wl_registry_destroy(term.registry);
	wl_display_flush(term.display);
	wl_display_disconnect(term.display);

econnect:
	xkb_keymap_unref(term.xkb_keymap);
	xkb_state_unref(term.xkb_state);
	xkb_compose_table_unref(term.xkb_compose_table);
	xkb_compose_state_unref(term.xkb_compose_state);
	xkb_context_unref(term.xkb_ctx);
exkb:
	font_deinit();
efont:
	b = term.binding;
	while (b) {
		struct binding *tmp = b;
		b = b->next;
		free(tmp);
	}
	return ret;
}
