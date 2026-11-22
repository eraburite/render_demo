#include "ShaderAssetGroup.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

// Minimal layout used by createEmpty(): an asset with no descriptor sets and
// no push constants.  It exists only so the returned asset can be wired into
// the same pipeline factories as a real asset.
class EmptyDescriptorLayout final : public IDescriptorSetLayoutGroup {
public:
    uint32_t maxFramesInFlight() const override { return 1; }
    uint32_t setCount() const override { return 0; }
    bool isSetShared(uint32_t) const override { return false; }
    UpdateFrequency frequencyOfSet(uint32_t) const override { return UpdateFrequency::eEachFrame; }
    const std::vector<SetLayoutDetail> &setDetails() const override { return m_emptyDetails; }
    std::vector<vk::DescriptorSetLayoutCreateInfo> layoutCreateInfos() const override { return {}; }
    std::vector<vk::DescriptorPoolSize> poolSizesPerFrame() const override { return {}; }
    const std::vector<vk::PushConstantRange> &pushConstantRanges() const override { return m_emptyRanges; }
    uint32_t pushConstantSize() const override { return 0; }
    std::shared_ptr<const IDescriptorSetLayoutGroup> sharedClone() const override
    {
        return std::make_shared<EmptyDescriptorLayout>();
    }

private:
    std::vector<SetLayoutDetail> m_emptyDetails;
    std::vector<vk::PushConstantRange> m_emptyRanges;
};

} // namespace

ShaderAssetGroup::ShaderAssetGroup(std::shared_ptr<vk::Device> device,
                                   std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                   uint32_t maxFrames) :
    m_device(std::move(device)), m_descriptorLayout(std::move(descriptorLayout)), m_maxFrames(maxFrames)
{
    if (m_maxFrames == 0) {
        m_maxFrames = 1;
    }
    if (m_descriptorLayout) {
        m_pushConstantCapacity = m_descriptorLayout->pushConstantSize();
    }
    m_pushConstants.assign(m_maxFrames, std::vector<uint8_t>(m_pushConstantCapacity, 0));
}

std::shared_ptr<ShaderAssetGroup> ShaderAssetGroup::createEmpty()
{
    return std::make_shared<ShaderAssetGroup>(nullptr, std::make_shared<EmptyDescriptorLayout>(), 1);
}

void ShaderAssetGroup::addInputResource(ResourceTypes type, std::shared_ptr<IDeviceResource> resource)
{
    m_inputResources[type] = std::move(resource);
}

void ShaderAssetGroup::addDescriptorResource(ResourceKey key, std::shared_ptr<IDeviceResource> resource)
{
    m_descriptorResources[key] = std::move(resource);
}

void ShaderAssetGroup::AliasDescriptorResource(ResourceKey alias, ResourceKey target)
{
    // insert_or_assign avoids requiring a default ResourceKey constructor.
    m_aliases.insert_or_assign(alias, target);
}

void ShaderAssetGroup::addDescriptorResourceForAllFrames(
    ResourceId id, const std::function<std::shared_ptr<IDeviceResource>(uint32_t)> &factory)
{
    for (uint32_t frame = 0; frame < m_maxFrames; ++frame) {
        addDescriptorResource(ResourceKey::make_key(id, frame), factory(frame));
    }
}

std::shared_ptr<IDeviceResource> ShaderAssetGroup::resolve(ResourceKey key) const
{
    // Alias chains are bounded; a cycle simply resolves to "not found" rather
    // than looping forever.
    for (uint32_t hop = 0; hop < 64; ++hop) {
        const auto resourceIt = m_descriptorResources.find(key);
        if (resourceIt != m_descriptorResources.end()) {
            return resourceIt->second;
        }
        const auto aliasIt = m_aliases.find(key);
        if (aliasIt == m_aliases.end()) {
            return nullptr;
        }
        key = aliasIt->second;
    }
    return nullptr;
}

uint32_t ShaderAssetGroup::effectiveFrame(ResourceId id, uint32_t frame) const
{
    if (m_descriptorLayout && m_descriptorLayout->isSetShared(id.getSetId())) {
        return 0;
    }
    return frame < m_maxFrames ? frame : 0;
}

std::shared_ptr<IDeviceResource> ShaderAssetGroup::DescriptorResource(ResourceKey key) const
{
    if (std::shared_ptr<IDeviceResource> resource = resolve(key)) {
        return resource;
    }
    // A static (shared) layer only owns a frame-0 resource; every other frame
    // falls back to that single copy.
    if (key.currentFrame() != 0) {
        return resolve(ResourceKey::make_key(key.getResourceId(), 0));
    }
    return nullptr;
}

std::shared_ptr<IDeviceResource> ShaderAssetGroup::DescriptorResource(ResourceId id, uint32_t frame) const
{
    return DescriptorResource(ResourceKey::make_key(id, effectiveFrame(id, frame)));
}

void ShaderAssetGroup::UpdateInputResourceData(ResourceTypes type, const void *data, size_t size)
{
    const auto it = m_inputResources.find(type);
    if (it == m_inputResources.end() || !it->second) {
        return;
    }
    it->second->updateRawData(data, size);
}

void ShaderAssetGroup::UpdateDescriptorResourceData(ResourceKey key, const void *data, size_t size)
{
    if (std::shared_ptr<IDeviceResource> resource = DescriptorResource(key)) {
        resource->updateRawData(data, size);
    }
}

void ShaderAssetGroup::UpdateConstantsResource(uint32_t frame, const void *data, size_t size)
{
    if (frame >= m_maxFrames) {
        throw std::out_of_range("UpdateConstantsResource: frame out of range");
    }
    if (size > m_pushConstantCapacity) {
        throw std::overflow_error("UpdateConstantsResource: data exceeds push constant capacity");
    }
    if (size == 0 || data == nullptr) {
        return;
    }
    std::memcpy(m_pushConstants[frame].data(), data, size);
}

void ShaderAssetGroup::SyncDataToDevice(const vk::CommandBuffer &cmd, uint32_t frame)
{
    (void)frame; // descriptor resources of every frame live in this same table
    const auto syncOne = [&](const std::shared_ptr<IDeviceResource> &resource) {
        if (resource && resource->NeedsUpload()) {
            resource->recordCopyFromStagingToDevice(cmd);
            resource->OnUploaded();
        }
    };

    for (const auto &[type, resource] : m_inputResources) {
        (void)type;
        syncOne(resource);
    }
    for (const auto &[key, resource] : m_descriptorResources) {
        (void)key;
        syncOne(resource);
    }
}

void ShaderAssetGroup::OnUploadCompleted()
{
    const auto finishOne = [](const std::shared_ptr<IDeviceResource> &resource) {
        if (resource) {
            resource->OnUploadCompleted();
        }
    };

    for (const auto &[type, resource] : m_inputResources) {
        (void)type;
        finishOne(resource);
    }
    for (const auto &[key, resource] : m_descriptorResources) {
        (void)key;
        finishOne(resource);
    }
}

void ShaderAssetGroup::bindToPipeline(const vk::CommandBuffer &cmd)
{
    for (const auto &[type, resource] : m_inputResources) {
        if (IsInputResource(type) && resource) {
            resource->bindToPipeline(cmd);
        }
    }
}

void ShaderAssetGroup::PushConstantData(const vk::CommandBuffer &cmd, vk::PipelineLayout layout,
                                        const std::vector<vk::PushConstantRange> &ranges, uint32_t frame)
{
    if (!layout || ranges.empty() || frame >= m_maxFrames) {
        return;
    }
    const std::vector<uint8_t> &data = m_pushConstants[frame];
    for (const vk::PushConstantRange &range : ranges) {
        if (range.offset + range.size > data.size()) {
            continue;
        }
        cmd.pushConstants(layout, range.stageFlags, range.offset, range.size, data.data() + range.offset);
    }
}

void ShaderAssetGroup::RecordDescriptorResourceReadback(ResourceKey key, const vk::CommandBuffer &cmd,
                                                        uint32_t frame)
{
    // Prefer the exact key (aliases included); the frame argument is a
    // convenience for callers that hold only a ResourceId-style key.
    std::shared_ptr<IDeviceResource> resource = resolve(key);
    if (!resource) {
        resource = DescriptorResource(key.getResourceId(), frame);
    }
    if (!resource) {
        return;
    }
    resource->OnReadbackRequested();
    resource->recordCopyFromDeviceToStaging(cmd);
}

std::vector<std::byte> ShaderAssetGroup::ReadStagedDescriptorResource(ResourceKey key)
{
    const std::shared_ptr<IDeviceResource> resource = DescriptorResource(key);
    if (!resource) {
        return {};
    }
    const size_t byteSize = resource->MappedByteSize();
    const void *mapped = resource->MappedData();
    if (byteSize == 0 || mapped == nullptr) {
        return {};
    }

    std::vector<std::byte> result(byteSize);
    std::memcpy(result.data(), mapped, byteSize);
    return result;
}
