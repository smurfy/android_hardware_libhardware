/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <linux/fb.h>

#define NUM_BUFFERS 2

enum {
    PAGE_FLIP = 0x00000001,
    LOCKED = 0x00000002
};

struct private_module_t {
    gralloc_module_t base;

    uint32_t flags;
    uint32_t numBuffers;
    uint32_t bufferMask;
    pthread_mutex_t lock;
    buffer_handle_t currentBuffer;
    int pmem_master;
    void* pmem_master_base;

    struct fb_var_screeninfo info;
    struct fb_fix_screeninfo finfo;
    float xdpi;
    float ydpi;
    float fps;

    int shm_fd;
    void *shm_ptr;
};

static gralloc_module_t *the_gralloc_module = NULL;

struct fb_context_t {
    framebuffer_device_t device;
};

static int fb_setSwapInterval(struct framebuffer_device_t* dev,
            int interval)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;
    // FIXME: implement fb_setSwapInterval
    return 0;
}

static int fb_setUpdateRect(struct framebuffer_device_t* dev,
        int l, int t, int w, int h)
{
    if (((w|h) <= 0) || ((l|t)<0))
        return -EINVAL;
        
    fb_context_t* ctx = (fb_context_t*)dev;
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    m->info.reserved[0] = 0x54445055; // "UPDT";
    m->info.reserved[1] = (uint16_t)l | ((uint32_t)t << 16);
    m->info.reserved[2] = (uint16_t)(l+w) | ((uint32_t)(t+h) << 16);
    return 0;
}

static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
    fb_context_t* ctx = (fb_context_t*)dev;

    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);

    void* buffer_vaddr;

    if(!the_gralloc_module)
    {
        ALOGE("gralloc module not initialized");
        return -1;
    }

    the_gralloc_module->lock(the_gralloc_module, buffer, 
            GRALLOC_USAGE_SW_READ_RARELY, 
            0, 0, m->info.xres, m->info.yres,
            &buffer_vaddr);

    if (m->shm_fd != -1 && m->shm_ptr != MAP_FAILED)
    {
        memcpy(m->shm_ptr, buffer_vaddr, m->finfo.line_length * m->info.yres);
    } else {
        ALOGW("Not ready: fd = %d, shm = %p", m->shm_fd, m->shm_ptr);
    }
    
    the_gralloc_module->unlock(the_gralloc_module, buffer); 
    
    return 0;
}

int mapFrameBufferLocked(struct private_module_t* module)
{
    char const * const device_template[] = {
            "/dev/graphics/fb%u",
            "/dev/fb%u",
            0 };

    int fd = -1;
    int i=0;
    char name[64];

    while ((fd==-1) && device_template[i]) {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);
        i++;
    }
    if (fd < 0)
        return -errno;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    struct fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_NOW;

    /*
     * Request NUM_BUFFERS screens (at lest 2 for page flipping)
     */
    info.yres_virtual = info.yres * NUM_BUFFERS;


    uint32_t flags = PAGE_FLIP;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1) {
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        ALOGW("FBIOPUT_VSCREENINFO failed, page flipping not supported");
    }

    if (info.yres_virtual < info.yres * 2) {
        // we need at least 2 for page-flipping
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        ALOGW("page flipping not supported (yres_virtual=%d, requested=%d)",
                info.yres_virtual, info.yres*2);
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    uint64_t  refreshQuotient =
    (
            uint64_t( info.upper_margin + info.lower_margin + info.yres )
            * ( info.left_margin  + info.right_margin + info.xres )
            * info.pixclock
    );

    /* Beware, info.pixclock might be 0 under emulation, so avoid a
     * division-by-0 here (SIGFPE on ARM) */
    int refreshRate = refreshQuotient > 0 ? (int)(1000000000000000LLU / refreshQuotient) : 0;

    if (refreshRate == 0) {
        // bleagh, bad info from the driver
        refreshRate = 60*1000;  // 60 Hz
    }

    if (int(info.width) <= 0 || int(info.height) <= 0) {
        // the driver doesn't return that information
        // default to 160 dpi
        info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
        info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
    }

    float xdpi = (info.xres * 25.4f) / info.width;
    float ydpi = (info.yres * 25.4f) / info.height;
    float fps  = refreshRate / 1000.0f;

    ALOGI(   "using (fd=%d)\n"
            "id           = %s\n"
            "xres         = %d px\n"
            "yres         = %d px\n"
            "xres_virtual = %d px\n"
            "yres_virtual = %d px\n"
            "bpp          = %d\n"
            "r            = %2u:%u\n"
            "g            = %2u:%u\n"
            "b            = %2u:%u\n",
            fd,
            finfo.id,
            info.xres,
            info.yres,
            info.xres_virtual,
            info.yres_virtual,
            info.bits_per_pixel,
            info.red.offset, info.red.length,
            info.green.offset, info.green.length,
            info.blue.offset, info.blue.length
    );

    ALOGI(   "width        = %d mm (%f dpi)\n"
            "height       = %d mm (%f dpi)\n"
            "refresh rate = %.2f Hz\n",
            info.width,  xdpi,
            info.height, ydpi,
            fps
    );


    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    if (finfo.smem_len <= 0)
        return -errno;


    module->flags = flags;
    module->info = info;
    module->finfo = finfo;
    module->xdpi = xdpi;
    module->ydpi = ydpi;
    module->fps = fps;

    void *tmp = (void*)malloc(finfo.line_length * info.yres);
    if(!tmp)
    {
        ALOGE("failed to allocate shm buffer");
        return 1;
    }

    FILE *fp = fopen("/dev/shm/droid_screen", "wb");
    int written = fwrite(tmp, finfo.line_length * info.yres, 1, fp);

    ALOGW("allocated %d byte shm buffer", written);
    fclose(fp);
    free(tmp);

    module->shm_fd = open("/dev/shm/droid_screen", O_RDWR | O_CREAT, 0777);
    if (module->shm_fd != -1) {
        module->shm_ptr = mmap(NULL, finfo.line_length * info.yres,
               PROT_READ | PROT_WRITE, MAP_SHARED, module->shm_fd, 0);
    }

    return 0;
}

static int mapFrameBuffer(struct private_module_t* module)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module);
    pthread_mutex_unlock(&module->lock);
    return err;
}

static int fb_close(struct hw_device_t *dev)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

static int shmbuffer_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

int shmbuffer_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

int shmbuffer_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

int shmbuffer_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

int shmbuffer_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

static struct hw_module_methods_t shmbuffer_module_methods = {
        open: shmbuffer_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: SHMBUFFER_HARDWARE_MODULE_ID,
            name: "shmbuffer",
            author: "krnlyng",
            methods: &shmbuffer_module_methods
        },
        registerBuffer: shmbuffer_register_buffer,
        unregisterBuffer: shmbuffer_unregister_buffer,
        lock: shmbuffer_lock,
        unlock: shmbuffer_unlock,
    },
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: 0,
};

static int shmbuffer_alloc(alloc_device_t* dev,
        int w, int h, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    ALOGW("shmbuffer_alloc stub");
    return 0;
}

static int shmbuffer_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    ALOGW("shmbuffer_free stub");
    return 0;
}

int shmbuffer_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr)
{
    ALOGW("shmbuffer_lock stub");
    return 0;
}

int shmbuffer_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle)
{
    ALOGW("shmbuffer_unlock stub");
    return 0;
}

int shmbuffer_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    ALOGW("shmbuffer_register_buffer stub");
    return 0;
}

int shmbuffer_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    ALOGW("shmbuffer_unregister_buffer stub");
    return 0;
}

static int shmbuffer_close(struct hw_device_t *dev)
{
    ALOGW("shmbuffer_close stub");
    return 0;
}

int shmbuffer_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        ALOGE("FATAL: Tried to load shmbuffer module with %s as argument.", name);
    } else {
        int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, (const hw_module_t**)&the_gralloc_module);
        ALOGE_IF(err, "FATAL: can't find the %s module", GRALLOC_HARDWARE_MODULE_ID);
        if(err == 0)
        {
            /* initialize our state here */
            fb_context_t *dev = (fb_context_t*)malloc(sizeof(*dev));
            memset(dev, 0, sizeof(*dev));

            /* initialize the procs */
            dev->device.common.tag = HARDWARE_DEVICE_TAG;
            dev->device.common.version = 0;
            dev->device.common.module = const_cast<hw_module_t*>(module);
            dev->device.common.close = fb_close;
            dev->device.setSwapInterval = fb_setSwapInterval;
            dev->device.post            = fb_post;
            dev->device.setUpdateRect = 0;

            private_module_t* m = (private_module_t*)module;
            status = mapFrameBuffer(m);
            if (status >= 0) {
                int stride = m->finfo.line_length / (m->info.bits_per_pixel >> 3);
                int format = (m->info.bits_per_pixel == 32)
                             ? HAL_PIXEL_FORMAT_RGBX_8888
                             : HAL_PIXEL_FORMAT_RGB_565;
                const_cast<uint32_t&>(dev->device.flags) = 0;
                const_cast<uint32_t&>(dev->device.width) = m->info.xres;
                const_cast<uint32_t&>(dev->device.height) = m->info.yres;
                const_cast<int&>(dev->device.stride) = stride;
                const_cast<int&>(dev->device.format) = format;
                const_cast<float&>(dev->device.xdpi) = m->xdpi;
                const_cast<float&>(dev->device.ydpi) = m->ydpi;
                const_cast<float&>(dev->device.fps) = m->fps;
                const_cast<int&>(dev->device.minSwapInterval) = 1;
                const_cast<int&>(dev->device.maxSwapInterval) = 1;
                *device = &dev->device.common;
            }
        }
    }
    return status;
}
