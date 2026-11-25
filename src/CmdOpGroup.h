#pragma once
#include "CmdOpBase.h"
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

// Records a list of operations into one command buffer, in insertion order.
//
// A multi-stage compute chain is usually one submission, not one per dispatch:
// the stages then share a single fence and a single vkQueueSubmit instead of
// paying a round trip (and a blocking wait) per stage.  Execution order comes
// for free, but memory visibility between stages does not -- an SSBO written by
// dispatch A is only visible to dispatch B after a barrier, so insert a
// CmdOpComputeBarrier between them.
class CmdOpGroup : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;

    CmdOpGroup() = default;
    explicit CmdOpGroup(std::vector<std::shared_ptr<CmdOpBase>> ops) : m_ops(std::move(ops)) {}

    void add(std::shared_ptr<CmdOpBase> op)
    {
        if (op) {
            m_ops.push_back(std::move(op));
        }
    }

    void clear() { m_ops.clear(); }
    size_t size() const { return m_ops.size(); }

    // Data updates are forwarded, so the caller keeps one handle for the chain.
    void OnUpdateRawData(ResourceId id, uint32_t frame, const void *data, size_t size) override
    {
        for (const std::shared_ptr<CmdOpBase> &op : m_ops) {
            op->OnUpdateRawData(id, frame, data, size);
        }
    }
    void OnUpdateRawData(ResourceTypes type, uint32_t frame, const void *data, size_t size) override
    {
        for (const std::shared_ptr<CmdOpBase> &op : m_ops) {
            op->OnUpdateRawData(type, frame, data, size);
        }
    }
    void OnUpdateRawData(ResourceTypes type, const void *data, size_t size) override
    {
        for (const std::shared_ptr<CmdOpBase> &op : m_ops) {
            op->OnUpdateRawData(type, data, size);
        }
    }

    // A group containing any per-frame operation is itself per-frame.
    UpdateFrequency GetFrequency() const override
    {
        for (const std::shared_ptr<CmdOpBase> &op : m_ops) {
            if (op->GetFrequency() == UpdateFrequency::eEachFrame) {
                return UpdateFrequency::eEachFrame;
            }
        }
        return UpdateFrequency::eStatic;
    }

    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) override
    {
        for (const std::shared_ptr<CmdOpBase> &op : m_ops) {
            op->Record(cmd, frame, state);
        }
    }

private:
    std::vector<std::shared_ptr<CmdOpBase>> m_ops;
};
