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

#define SDL_VULKAN_NUM_BUFFERS        2
#define SDL_VULKAN_NUM_VERTEX_BUFFERS 256
#define SDL_VULKAN_MAX_NUM_TEXTURES   16384
#define SDL_VULKAN_NUM_UPLOAD_BUFFERS 32

#include "SDL_vulkan.h"
#include <vulkan/vulkan.h>
#include "../SDL_sysrender.h"
#include "../SDL_d3dmath.h"

/* Vertex shader, common values */
typedef struct
{
    Float4X4 model;
    Float4X4 projectionAndView;
} VertexShaderConstants;

/* Per-vertex data */
typedef struct
{
    Float2 pos;
    Float2 tex;
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
#if D3D12_PORT
    VULKAN_Shader shader;
    SDL_BlendMode blendMode;
    VULKAN_PRIMITIVE_TOPOLOGY_TYPE topology;
    DXGI_FORMAT rtvFormat;
    IVULKANPipelineState *pipelineState;
#else
    int port;
#endif
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
#if D3D12_PORT
    IVULKANDevice1 *d3dDevice;
    IVULKANDebug *debugInterface;
    IVULKANCommandQueue *commandQueue;
    IVULKANGraphicsCommandList2 *commandList;
    DXGI_SWAP_EFFECT swapEffect;
    UINT swapFlags;
    DXGI_FORMAT renderTargetFormat;
    SDL_bool pixelSizeChanged;

    /* Descriptor heaps */
    IVULKANDescriptorHeap *rtvDescriptorHeap;
    UINT rtvDescriptorSize;
    IVULKANDescriptorHeap *textureRTVDescriptorHeap;
    IVULKANDescriptorHeap *srvDescriptorHeap;
    UINT srvDescriptorSize;
    IVULKANDescriptorHeap *samplerDescriptorHeap;
    UINT samplerDescriptorSize;

    /* Data needed per backbuffer */
    IVULKANCommandAllocator *commandAllocators[SDL_VULKAN_NUM_BUFFERS];
    IVULKANResource *renderTargets[SDL_VULKAN_NUM_BUFFERS];
    UINT64 fenceValue;
    int currentBackBufferIndex;

    /* Fences */
    IVULKANFence *fence;
    HANDLE fenceEvent;

    /* Root signature and pipeline state data */
    IVULKANRootSignature *rootSignatures[NUM_ROOTSIGS];
    int pipelineStateCount;
    VULKAN_PipelineState *pipelineStates;
    VULKAN_PipelineState *currentPipelineState;

    VULKAN_VertexBuffer vertexBuffers[SDL_VULKAN_NUM_VERTEX_BUFFERS];
    VULKAN_CPU_DESCRIPTOR_HANDLE nearestPixelSampler;
    VULKAN_CPU_DESCRIPTOR_HANDLE linearSampler;

    /* Data for staging/allocating textures */
    IVULKANResource *uploadBuffers[SDL_VULKAN_NUM_UPLOAD_BUFFERS];
    int currentUploadBuffer;

    /* Pool allocator to handle reusing SRV heap indices */
    VULKAN_SRVPoolNode *srvPoolHead;
    VULKAN_SRVPoolNode srvPoolNodes[SDL_VULKAN_MAX_NUM_TEXTURES];

    /* Vertex buffer constants */
    VertexShaderConstants vertexShaderConstantsData;

    /* Cached renderer properties */
    DXGI_MODE_ROTATION rotation;
    VULKAN_TextureData *textureRenderTarget;
    VULKAN_CPU_DESCRIPTOR_HANDLE currentRenderTargetView;
    VULKAN_CPU_DESCRIPTOR_HANDLE currentShaderResource;
    VULKAN_CPU_DESCRIPTOR_HANDLE currentSampler;
    SDL_bool cliprectDirty;
    SDL_bool currentCliprectEnabled;
    SDL_Rect currentCliprect;
    SDL_Rect currentViewport;
    int currentViewportRotation;
    SDL_bool viewportDirty;
    Float4X4 identity;
    int currentVertexBuffer;
    SDL_bool issueBatch;
#else
    int port;
#endif
} VULKAN_RenderData;


static void VULKAN_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture);

static void VULKAN_ReleaseAll(SDL_Renderer *renderer)
{
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    SDL_Texture *texture = NULL;

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    SDL_SetProperty(props, SDL_PROP_RENDERER_VULKAN_DEVICE_POINTER, NULL);
    SDL_SetProperty(props, SDL_PROP_RENDERER_VULKAN_COMMAND_QUEUE_POINTER, NULL);

    /* Release all textures */
    for (texture = renderer->textures; texture; texture = texture->next) {
        VULKAN_DestroyTexture(renderer, texture);
    }

    /* Release/reset everything else */
    if (data) {
        int i;

#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
        SAFE_RELEASE(data->dxgiFactory);
        SAFE_RELEASE(data->dxgiAdapter);
        SAFE_RELEASE(data->swapChain);
#endif
        SAFE_RELEASE(data->d3dDevice);
        SAFE_RELEASE(data->debugInterface);
        SAFE_RELEASE(data->commandQueue);
        SAFE_RELEASE(data->commandList);
        SAFE_RELEASE(data->rtvDescriptorHeap);
        SAFE_RELEASE(data->textureRTVDescriptorHeap);
        SAFE_RELEASE(data->srvDescriptorHeap);
        SAFE_RELEASE(data->samplerDescriptorHeap);
        SAFE_RELEASE(data->fence);

        for (i = 0; i < SDL_VULKAN_NUM_BUFFERS; ++i) {
            SAFE_RELEASE(data->commandAllocators[i]);
            SAFE_RELEASE(data->renderTargets[i]);
        }

        if (data->pipelineStateCount > 0) {
            for (i = 0; i < data->pipelineStateCount; ++i) {
                SAFE_RELEASE(data->pipelineStates[i].pipelineState);
            }
            SDL_free(data->pipelineStates);
            data->pipelineStateCount = 0;
        }

        for (i = 0; i < NUM_ROOTSIGS; ++i) {
            SAFE_RELEASE(data->rootSignatures[i]);
        }

        for (i = 0; i < SDL_VULKAN_NUM_VERTEX_BUFFERS; ++i) {
            SAFE_RELEASE(data->vertexBuffers[i].resource);
            data->vertexBuffers[i].size = 0;
        }

        data->swapEffect = (DXGI_SWAP_EFFECT)0;
        data->swapFlags = 0;
        data->currentRenderTargetView.ptr = 0;
        data->currentSampler.ptr = 0;

#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
        /* Check for any leaks if in debug mode */
        if (data->dxgiDebug) {
            DXGI_DEBUG_RLO_FLAGS rloFlags = (DXGI_DEBUG_RLO_FLAGS)(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL);
            D3D_CALL(data->dxgiDebug, ReportLiveObjects, SDL_DXGI_DEBUG_ALL, rloFlags);
            SAFE_RELEASE(data->dxgiDebug);
        }
#endif

        /* Unload the D3D libraries.  This should be done last, in order
         * to prevent IUnknown::Release() calls from crashing.
         */
        if (data->hVULKANMod) {
            SDL_UnloadObject(data->hVULKANMod);
            data->hVULKANMod = NULL;
        }
        if (data->hDXGIMod) {
            SDL_UnloadObject(data->hDXGIMod);
            data->hDXGIMod = NULL;
        }
    }
#else
    int port;
#endif
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
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_WaitForGPU(data);
    VULKAN_ReleaseAll(renderer);
    if (data) {
        SDL_free(data);
    }
    SDL_free(renderer);
#endif
}

#if D3D12_PORT

static void VULKAN_CreateBlendState(SDL_Renderer *renderer, SDL_BlendMode blendMode, VULKAN_BLEND_DESC *outBlendDesc)
{
    SDL_BlendFactor srcColorFactor = SDL_GetBlendModeSrcColorFactor(blendMode);
    SDL_BlendFactor srcAlphaFactor = SDL_GetBlendModeSrcAlphaFactor(blendMode);
    SDL_BlendOperation colorOperation = SDL_GetBlendModeColorOperation(blendMode);
    SDL_BlendFactor dstColorFactor = SDL_GetBlendModeDstColorFactor(blendMode);
    SDL_BlendFactor dstAlphaFactor = SDL_GetBlendModeDstAlphaFactor(blendMode);
    SDL_BlendOperation alphaOperation = SDL_GetBlendModeAlphaOperation(blendMode);

    SDL_zerop(outBlendDesc);
    outBlendDesc->AlphaToCoverageEnable = FALSE;
    outBlendDesc->IndependentBlendEnable = FALSE;
    outBlendDesc->RenderTarget[0].BlendEnable = TRUE;
    outBlendDesc->RenderTarget[0].SrcBlend = GetBlendFunc(srcColorFactor);
    outBlendDesc->RenderTarget[0].DestBlend = GetBlendFunc(dstColorFactor);
    outBlendDesc->RenderTarget[0].BlendOp = GetBlendEquation(colorOperation);
    outBlendDesc->RenderTarget[0].SrcBlendAlpha = GetBlendFunc(srcAlphaFactor);
    outBlendDesc->RenderTarget[0].DestBlendAlpha = GetBlendFunc(dstAlphaFactor);
    outBlendDesc->RenderTarget[0].BlendOpAlpha = GetBlendEquation(alphaOperation);
    outBlendDesc->RenderTarget[0].RenderTargetWriteMask = VULKAN_COLOR_WRITE_ENABLE_ALL;
}

static VULKAN_PipelineState *VULKAN_CreatePipelineState(SDL_Renderer *renderer,
                                                      VULKAN_Shader shader,
                                                      SDL_BlendMode blendMode,
                                                      VULKAN_PRIMITIVE_TOPOLOGY_TYPE topology,
                                                      DXGI_FORMAT rtvFormat)
{
    const VULKAN_INPUT_ELEMENT_DESC vertexDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, VULKAN_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, VULKAN_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, VULKAN_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    VULKAN_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc;
    IVULKANPipelineState *pipelineState = NULL;
    VULKAN_PipelineState *pipelineStates;
    HRESULT result = S_OK;

    SDL_zero(pipelineDesc);
    pipelineDesc.pRootSignature = data->rootSignatures[VULKAN_GetRootSignatureType(shader)];
    VULKAN_GetVertexShader(shader, &pipelineDesc.VS);
    VULKAN_GetPixelShader(shader, &pipelineDesc.PS);
    VULKAN_CreateBlendState(renderer, blendMode, &pipelineDesc.BlendState);
    pipelineDesc.SampleMask = 0xffffffff;

    pipelineDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    pipelineDesc.RasterizerState.CullMode = VULKAN_CULL_MODE_NONE;
    pipelineDesc.RasterizerState.DepthBias = 0;
    pipelineDesc.RasterizerState.DepthBiasClamp = 0.0f;
    pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
    pipelineDesc.RasterizerState.FillMode = VULKAN_FILL_MODE_SOLID;
    pipelineDesc.RasterizerState.FrontCounterClockwise = FALSE;
    pipelineDesc.RasterizerState.MultisampleEnable = FALSE;
    pipelineDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;

    pipelineDesc.InputLayout.pInputElementDescs = vertexDesc;
    pipelineDesc.InputLayout.NumElements = 3;

    pipelineDesc.PrimitiveTopologyType = topology;

    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] = rtvFormat;
    pipelineDesc.SampleDesc.Count = 1;
    pipelineDesc.SampleDesc.Quality = 0;

    result = D3D_CALL(data->d3dDevice, CreateGraphicsPipelineState,
                      &pipelineDesc,
                      D3D_GUID(SDL_IID_IVULKANPipelineState),
                      (void **)&pipelineState);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateGraphicsPipelineState"), result);
        return NULL;
    }

    pipelineStates = (VULKAN_PipelineState *)SDL_realloc(data->pipelineStates, (data->pipelineStateCount + 1) * sizeof(*pipelineStates));
    if (!pipelineStates) {
        SAFE_RELEASE(pipelineState);
        return NULL;
    }

    pipelineStates[data->pipelineStateCount].shader = shader;
    pipelineStates[data->pipelineStateCount].blendMode = blendMode;
    pipelineStates[data->pipelineStateCount].topology = topology;
    pipelineStates[data->pipelineStateCount].rtvFormat = rtvFormat;
    pipelineStates[data->pipelineStateCount].pipelineState = pipelineState;
    data->pipelineStates = pipelineStates;
    ++data->pipelineStateCount;

    return &pipelineStates[data->pipelineStateCount - 1];
}
#endif

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

/* Create resources that depend on the device. */
static VkResult VULKAN_CreateDeviceResources(SDL_Renderer *renderer)
{
#if D3D12_PORT
#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
    typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY)(UINT flags, REFIID riid, void **ppFactory);
    PFN_CREATE_DXGI_FACTORY CreateDXGIFactoryFunc;
    PFN_VULKAN_CREATE_DEVICE VULKANCreateDeviceFunc;
#endif
    typedef HANDLE(WINAPI * PFN_CREATE_EVENT_EX)(LPSECURITY_ATTRIBUTES lpEventAttributes, LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
    PFN_CREATE_EVENT_EX CreateEventExFunc;

    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    IVULKANDevice *d3dDevice = NULL;
    HRESULT result = S_OK;
    UINT creationFlags = 0;
    int i, j, k, l;
    SDL_bool createDebug;

    VULKAN_COMMAND_QUEUE_DESC queueDesc;
    VULKAN_DESCRIPTOR_HEAP_DESC descriptorHeapDesc;
    VULKAN_SAMPLER_DESC samplerDesc;
    IVULKANDescriptorHeap *rootDescriptorHeaps[2];

    const SDL_BlendMode defaultBlendModes[] = {
        SDL_BLENDMODE_NONE,
        SDL_BLENDMODE_BLEND,
        SDL_BLENDMODE_ADD,
        SDL_BLENDMODE_MOD,
        SDL_BLENDMODE_MUL
    };
    const DXGI_FORMAT defaultRTVFormats[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8X8_UNORM,
        DXGI_FORMAT_R8_UNORM
    };

    /* See if we need debug interfaces */
    createDebug = SDL_GetHintBoolean(SDL_HINT_RENDER_DIRECT3D11_DEBUG, SDL_FALSE);

#ifdef SDL_PLATFORM_GDK
    CreateEventExFunc = CreateEventExW;
#else
    /* CreateEventEx() arrived in Vista, so we need to load it with GetProcAddress for XP. */
    {
        HMODULE kernel32 = GetModuleHandle(TEXT("kernel32.dll"));
        CreateEventExFunc = NULL;
        if (kernel32) {
            CreateEventExFunc = (PFN_CREATE_EVENT_EX)GetProcAddress(kernel32, "CreateEventExW");
        }
    }
#endif
    if (!CreateEventExFunc) {
        result = E_FAIL;
        goto done;
    }

#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
    data->hDXGIMod = SDL_LoadObject("dxgi.dll");
    if (!data->hDXGIMod) {
        result = E_FAIL;
        goto done;
    }

    CreateDXGIFactoryFunc = (PFN_CREATE_DXGI_FACTORY)SDL_LoadFunction(data->hDXGIMod, "CreateDXGIFactory2");
    if (!CreateDXGIFactoryFunc) {
        result = E_FAIL;
        goto done;
    }

    data->hVULKANMod = SDL_LoadObject("VULKAN.dll");
    if (!data->hVULKANMod) {
        result = E_FAIL;
        goto done;
    }

    VULKANCreateDeviceFunc = (PFN_VULKAN_CREATE_DEVICE)SDL_LoadFunction(data->hVULKANMod, "VULKANCreateDevice");
    if (!VULKANCreateDeviceFunc) {
        result = E_FAIL;
        goto done;
    }

    if (createDebug) {
        PFN_VULKAN_GET_DEBUG_INTERFACE VULKANGetDebugInterfaceFunc;

        VULKANGetDebugInterfaceFunc = (PFN_VULKAN_GET_DEBUG_INTERFACE)SDL_LoadFunction(data->hVULKANMod, "VULKANGetDebugInterface");
        if (!VULKANGetDebugInterfaceFunc) {
            result = E_FAIL;
            goto done;
        }
        if (SUCCEEDED(VULKANGetDebugInterfaceFunc(D3D_GUID(SDL_IID_IVULKANDebug), (void **)&data->debugInterface))) {
            D3D_CALL(data->debugInterface, EnableDebugLayer);
        }
    }
#endif /*!defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)*/

#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
    result = VULKAN_XBOX_CreateDevice(&d3dDevice, createDebug);
    if (FAILED(result)) {
        /* SDL Error is set by VULKAN_XBOX_CreateDevice */
        goto done;
    }
#else
    if (createDebug) {
#ifdef __IDXGIInfoQueue_INTERFACE_DEFINED__
        IDXGIInfoQueue *dxgiInfoQueue = NULL;
        PFN_CREATE_DXGI_FACTORY DXGIGetDebugInterfaceFunc;

        /* If the debug hint is set, also create the DXGI factory in debug mode */
        DXGIGetDebugInterfaceFunc = (PFN_CREATE_DXGI_FACTORY)SDL_LoadFunction(data->hDXGIMod, "DXGIGetDebugInterface1");
        if (!DXGIGetDebugInterfaceFunc) {
            result = E_FAIL;
            goto done;
        }

        result = DXGIGetDebugInterfaceFunc(0, D3D_GUID(SDL_IID_IDXGIDebug1), (void **)&data->dxgiDebug);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("DXGIGetDebugInterface1"), result);
            goto done;
        }

        result = DXGIGetDebugInterfaceFunc(0, D3D_GUID(SDL_IID_IDXGIInfoQueue), (void **)&dxgiInfoQueue);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("DXGIGetDebugInterface1"), result);
            goto done;
        }

        D3D_CALL(dxgiInfoQueue, SetBreakOnSeverity, SDL_DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE);
        D3D_CALL(dxgiInfoQueue, SetBreakOnSeverity, SDL_DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        SAFE_RELEASE(dxgiInfoQueue);
#endif /* __IDXGIInfoQueue_INTERFACE_DEFINED__ */
        creationFlags = DXGI_CREATE_FACTORY_DEBUG;
    }

    result = CreateDXGIFactoryFunc(creationFlags, D3D_GUID(SDL_IID_IDXGIFactory6), (void **)&data->dxgiFactory);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("CreateDXGIFactory"), result);
        goto done;
    }

    /* Prefer a high performance adapter if there are multiple choices */
    result = D3D_CALL(data->dxgiFactory, EnumAdapterByGpuPreference,
                      0,
                      DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                      D3D_GUID(SDL_IID_IDXGIAdapter4),
                      (void **)&data->dxgiAdapter);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("VULKANCreateDevice"), result);
        goto done;
    }

    result = VULKANCreateDeviceFunc((IUnknown *)data->dxgiAdapter,
                                   D3D_FEATURE_LEVEL_11_0, /* Request minimum feature level 11.0 for maximum compatibility */
                                   D3D_GUID(SDL_IID_IVULKANDevice1),
                                   (void **)&d3dDevice);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("VULKANCreateDevice"), result);
        goto done;
    }

    /* Setup the info queue if in debug mode */
    if (createDebug) {
        IVULKANInfoQueue *infoQueue = NULL;
        VULKAN_MESSAGE_SEVERITY severities[] = { VULKAN_MESSAGE_SEVERITY_INFO };
        VULKAN_INFO_QUEUE_FILTER filter;

        result = D3D_CALL(d3dDevice, QueryInterface, D3D_GUID(SDL_IID_IVULKANInfoQueue), (void **)&infoQueue);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice to IVULKANInfoQueue"), result);
            goto done;
        }

        SDL_zero(filter);
        filter.DenyList.NumSeverities = 1;
        filter.DenyList.pSeverityList = severities;
        D3D_CALL(infoQueue, PushStorageFilter, &filter);

        D3D_CALL(infoQueue, SetBreakOnSeverity, VULKAN_MESSAGE_SEVERITY_ERROR, TRUE);
        D3D_CALL(infoQueue, SetBreakOnSeverity, VULKAN_MESSAGE_SEVERITY_CORRUPTION, TRUE);

        SAFE_RELEASE(infoQueue);
    }
#endif /*!defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)*/

    result = D3D_CALL(d3dDevice, QueryInterface, D3D_GUID(SDL_IID_IVULKANDevice1), (void **)&data->d3dDevice);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice to IVULKANDevice1"), result);
        goto done;
    }

    /* Create a command queue */
    SDL_zero(queueDesc);
    queueDesc.Flags = VULKAN_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = VULKAN_COMMAND_LIST_TYPE_DIRECT;

    result = D3D_CALL(data->d3dDevice, CreateCommandQueue,
                      &queueDesc,
                      D3D_GUID(SDL_IID_IVULKANCommandQueue),
                      (void **)&data->commandQueue);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommandQueue"), result);
        goto done;
    }

    /* Create the descriptor heaps for the render target view, texture SRVs, and samplers */
    SDL_zero(descriptorHeapDesc);
    descriptorHeapDesc.NumDescriptors = SDL_VULKAN_NUM_BUFFERS;
    descriptorHeapDesc.Type = VULKAN_DESCRIPTOR_HEAP_TYPE_RTV;
    result = D3D_CALL(data->d3dDevice, CreateDescriptorHeap,
                      &descriptorHeapDesc,
                      D3D_GUID(SDL_IID_IVULKANDescriptorHeap),
                      (void **)&data->rtvDescriptorHeap);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateDescriptorHeap [rtv]"), result);
        goto done;
    }
    data->rtvDescriptorSize = D3D_CALL(d3dDevice, GetDescriptorHandleIncrementSize, VULKAN_DESCRIPTOR_HEAP_TYPE_RTV);

    descriptorHeapDesc.NumDescriptors = SDL_VULKAN_MAX_NUM_TEXTURES;
    result = D3D_CALL(data->d3dDevice, CreateDescriptorHeap,
                      &descriptorHeapDesc,
                      D3D_GUID(SDL_IID_IVULKANDescriptorHeap),
                      (void **)&data->textureRTVDescriptorHeap);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateDescriptorHeap [texture rtv]"), result);
        goto done;
    }

    SDL_zero(descriptorHeapDesc);
    descriptorHeapDesc.NumDescriptors = SDL_VULKAN_MAX_NUM_TEXTURES;
    descriptorHeapDesc.Type = VULKAN_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.Flags = VULKAN_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = D3D_CALL(data->d3dDevice, CreateDescriptorHeap,
                      &descriptorHeapDesc,
                      D3D_GUID(SDL_IID_IVULKANDescriptorHeap),
                      (void **)&data->srvDescriptorHeap);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateDescriptorHeap  [srv]"), result);
        goto done;
    }
    rootDescriptorHeaps[0] = data->srvDescriptorHeap;
    data->srvDescriptorSize = D3D_CALL(d3dDevice, GetDescriptorHandleIncrementSize, VULKAN_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    SDL_zero(descriptorHeapDesc);
    descriptorHeapDesc.NumDescriptors = 2;
    descriptorHeapDesc.Type = VULKAN_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    descriptorHeapDesc.Flags = VULKAN_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = D3D_CALL(data->d3dDevice, CreateDescriptorHeap,
                      &descriptorHeapDesc,
                      D3D_GUID(SDL_IID_IVULKANDescriptorHeap),
                      (void **)&data->samplerDescriptorHeap);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateDescriptorHeap  [sampler]"), result);
        goto done;
    }
    rootDescriptorHeaps[1] = data->samplerDescriptorHeap;
    data->samplerDescriptorSize = D3D_CALL(d3dDevice, GetDescriptorHandleIncrementSize, VULKAN_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    /* Create a command allocator for each back buffer */
    for (i = 0; i < SDL_VULKAN_NUM_BUFFERS; ++i) {
        result = D3D_CALL(data->d3dDevice, CreateCommandAllocator,
                          VULKAN_COMMAND_LIST_TYPE_DIRECT,
                          D3D_GUID(SDL_IID_IVULKANCommandAllocator),
                          (void **)&data->commandAllocators[i]);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommandAllocator"), result);
            goto done;
        }
    }

    /* Create the command list */
    result = D3D_CALL(data->d3dDevice, CreateCommandList,
                      0,
                      VULKAN_COMMAND_LIST_TYPE_DIRECT,
                      data->commandAllocators[0],
                      NULL,
                      D3D_GUID(SDL_IID_IVULKANGraphicsCommandList2),
                      (void **)&data->commandList);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateCommandList"), result);
        goto done;
    }

    /* Set the descriptor heaps to the correct initial value */
    D3D_CALL(data->commandList, SetDescriptorHeaps, 2, rootDescriptorHeaps);

    /* Create the fence and fence event */
    result = D3D_CALL(data->d3dDevice, CreateFence,
                      data->fenceValue,
                      VULKAN_FENCE_FLAG_NONE,
                      D3D_GUID(SDL_IID_IVULKANFence),
                      (void **)&data->fence);
    if (FAILED(result)) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateFence"), result);
        goto done;
    }

    data->fenceValue++;

    data->fenceEvent = CreateEventExFunc(NULL, NULL, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!data->fenceEvent) {
        WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("CreateEventEx"), result);
        goto done;
    }

    /* Create all the root signatures */
    for (i = 0; i < NUM_ROOTSIGS; ++i) {
        VULKAN_SHADER_BYTECODE rootSigData;
        VULKAN_GetRootSignatureData((VULKAN_RootSignature)i, &rootSigData);
        result = D3D_CALL(data->d3dDevice, CreateRootSignature,
                          0,
                          rootSigData.pShaderBytecode,
                          rootSigData.BytecodeLength,
                          D3D_GUID(SDL_IID_IVULKANRootSignature),
                          (void **)&data->rootSignatures[i]);
        if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IVULKANDevice::CreateRootSignature"), result);
            goto done;
        }
    }

    /* Create all the default pipeline state objects
       (will add everything except custom blend states) */
    for (i = 0; i < NUM_SHADERS; ++i) {
        for (j = 0; j < SDL_arraysize(defaultBlendModes); ++j) {
            for (k = VULKAN_PRIMITIVE_TOPOLOGY_TYPE_POINT; k < VULKAN_PRIMITIVE_TOPOLOGY_TYPE_PATCH; ++k) {
                for (l = 0; l < SDL_arraysize(defaultRTVFormats); ++l) {
                    if (!VULKAN_CreatePipelineState(renderer, (VULKAN_Shader)i, defaultBlendModes[j], (VULKAN_PRIMITIVE_TOPOLOGY_TYPE)k, defaultRTVFormats[l])) {
                        /* VULKAN_CreatePipelineState will set the SDL error, if it fails */
                        goto done;
                    }
                }
            }
        }
    }

    /* Create default vertex buffers  */
    for (i = 0; i < SDL_VULKAN_NUM_VERTEX_BUFFERS; ++i) {
        VULKAN_CreateVertexBuffer(data, i, VULKAN_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    }

    /* Create samplers to use when drawing textures: */
    SDL_zero(samplerDesc);
    samplerDesc.Filter = VULKAN_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = VULKAN_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = VULKAN_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = VULKAN_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = VULKAN_COMPARISON_FUNC_ALWAYS;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = VULKAN_FLOAT32_MAX;
    D3D_CALL_RET(data->samplerDescriptorHeap, GetCPUDescriptorHandleForHeapStart, &data->nearestPixelSampler);
    D3D_CALL(data->d3dDevice, CreateSampler, &samplerDesc, data->nearestPixelSampler);

    samplerDesc.Filter = VULKAN_FILTER_MIN_MAG_MIP_LINEAR;
    data->linearSampler.ptr = data->nearestPixelSampler.ptr + data->samplerDescriptorSize;
    D3D_CALL(data->d3dDevice, CreateSampler, &samplerDesc, data->linearSampler);

    /* Initialize the pool allocator for SRVs */
    for (i = 0; i < SDL_VULKAN_MAX_NUM_TEXTURES; ++i) {
        data->srvPoolNodes[i].index = (SIZE_T)i;
        if (i != SDL_VULKAN_MAX_NUM_TEXTURES - 1) {
            data->srvPoolNodes[i].next = &data->srvPoolNodes[i + 1];
        }
    }
    data->srvPoolHead = &data->srvPoolNodes[0];

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    SDL_SetProperty(props, SDL_PROP_RENDERER_VULKAN_DEVICE_POINTER, data->d3dDevice);
    SDL_SetProperty(props, SDL_PROP_RENDERER_VULKAN_COMMAND_QUEUE_POINTER, data->commandQueue);

done:
    SAFE_RELEASE(d3dDevice);
    return result;
#else
    return VK_SUCCESS;
#endif
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
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    IDXGISwapChain1 *swapChain = NULL;
    HRESULT result = S_OK;

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
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
    HRESULT result = S_OK;
    int i, w, h;

    VULKAN_RENDER_TARGET_VIEW_DESC rtvDesc;
    VULKAN_CPU_DESCRIPTOR_HANDLE rtvDescriptor;

    /* Release resources in the current command list */
    VULKAN_IssueBatch(data);
    D3D_CALL(data->commandList, OMSetRenderTargets, 0, NULL, FALSE, NULL);

    /* Release render targets */
    for (i = 0; i < SDL_VULKAN_NUM_BUFFERS; ++i) {
        SAFE_RELEASE(data->renderTargets[i]);
    }

    /* The width and height of the swap chain must be based on the display's
     * non-rotated size.
     */
    SDL_GetWindowSizeInPixels(renderer->window, &w, &h);
    data->rotation = VULKAN_GetCurrentRotation();
    if (VULKAN_IsDisplayRotated90Degrees(data->rotation)) {
        int tmp = w;
        w = h;
        h = tmp;
    }

#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
    if (data->swapChain) {
        /* If the swap chain already exists, resize it. */
        result = D3D_CALL(data->swapChain, ResizeBuffers,
                          0,
                          w, h,
                          DXGI_FORMAT_UNKNOWN,
                          data->swapFlags);
        if (result == DXGI_ERROR_DEVICE_REMOVED) {
            /* If the device was removed for any reason, a new device and swap chain will need to be created. */
            VULKAN_HandleDeviceLost(renderer);

            /* Everything is set up now. Do not continue execution of this method. HandleDeviceLost will reenter this method
             * and correctly set up the new device.
             */
            goto done;
        } else if (FAILED(result)) {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain::ResizeBuffers"), result);
            goto done;
        }
    } else {
        result = VULKAN_CreateSwapChain(renderer, w, h);
        if (FAILED(result) || !data->swapChain) {
            goto done;
        }
    }

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
#endif /*!defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)*/

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
        verts->pos.x = points[i].x + 0.5f;
        verts->pos.y = points[i].y + 0.5f;
        verts->tex.x = 0.0f;
        verts->tex.y = 0.0f;
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

        verts->pos.x = xy_[0] * scale_x;
        verts->pos.y = xy_[1] * scale_y;
        verts->color = *(SDL_FColor *)((char *)color + j * color_stride);

        if (texture) {
            float *uv_ = (float *)((char *)uv + j * uv_stride);
            verts->tex.x = uv_[0];
            verts->tex.y = uv_[1];
        } else {
            verts->tex.x = 0.0f;
            verts->tex.y = 0.0f;
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

static int VULKAN_SetCopyState(SDL_Renderer *renderer, const SDL_RenderCommand *cmd, const Float4X4 *matrix)
{
#if D3D12_PORT
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
#endif
}

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
#if D3D12_PORT
    VULKAN_RenderData *rendererData = (VULKAN_RenderData *)renderer->driverdata;
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
            VULKAN_CPU_DESCRIPTOR_HANDLE rtvDescriptor = VULKAN_GetCurrentRenderTargetView(renderer);
            D3D_CALL(rendererData->commandList, ClearRenderTargetView, rtvDescriptor, &cmd->data.color.color.r, 0, NULL);
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
        {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const size_t start = first / sizeof(VertexPositionColor);
            VULKAN_SetDrawState(renderer, cmd, SHADER_SOLID, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_POINT, 0, NULL, NULL, NULL);
            VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, start, count);
            break;
        }

        case SDL_RENDERCMD_DRAW_LINES:
        {
            const size_t count = cmd->data.draw.count;
            const size_t first = cmd->data.draw.first;
            const size_t start = first / sizeof(VertexPositionColor);
            const VertexPositionColor *verts = (VertexPositionColor *)(((Uint8 *)vertices) + first);
            VULKAN_SetDrawState(renderer, cmd, SHADER_SOLID, VULKAN_PRIMITIVE_TOPOLOGY_TYPE_LINE, 0, NULL, NULL, NULL);
            VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, start, count);
            if (verts[0].pos.x != verts[count - 1].pos.x || verts[0].pos.y != verts[count - 1].pos.y) {
                VULKAN_DrawPrimitives(renderer, D3D_PRIMITIVE_TOPOLOGY_POINTLIST, start + (count - 1), 1);
            }
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
            break;
        }

        case SDL_RENDERCMD_NO_OP:
            break;
        }

        cmd = cmd->next;
    }
#endif
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
#if D3D12_PORT
    VULKAN_RenderData *data = (VULKAN_RenderData *)renderer->driverdata;
#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
    UINT syncInterval;
    UINT presentFlags;
#endif
    HRESULT result;

    /* Transition the render target to present state */
    VULKAN_TransitionResource(data,
                             data->renderTargets[data->currentBackBufferIndex],
                             VULKAN_RESOURCE_STATE_RENDER_TARGET,
                             VULKAN_RESOURCE_STATE_PRESENT);

    /* Issue the command list */
    result = D3D_CALL(data->commandList, Close);
    D3D_CALL(data->commandQueue, ExecuteCommandLists, 1, (IVULKANCommandList *const *)&data->commandList);

#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
    result = VULKAN_XBOX_PresentFrame(data->commandQueue, data->frameToken, data->renderTargets[data->currentBackBufferIndex]);
#else
    if (renderer->info.flags & SDL_RENDERER_PRESENTVSYNC) {
        syncInterval = 1;
        presentFlags = 0;
    } else {
        syncInterval = 0;
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    }

    /* The application may optionally specify "dirty" or "scroll"
     * rects to improve efficiency in certain scenarios.
     */
    result = D3D_CALL(data->swapChain, Present, syncInterval, presentFlags);
#endif

    if (FAILED(result) && result != DXGI_ERROR_WAS_STILL_DRAWING) {
        /* If the device was removed either by a disconnect or a driver upgrade, we
         * must recreate all device resources.
         */
        if (result == DXGI_ERROR_DEVICE_REMOVED) {
            VULKAN_HandleDeviceLost(renderer);
        } else if (result == DXGI_ERROR_INVALID_CALL) {
            /* We probably went through a fullscreen <-> windowed transition */
            VULKAN_CreateWindowSizeDependentResources(renderer);
        } else {
            WIN_SetErrorFromHRESULT(SDL_COMPOSE_ERROR("IDXGISwapChain::Present"), result);
        }
        return -1;
    } else {
        /* Wait for the GPU and move to the next frame */
        result = D3D_CALL(data->commandQueue, Signal, data->fence, data->fenceValue);

        if (D3D_CALL(data->fence, GetCompletedValue) < data->fenceValue) {
            result = D3D_CALL(data->fence, SetEventOnCompletion,
                              data->fenceValue,
                              data->fenceEvent);
            WaitForSingleObjectEx(data->fenceEvent, INFINITE, FALSE);
        }

        data->fenceValue++;
#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
        data->currentBackBufferIndex++;
        data->currentBackBufferIndex %= SDL_VULKAN_NUM_BUFFERS;
#else
        data->currentBackBufferIndex = D3D_CALL(data->swapChain, GetCurrentBackBufferIndex);
#endif

        /* Reset the command allocator and command list, and transition back to render target */
        VULKAN_ResetCommandList(data);
        VULKAN_TransitionResource(data,
                                 data->renderTargets[data->currentBackBufferIndex],
                                 VULKAN_RESOURCE_STATE_PRESENT,
                                 VULKAN_RESOURCE_STATE_RENDER_TARGET);

#if defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
        VULKAN_XBOX_StartFrame(data->d3dDevice, &data->frameToken);
#endif
        return 0;
    }
#else
    return 0;
#endif
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
