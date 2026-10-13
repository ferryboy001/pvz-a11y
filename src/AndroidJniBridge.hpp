// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#if defined(__ANDROID__)
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <string>

namespace djinni {
    void jniInit(JavaVM* jvm);
}

namespace pvz_a11y {

class AndroidJniBridge {
public:
    static bool Initialize() {
        if (s_initialized) return true;

        using GetJniEnvFn = void* (*)();
        using GetActivityFn = void* (*)();

        auto getEnv = reinterpret_cast<GetJniEnvFn>(dlsym(RTLD_DEFAULT, "SDL_GetAndroidJNIEnv"));
        if (!getEnv) {
            getEnv = reinterpret_cast<GetJniEnvFn>(dlsym(RTLD_DEFAULT, "Android_JNI_GetEnv"));
        }
        auto getActivity = reinterpret_cast<GetActivityFn>(dlsym(RTLD_DEFAULT, "SDL_GetAndroidActivity"));

        if (!getEnv) {
            __android_log_write(ANDROID_LOG_WARN, "pvz-a11y", "Could not resolve SDL_GetAndroidJNIEnv via dlsym");
            return false;
        }

        void* rawEnv = getEnv();
        if (!rawEnv) {
            __android_log_write(ANDROID_LOG_WARN, "pvz-a11y", "getEnv() returned null");
            return false;
        }

        auto* env = static_cast<JNIEnv*>(rawEnv);
        JavaVM* jvm = nullptr;
        if (env->GetJavaVM(&jvm) != JNI_OK || !jvm) {
            __android_log_write(ANDROID_LOG_ERROR, "pvz-a11y", "Failed to retrieve JavaVM from JNIEnv");
            return false;
        }

        s_jvm = jvm;

        // 1. 初始化 Djinni JNI 运行时
        try {
            djinni::jniInit(jvm);
        } catch (...) {
            __android_log_write(ANDROID_LOG_WARN, "pvz-a11y", "djinni::jniInit threw exception during startup");
        }

        // 2. 将宿主 Activity (Context) 设置给 PrismContext
        void* rawActivity = getActivity ? getActivity() : nullptr;
        if (rawActivity) {
            auto activity = static_cast<jobject>(rawActivity);
            s_activity = env->NewGlobalRef(activity);

            jclass prismContextCls = env->FindClass("com/github/ethindp/prism/PrismContext");
            if (prismContextCls) {
                jmethodID setMethod = env->GetStaticMethodID(prismContextCls, "set", "(Landroid/content/Context;)V");
                if (setMethod) {
                    env->CallStaticVoidMethod(prismContextCls, setMethod, activity);
                }
                env->DeleteLocalRef(prismContextCls);
            } else {
                if (env->ExceptionCheck()) env->ExceptionClear();
                __android_log_write(ANDROID_LOG_INFO, "pvz-a11y", "PrismContext class not in default classloader");
            }
        }

        s_initialized = true;
        return true;
    }

    static JavaVM* GetJavaVM() { return s_jvm; }
    static jobject GetActivity() { return s_activity; }

    static void Shutdown() {
        if (!s_initialized) return;
        if (s_jvm && s_activity) {
            JNIEnv* env = nullptr;
            if (s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env) {
                env->DeleteGlobalRef(s_activity);
            }
            s_activity = nullptr;
        }
        s_initialized = false;
    }

private:
    inline static bool s_initialized = false;
    inline static JavaVM* s_jvm = nullptr;
    inline static jobject s_activity = nullptr;
};

} // namespace pvz_a11y
#endif
