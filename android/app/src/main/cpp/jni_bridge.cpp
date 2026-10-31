#include "iso_source.h"
#include "vulkan_renderer.h"

#include <jni.h>
#include <android/native_window_jni.h>

#include <mutex>
#include <string>

namespace {
std::mutex g_mutex;
IsoSource g_iso;
VulkanRenderer g_renderer;
std::string g_lastError;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeAttachIso(
        JNIEnv*, jclass, jint fd, jlong offset, jlong length) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string error;
    const bool attached = g_iso.attach(fd, static_cast<int64_t>(offset),
                                       static_cast<int64_t>(length), error);
    g_lastError = attached ? std::string() : error;
    return attached ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeClearIso(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_iso.close();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeSetSurface(
        JNIEnv* env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    const bool ok = g_renderer.setSurface(window);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeResizeSurface(
        JNIEnv*, jclass, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_renderer.resize(width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeRenderFrame(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_renderer.render() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeClearSurface(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_renderer.clearSurface();
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeStatus(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string message = g_iso.description();
    if (!g_lastError.empty()) message += "\\nFalha na imagem: " + g_lastError;
    message += "\\n";
    message += g_renderer.status();
    return env->NewStringUTF(message.c_str());
}
