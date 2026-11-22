#pragma once
#include <cstdint>
#include <vulkan/vulkan.hpp>

// Tracks which descriptor sets are bound in the current command buffer under
// the currently-bound pipeline layout.  Enables "bind once" incremental
// binding: consecutive draws with the same layout only bind missing sets.
class DescriptorBindingState {
public:
    void Reset()
    {
        m_layout = vk::PipelineLayout{};
        m_boundMask = 0;
    }

    void OnPipelineLayoutBound(vk::PipelineLayout layout)
    {
        if (layout != m_layout) {
            m_layout = layout;
            m_boundMask = 0;
        }
    }

    uint32_t FirstUnboundSet(uint32_t setCount) const
    {
        for (uint32_t i = 0; i < setCount && i < 32; ++i) {
            if ((m_boundMask & (uint32_t{1} << i)) == 0) {
                return i;
            }
        }
        return setCount;
    }

    void MarkBoundRange(uint32_t firstSet, uint32_t count)
    {
        for (uint32_t i = firstSet; i < firstSet + count && i < 32; ++i) {
            m_boundMask |= (uint32_t{1} << i);
        }
    }

private:
    vk::PipelineLayout m_layout{};
    uint32_t m_boundMask = 0;
};
