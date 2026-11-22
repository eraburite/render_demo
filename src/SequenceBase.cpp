#include "SequenceBase.h"

#include "RenderManager.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

constexpr uint64_t kAvailabilityBit = uint64_t{1} << 63;

} // namespace

SequenceBase::SequenceBase(std::shared_ptr<RenderManager> renderMgr) : m_renderMgr(std::move(renderMgr))
{
    if (!m_renderMgr) {
        throw std::runtime_error("SequenceBase requires a RenderManager");
    }
    m_device = m_renderMgr->getLogicalDevice();
    m_graphicsQueue = m_renderMgr->getGraphicsQueue();
    m_presentQueue = m_renderMgr->getPresentQueue();
    m_graphicsFamily = m_renderMgr->getQueueFamilyIndices().graphicsFamily.value_or(0);
    m_deviceProperties = m_renderMgr->deviceProperties();
}

std::shared_ptr<vk::QueryPool> SequenceBase::CreateTimestampQueryPool(uint32_t queryCount)
{
    // Timestamp queries are optional: callers treat a null pool as "disabled".
    if (!m_device || !m_deviceProperties.limits.timestampComputeAndGraphics) {
        return nullptr;
    }

    vk::QueryPoolCreateInfo createInfo{};
    createInfo.queryType = vk::QueryType::eTimestamp;
    createInfo.queryCount = queryCount;

    vk::QueryPool pool{};
    check(m_device->createQueryPool(&createInfo, nullptr, &pool), "vkCreateQueryPool");

    auto result = std::shared_ptr<vk::QueryPool>(new vk::QueryPool(pool), [device = m_device](vk::QueryPool *handle) {
        if (handle != nullptr && *handle) {
            device->destroyQueryPool(*handle);
        }
        delete handle;
    });
    m_timestampPools.push_back(result);
    return result;
}

void SequenceBase::LogExecutionTime(uint32_t frameIndex, uint32_t queryPoolIndex)
{
    if (queryPoolIndex >= m_timestampPools.size() || !m_timestampPools[queryPoolIndex]) {
        return;
    }

    // With eWithAvailability each result occupies 16 bytes (timestamp + availability).
    std::array<uint64_t, 4> results{};
    const vk::Result result =
        m_device->getQueryPoolResults(*m_timestampPools[queryPoolIndex], 0, 2, sizeof(results), results.data(),
                                      2 * sizeof(uint64_t),
                                      vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess) {
        // VK_NOT_READY (or eTimeout) simply means the GPU has not finished the
        // sampled frame yet; skip instead of surfacing a validation error.
        return;
    }
    // results[0]=timestamp[0], results[1]=availability[0], results[2]=timestamp[1], results[3]=availability[1]
    if (results[1] == 0 || results[3] == 0) {
        return;
    }

    const uint64_t begin = results[0];
    const uint64_t end = results[2];
    if (end < begin) {
        return;
    }

    const double durationMs = static_cast<double>(end - begin)
                              * static_cast<double>(m_deviceProperties.limits.timestampPeriod) / 1e6;
    std::cout << "frame " << frameIndex << " GPU time: " << durationMs << " ms\n";
}
