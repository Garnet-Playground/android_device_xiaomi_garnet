/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "UdfpsHandler.garnet"

#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <fstream>
#include <thread>

#include <display/drm/mi_disp.h>

#include "UdfpsHandler.h"
#include "xiaomi_touch.h"

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define COMMAND_FOD_PRESS_STATUS 1
#define COMMAND_FOD_PRESS_X 2
#define COMMAND_FOD_PRESS_Y 3
#define PARAM_FOD_PRESSED 1
#define PARAM_FOD_RELEASED 0

#define FOD_STATUS_OFF 0
#define FOD_STATUS_ON 1

#define TOUCH_DEV_PATH "/dev/xiaomi-touch"
#define TOUCH_MAGIC 'T'
#define TOUCH_IOC_SET_CUR_VALUE _IO(TOUCH_MAGIC, SET_CUR_VALUE)
#define TOUCH_IOC_GET_CUR_VALUE _IO(TOUCH_MAGIC, GET_CUR_VALUE)

#define DISP_FEATURE_PATH "/dev/mi_display/disp_feature"

#define FOD_PRESS_STATUS_PATH "/sys/class/touch/touch_dev/fod_press_status"

namespace {

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

static disp_event_resp* parseDispEvent(int fd) {
    disp_event header;
    ssize_t headerSize = read(fd, &header, sizeof(header));
    if (headerSize < sizeof(header)) {
        LOG(ERROR) << "unexpected display event header size: " << headerSize;
        return nullptr;
    }

    struct disp_event_resp* response =
            reinterpret_cast<struct disp_event_resp*>(malloc(header.length));
    response->base = header;
    int dataLength = response->base.length - sizeof(response->base);
    ssize_t dataSize = read(fd, &response->data, dataLength);
    if (dataSize < dataLength) {
        LOG(ERROR) << "unexpected display event data size: " << dataSize;
        return nullptr;
    }

    return response;
}

}  // anonymous namespace

class XiaomiGarnetUdfpsHander : public UdfpsHandler {
  public:
    void init(fingerprint_device_t* device) {
        mDevice = device;
        touch_fd_ = android::base::unique_fd(open(TOUCH_DEV_PATH, O_RDWR));
        disp_fd_ = android::base::unique_fd(open(DISP_FEATURE_PATH, O_RDWR));

        // Thread to notify fingeprint hwmodule about fod presses
        std::thread([this]() {
            int fd = open(FOD_PRESS_STATUS_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << FOD_PRESS_STATUS_PATH << " , err: " << fd;
                return;
            }

            struct pollfd fodPressStatusPoll = {
                    .fd = fd,
                    .events = POLLERR | POLLPRI,
                    .revents = 0,
            };

            while (true) {
                int rc = poll(&fodPressStatusPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    continue;
                }

                bool pressed = readBool(fd);
                mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_STATUS,
                                pressed ? PARAM_FOD_PRESSED : PARAM_FOD_RELEASED);

                // Request HBM
                disp_local_hbm_req req;
                req.base.flag = 0;
                req.base.disp_id = MI_DISP_PRIMARY;
                req.local_hbm_value = pressed ? LHBM_TARGET_BRIGHTNESS_WHITE_1000NIT
                                              : LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
                ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);
            }
        }).detach();

        // Thread to listen for fod ui changes
        std::thread([this]() {
            int fd = open(DISP_FEATURE_PATH, O_RDWR);
            if (fd < 0) {
                LOG(ERROR) << "failed to open " << DISP_FEATURE_PATH << " , err: " << fd;
                return;
            }

            // Register for FOD events
            disp_event_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.type = MI_DISP_EVENT_FOD;
            ioctl(fd, MI_DISP_IOCTL_REGISTER_EVENT, &req);

            struct pollfd dispEventPoll = {
                    .fd = fd,
                    .events = POLLIN,
                    .revents = 0,
            };

            while (true) {
                int rc = poll(&dispEventPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll " << FOD_PRESS_STATUS_PATH << ", err: " << rc;
                    continue;
                }

                struct disp_event_resp* response = parseDispEvent(fd);
                if (response == nullptr) {
                    continue;
                }

                if (response->base.type != MI_DISP_EVENT_FOD) {
                    LOG(ERROR) << "unexpected display event: " << response->base.type;
                    continue;
                }

                int value = response->data[0];
                LOG(DEBUG) << "received data: " << std::bitset<8>(value);

                bool localHbmUiReady = value & LOCAL_HBM_UI_READY;

                mDevice->extCmd(mDevice, COMMAND_NIT,
                                localHbmUiReady ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }).detach();
    }

    void onFingerDown(uint32_t x, uint32_t y, float /*minor*/, float /*major*/) {
        LOG(INFO) << __func__ << "x: " << x << ", y: " << y;
        // Track x and y coordinates
        lastPressX = x;
        lastPressY = y;
        // Ensure touchscreen is aware of the press state, ideally this is not needed
        setFingerDown(true);
    }

    void onFingerUp() {
        LOG(INFO) << __func__;
        // Ensure touchscreen is aware of the press state, ideally this is not needed
        setFingerDown(false);
    }

    void onAcquired(int32_t result, int32_t vendorCode) {
        LOG(INFO) << __func__ << " result: " << result << " vendorCode: " << vendorCode;
        if (result == FINGERPRINT_ACQUIRED_GOOD) {
            // Request to disable HBM already, even if the finger is still pressed
            disp_local_hbm_req req;
            req.base.flag = 0;
            req.base.disp_id = MI_DISP_PRIMARY;
            req.local_hbm_value = LHBM_TARGET_BRIGHTNESS_OFF_FINGER_UP;
            ioctl(disp_fd_.get(), MI_DISP_IOCTL_SET_LOCAL_HBM, &req);

            if (!enrolling) {
                setFodStatus(FOD_STATUS_OFF);
            }
        }

        /* vendorCode
         * 21: waiting for finger
         * 22: finger down
         * 23: finger up
         */
        if (vendorCode == 21) {
            setFodStatus(FOD_STATUS_ON);
        }
    }

    void cancel() {
        LOG(INFO) << __func__;
        enrolling = false;

        setFodStatus(FOD_STATUS_OFF);
    }

    void preEnroll() {
        LOG(INFO) << __func__;
        enrolling = true;
    }

    void enroll() {
        LOG(INFO) << __func__;
        enrolling = true;
    }

    void postEnroll() {
        LOG(INFO) << __func__;
        enrolling = false;

        setFodStatus(FOD_STATUS_OFF);
    }

  private:
    fingerprint_device_t* mDevice;
    android::base::unique_fd touch_fd_;
    android::base::unique_fd disp_fd_;
    uint32_t lastPressX, lastPressY;
    bool enrolling = false;

    void setFodStatus(int value) {
        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, value};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }

    void setFingerDown(bool pressed) {
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_X, pressed ? lastPressX : 0);
        mDevice->extCmd(mDevice, COMMAND_FOD_PRESS_Y, pressed ? lastPressY : 0);

        int buf[MAX_BUF_SIZE] = {MI_DISP_PRIMARY, Touch_Fod_Enable, pressed ? 1 : 0};
        ioctl(touch_fd_.get(), TOUCH_IOC_SET_CUR_VALUE, &buf);
    }
};

static UdfpsHandler* create() {
    return new XiaomiGarnetUdfpsHander();
}

static void destroy(UdfpsHandler* handler) {
    delete handler;
}

extern "C" UdfpsHandlerFactory UDFPS_HANDLER_FACTORY = {
        .create = create,
        .destroy = destroy,
};
