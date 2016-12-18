/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <stdio.h>

#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "minui.h"
#include "graphics.h"
#include <pixelflinger/pixelflinger.h>

static GRSurface* fbdev_init(minui_backend*);
static GRSurface* fbdev_flip(minui_backend*);
static void fbdev_blank(minui_backend*, bool);
static void fbdev_exit(minui_backend*);

static GRSurface gr_framebuffer[2];
static bool double_buffered;
static GRSurface* gr_draw = NULL;
static int displayed_buffer;

static fb_var_screeninfo vi;
int flp = 2;
static int fb_fd = -1;
static __u32 smem_len;

unsigned char *bobs;

static minui_backend my_backend = {
    .init = fbdev_init,
    .flip = fbdev_flip,
    .blank = fbdev_blank,
    .exit = fbdev_exit,
};

minui_backend* open_fbdev() {
    return &my_backend;
}

static void fbdev_blank(minui_backend* backend __unused, bool blank)
{
#if defined(TW_NO_SCREEN_BLANK) && defined(TW_BRIGHTNESS_PATH) && defined(TW_MAX_BRIGHTNESS)
    int fd;
    char brightness[4];
    snprintf(brightness, 4, "%03d", TW_MAX_BRIGHTNESS/2);

    fd = open(TW_BRIGHTNESS_PATH, O_RDWR);
    if (fd < 0) {
        perror("cannot open LCD backlight");
        return;
    }
    close(fd);
#else
#ifndef TW_NO_SCREEN_BLANK
    int ret;
#endif
#endif
}

static void set_displayed_framebuffer(unsigned n)
{
}

static GRSurface* fbdev_init(minui_backend* backend) {
    int fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd == -1) {
        perror("cannot open fb0");
        return NULL;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        perror("failed to get fb0 info (FBIOGET_VSCREENINFO)");
        close(fd);
        return NULL;
    }

 vi.bits_per_pixel = 32;
 vi.vmode = FB_VMODE_NONINTERLACED;
  vi.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
 if (ioctl(fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
             close(fd);
                 return NULL;
     }


    fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        perror("failed to get fb0 info (FBIOGET_FSCREENINFO)");
        close(fd);
        return NULL;
    }


    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0) {
        perror("failed to get fb0 info");
        close(fd);
        return NULL;
    }

    // We print this out for informational purposes only, but
    // throughout we assume that the framebuffer device uses an RGBX
    // pixel format.  This is the case for every development device I
    // have access to.  For some of those devices (eg, hammerhead aka
    // Nexus 5), FBIOGET_VSCREENINFO *reports* that it wants a
    // different format (XBGR) but actually produces the correct
    // results on the display when you write RGBX.
    //
    // If you have a device that actually *needs* another pixel format
    // (ie, BGRX, or 565), patches welcome...

    printf("fb0 reports (possibly inaccurate):\n"
           "  vi.bits_per_pixel = %d\n"
           "  vi.red.offset   = %3d   .length = %3d\n"
           "  vi.green.offset = %3d   .length = %3d\n"
           "  vi.blue.offset  = %3d   .length = %3d\n"
           "  vi.xres = %3d vi.yres = %3d fi.line_length = %3d\n",

           vi.bits_per_pixel,
           vi.red.offset, vi.red.length,
           vi.green.offset, vi.green.length,
           vi.blue.offset, vi.blue.length, vi.xres, vi.yres, fi.line_length);

    void* bits = mmap(0, (vi.yres * fi.line_length), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bits == MAP_FAILED) {
        perror("failed to mmap framebuffer");
        close(fd);
        return NULL;
    }

    memset(bits, 0, (vi.yres * fi.line_length));

    gr_framebuffer[0].width = vi.xres;
    gr_framebuffer[0].height = vi.yres;
    gr_framebuffer[0].row_bytes = fi.line_length;
    gr_framebuffer[0].pixel_bytes = vi.bits_per_pixel / 8;
    gr_framebuffer[0].format = GGL_PIXEL_FORMAT_BGRA_8888;

#ifdef RECOVERY_GRAPHICS_FORCE_USE_LINELENGTH
    printf("Forcing line length\n");
    vi.xres_virtual = fi.line_length / gr_framebuffer[0].pixel_bytes;
#endif
    gr_framebuffer[0].data = reinterpret_cast<uint8_t*>(bits);
    bobs = reinterpret_cast<uint8_t*>(bits);

    gr_draw = (GRSurface*) malloc(sizeof(GRSurface));
    if (!gr_draw) {
        perror("failed to allocate gr_draw");
        close(fd);
        munmap(bits, fi.smem_len);
        return NULL;
    }
    memcpy(gr_draw, gr_framebuffer, sizeof(GRSurface));
    gr_draw->data = (unsigned char*) calloc(gr_draw->height * gr_draw->row_bytes, 1);
    if (!gr_draw->data) {
        perror("failed to allocate in-memory surface");
        close(fd);
        free(gr_draw);
        munmap(bits, fi.smem_len);
        return NULL;
    }

        double_buffered = false;
        printf("single buffered\n");

    fb_fd = fd;

 if (ioctl(fd, FBIOPUT_VSCREENINFO, &vi) < 0) {
             close(fd);
                 return NULL;
     }

    printf("framebuffer: %d (%d x %d)\n", fb_fd, gr_draw->width, gr_draw->height);

    smem_len = fi.smem_len;

    return gr_draw;
}

static GRSurface* fbdev_flip(minui_backend* backend __unused) {

    int x, y, z = 0;
    long int location = 0;

        __u32 dummy = 0;

    memcpy(gr_framebuffer[0].data, gr_draw->data, gr_draw->height * gr_draw->row_bytes);
    ioctl(fb_fd, FBIO_WAITFORVSYNC, &dummy);
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);

    return gr_draw;
}

static void fbdev_exit(minui_backend* backend __unused) {
    close(fb_fd);
    fb_fd = -1;

    if (gr_draw) {
        free(gr_draw->data);
        free(gr_draw);
    }
    gr_draw = NULL;
    munmap(gr_framebuffer[0].data, smem_len);
}
