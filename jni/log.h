#pragma once
#include <android/log.h>

#define CFG_TAG "cleanfg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  CFG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  CFG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, CFG_TAG, __VA_ARGS__)
#define LOGD(...) do { if (cleanfg::g_config.debug) \
    __android_log_print(ANDROID_LOG_DEBUG, CFG_TAG, __VA_ARGS__); } while (0)
