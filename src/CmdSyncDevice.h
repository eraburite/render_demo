#pragma once
#include "CmdOpBase.h"
#include "MeshBuffer.h"
#include "GraphicsPipeline.h"
#include "typedefs.h"
#include <mutex>
#include <unordered_map>

class CmdOpUpload : public CmdOpBase {
public:
    CmdOpUpload(const std::shared_ptr<GraphicsPipeline> &pipeline)
    {
        m_pipeline = pipeline;
    }

    void OnUpdateRawData(ResourceTypes resourceType, const void *data, size_t size) override
    {
        m_pipeline->UploadInputResourceData(resourceType, data, size);
    }

    void OnUpdateRawData(ResourceId resourceId, uint32_t currentFrame, const void *data, size_t size) override
    {
        m_pipeline->UploadDescriptorResourceData(resourceId, currentFrame, data, size);
    }

    void OnUpdateRawData(ResourceTypes resourceType, uint32_t currentFrame, const void *data, size_t size) override
    { // non-op
    }
    UpdateFrequency GetFrequency() const override
    {
        return UpdateFrequency::eStatic;
    }
    void BindFrameBuffer(const std::shared_ptr<vk::Framebuffer> &framebuffer,
                         const std::shared_ptr<vk::Extent2D> &swapChainExtent) override
    {
        // non-op
    }

    void Record(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame) override
    {
        m_pipeline->recordUploadCommand(commandBuffer, currentFrame);
    }

private:
    std::shared_ptr<GraphicsPipeline> m_pipeline;
};
