LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:=  nativeDecoder.cpp rtp.c tsparse.c

LOCAL_SHARED_LIBRARIES := \
	liblog libutils libbinder libgui libcutils libui \
	libstagefright libstagefright_foundation libmedia libmedia_native

LOCAL_C_INCLUDES:= \
	frameworks/av/media/libstagefright

LOCAL_CFLAGS += -Wno-multichar

LOCAL_MODULE_TAGS := debug

LOCAL_MODULE:= nativeDecoder

include $(BUILD_EXECUTABLE)
