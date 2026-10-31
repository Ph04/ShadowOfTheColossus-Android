#include "vulkan_renderer.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

struct VulkanRenderer::State {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    bool fragmentInterlock = false;
    std::string deviceName;
};

namespace {
bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> values(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, values.data()) != VK_SUCCESS) return false;
    for (const auto& value : values) if (std::strcmp(value.extensionName, name) == 0) return true;
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> values(count);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, values.data()) != VK_SUCCESS) return false;
    for (const auto& value : values) if (std::strcmp(value.extensionName, name) == 0) return true;
    return false;
}

const char* resultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult";
    }
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    for (const auto choice : choices) if ((supported & choice) != 0) return choice;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
}

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::setSurface(ANativeWindow* ownedWindow) {
    if (ownedWindow == nullptr) {
        status_ = "Vulkan: superfície Android inválida.";
        return false;
    }
    shutdown();
    window_ = ownedWindow;
    std::string error;
    if (!initialize(error)) {
        status_ = "Vulkan indisponível: " + error;
        shutdown();
        return false;
    }
    return true;
}

bool VulkanRenderer::resize(int width, int height) {
    if (window_ == nullptr || state_ == nullptr || width <= 0 || height <= 0) return false;
    std::string error;
    return recreateSwapchain(error);
}

bool VulkanRenderer::render() {
    std::string error;
    const bool ok = renderFrame(error);
    if (!ok && !error.empty()) status_ = "Vulkan: " + error;
    return ok;
}

void VulkanRenderer::clearSurface() {
    shutdown();
    status_ = "Vulkan: aguardando superfície Android.";
}

std::string VulkanRenderer::status() const {
    return status_;
}

bool VulkanRenderer::initialize(std::string& error) {
    state_ = new State();
    if (!createInstance(error) || !createSurface(error) || !createDevice(error) ||
        !createSwapchain(error) || !createFrameSync(error)) {
        return false;
    }

    std::ostringstream message;
    message << "Vulkan ativo: " << state_->deviceName
            << " | fragment interlock: " << (state_->fragmentInterlock ? "disponível" : "não disponível")
            << " | renderer GS completo: em desenvolvimento.";
    status_ = message.str();
    return true;
}

bool VulkanRenderer::createInstance(std::string& error) {
    if (!hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !hasInstanceExtension(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        error = "o driver não expõe VK_KHR_surface e VK_KHR_android_surface.";
        return false;
    }

    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "Shadow of the Colossus Android";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.pEngineName = "SOTC Android Native";
    application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;
    const VkResult result = vkCreateInstance(&createInfo, nullptr, &state_->instance);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateInstance: ") + resultName(result);
        return false;
    }
    return true;
}

bool VulkanRenderer::createSurface(std::string& error) {
    const auto createAndroidSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
        vkGetInstanceProcAddr(state_->instance, "vkCreateAndroidSurfaceKHR"));
    if (createAndroidSurface == nullptr) {
        error = "vkCreateAndroidSurfaceKHR não está disponível.";
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window_;
    const VkResult result = createAndroidSurface(state_->instance, &info, nullptr, &state_->surface);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateAndroidSurfaceKHR: ") + resultName(result);
        return false;
    }
    return true;
}

bool VulkanRenderer::createDevice(std::string& error) {
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(state_->instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        error = "nenhuma GPU Vulkan foi encontrada.";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(state_->instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        error = std::string("vkEnumeratePhysicalDevices: ") + resultName(result);
        return false;
    }

    for (VkPhysicalDevice candidate : devices) {
        if (!hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        for (uint32_t index = 0; index < queueCount; ++index) {
            if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, state_->surface, &canPresent);
            if (canPresent == VK_TRUE) {
                state_->physicalDevice = candidate;
                state_->queueFamily = index;
                break;
            }
        }
        if (state_->physicalDevice != VK_NULL_HANDLE) break;
    }

    if (state_->physicalDevice == VK_NULL_HANDLE) {
        error = "nenhuma fila Vulkan suporta gráficos e apresentação nesta superfície.";
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(state_->physicalDevice, &properties);
    state_->deviceName = properties.deviceName;

    if (hasDeviceExtension(state_->physicalDevice, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)) {
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT
        };
        // Android's Vulkan loader may expose core Vulkan 1.1 entry points only
        // through vkGetInstanceProcAddr; do not create a direct shared-library
        // reference to vkGetPhysicalDeviceFeatures2.
        const auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(state_->instance, "vkGetPhysicalDeviceFeatures2"));
        if (getFeatures2 != nullptr) {
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &interlock;
            getFeatures2(state_->physicalDevice, &features);
            state_->fragmentInterlock = interlock.fragmentShaderPixelInterlock == VK_TRUE;
        }
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = state_->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures enabledFeatures{};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    deviceInfo.pEnabledFeatures = &enabledFeatures;
    result = vkCreateDevice(state_->physicalDevice, &deviceInfo, nullptr, &state_->device);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateDevice: ") + resultName(result);
        return false;
    }
    vkGetDeviceQueue(state_->device, state_->queueFamily, 0, &state_->queue);
    return true;
}

bool VulkanRenderer::createSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR caps{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        state_->physicalDevice, state_->surface, &caps);
    if (result != VK_SUCCESS) {
        error = std::string("surface capabilities: ") + resultName(result);
        return false;
    }
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        error = "a superfície não aceita color attachments.";
        return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(state_->physicalDevice, state_->surface, &formatCount, nullptr);
    if (formatCount == 0) {
        error = "a superfície não informou formatos de imagem.";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(state_->physicalDevice, state_->surface, &formatCount, formats.data());
    VkSurfaceFormatKHR selected = formats.front();
    for (const auto& candidate : formats) {
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM ||
            candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            selected = candidate;
            break;
        }
    }
    state_->format = selected.format;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        const uint32_t width = static_cast<uint32_t>(std::max(1, ANativeWindow_getWidth(window_)));
        const uint32_t height = static_cast<uint32_t>(std::max(1, ANativeWindow_getHeight(window_)));
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    state_->extent = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = state_->surface;
    info.minImageCount = imageCount;
    info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha);
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(state_->device, &info, nullptr, &state_->swapchain);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateSwapchainKHR: ") + resultName(result);
        return false;
    }

    vkGetSwapchainImagesKHR(state_->device, state_->swapchain, &imageCount, nullptr);
    state_->images.resize(imageCount);
    result = vkGetSwapchainImagesKHR(state_->device, state_->swapchain, &imageCount, state_->images.data());
    if (result != VK_SUCCESS) {
        error = std::string("vkGetSwapchainImagesKHR: ") + resultName(result);
        return false;
    }

    state_->views.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = state_->images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = state_->format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        result = vkCreateImageView(state_->device, &view, nullptr, &state_->views[i]);
        if (result != VK_SUCCESS) {
            error = std::string("vkCreateImageView: ") + resultName(result);
            return false;
        }
    }

    VkAttachmentDescription attachment{};
    attachment.format = state_->format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;

    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    result = vkCreateRenderPass(state_->device, &pass, nullptr, &state_->renderPass);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateRenderPass: ") + resultName(result);
        return false;
    }

    state_->framebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass = state_->renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &state_->views[i];
        framebuffer.width = extent.width;
        framebuffer.height = extent.height;
        framebuffer.layers = 1;
        result = vkCreateFramebuffer(state_->device, &framebuffer, nullptr, &state_->framebuffers[i]);
        if (result != VK_SUCCESS) {
            error = std::string("vkCreateFramebuffer: ") + resultName(result);
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createFrameSync(std::string& error) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = state_->queueFamily;
    VkResult result = vkCreateCommandPool(state_->device, &pool, nullptr, &state_->commandPool);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateCommandPool: ") + resultName(result);
        return false;
    }

    VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = state_->commandPool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(state_->device, &command, &state_->commandBuffer);
    if (result != VK_SUCCESS) {
        error = std::string("vkAllocateCommandBuffers: ") + resultName(result);
        return false;
    }

    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = vkCreateFence(state_->device, &fence, nullptr, &state_->acquireFence);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateFence: ") + resultName(result);
        return false;
    }
    return true;
}

bool VulkanRenderer::renderFrame(std::string& error) {
    if (state_ == nullptr || state_->device == VK_NULL_HANDLE || state_->swapchain == VK_NULL_HANDLE) {
        error = "swapchain não inicializada.";
        return false;
    }

    vkQueueWaitIdle(state_->queue);
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(state_->device, state_->swapchain, UINT64_MAX,
                                            VK_NULL_HANDLE, state_->acquireFence, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return recreateSwapchain(error);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        error = std::string("vkAcquireNextImageKHR: ") + resultName(result);
        return false;
    }

    result = vkWaitForFences(state_->device, 1, &state_->acquireFence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        error = std::string("vkWaitForFences: ") + resultName(result);
        return false;
    }
    vkResetFences(state_->device, 1, &state_->acquireFence);

    vkResetCommandBuffer(state_->commandBuffer, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    result = vkBeginCommandBuffer(state_->commandBuffer, &begin);
    if (result != VK_SUCCESS) {
        error = std::string("vkBeginCommandBuffer: ") + resultName(result);
        return false;
    }

    VkClearValue clear{};
    clear.color = {{0.018f, 0.026f, 0.043f, 1.0f}};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = state_->renderPass;
    pass.framebuffer = state_->framebuffers[imageIndex];
    pass.renderArea.extent = state_->extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(state_->commandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(state_->commandBuffer);

    result = vkEndCommandBuffer(state_->commandBuffer);
    if (result != VK_SUCCESS) {
        error = std::string("vkEndCommandBuffer: ") + resultName(result);
        return false;
    }

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &state_->commandBuffer;
    result = vkQueueSubmit(state_->queue, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        error = std::string("vkQueueSubmit: ") + resultName(result);
        return false;
    }
    result = vkQueueWaitIdle(state_->queue);
    if (result != VK_SUCCESS) {
        error = std::string("vkQueueWaitIdle: ") + resultName(result);
        return false;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &state_->swapchain;
    present.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(state_->queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return recreateSwapchain(error);
    }
    if (result != VK_SUCCESS) {
        error = std::string("vkQueuePresentKHR: ") + resultName(result);
        return false;
    }
    return true;
}

bool VulkanRenderer::recreateSwapchain(std::string& error) {
    if (state_ == nullptr || state_->device == VK_NULL_HANDLE) return false;
    vkDeviceWaitIdle(state_->device);
    destroySwapchain();
    if (!createSwapchain(error)) {
        status_ = "Vulkan: falha ao recriar swapchain: " + error;
        return false;
    }
    return true;
}

void VulkanRenderer::destroySwapchain() {
    if (state_ == nullptr || state_->device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(state_->device);
    for (VkFramebuffer framebuffer : state_->framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(state_->device, framebuffer, nullptr);
    }
    state_->framebuffers.clear();
    for (VkImageView view : state_->views) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(state_->device, view, nullptr);
    }
    state_->views.clear();
    state_->images.clear();
    if (state_->renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(state_->device, state_->renderPass, nullptr);
        state_->renderPass = VK_NULL_HANDLE;
    }
    if (state_->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(state_->device, state_->swapchain, nullptr);
        state_->swapchain = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::shutdown() {
    if (state_ != nullptr) {
        if (state_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(state_->device);
        destroySwapchain();
        if (state_->device != VK_NULL_HANDLE && state_->acquireFence != VK_NULL_HANDLE) {
            vkDestroyFence(state_->device, state_->acquireFence, nullptr);
        }
        if (state_->device != VK_NULL_HANDLE && state_->commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(state_->device, state_->commandPool, nullptr);
        }
        if (state_->device != VK_NULL_HANDLE) vkDestroyDevice(state_->device, nullptr);
        if (state_->surface != VK_NULL_HANDLE && state_->instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(state_->instance, state_->surface, nullptr);
        }
        if (state_->instance != VK_NULL_HANDLE) vkDestroyInstance(state_->instance, nullptr);
        delete state_;
        state_ = nullptr;
    }
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
}
