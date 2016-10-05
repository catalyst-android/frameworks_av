LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        SoftVPXEncoder.cpp \
        SoftVP8Encoder.cpp \
        SoftVP9Encoder.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/external/libvpx/libvpx \
        $(TOP)/external/libvpx/libvpx/vpx_codec \
        $(TOP)/external/libvpx/libvpx/vpx_ports \
        frameworks/av/media/libstagefright/include \
        frameworks/native/include/media/openmax \

LOCAL_SANITIZE := signed-integer-overflow unsigned-integer-overflow

LOCAL_STATIC_LIBRARIES := \
        libvpx

LOCAL_SHARED_LIBRARIES := \
        libmedia libstagefright_omx libstagefright_foundation libutils liblog \
        libhardware \

LOCAL_MODULE := libstagefright_soft_vpxenc
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
