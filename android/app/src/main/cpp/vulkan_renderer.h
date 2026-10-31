#pragma once

#include <android/native_window.h>
#include <string>

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool setSurface(ANativeWindow* ownedWindow);
    bool resize(int width, int height);
    bool render();
    void clearSurface();
    std::string status() const;

private:
    bool initialize(std::string& error);
    bool createInstance(std::string& error);
    bool createSurface(std::string& error);
    bool createDevice(std::string& error);
    bool createSwapchain(std::string& error);
    bool createFrameSync(std::string& error);
    bool renderFrame(std::string& error);
    bool recreateSwapchain(std::string& error);
    void destroySwapchain();
    void shutdown();

    ANativeWindow* window_ = nullptr;
    struct State;
    State* state_ = nullptr;
    std::string status_ = "Vulkan: aguardando superfície Android.";
};
