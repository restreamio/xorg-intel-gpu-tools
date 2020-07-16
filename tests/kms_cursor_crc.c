/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


IGT_TEST_DESCRIPTION(
   "Use the display CRC support to validate cursor plane functionality. "
   "The test will position the cursor plane either fully onscreen, "
   "partially onscreen, or fully offscreen, using either a fully opaque "
   "or fully transparent surface. In each case, it enables the cursor plane "
   "and then reads the PF CRC (hardware test) and compares it with the CRC "
   "value obtained when the cursor plane was disabled and its drawing is "
   "directly inserted on the PF by software.");

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb primary_fb[2];
	struct igt_fb fb;
	igt_output_t *output;
	enum pipe pipe;
	int left, right, top, bottom;
	int screenw, screenh;
	int refresh;
	int curw, curh; /* cursor size */
	int cursor_max_w, cursor_max_h;
	igt_pipe_crc_t *pipe_crc;
	unsigned flags;
	igt_plane_t *primary;
	igt_plane_t *cursor;
	cairo_surface_t *surface;
	uint32_t devid;
	drm_intel_bufmgr *bufmgr;
	igt_render_copyfunc_t rendercopy;
	drm_intel_bo * drmibo[2];
	struct intel_batchbuffer *batch;
	struct igt_buf igtbo[2];

} data_t;

#define TEST_DPMS (1<<0)
#define TEST_SUSPEND (1<<1)

#define FRONTBUFFER 0
#define RESTOREBUFFER 1

static void draw_cursor(cairo_t *cr, int x, int y, int cw, int ch, double a)
{
	int wl, wr, ht, hb;

	/* deal with odd cursor width/height */
	wl = cw / 2;
	wr = (cw + 1) / 2;
	ht = ch / 2;
	hb = (ch + 1) / 2;

	/* Cairo doesn't like to be fed numbers that are too wild */
	if ((x < SHRT_MIN) || (x > SHRT_MAX) || (y < SHRT_MIN) || (y > SHRT_MAX))
		return;
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
	/* 4 color rectangles in the corners, RGBY */
	igt_paint_color_alpha(cr, x,      y,      wl, ht, 1.0, 0.0, 0.0, a);
	igt_paint_color_alpha(cr, x + wl, y,      wr, ht, 0.0, 1.0, 0.0, a);
	igt_paint_color_alpha(cr, x,      y + ht, wl, hb, 0.0, 0.0, 1.0, a);
	igt_paint_color_alpha(cr, x + wl, y + ht, wr, hb, 1.0, 1.0, 1.0, a);
}

static void cursor_enable(data_t *data)
{
	igt_plane_set_fb(data->cursor, &data->fb);
	igt_plane_set_size(data->cursor, data->curw, data->curh);
	igt_fb_set_size(&data->fb, data->cursor, data->curw, data->curh);
}

static void cursor_disable(data_t *data)
{
	igt_plane_set_fb(data->cursor, NULL);
	igt_plane_set_position(data->cursor, 0, 0);
}

static bool chv_cursor_broken(data_t *data, int x)
{
	uint32_t devid;

	if (!is_i915_device(data->drm_fd))
		return false;

	devid = intel_get_drm_devid(data->drm_fd);

	/*
	 * CHV gets a FIFO underrun on pipe C when cursor x coordinate
	 * is negative and the cursor visible.
	 *
	 * i915 is fixed to return -EINVAL on cursor updates with those
	 * negative coordinates, so require cursor update to fail with
	 * -EINVAL in that case.
	 *
	 * See also kms_chv_cursor_fail.c
	 */
	if (x >= 0)
		return false;

	return IS_CHERRYVIEW(devid) && data->pipe == PIPE_C;
}

static bool cursor_visible(data_t *data, int x, int y)
{
	if (x + data->curw <= 0 || y + data->curh <= 0)
		return false;

	if (x >= data->screenw || y >= data->screenh)
		return false;

	return true;
}

static void restore_image(data_t *data)
{
	cairo_t *cr;

	if (data->rendercopy != NULL) {
		/* use rendercopy if available */
		data->rendercopy(data->batch, NULL,
				 &data->igtbo[RESTOREBUFFER], 0, 0,
				 data->primary_fb[RESTOREBUFFER].width,
				 data->primary_fb[RESTOREBUFFER].height,
				 &data->igtbo[FRONTBUFFER], 0, 0);
	} else {
		/* if rendercopy not implemented in igt use cairo */
		cr = igt_get_cairo_ctx(data->drm_fd,
				       &data->primary_fb[FRONTBUFFER]);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_surface(cr, data->surface, 0, 0);
		cairo_rectangle(cr, 0, 0, data->screenw, data->screenh);
		cairo_fill(cr);
		igt_put_cairo_ctx(cr);
	}
	igt_dirty_fb(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
}

static void do_single_test(data_t *data, int x, int y)
{
	igt_display_t *display = &data->display;
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t crc, ref_crc;
	cairo_t *cr;
	int ret = 0;

	igt_print_activity();

	/* Hardware test */
	restore_image(data);

	igt_plane_set_position(data->cursor, x, y);
	cursor_enable(data);

	if (chv_cursor_broken(data, x) && cursor_visible(data, x, y)) {
		ret = igt_display_try_commit2(display, COMMIT_LEGACY);
		igt_assert_eq(ret, -EINVAL);
		igt_plane_set_position(data->cursor, 0, y);

		return;
	}

	igt_display_commit(display);

	/* Extra vblank wait is because nonblocking cursor ioctl */
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc);

	if (data->flags & (TEST_DPMS | TEST_SUSPEND)) {
		igt_crc_t crc_after;
		/*
		 * stop/start crc to avoid dmesg notifications about userspace
		 * reading too slow.
		 */
		igt_pipe_crc_stop(pipe_crc);

		if (data->flags & TEST_DPMS) {
			igt_debug("dpms off/on cycle\n");
			kmstest_set_connector_dpms(data->drm_fd,
						   data->output->config.connector,
						   DRM_MODE_DPMS_OFF);
			kmstest_set_connector_dpms(data->drm_fd,
						   data->output->config.connector,
						   DRM_MODE_DPMS_ON);
		}

		if (data->flags & TEST_SUSPEND)
			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);

		igt_pipe_crc_start(pipe_crc);
		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc_after);
		igt_assert_crc_equal(&crc, &crc_after);
	}

	cursor_disable(data);

	/* Now render the same in software and collect crc */
	cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
	draw_cursor(cr, x, y, data->curw, data->curh, 1.0);
	igt_put_cairo_ctx(cr);
	igt_display_commit(display);
	igt_dirty_fb(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
	/* Extra vblank wait is because nonblocking cursor ioctl */
	igt_wait_for_vblank(data->drm_fd, data->pipe);

	igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &ref_crc);
	igt_assert_crc_equal(&crc, &ref_crc);
}

static void do_fail_test(data_t *data, int x, int y, int expect)
{
	igt_display_t *display = &data->display;
	int ret;

	igt_print_activity();

	/* Hardware test */
	restore_image(data);

	cursor_enable(data);
	igt_plane_set_position(data->cursor, x, y);
	ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	igt_plane_set_position(data->cursor, 0, 0);
	cursor_disable(data);
	igt_display_commit(display);

	igt_assert_eq(ret, expect);
}

static void do_test(data_t *data,
		    int left, int right, int top, int bottom)
{
	do_single_test(data, left, top);
	do_single_test(data, right, top);
	do_single_test(data, right, bottom);
	do_single_test(data, left, bottom);
}

static void test_crc_onscreen(data_t *data)
{
	int left = data->left;
	int right = data->right;
	int top = data->top;
	int bottom = data->bottom;
	int cursor_w = data->curw;
	int cursor_h = data->curh;

	/* fully inside  */
	do_test(data, left, right, top, bottom);

	/* 2 pixels inside */
	do_test(data, left - (cursor_w-2), right + (cursor_w-2), top               , bottom               );
	do_test(data, left               , right               , top - (cursor_h-2), bottom + (cursor_h-2));
	do_test(data, left - (cursor_w-2), right + (cursor_w-2), top - (cursor_h-2), bottom + (cursor_h-2));

	/* 1 pixel inside */
	do_test(data, left - (cursor_w-1), right + (cursor_w-1), top               , bottom               );
	do_test(data, left               , right               , top - (cursor_h-1), bottom + (cursor_h-1));
	do_test(data, left - (cursor_w-1), right + (cursor_w-1), top - (cursor_h-1), bottom + (cursor_h-1));
}

static void test_crc_offscreen(data_t *data)
{
	int left = data->left;
	int right = data->right;
	int top = data->top;
	int bottom = data->bottom;
	int cursor_w = data->curw;
	int cursor_h = data->curh;

	/* fully outside */
	do_test(data, left - (cursor_w), right + (cursor_w), top             , bottom             );
	do_test(data, left             , right             , top - (cursor_h), bottom + (cursor_h));
	do_test(data, left - (cursor_w), right + (cursor_w), top - (cursor_h), bottom + (cursor_h));

	/* fully outside by 1 extra pixels */
	do_test(data, left - (cursor_w+1), right + (cursor_w+1), top               , bottom               );
	do_test(data, left               , right               , top - (cursor_h+1), bottom + (cursor_h+1));
	do_test(data, left - (cursor_w+1), right + (cursor_w+1), top - (cursor_h+1), bottom + (cursor_h+1));

	/* fully outside by 2 extra pixels */
	do_test(data, left - (cursor_w+2), right + (cursor_w+2), top               , bottom               );
	do_test(data, left               , right               , top - (cursor_h+2), bottom + (cursor_h+2));
	do_test(data, left - (cursor_w+2), right + (cursor_w+2), top - (cursor_h+2), bottom + (cursor_h+2));

	/* fully outside by a lot of extra pixels */
	do_test(data, left - (cursor_w+512), right + (cursor_w+512), top                 , bottom                 );
	do_test(data, left                 , right                 , top - (cursor_h+512), bottom + (cursor_h+512));
	do_test(data, left - (cursor_w+512), right + (cursor_w+512), top - (cursor_h+512), bottom + (cursor_h+512));

	/* go nuts */
	do_test(data, INT_MIN, INT_MAX - cursor_w, INT_MIN, INT_MAX - cursor_h);
	do_test(data, SHRT_MIN, SHRT_MAX, SHRT_MIN, SHRT_MAX);

	/* Make sure we get -ERANGE on integer overflow */
	do_fail_test(data, INT_MAX - cursor_w + 1, INT_MAX - cursor_h + 1, -ERANGE);
}

static void test_crc_sliding(data_t *data)
{
	int i;

	/* Make sure cursor moves smoothly and pixel-by-pixel, and that there are
	 * no alignment issues. Horizontal, vertical and diagonal test.
	 */
	for (i = 0; i < 16; i++) {
		do_single_test(data, i, 0);
		do_single_test(data, 0, i);
		do_single_test(data, i, i);
	}
}

static void test_crc_random(data_t *data)
{
	int i, max;

	max = data->flags & (TEST_DPMS | TEST_SUSPEND) ? 2 : 50;

	/* Random cursor placement */
	for (i = 0; i < max; i++) {
		int x = rand() % (data->screenw + data->curw * 2) - data->curw;
		int y = rand() % (data->screenh + data->curh * 2) - data->curh;
		do_single_test(data, x, y);
	}
}

static void cleanup_crtc(data_t *data)
{
	igt_display_t *display = &data->display;

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	cairo_surface_destroy(data->surface);

	igt_plane_set_fb(data->primary, NULL);
	igt_display_commit(display);

	igt_remove_fb(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
	igt_remove_fb(data->drm_fd, &data->primary_fb[RESTOREBUFFER]);

	igt_display_reset(display);
}

static void scratch_buf_init(data_t *data, int buffer)
{
	data->igtbo[buffer].bo = data->drmibo[buffer];
	data->igtbo[buffer].surface[0].stride = data->primary_fb[buffer].strides[0];
	data->igtbo[buffer].tiling = data->primary_fb[buffer].modifier;
	data->igtbo[buffer].surface[0].size = data->primary_fb[buffer].size;
	data->igtbo[buffer].bpp = data->primary_fb[buffer].plane_bpp[0];
}

static void prepare_crtc(data_t *data, igt_output_t *output,
			 int cursor_w, int cursor_h)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	cairo_t *cr;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, data->pipe);

	/* create and set the primary plane fbs */
	mode = igt_output_get_mode(output);
	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &data->primary_fb[FRONTBUFFER]);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &data->primary_fb[RESTOREBUFFER]);

	data->primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	data->cursor = igt_output_get_plane_type(output, DRM_PLANE_TYPE_CURSOR);

	igt_plane_set_fb(data->primary, &data->primary_fb[FRONTBUFFER]);

	igt_display_commit(display);

	/* create the pipe_crc object for this pipe */
	if (data->pipe_crc)
		igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
					  INTEL_PIPE_CRC_SOURCE_AUTO);

	/* x/y position where the cursor is still fully visible */
	data->left = 0;
	data->right = mode->hdisplay - cursor_w;
	data->top = 0;
	data->bottom = mode->vdisplay - cursor_h;
	data->screenw = mode->hdisplay;
	data->screenh = mode->vdisplay;
	data->curw = cursor_w;
	data->curh = cursor_h;
	data->refresh = mode->vrefresh;

	data->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, data->screenw, data->screenh);

	if (data->rendercopy == NULL) {
		/* store test image as cairo surface */
		cr = cairo_create(data->surface);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		igt_paint_test_pattern(cr, data->screenw, data->screenh);
		cairo_destroy(cr);
	} else {
		/* store test image as fb if rendercopy is available */
		cr = igt_get_cairo_ctx(data->drm_fd,
		                       &data->primary_fb[RESTOREBUFFER]);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		igt_paint_test_pattern(cr, data->screenw, data->screenh);
		igt_put_cairo_ctx(cr);

		data->drmibo[FRONTBUFFER] = gem_handle_to_libdrm_bo(data->bufmgr,
								    data->drm_fd,
								    "", data->primary_fb[FRONTBUFFER].gem_handle);
		igt_assert(data->drmibo[FRONTBUFFER]);

		data->drmibo[RESTOREBUFFER] = gem_handle_to_libdrm_bo(data->bufmgr,
								      data->drm_fd,
								      "", data->primary_fb[RESTOREBUFFER].gem_handle);
		igt_assert(data->drmibo[RESTOREBUFFER]);

		scratch_buf_init(data, RESTOREBUFFER);
		scratch_buf_init(data, FRONTBUFFER);

		data->batch = intel_batchbuffer_alloc(data->bufmgr,
						      data->devid);
		igt_assert(data->batch);
	}

	igt_pipe_crc_start(data->pipe_crc);
}

static void test_cursor_alpha(data_t *data, double a)
{
	igt_display_t *display = &data->display;
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t crc, ref_crc;
	cairo_t *cr;
	uint32_t fb_id;
	int curw = data->curw;
	int curh = data->curh;

	/* Alpha cursor fb with white color */
	fb_id = igt_create_fb(data->drm_fd, curw, curh,
				    DRM_FORMAT_ARGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    &data->fb);
	igt_assert(fb_id);
	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb);
	igt_paint_color_alpha(cr, 0, 0, curw, curh, 1.0, 1.0, 1.0, a);
	igt_put_cairo_ctx(cr);

	/* Hardware Test - enable cursor and get PF CRC */
	cursor_enable(data);
	igt_display_commit(display);
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc);

	cursor_disable(data);
	igt_remove_fb(data->drm_fd, &data->fb);

	/* Software Test - render cursor in software, drawn it directly on PF */
	cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
	igt_paint_color_alpha(cr, 0, 0, curw, curh, 1.0, 1.0, 1.0, a);
	igt_put_cairo_ctx(cr);

	igt_display_commit(display);
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &ref_crc);

	/* Compare CRC from Hardware/Software tests */
	igt_assert_crc_equal(&crc, &ref_crc);

	/*Clear Screen*/
	cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
	igt_paint_color(cr, 0, 0, data->screenw, data->screenh,
			0.0, 0.0, 0.0);
	igt_put_cairo_ctx(cr);
}

static void test_cursor_transparent(data_t *data)
{
	test_cursor_alpha(data, 0.0);

}

static void test_cursor_opaque(data_t *data)
{
	test_cursor_alpha(data, 1.0);
}

static void run_test(data_t *data, void (*testfunc)(data_t *), int cursor_w, int cursor_h)
{
	prepare_crtc(data, data->output, cursor_w, cursor_h);
	testfunc(data);
	cleanup_crtc(data);
}

static void create_cursor_fb(data_t *data, int cur_w, int cur_h)
{
	cairo_t *cr;
	uint32_t fb_id;

	/*
	 * Make the FB slightly taller and leave the extra
	 * line opaque white, so that we can see that the
	 * hardware won't scan beyond what it should (esp.
	 * with non-square cursors).
	 */
	fb_id = igt_create_color_fb(data->drm_fd, cur_w, cur_h + 1,
				    DRM_FORMAT_ARGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    1.0, 1.0, 1.0,
				    &data->fb);

	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb);
	draw_cursor(cr, 0, 0, cur_w, cur_h, 1.0);
	igt_put_cairo_ctx(cr);
}

static bool has_nonsquare_cursors(data_t *data)
{
	uint32_t devid;

	if (!is_i915_device(data->drm_fd))
		return false;

	devid = intel_get_drm_devid(data->drm_fd);

	/*
	 * Test non-square cursors a bit on the platforms
	 * that support such things.
	 */
	if (devid == PCI_CHIP_845_G || devid == PCI_CHIP_I865_G)
		return true;

	if (IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid))
		return false;

	return intel_gen(devid) >= 7;
}

static void test_cursor_size(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t crc[10], ref_crc;
	cairo_t *cr;
	uint32_t fb_id;
	int i, size;
	int cursor_max_size = data->cursor_max_w;

	/* Create a maximum size cursor, then change the size in flight to
	 * smaller ones to see that the size is applied correctly
	 */
	fb_id = igt_create_fb(data->drm_fd, cursor_max_size, cursor_max_size,
			      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			      &data->fb);
	igt_assert(fb_id);

	/* Use a solid white rectangle as the cursor */
	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb);
	igt_paint_color_alpha(cr, 0, 0, cursor_max_size, cursor_max_size, 1.0, 1.0, 1.0, 1.0);
	igt_put_cairo_ctx(cr);

	/* Hardware test loop */
	cursor_enable(data);
	for (i = 0, size = cursor_max_size; size >= 64; size /= 2, i++) {
		/* Change size in flight: */
		igt_plane_set_size(data->cursor, size, size);
		igt_fb_set_size(&data->fb, data->cursor, size, size);
		igt_display_commit(display);
		igt_wait_for_vblank(data->drm_fd, data->pipe);
		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &crc[i]);
	}
	cursor_disable(data);
	igt_display_commit(display);
	igt_remove_fb(data->drm_fd, &data->fb);
	/* Software test loop */
	for (i = 0, size = cursor_max_size; size >= 64; size /= 2, i++) {
		/* Now render the same in software and collect crc */
		cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
		igt_paint_color_alpha(cr, 0, 0, size, size, 1.0, 1.0, 1.0, 1.0);
		igt_put_cairo_ctx(cr);

		igt_display_commit(display);
		igt_wait_for_vblank(data->drm_fd, data->pipe);
		igt_pipe_crc_get_current(data->drm_fd, pipe_crc, &ref_crc);
		/* Clear screen afterwards */
		cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb[FRONTBUFFER]);
		igt_paint_color(cr, 0, 0, data->screenw, data->screenh,
				0.0, 0.0, 0.0);
		igt_put_cairo_ctx(cr);
		igt_assert_crc_equal(&crc[i], &ref_crc);
	}
}

static void test_rapid_movement(data_t *data)
{
	struct timeval start, end, delta;
	int x = 0, y = 0;
	long usec;
	igt_display_t *display = &data->display;

	cursor_enable(data);

	gettimeofday(&start, NULL);
	for ( ; x < 100; x++) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; y < 100; y++) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; x > 0; x--) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	for ( ; y > 0; y--) {
		igt_plane_set_position(data->cursor, x, y);
		igt_display_commit(display);
	}
	gettimeofday(&end, NULL);

	/*
	 * We've done 400 cursor updates now.  If we're being throttled to
	 * vblank, then that would take roughly 400/refresh seconds.  If the
	 * elapsed time is greater than 90% of that value, we'll consider it
	 * a failure (since cursor updates shouldn't be throttled).
	 */
	timersub(&end, &start, &delta);
	usec = delta.tv_usec + 1000000 * delta.tv_sec;
	igt_assert_lt(usec, 0.9 * 400 * 1000000 / data->refresh);
}

static void run_tests_on_pipe(data_t *data, enum pipe pipe)
{
	int cursor_size;

	igt_fixture {
		data->pipe = pipe;
		data->output = igt_get_single_output_for_pipe(&data->display, pipe);
		igt_require(data->output);
	}

	igt_describe("Create a maximum size cursor, then change the size in "
		     "flight to smaller ones to see that the size is applied "
		     "correctly.");
	igt_subtest_f("pipe-%s-cursor-size-change", kmstest_pipe_name(pipe))
		run_test(data, test_cursor_size,
			 data->cursor_max_w, data->cursor_max_h);

	igt_describe("Validates the composition of a fully opaque cursor "
		     "plane, i.e., alpha channel equal to 1.0.");
	igt_subtest_f("pipe-%s-cursor-alpha-opaque", kmstest_pipe_name(pipe))
		run_test(data, test_cursor_opaque, data->cursor_max_w, data->cursor_max_h);

	igt_describe("Validates the composition of a fully transparent cursor "
		     "plane, i.e., alpha channel equal to 0.0.");
	igt_subtest_f("pipe-%s-cursor-alpha-transparent", kmstest_pipe_name(pipe))
		run_test(data, test_cursor_transparent, data->cursor_max_w, data->cursor_max_h);

	igt_fixture
		create_cursor_fb(data, data->cursor_max_w, data->cursor_max_h);

	igt_subtest_f("pipe-%s-cursor-dpms", kmstest_pipe_name(pipe)) {
		data->flags = TEST_DPMS;
		run_test(data, test_crc_random, data->cursor_max_w, data->cursor_max_h);
	}
	data->flags = 0;

	igt_subtest_f("pipe-%s-cursor-suspend", kmstest_pipe_name(pipe)) {
		data->flags = TEST_SUSPEND;
		run_test(data, test_crc_random, data->cursor_max_w, data->cursor_max_h);
	}
	data->flags = 0;

	igt_fixture
		igt_remove_fb(data->drm_fd, &data->fb);

	for (cursor_size = 64; cursor_size <= 512; cursor_size *= 2) {
		int w = cursor_size;
		int h = cursor_size;

		igt_fixture {
			igt_require(w <= data->cursor_max_w &&
				    h <= data->cursor_max_h);

			create_cursor_fb(data, w, h);
		}

		/* Using created cursor FBs to test cursor support */
		igt_describe("Check if a given-size cursor is well-positioned inside the screen.");
		igt_subtest_f("pipe-%s-cursor-%dx%d-onscreen", kmstest_pipe_name(pipe), w, h)
			run_test(data, test_crc_onscreen, w, h);

		igt_describe("Check if a given-size cursor is well-positioned outside the screen.");
		igt_subtest_f("pipe-%s-cursor-%dx%d-offscreen", kmstest_pipe_name(pipe), w, h)
			run_test(data, test_crc_offscreen, w, h);

		igt_describe("Check the smooth and pixel-by-pixel given-size cursor movements on"
		             "horizontal, vertical and diagonal.");
		igt_subtest_f("pipe-%s-cursor-%dx%d-sliding", kmstest_pipe_name(pipe), w, h)
			run_test(data, test_crc_sliding, w, h);

		igt_describe("Check random placement of a cursor with given size.");
		igt_subtest_f("pipe-%s-cursor-%dx%d-random", kmstest_pipe_name(pipe), w, h)
			run_test(data, test_crc_random, w, h);

		igt_describe("Check the rapid update of given-size cursor movements.");
		igt_subtest_f("pipe-%s-cursor-%dx%d-rapid-movement", kmstest_pipe_name(pipe), w, h) {
			run_test(data, test_rapid_movement, w, h);
		}

		igt_fixture
			igt_remove_fb(data->drm_fd, &data->fb);

		/*
		 * Test non-square cursors a bit on the platforms
		 * that support such things. And make it a bit more
		 * interesting by using a non-pot height.
		 */
		h /= 3;

		igt_fixture {
			if (has_nonsquare_cursors(data))
				create_cursor_fb(data, w, h);
		}

		/* Using created cursor FBs to test cursor support */
		igt_subtest_f("pipe-%s-cursor-%dx%d-onscreen", kmstest_pipe_name(pipe), w, h) {
			igt_require(has_nonsquare_cursors(data));
			run_test(data, test_crc_onscreen, w, h);
		}
		igt_subtest_f("pipe-%s-cursor-%dx%d-offscreen", kmstest_pipe_name(pipe), w, h) {
			igt_require(has_nonsquare_cursors(data));
			run_test(data, test_crc_offscreen, w, h);
		}
		igt_subtest_f("pipe-%s-cursor-%dx%d-sliding", kmstest_pipe_name(pipe), w, h) {
			igt_require(has_nonsquare_cursors(data));
			run_test(data, test_crc_sliding, w, h);
		}
		igt_subtest_f("pipe-%s-cursor-%dx%d-random", kmstest_pipe_name(pipe), w, h) {
			igt_require(has_nonsquare_cursors(data));
			run_test(data, test_crc_random, w, h);
		}

		igt_fixture
			igt_remove_fb(data->drm_fd, &data->fb);
	}
}

static data_t data;

igt_main
{
	uint64_t cursor_width = 64, cursor_height = 64;
	int ret;
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width);
		igt_assert(ret == 0 || errno == EINVAL);
		/* Not making use of cursor_height since it is same as width, still reading */
		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height);
		igt_assert(ret == 0 || errno == EINVAL);

		/* We assume width and height are same so max is assigned width */
		igt_assert_eq(cursor_width, cursor_height);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);

		igt_display_require(&data.display, data.drm_fd);

		if (is_i915_device(data.drm_fd)) {
			data.bufmgr = drm_intel_bufmgr_gem_init(data.drm_fd, 4096);
			igt_assert(data.bufmgr);
			drm_intel_bufmgr_gem_enable_reuse(data.bufmgr);

			data.devid = intel_get_drm_devid(data.drm_fd);
			data.rendercopy = igt_get_render_copyfunc(data.devid);
		}
		igt_debug("Using %s for restoring test image\n", (data.rendercopy == NULL)?"Cairo":"rendercopy");
	}

	data.cursor_max_w = cursor_width;
	data.cursor_max_h = cursor_height;

	for_each_pipe_static(pipe)
		igt_subtest_group
			run_tests_on_pipe(&data, pipe);

	igt_fixture {
		if (data.pipe_crc != NULL) {
			igt_pipe_crc_stop(data.pipe_crc);
			igt_pipe_crc_free(data.pipe_crc);
		}

		if (data.bufmgr != NULL) {
			intel_batchbuffer_free(data.batch);
			drm_intel_bufmgr_destroy(data.bufmgr);
		}

		igt_display_fini(&data.display);
	}
}
