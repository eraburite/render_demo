#pragma once
#include <any>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class DeviceResourceVisitor;

// Capability interfaces: one resource may expose several capabilities, callers
// take only what they need.
class ITransferable {
public:
    virtual ~ITransferable() = default;
    virtual void recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd) = 0;
    virtual void recordCopyFromDeviceToStaging(const vk::CommandBuffer &cmd) = 0;
};

class IInputBindable {
public:
    virtual ~IInputBindable() = default;
    virtual void bindToPipeline(const vk::CommandBuffer &cmd) = 0;
};

class IDescriptorUpdater {
public:
    virtual ~IDescriptorUpdater() = default;
    virtual vk::WriteDescriptorSet constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                               uint32_t binding) = 0;
};

class IDeviceResource : public ITransferable, public IInputBindable, public IDescriptorUpdater {
public:
    virtual ~IDeviceResource() = default;

    virtual void updateRawData(const void *data, size_t size) = 0;
    virtual ResourceTypes resourceType() const = 0;

    // Dirty-flag upload: updateRawData marks dirty, SyncDataToDevice skips clean.
    virtual bool NeedsUpload() const = 0;
    virtual void OnUploaded() = 0;

    // staging lifecycle: release host copy after upload fence, rebuild on readback.
    virtual void OnUploadCompleted() = 0;
    virtual void OnReadbackRequested() = 0;

    // host-visible mapped copy (device-local -> staging for readback).
    virtual void *MappedData() = 0;
    virtual size_t MappedByteSize() const = 0;

    virtual void accept(DeviceResourceVisitor &visitor) = 0;
};

class DeviceResourceVisitor {
public:
    virtual ~DeviceResourceVisitor() = default;
    virtual void visit(class BufferObject &) {}
    virtual void visit(class TextureImage &) {}
    virtual void visit(class TextureSamplerCombine &) {}
};

// Heterogeneous resource table: std::any-wrapped shared_ptr<T> plus typed find.
template <typename Key, typename T>
class ResourceObjectRepository {
public:
    void insert(Key key, std::shared_ptr<T> value)
    {
        m_table.emplace(std::move(key), std::any(std::move(value)));
    }

    std::shared_ptr<T> findValue(const Key &key) const
    {
        auto it = m_table.find(key);
        if (it == m_table.end() || !it->second.has_value()) {
            return nullptr;
        }
        return std::any_cast<std::shared_ptr<T>>(it->second);
    }

    bool contains(const Key &key) const { return m_table.find(key) != m_table.end(); }

    std::vector<std::shared_ptr<T>> values() const
    {
        std::vector<std::shared_ptr<T>> result;
        for (const auto &[key, any] : m_table) {
            (void)key;
            if (any.has_value()) {
                result.push_back(std::any_cast<std::shared_ptr<T>>(any));
            }
        }
        return result;
    }

    void forEach(const std::function<void(const Key &, const std::shared_ptr<T> &)> &cb) const
    {
        for (const auto &[key, any] : m_table) {
            if (any.has_value()) {
                cb(key, std::any_cast<std::shared_ptr<T>>(any));
            }
        }
    }

private:
    std::unordered_map<Key, std::any, ResourceKeyHasher> m_table;
};

// Collects image views from a resource table (e.g. offscreen resolve targets).
class ImageViewCollector : public DeviceResourceVisitor {
public:
    void visit(TextureImage &texture) override;
    void visit(TextureSamplerCombine &combine) override;

    const std::vector<vk::ImageView> &views() const { return m_views; }

private:
    std::vector<vk::ImageView> m_views;
};
