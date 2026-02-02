LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := mockgps
LOCAL_SRC_FILES := module.cpp
LOCAL_LDLIBS    := -llog -ldl
LOCAL_CFLAGS    := -Os -fvisibility=hidden -ffunction-sections -fdata-sections -Wall -Wextra
LOCAL_LDFLAGS   := -Wl,--gc-sections
LOCAL_CPP_FEATURES := exceptions
include $(BUILD_SHARED_LIBRARY)
