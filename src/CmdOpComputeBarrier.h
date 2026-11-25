#pragma once
#include "CmdOpBase.h"

// Compute-shader -> next-stage memory dependency.
//
// Two dispatches recorded back to back (in one command buffer or in adjacent
// submissions) are ordered for *execution*, but the SSBO writes of the first are
// not guaranteed visible to the second: that needs an explicit dependency with
// a shader-write source scope.  LoopSequence::recordComputeBarrier() does this
// for the frame path; this op does the same for a one-shot chain built with
// CmdOpGroup.
class CmdOpComputeBarrier : public CmdOpBase {
public:
    // Stage that consumes the data.  Dispatch -> dispatch keeps eComputeShader;
    // a draw reading the SSBO right after a dispatch needs eVertexShader and/or
    // eFragmentShader.
    explicit CmdOpComputeBarrier(vk::PipelineStageFlagBits nextStage = vk::PipelineStageFlagBits::eComputeShader) :
        m_nextStage(nextStage)
    {
    }

    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eStatic; }

    void Record(const vk::CommandBuffer &cmd, uint32_t, DescriptorBindingState &) override
    {
        vk::MemoryBarrier barrier{};
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        // Writes stay in the destination scope as well: a later stage may both
        // read this data and overwrite it (ping-pong buffers do exactly that).
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, m_nextStage, vk::DependencyFlags{}, 1,
                            &barrier, 0, nullptr, 0, nullptr);
    }

private:
    vk::PipelineStageFlagBits m_nextStage;
};
