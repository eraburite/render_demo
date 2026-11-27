#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>

// Generic query pool for the Vulkan-Samples "queries" family.  Occlusion
// queries return one counter per query, pipeline-statistics queries return one
// counter per enabled statistic bit (see counterCount()).
//
// Two read paths:
//   fetch()          - non-blocking, uses VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
//                      so a frame never stalls on the GPU; returns nullopt
//                      while any queried slot is still pending.
//   fetchBlocking()  - VK_QUERY_RESULT_WAIT_BIT, exact values at shutdown or
//                      when --stats needs a number for the log.
//
// The pool is owned by shared_ptr and destroys its VkQueryPool in the deleter,
// matching the rest of the framework's RAII handles.
class QueryPool {
public:
    enum class Kind { eOcclusion, ePipelineStatistics };

    static std::shared_ptr<QueryPool> Create(const std::shared_ptr<vk::Device> &device, Kind kind,
                                             uint32_t queryCount,
                                             vk::QueryPipelineStatisticFlags statistics = {});

    ~QueryPool();

    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] uint32_t queryCount() const { return m_queryCount; }
    [[nodiscard]] uint32_t counterCount() const { return m_counterCount; }
    [[nodiscard]] vk::QueryPipelineStatisticFlags statistics() const { return m_statistics; }
    [[nodiscard]] vk::QueryPool handle() const { return *m_pool; }

    // Results for [firstQuery, firstQuery + count), flattened as
    // query0.counter0..counterN, query1.counter0.., ...
    [[nodiscard]] std::optional<std::vector<uint64_t>> fetch(uint32_t firstQuery, uint32_t count) const;
    [[nodiscard]] std::vector<uint64_t> fetchBlocking(uint32_t firstQuery, uint32_t count) const;

private:
    QueryPool(std::shared_ptr<vk::Device> device, Kind kind, uint32_t queryCount,
              uint32_t counterCount, vk::QueryPipelineStatisticFlags statistics,
              std::shared_ptr<vk::QueryPool> pool) :
        m_device(std::move(device)), m_kind(kind), m_queryCount(queryCount), m_counterCount(counterCount),
        m_statistics(statistics), m_pool(std::move(pool))
    {
    }

    std::shared_ptr<vk::Device> m_device;
    Kind m_kind{};
    uint32_t m_queryCount = 0;
    uint32_t m_counterCount = 0;
    vk::QueryPipelineStatisticFlags m_statistics{};
    std::shared_ptr<vk::QueryPool> m_pool;
};
