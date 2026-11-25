#pragma once
#include <vector>

#include "CmdOpBase.h"
#include "CmdOpStencilState.h"
#include "GraphicsPipeline.h"

// Draw operation: binds one graphics pipeline and issues one draw.  Render pass
// begin/end is owned by LoopSequence::recordPass, not by the op.
class CmdOpDrawFrame : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;
    explicit CmdOpDrawFrame(std::shared_ptr<GraphicsPipeline> pipeline) : m_pipeline(std::move(pipeline)) {}

    // Attach dynamic stencil state to this draw.  The state is recorded through
    // the pipeline's after-bind hook, so it always lands *after*
    // vkCmdBindPipeline -- the only place where a dynamic stencil value is
    // guaranteed to survive for the draw that follows.  Keeping the state on the
    // draw also means a CmdOpGroup cannot accidentally reorder it away.
    void setStencilState(std::shared_ptr<CmdOpStencilState> state)
    {
        m_stencilState = std::move(state);
        if (m_pipeline && m_stencilState) {
            std::shared_ptr<CmdOpStencilState> captured = m_stencilState;
            m_pipeline->setAfterBindHook(
                [captured](const vk::CommandBuffer &cmd) { captured->apply(cmd); });
        }
    }
    const std::shared_ptr<CmdOpStencilState> &stencilState() const { return m_stencilState; }

    // Draw several index windows with a single pipeline bind.  A model is one
    // vertex/index buffer sliced into per-primitive windows, so this is what
    // keeps a 79-primitive file at one bind instead of 79.  An empty list keeps
    // the plain "draw everything bound" behaviour.
    void setDrawRanges(std::vector<DrawRange> ranges) { m_ranges = std::move(ranges); }
    const std::vector<DrawRange> &drawRanges() const { return m_ranges; }

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
        if (m_ranges.empty()) {
            m_pipeline->recordDrawCommand(cmd, frame, state);
        } else {
            m_pipeline->recordDrawCommand(cmd, frame, state, m_ranges);
        }
    }

private:
    std::shared_ptr<GraphicsPipeline> m_pipeline;
    std::shared_ptr<CmdOpStencilState> m_stencilState;
    std::vector<DrawRange> m_ranges;
};
