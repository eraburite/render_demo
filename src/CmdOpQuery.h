#pragma once
#include <memory>
#include <stdexcept>
#include <vulkan/vulkan.hpp>

#include "CmdOpBase.h"
#include "QueryPool.h"

// Records one query-pool operation into the current frame's command buffer.
// Three phases are separate ops because vkCmdResetQueryPool must be recorded
// *outside* a render pass while begin/end must wrap draws *inside* it; the
// example places them accordingly in the frame body.
class CmdOpQuery : public CmdOpBase {
public:
    enum class Phase { eReset, eBegin, eEnd };

    CmdOpQuery(std::shared_ptr<QueryPool> pool, Phase phase, uint32_t query = 0,
               vk::QueryControlFlags flags = {}) :
        m_pool(std::move(pool)), m_phase(phase), m_query(query), m_flags(flags)
    {
        if (!m_pool) {
            throw std::runtime_error("CmdOpQuery requires a query pool");
        }
        if (m_phase != Phase::eReset && m_query >= m_pool->queryCount()) {
            throw std::runtime_error("CmdOpQuery: query index out of range");
        }
    }

    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eStatic; }

    void Record(const vk::CommandBuffer &cmd, uint32_t, DescriptorBindingState &) override
    {
        switch (m_phase) {
            case Phase::eReset:
                // Reset all slots at once, before the render pass starts.
                cmd.resetQueryPool(m_pool->handle(), 0, m_pool->queryCount());
                break;
            case Phase::eBegin:
                // Pipeline-statistics queries do not accept control flags.
                cmd.beginQuery(m_pool->handle(), m_query,
                               m_pool->kind() == QueryPool::Kind::eOcclusion ? m_flags
                                                                              : vk::QueryControlFlags{});
                break;
            case Phase::eEnd:
                cmd.endQuery(m_pool->handle(), m_query);
                break;
        }
    }

private:
    std::shared_ptr<QueryPool> m_pool;
    Phase m_phase{};
    uint32_t m_query = 0;
    vk::QueryControlFlags m_flags{};
};
