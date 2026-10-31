#include "disc_inspector.h"
#include "iso_source.h"
#include "on_device_pipeline.h"
#include "vulkan_renderer.h"

#include <jni.h>
#include <android/native_window_jni.h>

#include <atomic>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
std::mutex g_isoMutex;
std::mutex g_rendererMutex;
std::mutex g_progressMutex;
std::mutex g_threadMutex;

IsoSource g_iso;
VulkanRenderer g_renderer;
std::string g_lastError;
std::string g_discSummary;

SotcPipelineProgress g_pipelineProgress;
std::thread g_pipelineThread;
std::atomic_bool g_pipelineRunning{false};
std::atomic_bool g_cancelRequested{false};

std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char ch : text) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) out << '?';
            else out << static_cast<char>(ch);
            break;
        }
    }
    return out.str();
}

std::string progressJson(const SotcPipelineProgress& progress) {
    std::ostringstream out;
    const char* state = progress.failed ? "failed" :
        (progress.completed ? "completed" : (progress.running ? "running" : "idle"));
    out << "{"
        << "\"state\":\"" << state << "\","
        << "\"percent\":" << progress.percent << ","
        << "\"step\":" << progress.step << ","
        << "\"steps\":" << progress.totalSteps << ","
        << "\"eta_seconds\":" << progress.etaSeconds << ","
        << "\"stage\":\"" << jsonEscape(progress.stage) << "\","
        << "\"detail\":\"" << jsonEscape(progress.detail) << "\","
        << "\"error\":\"" << jsonEscape(progress.error) << "\","
        << "\"output_directory\":\"" << jsonEscape(progress.outputDirectory) << "\""
        << "}";
    return out.str();
}

void publishProgress(const SotcPipelineProgress& next) {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_pipelineProgress = next;
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeAttachIso(
        JNIEnv*, jclass, jint fd, jlong offset, jlong length) {
    if (g_pipelineRunning.load()) {
        if (fd >= 0) ::close(fd);
        return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lock(g_isoMutex);
    std::string error;
    const bool attached = g_iso.attach(fd, static_cast<int64_t>(offset),
                                       static_cast<int64_t>(length), error);
    g_lastError.clear();
    g_discSummary.clear();
    if (!attached) {
        g_lastError = error;
        return JNI_FALSE;
    }

    DiscInspection inspection;
    if (!inspectShadowOfTheColossusDisc(g_iso, inspection, error)) {
        g_lastError = error;
        return JNI_FALSE;
    }
    g_discSummary = inspection.summary;
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeClearIso(JNIEnv*, jclass) {
    if (g_pipelineRunning.load()) return;
    std::lock_guard<std::mutex> lock(g_isoMutex);
    g_iso.close();
    g_lastError.clear();
    g_discSummary.clear();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeSetSurface(
        JNIEnv* env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lock(g_rendererMutex);
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    const bool ok = g_renderer.setSurface(window);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeResizeSurface(
        JNIEnv*, jclass, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_rendererMutex);
    return g_renderer.resize(width, height) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeRenderFrame(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_rendererMutex);
    return g_renderer.render() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeClearSurface(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_rendererMutex);
    g_renderer.clearSurface();
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeStatus(JNIEnv* env, jclass) {
    std::string isoMessage;
    std::string discMessage;
    std::string lastError;
    {
        std::lock_guard<std::mutex> lock(g_isoMutex);
        isoMessage = g_iso.description();
        discMessage = g_discSummary;
        lastError = g_lastError;
    }
    std::string rendererMessage;
    {
        std::lock_guard<std::mutex> lock(g_rendererMutex);
        rendererMessage = g_renderer.status();
    }
    std::string message = isoMessage;
    if (!discMessage.empty()) message += "\n" + discMessage;
    if (!lastError.empty()) message += "\nFalha na validação: " + lastError;
    if (!rendererMessage.empty()) message += "\n" + rendererMessage;
    return env->NewStringUTF(message.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativeStartPipeline(
        JNIEnv* env, jclass, jstring filesDirectory) {
    if (filesDirectory == nullptr || g_pipelineRunning.load()) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(filesDirectory, nullptr);
    if (chars == nullptr) return JNI_FALSE;
    const std::string root(chars);
    env->ReleaseStringUTFChars(filesDirectory, chars);

    {
        std::lock_guard<std::mutex> isoLock(g_isoMutex);
        if (!g_iso.isOpen()) {
            std::lock_guard<std::mutex> progressLock(g_progressMutex);
            g_pipelineProgress = {};
            g_pipelineProgress.failed = true;
            g_pipelineProgress.error = "Selecione e valide primeiro uma imagem PAL SCES-53326 v1.00.";
            return JNI_FALSE;
        }
    }

    std::lock_guard<std::mutex> threadLock(g_threadMutex);
    if (g_pipelineThread.joinable()) g_pipelineThread.join();
    g_cancelRequested.store(false);
    g_pipelineRunning.store(true);
    {
        std::lock_guard<std::mutex> progressLock(g_progressMutex);
        g_pipelineProgress = {};
        g_pipelineProgress.running = true;
        g_pipelineProgress.step = 1;
        g_pipelineProgress.totalSteps = 7;
        g_pipelineProgress.percent = 1;
        g_pipelineProgress.stage = "Preparando";
        g_pipelineProgress.detail = "Iniciando a preparação no telefone...";
        g_pipelineProgress.outputDirectory = root + "/sotc_pipeline/SCES-53326-v1.00";
    }

    try {
        g_pipelineThread = std::thread([root]() {
            std::string error;
            bool success = false;
            {
                std::lock_guard<std::mutex> isoLock(g_isoMutex);
                try {
                    success = runSotcOnDevicePipeline(
                        g_iso, root, g_cancelRequested,
                        [](const SotcPipelineProgress& state) { publishProgress(state); },
                        error);
                } catch (const std::exception& exception) {
                    error = std::string("Erro inesperado na preparação: ") + exception.what();
                    success = false;
                } catch (...) {
                    error = "Erro inesperado não identificado durante a preparação.";
                    success = false;
                }
            }
            {
                std::lock_guard<std::mutex> progressLock(g_progressMutex);
                g_pipelineProgress.running = false;
                g_pipelineProgress.completed = success;
                g_pipelineProgress.failed = !success;
                if (!success) {
                    g_pipelineProgress.error = error.empty() ? "Falha desconhecida na preparação." : error;
                    g_pipelineProgress.detail = g_pipelineProgress.error;
                }
            }
            g_pipelineRunning.store(false);
        });
    } catch (const std::exception& exception) {
        g_pipelineRunning.store(false);
        std::lock_guard<std::mutex> progressLock(g_progressMutex);
        g_pipelineProgress.running = false;
        g_pipelineProgress.failed = true;
        g_pipelineProgress.error = std::string("Não foi possível iniciar a tarefa: ") + exception.what();
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativePipelineProgress(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    const std::string value = progressJson(g_pipelineProgress);
    return env->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativePipelineRunning(JNIEnv*, jclass) {
    return g_pipelineRunning.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ph04_sotcandroid_MainActivity_nativePipelineOutputDirectory(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    return env->NewStringUTF(g_pipelineProgress.outputDirectory.c_str());
}
