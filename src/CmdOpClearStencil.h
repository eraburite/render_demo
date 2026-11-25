#pragma once
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include "CmdOpBase.h"

// Clears only the S8 plane of the current depth-stencil attachment, from
// inside an open render pass (vkCmdClearAttachments).  This is the command-level
// counterpart of the render-pass-level stencilLoadOp: it lets one pass reuse the
// stencil buffer across several draws without ending and restarting the pass.
//
// The rect must stay inside the framebuffer, so the caller updates it with the
// current swapchain extent every frame (resize changes the framebuffer).
class CmdOpClearStencil : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;

    explicit CmdOpClearStencil(vk::Extent2D extent, uint32_t value = 0) : m_extent(extent), m_value(value) {}

    void setExtent(vk::Extent2D extent) { m_extent = extent; }
    void setValue(uint32_t value) { m_value = value; }
    uint32_t value() const { return m_value; }

    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eEachFrame; }

    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) override
    {
        (void)frame;
        (void)state;
        const vk::ClearValue clear(vk::ClearDepthStencilValue(0.0f, m_value));
        const vk::ClearAttachment attachment(vk::ImageAspectFlagBits::eStencil, 0, clear);
        const vk::ClearRect rect(vk::Rect2D(vk::Offset2D(0, 0), m_extent), 0, 1);
        cmd.clearAttachments(1, &attachment, 1, &rect);
    }

private:
    vk::Extent2D m_extent;
    uint32_t m_value = 0;
};
