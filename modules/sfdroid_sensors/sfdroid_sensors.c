/*
 * Copyright (C) 2009 The Android Open Source Project
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

#define LOG_TAG "sfdroidsensors"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <cutils/log.h>
#include <cutils/native_handle.h>
#include <cutils/sockets.h>
#include <hardware/sensors.h>

#include <sys/socket.h>
#include <sys/un.h>

#define SFDROID_ROOT "/tmp/sfdroid"
#define SENSORS_HANDLE_FILE (SFDROID_ROOT "/sensors_handle")

int connect_to_sfdroid()
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(fd < 0)
    {
        ALOGE("error creating socket stream");
        return -1;
    }

    // don't crash if we disconnect
    //int set = 1;
    //setsockopt(sd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
    signal(SIGPIPE, SIG_IGN);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SENSORS_HANDLE_FILE, sizeof(addr.sun_path)-1);

    if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        ALOGE("error connecting to sfdroidsensors: %s", strerror(errno));
        usleep(100000);
        close(fd);
        return -1;
    }

    struct timeval timeout;
    memset(&timeout, 0, sizeof(timeout));

    timeout.tv_sec = 1;

    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
    {
        ALOGE("failed to set timeout on sensor socket: %s", strerror(errno));
    }

    return fd;
}

#if 0
#define  D(...)  ALOGD(__VA_ARGS__)
#else
#define  D(...)  ((void)0)
#endif

#define  E(...)  ALOGE(__VA_ARGS__)

/** SENSOR IDS AND NAMES
 **/

#define MAX_NUM_SENSORS 5

#define SUPPORTED_SENSORS  ((1<<MAX_NUM_SENSORS)-1)

#define  ID_BASE           SENSORS_HANDLE_BASE
#define  ID_ACCELERATION   (ID_BASE+0)
#define  ID_MAGNETIC_FIELD (ID_BASE+1)
#define  ID_ORIENTATION    (ID_BASE+2)
#define  ID_TEMPERATURE    (ID_BASE+3)
#define  ID_PROXIMITY      (ID_BASE+4)

#define  SENSORS_ACCELERATION   (1 << ID_ACCELERATION)
#define  SENSORS_MAGNETIC_FIELD  (1 << ID_MAGNETIC_FIELD)
#define  SENSORS_ORIENTATION     (1 << ID_ORIENTATION)
#define  SENSORS_TEMPERATURE     (1 << ID_TEMPERATURE)
#define  SENSORS_PROXIMITY       (1 << ID_PROXIMITY)

#define  ID_CHECK(x)  ((unsigned)((x)-ID_BASE) < MAX_NUM_SENSORS)

#define  SENSORS_LIST  \
    SENSOR_(ACCELERATION,"acceleration") \
    SENSOR_(MAGNETIC_FIELD,"magnetic-field") \
    SENSOR_(ORIENTATION,"orientation") \
    SENSOR_(TEMPERATURE,"temperature") \
    SENSOR_(PROXIMITY,"proximity") \

static const struct {
    const char*  name;
    int          id; } _sensorIds[MAX_NUM_SENSORS] =
{
#define SENSOR_(x,y)  { y, ID_##x },
    SENSORS_LIST
#undef  SENSOR_
};

static const char*
_sensorIdToName( int  id )
{
    int  nn;
    for (nn = 0; nn < MAX_NUM_SENSORS; nn++)
        if (id == _sensorIds[nn].id)
            return _sensorIds[nn].name;
    return "<UNKNOWN>";
}

static int
_sensorIdFromName( const char*  name )
{
    int  nn;

    if (name == NULL)
        return -1;

    for (nn = 0; nn < MAX_NUM_SENSORS; nn++)
        if (!strcmp(name, _sensorIds[nn].name))
            return _sensorIds[nn].id;

    return -1;
}

/** SENSORS POLL DEVICE
 **/

typedef struct SensorPoll {
    struct sensors_poll_device_t  device;
    int                           fd;
    int64_t                       delay;
} SensorPoll;

/** SENSORS POLL DEVICE FUNCTIONS **/

static int poll__close(struct hw_device_t* dev)
{
    SensorPoll*  ctl = (void*)dev;
    if (ctl->fd >= 0) {
        close(ctl->fd);
    }
    free(ctl);
    return 0;
}

static int poll__poll(struct sensors_poll_device_t *dev,
            sensors_event_t* data, int count)
{
    SensorPoll*  ctl = (void*)dev;
    char buff[256];
    char            command[128];
    int ret;
    int i;
    D("%s: dev=%p data=%p count=%d ", __FUNCTION__, dev, data, count);
    if (ctl->fd < 0) {
        D("%s: OPEN CONNECTION", __FUNCTION__);
        ctl->fd = connect_to_sfdroid();
    }

    if(ctl->fd >= 0)
    {
        for (i = 0; i < count; i++)  {
            int64_t timestamp;
            float params[3];
            int len;
            char syncbuf[1];

            usleep(ctl->delay / 1000);

            snprintf(command, sizeof command, "get:accelerometer");

            syncbuf[0] = strlen(command) + 1;
            ret = send(ctl->fd, syncbuf, 1, MSG_NOSIGNAL);
            if (ret < 0) {
                E("%s: when sending sync byte errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                close(ctl->fd);
                ctl->fd = -1;
                return i;
            }

            ret = send(ctl->fd, command, strlen(command) + 1, MSG_NOSIGNAL);
            if (ret < 0) {
                E("%s: when sending command errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                close(ctl->fd);
                ctl->fd = -1;
                return i;
            }

            len = recv(ctl->fd, syncbuf, 1, 0);
            if(len < 0)
            {
                ALOGE("%s recv failed", __FUNCTION__);
                close(ctl->fd);
                ctl->fd = -1;
                return i;
            }

            len = recv(ctl->fd, buff, syncbuf[0], 0);
            if(len < 0)
            {
                ALOGE("%s recv failed", __FUNCTION__);
                close(ctl->fd);
                ctl->fd = -1;
                return i;
            }
            buff[len] = 0;

            /* "acceleration:<x>:<y>:<z>" corresponds to an acceleration event */
            if (sscanf(buff, "acceleration:%g:%g:%g:%lld", params+0, params+1, params+2, &timestamp) == 4) {
                data->sensor = ID_ACCELERATION;
                data->version = sizeof(*data);
                data->acceleration.x = params[0];
                data->acceleration.y = params[1];
                data->acceleration.z = params[2];
                data->timestamp = timestamp;
                data++;
                continue;
            }

            ALOGE("unsupported command: %s", buff);
            return i;
        }

        return count;
    }

    // sfdroid not up
    return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev,
            int handle, int enabled)
{
    int ret;
    char            command[128];
    native_handle_t* hdl;
    SensorPoll*  ctl = (void*)dev;
    D("%s: dev=%p handle=%x enable=%d ", __FUNCTION__, dev, handle, enabled);
    if (ctl->fd < 0) {
        D("%s: OPEN CONNECTION", __FUNCTION__);
        ctl->fd = connect_to_sfdroid();
    }
    if(handle == ID_ACCELERATION)
    {
        if(ctl->fd >= 0)
        {
            char syncbuf[1];
            snprintf(command, sizeof command, "set:%s:%d",
                        _sensorIdToName(handle), enabled != 0);

            syncbuf[0] = strlen(command) + 1;
            ret = send(ctl->fd, syncbuf, 1, MSG_NOSIGNAL);
            if (ret < 0) {
                E("%s: when sending sync byte errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                close(ctl->fd);
                ctl->fd = -1;
                return -1;
            }

            ret = send(ctl->fd, command, strlen(command) + 1, MSG_NOSIGNAL);
            if (ret < 0) {
                close(ctl->fd);
                ctl->fd = -1;
                E("%s: when sending command errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                return -1;
            }
        }

        // sfdroid not up yet
        return 0;
    }
    return -1;
}

static int poll__setDelay(struct sensors_poll_device_t *dev,
            int handle, int64_t ns)
{
    int ret;
    char            command[128];
    native_handle_t* hdl;
    SensorPoll*  ctl = (void*)dev;
    D("%s: dev=%p handle=%x enable=%d ", __FUNCTION__, dev, handle, enabled);
    if (ctl->fd < 0) {
        D("%s: OPEN CONNECTION", __FUNCTION__);
        ctl->fd = connect_to_sfdroid();
    }
    ctl->delay = ns;
    if(handle == ID_ACCELERATION)
    {
        if(ctl->fd >= 0)
        {
            char syncbuf[1];

            snprintf(command, sizeof command, "setDelay:%s:%lld",
                        _sensorIdToName(handle), ns);

            syncbuf[0] = strlen(command) + 1;
            ret = send(ctl->fd, syncbuf, 1, MSG_NOSIGNAL);
            if (ret < 0) {
                E("%s: when sending sync byte errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                close(ctl->fd);
                ctl->fd = -1;
                return -1;
            }

            ret = send(ctl->fd, command, strlen(command) + 1, MSG_NOSIGNAL);
            if (ret < 0) {
                close(ctl->fd);
                ctl->fd = -1;
                E("%s: when sending command errno=%d: %s", __FUNCTION__, errno, strerror(errno));
                return -1;
            }
        }

        // sfdroid not up yet
        return 0;
    }
    return -1;
}

/** MODULE REGISTRATION SUPPORT
 **
 ** This is required so that hardware/libhardware/hardware.c
 ** will dlopen() this library appropriately.
 **/

/*
 * the following is the list of all supported sensors.
 * this table is used to build sSensorList declared below
 * according to which hardware sensors are reported as
 * available from the emulator (see get_sensors_list below)
 *
 */
static const struct sensor_t sSensorListInit[] = {
        { .name       = "sfdroid 3-axis Accelerometer",
          .vendor     = "sfdroid",
          .version    = 1,
          .handle     = ID_ACCELERATION,
          .type       = SENSOR_TYPE_ACCELEROMETER,
          .maxRange   = 500.f, // dummy
          .resolution = 1.f/2000.f, // dummy
          .power      = 3.0f,
          .reserved   = {}
        },
#if 0
        { .name       = "Goldfish 3-axis Magnetic field sensor",
          .vendor     = "The Android Open Source Project",
          .version    = 1,
          .handle     = ID_MAGNETIC_FIELD,
          .type       = SENSOR_TYPE_MAGNETIC_FIELD,
          .maxRange   = 2000.0f,
          .resolution = 1.0f,
          .power      = 6.7f,
          .reserved   = {}
        },

        { .name       = "Goldfish Orientation sensor",
          .vendor     = "The Android Open Source Project",
          .version    = 1,
          .handle     = ID_ORIENTATION,
          .type       = SENSOR_TYPE_ORIENTATION,
          .maxRange   = 360.0f,
          .resolution = 1.0f,
          .power      = 9.7f,
          .reserved   = {}
        },

        { .name       = "Goldfish Temperature sensor",
          .vendor     = "The Android Open Source Project",
          .version    = 1,
          .handle     = ID_TEMPERATURE,
          .type       = SENSOR_TYPE_TEMPERATURE,
          .maxRange   = 80.0f,
          .resolution = 1.0f,
          .power      = 0.0f,
          .reserved   = {}
        },

        { .name       = "Goldfish Proximity sensor",
          .vendor     = "The Android Open Source Project",
          .version    = 1,
          .handle     = ID_PROXIMITY,
          .type       = SENSOR_TYPE_PROXIMITY,
          .maxRange   = 1.0f,
          .resolution = 1.0f,
          .power      = 20.0f,
          .reserved   = {}
        },
#endif
};

static struct sensor_t  sSensorList[MAX_NUM_SENSORS];

static int sensors__get_sensors_list(struct sensors_module_t* module,
        struct sensor_t const** list)
{
    // only accelerometer for now
    sSensorList[0] = sSensorListInit[0];
    *list = sSensorList;
    return 1;
}


static int
open_sensors(const struct hw_module_t* module,
             const char*               name,
             struct hw_device_t*      *device)
{
    int  status = -EINVAL;

    D("%s: name=%s", __FUNCTION__, name);

    if (!strcmp(name, SENSORS_HARDWARE_POLL)) {
        SensorPoll *dev = malloc(sizeof(*dev));

        memset(dev, 0, sizeof(*dev));

        dev->device.common.tag     = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module  = (struct hw_module_t*) module;
        dev->device.common.close   = poll__close;
        dev->device.poll           = poll__poll;
        dev->device.activate       = poll__activate;
        dev->device.setDelay       = poll__setDelay;
        dev->fd                    = -1;

        *device = &dev->device.common;
        status  = 0;
    }
    return status;
}


static struct hw_module_methods_t sensors_module_methods = {
    .open = open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = SFDROID_SENSORS_HARDWARE_MODULE_ID,
        .name = "sfdroid SENSORS Module",
        .author = "The Android Open Source Project",
        .methods = &sensors_module_methods,
    },
    .get_sensors_list = sensors__get_sensors_list
};
