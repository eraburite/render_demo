#pragma once
#include "CmdOpBase.h"
#include "GraphicsPipeline.h"

// Draw operation: binds one graphics pipeline and issues one draw.  Render pass
// begin/end is owned by LoopSequence::recordPass, not by the op.
class CmdOpDrawFrame : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;
    explicit CmdOpDrawFrame(std::shared_ptr<GraphicsPipeline> pipeline) : m_pipeline(std::move(pipeline)) {}

    void OnUpdateRawData(ResourceId id, uint32_t frame, const void *data, size_t size) override
    {
        m_pipeline->asset()->UpdateDescriptorResourceData(ResourceKey::make_key(id, frame), data, size);
    }
    void OnUpdateRawData(ResourceTypes, uint32_t frame, const void *data, size_t size) override
    {
        m_pipeline->asset()->UpdateConstantsResource(frame, data, size);
    }
    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eEachFrame; }
    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) override
    {
        m_pipeline->recordDrawCommand(cmd, frame, state);
    }

private:
    std::shared_ptr<GraphicsPipeline> m_pipeline;
};
