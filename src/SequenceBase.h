#pragma once
#include <cstddef>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "ShaderAssetGroup.h"
#include "typedefs.h"

class RenderManager;

// Common base for OnceSequence / LoopSequence: queues, limits, upload template,
// timestamp query pool and log.
class SequenceBase {
public:
    explicit SequenceBase(std::shared_ptr<RenderManager> renderMgr);
    virtual ~SequenceBase() = default;

    template <typename T>
    void OnUpdateRawData(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceTypes type, const T &data)
    {
        if constexpr (requires { data.data(); data.size(); }) {
            asset->UpdateInputResourceData(type, data.data(), data.size() * sizeof(decltype(*data.data())));
        } else {
            asset->UpdateInputResourceData(type, &data, sizeof(T));
        }
    }

    template <typename T, typename ResourceT>
    void OnUpdateRawData(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceT id, uint32_t frame, const T &data)
    {
        if constexpr (requires { data.data(); data.size(); }) {
            asset->UpdateDescriptorResourceData(ResourceKey::make_key(id, frame), data.data(),
                                                data.size() * sizeof(decltype(*data.data())));
        } else {
            asset->UpdateDescriptorResourceData(ResourceKey::make_key(id, frame), &data, sizeof(T));
        }
    }

protected:
    std::shared_ptr<vk::QueryPool> CreateTimestampQueryPool(uint32_t queryCount);
    void LogExecutionTime(uint32_t frameIndex, uint32_t queryPoolIndex = 0);

protected:
    std::shared_ptr<RenderManager> m_renderMgr;
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<vk::Queue> m_graphicsQueue;
    std::shared_ptr<vk::Queue> m_presentQueue;
    uint32_t m_graphicsFamily = 0;
    vk::PhysicalDeviceProperties m_deviceProperties{};
    std::vector<std::shared_ptr<vk::QueryPool>> m_timestampPools;
};
