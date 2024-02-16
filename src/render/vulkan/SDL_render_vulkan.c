/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#if defined(SDL_VIDEO_RENDER_VULKAN) && !defined(SDL_RENDER_DISABLED)

#define SDL_VULKAN_FRAME_QUEUE_DEPTH 2
#define SDL_VULKAN_NUM_VERTEX_BUFFERS 256
#define SDL_VULKAN_MAX_NUM_TEXTURES   16384
#define SDL_VULKAN_NUM_UPLOAD_BUFFERS 32


/* Renderpass types */
typedef enum {
    SDL_VULKAN_RENDERPASS_LOAD = 0,
    SDL_VULKAN_RENDERPASS_CLEAR = 1,
    SDL_VULKAN_NUM_RENDERPASSES
} SDL_vulkan_renderpass_type;

#define VK_NO_PROTOTYPES
#include "SDL_vulkan.h"
#include "SDL_shaders_vulkan.h"
#include <vulkan/vulkan.h>
#include "../SDL_sysrender.h"
#include "../SDL_sysvideo.h"

extern const char *SDL_Vulkan_GetResultString(VkResult result);

#define VULKAN_FUNCTIONS()                                              \
    VULKAN_DEVICE_FUNCTION(vkAcquireNextImageKHR)                       \
    VULKAN_DEVICE_FUNCTION(vkAllocateCommandBuffers)                    \
    VULKAN_DEVICE_FUNCTION(vkBeginCommandBuffer)                        \
    VULKAN_DEVICE_FUNCTION(vkCmdBeginRenderPass)                        \
    VULKAN_DEVICE_FUNCTION(vkCmdClearColorImage)                        \
    VULKAN_DEVICE_FUNCTION(vkCmdEndRenderPass)                          \
    VULKAN_DEVICE_FUNCTION(vkCmdPipelineBarrier)                        \
    VULKAN_DEVICE_FUNCTION(vkCreateCommandPool)                         \
    VULKAN_DEVICE_FUNCTION(vkCreateDescriptorSetLayout)                 \
    VULKAN_DEVICE_FUNCTION(vkCreateFence)                               \
    VULKAN_DEVICE_FUNCTION(vkCreateFramebuffer)                         \
    VULKAN_DEVICE_FUNCTION(vkCreateGraphicsPipelines)                   \
    VULKAN_DEVICE_FUNCTION(vkCreateImageView)                           \
    VULKAN_DEVICE_FUNCTION(vkCreatePipelineLayout)                      \
    VULKAN_DEVICE_FUNCTION(vkCreateRenderPass)                          \
    VULKAN_DEVICE_FUNCTION(vkCreateSemaphore)                           \
    VULKAN_DEVICE_FUNCTION(vkCreateShaderModule)                        \
    VULKAN_DEVICE_FUNCTION(vkCreateSwapchainKHR)                        \
    VULKAN_DEVICE_FUNCTION(vkDestroyCommandPool)                        \
    VULKAN_DEVICE_FUNCTION(vkDestroyDevice)                             \
    VULKAN_DEVICE_FUNCTION(vkDestroyDescriptorSetLayout)                \
    VULKAN_DEVICE_FUNCTION(vkDestroyFence)                              \
    VULKAN_DEVICE_FUNCTION(vkDestroyFramebuffer)                        \
    VULKAN_DEVICE_FUNCTION(vkDestroyImageView)                          \
    VULKAN_DEVICE_FUNCTION(vkDestroyPipeline)                           \
    VULKAN_DEVICE_FUNCTION(vkDestroyPipelineLayout)                     \
    VULKAN_DEVICE_FUNCTION(vkDestroyRenderPass)                         \
    VULKAN_DEVICE_FUNCTION(vkDestroySemaphore)                          \
    VULKAN_DEVICE_FUNCTION(vkDestroyShaderModule)                       \
    VULKAN_DEVICE_FUNCTION(vkDestroySwapchainKHR)                       \
    VULKAN_DEVICE_FUNCTION(vkDeviceWaitIdle)                            \
    VULKAN_DEVICE_FUNCTION(vkEndCommandBuffer)                          \
    VULKAN_DEVICE_FUNCTION(vkFreeCommandBuffers)                        \
    VULKAN_DEVICE_FUNCTION(vkGetDeviceQueue)                            \
    VULKAN_DEVICE_FUNCTION(vkGetFenceStatus)                            \
    VULKAN_DEVICE_FUNCTION(vkGetSwapchainImagesKHR)                     \
    VULKAN_DEVICE_FUNCTION(vkQueuePresentKHR)                           \
    VULKAN_DEVICE_FUNCTION(vkQueueSubmit)                               \
    VULKAN_DEVICE_FUNCTION(vkResetCommandBuffer)                        \
    VULKAN_DEVICE_FUNCTION(vkResetFences)                               \
    VULKAN_DEVICE_FUNCTION(vkWaitForFences)                             \
    VULKAN_GLOBAL_FUNCTION(vkCreateInstance)                            \
    VULKAN_GLOBAL_FUNCTION(vkEnumerateInstanceExtensionProperties)      \
    VULKAN_GLOBAL_FUNCTION(vkEnumerateInstanceLayerProperties)          \
    VULKAN_INSTANCE_FUNCTION(vkCreateDevice)                            \
    VULKAN_INSTANCE_FUNCTION(vkDestroyInstance)                         \
    VULKAN_INSTANCE_FUNCTION(vkDestroySurfaceKHR)                       \
    VULKAN_INSTANCE_FUNCTION(vkEnumerateDeviceExtensionProperties)      \
    VULKAN_INSTANCE_FUNCTION(vkEnumeratePhysicalDevices)                \
    VULKAN_INSTANCE_FUNCTION(vkGetDeviceProcAddr)                       \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceFeatures)               \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceProperties)             \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties)  \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    VULKAN_INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR)

#define VULKAN_DEVICE_FUNCTION(name)   static PFN_##name name = NULL;
#define VULKAN_GLOBAL_FUNCTION(name)   static PFN_##name name = NULL;
#define VULKAN_INSTANCE_FUNCTION(name) static PFN_##name name = NULL;
VULKAN_FUNCTIONS()
#undef VULKAN_DEVICE_FUNCTION
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION

/* Vertex shader, common values */
typedef struct
{
#if D3D12_PORT
    Float4X4 model;
    Float4X4 projectionAndView;
#else
	int port;
#endif
} VertexShaderConstants;

/* Per-vertex data */
typedef struct
{
    float pos[2];
    float tex[2];
    SDL_FColor color;
} VertexPositionColor;

/* Per-texture data */
typedef struct
{
#if D3D12_PORT
    IVULKANResource *mainTexture;
    VULKAN_CPU_DESCRIPTOR_HANDLE mainTextureResourceView;
    VULKAN_RESOURCE_STATES mainResourceState;
    SIZE_T mainSRVIndex;
    VULKAN_CPU_DESCRIPTOR_HANDLE mainTextureRenderTargetView;
    DXGI_FORMAT mainTextureFormat;
    IVULKANResource *stagingBuffer;
    VULKAN_RESOURCE_STATES stagingResourceState;
    VULKAN_FILTER scaleMode;
#if SDL_HAVE_YUV
    /* YV12 texture support */
    SDL_bool yuv;
    IVULKANResource *mainTextureU;
    VULKAN_CPU_DESCRIPTOR_HANDLE mainTextureResourceViewU;
    VULKAN_RESOURCE_STATES mainResourceStateU;
    SIZE_T mainSRVIndexU;
    IVULKANResource *mainTextureV;
    VULKAN_CPU_DESCRIPTOR_HANDLE mainTextureResourceViewV;
    VULKAN_RESOURCE_STATES mainResourceStateV;
    SIZE_T mainSRVIndexV;

    /* NV12 texture support */
    SDL_bool nv12;
    VULKAN_CPU_DESCRIPTOR_HANDLE mainTextureResourceViewNV;
    SIZE_T mainSRVIndexNV;

    Uint8 *pixels;
    int pitch;
#endif
#endif
    SDL_Rect lockedRect;
} VULKAN_TextureData;

/* Pipeline State Object data */
typedef struct
{
    VULKAN_Shader shader;
    SDL_BlendMode blendMode;
    VkPrimitiveTopology topology;
    VkFormat format;
    VkPipeline pipeline;
} VULKAN_PipelineState;

/* Vertex Buffer */
typedef struct
{
#if D3D12_PORT
    IVULKANResource *resource;
    VULKAN_VERTEX_BUFFER_VIEW view;
    size_t size;
#else
    int port;
#endif

} VULKAN_VertexBuffer;

/* For SRV pool allocator */
typedef struct
{
#if D3D12_PORT
    SIZE_T index;
    void *next;
#else
    int port;
#endif
} VULKAN_SRVPoolNode;

/* Private renderer data */
typedef struct
{
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceProperties physicalDeviceProperties;
    VkPhysicalDeviceFeatures physicalDeviceFeatures;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    VkDevice device;
    uint32_t graphicsQueueFamilyIndex;
    uint32_t presentQueueFamilyIndex;
    VkSwapchainKHR swapchain;
    VkCommandPool commandPool;
    VkCommandBuffer *commandBuffers;
    uint32_t currentCommandBufferIndex;
    VkCommandBuffer currentCommandBuffer;
    VkFence *fences;
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    VkSurfaceFormatKHR *surfaceFormats;

    VkFramebuffer *framebuffers;
    VkRenderPass renderPasses[SDL_VULKAN_NUM_RENDERPASSES];
    VkRenderPass currentRenderPass;

    VkShaderModule vertexShaderModules[NUM_SHADERS];
    VkShaderModule fragmentShaderModules[NUM_SHADERS];

    int pipelineStateCount;
    VULKAN_PipelineState *pipelineStates;
    VULKAN_PipelineState *currentPipelineState;

    uint32_t surfaceFormatsAllocatedCount;
    uint32_t surfaceFormatsCount;
    uint32_t swapchainDesiredImageCount;
    VkSurfaceFormatKHR surfaceFormat;
    VkExtent2D swapchainSize;
    uint32_t swapchainImageCount;
    VkImage *swapchainImages;
    VkImageView *swapChainImageViews;
    VkImageLayout *swapChainImageLayouts;
    VkSemaphore imageAvailableSemaphore;
    uint32_t currentSwapchainImageIndex;
    
    /* Cached renderer properties */
    SDL_bool cliprectDirty;
    SDL_bool currentCliprectEnabled;
    SDL_Rect currentCliprect;
    SDL_Rect currentViewport;
    int currentViewportRotation;
    SDL_bool viewportDirty;
    int currentVertexBuffer;
    SDL_bool issueBatch;


} VULKAN_RenderData;


static void VULKAN_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture);

static void VULKAN_DestroyAll(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data;
    if (renderer == NULL) {
        return;
    }
    data = (VULKAN_RenderData *)renderer->driverdata;
    if (data == NULL) {
        return;
    }
    
    if (data->surfaceFormats != NULL) {
        SDL_free(data->surfaceFormats);
        data->surfaceFormats = NULL;
    }
    if (data->swapchainImages != NULL) {
        SDL_free(data->swapchainImages);
        data->swapchainImages = NULL;
    }
    if (data->swapchain) {
        vkDestroySwapchainKHR(data->device, data->swapchain, NULL);
        data->swapchain = VK_NULL_HANDLE;
    }
    if (data->fences != NULL) {
        for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
            if (data->fences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(data->device, data->fences[i], NULL);
                data->fences[i] = VK_NULL_HANDLE;
            }
        }
        SDL_free( data->fences );
        data->fences = NULL;
    }
    if (data->swapChainImageViews) {
        for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
            if (data->swapChainImageViews[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(data->device, data->swapChainImageViews[i], NULL);
            }
        }
        SDL_free(data->swapChainImageViews);
        data->swapChainImageViews = NULL;
    }
    if (data->swapChainImageLayouts) {
        SDL_free(data->swapChainImageLayouts);
        data->swapChainImageLayouts = NULL;
    }
    if (data->framebuffers) {
        for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
            if (data->framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(data->device, data->framebuffers[i], NULL);
            }
        }
        SDL_free(data->framebuffers);
        data->framebuffers = NULL;
    }
    for (uint32_t i = 0; i < SDL_VULKAN_NUM_RENDERPASSES; i++) {
        if (data->renderPasses[i] != VK_NULL_HANDLE) {
            vkDestroyRenderPass(data->device, data->renderPasses[i], NULL);
            data->renderPasses[i] = VK_NULL_HANDLE;
        }
    }
    if (data->imageAvailableSemaphore) {
        vkDestroySemaphore(data->device, data->imageAvailableSemaphore, NULL);
        data->imageAvailableSemaphore = NULL;
    }
    if (data->commandPool) {
        if (data->commandBuffers) {
            vkFreeCommandBuffers(data->device, data->commandPool, data->swapchainImageCount, data->commandBuffers);
            SDL_free(data->commandBuffers);
            data->commandBuffers = NULL;
        }
        vkDestroyCommandPool(data->device, data->commandPool, NULL);
        data->commandPool = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < NUM_SHADERS; i++) {
        if (data->vertexShaderModules[i] != VK_NULL_HANDLE) {
            vkDestroyShaderModule(data->device, data->vertexShaderModules[i], NULL);
            data->vertexShaderModules[i] = VK_NULL_HANDLE;
        }
        if (data->fragmentShaderModules[i] != VK_NULL_HANDLE) {
            vkDestroyShaderModule(data->device, data->fragmentShaderModules[i], NULL);
            data->fragmentShaderModules[i] = VK_NULL_HANDLE;
        }
    }

    if (data->device != VK_NULL_HANDLE) {
        vkDestroyDevice(data->device, NULL);
        data->device = VK_NULL_HANDLE;
    }
    if (data->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(data->instance, data->surface, NULL);
        data->surface = VK_NULL_HANDLE;
    }
    if (data->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(data->instance, NULL);
        data->instance = VK_NULL_HANDLE;
    }
}

static void VULKAN_RecordPipelineImageBarrier(SDL_Renderer *renderer, VkAccessFlags sourceAccessMask, VkAccessFlags destAccessMask,
    VkPipelineStageFlags srcStageFlags, VkPipelineStageFlags dstStageFlags, VkImageLayout sourceLayout, VkImageLayout destLayout, VkImage image)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VkImageMemoryBarrier barrier = { 0 };
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccessMask;
    barrier.dstAccessMask = destAccessMask;
    barrier.oldLayout = sourceLayout;
    barrier.newLayout = destLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(data->currentCommandBuffer, srcStageFlags, dstStageFlags, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static SDL_bool VULKAN_ActivateCommandBuffer(SDL_Renderer *renderer, VkAttachmentLoadOp loadOp, VkClearColorValue *clearColor)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;

    /* Our SetRenderTarget just signals that the next render operation should
     * set up a new render pass. This is where that work happens. */
    if (data->currentCommandBuffer == VK_NULL_HANDLE) {

        // TODO: Probably doesn't belong here
        VkResult result;
        
        result = vkAcquireNextImageKHR(data->device, data->swapchain, UINT64_MAX,
                                      data->imageAvailableSemaphore, VK_NULL_HANDLE, &data->currentSwapchainImageIndex);
        if (result != VK_SUCCESS) {
            // TODO: Handle out-of-date, suboptimal, etc.
            return SDL_FALSE;
        }

        data->currentCommandBuffer = data->commandBuffers[data->currentCommandBufferIndex];
        vkResetCommandBuffer(data->currentCommandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo = { 0 };
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        vkBeginCommandBuffer(data->currentCommandBuffer, &beginInfo);

        if (data->swapChainImageLayouts[data->currentCommandBufferIndex] == VK_IMAGE_LAYOUT_UNDEFINED) {
            VULKAN_RecordPipelineImageBarrier(renderer,
                0,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                data->swapchainImages[data->currentCommandBufferIndex]);
        }
        else
        {
            VULKAN_RecordPipelineImageBarrier(renderer,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                data->swapChainImageLayouts[data->currentCommandBufferIndex],
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                data->swapchainImages[data->currentCommandBufferIndex]);
        }
        data->swapChainImageLayouts[data->currentCommandBufferIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    
    if (data->currentRenderPass != VK_NULL_HANDLE) {
        vkCmdEndRenderPass(data->currentCommandBuffer);
        data->currentRenderPass = VK_NULL_HANDLE;
    }

    switch (loadOp) {
    case VK_ATTACHMENT_LOAD_OP_CLEAR:
        data->currentRenderPass = data->renderPasses[SDL_VULKAN_RENDERPASS_CLEAR];
        break;

    case VK_ATTACHMENT_LOAD_OP_LOAD:
    default:
        data->currentRenderPass = data->renderPasses[SDL_VULKAN_RENDERPASS_LOAD];
        break;
    }

    VkRenderPassBeginInfo renderPassBeginInfo = { 0 };
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.pNext = NULL;
    renderPassBeginInfo.renderPass = data->currentRenderPass;
    renderPassBeginInfo.framebuffer = data->framebuffers[data->currentCommandBufferIndex];
    renderPassBeginInfo.renderArea.offset.x = data->currentViewport.x;
    renderPassBeginInfo.renderArea.offset.y = data->currentViewport.y;
    renderPassBeginInfo.renderArea.extent.width = data->currentViewport.w;
    renderPassBeginInfo.renderArea.extent.height = data->currentViewport.h;
    renderPassBeginInfo.clearValueCount = (clearColor == NULL) ? 0 : 1;
    VkClearValue clearValue;
    if (clearColor != NULL) {
        clearValue.color = *clearColor;
        renderPassBeginInfo.pClearValues = &clearValue;
    }
    vkCmdBeginRenderPass(data->currentCommandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    return SDL_TRUE;
}

static void VULKAN_WaitForGPU(VULKAN_RenderData *data)
{
#if D3D12_PORT
    if (data->commandQueue && data->fence && data->fenceEvent) {
        D3D_CALL(data->commandQueue, Signal, data->fence, data->fenceValue);
        if (D3D_CALL(data->fence, GetCompletedValue) < data->fenceValue) {
            D3D_CALL(data->fence, SetEventOnCompletion,
                     data->fenceValue,
                     data->fenceEvent);
            WaitForSingleObjectEx(data->fenceEvent, INFINITE, FALSE);
        }

        data->fenceValue++;
    }
#endif
}


static void VULKAN_ResetCommandList(VULKAN_RenderData *data)
{
#if D3D12_PORT
    int i;
    IVULKANDescriptorHeap *rootDescriptorHeaps[] = { data->srvDescriptorHeap, data->samplerDescriptorHeap };
    IVULKANCommandAllocator *commandAllocator = data->commandAllocators[data->currentBackBufferIndex];

    D3D_CALL(commandAllocator, Reset);
    D3D_CALL(data->commandList, Reset, commandAllocator, NULL);
    data->currentPipelineState = NULL;
    data->currentVertexBuffer = 0;
    data->issueBatch = SDL_FALSE;
    data->cliprectDirty = SDL_TRUE;
    data->viewportDirty = SDL_TRUE;
    data->currentRenderTargetView.ptr = 0;
    /* FIXME should we also clear currentSampler.ptr and currentRenderTargetView.ptr ? (and use VULKAN_InvalidateCachedState() instead) */

    /* Release any upload buffers that were inflight */
    for (i = 0; i < data->currentUploadBuffer; ++i) {
        SAFE_RELEASE(data->uploadBuffers[i]);
    }
    data->currentUploadBuffer = 0;

    D3D_CALL(data->commandList, SetDescriptorHeaps, 2, rootDescriptorHeaps);
#endif
}

static int VULKAN_IssueBatch(VULKAN_RenderData *data)
{
#if D3D12_PORT

    HRESULT result = S_OK;

    /* Issue the command list */
    result = D3D_CALL(data->commandList, Close);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("VULKAN_IssueBatch"), result);
        return result;
    }
    D3D_CALL(data->commandQueue, ExecuteCommandLists, 1, (IVULKANCommandList *const *)&data->commandList);

    VULKAN_WaitForGPU(data);

    VULKAN_ResetCommandList(data);

    return result;
#endif
}

static void VULKAN_DestroyRenderer(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_WaitForGPU(data);
    VULKAN_DestroyAll(renderer);
    if (data) {
        SDL_free(data);
    }
    SDL_free(renderer);
}

static VkBlendFactor GetBlendFactor(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case SDL_BLENDFACTOR_ONE:
        return VK_BLEND_FACTOR_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default:
        return VK_BLEND_FACTOR_ZERO;
    }
}

static VkBlendOp GetBlendOp(SDL_BlendOperation operation)
{
    switch (operation) {
    case SDL_BLENDOPERATION_ADD:
        return VK_BLEND_OP_ADD;
    case SDL_BLENDOPERATION_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case SDL_BLENDOPERATION_MINIMUM:
        return VK_BLEND_OP_MIN;
    case SDL_BLENDOPERATION_MAXIMUM:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}


static VULKAN_PipelineState *VULKAN_CreatePipelineState(SDL_Renderer *renderer,
    VULKAN_Shader shader, SDL_BlendMode blendMode, VkPrimitiveTopology topology, VkFormat format)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_PipelineState *pipelineStates;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo = { 0 };
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo = { 0 };
    VkVertexInputAttributeDescription attributeDescriptions[3];
    VkVertexInputBindingDescription bindingDescriptions[1];
    VkPipelineShaderStageCreateInfo shaderStageCreateInfo[2];
    VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo = { 0 };
    VkPipelineViewportStateCreateInfo viewportStateCreateInfo = { 0 };
    VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo = { 0 };
    VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo = { 0 };
    VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = { 0 };
    VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo = { 0 };

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = { 0 };
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.flags = 0;
    pipelineCreateInfo.pStages = shaderStageCreateInfo;
    pipelineCreateInfo.pVertexInputState = &vertexInputCreateInfo;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
    pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
    pipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
    pipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
    pipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
    pipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
    pipelineCreateInfo.pDynamicState = &dynamicStateCreateInfo;

    /* Shaders */
    const char *name = "main";
    for (uint32_t i = 0; i < 2; i++) {
        SDL_memset(&shaderStageCreateInfo[i], 0, sizeof(shaderStageCreateInfo[i]));
        shaderStageCreateInfo[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageCreateInfo[i].module = (i == 0) ? data->vertexShaderModules[shader] : data->fragmentShaderModules[shader];
        shaderStageCreateInfo[i].stage = (i == 0) ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStageCreateInfo[i].pName = name;
    }
    pipelineCreateInfo.stageCount = 2;
    pipelineCreateInfo.pStages = &shaderStageCreateInfo[0];


    /* Vertex input */
    vertexInputCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputCreateInfo.vertexAttributeDescriptionCount = 3;
    vertexInputCreateInfo.pVertexAttributeDescriptions = &attributeDescriptions[0];
    vertexInputCreateInfo.vertexBindingDescriptionCount = 1;
    vertexInputCreateInfo.pVertexBindingDescriptions = &bindingDescriptions[0];

    attributeDescriptions[ 0 ].binding = 0;
    attributeDescriptions[ 0 ].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[ 0 ].location = 0;
    attributeDescriptions[ 0 ].offset = 0;
    attributeDescriptions[ 1 ].binding = 0;
    attributeDescriptions[ 1 ].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[ 1 ].location = 1;
    attributeDescriptions[ 1 ].offset = 8;
    attributeDescriptions[ 2 ].binding = 0;
    attributeDescriptions[ 2 ].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[ 2 ].location = 2;
    attributeDescriptions[ 2 ].offset = 16;

    bindingDescriptions[ 0 ].binding = 0;
    bindingDescriptions[ 0 ].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindingDescriptions[ 0 ].stride = 32;

    /* Input assembly */
    inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyStateCreateInfo.topology = ( VkPrimitiveTopology ) topology;
    inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;

    viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;

    /* Dynamic states */
    dynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    VkDynamicState dynamicStates[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    dynamicStateCreateInfo.dynamicStateCount = SDL_arraysize(dynamicStates);
    dynamicStateCreateInfo.pDynamicStates = dynamicStates;

    /* Rasterization state */
    rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationStateCreateInfo.depthClampEnable = VK_FALSE;
    rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
    rasterizationStateCreateInfo.cullMode = VK_CULL_MODE_NONE;
    rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationStateCreateInfo.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationStateCreateInfo.depthBiasEnable = VK_FALSE;
    rasterizationStateCreateInfo.depthBiasConstantFactor = 0.0f;
    rasterizationStateCreateInfo.depthBiasClamp = 0.0f;
    rasterizationStateCreateInfo.depthBiasSlopeFactor = 0.0f;
    rasterizationStateCreateInfo.lineWidth = 1.0f;

    /* MSAA state */
    multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    VkSampleMask multiSampleMask = 0xFFFFFFFF;
    multisampleStateCreateInfo.pSampleMask = &multiSampleMask;

    /* Depth Stencil */
    depthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    /* Color blend */
    VkPipelineColorBlendAttachmentState colorBlendAttachment = { 0 };
    colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendStateCreateInfo.attachmentCount = 1;
    colorBlendStateCreateInfo.pAttachments = &colorBlendAttachment;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = GetBlendFactor(SDL_GetBlendModeSrcColorFactor(blendMode));
    colorBlendAttachment.srcAlphaBlendFactor = GetBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blendMode));
    colorBlendAttachment.colorBlendOp = GetBlendOp(SDL_GetBlendModeColorOperation(blendMode));
    colorBlendAttachment.dstColorBlendFactor = GetBlendFactor(SDL_GetBlendModeDstColorFactor(blendMode));
    colorBlendAttachment.dstAlphaBlendFactor = GetBlendFactor(SDL_GetBlendModeDstAlphaFactor(blendMode));
    colorBlendAttachment.alphaBlendOp = GetBlendOp(SDL_GetBlendModeAlphaOperation(blendMode));
    colorBlendAttachment.colorWriteMask = 0xFFFFFFFF;


    /* Renderpass / layout */
    pipelineCreateInfo.renderPass = data->currentRenderPass;
    pipelineCreateInfo.subpass = 0;
    pipelineCreateInfo.layout = VK_NULL_HANDLE; // TODO

    result = vkCreateGraphicsPipelines(data->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, NULL, &pipeline);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateGraphicsPipelines(): %s\n", SDL_Vulkan_GetResultString(result));
        return NULL;
    }

    pipelineStates = (VULKAN_PipelineState *)SDL_realloc(data->pipelineStates, (data->pipelineStateCount + 1) * sizeof(*pipelineStates));
    pipelineStates[data->pipelineStateCount].shader = shader;
    pipelineStates[data->pipelineStateCount].blendMode = blendMode;
    pipelineStates[data->pipelineStateCount].topology = topology;
    pipelineStates[data->pipelineStateCount].format = format;
    pipelineStates[data->pipelineStateCount].pipeline = pipeline;
    data->pipelineStates = pipelineStates;
    ++data->pipelineStateCount;

    return &pipelineStates[data->pipelineStateCount - 1];
}

static VkResult VULKAN_CreateVertexBuffer(VULKAN_RenderData *data, size_t vbidx, size_t size)
{
#if D3D12_PORT
    VULKAN_HEAP_PROPERTIES vbufferHeapProps;
    VULKAN_RESOURCE_DESC vbufferDesc;
    HRESULT result;

    SAFE_RELEASE(data->vertexBuffers[vbidx].resource);

    SDL_zero(vbufferHeapProps);
    vbufferHeapProps.Type = VULKAN_HEAP_TYPE_UPLOAD;
    vbufferHeapProps.CreationNodeMask = 1;
    vbufferHeapProps.VisibleNodeMask = 1;

    SDL_zero(vbufferDesc);
    vbufferDesc.Dimension = VULKAN_RESOURCE_DIMENSION_BUFFER;
    vbufferDesc.Alignment = VULKAN_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    vbufferDesc.Width = size;
    vbufferDesc.Height = 1;
    vbufferDesc.DepthOrArraySize = 1;
    vbufferDesc.MipLevels = 1;
    vbufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    vbufferDesc.SampleDesc.Count = 1;
    vbufferDesc.SampleDesc.Quality = 0;
    vbufferDesc.Layout = VULKAN_TEXTURE_LAYOUT_ROW_MAJOR;
    vbufferDesc.Flags = VULKAN_RESOURCE_FLAG_NONE;

    result = D3D_CALL(data->d3dDevice, CreateCommittedResource,
                      &vbufferHeapProps,
                      VULKAN_HEAP_FLAG_NONE,
                      &vbufferDesc,
                      VULKAN_RESOURCE_STATE_GENERIC_READ,
                      NULL,
                      D3D_GUID(SDL_IID_IVULKANResource),
                      (void **)&data->vertexBuffers[vbidx].resource);

    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreatePlacedResource [vertex buffer]"), result);
        return result;
    }

    data->vertexBuffers[vbidx].view.BufferLocation = D3D_CALL(data->vertexBuffers[vbidx].resource, GetGPUVirtualAddress);
    data->vertexBuffers[vbidx].view.StrideInBytes = sizeof(VertexPositionColor);
    data->vertexBuffers[vbidx].size = size;

    return result;
#else
    return VK_SUCCESS;
#endif
}

static int VULKAN_LoadGlobalFunctions(VULKAN_RenderData *data)
{
#define VULKAN_DEVICE_FUNCTION(name)
#define VULKAN_GLOBAL_FUNCTION(name)                                                   \
    name = (PFN_##name)data->vkGetInstanceProcAddr(VK_NULL_HANDLE, #name);             \
    if (!name) {                                                                       \
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,                                          \
                     "vkGetInstanceProcAddr(VK_NULL_HANDLE, \"" #name "\") failed\n"); \
        return -1;                                                                     \
    }
#define VULKAN_INSTANCE_FUNCTION(name)
    VULKAN_FUNCTIONS()
#undef VULKAN_DEVICE_FUNCTION
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION

    return 0;
}

static int VULKAN_LoadInstanceFunctions(VULKAN_RenderData *data)
{
#define VULKAN_DEVICE_FUNCTION(name)
#define VULKAN_GLOBAL_FUNCTION(name)
#define VULKAN_INSTANCE_FUNCTION(name)                                              \
    name = (PFN_##name)data->vkGetInstanceProcAddr(data->instance, #name);          \
    if (!name) {                                                                    \
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,                                       \
                     "vkGetInstanceProcAddr(instance, \"" #name "\") failed\n");    \
        return -1;                                                                  \
    }
    VULKAN_FUNCTIONS()
#undef VULKAN_DEVICE_FUNCTION
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION

    return 0;
}

static int VULKAN_LoadDeviceFunctions(VULKAN_RenderData *data)
{
#define VULKAN_DEVICE_FUNCTION(name)                                         \
    name = (PFN_##name)vkGetDeviceProcAddr(data->device, #name);             \
    if (!name) {                                                             \
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,                                \
                     "vkGetDeviceProcAddr(device, \"" #name "\") failed\n"); \
        return -1;                                                           \
    }
#define VULKAN_GLOBAL_FUNCTION(name)
#define VULKAN_INSTANCE_FUNCTION(name)
    VULKAN_FUNCTIONS()
#undef VULKAN_DEVICE_FUNCTION
#undef VULKAN_GLOBAL_FUNCTION
#undef VULKAN_INSTANCE_FUNCTION
    return 0;
}

static VkResult VULKAN_FindPhysicalDevice(VULKAN_RenderData *data)
{
    uint32_t physicalDeviceCount = 0;
    VkPhysicalDevice *physicalDevices;
    VkQueueFamilyProperties *queueFamiliesProperties = NULL;
    uint32_t queueFamiliesPropertiesAllocatedSize = 0;
    VkExtensionProperties *deviceExtensions = NULL;
    uint32_t deviceExtensionsAllocatedSize = 0;
    uint32_t physicalDeviceIndex;
    VkResult result;

    result = vkEnumeratePhysicalDevices(data->instance, &physicalDeviceCount, NULL);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkEnumeratePhysicalDevices(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }
    if (physicalDeviceCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkEnumeratePhysicalDevices(): no physical devices\n");
        return VK_ERROR_UNKNOWN;
    }
    physicalDevices = (VkPhysicalDevice *)SDL_malloc(sizeof(VkPhysicalDevice) * physicalDeviceCount);
    result = vkEnumeratePhysicalDevices(data->instance, &physicalDeviceCount, physicalDevices);
    if (result != VK_SUCCESS) {
        SDL_free(physicalDevices);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,"vkEnumeratePhysicalDevices(): %s\n",SDL_Vulkan_GetResultString(result));
        return result;
    }
    data->physicalDevice = NULL;
    for (physicalDeviceIndex = 0; physicalDeviceIndex < physicalDeviceCount; physicalDeviceIndex++) {
        uint32_t queueFamiliesCount = 0;
        uint32_t queueFamilyIndex;
        uint32_t deviceExtensionCount = 0;
        SDL_bool hasSwapchainExtension = SDL_FALSE;
        uint32_t i;

        VkPhysicalDevice physicalDevice = physicalDevices[physicalDeviceIndex];
        vkGetPhysicalDeviceProperties(physicalDevice, &data->physicalDeviceProperties);
        if (VK_VERSION_MAJOR(data->physicalDeviceProperties.apiVersion) < 1) {
            continue;
        }
        vkGetPhysicalDeviceFeatures(physicalDevice, &data->physicalDeviceFeatures);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamiliesCount, NULL);
        if (queueFamiliesCount == 0) {
            continue;
        }
        if (queueFamiliesPropertiesAllocatedSize < queueFamiliesCount) {
            SDL_free(queueFamiliesProperties);
            queueFamiliesPropertiesAllocatedSize = queueFamiliesCount;
            queueFamiliesProperties = (VkQueueFamilyProperties *)SDL_malloc(sizeof(VkQueueFamilyProperties) * queueFamiliesPropertiesAllocatedSize);
            if (!queueFamiliesProperties) {
                SDL_free(physicalDevices);
                SDL_free(deviceExtensions);
                return VK_ERROR_UNKNOWN;
            }
        }
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamiliesCount, queueFamiliesProperties);
        data->graphicsQueueFamilyIndex = queueFamiliesCount;
        data->presentQueueFamilyIndex = queueFamiliesCount;
        for (queueFamilyIndex = 0; queueFamilyIndex < queueFamiliesCount; queueFamilyIndex++) {
            VkBool32 supported = 0;

            if (queueFamiliesProperties[queueFamilyIndex].queueCount == 0) {
                continue;
            }

            if (queueFamiliesProperties[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                data->graphicsQueueFamilyIndex = queueFamilyIndex;
            }

            result = vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, data->surface, &supported);
            if (result != VK_SUCCESS) {
                SDL_free(physicalDevices);
                SDL_free(queueFamiliesProperties);
                SDL_free(deviceExtensions);
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetPhysicalDeviceSurfaceSupportKHR(): %s\n", SDL_Vulkan_GetResultString(result));
                return VK_ERROR_UNKNOWN;
            }
            if (supported) {
                data->presentQueueFamilyIndex = queueFamilyIndex;
                if (queueFamiliesProperties[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    break; // use this queue because it can present and do graphics
                }
            }
        }

        if (data->graphicsQueueFamilyIndex == queueFamiliesCount) { // no good queues found
            continue;
        }
        if (data->presentQueueFamilyIndex == queueFamiliesCount) { // no good queues found
            continue;
        }
        result = vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, NULL);
        if (result != VK_SUCCESS) {
            SDL_free(physicalDevices);
            SDL_free(queueFamiliesProperties);
            SDL_free(deviceExtensions);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkEnumerateDeviceExtensionProperties(): %s\n", SDL_Vulkan_GetResultString(result));
            return VK_ERROR_UNKNOWN;
        }
        if (deviceExtensionCount == 0) {
            continue;
        }
        if (deviceExtensionsAllocatedSize < deviceExtensionCount) {
            SDL_free(deviceExtensions);
            deviceExtensionsAllocatedSize = deviceExtensionCount;
            deviceExtensions = SDL_malloc(sizeof(VkExtensionProperties) * deviceExtensionsAllocatedSize);
            if (!deviceExtensions) {
                SDL_free(physicalDevices);
                SDL_free(queueFamiliesProperties);
                return VK_ERROR_UNKNOWN;
            }
        }
        result = vkEnumerateDeviceExtensionProperties(physicalDevice, NULL, &deviceExtensionCount, deviceExtensions);
        if (result != VK_SUCCESS) {
            SDL_free(physicalDevices);
            SDL_free(queueFamiliesProperties);
            SDL_free(deviceExtensions);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkEnumerateDeviceExtensionProperties(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }
        for (i = 0; i < deviceExtensionCount; i++) {
            if (SDL_strcmp(deviceExtensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                hasSwapchainExtension = SDL_TRUE;
                break;
            }
        }
        if (!hasSwapchainExtension) {
            continue;
        }
        data->physicalDevice = physicalDevice;
        break;
    }
    SDL_free(physicalDevices);
    SDL_free(queueFamiliesProperties);
    SDL_free(deviceExtensions);
    if (!data->physicalDevice) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan: no viable physical devices found");
        return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

static VkResult VULKAN_GetSurfaceFormats(VULKAN_RenderData *data)
{
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(data->physicalDevice,
                                                           data->surface,
                                                           &data->surfaceFormatsCount,
                                                           NULL);
    if (result != VK_SUCCESS) {
        data->surfaceFormatsCount = 0;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetPhysicalDeviceSurfaceFormatsKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }
    if (data->surfaceFormatsCount > data->surfaceFormatsAllocatedCount) {
        data->surfaceFormatsAllocatedCount = data->surfaceFormatsCount;
        SDL_free(data->surfaceFormats);
        data->surfaceFormats = (VkSurfaceFormatKHR *)SDL_malloc(sizeof(VkSurfaceFormatKHR) * data->surfaceFormatsAllocatedCount);
    }
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(data->physicalDevice,
                                                  data->surface,
                                                  &data->surfaceFormatsCount,
                                                  data->surfaceFormats);
    if (result != VK_SUCCESS) {
        data->surfaceFormatsCount = 0;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetPhysicalDeviceSurfaceFormatsKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }
    
    return VK_SUCCESS;
}

static VkSemaphore VULKAN_CreateSemaphore(VULKAN_RenderData *data)
{
    VkResult result;
    VkSemaphore semaphore = VK_NULL_HANDLE;

    VkSemaphoreCreateInfo semaphoreCreateInfo = { 0 };
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    result = vkCreateSemaphore(data->device, &semaphoreCreateInfo, NULL, &semaphore);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateSemaphore(): %s\n", SDL_Vulkan_GetResultString(result));
        return VK_NULL_HANDLE;
    }
    return semaphore;
}

static SDL_bool VULKAN_ValidationLayersFound()
{
    const char *validationLayerName = "VK_LAYER_KHRONOS_validation";
    uint32_t instanceLayerCount = 0;
    uint32_t i;
    SDL_bool foundValidation = SDL_FALSE;
    
    vkEnumerateInstanceLayerProperties(&instanceLayerCount, NULL);
    if (instanceLayerCount > 0) {
        VkLayerProperties *instanceLayers = SDL_calloc(instanceLayerCount, sizeof(VkLayerProperties));
        vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers);
        for (i = 0; i < instanceLayerCount; i++) {
            if (!SDL_strcmp(validationLayerName, instanceLayers[i].layerName)) {
                foundValidation = SDL_TRUE;
                break;
            }
        }
        SDL_free(instanceLayers);
    }
    
    return foundValidation;
}

/* Create resources that depend on the device. */
static VkResult VULKAN_CreateDeviceResources(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    VkResult result = VK_SUCCESS;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;
    SDL_bool createDebug = SDL_GetHintBoolean(SDL_HINT_RENDER_VULKAN_DEBUG, SDL_FALSE);

    if (SDL_Vulkan_LoadLibrary(NULL) < 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, "SDL_Vulkan_LoadLibrary failed." );
        return VK_ERROR_UNKNOWN;
    }
    vkGetInstanceProcAddr = device ? (PFN_vkGetInstanceProcAddr)device->vulkan_config.vkGetInstanceProcAddr : NULL;
    if(!vkGetInstanceProcAddr) {
        SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, "vkGetInstanceProcAddr is NULL" );
        return VK_ERROR_UNKNOWN;
    }

    /* Load global Vulkan functions */
    data->vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    if (VULKAN_LoadGlobalFunctions(data) != 0) {
        return VK_ERROR_UNKNOWN;
    }

    /* Create VkInstance */
    VkInstanceCreateInfo instanceCreateInfo = { 0 };
    VkApplicationInfo appInfo = { 0 };
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_0;
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &appInfo;
    instanceCreateInfo.ppEnabledExtensionNames = SDL_Vulkan_GetInstanceExtensions(&instanceCreateInfo.enabledExtensionCount);
    if (createDebug && VULKAN_ValidationLayersFound()) {
        const char *validationLayerName[] = { "VK_LAYER_KHRONOS_validation" };
        instanceCreateInfo.ppEnabledLayerNames = validationLayerName;
        instanceCreateInfo.enabledLayerCount = 1;
    }
    result = vkCreateInstance(&instanceCreateInfo, NULL, &data->instance);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateInstance(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }

    /* Load instance Vulkan functions */
    if (VULKAN_LoadInstanceFunctions(data) != 0) {
        VULKAN_DestroyAll(renderer);
        return VK_ERROR_UNKNOWN;
    }

    /* Create Vulkan surface */
    if (!device->Vulkan_CreateSurface || !device->Vulkan_CreateSurface(device, renderer->window, data->instance, NULL, &data->surface)) {
        VULKAN_DestroyAll(renderer);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Vulkan_CreateSurface() failed.\n");
        return VK_ERROR_UNKNOWN;
    }

    /* Choose Vulkan physical device */
    if (VULKAN_FindPhysicalDevice(data) != VK_SUCCESS) {
        VULKAN_DestroyAll(renderer);
        return VK_ERROR_UNKNOWN;
    }

    /* Create Vulkan device */
    VkDeviceQueueCreateInfo deviceQueueCreateInfo[1] = { { 0 } };
    static const float queuePriority[] = { 1.0f };
    VkDeviceCreateInfo deviceCreateInfo = { 0 };
    static const char *const deviceExtensionNames[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    deviceQueueCreateInfo->sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    deviceQueueCreateInfo->queueFamilyIndex = data->graphicsQueueFamilyIndex;
    deviceQueueCreateInfo->queueCount = 1;
    deviceQueueCreateInfo->pQueuePriorities = &queuePriority[0];

    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = deviceQueueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = NULL;
    deviceCreateInfo.enabledExtensionCount = SDL_arraysize(deviceExtensionNames);
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensionNames;
    result = vkCreateDevice(data->physicalDevice, &deviceCreateInfo, NULL, &data->device);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateDevice(): %s\n", SDL_Vulkan_GetResultString(result));
        VULKAN_DestroyAll(renderer);
        return result;
    }

    if(VULKAN_LoadDeviceFunctions(data) != 0) {
        VULKAN_DestroyAll(renderer);
        return VK_ERROR_UNKNOWN;
    }
    
    /* Get graphics/present queues */
    vkGetDeviceQueue(data->device, data->graphicsQueueFamilyIndex, 0, &data->graphicsQueue);
    if (data->graphicsQueueFamilyIndex != data->presentQueueFamilyIndex) {
        vkGetDeviceQueue(data->device, data->presentQueueFamilyIndex, 0, &data->presentQueue);
    } else {
        data->presentQueue = data->graphicsQueue;
    }
    
    /* Create command pool/command buffers */
    VkCommandPoolCreateInfo commandPoolCreateInfo = { 0 };
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = data->graphicsQueueFamilyIndex;
    result = vkCreateCommandPool(data->device, &commandPoolCreateInfo, NULL, &data->commandPool);
    if (result != VK_SUCCESS) {
        VULKAN_DestroyAll(renderer);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateCommandPool(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }
    
    /* Create semaphores */
    data->imageAvailableSemaphore = VULKAN_CreateSemaphore(data);
    if (data->imageAvailableSemaphore == VK_NULL_HANDLE) {
        VULKAN_DestroyAll(renderer);
        return VK_ERROR_UNKNOWN;
    }
    if (VULKAN_GetSurfaceFormats(data) != VK_SUCCESS) {
        VULKAN_DestroyAll(renderer);
        return result;
    }

    /* Create shaders */
    for (uint32_t i = 0; i < NUM_SHADERS; i++) {
        VkShaderModuleCreateInfo shaderModuleCreateInfo = { 0 };
        shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        VULKAN_GetVertexShader(i, &shaderModuleCreateInfo.pCode, &shaderModuleCreateInfo.codeSize);
        result = vkCreateShaderModule(data->device, &shaderModuleCreateInfo, NULL, &data->vertexShaderModules[i]);
        if (result != VK_SUCCESS) {
            VULKAN_DestroyAll(renderer);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateShaderModule(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }
        VULKAN_GetPixelShader(i, &shaderModuleCreateInfo.pCode, &shaderModuleCreateInfo.codeSize);
        result = vkCreateShaderModule(data->device, &shaderModuleCreateInfo, NULL, &data->fragmentShaderModules[i]);
        if (result != VK_SUCCESS) {
            VULKAN_DestroyAll(renderer);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateShaderModule(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }
    }

    return VK_SUCCESS;
}

#if D3D12_PORT
static DXGI_MODE_ROTATION VULKAN_GetCurrentRotation()
{
    /* FIXME */
    return DXGI_MODE_ROTATION_IDENTITY;
}

static BOOL VULKAN_IsDisplayRotated90Degrees(DXGI_MODE_ROTATION rotation)
{
    switch (rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE270:
        return TRUE;
    default:
        return FALSE;
    }
}

static int VULKAN_GetRotationForCurrentRenderTarget(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    if (data->textureRenderTarget) {
        return DXGI_MODE_ROTATION_IDENTITY;
    } else {
        return data->rotation;
    }
}

static int VULKAN_GetViewportAlignedD3DRect(SDL_Renderer *renderer, const SDL_Rect *sdlRect, VULKAN_RECT *outRect, BOOL includeViewportOffset)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    const int rotation = VULKAN_GetRotationForCurrentRenderTarget(renderer);
    const SDL_Rect *viewport = &data->currentViewport;

    switch (rotation) {
    case DXGI_MODE_ROTATION_IDENTITY:
        outRect->left = sdlRect->x;
        outRect->right = (LONG)sdlRect->x + sdlRect->w;
        outRect->top = sdlRect->y;
        outRect->bottom = (LONG)sdlRect->y + sdlRect->h;
        if (includeViewportOffset) {
            outRect->left += viewport->x;
            outRect->right += viewport->x;
            outRect->top += viewport->y;
            outRect->bottom += viewport->y;
        }
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        outRect->left = sdlRect->y;
        outRect->right = (LONG)sdlRect->y + sdlRect->h;
        outRect->top = viewport->w - sdlRect->x - sdlRect->w;
        outRect->bottom = viewport->w - sdlRect->x;
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        outRect->left = viewport->w - sdlRect->x - sdlRect->w;
        outRect->right = viewport->w - sdlRect->x;
        outRect->top = viewport->h - sdlRect->y - sdlRect->h;
        outRect->bottom = viewport->h - sdlRect->y;
        break;
    case DXGI_MODE_ROTATION_ROTATE90:
        outRect->left = viewport->h - sdlRect->y - sdlRect->h;
        outRect->right = viewport->h - sdlRect->y;
        outRect->top = sdlRect->x;
        outRect->bottom = (LONG)sdlRect->x + sdlRect->h;
        break;
    default:
        return SDL_SetError("The physical display is in an unknown or unsupported rotation");
    }
    return 0;
}
#endif

static VkResult VULKAN_CreateSwapChain(SDL_Renderer *renderer, int w, int h)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(data->physicalDevice, data->surface, &data->surfaceCapabilities);
    if (result != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }
        
    // pick an image count
    data->swapchainDesiredImageCount = data->surfaceCapabilities.minImageCount + SDL_VULKAN_FRAME_QUEUE_DEPTH;
    if ((data->swapchainDesiredImageCount > data->surfaceCapabilities.maxImageCount) &&
        (data->surfaceCapabilities.maxImageCount > 0)) {
        data->swapchainDesiredImageCount = data->surfaceCapabilities.maxImageCount;
    }
    
    if ((data->surfaceFormatsCount == 1) &&
        (data->surfaceFormats[0].format == VK_FORMAT_UNDEFINED)) {
        // aren't any preferred formats, so we pick
        data->surfaceFormat.colorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        data->surfaceFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
    } else {
        data->surfaceFormat = data->surfaceFormats[0];
        for (uint32_t i = 0; i < data->surfaceFormatsCount; i++) {
            if (data->surfaceFormats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
                data->surfaceFormat = data->surfaceFormats[i];
                break;
            }
        }
    }

    data->swapchainSize.width = SDL_clamp((uint32_t)w,
                                          data->surfaceCapabilities.minImageExtent.width,
                                          data->surfaceCapabilities.maxImageExtent.width);

    data->swapchainSize.height = SDL_clamp((uint32_t)h,
                                           data->surfaceCapabilities.minImageExtent.height,
                                           data->surfaceCapabilities.maxImageExtent.height);


    VkSwapchainCreateInfoKHR swapchainCreateInfo = { 0 };
    swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.surface = data->surface;
    swapchainCreateInfo.minImageCount = data->swapchainDesiredImageCount;
    swapchainCreateInfo.imageFormat = data->surfaceFormat.format;
    swapchainCreateInfo.imageColorSpace = data->surfaceFormat.colorSpace;
    swapchainCreateInfo.imageExtent = data->swapchainSize;
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.preTransform = data->surfaceCapabilities.currentTransform;
    //TODO
#if 0
    if (flags & SDL_WINDOW_TRANSPARENT) {
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    } else 
#endif
    {
        swapchainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // TODO
    swapchainCreateInfo.clipped = VK_TRUE;
    swapchainCreateInfo.oldSwapchain = data->swapchain;
    result = vkCreateSwapchainKHR(data->device, &swapchainCreateInfo, NULL, &data->swapchain);

    if (swapchainCreateInfo.oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(data->device, swapchainCreateInfo.oldSwapchain, NULL);
    }

    if (result != VK_SUCCESS) {
        data->swapchain = VK_NULL_HANDLE;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateSwapchainKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }

    SDL_free(data->swapchainImages);
    data->swapchainImages = NULL;
    result = vkGetSwapchainImagesKHR(data->device, data->swapchain, &data->swapchainImageCount, NULL);
    if (result != VK_SUCCESS) {
        data->swapchainImageCount = 0;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetSwapchainImagesKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }

    data->swapchainImages = SDL_malloc(sizeof(VkImage) * data->swapchainImageCount);
    result = vkGetSwapchainImagesKHR(data->device,
                                     data->swapchain,
                                     &data->swapchainImageCount,
                                     data->swapchainImages);
    if (result != VK_SUCCESS) {
        SDL_free(data->swapchainImages);
        data->swapchainImages = NULL;
        data->swapchainImageCount = 0;
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkGetSwapchainImagesKHR(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }

    /* Create VkImageView's for swapchain images */
    {
        VkImageViewCreateInfo imageViewCreateInfo = { 0 };
        imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewCreateInfo.flags = 0;
        imageViewCreateInfo.format = data->surfaceFormat.format;
        imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
        imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
        imageViewCreateInfo.subresourceRange.layerCount = 1;
        imageViewCreateInfo.subresourceRange.levelCount = 1;
        imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        data->swapChainImageViews = SDL_calloc(sizeof(VkImageView), data->swapchainImageCount);
        data->swapChainImageLayouts = SDL_calloc(sizeof(VkImageLayout), data->swapchainImageCount);
        for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
            imageViewCreateInfo.image = data->swapchainImages[i];
            result = vkCreateImageView(data->device, &imageViewCreateInfo, NULL, &data->swapChainImageViews[i]);
            if (result != VK_SUCCESS) {
                VULKAN_DestroyAll(renderer);
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateImageView(): %s\n", SDL_Vulkan_GetResultString(result));
                return result;
            }
            data->swapChainImageLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        
    }

    VkCommandBufferAllocateInfo commandBufferAllocateInfo = { 0 };
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = data->commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = data->swapchainImageCount;
    data->commandBuffers = SDL_calloc(sizeof(VkCommandBuffer), data->swapchainImageCount);
    result = vkAllocateCommandBuffers(data->device, &commandBufferAllocateInfo, data->commandBuffers);
    if (result != VK_SUCCESS) {
        VULKAN_DestroyAll(renderer);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkAllocateCommandBuffers(): %s\n", SDL_Vulkan_GetResultString(result));
        return result;
    }

    /* Create fences */
    data->fences = SDL_calloc(sizeof(VkFence), data->swapchainImageCount);
    for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
        VkFenceCreateInfo fenceCreateInfo = { 0 };
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(data->device, &fenceCreateInfo, NULL, &data->fences[i]);
        if (result != VK_SUCCESS) {
            VULKAN_DestroyAll(renderer);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateFence(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }
    }

    /* Create renderpasses and framebuffer */
    {
        VkAttachmentDescription attachmentDescription = { 0 };
        attachmentDescription.format = data->surfaceFormat.format;
        attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachmentDescription.samples = 1;
        attachmentDescription.flags = 0;

        VkAttachmentReference colorAttachmentReference = { 0 };
        colorAttachmentReference.attachment = 0;
        colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpassDescription = { 0 };
        subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDescription.flags = 0;
        subpassDescription.inputAttachmentCount = 0;
        subpassDescription.pInputAttachments = NULL;
        subpassDescription.colorAttachmentCount = 1;
        subpassDescription.pColorAttachments = &colorAttachmentReference;
        subpassDescription.pResolveAttachments = NULL;
        subpassDescription.pDepthStencilAttachment = NULL;
        subpassDescription.preserveAttachmentCount = 0;
        subpassDescription.pPreserveAttachments = NULL;

        VkSubpassDependency subPassDependency = { 0 };
        subPassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        subPassDependency.dstSubpass = 0;
        subPassDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subPassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        subPassDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        subPassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        subPassDependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassCreateInfo = { 0 };
        renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassCreateInfo.flags = 0;
        renderPassCreateInfo.attachmentCount = 1;
        renderPassCreateInfo.pAttachments = &attachmentDescription;
        renderPassCreateInfo.subpassCount = 1;
        renderPassCreateInfo.pSubpasses = &subpassDescription;
        renderPassCreateInfo.dependencyCount = 1;
        renderPassCreateInfo.pDependencies = &subPassDependency;

        result = vkCreateRenderPass(data->device, &renderPassCreateInfo, NULL, &data->renderPasses[SDL_VULKAN_RENDERPASS_LOAD]);
        if (result != VK_SUCCESS) {
            VULKAN_DestroyAll(renderer);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateRenderPass(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }

        attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        result = vkCreateRenderPass(data->device, &renderPassCreateInfo, NULL, &data->renderPasses[SDL_VULKAN_RENDERPASS_CLEAR]);
        if (result != VK_SUCCESS) {
            VULKAN_DestroyAll(renderer);
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateRenderPass(): %s\n", SDL_Vulkan_GetResultString(result));
            return result;
        }

        VkFramebufferCreateInfo framebufferCreateInfo = { 0 };
        framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferCreateInfo.pNext = NULL;
        framebufferCreateInfo.renderPass = data->renderPasses[SDL_VULKAN_RENDERPASS_LOAD];
        framebufferCreateInfo.attachmentCount = 1;
        framebufferCreateInfo.width = data->swapchainSize.width;
        framebufferCreateInfo.height = data->swapchainSize.height;
        framebufferCreateInfo.layers = 1;

        data->framebuffers = SDL_calloc(sizeof(VkFramebuffer), data->swapchainImageCount);
        for (uint32_t i = 0; i < data->swapchainImageCount; i++) {
            framebufferCreateInfo.pAttachments = &data->swapChainImageViews[i];
            result = vkCreateFramebuffer(data->device, &framebufferCreateInfo, NULL, &data->framebuffers[i]);
            if (result != VK_SUCCESS) {
                VULKAN_DestroyAll(renderer);
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "vkCreateFramebuffer(): %s\n", SDL_Vulkan_GetResultString(result));
                return result;
            }
        }
        
    }

    
#if D3D12_PORT
    

    /* Create a swap chain using the same adapter as the existing Direct3D device. */
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    SDL_zero(swapChainDesc);
    swapChainDesc.Width = w;
    swapChainDesc.Height = h;
    switch (renderer->output_colorspace) {
    case SDL_COLORSPACE_SCRGB:
        swapChainDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        data->renderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case SDL_COLORSPACE_HDR10:
        swapChainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        data->renderTargetFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
        break;
    default:
        swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; /* This is the most common swap chain format. */
        if (renderer->colorspace_conversion && renderer->output_colorspace == SDL_COLORSPACE_SRGB) {
            data->renderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        } else {
            data->renderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        break;
    }
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1; /* Don't use multi-sampling. */
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2; /* Use double-buffering to minimize latency. */
    if (WIN_IsWindows8OrGreater()) {
        swapChainDesc.Scaling = DXGI_SCALING_NONE;
    } else {
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    }
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;               /* All Windows Store apps must use this SwapEffect. */
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | /* To support SetMaximumFrameLatency */
                          DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;                  /* To support presenting with allow tearing on */

    HWND hwnd = (HWND)SDL_GetProperty(SDL_GetWindowProperties(renderer->window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    result = D3D_CALL(data->dxgiFactory, CreateSwapChainForHwnd,
                      (IUnknown *)data->commandQueue,
                      hwnd,
                      &swapChainDesc,
                      NULL,
                      NULL, /* Allow on all displays. */
                      &swapChain);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGIFactory2::CreateSwapChainForHwnd"), result);
        goto done;
    }

    D3D_CALL(data->dxgiFactory, MakeWindowAssociation, hwnd, DXGI_MWA_NO_WINDOW_CHANGES);

    result = D3D_CALL(swapChain, QueryInterface, D3D_GUID(SDL_IID_IDXGISwapChain4), (void **)&data->swapChain);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain1::QueryInterface"), result);
        goto done;
    }

    /* Ensure that the swapchain does not queue more than one frame at a time. This both reduces latency
     * and ensures that the application will only render after each VSync, minimizing power consumption.
     */
    result = D3D_CALL(data->swapChain, SetMaximumFrameLatency, 1);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain4::SetMaximumFrameLatency"), result);
        goto done;
    }

    data->swapEffect = swapChainDesc.SwapEffect;
    data->swapFlags = swapChainDesc.Flags;

    DXGI_COLOR_SPACE_TYPE ColorSpace;
    switch (renderer->output_colorspace) {
    case SDL_COLORSPACE_SCRGB:
        ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        break;
    case SDL_COLORSPACE_HDR10:
        ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        break;
    default:
        /* sRGB */
        ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        break;
    }
    result = D3D_CALL(data->swapChain, SetColorSpace1, ColorSpace);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain3::SetColorSpace1"), result);
        goto done;
    }

done:
    SAFE_RELEASE(swapChain);
    return result;
#else
    return VK_SUCCESS;
#endif
}

VkResult VULKAN_HandleDeviceLost(SDL_Renderer *renderer)
{
#if D3D12_PORT
    HRESULT result = S_OK;

    VULKAN_ReleaseAll(renderer);

    result = VULKAN_CreateDeviceResources(renderer);
    if (FAILED(result)) {
        /* VULKAN_CreateDeviceResources will set the SDL error */
        return result;
    }

    result = VULKAN_UpdateForWindowSizeChange(renderer);
    if (FAILED(result)) {
        /* VULKAN_UpdateForWindowSizeChange will set the SDL error */
        return result;
    }

    /* Let the application know that the device has been reset */
    {
        SDL_Event event;
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        event.common.timestamp = 0;
        SDL_PushEvent(&event);
    }

    return S_OK;
#else
    return VK_SUCCESS;
#endif
}

/* Initialize all resources that change when the window's size changes. */
static VkResult VULKAN_CreateWindowSizeDependentResources(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VkResult result = VK_SUCCESS;
    int i, w, h;

#if D3D12_PORT
    /* Release resources in the current command list */
    VULKAN_IssueBatch(data);
    D3D_CALL(data->commandList, OMSetRenderTargets, 0, NULL, FALSE, NULL);

    /* Release render targets */
    for (i = 0; i < SDL_VULKAN_NUM_BUFFERS; ++i) {
        SAFE_RELEASE(data->renderTargets[i]);
    }
#endif

    /* The width and height of the swap chain must be based on the display's
     * non-rotated size.
     */
    SDL_GetWindowSizeInPixels(renderer->window, &w, &h);

#if D3D12_PORT
    data->rotation = VULKAN_GetCurrentRotation();
    if (VULKAN_IsDisplayRotated90Degrees(data->rotation)) {
        int tmp = w;
        w = h;
        h = tmp;
    }
#endif

    result = VULKAN_CreateSwapChain(renderer, w, h);
    
#if D3D12_PORT
    /* Set the proper rotation for the swap chain. */
    if (WIN_IsWindows8OrGreater()) {
        if (data->swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) {
            result = D3D_CALL(data->swapChain, SetRotation, data->rotation); /* NOLINT(clang-analyzer-core.NullDereference) */
            if (FAILED(result)) {
                WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain4::SetRotation"), result);
                goto done;
            }
        }
    }

    /* Get each back buffer render target and create render target views */
    for (i = 0; i < SDL_VULKAN_NUM_BUFFERS; ++i) {
#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
        result = VULKAN_XBOX_CreateBackBufferTarget(data->d3dDevice, renderer->window->w, renderer->window->h, (void **)&data->renderTargets[i]);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("VULKAN_XBOX_CreateBackBufferTarget"), result);
            goto done;
        }
#else
        result = D3D_CALL(data->swapChain, GetBuffer, /* NOLINT(clang-analyzer-core.NullDereference) */
                          i,
                          D3D_GUID(SDL_IID_IVULKANResource),
                          (void **)&data->renderTargets[i]);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain4::GetBuffer"), result);
            goto done;
        }
#endif

        SDL_zero(rtvDesc);
        rtvDesc.Format = data->renderTargetFormat;
        rtvDesc.ViewDimension = VULKAN_RTV_DIMENSION_TEXTURE2D;

        SDL_zero(rtvDescriptor);
        D3D_CALL_RET(data->rtvDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &rtvDescriptor);
        rtvDescriptor.ptr += i * data->rtvDescriptorSize;
        D3D_CALL(data->d3dDevice, CreateRenderTargetView, data->renderTargets[i], &rtvDesc, rtvDescriptor);
    }

    /* Set back buffer index to current buffer */
#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
    data->currentBackBufferIndex = 0;
#else
    data->currentBackBufferIndex = D3D_CALL(data->swapChain, GetCurrentBackBufferIndex);
#endif

    /* Set the swap chain target immediately, so that a target is always set
     * even before we get to SetDrawState. Without this it's possible to hit
     * null references in places like ReadPixels!
     */
    data->currentRenderTargetView = VULKAN_GetCurrentRenderTargetView(renderer);
    D3D_CALL(data->commandList, OMSetRenderTargets, 1, &data->currentRenderTargetView, FALSE, NULL);
    VULKAN_TransitionResource(data,
                             data->renderTargets[data->currentBackBufferIndex],
                             VULKAN_RESOURCE_STATE_PRESENT,
                             VULKAN_RESOURCE_STATE_RENDER_TARGET);

    data->viewportDirty = SDL_TRUE;

#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
    VULKAN_XBOX_StartFrame(data->d3dDevice, &data->frameToken);
#endif

done:
    return result;
#else
    return VK_SUCCESS;
#endif
}

/* This method is called when the window's size changes. */
static VkResult VULKAN_UpdateForWindowSizeChange(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    /* If the GPU has previous work, wait for it to be done first */
    VULKAN_WaitForGPU(data);
    return VULKAN_CreateWindowSizeDependentResources(renderer);
}

static void VULKAN_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;

#if D3D12_PORT
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        data->pixelSizeChanged = SDL_TRUE;
    }
#endif
}

static SDL_bool VULKAN_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
#if D3D12_PORT

    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    if (!GetBlendFunc(srcColorFactor) || !GetBlendFunc(srcAlphaFactor) ||
        !GetBlendEquation(colorOperation) ||
        !GetBlendFunc(dstColorFactor) || !GetBlendFunc(dstAlphaFactor) ||
        !GetBlendEquation(alphaOperation)) {
        return SDL_FALSE;
    }
#endif
    return SDL_TRUE;
}

static int VULKAN_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData;
    HRESULT result;
    DXGI_FORMAT textureFormat = SDLPixelFormatToDXGITextureFormat(texture->format,  texture->colorspace, renderer->colorspace_conversion);
    VULKAN_RESOURCE_DESC textureDesc;
    VULKAN_HEAP_PROPERTIES heapProps;
    VULKAN_SHADER_RESOURCE_VIEW_DESC resourceViewDesc;

    if (textureFormat == DXGI_FORMAT_UNKNOWN) {
        return SDL_SetError("%s, An unsupported SDL pixel format (0x%x) was specified", __FUNCTION__, texture->format);
    }

    textureData = (VULKAN_TextureData *)SDL_calloc(1, sizeof(*textureData));
    if (!textureData) {
        return -1;
    }
    textureData->scaleMode = (texture->scaleMode == SDL_SCALEMODE_NEAREST) ? VULKAN_FILTER_MIN_MAG_MIP_POINT : VULKAN_FILTER_MIN_MAG_MIP_LINEAR;

    texture->driverdata = textureData;
    textureData->mainTextureFormat = textureFormat;

    SDL_zero(textureDesc);
    textureDesc.Width = texture->w;
    textureDesc.Height = texture->h;
    textureDesc.MipLevels = 1;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.Format = textureFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = VULKAN_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Flags = VULKAN_RESOURCE_FLAG_NONE;

    if (texture->access == SDL_TEXTUREACCESS_TARGET) {
        textureDesc.Flags = VULKAN_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    SDL_zero(heapProps);
    heapProps.Type = VULKAN_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    if (GetTextureProperty(create_props, "d3d12.texture", &textureData->mainTexture) < 0) {
        return -1;
    }
    if (!textureData->mainTexture) {
        result = D3D_CALL(rendererData->d3dDevice, CreateCommittedResource,
                          &heapProps,
                          VULKAN_HEAP_FLAG_NONE,
                          &textureDesc,
                          VULKAN_RESOURCE_STATE_COPY_DEST,
                          NULL,
                          D3D_GUID(SDL_IID_IVULKANResource),
                          (void **)&textureData->mainTexture);
        if (FAILED(result)) {
            VULKAN_DestroyTexture(renderer, texture);
            return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommittedResource [texture]"), result);
        }
    }
    textureData->mainResourceState = VULKAN_RESOURCE_STATE_COPY_DEST;
    SDL_SetProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_VULKAN_TEXTURE_POINTER, textureData->mainTexture);
#if SDL_HAVE_YUV
    if (texture->format == SDL_PIXELFORMAT_YV12 ||
        texture->format == SDL_PIXELFORMAT_IYUV) {
        textureData->yuv = SDL_TRUE;

        textureDesc.Width = (textureDesc.Width + 1) / 2;
        textureDesc.Height = (textureDesc.Height + 1) / 2;

        if (GetTextureProperty(create_props, "d3d12.texture_u", &textureData->mainTextureU) < 0) {
            return -1;
        }
        if (!textureData->mainTextureU) {
            result = D3D_CALL(rendererData->d3dDevice, CreateCommittedResource,
                              &heapProps,
                              VULKAN_HEAP_FLAG_NONE,
                              &textureDesc,
                              VULKAN_RESOURCE_STATE_COPY_DEST,
                              NULL,
                              D3D_GUID(SDL_IID_IVULKANResource),
                              (void **)&textureData->mainTextureU);
            if (FAILED(result)) {
                VULKAN_DestroyTexture(renderer, texture);
                return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommittedResource [texture]"), result);
            }
        }
        textureData->mainResourceStateU = VULKAN_RESOURCE_STATE_COPY_DEST;
        SDL_SetProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_VULKAN_TEXTURE_U_POINTER, textureData->mainTextureU);

        if (GetTextureProperty(create_props, "d3d12.texture_v", &textureData->mainTextureV) < 0) {
            return -1;
        }
        if (!textureData->mainTextureV) {
            result = D3D_CALL(rendererData->d3dDevice, CreateCommittedResource,
                              &heapProps,
                              VULKAN_HEAP_FLAG_NONE,
                              &textureDesc,
                              VULKAN_RESOURCE_STATE_COPY_DEST,
                              NULL,
                              D3D_GUID(SDL_IID_IVULKANResource),
                              (void **)&textureData->mainTextureV);
            if (FAILED(result)) {
                VULKAN_DestroyTexture(renderer, texture);
                return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommittedResource [texture]"), result);
            }
        }
        textureData->mainResourceStateV = VULKAN_RESOURCE_STATE_COPY_DEST;
        SDL_SetProperty(SDL_GetTextureProperties(texture), SDL_PROP_TEXTURE_VULKAN_TEXTURE_V_POINTER, textureData->mainTextureV);
    }

    if (texture->format == SDL_PIXELFORMAT_NV12 ||
        texture->format == SDL_PIXELFORMAT_NV21) {
        textureData->nv12 = SDL_TRUE;
    }
#endif /* SDL_HAVE_YUV */
    SDL_zero(resourceViewDesc);
    resourceViewDesc.Shader4ComponentMapping = VULKAN_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    resourceViewDesc.Format = SDLPixelFormatToDXGIMainResourceViewFormat(texture->format, texture->colorspace, renderer->colorspace_conversion);
    resourceViewDesc.ViewDimension = VULKAN_SRV_DIMENSION_TEXTURE2D;
    resourceViewDesc.Texture2D.MipLevels = textureDesc.MipLevels;

    textureData->mainSRVIndex = VULKAN_GetAvailableSRVIndex(renderer);
    D3D_CALL_RET(rendererData->srvDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &textureData->mainTextureResourceView);
    textureData->mainTextureResourceView.ptr += textureData->mainSRVIndex * rendererData->srvDescriptorSize;

    D3D_CALL(rendererData->d3dDevice, CreateShaderResourceView,
             textureData->mainTexture,
             &resourceViewDesc,
             textureData->mainTextureResourceView);
#if SDL_HAVE_YUV
    if (textureData->yuv) {
        D3D_CALL_RET(rendererData->srvDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &textureData->mainTextureResourceViewU);
        textureData->mainSRVIndexU = VULKAN_GetAvailableSRVIndex(renderer);
        textureData->mainTextureResourceViewU.ptr += textureData->mainSRVIndexU * rendererData->srvDescriptorSize;
        D3D_CALL(rendererData->d3dDevice, CreateShaderResourceView,
                 textureData->mainTextureU,
                 &resourceViewDesc,
                 textureData->mainTextureResourceViewU);

        D3D_CALL_RET(rendererData->srvDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &textureData->mainTextureResourceViewV);
        textureData->mainSRVIndexV = VULKAN_GetAvailableSRVIndex(renderer);
        textureData->mainTextureResourceViewV.ptr += textureData->mainSRVIndexV * rendererData->srvDescriptorSize;
        D3D_CALL(rendererData->d3dDevice, CreateShaderResourceView,
                 textureData->mainTextureV,
                 &resourceViewDesc,
                 textureData->mainTextureResourceViewV);
    }

    if (textureData->nv12) {
        VULKAN_SHADER_RESOURCE_VIEW_DESC nvResourceViewDesc = resourceViewDesc;

        nvResourceViewDesc.Format = DXGI_FORMAT_R8G8_UNORM;
        nvResourceViewDesc.Texture2D.PlaneSlice = 1;

        D3D_CALL_RET(rendererData->srvDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &textureData->mainTextureResourceViewNV);
        textureData->mainSRVIndexNV = VULKAN_GetAvailableSRVIndex(renderer);
        textureData->mainTextureResourceViewNV.ptr += textureData->mainSRVIndexNV * rendererData->srvDescriptorSize;
        D3D_CALL(rendererData->d3dDevice, CreateShaderResourceView,
                 textureData->mainTexture,
                 &nvResourceViewDesc,
                 textureData->mainTextureResourceViewNV);
    }
#endif /* SDL_HAVE_YUV */

    if (texture->access & SDL_TEXTUREACCESS_TARGET) {
        VULKAN_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
        SDL_zero(renderTargetViewDesc);
        renderTargetViewDesc.Format = textureDesc.Format;
        renderTargetViewDesc.ViewDimension = VULKAN_RTV_DIMENSION_TEXTURE2D;
        renderTargetViewDesc.Texture2D.MipSlice = 0;

        D3D_CALL_RET(rendererData->textureRTVDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &textureData->mainTextureRenderTargetView);
        textureData->mainTextureRenderTargetView.ptr += textureData->mainSRVIndex * rendererData->rtvDescriptorSize;

        D3D_CALL(rendererData->d3dDevice, CreateRenderTargetView,
                 (IVULKANResource *)textureData->mainTexture,
                 &renderTargetViewDesc,
                 textureData->mainTextureRenderTargetView);
    }
#endif

    return 0;
}

static void VULKAN_DestroyTexture(SDL_Renderer *renderer,
                                 SDL_Texture *texture)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData) {
        return;
    }

    /* Because SDL_DestroyTexture might be called while the data is in-flight, we need to issue the batch first
       Unfortunately, this means that deleting a lot of textures mid-frame will have poor performance. */
    VULKAN_IssueBatch(rendererData);

    SAFE_RELEASE(textureData->mainTexture);
    SAFE_RELEASE(textureData->stagingBuffer);
    VULKAN_FreeSRVIndex(renderer, textureData->mainSRVIndex);
#if SDL_HAVE_YUV
    SAFE_RELEASE(textureData->mainTextureU);
    SAFE_RELEASE(textureData->mainTextureV);
    if (textureData->yuv) {
        VULKAN_FreeSRVIndex(renderer, textureData->mainSRVIndexU);
        VULKAN_FreeSRVIndex(renderer, textureData->mainSRVIndexV);
    }
    if (textureData->nv12) {
        VULKAN_FreeSRVIndex(renderer, textureData->mainSRVIndexNV);
    }
    SDL_free(textureData->pixels);
#endif
    SDL_free(textureData);
    texture->driverdata = NULL;
#endif
}

#if D3D12_PORT
static int VULKAN_UpdateTextureInternal(VULKAN_RenderData *rendererData, IVULKANResource *texture, int plane, int x, int y, int w, int h, const void *pixels, int pitch, VULKAN_RESOURCE_STATES *resourceState)
{
    const Uint8 *src;
    Uint8 *dst;
    UINT length;
    HRESULT result;
    VULKAN_RESOURCE_DESC textureDesc;
    VULKAN_RESOURCE_DESC uploadDesc;
    VULKAN_HEAP_PROPERTIES heapProps;
    VULKAN_PLACED_SUBRESOURCE_FOOTPRINT placedTextureDesc;
    VULKAN_TEXTURE_COPY_LOCATION srcLocation;
    VULKAN_TEXTURE_COPY_LOCATION dstLocation;
    BYTE *textureMemory;
    IVULKANResource *uploadBuffer;
    UINT row, NumRows, RowPitch;
    UINT64 RowLength;

    /* Create an upload buffer, which will be used to write to the main texture. */
    SDL_zero(textureDesc);
    D3D_CALL_RET(texture, GetDesc, &textureDesc);
    textureDesc.Width = w;
    textureDesc.Height = h;

    SDL_zero(uploadDesc);
    uploadDesc.Dimension = VULKAN_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment = VULKAN_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout = VULKAN_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = VULKAN_RESOURCE_FLAG_NONE;

    /* Figure out how much we need to allocate for the upload buffer */
    D3D_CALL(rendererData->d3dDevice, GetCopyableFootprints,
             &textureDesc,
             plane,
             1,
             0,
             &placedTextureDesc,
             &NumRows,
             &RowLength,
             &uploadDesc.Width);
    RowPitch = placedTextureDesc.Footprint.RowPitch;

    SDL_zero(heapProps);
    heapProps.Type = VULKAN_HEAP_TYPE_UPLOAD;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    /* Create the upload buffer */
    result = D3D_CALL(rendererData->d3dDevice, CreateCommittedResource,
                      &heapProps,
                      VULKAN_HEAP_FLAG_NONE,
                      &uploadDesc,
                      VULKAN_RESOURCE_STATE_GENERIC_READ,
                      NULL,
                      D3D_GUID(SDL_IID_IVULKANResource),
                      (void **)&rendererData->uploadBuffers[rendererData->currentUploadBuffer]);
    if (FAILED(result)) {
        return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommittedResource [create upload buffer]"), result);
    }

    /* Get a write-only pointer to data in the upload buffer: */
    uploadBuffer = rendererData->uploadBuffers[rendererData->currentUploadBuffer];
    result = D3D_CALL(uploadBuffer, Map,
                      0,
                      NULL,
                      (void **)&textureMemory);
    if (FAILED(result)) {
        SAFE_RELEASE(rendererData->uploadBuffers[rendererData->currentUploadBuffer]);
        return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANResource::Map [map staging texture]"), result);
    }

    src = (const Uint8 *)pixels;
    dst = textureMemory;
    length = (UINT)RowLength;
    if (length == (UINT)pitch && length == RowPitch) {
        SDL_memcpy(dst, src, (size_t)length * NumRows);
    } else {
        if (length > (UINT)pitch) {
            length = pitch;
        }
        if (length > RowPitch) {
            length = RowPitch;
        }
        for (row = NumRows; row--; ) {
            SDL_memcpy(dst, src, length);
            src += pitch;
            dst += RowPitch;
        }
    }

    /* Commit the changes back to the upload buffer: */
    D3D_CALL(uploadBuffer, Unmap, 0, NULL);

    /* Make sure the destination is in the correct resource state */
    VULKAN_TransitionResource(rendererData, texture, *resourceState, VULKAN_RESOURCE_STATE_COPY_DEST);
    *resourceState = VULKAN_RESOURCE_STATE_COPY_DEST;

    SDL_zero(dstLocation);
    dstLocation.pResource = texture;
    dstLocation.Type = VULKAN_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = plane;

    SDL_zero(srcLocation);
    srcLocation.pResource = rendererData->uploadBuffers[rendererData->currentUploadBuffer];
    srcLocation.Type = VULKAN_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = placedTextureDesc;

    D3D_CALL(rendererData->commandList, CopyTextureRegion,
             &dstLocation,
             x,
             y,
             0,
             &srcLocation,
             NULL);

    /* Transition the texture to be shader accessible */
    VULKAN_TransitionResource(rendererData, texture, *resourceState, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    *resourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    rendererData->currentUploadBuffer++;
    /* If we've used up all the upload buffers, we need to issue the batch */
    if (rendererData->currentUploadBuffer == SDL_VULKAN_NUM_UPLOAD_BUFFERS) {
        VULKAN_IssueBatch(rendererData);
    }

    return 0;
}
#endif

static int VULKAN_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_Rect *rect, const void *srcPixels,
                               int srcPitch)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData) {
        return SDL_SetError("Texture is not currently available");
    }

    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTexture, 0, rect->x, rect->y, rect->w, rect->h, srcPixels, srcPitch, &textureData->mainResourceState) < 0) {
        return -1;
    }
#if SDL_HAVE_YUV
    if (textureData->yuv) {
        /* Skip to the correct offset into the next texture */
        srcPixels = (const void *)((const Uint8 *)srcPixels + rect->h * srcPitch);

        if (VULKAN_UpdateTextureInternal(rendererData, texture->format == SDL_PIXELFORMAT_YV12 ? textureData->mainTextureV : textureData->mainTextureU, 0, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, srcPixels, (srcPitch + 1) / 2, texture->format == SDL_PIXELFORMAT_YV12 ? &textureData->mainResourceStateV : &textureData->mainResourceStateU) < 0) {
            return -1;
        }

        /* Skip to the correct offset into the next texture */
        srcPixels = (const void *)((const Uint8 *)srcPixels + ((rect->h + 1) / 2) * ((srcPitch + 1) / 2));
        if (VULKAN_UpdateTextureInternal(rendererData, texture->format == SDL_PIXELFORMAT_YV12 ? textureData->mainTextureU : textureData->mainTextureV, 0, rect->x / 2, rect->y / 2, (rect->w + 1) / 2, (rect->h + 1) / 2, srcPixels, (srcPitch + 1) / 2, texture->format == SDL_PIXELFORMAT_YV12 ? &textureData->mainResourceStateU : &textureData->mainResourceStateV) < 0) {
            return -1;
        }
    }

    if (textureData->nv12) {
        /* Skip to the correct offset into the next texture */
        srcPixels = (const void *)((const Uint8 *)srcPixels + rect->h * srcPitch);

        if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTexture, 1, rect->x, rect->y, rect->w, rect->h, srcPixels, srcPitch, &textureData->mainResourceState) < 0) {
            return -1;
        }
    }
#endif /* SDL_HAVE_YUV */
#endif
    return 0;
}

#if SDL_HAVE_YUV
static int VULKAN_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture,
                                  const SDL_Rect *rect,
                                  const Uint8 *Yplane, int Ypitch,
                                  const Uint8 *Uplane, int Upitch,
                                  const Uint8 *Vplane, int Vpitch)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData) {
        return SDL_SetError("Texture is not currently available");
    }

    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTexture, 0, rect->x, rect->y, rect->w, rect->h, Yplane, Ypitch, &textureData->mainResourceState) < 0) {
        return -1;
    }
    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTextureU, 0, rect->x / 2, rect->y / 2, rect->w / 2, rect->h / 2, Uplane, Upitch, &textureData->mainResourceStateU) < 0) {
        return -1;
    }
    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTextureV, 0, rect->x / 2, rect->y / 2, rect->w / 2, rect->h / 2, Vplane, Vpitch, &textureData->mainResourceStateV) < 0) {
        return -1;
    }
#endif
    return 0;
}

static int VULKAN_UpdateTextureNV(SDL_Renderer *renderer, SDL_Texture *texture,
                                 const SDL_Rect *rect,
                                 const Uint8 *Yplane, int Ypitch,
                                 const Uint8 *UVplane, int UVpitch)
{
#if D3D12_PORT

    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData) {
        return SDL_SetError("Texture is not currently available");
    }

    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTexture, 0, rect->x, rect->y, rect->w, rect->h, Yplane, Ypitch, &textureData->mainResourceState) < 0) {
        return -1;
    }

    if (VULKAN_UpdateTextureInternal(rendererData, textureData->mainTexture, 1, rect->x, rect->y, rect->w, rect->h, UVplane, UVpitch, &textureData->mainResourceState) < 0) {
        return -1;
    }
#endif
    return 0;

}
#endif

static int VULKAN_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                             const SDL_Rect *rect, void **pixels, int *pitch)
{
#if D3D12_PORT

    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;
    HRESULT result = S_OK;

    VULKAN_RESOURCE_DESC textureDesc;
    VULKAN_RESOURCE_DESC uploadDesc;
    VULKAN_HEAP_PROPERTIES heapProps;
    VULKAN_SUBRESOURCE_FOOTPRINT pitchedDesc;
    BYTE *textureMemory;
    int bpp;

    if (!textureData) {
        return SDL_SetError("Texture is not currently available");
    }
#if SDL_HAVE_YUV
    if (textureData->yuv || textureData->nv12) {
        /* It's more efficient to upload directly... */
        if (!textureData->pixels) {
            textureData->pitch = texture->w;
            textureData->pixels = (Uint8 *)SDL_malloc((texture->h * textureData->pitch * 3) / 2);
            if (!textureData->pixels) {
                return -1;
            }
        }
        textureData->lockedRect = *rect;
        *pixels =
            (void *)(textureData->pixels + rect->y * textureData->pitch +
                     rect->x * SDL_BYTESPERPIXEL(texture->format));
        *pitch = textureData->pitch;
        return 0;
    }
#endif
    if (textureData->stagingBuffer) {
        return SDL_SetError("texture is already locked");
    }

    /* Create an upload buffer, which will be used to write to the main texture. */
    SDL_zero(textureDesc);
    D3D_CALL_RET(textureData->mainTexture, GetDesc, &textureDesc);
    textureDesc.Width = rect->w;
    textureDesc.Height = rect->h;

    SDL_zero(uploadDesc);
    uploadDesc.Dimension = VULKAN_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Alignment = VULKAN_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.SampleDesc.Quality = 0;
    uploadDesc.Layout = VULKAN_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = VULKAN_RESOURCE_FLAG_NONE;

    /* Figure out how much we need to allocate for the upload buffer */
    D3D_CALL(rendererData->d3dDevice, GetCopyableFootprints,
             &textureDesc,
             0,
             1,
             0,
             NULL,
             NULL,
             NULL,
             &uploadDesc.Width);

    SDL_zero(heapProps);
    heapProps.Type = VULKAN_HEAP_TYPE_UPLOAD;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    /* Create the upload buffer */
    result = D3D_CALL(rendererData->d3dDevice, CreateCommittedResource,
                      &heapProps,
                      VULKAN_HEAP_FLAG_NONE,
                      &uploadDesc,
                      VULKAN_RESOURCE_STATE_GENERIC_READ,
                      NULL,
                      D3D_GUID(SDL_IID_IVULKANResource),
                      (void **)&textureData->stagingBuffer);
    if (FAILED(result)) {
        return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommittedResource [create upload buffer]"), result);
    }

    /* Get a write-only pointer to data in the upload buffer: */
    result = D3D_CALL(textureData->stagingBuffer, Map,
                      0,
                      NULL,
                      (void **)&textureMemory);
    if (FAILED(result)) {
        SAFE_RELEASE(rendererData->uploadBuffers[rendererData->currentUploadBuffer]);
        return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANResource::Map [map staging texture]"), result);
    }

    SDL_zero(pitchedDesc);
    pitchedDesc.Format = textureDesc.Format;
    pitchedDesc.Width = rect->w;
    pitchedDesc.Height = rect->h;
    pitchedDesc.Depth = 1;
    if (pitchedDesc.Format == DXGI_FORMAT_R8_UNORM) {
        bpp = 1;
    } else {
        bpp = 4;
    }
    pitchedDesc.RowPitch = VULKAN_Align(rect->w * bpp, VULKAN_TEXTURE_DATA_PITCH_ALIGNMENT);

    /* Make note of where the staging texture will be written to
     * (on a call to SDL_UnlockTexture):
     */
    textureData->lockedRect = *rect;

    /* Make sure the caller has information on the texture's pixel buffer,
     * then return:
     */
    *pixels = textureMemory;
    *pitch = pitchedDesc.RowPitch;
#endif
    return 0;

}

static void VULKAN_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    VULKAN_RESOURCE_DESC textureDesc;
    VULKAN_SUBRESOURCE_FOOTPRINT pitchedDesc;
    VULKAN_PLACED_SUBRESOURCE_FOOTPRINT placedTextureDesc;
    VULKAN_TEXTURE_COPY_LOCATION srcLocation;
    VULKAN_TEXTURE_COPY_LOCATION dstLocation;
    int bpp;

    if (!textureData) {
        return;
    }
#if SDL_HAVE_YUV
    if (textureData->yuv || textureData->nv12) {
        const SDL_Rect *rect = &textureData->lockedRect;
        void *pixels =
            (void *)(textureData->pixels + rect->y * textureData->pitch +
                     rect->x * SDL_BYTESPERPIXEL(texture->format));
        VULKAN_UpdateTexture(renderer, texture, rect, pixels, textureData->pitch);
        return;
    }
#endif
    /* Commit the pixel buffer's changes back to the staging texture: */
    D3D_CALL(textureData->stagingBuffer, Unmap, 0, NULL);

    SDL_zero(textureDesc);
    D3D_CALL_RET(textureData->mainTexture, GetDesc, &textureDesc);
    textureDesc.Width = textureData->lockedRect.w;
    textureDesc.Height = textureData->lockedRect.h;

    SDL_zero(pitchedDesc);
    pitchedDesc.Format = textureDesc.Format;
    pitchedDesc.Width = (UINT)textureDesc.Width;
    pitchedDesc.Height = textureDesc.Height;
    pitchedDesc.Depth = 1;
    if (pitchedDesc.Format == DXGI_FORMAT_R8_UNORM) {
        bpp = 1;
    } else {
        bpp = 4;
    }
    pitchedDesc.RowPitch = VULKAN_Align(textureData->lockedRect.w * bpp, VULKAN_TEXTURE_DATA_PITCH_ALIGNMENT);

    SDL_zero(placedTextureDesc);
    placedTextureDesc.Offset = 0;
    placedTextureDesc.Footprint = pitchedDesc;

    VULKAN_TransitionResource(rendererData, textureData->mainTexture, textureData->mainResourceState, VULKAN_RESOURCE_STATE_COPY_DEST);
    textureData->mainResourceState = VULKAN_RESOURCE_STATE_COPY_DEST;

    SDL_zero(dstLocation);
    dstLocation.pResource = textureData->mainTexture;
    dstLocation.Type = VULKAN_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    SDL_zero(srcLocation);
    srcLocation.pResource = textureData->stagingBuffer;
    srcLocation.Type = VULKAN_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = placedTextureDesc;

    D3D_CALL(rendererData->commandList, CopyTextureRegion,
             &dstLocation,
             textureData->lockedRect.x,
             textureData->lockedRect.y,
             0,
             &srcLocation,
             NULL);

    /* Transition the texture to be shader accessible */
    VULKAN_TransitionResource(rendererData, textureData->mainTexture, textureData->mainResourceState, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    textureData->mainResourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    /* Execute the command list before releasing the staging buffer */
    VULKAN_IssueBatch(rendererData);
    SAFE_RELEASE(textureData->stagingBuffer);
#endif
}

static void VULKAN_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
#if D3D12_PORT
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData) {
        return;
    }

    textureData->scaleMode = (scaleMode == SDL_SCALEMODE_NEAREST) ? VULKAN_FILTER_MIN_MAG_MIP_POINT : VULKAN_FILTER_MIN_MAG_MIP_LINEAR;
#endif
}

static int VULKAN_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = NULL;

    if (!texture) {
        if (rendererData->textureRenderTarget) {
            VULKAN_TransitionResource(rendererData,
                                     rendererData->textureRenderTarget->mainTexture,
                                     rendererData->textureRenderTarget->mainResourceState,
                                     VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            rendererData->textureRenderTarget->mainResourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        rendererData->textureRenderTarget = NULL;
        return 0;
    }

    textureData = (VULKAN_TextureData *)texture->driverdata;

    if (!textureData->mainTextureRenderTargetView.ptr) {
        return SDL_SetError("specified texture is not a render target");
    }

    rendererData->textureRenderTarget = textureData;
    VULKAN_TransitionResource(rendererData,
                             rendererData->textureRenderTarget->mainTexture,
                             rendererData->textureRenderTarget->mainResourceState,
                             VULKAN_RESOURCE_STATE_RENDER_TARGET);
    rendererData->textureRenderTarget->mainResourceState = VULKAN_RESOURCE_STATE_RENDER_TARGET;

#endif
    return 0;
}

static int VULKAN_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return 0; /* nothing to do in this backend. */
}

static int VULKAN_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    VertexPositionColor *verts = (VertexPositionColor *)SDL_AllocateRenderVertices(renderer, count * sizeof(VertexPositionColor), 0, &cmd->data.draw.first);
    int i;

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    for (i = 0; i < count; i++) {
        verts->pos[0] = points[i].x + 0.5f;
        verts->pos[1] = points[i].y + 0.5f;
        verts->tex[0] = 0.0f;
        verts->tex[1] = 0.0f;
        verts->color = cmd->data.draw.color;
        verts++;
    }
    return 0;
}

static int VULKAN_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                               const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride,
                               int num_vertices, const void *indices, int num_indices, int size_indices,
                               float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    VertexPositionColor *verts = (VertexPositionColor *)SDL_AllocateRenderVertices(renderer, count * sizeof(VertexPositionColor), 0, &cmd->data.draw.first);

    if (!verts) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (i = 0; i < count; i++) {
        int j;
        float *xy_;
        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (float *)((char *)xy + j * xy_stride);

        verts->pos[0] = xy_[0] * scale_x;
        verts->pos[1] = xy_[1] * scale_y;
        verts->color = *(SDL_FColor *)((char *)color + j * color_stride);

        if (texture) {
            float *uv_ = (float *)((char *)uv + j * uv_stride);
            verts->tex[0] = uv_[0];
            verts->tex[1] = uv_[1];
        } else {
            verts->tex[0] = 0.0f;
            verts->tex[1] = 0.0f;
        }

        verts += 1;
    }
    return 0;
}

static int VULKAN_UpdateVertexBuffer(SDL_Renderer *renderer,
                                    const void *vertexData, size_t dataSizeInBytes)
{
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    HRESULT result = S_OK;
    const int vbidx = rendererData->currentVertexBuffer;
    UINT8 *vertexBufferData = NULL;
    VULKAN_RANGE range;
    IVULKANResource *vertexBuffer;

    range.Begin = 0;
    range.End = 0;

    if (dataSizeInBytes == 0) {
        return 0; /* nothing to do. */
    }

    if (rendererData->issueBatch) {
        if (FAILED(VULKAN_IssueBatch(rendererData))) {
            SDL_SetError("Failed to issue intermediate batch");
            return E_FAIL;
        }
    }

    /* If the existing vertex buffer isn't big enough, we need to recreate a big enough one */
    if (dataSizeInBytes > rendererData->vertexBuffers[vbidx].size) {
        VULKAN_CreateVertexBuffer(rendererData, vbidx, dataSizeInBytes);
    }

    vertexBuffer = rendererData->vertexBuffers[vbidx].resource;
    result = D3D_CALL(vertexBuffer, Map, 0, &range, (void **)&vertexBufferData);
    if (FAILED(result)) {
        return WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANResource::Map [vertex buffer]"), result);
    }
    SDL_memcpy(vertexBufferData, vertexData, dataSizeInBytes);
    D3D_CALL(vertexBuffer, Unmap, 0, NULL);

    rendererData->vertexBuffers[vbidx].view.SizeInBytes = (UINT)dataSizeInBytes;

    D3D_CALL(rendererData->commandList, IASetVertexBuffers, 0, 1, &rendererData->vertexBuffers[vbidx].view);

    rendererData->currentVertexBuffer++;
    if (rendererData->currentVertexBuffer >= SDL_VULKAN_NUM_VERTEX_BUFFERS) {
        rendererData->currentVertexBuffer = 0;
        rendererData->issueBatch = SDL_TRUE;
    }

    return S_OK;
#else
    return 0;
#endif
}

static int VULKAN_UpdateViewport(SDL_Renderer *renderer)
{
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    const SDL_Rect *viewport = &data->currentViewport;
    Float4X4 projection;
    Float4X4 view;
    SDL_FRect orientationAlignedViewport;
    BOOL swapDimensions;
    VULKAN_VIEWPORT d3dviewport;
    const int rotation = VULKAN_GetRotationForCurrentRenderTarget(renderer);

    if (viewport->w == 0 || viewport->h == 0) {
        /* If the viewport is empty, assume that it is because
         * SDL_CreateRenderer is calling it, and will call it again later
         * with a non-empty viewport.
         */
        /* SDL_Log("%s, no viewport was set!\n", __FUNCTION__); */
        return -1;
    }

    /* Make sure the SDL viewport gets rotated to that of the physical display's rotation.
     * Keep in mind here that the Y-axis will be been inverted (from Direct3D's
     * default coordinate system) so rotations will be done in the opposite
     * direction of the DXGI_MODE_ROTATION enumeration.
     */
    switch (rotation) {
    case DXGI_MODE_ROTATION_IDENTITY:
        projection = MatrixIdentity();
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        projection = MatrixRotationZ(SDL_PI_F * 0.5f);
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        projection = MatrixRotationZ(SDL_PI_F);
        break;
    case DXGI_MODE_ROTATION_ROTATE90:
        projection = MatrixRotationZ(-SDL_PI_F * 0.5f);
        break;
    default:
        return SDL_SetError("An unknown DisplayOrientation is being used");
    }

    /* Update the view matrix */
    SDL_zero(view);
    view.m[0][0] = 2.0f / viewport->w;
    view.m[1][1] = -2.0f / viewport->h;
    view.m[2][2] = 1.0f;
    view.m[3][0] = -1.0f;
    view.m[3][1] = 1.0f;
    view.m[3][3] = 1.0f;

    /* Combine the projection + view matrix together now, as both only get
     * set here (as of this writing, on Dec 26, 2013).  When done, store it
     * for eventual transfer to the GPU.
     */
    data->vertexShaderConstantsData.projectionAndView = MatrixMultiply(
        view,
        projection);

    /* Update the Direct3D viewport, which seems to be aligned to the
     * swap buffer's coordinate space, which is always in either
     * a landscape mode, for all Windows 8/RT devices, or a portrait mode,
     * for Windows Phone devices.
     */
    swapDimensions = VULKAN_IsDisplayRotated90Degrees((DXGI_MODE_ROTATION)rotation);
    if (swapDimensions) {
        orientationAlignedViewport.x = (float)viewport->y;
        orientationAlignedViewport.y = (float)viewport->x;
        orientationAlignedViewport.w = (float)viewport->h;
        orientationAlignedViewport.h = (float)viewport->w;
    } else {
        orientationAlignedViewport.x = (float)viewport->x;
        orientationAlignedViewport.y = (float)viewport->y;
        orientationAlignedViewport.w = (float)viewport->w;
        orientationAlignedViewport.h = (float)viewport->h;
    }

    d3dviewport.TopLeftX = orientationAlignedViewport.x;
    d3dviewport.TopLeftY = orientationAlignedViewport.y;
    d3dviewport.Width = orientationAlignedViewport.w;
    d3dviewport.Height = orientationAlignedViewport.h;
    d3dviewport.MinDepth = 0.0f;
    d3dviewport.MaxDepth = 1.0f;
    /* SDL_Log("%s: D3D viewport = {%f,%f,%f,%f}\n", __FUNCTION__, d3dviewport.TopLeftX, d3dviewport.TopLeftY, d3dviewport.Width, d3dviewport.Height); */
    D3D_CALL(data->commandList, RSSetViewports, 1, &d3dviewport);

    data->viewportDirty = SDL_FALSE;
#endif
    return 0;
}

#if D3D12_PORT
static int VULKAN_SetDrawState(SDL_Renderer *renderer, const SDL_RenderCommand *cmd, VULKAN_Shader shader,
                              VULKAN_PRIMITIVE_TOPOLOGY_TYPE topology,
                              const int numShaderResources, VULKAN_CPU_DESCRIPTOR_HANDLE *shaderResources,
                              VULKAN_CPU_DESCRIPTOR_HANDLE *sampler, const Float4X4 *matrix)

{
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    const Float4X4 *newmatrix = matrix ? matrix : &rendererData->identity;
    VULKAN_CPU_DESCRIPTOR_HANDLE renderTargetView = VULKAN_GetCurrentRenderTargetView(renderer);
    const SDL_BlendMode blendMode = cmd->data.draw.blend;
    SDL_bool updateSubresource = SDL_FALSE;
    int i;
    VULKAN_CPU_DESCRIPTOR_HANDLE firstShaderResource;
    DXGI_FORMAT rtvFormat = rendererData->renderTargetFormat;

    if (rendererData->textureRenderTarget) {
        rtvFormat = rendererData->textureRenderTarget->mainTextureFormat;
    }

    /* See if we need to change the pipeline state */
    if (!rendererData->currentPipelineState ||
        rendererData->currentPipelineState->shader != shader ||
        rendererData->currentPipelineState->blendMode != blendMode ||
        rendererData->currentPipelineState->topology != topology ||
        rendererData->currentPipelineState->rtvFormat != rtvFormat) {

        /* Find the matching pipeline.
           NOTE: Although it may seem inefficient to linearly search through ~450 pipelines
           to find the correct one, in profiling this doesn't come up at all.
           It's unlikely that using a hash table would affect performance a measurable amount unless
           it's a degenerate case that's changing the pipeline state dozens of times per frame.
        */
        rendererData->currentPipelineState = NULL;
        for (i = 0; i < rendererData->pipelineStateCount; ++i) {
            VULKAN_PipelineState *candidatePiplineState = &rendererData->pipelineStates[i];
            if (candidatePiplineState->shader == shader &&
                candidatePiplineState->blendMode == blendMode &&
                candidatePiplineState->topology == topology &&
                candidatePiplineState->rtvFormat == rtvFormat) {
                rendererData->currentPipelineState = candidatePiplineState;
                break;
            }
        }

        /* If we didn't find a match, create a new one -- it must mean the blend mode is non-standard */
        if (!rendererData->currentPipelineState) {
            rendererData->currentPipelineState = VULKAN_CreatePipelineState(renderer, shader, blendMode, topology, rtvFormat);
        }

        if (!rendererData->currentPipelineState) {
            return SDL_SetError("[direct3d12] Unable to create required pipeline state");
        }

        D3D_CALL(rendererData->commandList, SetPipelineState, rendererData->currentPipelineState->pipelineState);
        D3D_CALL(rendererData->commandList, SetGraphicsRootSignature,
                 rendererData->rootSignatures[VULKAN_GetRootSignatureType(rendererData->currentPipelineState->shader)]);
        /* When we change these we will need to re-upload the constant buffer and reset any descriptors */
        updateSubresource = SDL_TRUE;
        rendererData->currentSampler.ptr = 0;
        rendererData->currentShaderResource.ptr = 0;
    }

    if (renderTargetView.ptr != rendererData->currentRenderTargetView.ptr) {
        D3D_CALL(rendererData->commandList, OMSetRenderTargets, 1, &renderTargetView, FALSE, NULL);
        rendererData->currentRenderTargetView = renderTargetView;
    }

    if (rendererData->viewportDirty) {
        if (VULKAN_UpdateViewport(renderer) == 0) {
            /* vertexShaderConstantsData.projectionAndView has changed */
            updateSubresource = SDL_TRUE;
        }
    }

    if (rendererData->cliprectDirty) {
        VULKAN_RECT scissorRect;
        if (VULKAN_GetViewportAlignedD3DRect(renderer, &rendererData->currentCliprect, &scissorRect, TRUE) != 0) {
            /* VULKAN_GetViewportAlignedD3DRect will have set the SDL error */
            return -1;
        }
        D3D_CALL(rendererData->commandList, RSSetScissorRects, 1, &scissorRect);
        rendererData->cliprectDirty = SDL_FALSE;
    }

    if (numShaderResources > 0) {
        firstShaderResource = shaderResources[0];
    } else {
        firstShaderResource.ptr = 0;
    }
    if (firstShaderResource.ptr != rendererData->currentShaderResource.ptr) {
        for (i = 0; i < numShaderResources; ++i) {
            VULKAN_GPU_DESCRIPTOR_HANDLE GPUHandle = VULKAN_CPUtoGPUHandle(rendererData->srvDescriptorHeap, shaderResources[i]);
            D3D_CALL(rendererData->commandList, SetGraphicsRootDescriptorTable, i + 1, GPUHandle);
        }
        rendererData->currentShaderResource.ptr = firstShaderResource.ptr;
    }

    if (sampler && sampler->ptr != rendererData->currentSampler.ptr) {
        VULKAN_GPU_DESCRIPTOR_HANDLE GPUHandle = VULKAN_CPUtoGPUHandle(rendererData->samplerDescriptorHeap, *sampler);
        UINT tableIndex = 0;

        /* Figure out the correct sampler descriptor table index based on the type of shader */
        switch (shader) {
        case SHADER_RGB:
            tableIndex = 2;
            break;
#if SDL_HAVE_YUV
        case SHADER_YUV_JPEG:
        case SHADER_YUV_BT601:
        case SHADER_YUV_BT709:
            tableIndex = 4;
            break;
        case SHADER_NV12_JPEG:
        case SHADER_NV12_BT601:
        case SHADER_NV12_BT709:
        case SHADER_NV21_JPEG:
        case SHADER_NV21_BT601:
        case SHADER_NV21_BT709:
            tableIndex = 3;
            break;
#endif
        default:
            return SDL_SetError("[direct3d12] Trying to set a sampler for a shader which doesn't have one");
            break;
        }

        D3D_CALL(rendererData->commandList, SetGraphicsRootDescriptorTable, tableIndex, GPUHandle);
        rendererData->currentSampler = *sampler;
    }

    if (updateSubresource == SDL_TRUE || SDL_memcmp(&rendererData->vertexShaderConstantsData.model, newmatrix, sizeof(*newmatrix)) != 0) {
        SDL_memcpy(&rendererData->vertexShaderConstantsData.model, newmatrix, sizeof(*newmatrix));
        D3D_CALL(rendererData->commandList, SetGraphicsRoot32BitConstants,
                 0,
                 32,
                 &rendererData->vertexShaderConstantsData,
                 0);
    }

    return 0;
}
#endif

#if D3D12_PORT
static int VULKAN_SetCopyState(SDL_Renderer *renderer, const SDL_RenderCommand *cmd, const Float4X4 *matrix)
{
    SDL_Texture *texture = cmd->data.draw.texture;
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_TextureData *textureData = (VULKAN_TextureData *)texture->driverdata;
    VULKAN_CPU_DESCRIPTOR_HANDLE *textureSampler;

    switch (textureData->scaleMode) {
    case VULKAN_FILTER_MIN_MAG_MIP_POINT:
        textureSampler = &rendererData->nearestPixelSampler;
        break;
    case VULKAN_FILTER_MIN_MAG_MIP_LINEAR:
        textureSampler = &rendererData->linearSampler;
        break;
    default:
        return SDL_SetError("Unknown scale mode: %d\n", textureData->scaleMode);
    }
#if SDL_HAVE_YUV
    if (textureData->yuv) {
        VULKAN_CPU_DESCRIPTOR_HANDLE shaderResources[] = {
            textureData->mainTextureResourceView,
            textureData->mainTextureResourceViewU,
            textureData->mainTextureResourceViewV
        };
        VULKAN_Shader shader;

        switch (SDL_GetYUVConversionModeForResolution(texture->w, texture->h)) {
        case SDL_YUV_CONVERSION_JPEG:
            shader = SHADER_YUV_JPEG;
            break;
        case SDL_YUV_CONVERSION_BT601:
            shader = SHADER_YUV_BT601;
            break;
        case SDL_YUV_CONVERSION_BT709:
            shader = SHADER_YUV_BT709;
            break;
        default:
            return SDL_SetError("Unsupported YUV conversion mode");
        }

        /* Make sure each texture is in the correct state to be accessed by the pixel shader. */
        VULKAN_TransitionResource(rendererData, textureData->mainTexture, textureData->mainResourceState, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        textureData->mainResourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        VULKAN_TransitionResource(rendererData, textureData->mainTextureU, textureData->mainResourceStateU, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        textureData->mainResourceStateU = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        VULKAN_TransitionResource(rendererData, textureData->mainTextureV, textureData->mainResourceStateV, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        textureData->mainResourceStateV = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        return VULKAN_SetDrawState(renderer, cmd, shader, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, SDL_arraysize(shaderResources), shaderResources,
                                  textureSampler, matrix);
    } else if (textureData->nv12) {
        VULKAN_CPU_DESCRIPTOR_HANDLE shaderResources[] = {
            textureData->mainTextureResourceView,
            textureData->mainTextureResourceViewNV,
        };
        VULKAN_Shader shader;

        switch (SDL_GetYUVConversionModeForResolution(texture->w, texture->h)) {
        case SDL_YUV_CONVERSION_JPEG:
            shader = texture->format == SDL_PIXELFORMAT_NV12 ? SHADER_NV12_JPEG : SHADER_NV21_JPEG;
            break;
        case SDL_YUV_CONVERSION_BT601:
            shader = texture->format == SDL_PIXELFORMAT_NV12 ? SHADER_NV12_BT601 : SHADER_NV21_BT601;
            break;
        case SDL_YUV_CONVERSION_BT709:
            shader = texture->format == SDL_PIXELFORMAT_NV12 ? SHADER_NV12_BT709 : SHADER_NV21_BT709;
            break;
        default:
            return SDL_SetError("Unsupported YUV conversion mode");
        }

        /* Make sure each texture is in the correct state to be accessed by the pixel shader. */
        VULKAN_TransitionResource(rendererData, textureData->mainTexture, textureData->mainResourceState, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        textureData->mainResourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        return VULKAN_SetDrawState(renderer, cmd, shader, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, SDL_arraysize(shaderResources), shaderResources,
                                  textureSampler, matrix);
    }
#endif /* SDL_HAVE_YUV */
    VULKAN_TransitionResource(rendererData, textureData->mainTexture, textureData->mainResourceState, VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    textureData->mainResourceState = VULKAN_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return VULKAN_SetDrawState(renderer, cmd, SHADER_RGB, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 1, &textureData->mainTextureResourceView,
                              textureSampler, matrix);
}
#endif

#if D3D12_PORT
static void VULKAN_DrawPrimitives(SDL_Renderer *renderer, VULKAN_PRIMITIVE_TOPOLOGY primitiveTopology, const size_t vertexStart, const size_t vertexCount)
{
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
    D3D_CALL(rendererData->commandList, IASetPrimitiveTopology, primitiveTopology);
    D3D_CALL(rendererData->commandList, DrawInstanced, (UINT)vertexCount, 1, (UINT)vertexStart, 0);
}
#endif

static void VULKAN_InvalidateCachedState(SDL_Renderer *renderer)
{
#if D3D12_PORT

    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    data->currentRenderTargetView.ptr = 0;
    data->currentShaderResource.ptr = 0;
    data->currentSampler.ptr = 0;
    data->cliprectDirty = SDL_TRUE;
    data->viewportDirty = SDL_TRUE;
#endif
}

static int VULKAN_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;

#if D3D12_PORT
    const int viewportRotation = VULKAN_GetRotationForCurrentRenderTarget(renderer);

    if (rendererData->pixelSizeChanged) {
        VULKAN_UpdateForWindowSizeChange(renderer);
        rendererData->pixelSizeChanged = SDL_FALSE;
    }

    if (rendererData->currentViewportRotation != viewportRotation) {
        rendererData->currentViewportRotation = viewportRotation;
        rendererData->viewportDirty = SDL_TRUE;
    }

    if (VULKAN_UpdateVertexBuffer(renderer, vertices, vertsize) < 0) {
        return -1;
    }
#endif

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETDRAWCOLOR:
        {
            break; /* this isn't currently used in this render backend. */
        }

        case SDL_RENDERCMD_SETVIEWPORT:
        {
            SDL_Rect *viewport = &rendererData->currentViewport;
            if (SDL_memcmp(viewport, &cmd->data.viewport.rect, sizeof(cmd->data.viewport.rect)) != 0) {
                SDL_copyp(viewport, &cmd->data.viewport.rect);
                rendererData->viewportDirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT:
        {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            SDL_Rect viewport_cliprect;
            if (rendererData->currentCliprectEnabled != cmd->data.cliprect.enabled) {
                rendererData->currentCliprectEnabled = cmd->data.cliprect.enabled;
                rendererData->cliprectDirty = SDL_TRUE;
            }
            if (!rendererData->currentCliprectEnabled) {
                /* If the clip rect is disabled, then the scissor rect should be the whole viewport,
                   since direct3d12 doesn't allow disabling the scissor rectangle */
                viewport_cliprect.x = 0;
                viewport_cliprect.y = 0;
                viewport_cliprect.w = rendererData->currentViewport.w;
                viewport_cliprect.h = rendererData->currentViewport.h;
                rect = &viewport_cliprect;
            }
            if (SDL_memcmp(&rendererData->currentCliprect, rect, sizeof(*rect)) != 0) {
                SDL_copyp(&rendererData->currentCliprect, rect);
                rendererData->cliprectDirty = SDL_TRUE;
            }
            break;
        }

        case SDL_RENDERCMD_CLEAR:
        {
            VkClearColorValue clearColor;
            clearColor.float32[0] = cmd->data.color.color.r;
            clearColor.float32[1] = cmd->data.color.color.g;
            clearColor.float32[2] = cmd->data.color.color.b;
            clearColor.float32[3] = cmd->data.color.color.a;
            VULKAN_ActivateCommandBuffer(renderer, VK_ATTACHMENT_LOAD_OP_CLEAR, &clearColor);
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        {
#if D3D12_PORT
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const size_t start = first / sizeof(VertexPositionColor);
            VULKAN_SetDrawState(renderer, cmd, SHADER_SOLID, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_POINT, 0, NULL, NULL, NULL);
            VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, start, count);
#endif
            break;
        }

        case SDL_RENDERCMD_DRAW_LINES:
        {
#if D3D12_PORT
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const size_t start = first / sizeof(VertexPositionColor);
            const VertexPositionColor *verts = (VertexPositionColor *)(((Uint8 *)vertices) + first);
            VULKAN_SetDrawState(renderer, cmd, SHADER_SOLID, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_LINE, 0, NULL, NULL, NULL);
            VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, start, count);
            if (verts[0].pos.x != verts[count - 1].pos.x || verts[0].pos.y != verts[count - 1].pos.y) {
                VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, start + (count - 1), 1);
            }
#endif
            break;
        }

        case SDL_RENDERCMD_FILL_RECTS: /* unused */
            break;

        case SDL_RENDERCMD_COPY: /* unused */
            break;

        case SDL_RENDERCMD_COPY_EX: /* unused */
            break;

        case SDL_RENDERCMD_GEOMETRY:
        {
#if D3D12_PORT
            SDL_Texture *texture = cmd->data.draw.texture;
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const size_t start = first / sizeof(VertexPositionColor);

            if (texture) {
                VULKAN_SetCopyState(renderer, cmd, NULL);
            } else {
                VULKAN_SetDrawState(renderer, cmd, SHADER_SOLID, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, NULL, NULL, NULL);
            }

            VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, start, count);
#endif
            break;
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }
    return 0;
}

static int VULKAN_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect,
                                  Uint32 format, void *pixels, int pitch)
{
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    IVULKANResource *backBuffer = NULL;
    IVULKANResource *readbackBuffer = NULL;
    HRESULT result;
    int status = -1;
    VULKAN_RESOURCE_DESC textureDesc;
    VULKAN_RESOURCE_DESC readbackDesc;
    VULKAN_HEAP_PROPERTIES heapProps;
    VULKAN_RECT srcRect = { 0, 0, 0, 0 };
    VULKAN_BOX srcBox;
    VULKAN_TEXTURE_COPY_LOCATION dstLocation;
    VULKAN_TEXTURE_COPY_LOCATION srcLocation;
    VULKAN_PLACED_SUBRESOURCE_FOOTPRINT placedTextureDesc;
    VULKAN_SUBRESOURCE_FOOTPRINT pitchedDesc;
    BYTE *textureMemory;
    int bpp;

    if (data->textureRenderTarget) {
        backBuffer = data->textureRenderTarget->mainTexture;
    } else {
        backBuffer = data->renderTargets[data->currentBackBufferIndex];
    }

    /* Create a staging texture to copy the screen's data to: */
    SDL_zero(textureDesc);
    D3D_CALL_RET(backBuffer, GetDesc, &textureDesc);
    textureDesc.Width = rect->w;
    textureDesc.Height = rect->h;

    SDL_zero(readbackDesc);
    readbackDesc.Dimension = VULKAN_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Alignment = VULKAN_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.SampleDesc.Quality = 0;
    readbackDesc.Layout = VULKAN_TEXTURE_LAYOUT_ROW_MAJOR;
    readbackDesc.Flags = VULKAN_RESOURCE_FLAG_NONE;

    /* Figure out how much we need to allocate for the upload buffer */
    D3D_CALL(data->d3dDevice, GetCopyableFootprints,
             &textureDesc,
             0,
             1,
             0,
             NULL,
             NULL,
             NULL,
             &readbackDesc.Width);

    SDL_zero(heapProps);
    heapProps.Type = VULKAN_HEAP_TYPE_READBACK;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    result = D3D_CALL(data->d3dDevice, CreateCommittedResource,
                      &heapProps,
                      VULKAN_HEAP_FLAG_NONE,
                      &readbackDesc,
                      VULKAN_RESOURCE_STATE_COPY_DEST,
                      NULL,
                      D3D_GUID(SDL_IID_IVULKANResource),
                      (void **)&readbackBuffer);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateTexture2D [create staging texture]"), result);
        goto done;
    }

    /* Transition the render target to be copyable from */
    VULKAN_TransitionResource(data, backBuffer, VULKAN_RESOURCE_STATE_RENDER_TARGET, VULKAN_RESOURCE_STATE_COPY_SOURCE);

    /* Copy the desired portion of the back buffer to the staging texture: */
    if (VULKAN_GetViewportAlignedD3DRect(renderer, rect, &srcRect, FALSE) != 0) {
        /* VULKAN_GetViewportAlignedD3DRect will have set the SDL error */
        goto done;
    }
    srcBox.left = srcRect.left;
    srcBox.right = srcRect.right;
    srcBox.top = srcRect.top;
    srcBox.bottom = srcRect.bottom;
    srcBox.front = 0;
    srcBox.back = 1;

    /* Issue the copy texture region */
    SDL_zero(pitchedDesc);
    pitchedDesc.Format = textureDesc.Format;
    pitchedDesc.Width = (UINT)textureDesc.Width;
    pitchedDesc.Height = textureDesc.Height;
    pitchedDesc.Depth = 1;
    if (pitchedDesc.Format == DXGI_FORMAT_R8_UNORM) {
        bpp = 1;
    } else {
        bpp = 4;
    }
    pitchedDesc.RowPitch = VULKAN_Align(pitchedDesc.Width * bpp, VULKAN_TEXTURE_DATA_PITCH_ALIGNMENT);

    SDL_zero(placedTextureDesc);
    placedTextureDesc.Offset = 0;
    placedTextureDesc.Footprint = pitchedDesc;

    SDL_zero(dstLocation);
    dstLocation.pResource = readbackBuffer;
    dstLocation.Type = VULKAN_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLocation.PlacedFootprint = placedTextureDesc;

    SDL_zero(srcLocation);
    srcLocation.pResource = backBuffer;
    srcLocation.Type = VULKAN_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLocation.SubresourceIndex = 0;

    D3D_CALL(data->commandList, CopyTextureRegion,
             &dstLocation,
             0, 0, 0,
             &srcLocation,
             &srcBox);

    /* We need to issue the command list for the copy to finish */
    VULKAN_IssueBatch(data);

    /* Transition the render target back to a render target */
    VULKAN_TransitionResource(data, backBuffer, VULKAN_RESOURCE_STATE_COPY_SOURCE, VULKAN_RESOURCE_STATE_RENDER_TARGET);

    /* Map the staging texture's data to CPU-accessible memory: */
    result = D3D_CALL(readbackBuffer, Map,
                      0,
                      NULL,
                      (void **)&textureMemory);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANResource::Map [map staging texture]"), result);
        goto done;
    }

    /* Copy the data into the desired buffer, converting pixels to the
     * desired format at the same time:
     */
    status = SDL_ConvertPixelsAndColorspace(
        rect->w, rect->h,
        VULKAN_DXGIFormatToSDLPixelFormat(textureDesc.Format),
        renderer->target ? renderer->target->colorspace : renderer->output_colorspace,
        textureMemory,
        pitchedDesc.RowPitch,
        format,
        renderer->input_colorspace,
        pixels,
        pitch);

    /* Unmap the texture: */
    D3D_CALL(readbackBuffer, Unmap, 0, NULL);

done:
    SAFE_RELEASE(readbackBuffer);
    return status;
#else
    return 0;
#endif
}

static int VULKAN_RenderPresent(SDL_Renderer *renderer)
{
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VkResult result = VK_SUCCESS;
    if (data->currentCommandBuffer) {
        if (data->currentRenderPass) {
            vkCmdEndRenderPass(data->currentCommandBuffer);
            data->currentRenderPass = VK_NULL_HANDLE;
        }

        VULKAN_RecordPipelineImageBarrier(renderer,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            data->swapChainImageLayouts[data->currentCommandBufferIndex],
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            data->swapchainImages[data->currentCommandBufferIndex]);
        data->swapChainImageLayouts[data->currentCommandBufferIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        vkEndCommandBuffer(data->currentCommandBuffer);

        VkPipelineStageFlags waitDestStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo = { 0 };
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &data->imageAvailableSemaphore;
        submitInfo.pWaitDstStageMask = &waitDestStageMask;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &data->currentCommandBuffer;
        //submitInfo.signalSemaphoreCount = 1;
        //submitInfo.pSignalSemaphores = &vulkanContext->renderingFinishedSemaphore;
        result = vkQueueSubmit(data->graphicsQueue, 1, &submitInfo, NULL); // TODO-  fence data->fences[frameIndex]);
        
        data->currentCommandBuffer = VK_NULL_HANDLE;
        
        //TODO - this it temporary
        vkDeviceWaitIdle(data->device);
        
        VkPresentInfoKHR presentInfo = { 0 };
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        //presentInfo.waitSemaphoreCount = 1;
        //presentInfo.pWaitSemaphores = &vulkanContext->renderingFinishedSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &data->swapchain;
        presentInfo.pImageIndices = &data->currentSwapchainImageIndex;
        result = vkQueuePresentKHR(data->presentQueue, &presentInfo);
        // TODO: Handle this
        
        //TODO - this it temporary
        vkDeviceWaitIdle(data->device);

        data->currentCommandBufferIndex = ( data->currentCommandBufferIndex + 1 ) % data->swapchainImageCount;

    }
    
    return (result == VK_SUCCESS);
}

static int VULKAN_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    if (vsync) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    } else {
        renderer->info.flags &= ~SDL_RENDERER_PRESENTVSYNC;
    }
    return 0;
}

SDL_Renderer *VULKAN_CreateRenderer(SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_Renderer *renderer;
    VULKAN_RenderData *data;

    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        return NULL;
    }
    renderer->magic = &SDL_renderer_magic;

    SDL_SetupRendererColorspace(renderer, create_props);

    if (renderer->output_colorspace != SDL_COLORSPACE_SRGB &&
        renderer->output_colorspace != SDL_COLORSPACE_SCRGB
        /*&& renderer->output_colorspace != SDL_COLORSPACE_HDR10*/) {
        SDL_SetError("Unsupported output colorspace");
        SDL_free(renderer);
        return NULL;
    }

    data = (VULKAN_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        SDL_free(renderer);
        return NULL;
    }

#if D3D12_PORT
    data->identity = MatrixIdentity();
#endif

    renderer->WindowEvent = VULKAN_WindowEvent;
    renderer->SupportsBlendMode = VULKAN_SupportsBlendMode;
    renderer->CreateTexture = VULKAN_CreateTexture;
    renderer->UpdateTexture = VULKAN_UpdateTexture;
#if SDL_HAVE_YUV
    renderer->UpdateTextureYUV = VULKAN_UpdateTextureYUV;
    renderer->UpdateTextureNV = VULKAN_UpdateTextureNV;
#endif
    renderer->LockTexture = VULKAN_LockTexture;
    renderer->UnlockTexture = VULKAN_UnlockTexture;
    renderer->SetTextureScaleMode = VULKAN_SetTextureScaleMode;
    renderer->SetRenderTarget = VULKAN_SetRenderTarget;
    renderer->QueueSetViewport = VULKAN_QueueSetViewport;
    renderer->QueueSetDrawColor = VULKAN_QueueSetViewport; /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = VULKAN_QueueDrawPoints;
    renderer->QueueDrawLines = VULKAN_QueueDrawPoints; /* lines and points queue vertices the same way. */
    renderer->QueueGeometry = VULKAN_QueueGeometry;
    renderer->InvalidateCachedState = VULKAN_InvalidateCachedState;
    renderer->RunCommandQueue = VULKAN_RunCommandQueue;
    renderer->RenderReadPixels = VULKAN_RenderReadPixels;
    renderer->RenderPresent = VULKAN_RenderPresent;
    renderer->DestroyTexture = VULKAN_DestroyTexture;
    renderer->DestroyRenderer = VULKAN_DestroyRenderer;
    renderer->info = VULKAN_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED;
    renderer->driverdata = data;
    VULKAN_InvalidateCachedState(renderer);

    if (SDL_GetBooleanProperty(create_props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_BOOLEAN, SDL_FALSE)) {
        renderer->info.flags |= SDL_RENDERER_PRESENTVSYNC;
    }
    renderer->SetVSync = VULKAN_SetVSync;

    /* HACK: make sure the SDL_Renderer references the SDL_Window data now, in
     * order to give init functions access to the underlying window handle:
     */
    renderer->window = window;

    /* Initialize Direct3D resources */
    if (VULKAN_CreateDeviceResources(renderer) != VK_SUCCESS) {
        VULKAN_DestroyRenderer(renderer);
        return NULL;
    }
    if (VULKAN_CreateWindowSizeDependentResources(renderer) != VK_SUCCESS) {
        VULKAN_DestroyRenderer(renderer);
        return NULL;
    }

    return renderer;
}

SDL_RenderDriver VULKAN_RenderDriver = {
    VULKAN_CreateRenderer,
    {
        "vulkan",
        (SDL_RENDERER_ACCELERATED |
         SDL_RENDERER_PRESENTVSYNC), /* flags.  see SDL_RendererFlags */
        7,                           /* num_texture_formats */
        {                            /* texture_formats */
          SDL_PIXELFORMAT_ARGB8888,
          SDL_PIXELFORMAT_XRGB8888,
          SDL_PIXELFORMAT_RGBA64_FLOAT,
          SDL_PIXELFORMAT_YV12,
          SDL_PIXELFORMAT_IYUV,
          SDL_PIXELFORMAT_NV12,
          SDL_PIXELFORMAT_NV21 },
        16384, /* max_texture_width */
        16384  /* max_texture_height */
    }
};

#endif /* SDL_VIDEO_RENDER_VULKAN && !SDL_RENDER_DISABLED */
