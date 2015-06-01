/*
 * Copyright © 2013 Siarhei Siamashka <siarhei.siamashka@gmail.com>
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>

#include "sunxi_disp.h"
#include "sunxi_disp_ioctl.h"
#include "g2d_driver.h"
#include "sunxi_ion.h"


/*****************************************************************************/

sunxi_disp_t *sunxi_disp_init(const char *device, void *xserver_fbmem)
{
	sunxi_disp_t *ctx = calloc(sizeof(sunxi_disp_t), 1);
	struct fb_var_screeninfo fb_var;
	struct fb_fix_screeninfo fb_fix;

	int tmp, version;
	int gfx_layer_size;
	int ovl_layer_size;

	if (strcmp(device, "/dev/fb0") == 0) {
		ctx->fb_id = 0;
	}
	else if (strcmp(device, "/dev/fb1") == 0) {
		ctx->fb_id = 1;
	}
	else
	{
		free(ctx);
		return NULL;
	}
		ctx->fb_id = 1;

	/* store the already existing mapping done by xserver */
	ctx->xserver_fbmem = xserver_fbmem;

	ctx->fd_disp = open("/dev/disp", O_RDWR);

	/* maybe it's even not a sunxi hardware */
	if (ctx->fd_disp < 0) {
		free(ctx);
		return NULL;
	}

	/* version check */
	//tmp = SUNXI_DISP_VERSION;
	//version = ioctl(ctx->fd_disp, DISP_CMD_VERSION, &tmp);
	//if (version < 0) {
	//    close(ctx->fd_disp);
	//    free(ctx);
	//    return NULL;
	//}

	ctx->fd_fb = open(device, O_RDWR);
	if (ctx->fd_fb < 0) {
		close(ctx->fd_disp);
		free(ctx);
		return NULL;
	}

	if (ioctl(ctx->fd_fb, FBIOGET_VSCREENINFO, &fb_var) < 0 ||
			ioctl(ctx->fd_fb, FBIOGET_FSCREENINFO, &fb_fix) < 0)
	{
		close(ctx->fd_fb);
		close(ctx->fd_disp);
		free(ctx);
		return NULL;
	}

	ctx->xres = fb_var.xres;
	ctx->yres = fb_var.yres;
	ctx->bits_per_pixel = fb_var.bits_per_pixel;
	ctx->framebuffer_paddr = fb_fix.smem_start;
	ctx->framebuffer_size = fb_fix.smem_len;
	ctx->framebuffer_height = ctx->framebuffer_size /
		(ctx->xres * ctx->bits_per_pixel / 8);
	ctx->gfx_layer_size = ctx->xres * ctx->yres * fb_var.bits_per_pixel / 8;

	if (ctx->framebuffer_size < ctx->gfx_layer_size) {
		close(ctx->fd_fb);
		close(ctx->fd_disp);
		free(ctx);
		return NULL;
	}

	if (ctx->xserver_fbmem) {
		/* use already existing mapping */
		ctx->framebuffer_addr = ctx->xserver_fbmem;
	}
	else {
		/* mmap framebuffer memory */
		ctx->framebuffer_addr = (uint8_t *)mmap(0, ctx->framebuffer_size,
				PROT_READ | PROT_WRITE,
				MAP_SHARED, ctx->fd_fb, 0);
		if (ctx->framebuffer_addr == MAP_FAILED) {
			close(ctx->fd_fb);
			close(ctx->fd_disp);
			free(ctx);
			return NULL;
		}
	}

	ctx->cursor_enabled = 0;
	ctx->cursor_x = -1;
	ctx->cursor_y = -1;

	/* Get the id of the screen layer */
	//if (ioctl(ctx->fd_fb,
	//          ctx->fb_id == 0 ? FBIOGET_LAYER_HDL_0 : FBIOGET_LAYER_HDL_1,
	//          &ctx->gfx_layer_id))
	//{
	//    close(ctx->fd_fb);
	//    close(ctx->fd_disp);
	//    free(ctx);
	//    return NULL;
	//}
	int ret;
	uint32_t vga_args[4];
	vga_args[0] = 0;
        ret = ioctl(ctx->fd_disp, DISP_CMD_IS_VGA_USED,&vga_args);
	if(ret == 1){
		 ctx->fb_id = 0;
	}
	else if (ret < 0){
			close(ctx->fd_fb);
			close(ctx->fd_disp);
			free(ctx);
			return NULL;
	}
	ctx->fd_g2d = open("/dev/g2d", O_RDWR);

	ctx->blt2d.self = ctx;
	ctx->blt2d.overlapped_blt = sunxi_g2d_blt;

	/* use layer 1 for screen */
	uint32_t args[4];
	disp_layer_info layer_win;
	ctx->gfx_layer_id = 1;
	ctx->layer_id = 0;
	args[0] = ctx->fb_id;
	args[1] = 0;
	args[2] = (uintptr_t)&layer_win;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &args) < 0)
		return NULL;

	args[0] = ctx->fb_id;
	args[1] = 1;
	args[2] = (uintptr_t)&layer_win;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &args) < 0)
		return NULL;
	ioctl(ctx->fd_disp, DISP_CMD_LAYER_ENABLE, &args);
	args[1] = 0;
	ioctl(ctx->fd_disp, DISP_CMD_LAYER_DISABLE, &args);

	if (sunxi_layer_reserve(ctx) < 0)
	{
		close(ctx->fd_fb);
		close(ctx->fd_disp);
		free(ctx);
		return NULL;
	}

	ctx->handle_data.handle = sunxi_ion_alloc(ctx, ctx->framebuffer_size);



	return ctx;
}

int sunxi_disp_close(sunxi_disp_t *ctx)
{
	if (ctx->fd_disp >= 0) {
		if (ctx->fd_g2d >= 0) {
			close(ctx->fd_g2d);
		}
		/* release layer */
		sunxi_layer_release(ctx);
		uint32_t tmp[4];
		disp_layer_info layer_win;
		ctx->gfx_layer_id = 0;
		ctx->layer_id = 1;
		tmp[0] = ctx->fb_id;
		tmp[1] = 1;
		tmp[2] = (uintptr_t)&layer_win;
		if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &tmp) < 0)
			return -1;
		tmp[0] = ctx->fb_id;
		tmp[1] = 0;
		tmp[2] = (uintptr_t)&layer_win;
		if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp) < 0)
		ioctl(ctx->fd_disp, DISP_CMD_LAYER_ENABLE, &tmp);
			return -1;
		/* disable cursor */
		if (ctx->cursor_enabled)
			sunxi_hw_cursor_hide(ctx);
		/* close descriptors */
		if (!ctx->xserver_fbmem)
			munmap(ctx->framebuffer_addr, ctx->framebuffer_size);
		close(ctx->fd_fb);
		close(ctx->fd_disp);
		ctx->fd_disp = -1;
		if(!ctx->buffer_addr)
			sunxi_ion_free(ctx->handle_data.handle);
		free(ctx);
	}
	return 0;
}

/*****************************************************************************
 * Support for hardware cursor, which has 64x64 size, 2 bits per pixel,      *
 * four 32-bit ARGB entries in the palette.                                  *
 *****************************************************************************/

int sunxi_hw_cursor_load_64x64x2bpp(sunxi_disp_t *ctx, uint8_t pixeldata[1024])
{
	uint32_t tmp[4];
	disp_cursor_fb hwc;
	hwc.addr = (uintptr_t)&pixeldata[0];
	hwc.mode = DISP_HWC_MOD_64X64_2BPP;
	tmp[0] = ctx->fb_id;
	tmp[1] = (uintptr_t)&hwc;
	return ioctl(ctx->fd_disp, DISP_CMD_CURSOR_SET_FB, &tmp);
}

int sunxi_hw_cursor_load_32x32x8bpp(sunxi_disp_t *ctx, uint8_t pixeldata[1024])
{
	uint32_t tmp[4];
	disp_cursor_fb hwc;
	hwc.addr = (uintptr_t)&pixeldata[0];
	hwc.mode = DISP_HWC_MOD_32X32_8BPP;
	tmp[0] = ctx->fb_id;
	tmp[1] = (uintptr_t)&hwc;
	return ioctl(ctx->fd_disp, DISP_CMD_CURSOR_SET_FB, &tmp);
}

int sunxi_hw_cursor_load_palette(sunxi_disp_t *ctx, uint32_t *palette, int n)
{
	uint32_t tmp[4];
	tmp[0] = ctx->fb_id;
	tmp[1] = (uintptr_t)palette;
	tmp[2] = 0;
	tmp[3] = n * sizeof(uint32_t);
	return ioctl(ctx->fd_disp, DISP_CMD_CURSOR_SET_PALETTE, &tmp);
}

int sunxi_hw_cursor_set_position(sunxi_disp_t *ctx, int x, int y)
{
	int result;
	uint32_t tmp[4];
	disp_position pos = { x, y };
	tmp[0] = ctx->fb_id;
	tmp[1] = (uintptr_t)&pos;
	if (pos.x < 0)
		pos.x = 0;
	if (pos.y < 0)
		pos.y = 0;
	result = ioctl(ctx->fd_disp, DISP_CMD_CURSOR_SET_POS, &tmp);
	if (result >= 0) {
		ctx->cursor_x = pos.x;
		ctx->cursor_y = pos.y;
	}
	return result;
}

int sunxi_hw_cursor_show(sunxi_disp_t *ctx)
{
	int result;
	uint32_t tmp[4];

	tmp[0] = ctx->fb_id;
	result = ioctl(ctx->fd_disp, DISP_CMD_CURSOR_ENABLE, &tmp);
	if (result >= 0)
		ctx->cursor_enabled = 1;

	return result;
}

int sunxi_hw_cursor_hide(sunxi_disp_t *ctx)
{
	int result;
	uint32_t tmp[4];
	tmp[0] = ctx->fb_id;
	result = ioctl(ctx->fd_disp, DISP_CMD_CURSOR_DISABLE, &tmp);
	if (result >= 0)
		ctx->cursor_enabled = 0;
	return result;
}

/*****************************************************************************
 * Support for scaled layers                                                 *
 *****************************************************************************/

static int sunxi_layer_change_work_mode(sunxi_disp_t *ctx, int new_mode)
{
	disp_layer_info layer_info;
	uint32_t tmp[4];

	if (ctx->layer_id < 0)
		return -1;

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, tmp) < 0)
		return -1;

	layer_info.mode = new_mode;

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, tmp);
}

int sunxi_layer_reserve(sunxi_disp_t *ctx)
{
	disp_layer_info layer_info;
	uint32_t tmp[4];

	/* try to allocate a layer */
	/* ctx->layer_id = 1; //hcl:use layer 0 for video */
	/* On the A80 platform,no need request a layer, just GET/SET a layer info */
#if 0
	tmp[0] = ctx->fb_id;
	tmp[1] = DISP_LAYER_WORK_MODE_NORMAL;
	ctx->layer_id = ioctl(ctx->fd_disp, DISP_CMD_LAYER_REQUEST, &tmp);
	if (ctx->layer_id < 0)
		return -1;
#endif
	/* Initially set the layer configuration to something reasonable */

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, tmp) < 0)
		return -1;

	/* the screen and overlay layers need to be in different pipes */
	layer_info.pipe      = 1;
	layer_info.alpha_mode  = 1;
	layer_info.alpha_value = 255;

	layer_info.fb.addr[0] = ctx->framebuffer_paddr;
	layer_info.fb.size.width = 1;
	layer_info.fb.size.height = 1;
	layer_info.fb.format = DISP_FORMAT_ARGB_8888;

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, tmp) < 0)
		return -1;

	/* Now probe the scaler mode to see if there is a free scaler available */
	if (sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_SCALER) == 0)
		ctx->layer_has_scaler = 1;

	/* Revert back to normal mode */
	sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_NORMAL);
	ctx->layer_scaler_is_enabled = 0;
	ctx->layer_format = DISP_FORMAT_ARGB_8888;

	return ctx->layer_id;
}


int sunxi_layer_release(sunxi_disp_t *ctx)
{
	int result;
	uint32_t tmp[4];

	if (ctx->layer_id < 0)
		return -1;

	//    tmp[0] = ctx->fb_id;
	//    tmp[1] = ctx->layer_id;
	//    ioctl(ctx->fd_disp, DISP_CMD_LAYER_RELEASE, &tmp);

	ctx->layer_id = -1;
	ctx->layer_has_scaler = 0;
	return 0;
}

int sunxi_layer_set_rgb_input_buffer(sunxi_disp_t *ctx,
		int           bpp,
		uint32_t      offset_in_framebuffer,
		int           width,
		int           height,
		int           stride)
{
	disp_fb_info fb;
	disp_layer_info layer_info;
	disp_window rect = { 0, 0, width, height };
	uint32_t tmp[4];
	memset(&fb, 0, sizeof(fb));
	memset(&layer_info, 0, sizeof(layer_info));

	if (ctx->layer_id < 0)
		return -1;

	if (ctx->layer_scaler_is_enabled) {
		if (sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_NORMAL) == 0)
			ctx->layer_scaler_is_enabled = 0;
		else
			return -1;
	}
	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &tmp) < 0)
		return -1;

	layer_info.fb.addr[0] = ctx->framebuffer_paddr + offset_in_framebuffer;
	layer_info.fb.size.width = stride;
	layer_info.fb.size.height = height;
	if (bpp == 32) {
		layer_info.fb.format = DISP_FORMAT_ARGB_8888;
	} else if (bpp == 16) {
		layer_info.fb.format = DISP_FORMAT_RGB_565;
		layer_info.fb.size.width = stride * 2;
	} else {
		return -1;
	}
	layer_info.fb.src_win.x = rect.x;
	layer_info.fb.src_win.y = rect.y;
	layer_info.fb.src_win.width = rect.width;
	layer_info.fb.src_win.height = rect.height;

	return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp);
}

int sunxi_layer_set_yuv420_input_buffer(sunxi_disp_t *ctx,
		uint32_t      y_offset_in_framebuffer,
		uint32_t      u_offset_in_framebuffer,
		uint32_t      v_offset_in_framebuffer,
		int           width,
		int           height,
		int           stride,
		int           x_pixel_offset,
		int           y_pixel_offset)
{
	disp_fb_info fb;
	disp_layer_info layer_info;
	disp_window rect = { x_pixel_offset, y_pixel_offset, width, height };
	uint32_t tmp[4];
	memset(&fb, 0, sizeof(fb));
	memset(&layer_info, 0, sizeof(layer_info));

	if (ctx->layer_id < 0)
		return -1;

	if (!ctx->layer_scaler_is_enabled) {
		if (sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_SCALER) == 0)
			ctx->layer_scaler_is_enabled = 1;
		else
			return -1;
	}

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &tmp) < 0)
		return -1;

	layer_info.fb.addr[0] = ctx->framebuffer_paddr + y_offset_in_framebuffer;
	layer_info.fb.addr[1] = ctx->framebuffer_paddr + u_offset_in_framebuffer;
	layer_info.fb.addr[2] = ctx->framebuffer_paddr + v_offset_in_framebuffer;
	layer_info.fb.size.width = stride;
	layer_info.fb.size.height = height;
	layer_info.fb.format = DISP_FORMAT_YUV420_P;
	layer_info.fb.src_win.x = rect.x;
	layer_info.fb.src_win.y = rect.y;
	layer_info.fb.src_win.width = rect.width;
	layer_info.fb.src_win.height = rect.height;

	return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp);

}

int sunxi_layer_set_output_window(sunxi_disp_t *ctx, int x, int y, int w, int h)
{
	disp_window buf_rect = {
		ctx->layer_buf_x, ctx->layer_buf_y,
		ctx->layer_buf_w, ctx->layer_buf_h
	};
	disp_layer_info layer_info;
	disp_window win_rect = { x, y, w, h };
	uint32_t tmp[4];
	int err;

	memset(&layer_info, 0, sizeof(layer_info));

	if (ctx->layer_id < 0 || w <= 0 || h <= 0)
		return -1;

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_info;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &tmp) < 0)
		return -1;
	/*
	 * Handle negative window Y coordinates (workaround a bug).
	 * The Allwinner A10/A13 display controller hardware is expected to
	 * support negative coordinates of the top left corners of the layers.
	 * But there is some bug either in the kernel driver or in the hardware,
	 * which messes up the picture on screen when the Y coordinate is negative
	 * for YUV layer. Negative X coordinates are not affected. RGB formats
	 * are not affected too.
	 *
	 * We fix this by just recalculating which part of the buffer in memory
	 * corresponds to Y=0 on screen and adjust the input buffer settings.
	 */
	if (ctx->layer_format == DISP_FORMAT_YUV420_P &&
			(y < 0 || ctx->layer_win_y < 0)) {
		if (win_rect.y < 0) {
			int y_shift = -(double)y * buf_rect.height / win_rect.height;
			buf_rect.y      += y_shift;
			buf_rect.height -= y_shift;
			win_rect.height += win_rect.y;
			win_rect.y       = 0;
		}

		if (buf_rect.height <= 0 || win_rect.height <= 0) {
			/* No part of the window is visible. Just construct a fake rectangle
			 * outside the screen as a window placement (but with a non-negative Y
			 * coordinate). Do this to avoid passing bogus negative heights to
			 * the kernel driver (who knows how it would react?) */
			win_rect.x = -1;
			win_rect.y = 0;
			win_rect.width = 1;
			win_rect.height = 1;
			tmp[0] = ctx->fb_id;
			tmp[1] = ctx->layer_id;
			/*
			   tmp[2] = (uintptr_t)&win_rect;
			   return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, &tmp);
			   */
			tmp[2] = (uintptr_t)&layer_info;
			memcpy(&layer_info.screen_win, &win_rect, sizeof(disp_window));
			return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp);
		}

		tmp[0] = ctx->fb_id;
		tmp[1] = ctx->layer_id;
		/*
		   tmp[2] = (uintptr_t)&buf_rect;
		   if ((err = ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_SRC_WINDOW, &tmp)))
		   return err;
		   */
		tmp[2] = (uintptr_t)&layer_info;
		memcpy(&layer_info.fb.src_win, &buf_rect, sizeof(disp_window));
		if ((err = ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp)))
			return err;
	}
	/* Save the new non-adjusted window position */
	ctx->layer_win_x = x;
	ctx->layer_win_y = y;

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	/*
	   tmp[2] = (uintptr_t)&win_rect;
	   return ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_SCN_WINDOW, &tmp);
	   */
	tmp[2] = (uintptr_t)&layer_info;
	memcpy(&layer_info.screen_win, &win_rect, sizeof(disp_window));
	if ((err = ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp)))
		return err;

}

int sunxi_layer_show(sunxi_disp_t *ctx)
{
	uint32_t tmp[4];

	if (ctx->layer_id < 0)
		return -1;

	/* YUV formats need to use a scaler */
	if (ctx->layer_format == DISP_FORMAT_YUV420_P && !ctx->layer_scaler_is_enabled) {
		if (sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_SCALER) == 0)
			ctx->layer_scaler_is_enabled = 1;
	}

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	ioctl(ctx->fd_disp, DISP_CMD_LAYER_ENABLE, &tmp);

}

int sunxi_layer_hide(sunxi_disp_t *ctx)
{
	int result;
	uint32_t tmp[4];

	if (ctx->layer_id < 0)
		return -1;

	/* If the layer is hidden, there is no need to keep the scaler occupied */
	if (ctx->layer_scaler_is_enabled) {
		if (sunxi_layer_change_work_mode(ctx, DISP_LAYER_WORK_MODE_NORMAL) == 0)
			ctx->layer_scaler_is_enabled = 0;
	}

	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	return ioctl(ctx->fd_disp, DISP_CMD_LAYER_DISABLE, &tmp);
#if 0
	tmp[0] = ctx->fb_id;
	tmp[1] = 2;
	return ioctl(ctx->fd_disp, DISP_CMD_LAYER_DISABLE, &tmp);
#endif
}

int sunxi_layer_set_colorkey(sunxi_disp_t *ctx, uint32_t color)
{


	uint32_t tmp[4];
	disp_colorkey colorkey;
	disp_color_info disp_color;

	disp_color.alpha = (color >> 24) & 0xFF;
	disp_color.red   = (color >> 16) & 0xFF;
	disp_color.green = (color >> 8)  & 0xFF;
	disp_color.blue  = (color >> 0)  & 0xFF;

	colorkey.ck_min = disp_color;
	colorkey.ck_max = disp_color;
	colorkey.red_match_rule   = 2;
	colorkey.green_match_rule = 2;
	colorkey.blue_match_rule  = 2;

	tmp[0] = ctx->fb_id;
	tmp[1] = (uintptr_t)&colorkey;
	if (ioctl(ctx->fd_disp, DISP_CMD_SET_COLORKEY, &tmp))
		return -1;
	disp_layer_info layer_video;
	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_video;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_GET_INFO, &tmp) < 0)
		return -1;
	layer_video.ck_enable = 1;
	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp) < 0)
		return -1;

	return 0;
}

int sunxi_layer_disable_colorkey(sunxi_disp_t *ctx)
{

	uint32_t tmp[4];
	disp_layer_info layer_video;

	layer_video.ck_enable = 0;
	tmp[0] = ctx->fb_id;
	tmp[1] = ctx->layer_id;
	tmp[2] = (uintptr_t)&layer_video;

	if (ioctl(ctx->fd_disp, DISP_CMD_LAYER_SET_INFO, &tmp) < 0)
		return -1;

	return 0;
}

/*****************************************************************************/

int sunxi_wait_for_vsync(sunxi_disp_t *ctx)
{
	return ioctl(ctx->fd_fb, FBIO_WAITFORVSYNC, 0);
}

/*****************************************************************************/

int sunxi_g2d_fill_a8r8g8b8(sunxi_disp_t *disp,
		int           x,
		int           y,
		int           w,
		int           h,
		uint32_t      color)
{
	g2d_fillrect tmp;

	if (disp->fd_g2d < 0)
		return -1;

	if (w <= 0 || h <= 0)
		return 0;

	tmp.flag                = G2D_FIL_NONE;
	tmp.dst_image.addr[0]   = disp->framebuffer_paddr;
	tmp.dst_image.w         = disp->xres;
	tmp.dst_image.h         = disp->framebuffer_height;
	tmp.dst_image.format    = G2D_FMT_ARGB_AYUV8888;
	tmp.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	tmp.dst_rect.x          = x;
	tmp.dst_rect.y          = y;
	tmp.dst_rect.w          = w;
	tmp.dst_rect.h          = h;
	tmp.color               = color;
	tmp.alpha               = 0;

	return ioctl(disp->fd_g2d, G2D_CMD_FILLRECT, &tmp);
}

int sunxi_g2d_blit_a8r8g8b8(sunxi_disp_t *disp,
		int           dst_x,
		int           dst_y,
		int           src_x,
		int           src_y,
		int           w,
		int           h)
{
	g2d_blt tmp;

	if (disp->fd_g2d < 0)
		return -1;

	if (w <= 0 || h <= 0)
		return 0;

	tmp.flag                = G2D_BLT_NONE;
	tmp.src_image.addr[0]   = disp->framebuffer_paddr;
	tmp.src_image.w         = disp->xres;
	tmp.src_image.h         = disp->framebuffer_height;
	tmp.src_image.format    = G2D_FMT_ARGB_AYUV8888;
	tmp.src_image.pixel_seq = G2D_SEQ_NORMAL;
	tmp.src_rect.x          = src_x;
	tmp.src_rect.y          = src_y;
	tmp.src_rect.w          = w;
	tmp.src_rect.h          = h;
	tmp.dst_image.addr[0]   = disp->framebuffer_paddr;
	tmp.dst_image.w         = disp->xres;
	tmp.dst_image.h         = disp->framebuffer_height;
	tmp.dst_image.format    = G2D_FMT_ARGB_AYUV8888;
	tmp.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	tmp.dst_x               = dst_x;
	tmp.dst_y               = dst_y;
	tmp.color               = 0;
	tmp.alpha               = 0;

	return ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp);
}

/*
 * The following function implements a 16bpp blit using 32bpp mode by
 * splitting the area into an aligned middle part (which is blit using
 * 32bpp mode) and left and right edges if required.
 *
 * It assumes the parameters have already been validated by the caller.
 * This includes the condition (src_x & 1) == (dst_x & 1), which is
 * necessary to be able to use 32bpp mode.
 */

int sunxi_g2d_blit_r5g6b5_in_three(sunxi_disp_t *disp, uint8_t *src_bits,
		uint8_t *dst_bits, int src_stride, int dst_stride, int src_x, int src_y,
		int dst_x, int dst_y, int w, int h)
{
	g2d_blt tmp;
	/* Set up the invariant blit parameters. */
	tmp.flag                = G2D_BLT_NONE;
	tmp.src_image.h         = src_y + h;
	tmp.src_rect.y          = src_y;
	tmp.src_rect.h          = h;
	tmp.dst_image.h         = dst_y + h;
	tmp.dst_y               = dst_y;
	tmp.color               = 0;
	tmp.alpha               = 0;

	if (src_x & 1) {
		tmp.src_image.addr[0]   = disp->framebuffer_paddr +
			(src_bits - disp->framebuffer_addr);
		tmp.src_image.format    = G2D_FMT_RGB565;
		tmp.src_image.pixel_seq = G2D_SEQ_P10;
		tmp.src_image.w         = src_stride * 2;
		tmp.src_rect.x          = src_x;
		tmp.src_rect.w          = 1;
		tmp.dst_image.addr[0]   = disp->framebuffer_paddr +
			(dst_bits - disp->framebuffer_addr);
		tmp.dst_image.format    = G2D_FMT_RGB565;
		tmp.dst_image.pixel_seq = G2D_SEQ_P10;
		tmp.dst_image.w         = dst_stride * 2;
		tmp.dst_x               = dst_x;
		if (ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp))
			return 0;
		src_x++;
		dst_x++;
		w--;
	}
	if (w >= 2) {
		int w2;
		tmp.src_image.addr[0]   = disp->framebuffer_paddr +
			(src_bits - disp->framebuffer_addr);
		tmp.src_image.format    = G2D_FMT_ARGB_AYUV8888;
		tmp.src_image.pixel_seq = G2D_SEQ_NORMAL;
		tmp.src_image.w         = src_stride;
		tmp.src_rect.x          = src_x >> 1;
		tmp.src_rect.w          = w >> 1;
		tmp.dst_image.addr[0]   = disp->framebuffer_paddr +
			(dst_bits - disp->framebuffer_addr);
		tmp.dst_image.format    = G2D_FMT_ARGB_AYUV8888;
		tmp.dst_image.pixel_seq = G2D_SEQ_NORMAL;
		tmp.dst_image.w         = dst_stride;
		tmp.dst_x               = dst_x >> 1;
		if (ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp))
			return 0;
		w2 = (w >> 1) * 2;
		src_x += w2;
		dst_x += w2;
		w &= 1;
	}
	if (w) {
		tmp.src_image.addr[0]   = disp->framebuffer_paddr +
			(src_bits - disp->framebuffer_addr);
		tmp.src_image.format    = G2D_FMT_RGB565;
		tmp.src_image.pixel_seq = G2D_SEQ_P10;
		tmp.src_image.w         = src_stride * 2;
		tmp.src_rect.x          = src_x;
		tmp.src_rect.w          = 1;
		tmp.dst_image.addr[0]   = disp->framebuffer_paddr +
			(dst_bits - disp->framebuffer_addr);

		tmp.dst_image.format    = G2D_FMT_RGB565;
		tmp.dst_image.pixel_seq = G2D_SEQ_P10;
		tmp.dst_image.w         = dst_stride * 2;
		tmp.dst_x               = dst_x;
		if (ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp))
			return 0;
	}
	return 1;
}

static inline int sunxi_g2d_try_fallback_blt(void               *self,
		uint32_t           *src_bits,
		uint32_t           *dst_bits,
		int                 src_stride,
		int                 dst_stride,
		int                 src_bpp,
		int                 dst_bpp,
		int                 src_x,
		int                 src_y,
		int                 dst_x,
		int                 dst_y,
		int                 w,
		int                 h)
{
	sunxi_disp_t *disp = (sunxi_disp_t *)self;
	if (disp->fallback_blt2d)
		return disp->fallback_blt2d->overlapped_blt(disp->fallback_blt2d->self,
				src_bits, dst_bits,
				src_stride, dst_stride,
				src_bpp, dst_bpp,
				src_x, src_y,
				dst_x, dst_y, w, h);
	return 0;

}

#define FALLBACK_BLT() sunxi_g2d_try_fallback_blt(self, src_bits,        \
		dst_bits, src_stride,  \
		dst_stride, src_bpp,   \
		dst_bpp, src_x, src_y, \
		dst_x, dst_y, w, h);

/*
 * G2D counterpart for pixman_blt (function arguments are the same with
 * only sunxi_disp_t extra argument added). Supports 16bpp (r5g6b5) and
 * 32bpp (a8r8g8b8) formats and also conversion between them.
 *
 * Can do G2D accelerated blits only if both source and destination
 * buffers are inside framebuffer. Returns FALSE (0) otherwise.
 */
int sunxi_g2d_blt(void               *self,
		uint32_t           *src_bits,
		uint32_t           *dst_bits,
		int                 src_stride,
		int                 dst_stride,
		int                 src_bpp,
		int                 dst_bpp,
		int                 src_x,
		int                 src_y,
		int                 dst_x,
		int                 dst_y,
		int                 w,
		int                 h)
{
	sunxi_disp_t *disp = (sunxi_disp_t *)self;
	int blt_size_threshold;
	g2d_blt tmp;

	/* Zero size blit, nothing to do */
	if (w <= 0 || h <= 0)
		return 1;

	/*
	 * Very minimal validation here. We just assume that if the begginging
	 * of both source and destination images belongs to the framebuffer,
	 * then these images are entirely residing inside the framebuffer
	 * without crossing its borders. Any other checks are supposed
	 * to be done by the caller.
	 */
	if ((uint8_t *)src_bits < disp->framebuffer_addr ||
			(uint8_t *)src_bits >= disp->framebuffer_addr + disp->framebuffer_size ||
			(uint8_t *)dst_bits < disp->framebuffer_addr ||
			(uint8_t *)dst_bits >= disp->framebuffer_addr + disp->framebuffer_size)
	{
		return FALLBACK_BLT();
	}

	/*
	 * If the area is smaller than G2D_BLT_SIZE_THRESHOLD, prefer to avoid the
	 * overhead of G2D and do a CPU blit instead. There is a special threshold
	 * for 16bpp to 16bpp copy.
	 */
	if (src_bpp == 16 && dst_bpp == 16)
		blt_size_threshold = G2D_BLT_SIZE_THRESHOLD_16BPP;
	else
		blt_size_threshold = G2D_BLT_SIZE_THRESHOLD;
	if (w * h < blt_size_threshold)
		return FALLBACK_BLT();

	/* Unsupported overlapping type */
	/*hcl add:we add a ion buffer to support it!
	 */
	if (src_bits == dst_bits  && ((src_y < dst_y) || (src_y == dst_y && src_x  < dst_x)) && disp->buffer_paddr == 0)
	{
		fprintf(stderr,"FALLBACK_BLT() disp->buffer_paddr=0x%x\n",disp->buffer_paddr);
		return FALLBACK_BLT();
	}

	if (disp->fd_g2d < 0)
		return FALLBACK_BLT();

	/* Do a 16-bit using 32-bit mode if possible. */
	if (src_bpp == 16 && dst_bpp == 16 && (src_x & 1) == (dst_x & 1))
		/* Check whether the overlapping type is supported, the condition */
		/* is slightly different compared to the regular blit. */
		if (!(src_bits == dst_bits && src_y == dst_y && src_x < dst_x))
			return sunxi_g2d_blit_r5g6b5_in_three(disp, (uint8_t *)src_bits,
					(uint8_t *)dst_bits, src_stride, dst_stride, src_x, src_y,
					dst_x, dst_y, w, h);

	if ((src_bpp != 16 && src_bpp != 32) || (dst_bpp != 16 && dst_bpp != 32))
		return FALLBACK_BLT();

	tmp.flag                    = G2D_BLT_NONE;
	tmp.src_image.addr[0]       = disp->framebuffer_paddr +
		((uint8_t *)src_bits - disp->framebuffer_addr);
	tmp.src_rect.x              = src_x;
	tmp.src_rect.y              = src_y;
	tmp.src_rect.w              = w;
	tmp.src_rect.h              = h;
	tmp.src_image.h             = src_y + h;
	if (src_bpp == 32) {
		tmp.src_image.w         = src_stride;
		tmp.src_image.format    = G2D_FMT_ARGB_AYUV8888;
		tmp.src_image.pixel_seq = G2D_SEQ_NORMAL;
	}
	else if (src_bpp == 16) {
		tmp.src_image.w         = src_stride * 2;
		tmp.src_image.format    = G2D_FMT_RGB565;
		tmp.src_image.pixel_seq = G2D_SEQ_P10;
	}

	tmp.dst_image.addr[0]       = disp->framebuffer_paddr +
		((uint8_t *)dst_bits - disp->framebuffer_addr);
	tmp.dst_x                   = dst_x;
	tmp.dst_y                   = dst_y;
	tmp.color                   = 0;
	tmp.alpha                   = 0;
	tmp.dst_image.h             = dst_y + h;
	if (dst_bpp == 32) {
		tmp.dst_image.w         = dst_stride;
		tmp.dst_image.format    = G2D_FMT_ARGB_AYUV8888;
		tmp.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	}
	else if (dst_bpp == 16) {
		tmp.dst_image.w         = dst_stride * 2;
		tmp.dst_image.format    = G2D_FMT_RGB565;
		tmp.dst_image.pixel_seq = G2D_SEQ_P10;
	}
	/*hcl add:support overlapping type
	 *
	 */
	if(src_bits == dst_bits  && ((src_y < dst_y) || (src_y == dst_y && src_x  < dst_x)))
	{
		tmp.dst_image.addr[0]       = disp->buffer_paddr;
		tmp.dst_x                   = 0;
		tmp.dst_y                   = 0;
		ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp);
		tmp.dst_x                   = dst_x;
		tmp.dst_y                   = dst_y;
		tmp.src_rect.x              = 0;
		tmp.src_rect.y              = 0;
		tmp.dst_image.addr[0]       = disp->framebuffer_paddr +
			((uint8_t *)dst_bits - disp->framebuffer_addr);
		tmp.src_image.addr[0]       = disp->buffer_paddr;
	}

	return ioctl(disp->fd_g2d, G2D_CMD_BITBLT, &tmp) == 0;
}

/*****************************************************************************
 * Use G2D to convert image format and scale image.                          *
 *****************************************************************************/
int sunxi_g2d_stretchblt(void *self,short src_w, short src_h, short rect_w, short rect_h, uint32_t offset,uint32_t y,uint32_t u,uint32_t v,short dst_w,short dst_h)
{
	g2d_stretchblt str;
	g2d_image image_front,scn;
	g2d_rect src_rect,dst_rect;
	sunxi_disp_t *disp = (sunxi_disp_t *)self;

	str.flag = G2D_BLT_NONE;
	str.color                   = 0;
	str.alpha                   = 0;
	
	str.src_image.addr[0] = disp->buffer_paddr+offset;
	str.src_image.addr[1] = disp->buffer_paddr+offset+u;
	str.src_image.w = src_w;
	str.src_image.h = src_h;
	str.src_image.format = G2D_FMT_PYUV420UVC;
	str.src_image.pixel_seq = G2D_SEQ_NORMAL;
	str.src_rect.x = 0;
	str.src_rect.y = 0;
	str.src_rect.w = rect_w;
	str.src_rect.h = rect_h;
	
	str.dst_image.addr[0] = disp->framebuffer_paddr+offset;
	str.dst_image.w = dst_w;
	str.dst_image.h = dst_h;
	str.dst_image.format = G2D_FMT_ARGB_AYUV8888;
	str.dst_image.pixel_seq = G2D_SEQ_NORMAL;
	str.dst_rect.x = 0;
	str.dst_rect.y = 0;
	str.dst_rect.w = dst_w;
	str.dst_rect.h = dst_h;

	int ret = ioctl(disp->ion_fd, ION_IOC_SYNC, &(disp->handle_data));
	if(ret < 0)
	{
		fprintf(stderr,"ION_IOC_SYNC error! ret=%d\n",ret);
		
	}

	struct ion_custom_data custom_data;
	sunxi_cache_range range;
	range.start = (unsigned long)disp->buffer_addr + offset;
	range.end = (unsigned long)disp->buffer_addr + offset + v + v -u;
	custom_data.cmd = ION_IOC_SUNXI_FLUSH_RANGE;
	custom_data.arg = (unsigned long)&range;
	ret = ioctl(disp->ion_fd, ION_IOC_CUSTOM, &custom_data);
	if(ret < 0) {
		fprintf(stderr,"%s(%d): ION_IOC_CUSTOM err, ret %d\n", __func__, __LINE__, ret);
	}

	ret = ioctl(disp->fd_g2d,G2D_CMD_STRETCHBLT, &str);//G2D_CMD_BITBLT G2D_CMD_STRETCHBLT
	if(ret < 0)
	{
		fprintf(stderr,"g2d_stretchblt error! ret=%d\n",ret);
		
	}

	return ret;
}

/*****************************************************************************
 * Allocate a physical memory continuously for G2D accelerate.               *
 *****************************************************************************/
void* sunxi_ion_alloc(sunxi_disp_t *disp, int len)
{
	struct ion_allocation_data alloc_data;
	if(len <= 0)
		return NULL;
	disp->ion_fd = -1;	
	int ret = -1, fd;

	if((fd = open(ION_DEV_NAME, O_RDWR)) < 0) {
		fprintf(stderr,"%s(%d) err: open %s dev failed\n", __func__, __LINE__, ION_DEV_NAME);
		return NULL;
	}

	/* alloc buffer */
	alloc_data.len = len;
	alloc_data.align = ION_ALLOC_ALIGN;
	alloc_data.heap_id_mask = ION_HEAP_CARVEOUT_MASK;//ION_HEAP_TYPE_DMA_MASK;// | ION_HEAP_SYSTEM_CONTIG_MASK;
	alloc_data.flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
	ret = ioctl(fd, ION_IOC_ALLOC, &alloc_data);
	if(ret) {
		fprintf(stderr,"%s(%d): ION_IOC_ALLOC err, ret %d, handle 0x%08x\n", __func__, __LINE__, ret, (unsigned int)alloc_data.handle);
		close(fd);
		return NULL;
	}
	disp->ion_fd = fd;
	disp->handle_data.handle = alloc_data.handle;

	//get phys_addr
	struct ion_custom_data custom_data;
	sunxi_phys_data phys_data;

	custom_data.cmd = ION_IOC_SUNXI_PHYS_ADDR;
	phys_data.handle = alloc_data.handle;
	custom_data.arg = (unsigned long)&phys_data;
	ret = ioctl(disp->ion_fd, ION_IOC_CUSTOM, &custom_data);
	if(ret) {
		printf("%s(%d): ION_IOC_SUNXI_PHYS_ADDR err, ret %d\n", __func__, __LINE__, ret);
		disp->buffer_paddr = 0;
	}else{
		disp->buffer_paddr = phys_data.phys_addr;
	}
	
	struct ion_fd_data fd_data;
	
	/* map dma buffer fd */
	fd_data.handle = alloc_data.handle;
	ret = ioctl(disp->ion_fd, ION_IOC_MAP, &fd_data);
	if(ret) {
		fprintf(stderr,"%s(%d): ION_IOC_MAP err, ret %d, dmabuf fd 0x%08x\n", __func__, __LINE__, ret, (unsigned int)fd_data.fd);
		return NULL;
	}


	/* mmap to user */
	disp->buffer_addr = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd_data.fd, 0);
	if(MAP_FAILED == disp->buffer_addr) {
		fprintf(stderr,"%s(%d): mmap err, ret %d\n", __func__, __LINE__, (unsigned int)disp->buffer_addr);
	}


	return disp->buffer_addr;
}

int sunxi_ion_free(sunxi_disp_t *disp)
{
	int ret = -1;
	if(disp->handle_data.handle == NULL)
		return -1;
	munmap(disp->buffer_addr, disp->framebuffer_size);
	ret = ioctl(disp->ion_fd, ION_IOC_FREE, &(disp->handle_data.handle));
	if(ret) {
		fprintf(stderr,"%s(%d): ION_IOC_FREE err, ret %d\n", __func__, __LINE__, ret);
		goto out;
	}

	ret = 0;
out:
	close(disp->ion_fd);
	return ret;
}

/*****************************************************************************
 * Set a part of image in layer to show on window.                           *
 * This function just for A80 and H8,not for A10/A20.                        *
 * 当视频的一部分被拖放到屏幕以外时，我们需要重新计算视频应该显示的区域。          *
 * 这个函数实现的原理是，根据缩放比例，计算出视频图层仍然在屏幕中的区域，然后将这   *
 * 部分区域设置为显示区域。                                                    *
 *****************************************************************************/
int sunxi_resize_layer_window(sunxi_disp_t *disp, int drw_x, int drw_y, int drw_w, int drw_h, int src_x, int src_y, int src_w, int src_h)
{
	int x,y,w,h;
	int sx,sy,sw,sh;
	x = drw_x;
	y = drw_y;
	w = drw_w;
	h = drw_h;
	sx = src_x;
	sy = src_y;
	sw = src_w;
	sh = src_h;

	disp_layer_info layer_video,layer_win;
	uint32_t args_video[4];
	uint32_t args_win[4];

	args_video[0] = disp->fb_id;
	args_video[1] = disp->layer_id;
	args_video[2] = (uintptr_t)&layer_video;
	if (ioctl(disp->fd_disp, DISP_CMD_LAYER_GET_INFO, &args_video) < 0)
		return -1;

	args_win[0] = disp->fb_id;
	args_win[1] = disp->gfx_layer_id;
	args_win[2] = (uintptr_t)&layer_win;
	if (ioctl(disp->fd_disp, DISP_CMD_LAYER_GET_INFO, &args_win) < 0)
		return -1;

	if(layer_video.mode == DISP_LAYER_WORK_MODE_SCALER)
	{
		if(drw_x < 0)
		{
			x = 0;
			w = drw_x + drw_w;
			sx = 0-(drw_x * ((float)src_w/drw_w));
			sw = w * ((float)src_w/drw_w);
		}
		else if(drw_x+drw_w > layer_win.fb.src_win.width)
		{
			w = layer_win.fb.src_win.width - drw_x;
			sw = w * ((float)src_w/drw_w);
		}
	
		if(drw_y < 0)
		{
			y = 0;
			h = drw_h + drw_y;
			sy = 0-(drw_y * ((float)src_h/drw_h));
			sh = h * ((float)src_h/drw_h);
		}
		else if(drw_y+drw_h > layer_win.fb.src_win.height)
		{
			h = layer_win.fb.src_win.height - drw_y;
			sh = h * ((float)src_h/drw_h);
		}		
	}
	else{
		sw = w;
		sh = h;

#if 1
		if(drw_x < 0)
		{
			x = 0;
			sx = 0-drw_x;
		}
		else if(drw_x+drw_w > layer_win.fb.src_win.width)
		{
			w = layer_win.fb.src_win.width - drw_x;
		}
	
		if(drw_y < 0)
		{
			y = 0;
			sy = 0-drw_y;
		}
		else if(drw_y+drw_h > layer_win.fb.src_win.height)
		{
			h = layer_win.fb.src_win.height - drw_y;
		}		
#endif
	}

	layer_video.fb.src_win.x =sx;
	layer_video.fb.src_win.y =sy;
	layer_video.fb.src_win.width =sw;
	layer_video.fb.src_win.height =sh;
	layer_video.screen_win.x =x;
	layer_video.screen_win.y =y;
	layer_video.screen_win.width =w;
	layer_video.screen_win.height =h;
	args_video[0] = disp->fb_id;
	args_video[1] = disp->layer_id;
	args_video[2] = (uintptr_t)&layer_video;
	if (ioctl(disp->fd_disp, DISP_CMD_LAYER_SET_INFO, &args_video) < 0)
		return -1;
	return 0;
}

