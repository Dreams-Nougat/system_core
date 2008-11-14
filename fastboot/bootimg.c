/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the 
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED 
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fastboot.h"

void bootimg_set_cmdline(boot_img_hdr *h, const char *cmdline)
{
    strcpy(h->cmdline, cmdline);
}

boot_img_hdr *mkbootimg(void *kernel, size_t kernel_size,
                        void *ramdisk, size_t ramdisk_size,
                        void *second, size_t second_size,
                        size_t page_size,
                        size_t *bootimg_size)
{
    size_t kernel_actual;
    size_t ramdisk_actual;
    size_t second_actual;
    size_t page_mask;
    boot_img_hdr *hdr;
    
    page_mask = page_size - 1;
    
    kernel_actual = (kernel_size + page_mask) & (~page_mask);
    ramdisk_actual = (ramdisk_size + page_mask) & (~page_mask);
    second_actual = (second_size + page_mask) & (~page_mask);
    
    *bootimg_size = page_size + kernel_actual + ramdisk_actual + second_actual;
    
    hdr = calloc(*bootimg_size, 1);
    
    if (hdr == NULL) {
        return hdr;
    }

    memcpy(hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE);
    
    hdr->kernel_size = kernel_size;
    hdr->kernel_addr = 0x10008000;
    hdr->ramdisk_size = ramdisk_size;
    hdr->ramdisk_addr = 0x11000000;
    hdr->second_size = second_size;
    hdr->second_addr = 0x10F00000;
    
    hdr->tags_addr = 0x10000100;
    hdr->page_size = page_size;

    memcpy(hdr->magic + page_size, 
           kernel, kernel_size);
    memcpy(hdr->magic + page_size + kernel_actual, 
           ramdisk, ramdisk_size);
    memcpy(hdr->magic + page_size + kernel_actual + ramdisk_actual,
           second, second_size);
    return hdr;
}
