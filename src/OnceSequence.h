#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "SequenceBase.h"

// One-shot upload / readback sequence.  Single command pool + fence; each
// submission is synchronized with waitForFences and the temporary command
// buffer is released afterwards.
class OnceSequence : public SequenceBase {
public:
    explicit OnceSequence(std::shared_ptr<RenderManager> renderMgr);
    ~OnceSequence();

    void emitToDevice(const std::shared_ptr<ShaderAssetGroup> &asset);
    void submit(const std::shared_ptr<CmdOpBase> &op, const std::shared_ptr<ShaderAssetGroup> &asset);
    std::vector<std::byte> ReadBack(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceKey key, size_t byteSize);
    std::vector<std::byte> ReadBack(const std::shared_ptr<ShaderAssetGroup> &asset, ResourceId id, size_t byteSize);

private:
    void createCommandPool();
    void createFence();

private:
    std::shared_ptr<vk::CommandPool> m_commandPool;
    std::shared_ptr<vk::Fence> m_fence;
};
