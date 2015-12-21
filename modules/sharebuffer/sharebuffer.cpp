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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <linux/fb.h>

#define NUM_BUFFERS 2

#define SFDROID_ROOT "/tmp/sfdroid/"
#define SHM_BUFFER_HANDLE_FILE (SFDROID_ROOT "/gralloc_buffer_handle")

struct buffer_info_t
{
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t pixel_format;
};

int connect_to_renderer()
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0)
    {
        ALOGE("error creating socket stream");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SHM_BUFFER_HANDLE_FILE, sizeof(addr.sun_path)-1);

    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        ALOGE("error connecting to renderer: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int send_native_handle(int fd, const native_handle_t *handle, uint32_t width, uint32_t height, uint32_t stride, int32_t pixel_format)
{
    struct msghdr socket_message;
    struct iovec io_vector[1];
    struct cmsghdr *control_message = NULL;
    unsigned int buffer_size = sizeof(struct buffer_info_t) + sizeof(native_handle_t) + sizeof(int)*(handle->numFds + handle->numInts);
    unsigned int handle_size = sizeof(native_handle_t) + sizeof(int)*(handle->numFds + handle->numInts);
    char message_buffer[buffer_size];
    char ancillary_buffer[CMSG_SPACE(sizeof(int) * handle->numFds)];
    struct buffer_info_t info;

    info.width = width;
    info.height = height;
    info.stride = stride;
    info.pixel_format = pixel_format;

    memcpy(message_buffer, &info, sizeof(struct buffer_info_t));
    memcpy(message_buffer + sizeof(struct buffer_info_t), handle, handle_size);

    io_vector[0].iov_base = message_buffer;
    io_vector[0].iov_len = buffer_size;

    memset(&socket_message, 0, sizeof(struct msghdr));
    socket_message.msg_iov = io_vector;
    socket_message.msg_iovlen = 1;

    memset(ancillary_buffer, 0, CMSG_SPACE(sizeof(int) * handle->numFds));

    socket_message.msg_control = ancillary_buffer;
    socket_message.msg_controllen = CMSG_SPACE(sizeof(int) * handle->numFds);

    control_message = CMSG_FIRSTHDR(&socket_message);
    control_message->cmsg_len = socket_message.msg_controllen;
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;

    for(int i=0;i<handle->numFds;i++)
    {
        ((int*)CMSG_DATA(control_message))[i] = handle->data[i];
    }

    return sendmsg(fd, &socket_message, 0);
}

int recv_status(int fd, int *failed)
{
    char message_buffer[3];

    if(recv(fd, message_buffer, sizeof(message_buffer), MSG_WAITALL) < 0)
    {
        //*failed = 1;
        return -1;
    }

    if(message_buffer[2] != 0)
    {
        ALOGE("message_buffer is not a 0 terminated string");
        return -1;
    }

    if(strcmp(message_buffer, "OK") == 0)
    {
        *failed = 0;
        return 0;
    }
    else
    {
        if(strcmp(message_buffer, "FA") != 0)
        {
            ALOGE("unknown status: %s", message_buffer);
        }
        *failed = 1;
        return -1;
    }

    return -1;
}

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

    int fd_renderer;
};

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

static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer, uint32_t width, uint32_t height, uint32_t stride, int32_t pixel_format)
{
    // TODO: helpers for the sizeofs
    fb_context_t* ctx = (fb_context_t*)dev;

    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);

    if(m->fd_renderer < 0)
    {
        ALOGW("connecting to renderer");
        m->fd_renderer = connect_to_renderer();
    }

    if(m->fd_renderer >= 0)
    {
        int failed;

        if(send_native_handle(m->fd_renderer, buffer, width, height, stride, pixel_format) < 0)
        {
            ALOGW("sending buffer failed: %s", strerror(errno));
            goto exit_error;
        }

        if(recv_status(m->fd_renderer, &failed) < 0)
        {
            ALOGW("recv_status failed: %s", strerror(errno));
            goto exit_error;
        }
    }

    return 0;

exit_error:
    close(m->fd_renderer);
    m->fd_renderer = -1;
    return -1;
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

    // fb_post will this. but only if set like this.
    module->fd_renderer = -1;

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

static int sharebuffer_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

int sharebuffer_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

int sharebuffer_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

int sharebuffer_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

int sharebuffer_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

static struct hw_module_methods_t sharebuffer_module_methods = {
        open: sharebuffer_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: SHAREBUFFER_HARDWARE_MODULE_ID,
            name: "sharebuffer",
            author: "krnlyng",
            methods: &sharebuffer_module_methods
        },
        registerBuffer: sharebuffer_register_buffer,
        unregisterBuffer: sharebuffer_unregister_buffer,
        lock: sharebuffer_lock,
        unlock: sharebuffer_unlock,
    },
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: 0,
};

static int sharebuffer_alloc(alloc_device_t* dev,
        int w, int h, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    ALOGW("sharebuffer_alloc stub");
    return 0;
}

static int sharebuffer_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    ALOGW("sharebuffer_free stub");
    return 0;
}

int sharebuffer_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr)
{
    ALOGW("sharebuffer_lock stub");
    return 0;
}

int sharebuffer_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle)
{
    ALOGW("sharebuffer_unlock stub");
    return 0;
}

int sharebuffer_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    ALOGW("sharebuffer_register_buffer stub");
    return 0;
}

int sharebuffer_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle)
{
    ALOGW("sharebuffer_unregister_buffer stub");
    return 0;
}

static int sharebuffer_close(struct hw_device_t *dev)
{
    ALOGW("sharebuffer_close stub");
    return 0;
}

int sharebuffer_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        ALOGE("FATAL: Tried to load sharebuffer module with %s as argument.", name);
    } else {
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
    return status;
}
