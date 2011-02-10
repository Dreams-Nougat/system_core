/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include "fdevent.h"
#include "adb.h"

#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/endian.h>

/* TODO:
** - sync with vsync to avoid tearing
*/
/* This version number defines the format of the fbinfo struct.
   It must match versioning in ddms where this data is consumed. */
#define DDMS_RAWIMAGE_VERSION 1
struct fbinfo {
    unsigned int version;
    unsigned int bpp;
    unsigned int size;
    unsigned int width;
    unsigned int height;
    unsigned int red_offset;
    unsigned int red_length;
    unsigned int blue_offset;
    unsigned int blue_length;
    unsigned int green_offset;
    unsigned int green_length;
    unsigned int alpha_offset;
    unsigned int alpha_length;
} __attribute__((packed));

void htole_buf(char* buf, size_t len, int bytespp){
    unsigned int i;

    /* 
     * the ddms library now only accepts 16 bits and 32 bits mode
     * see the implementation RawImage.getARGB()
     */

    if ( bytespp == 2 ) {
      for (i=0; i<len; i+=bytespp){ 
        uint16_t *p = (uint16_t*)(buf+i);
        *p = htole16(*p);
      }
    } else if (bytespp == 4 ){
      for (i=0; i<len; i+=bytespp) {
        uint32_t *p = (uint32_t*)(buf+i);
        *p = htole32(*p);
      }
    }
}

void framebuffer_service(int fd, void *cookie)
{
    struct fb_var_screeninfo vinfo;
    int fb, offset;
    char x[256];

    struct fbinfo fbinfo;
    unsigned i, bytespp;

    fb = open("/dev/graphics/fb0", O_RDONLY);
    if(fb < 0) goto done;

    if(ioctl(fb, FBIOGET_VSCREENINFO, &vinfo) < 0) goto done;
    fcntl(fb, F_SETFD, FD_CLOEXEC);

    bytespp = vinfo.bits_per_pixel / 8;

    fbinfo.version = htole32(DDMS_RAWIMAGE_VERSION);
    fbinfo.bpp = htole32(vinfo.bits_per_pixel);
    fbinfo.size = htole32(vinfo.xres * vinfo.yres * bytespp);
    fbinfo.width = htole32(vinfo.xres);
    fbinfo.height = htole32(vinfo.yres);
    fbinfo.red_offset = htole32(vinfo.red.offset);
    fbinfo.red_length = htole32(vinfo.red.length);
    fbinfo.green_offset = htole32(vinfo.green.offset);
    fbinfo.green_length = htole32(vinfo.green.length);
    fbinfo.blue_offset = htole32(vinfo.blue.offset);
    fbinfo.blue_length = htole32(vinfo.blue.length);
    fbinfo.alpha_offset = htole32(vinfo.transp.offset);
    fbinfo.alpha_length = htole32(vinfo.transp.length);

    /* HACK: for several of our 3d cores a specific alignment
     * is required so the start of the fb may not be an integer number of lines
     * from the base.  As a result we are storing the additional offset in
     * xoffset. This is not the correct usage for xoffset, it should be added
     * to each line, not just once at the beginning */
    offset = vinfo.xoffset * bytespp;

    offset += vinfo.xres * vinfo.yoffset * bytespp;

    if(writex(fd, &fbinfo, sizeof(fbinfo))) goto done;

    lseek(fb, offset, SEEK_SET);
    for(i = 0; i < fbinfo.size; i += 256) {
      if(readx(fb, &x, 256)) goto done;
      htole_buf(x, 256, bytespp);
      if(writex(fd, &x, 256)) goto done;
    }

    if(readx(fb, &x, fbinfo.size % 256)) goto done;
    htole_buf(x, fbinfo.size%256, bytespp);
    if(writex(fd, &x, fbinfo.size % 256)) goto done;

done:
    if(fb >= 0) close(fb);
    close(fd);
}
