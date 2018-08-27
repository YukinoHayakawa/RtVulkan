#include "VulkanGpuDevice.hpp"

#include <algorithm>

#include <Usagi/Core/Logging.hpp>
#include <Usagi/Runtime/Graphics/Resource/GpuImageView.hpp>
#include <Usagi/Runtime/Graphics/Resource/GpuSamplerCreateInfo.hpp>
#include <Usagi/Runtime/Memory/BitmapMemoryAllocator.hpp>
#include <Usagi/Utility/Flag.hpp>
#include <Usagi/Utility/TypeCast.hpp>

#include "VulkanFramebuffer.hpp"
#include "VulkanGpuBuffer.hpp"
#include "VulkanGpuCommandPool.hpp"
#include "VulkanGpuImageView.hpp"
#include "VulkanGraphicsCommandList.hpp"
#include "VulkanMemoryPool.hpp"
#include "VulkanPooledImage.hpp"
#include "VulkanSampler.hpp"
#include "VulkanSemaphore.hpp"
#include "VulkanEnumTranslation.hpp"
#include "VulkanGraphicsPipelineCompiler.hpp"
#include "VulkanHelper.hpp"
#include "VulkanRenderPass.hpp"

using namespace usagi::vulkan;

VkBool32 usagi::VulkanGpuDevice::debugMessengerCallbackDispatcher(
    const VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    const VkDebugUtilsMessageTypeFlagsEXT message_type,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
    void *user_data)
{
    const auto device = reinterpret_cast<VulkanGpuDevice*>(user_data);
    return device->debugMessengerCallback(
        vk::DebugUtilsMessageSeverityFlagBitsEXT { message_severity },
        vk::DebugUtilsMessageTypeFlagsEXT  { message_type },
        reinterpret_cast<const vk::DebugUtilsMessengerCallbackDataEXT *>(
            callback_data));
}

VkBool32 usagi::VulkanGpuDevice::debugMessengerCallback(
    const vk::DebugUtilsMessageSeverityFlagsEXT &message_severity,
    const vk::DebugUtilsMessageTypeFlagsEXT &message_type,
    const vk::DebugUtilsMessengerCallbackDataEXT *callback_data) const
{
    spdlog::level::level_enum level;
    using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
    if(message_severity & Severity::eVerbose)
        level = spdlog::level::debug;
    else if(message_severity & Severity::eInfo)
        level = spdlog::level::info;
    else if(message_severity & Severity::eWarning)
        level = spdlog::level::warn;
    else if(message_severity & Severity::eError)
        level = spdlog::level::err;
    else
        level = spdlog::level::info;

    LOG(log, level,
        "[Vulkan] {} [{}][ID={}]: {}",
        to_string(message_type),
        callback_data->pMessageIdName
            ? callback_data->pMessageIdName
            : "<Unknown>",
        callback_data->messageIdNumber,
        callback_data->pMessage);

    if(callback_data->objectCount > 0)
    {
        for(uint32_t i = 0; i < callback_data->objectCount; ++i)
        {
            const auto &object = callback_data->pObjects[i];
            LOG(log, level, "Object #{}: Handle {}, Type {}, Name \"{}\"",
                i,
                object.objectHandle,
                to_string(object.objectType),
                object.pObjectName ? object.pObjectName : "");
        }
    }
    if(callback_data->cmdBufLabelCount > 0)
    {
        LOG(info, "Command Buffer Label Count: {}",
            callback_data->cmdBufLabelCount);
        for(uint32_t i = 0; i < callback_data->cmdBufLabelCount; ++i)
        {
            LOG(log, level,
                "Label #{}: {} {{ {}, {}, {}, {} }}\n",
                i,
                callback_data->pCmdBufLabels[i].pLabelName,
                callback_data->pCmdBufLabels[i].color[0],
                callback_data->pCmdBufLabels[i].color[1],
                callback_data->pCmdBufLabels[i].color[2],
                callback_data->pCmdBufLabels[i].color[3]);
        }
    }

    // Don't bail out, but keep going. return false;
    return false;
}

uint32_t usagi::VulkanGpuDevice::selectQueue(
    std::vector<vk::QueueFamilyProperties> &queue_family,
    const vk::QueueFlags &queue_flags)
{
    for(auto iter = queue_family.begin(); iter != queue_family.end(); ++iter)
    {
        if(utility::matchAllFlags(iter->queueFlags, queue_flags))
        {
            return static_cast<uint32_t>(iter - queue_family.begin());
        }
    }
    throw std::runtime_error(
        "Could not find a queue family with required flags.");
}

void usagi::VulkanGpuDevice::createInstance()
{
    LOG(info, "Creating Vulkan intance");
    LOG(info, "--------------------------------");

    vk::ApplicationInfo application_info;
    application_info.setPApplicationName("UsagiEngine");
    application_info.setApplicationVersion(VK_MAKE_VERSION(1, 0, 0));
    application_info.setPEngineName("Usagi");
    application_info.setEngineVersion(VK_MAKE_VERSION(1, 0, 0));
    application_info.setApiVersion(VK_API_VERSION_1_0);

    // Extensions
    {
        LOG(info, "Available instance extensions");
        LOG(info, "--------------------------------");
        for(auto &&ext : vk::enumerateInstanceExtensionProperties())
        {
            LOG(info, ext.extensionName);
        }
        LOG(info, "--------------------------------");
    }
    // Validation layers
    {
        LOG(info, "Available validation layers");
        LOG(info, "--------------------------------");
        for(auto &&layer : vk::enumerateInstanceLayerProperties())
        {
            LOG(info, "Name       : {}", layer.layerName);
            LOG(info, "Description: {}", layer.description);
            LOG(info, "--------------------------------");
        }
    }

    vk::InstanceCreateInfo instance_create_info;
    instance_create_info.setPApplicationInfo(&application_info);

    std::vector<const char *> instance_extensions
    {
        // application window
        VK_KHR_SURFACE_EXTENSION_NAME,
        // provide feedback from validation layer, etc.
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    addPlatformSurfaceExtension(instance_extensions);
    instance_create_info.setEnabledExtensionCount(
        static_cast<uint32_t>(instance_extensions.size()));
    instance_create_info.setPpEnabledExtensionNames(instance_extensions.data());

    // todo: enumerate layers first
    // todo: disable in release
    const std::vector<const char*> validation_layers
    {
        "VK_LAYER_LUNARG_standard_validation"
    };
    instance_create_info.setEnabledLayerCount(
        static_cast<uint32_t>(validation_layers.size()));
    instance_create_info.setPpEnabledLayerNames(validation_layers.data());

    mInstance = createInstanceUnique(instance_create_info);
}

void usagi::VulkanGpuDevice::createDebugReport()
{
    vk::DebugUtilsMessengerCreateInfoEXT info;
    using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
    info.messageSeverity =
        // Severity::eVerbose |
        // Severity::eInfo |
        Severity::eWarning |
        Severity::eError;
    using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;
    info.messageType =
        Type::eGeneral |
        Type::eValidation |
        Type::ePerformance;
    info.pfnUserCallback = &debugMessengerCallbackDispatcher;

    mDebugUtilsMessenger = mInstance->createDebugUtilsMessengerEXTUnique(info);
}

void usagi::VulkanGpuDevice::selectPhysicalDevice()
{
    {
        LOG(info, "Available physical devices");
        LOG(info, "--------------------------------");
        auto physical_devices = mInstance->enumeratePhysicalDevices();
        for(auto &&dev : physical_devices)
        {
            const auto prop = dev.getProperties();
            LOG(info, "Device Name   : {}", prop.deviceName);
            LOG(info, "Device Type   : {}", to_string(prop.deviceType));
            LOG(info, "Device ID     : {}", prop.deviceID);
            LOG(info, "API Version   : {}", prop.apiVersion);
            LOG(info, "Driver Version: {}", prop.vendorID);
            LOG(info, "Vendor ID     : {}", prop.vendorID);
            LOG(info, "--------------------------------");
            // todo: select device based on features and score them / let the
            // user choose
            if(!mPhysicalDevice)
                mPhysicalDevice = dev;
            else if(prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
                mPhysicalDevice = dev;
        }
    }
    if(!mPhysicalDevice)
        throw std::runtime_error("No available GPU supporting Vulkan.");
    LOG(info, "Using physical device: {}",
        mPhysicalDevice.getProperties().deviceName);
}

void usagi::VulkanGpuDevice::createDeviceAndQueues()
{
    LOG(info, "Creating device and queues");

    auto queue_families = mPhysicalDevice.getQueueFamilyProperties();
    LOG(info, "Supported queue families:");
    for(std::size_t i = 0; i < queue_families.size(); ++i)
    {
        auto &qf = queue_families[i];
        LOG(info, "#{}: {} * {}", i, to_string(qf.queueFlags), qf.queueCount);
    }

    const auto graphics_queue_index = selectQueue(queue_families,
        vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eTransfer);
    checkQueuePresentationCapacity(graphics_queue_index);

    LOG(info, "Getting a queue from queue family {}.",
        graphics_queue_index);

    vk::DeviceCreateInfo device_create_info;

    vk::PhysicalDeviceFeatures features;
    device_create_info.setPEnabledFeatures(&features);
    features.setFillModeNonSolid(true);
    features.setLargePoints(true);
    features.setWideLines(true);

    vk::DeviceQueueCreateInfo queue_create_info[1];
    float queue_priority = 1;
    queue_create_info[0].setQueueFamilyIndex(graphics_queue_index);
    queue_create_info[0].setQueueCount(1);
    queue_create_info[0].setPQueuePriorities(&queue_priority);
    device_create_info.setQueueCreateInfoCount(1);
    device_create_info.setPQueueCreateInfos(queue_create_info);

    // todo: check device capacity
    const std::vector<const char *> device_extensions
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    device_create_info.setEnabledExtensionCount(static_cast<uint32_t>(
        device_extensions.size()));
    device_create_info.setPpEnabledExtensionNames(device_extensions.data());

    mDevice = mPhysicalDevice.createDeviceUnique(device_create_info);

    mGraphicsQueue = mDevice->getQueue(graphics_queue_index, 0);
    mGraphicsQueueFamilyIndex = graphics_queue_index;
}

void usagi::VulkanGpuDevice::createMemoryPools()
{
    mDynamicBufferPool = std::make_unique<BitmapBufferPool>(
        this,
        1024 * 1024 * 128, // 512MiB  todo from config
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent,
        vk::BufferUsageFlagBits::eVertexBuffer |
        vk::BufferUsageFlagBits::eIndexBuffer |
        vk::BufferUsageFlagBits::eUniformBuffer,
        [](const vk::MemoryRequirements &req) {
            return std::make_unique<BitmapMemoryAllocator>(
                nullptr,
                req.size,
                32 * 1024 /* 32 KiB */ // todo config
            );
        }
    );

    mHostImagePool = std::make_unique<BitmapImagePool>(
        this,
        1024 * 1024 * 128, // 512MiB  todo from config
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent,
        vk::ImageUsageFlagBits::eSampled,
        [](const vk::MemoryRequirements &req) {
            return std::make_unique<BitmapMemoryAllocator>(
                nullptr,
                req.size,
                32 * 1024 /* 32 KiB */ // todo config
            );
        }
    );
}

usagi::VulkanGpuDevice::VulkanGpuDevice()
{
    createInstance();
    createDebugReport();
    selectPhysicalDevice();
    createDeviceAndQueues();
    createMemoryPools();
}

usagi::VulkanGpuDevice::~VulkanGpuDevice()
{
    // Wait till all operations are completed so it is safe to release the
    // resources.
    mDevice->waitIdle();
}

std::unique_ptr<usagi::GraphicsPipelineCompiler> usagi::VulkanGpuDevice::
    createPipelineCompiler()
{
    return std::make_unique<VulkanGraphicsPipelineCompiler>(this);
}

std::shared_ptr<usagi::GpuCommandPool>
    usagi::VulkanGpuDevice::createCommandPool()
{
    return std::make_shared<VulkanGpuCommandPool>(this);
}

std::shared_ptr<usagi::RenderPass> usagi::VulkanGpuDevice::createRenderPass(
    const RenderPassCreateInfo &info)
{
    return std::make_shared<VulkanRenderPass>(this, info);
}

std::shared_ptr<usagi::Framebuffer> usagi::VulkanGpuDevice::createFramebuffer(
    const Vector2u32 &size,
    std::initializer_list<std::shared_ptr<GpuImageView>> views)
{
    auto vk_views = transformObjects(views,
        [&](auto &&v) {
            return dynamic_pointer_cast_throw<VulkanGpuImageView>(v);
        }
    );

    return std::make_shared<VulkanFramebuffer>(this, size, std::move(vk_views));
}

// todo sem pool
std::shared_ptr<usagi::GpuSemaphore> usagi::VulkanGpuDevice::createSemaphore()
{
    auto sem = mDevice->createSemaphoreUnique(vk::SemaphoreCreateInfo { });
    return std::make_unique<VulkanSemaphore>(std::move(sem));
}

std::shared_ptr<usagi::GpuBuffer> usagi::VulkanGpuDevice::createBuffer(
    GpuBufferUsage usage)
{
    return std::make_shared<VulkanGpuBuffer>(mDynamicBufferPool.get(), usage);
}

std::shared_ptr<usagi::GpuImage> usagi::VulkanGpuDevice::createImage(
    const GpuImageCreateInfo &info)
{
    // todo check format availability
    return mHostImagePool->createPooledImage(info);
}

std::shared_ptr<usagi::GpuSampler> usagi::VulkanGpuDevice::createSampler(
    const GpuSamplerCreateInfo &info)
{
    vk::SamplerCreateInfo vk_info;
    vk_info.setMagFilter(translate(info.mag_filter));
    vk_info.setMinFilter(translate(info.min_filter));
    vk_info.setMipmapMode(vk::SamplerMipmapMode::eLinear);
    vk_info.setAddressModeU(translate(info.addressing_mode_u));
    vk_info.setAddressModeV(translate(info.addressing_mode_v));
    // todo sampler setBorderColor
    // vk_info.setBorderColor({ });
    return std::make_shared<VulkanSampler>(
        mDevice->createSamplerUnique(vk_info));
}

void usagi::VulkanGpuDevice::submitGraphicsJobs(
    std::vector<std::shared_ptr<GraphicsCommandList>> jobs,
    std::initializer_list<std::shared_ptr<GpuSemaphore>> wait_semaphores,
    std::initializer_list<GraphicsPipelineStage> wait_stages,
    std::initializer_list<std::shared_ptr<GpuSemaphore>> signal_semaphores)
{
    const auto vk_jobs = transformObjects(jobs, [&](auto &&j) {
        return dynamic_cast_ref<VulkanGraphicsCommandList>(j).commandBuffer();
    });
    const auto vk_wait_sems = transformObjects(wait_semaphores,
        [&](auto &&s) {
            return dynamic_cast_ref<VulkanSemaphore>(s).semaphore();
        }
    );
    const auto vk_wait_stages = transformObjects(wait_stages,
        [&](auto &&s) {
            // todo wait on multiple stages
            return vk::PipelineStageFlags(translate(s));
        }
    );
    const auto vk_signal_sems = transformObjects(signal_semaphores,
        [&](auto &&s) {
            return dynamic_cast_ref<VulkanSemaphore>(s).semaphore();
        }
    );

    BatchResourceList batch_resources;
    batch_resources.fence = mDevice->createFenceUnique(vk::FenceCreateInfo { });

    vk::SubmitInfo info;
    info.setCommandBufferCount(static_cast<uint32_t>(vk_jobs.size()));
    info.setPCommandBuffers(vk_jobs.data());
    info.setWaitSemaphoreCount(static_cast<uint32_t>(vk_wait_sems.size()));
    info.setPWaitSemaphores(vk_wait_sems.data());
    info.setSignalSemaphoreCount(static_cast<uint32_t>(vk_signal_sems.size()));
    info.setPSignalSemaphores(vk_signal_sems.data());
    info.setPWaitDstStageMask(vk_wait_stages.data());

    const auto cast_append = [&](auto &&container) {
        std::transform(
            container.begin(), container.end(),
            std::back_inserter(batch_resources.resources),
            [&](auto &&j) {
                return dynamic_pointer_cast_throw<VulkanBatchResource>(j);
            }
        );
    };
    cast_append(jobs);
    cast_append(wait_semaphores);
    cast_append(signal_semaphores);

    mGraphicsQueue.submit({ info }, batch_resources.fence.get());

    mBatchResourceLists.push_back(std::move(batch_resources));
}

void usagi::VulkanGpuDevice::reclaimResources()
{
    for(auto i = mBatchResourceLists.begin(); i != mBatchResourceLists.end();)
    {
        if(mDevice->getFenceStatus(i->fence.get()) == vk::Result::eSuccess)
            i = mBatchResourceLists.erase(i);
        else
            ++i;
    }
}

void usagi::VulkanGpuDevice::waitIdle()
{
    mDevice->waitIdle();
}

uint32_t usagi::VulkanGpuDevice::graphicsQueueFamily() const
{
    return mGraphicsQueueFamilyIndex;
}

vk::Queue usagi::VulkanGpuDevice::presentQueue() const
{
    return mGraphicsQueue;
}

vk::Device usagi::VulkanGpuDevice::device() const
{
    return mDevice.get();
}

vk::PhysicalDevice usagi::VulkanGpuDevice::physicalDevice() const
{
    return mPhysicalDevice;
}
