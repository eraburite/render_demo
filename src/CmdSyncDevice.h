#pragma once
#include "CmdOpBase.h"
#include "ShaderAssetGroup.h"

// Upload operation: synchronizes all dirty resources of an asset from staging
// to device-local memory.  Used by OnceSequence.
class CmdSyncDevice : public CmdOpBase {
public:
    using CmdOpBase::OnUpdateRawData;
    explicit CmdSyncDevice(std::shared_ptr<ShaderAssetGroup> asset) : m_asset(std::move(asset)) {}

    void OnUpdateRawData(ResourceTypes type, const void *data, size_t size) override
    {
        m_asset->UpdateInputResourceData(type, data, size);
    }
    void OnUpdateRawData(ResourceId id, uint32_t frame, const void *data, size_t size) override
    {
        m_asset->UpdateDescriptorResourceData(ResourceKey::make_key(id, frame), data, size);
    }
    void OnUpdateRawData(ResourceTypes, uint32_t frame, const void *data, size_t size) override
    {
        m_asset->UpdateConstantsResource(frame, data, size);
    }
    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eStatic; }
    void Record(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &) override
    {
        m_asset->SyncDataToDevice(cmd, frame);
    }

private:
    std::shared_ptr<ShaderAssetGroup> m_asset;
};
