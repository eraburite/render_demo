#include "QueryPool.h"

#include <stdexcept>
#include <string>

namespace {

uint32_t popCount(vk::QueryPipelineStatisticFlags flags)
{
    uint32_t bits = static_cast<uint32_t>(flags);
    uint32_t count = 0;
    while (bits != 0) {
        count += bits & 1u;
        bits >>= 1;
    }
    return count;
}

} // namespace

std::shared_ptr<QueryPool> QueryPool::Create(const std::shared_ptr<vk::Device> &device, Kind kind,
                                            uint32_t queryCount, vk::QueryPipelineStatisticFlags statistics)
{
    if (!device) {
        throw std::runtime_error("QueryPool::Create requires a logical device");
    }
    if (queryCount == 0) {
        throw std::runtime_error("QueryPool::Create requires at least one query");
    }

    vk::QueryPoolCreateInfo info{};
    info.queryCount = queryCount;
    switch (kind) {
        case Kind::eOcclusion:
            info.queryType = vk::QueryType::eOcclusion;
            break;
        case Kind::ePipelineStatistics:
            info.queryType = vk::QueryType::ePipelineStatistics;
            info.pipelineStatistics = statistics;
            if (static_cast<uint32_t>(statistics) == 0) {
                throw std::runtime_error("a pipeline-statistics query pool needs at least one statistic bit");
            }
            break;
    }

    vk::QueryPool raw{};
    const vk::Result result = device->createQueryPool(&info, nullptr, &raw);
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error("vkCreateQueryPool failed: " + vk::to_string(result));
    }

    // Same deleter pattern as SequenceBase::CreateTimestampQueryPool: the pool
    // must outlive the command buffers that reference it, so it is refcounted.
    auto pool = std::shared_ptr<vk::QueryPool>(new vk::QueryPool(raw), [device](vk::QueryPool *handle) {
        if (handle != nullptr && *handle) {
            device->destroyQueryPool(*handle);
        }
        delete handle;
    });

    const uint32_t counters = kind == Kind::eOcclusion ? 1u : popCount(statistics);
    return std::shared_ptr<QueryPool>(new QueryPool(device, kind, queryCount, counters, statistics, pool));
}

QueryPool::~QueryPool() = default;

std::optional<std::vector<uint64_t>> QueryPool::fetch(uint32_t firstQuery, uint32_t count) const
{
    if (count == 0 || firstQuery + count > m_queryCount) {
        return std::nullopt;
    }
    // WITH_AVAILABILITY appends one uint64 per query after its counters.
    const size_t stride = static_cast<size_t>(m_counterCount) + 1;
    std::vector<uint64_t> raw(static_cast<size_t>(count) * stride, 0);
    const vk::Result result = m_device->getQueryPoolResults(
        *m_pool, firstQuery, count, raw.size() * sizeof(uint64_t), raw.data(), stride * sizeof(uint64_t),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess) {
        // VK_NOT_READY / VK_TIMEOUT: the GPU has not finished this frame yet.
        return std::nullopt;
    }

    std::vector<uint64_t> values(static_cast<size_t>(count) * m_counterCount);
    for (uint32_t query = 0; query < count; ++query) {
        const uint64_t available = raw[static_cast<size_t>(query) * stride + m_counterCount];
        if (available == 0) {
            return std::nullopt;
        }
        for (uint32_t counter = 0; counter < m_counterCount; ++counter) {
            values[static_cast<size_t>(query) * m_counterCount + counter] =
                raw[static_cast<size_t>(query) * stride + counter];
        }
    }
    return values;
}

std::vector<uint64_t> QueryPool::fetchBlocking(uint32_t firstQuery, uint32_t count) const
{
    if (count == 0 || firstQuery + count > m_queryCount) {
        return {};
    }
    std::vector<uint64_t> values(static_cast<size_t>(count) * m_counterCount, 0);
    const vk::Result result = m_device->getQueryPoolResults(
        *m_pool, firstQuery, count, values.size() * sizeof(uint64_t), values.data(),
        static_cast<size_t>(m_counterCount) * sizeof(uint64_t),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error("vkGetQueryPoolResults(wait) failed: " + vk::to_string(result));
    }
    return values;
}
