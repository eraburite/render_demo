#pragma once
#include "CmdOpBase.h"
#include "ComputePipeline.h"

class CmdOpComputeDispatch : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;
    CmdOpComputeDispatch(std::shared_ptr<ComputePipeline> pipeline, DispatchSize size) :
        m_pipeline(std::move(pipeline)), m_size(size)
    {
    }

    void OnUpdateRawData(ResourceId id, uint32_t frame, const void *data, size_t size) override
    {
        m_pipeline->asset()->UpdateDescriptorResourceData(ResourceKey::make_key(id, frame), data, size);
    }
    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eEachFrame; }
    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) override
    {
        m_pipeline->recordDispatchCommand(cmd, frame, m_size, state);
    }

private:
    std::shared_ptr<ComputePipeline> m_pipeline;
    DispatchSize m_size;
};
