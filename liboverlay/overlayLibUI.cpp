/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
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

#include "overlayLibUI.h"
#include "gralloc_priv.h"
#define LOG_TAG "OverlayUI"

using android::sp;
using gralloc::IMemAlloc;
using gralloc::alloc_data;

namespace {
/* helper functions */
bool checkOVState(int w, int h, int format, int orientation,
                            int zorder, const mdp_overlay& ov) {
    switch(orientation) {
        case HAL_TRANSFORM_ROT_90:
        case HAL_TRANSFORM_ROT_270: {
            int tmp = w;
            w = h;
            h = tmp;
            break;
        }
        default:
            break;
    }

    int srcw = (w + 31) & ~31;
    int srch = (h + 31) & ~31;
    bool displayAttrsCheck = ((srcw == ov.src.width) && (srch == ov.src.height) &&
                                 (format == ov.src.format));
    bool zOrderCheck = (ov.z_order == zorder);

    if (displayAttrsCheck && zorder == overlay::NO_INIT)
        return true;

    if (displayAttrsCheck && zorder != overlay::NO_INIT
                            && ov.z_order == zorder)
        return true;
    return false;
}

void swapOVRotWidthHeight(msm_rotator_img_info& rotInfo,
                                 mdp_overlay& ovInfo) {
    int srcWidth = ovInfo.src.width;
    ovInfo.src.width = ovInfo.src.height;
    ovInfo.src.height = srcWidth;

    int srcRectWidth = ovInfo.src_rect.w;
    ovInfo.src_rect.w = ovInfo.src_rect.h;
    ovInfo.src_rect.h = srcRectWidth;

    int dstWidth = rotInfo.dst.width;
    rotInfo.dst.width = rotInfo.dst.height;
    rotInfo.dst.height = dstWidth;
}

void setupOvRotInfo(int w, int h, int format, int orientation,
                             mdp_overlay& ovInfo, msm_rotator_img_info& rotInfo) {
    memset(&ovInfo, 0, sizeof(ovInfo));
    memset(&rotInfo, 0, sizeof(rotInfo));
    ovInfo.id = MSMFB_NEW_REQUEST;
    int srcw = (w + 31) & ~31;
    int srch = (h + 31) & ~31;
    ovInfo.src.width = srcw;
    ovInfo.src.height = srch;
    ovInfo.src.format = format;
    ovInfo.src_rect.w = w;
    ovInfo.src_rect.h = h;
    ovInfo.alpha = 0xff;
    ovInfo.transp_mask = 0xffffffff;
    rotInfo.src.format = format;
    rotInfo.dst.format = format;
    rotInfo.src.width = srcw;
    rotInfo.src.height = srch;
    rotInfo.src_rect.w = srcw;
    rotInfo.src_rect.h = srch;
    rotInfo.dst.width = srcw;
    rotInfo.dst.height = srch;

    int rot = orientation;
    switch(rot) {
        case 0:
        case HAL_TRANSFORM_FLIP_H:
        case HAL_TRANSFORM_FLIP_V:
            rot = 0;
            break;
        case HAL_TRANSFORM_ROT_90:
        case (HAL_TRANSFORM_ROT_90|HAL_TRANSFORM_FLIP_H):
        case (HAL_TRANSFORM_ROT_90|HAL_TRANSFORM_FLIP_V): {
            int tmp = ovInfo.src_rect.x;
            ovInfo.src_rect.x = ovInfo.src.height -
                                 (ovInfo.src_rect.y + ovInfo.src_rect.h);
            ovInfo.src_rect.y = tmp;
            swapOVRotWidthHeight(rotInfo, ovInfo);
            rot = HAL_TRANSFORM_ROT_90;
            break;
        }
        case HAL_TRANSFORM_ROT_180:
            break;
        case HAL_TRANSFORM_ROT_270: {
            int tmp = ovInfo.src_rect.y;
            ovInfo.src_rect.y = ovInfo.src.width -
                                   (ovInfo.src_rect.x + ovInfo.src_rect.w);
            ovInfo.src_rect.x = tmp;
            swapOVRotWidthHeight(rotInfo, ovInfo);
            break;
        }
        default:
            break;
    }

    int mdp_rotation = overlay::get_mdp_orientation(rot);
    if (mdp_rotation < 0)
        mdp_rotation = 0;
    ovInfo.user_data[0] = mdp_rotation;
    rotInfo.rotations = ovInfo.user_data[0];
    if (mdp_rotation)
        rotInfo.enable = 1;
    ovInfo.dst_rect.w = ovInfo.src_rect.w;
    ovInfo.dst_rect.h = ovInfo.src_rect.h;
}

bool isRGBType(int format) {
    bool ret = false;
    switch(format) {
        case MDP_RGBA_8888:
        case MDP_BGRA_8888:
        case MDP_RGBX_8888:
        case MDP_RGB_565:
            ret = true;
            break;
        default:
            ret = false;
            break;
    }
    return ret;
}

int getRGBBpp(int format) {
    int ret = -1;
    switch(format) {
        case MDP_RGBA_8888:
        case MDP_BGRA_8888:
        case MDP_RGBX_8888:
            ret = 4;
            break;
        case MDP_RGB_565:
            ret = 2;
            break;
        default:
            ret = -1;
            break;
    }

    return ret;
}

bool turnOFFVSync() {
    static int swapIntervalPropVal = -1;
    if (swapIntervalPropVal == -1) {
        char pval[PROPERTY_VALUE_MAX];
        property_get("debug.gr.swapinterval", pval, "1");
        swapIntervalPropVal = atoi(pval);
    }
    return (swapIntervalPropVal == 0);
}

};

namespace overlay {

status_t Display::openDisplay(int fbnum) {
    if (mFD != NO_INIT)
        return ALREADY_EXISTS;

    status_t ret = NO_INIT;
    char const * const device_template =
                        "/dev/graphics/fb%u";
    char dev_name[64];
    snprintf(dev_name, 64, device_template, fbnum);

    mFD = open(dev_name, O_RDWR, 0);
    if (mFD < 0) {
        LOGE("Failed to open FB %d", fbnum);
        return ret;
    }

    fb_var_screeninfo vinfo;
    if (ioctl(mFD, FBIOGET_VSCREENINFO, &vinfo)) {
        LOGE("FBIOGET_VSCREENINFO on failed on FB %d", fbnum);
        close(mFD);
        mFD = NO_INIT;
        return ret;
    }

    mFBWidth = vinfo.xres;
    mFBHeight = vinfo.yres;
    mFBBpp = vinfo.bits_per_pixel;
    ret = NO_ERROR;

    return ret;
}

status_t OVHelper::startOVSession(mdp_overlay& ovInfo, int fbnum) {
    status_t ret = NO_INIT;

    if (mSessionID == NO_INIT) {
        ret = mobjDisplay.openDisplay(fbnum);
        if (ret != NO_ERROR)
            return ret;

        if (ioctl(mobjDisplay.getFD(), MSMFB_OVERLAY_SET, &ovInfo)) {
            LOGE("OVerlay set failed..");
            mobjDisplay.closeDisplay();
            ret = BAD_VALUE;
        }
        else {
            mSessionID = ovInfo.id;
            mOVInfo = ovInfo;
            ret  = NO_ERROR;
        }
    }
    else
        ret = ALREADY_EXISTS;

    return ret;
}

status_t OVHelper::queueBuffer(msmfb_overlay_data ovData) {
    if (mSessionID == NO_INIT)
        return NO_INIT;

    ovData.id = mSessionID;
    if (ioctl(mobjDisplay.getFD(), MSMFB_OVERLAY_PLAY, &ovData))
        return BAD_VALUE;

    return NO_ERROR;
}

status_t OVHelper::closeOVSession() {
    if (mSessionID != NO_INIT) {
        ioctl(mobjDisplay.getFD(), MSMFB_OVERLAY_UNSET, &mSessionID);
        mobjDisplay.closeDisplay();
    }

    mSessionID = NO_INIT;

    return NO_ERROR;
}

status_t OVHelper::setPosition(int x, int y, int w, int h) {
    status_t ret = BAD_VALUE;
    if (mSessionID != NO_INIT) {
        int fd = mobjDisplay.getFD();
        if (x < 0 || y < 0 || ((x + w) > getFBWidth())) {
            LOGE("set position failed, invalid argument");
            return ret;
        }

        mdp_overlay ov;
        ov.id = mSessionID;
        if (!ioctl(fd, MSMFB_OVERLAY_GET, &ov)) {
            if (x != ov.dst_rect.x || y != ov.dst_rect.y ||
                      w != ov.dst_rect.w || h != ov.dst_rect.h) {
                ov.dst_rect.x = x;
                ov.dst_rect.y = y;
                ov.dst_rect.w = w;
                ov.dst_rect.h = h;
                if (ioctl(fd, MSMFB_OVERLAY_SET, &ov)) {
                    LOGE("set position failed");
                    return ret;
                }
            }
            mOVInfo = ov;
            return NO_ERROR;
        }
        return ret;
    }

    return NO_INIT;
}

status_t OVHelper::getOVInfo(mdp_overlay& ovInfo) {
    if (mSessionID == NO_INIT)
        return NO_INIT;
    ovInfo = mOVInfo;
    return NO_ERROR;
}

status_t Rotator::startRotSession(msm_rotator_img_info& rotInfo,
                                   int size, int numBuffers) {
    status_t ret = ALREADY_EXISTS;
    if (mSessionID == NO_INIT && mFD == NO_INIT) {
        mNumBuffers = numBuffers;
        mFD = open("/dev/msm_rotator", O_RDWR, 0);
        if (mFD < 0) {
            LOGE("Couldnt open rotator device");
            return NO_INIT;
        }

        if (int check = ioctl(mFD, MSM_ROTATOR_IOCTL_START, &rotInfo)) {
            close(mFD);
            mFD = NO_INIT;
            return NO_INIT;
        }

#ifdef USE_ION
        alloc_data data;
        data.base = 0;
        data.fd = -1;
        data.offset = 0;
        data.size = mSize * mNumBuffers;
        data.align = getpagesize();
        data.uncached = true;

        int err = mAlloc->allocate(data, GRALLOC_USAGE_PRIVATE_ADSP_HEAP|
                                         GRALLOC_USAGE_PRIVATE_SMI_HEAP, 0);

        if(err) {
            LOGE("Cant allocate from ION");
            closeRotSession();
            return NO_INIT;
        }
        mPmemFD = data.fd;
        mPmemAddr = data.base;
        mBufferType = data.allocType;
#else
        mSessionID = rotInfo.session_id;
        mPmemFD = open("/dev/pmem_adsp", O_RDWR | O_SYNC);
        if (mPmemFD < 0) {
            closeRotSession();
            return NO_INIT;
        }

        mSize = size;
        mPmemAddr = (void *) mmap(NULL, mSize* mNumBuffers, PROT_READ | PROT_WRITE,
                                                     MAP_SHARED, mPmemFD, 0);
        if (mPmemAddr == MAP_FAILED) {
            closeRotSession();
            return NO_INIT;
        }
#endif

        mCurrentItem = 0;
        for (int i = 0; i < mNumBuffers; i++)
            mRotOffset[i] = i * mSize;
        ret = NO_ERROR;
    }

    return ret;
}

status_t Rotator::closeRotSession() {
    if (mSessionID != NO_INIT && mFD != NO_INIT) {
        ioctl(mFD, MSM_ROTATOR_IOCTL_FINISH, &mSessionID);
        close(mFD);
#ifdef USE_ION
        sp<IMemAlloc> memalloc = mAlloc->getAllocator(mBufferType);
        memalloc->free_buffer(mPmemAddr, mSize * mNumBuffers, 0, mPmemFD);
#else
        munmap(mPmemAddr, mSize * mNumBuffers);
        close(mPmemFD);
#endif
        close(mPmemFD);
    }

    mFD = NO_INIT;
    mSessionID = NO_INIT;
    mPmemFD = NO_INIT;
    mPmemAddr = MAP_FAILED;

    return NO_ERROR;
}

status_t Rotator::rotateBuffer(msm_rotator_data_info& rotData) {
    status_t ret = NO_INIT;
    if (mSessionID != NO_INIT) {
        rotData.dst.memory_id = mPmemFD;
        rotData.dst.offset = mRotOffset[mCurrentItem];
        rotData.session_id = mSessionID;
        mCurrentItem = (mCurrentItem + 1) % mNumBuffers;
        if (ioctl(mFD, MSM_ROTATOR_IOCTL_ROTATE, &rotData)) {
            LOGE("Rotator failed to rotate");
            return BAD_VALUE;
        }
        return NO_ERROR;
    }

    return ret;
}

status_t OverlayUI::closeChannel() {
    mobjOVHelper.closeOVSession();
    mobjRotator.closeRotSession();
    mChannelState = CLOSED;
    return NO_ERROR;
}

status_t OverlayUI::setSource(const overlay_buffer_info& info, int orientation,
                                             bool useVGPipe, bool ignoreFB,
                                             int fbnum, int zorder) {
    status_t ret = NO_INIT;

    int format3D = FORMAT_3D(info.format);
    int colorFormat = COLOR_FORMAT(info.format);
    int format = get_mdp_format(colorFormat);

    if (format3D || !isRGBType(format))
        return ret;

    if (mChannelState == PENDING_CLOSE)
        closeChannel();

    if (mChannelState == UP) {
        mdp_overlay ov;
        if (mobjOVHelper.getOVInfo(ov) == NO_ERROR) {
            if (mOrientation == orientation &&
                   mFBNum == fbnum &&
                   checkOVState(info.width, info.height, format, orientation, zorder, ov))
                return NO_ERROR;
            else
                mChannelState = PENDING_CLOSE;
        }
        else
            mChannelState = PENDING_CLOSE;
        return ret;
    }

    mOrientation = orientation;
    mdp_overlay ovInfo;
    msm_rotator_img_info rotInfo;
    setupOvRotInfo(info.width, info.height, format, orientation, ovInfo, rotInfo);

    int flags = 0;
    if (ignoreFB)
        ovInfo.is_fg = 1;
    else
        flags |= MDP_OV_PLAY_NOWAIT;

    if (turnOFFVSync())
        flags |= MDP_OV_PLAY_NOWAIT;

    if (useVGPipe ||
          (fbnum == FB0 && getRGBBpp(format) != mobjOVHelper.getFBBpp()))
        flags |= MDP_OV_PIPE_SHARE;

    ovInfo.flags = flags;
    if (zorder != NO_INIT)
        ovInfo.z_order = zorder;

    ret = startChannel(fbnum, ovInfo, rotInfo, info.size);
    return ret;
}

status_t OverlayUI::startChannel(int fbnum, mdp_overlay& ovInfo,
                                 msm_rotator_img_info& rotInfo, int size) {
    status_t ret = BAD_VALUE;
    if (mChannelState == UP)
        return ret;

    ret = mobjOVHelper.startOVSession(ovInfo, fbnum);
    if (ret == NO_ERROR && mOrientation) {
        ret = mobjRotator.startRotSession(rotInfo, size);
    }

    if (ret == NO_ERROR) {
        mChannelState = UP;
        mFBNum = fbnum;
    }
    else
        LOGE("start channel failed.");

    return ret;
}

status_t OverlayUI::queueBuffer(buffer_handle_t buffer) {
    status_t ret = NO_INIT;

    if (mChannelState != UP)
        return ret;

    msmfb_overlay_data ovData;
    memset(&ovData, 0, sizeof(ovData));

    private_handle_t const* hnd = reinterpret_cast
                                        <private_handle_t const*>(buffer);
    ovData.data.memory_id = hnd->fd;
    ovData.data.offset = hnd->offset;
    if (mOrientation) {
        msm_rotator_data_info rotData;
        memset(&rotData, 0, sizeof(rotData));
        rotData.src.memory_id = hnd->fd;
        rotData.src.offset = hnd->offset;
        if (mobjRotator.rotateBuffer(rotData) != NO_ERROR) {
            LOGE("Rotator failed.. ");
            return BAD_VALUE;
        }
        ovData.data.memory_id = rotData.dst.memory_id;
        ovData.data.offset = rotData.dst.offset;
    }

    ret = mobjOVHelper.queueBuffer(ovData);
    if (ret != NO_ERROR)
        LOGE("Queuebuffer failed ");

    return ret;
}

};
