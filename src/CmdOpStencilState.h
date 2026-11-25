#pragma once
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.hpp>
#include "CmdOpBase.h"

// Records the dynamic stencil state of the currently bound graphics pipeline.
//
// Vulkan keeps stencil reference / compare mask / write mask as *pipeline*
// state, but each value can be declared dynamic and then set per-draw with a
// command.  Binding another pipeline resets those dynamic values, so the state
// has to be re-recorded immediately before every draw that depends on it --
// exactly what a CmdOp placed inside recordPass() does.
//
// Only the values actually set are written, so one op can act as "set the
// reference" and another as "set the write mask" without clobbering each
// other.
class CmdOpStencilState : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;

    explicit CmdOpStencilState(vk::StencilFaceFlags faces = vk::StencilFaceFlagBits::eFrontAndBack) :
        m_faces(faces)
    {
    }

    void setReference(uint32_t value) { m_reference = value; }
    void setCompareMask(uint32_t value) { m_compareMask = value; }
    void setWriteMask(uint32_t value) { m_writeMask = value; }
    void clearReference() { m_reference.reset(); }
    void clearCompareMask() { m_compareMask.reset(); }
    void clearWriteMask() { m_writeMask.reset(); }

    uint32_t reference() const { return m_reference.value_or(0); }
    uint32_t writeMask() const { return m_writeMask.value_or(0); }
    vk::StencilFaceFlags faces() const { return m_faces; }

    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eEachFrame; }

    // Also callable directly from a pipeline's after-bind hook (see
    // CmdOpDrawFrame::setStencilState), which needs the commands to land after
    // vkCmdBindPipeline without creating a second CmdOp.
    void apply(const vk::CommandBuffer &cmd) const
    {
        if (m_reference) {
            cmd.setStencilReference(m_faces, *m_reference);
        }
        if (m_compareMask) {
            cmd.setStencilCompareMask(m_faces, *m_compareMask);
        }
        if (m_writeMask) {
            cmd.setStencilWriteMask(m_faces, *m_writeMask);
        }
    }

    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &state) override
    {
        (void)frame;
        (void)state;
        apply(cmd);
    }

private:
    vk::StencilFaceFlags m_faces;
    std::optional<uint32_t> m_reference;
    std::optional<uint32_t> m_compareMask;
    std::optional<uint32_t> m_writeMask;
};
