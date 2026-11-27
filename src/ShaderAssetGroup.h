#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DeviceResource.h"
#include "IDescriptorSetDetail.h"
#include "typedefs.h"

// Aggregates all resources needed by one draw/dispatch: input buffers (VBO/IBO),
// descriptor resources (UBO/SSBO/texture), and push-constant data per frame.
class ShaderAssetGroup {
public:
    ShaderAssetGroup(std::shared_ptr<vk::Device> device,
                     std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout, uint32_t maxFrames);

    void addShaderSpirvCode(const std::string &name, std::vector<uint32_t> code) { m_shaders[name] = std::move(code); }
    const std::unordered_map<std::string, std::vector<uint32_t>> &shaderCodes() const { return m_shaders; }

    void addInputResource(ResourceTypes type, std::shared_ptr<IDeviceResource> resource);
    void addDescriptorResource(ResourceKey key, std::shared_ptr<IDeviceResource> resource);

    // Make two keys reference the same underlying resource (ping-pong alias).
    void AliasDescriptorResource(ResourceKey alias, ResourceKey target);

    void addDescriptorResourceForAllFrames(ResourceId id,
                                           const std::function<std::shared_ptr<IDeviceResource>(uint32_t)> &factory);

    void UpdateInputResourceData(ResourceTypes type, const void *data, size_t size);
    void UpdateDescriptorResourceData(ResourceKey key, const void *data, size_t size);
    void UpdateConstantsResource(uint32_t frame, const void *data, size_t size);

    // Sync only dirty resources; shared layers live in frame 0.
    void SyncDataToDevice(const vk::CommandBuffer &cmd, uint32_t frame);
    void OnUploadCompleted();

    void bindToPipeline(const vk::CommandBuffer &cmd);
    void PushConstantData(const vk::CommandBuffer &cmd, vk::PipelineLayout layout,
                          const std::vector<vk::PushConstantRange> &ranges, uint32_t frame);

    std::shared_ptr<IDeviceResource> DescriptorResource(ResourceKey key) const;
    std::shared_ptr<IDeviceResource> DescriptorResource(ResourceId id, uint32_t frame) const;

    // Input resources (VBO / IBO / per-instance VBO) can be read back too.
    // Descriptor readback was the only path until the instancing example needed
    // to prove that binding 1 really holds the CPU's instance array (see 28.3).
    std::shared_ptr<IDeviceResource> InputResource(ResourceTypes type) const;
    void RecordInputResourceReadback(ResourceTypes type, const vk::CommandBuffer &cmd);
    std::vector<std::byte> ReadStagedInputResource(ResourceTypes type);

    void RecordDescriptorResourceReadback(ResourceKey key, const vk::CommandBuffer &cmd, uint32_t frame);
    std::vector<std::byte> ReadStagedDescriptorResource(ResourceKey key);

    const std::shared_ptr<const IDescriptorSetLayoutGroup> &descriptorLayout() const { return m_descriptorLayout; }
    uint32_t maxFrames() const { return m_maxFrames; }

    // A default empty vertex-input asset is created lazily by RenderManager.
    static std::shared_ptr<ShaderAssetGroup> createEmpty();

private:
    std::shared_ptr<IDeviceResource> resolve(ResourceKey key) const;
    uint32_t effectiveFrame(ResourceId id, uint32_t frame) const;

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<const IDescriptorSetLayoutGroup> m_descriptorLayout;
    uint32_t m_maxFrames = 2;

    std::unordered_map<ResourceTypes, std::shared_ptr<IDeviceResource>> m_inputResources;
    std::unordered_map<ResourceKey, std::shared_ptr<IDeviceResource>, ResourceKeyHasher> m_descriptorResources;
    std::unordered_map<ResourceKey, ResourceKey, ResourceKeyHasher> m_aliases;
    std::unordered_map<std::string, std::vector<uint32_t>> m_shaders;
    std::vector<std::vector<uint8_t>> m_pushConstants;
    uint32_t m_pushConstantCapacity = 0;
};
