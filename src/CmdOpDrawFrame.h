#pragma once
#include "CmdOpBase.h"
#include "GraphicsPipeline.h"
#include "typedefs.h"
#include "SwapChain.h"

class CmdOpDrawFrame : public CmdOpBase {
public:
    CmdOpDrawFrame(const std::shared_ptr<GraphicsPipeline> &pipeline)
    {
        m_pipeline = pipeline;
    }

    void OnUpdateRawData(ResourceTypes resourceType, const void *data, size_t size) override
    {
        // non-op
    }

    void OnUpdateRawData(ResourceId resourceId, uint32_t currentFrame, const void *data, size_t size) override
    {
        m_pipeline->UploadDescriptorResourceData(resourceId, currentFrame, data, size);
    }

    void OnUpdateRawData(ResourceTypes resourceType, uint32_t currentFrame, const void *data, size_t size) override
    {
        m_pipeline->UploadConstantsResource(resourceType, currentFrame, data, size);
    }

    UpdateFrequency GetFrequency() const override
    {
        return UpdateFrequency::eEachFrame;
    }

    void BindFrameBuffer(const std::shared_ptr<vk::Framebuffer> &framebuffer,
                         const std::shared_ptr<vk::Extent2D> &swapChainExtent) override
    {
        m_framebuffer = framebuffer;
        m_swapChainExtent = swapChainExtent;
    }

    void Record(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame) override
    {
        // m_pipeline->recordUploadCommand(commandBuffer, currentFrame);
        m_pipeline->BeginRenderPass(commandBuffer, m_framebuffer, m_swapChainExtent);
        m_pipeline->recordDrawCommand(commandBuffer, currentFrame);
        m_pipeline->EndRenderPass(commandBuffer);
    }

private:
    std::shared_ptr<GraphicsPipeline> m_pipeline;
    std::shared_ptr<vk::Framebuffer> m_framebuffer;
    std::shared_ptr<vk::Extent2D> m_swapChainExtent;
};