#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "IVertexInputStateGroup.h"

struct Vertex {
    glm::vec3 position{};
    glm::vec3 color{};
    glm::vec2 texCoord{};
    glm::vec3 normal{};
    glm::vec3 tangent{};

    bool operator==(const Vertex &other) const
    {
        return position == other.position && color == other.color && texCoord == other.texCoord
               && normal == other.normal && tangent == other.tangent;
    }
};

enum class VertexComponent { ePosition, eColor, eUV, eNormal, eTangent };

struct VertexComponentSpec {
    VertexComponent component;
    vk::Format format;
    uint32_t offset;
    uint32_t size;
};

inline constexpr auto VertexComponentTable = std::make_tuple(
    VertexComponentSpec{VertexComponent::ePosition, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position), 12},
    VertexComponentSpec{VertexComponent::eColor, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color), 12},
    VertexComponentSpec{VertexComponent::eUV, vk::Format::eR32G32Sfloat, offsetof(Vertex, texCoord), 8},
    VertexComponentSpec{VertexComponent::eNormal, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal), 12},
    // Vertex::tangent is a vec3, so the attribute must be 3 components: a
    // R32G32B32A32 read would pull 4 bytes past the end of the last vertex.
    VertexComponentSpec{VertexComponent::eTangent, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, tangent), 12});

// ---- binding 1: per-instance stream --------------------------------------
// The Vulkan-Samples `instancing` example uses a separate vertex buffer whose
// input rate is VK_VERTEX_INPUT_RATE_INSTANCE.  It is declared here as a first
// class vertex-input option rather than an SSBO, so the same pipeline can be
// drawn with vkCmdDraw(..., instanceCount, ...) with no descriptor churn.
struct InstanceData {
    glm::vec3 position{};
    glm::vec3 rotation{};
    glm::vec3 scale{1.0f};
    glm::vec3 color{1.0f};
};
static_assert(sizeof(InstanceData) == 48, "InstanceData must be four vec3 (48 B)");

enum class VertexInstanceComponent { ePosition, eRotation, eScale, eColor };

struct VertexInstanceComponentSpec {
    VertexInstanceComponent component;
    vk::Format format;
    uint32_t offset;
    uint32_t size;
};

inline constexpr auto VertexInstanceComponentTable = std::make_tuple(
    VertexInstanceComponentSpec{VertexInstanceComponent::ePosition, vk::Format::eR32G32B32Sfloat,
                          offsetof(InstanceData, position), 12},
    VertexInstanceComponentSpec{VertexInstanceComponent::eRotation, vk::Format::eR32G32B32Sfloat,
                          offsetof(InstanceData, rotation), 12},
    VertexInstanceComponentSpec{VertexInstanceComponent::eScale, vk::Format::eR32G32B32Sfloat,
                          offsetof(InstanceData, scale), 12},
    VertexInstanceComponentSpec{VertexInstanceComponent::eColor, vk::Format::eR32G32B32Sfloat,
                          offsetof(InstanceData, color), 12});

// Concrete vertex input group.  location follows the listed component order.
class VertexInputAtlas : public IVertexInputStateGroup {
public:
    VertexInputAtlas(uint32_t binding, std::vector<VertexComponent> components, uint32_t vertexCount = 0,
                     uint32_t indexCount = 0, DataTypes indexType = DataTypes::eUnsignedInt,
                     bool emptyInput = false, uint32_t drawVertexCount = 0, uint32_t instanceCount = 1,
                     std::vector<VertexInstanceComponent> instanceComponents = {},
                     uint32_t instanceVertexCount = 0) :
        m_binding(binding), m_components(std::move(components)), m_vertexCount(vertexCount),
        m_indexCount(indexCount), m_indexType(indexType), m_emptyInput(emptyInput),
        m_drawVertexCount(drawVertexCount), m_instanceCount(instanceCount),
        m_instanceComponents(std::move(instanceComponents)),
        m_instanceVertexCount(instanceVertexCount == 0 ? instanceCount : instanceVertexCount)
    {
        rebuildInputState();
    }

    // The create-info points into m_bindings/m_attributes, so a clone must
    // rebuild those pointers instead of copying the shallow vk structure.
    VertexInputAtlas(const VertexInputAtlas &other) :
        IVertexInputStateGroup(), m_binding(other.m_binding), m_components(other.m_components),
        m_vertexCount(other.m_vertexCount), m_indexCount(other.m_indexCount), m_indexType(other.m_indexType),
        m_emptyInput(other.m_emptyInput), m_drawVertexCount(other.m_drawVertexCount),
        m_instanceCount(other.m_instanceCount), m_instanceComponents(other.m_instanceComponents),
        m_instanceVertexCount(other.m_instanceVertexCount)
    {
        rebuildInputState();
    }

    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputState() const override { return m_inputState; }
    uint32_t vertexByteSize() const override { return sizeof(Vertex); }
    uint32_t vertexCount() const override { return m_vertexCount; }
    uint32_t indexCount() const override { return m_indexCount; }
    DataTypes indexType() const override { return m_indexType; }
    bool hasIndexBuffer() const override { return m_indexCount > 0; }
    bool isEmptyInputState() const override { return m_emptyInput; }
    uint32_t getVertexCount() const override { return m_drawVertexCount; }
    // Zero means zero: culling can legitimately cut the scene down to nothing,
    // and a draw with instanceCount 0 is the cheapest way to say that.  The
    // constructor's default of 1 is what keeps a plain single-instance draw
    // working without every caller spelling it out.
    uint32_t drawInstanceCount() const override { return m_instanceCount; }
    bool hasInstanceBinding() const override { return !m_instanceComponents.empty(); }
    uint32_t instanceVertexCount() const override { return m_instanceVertexCount; }
    uint32_t instanceVertexByteSize() const override { return static_cast<uint32_t>(sizeof(InstanceData)); }

    // The vertex input state does not encode the instance count (it is a draw
    // parameter), so no rebuild is needed here.
    void setDrawInstanceCount(uint32_t count) override { m_instanceCount = count; }

    std::shared_ptr<IVertexInputStateGroup> sharedClone() const override
    {
        return std::make_shared<VertexInputAtlas>(*this);
    }

    void setVertexCount(uint32_t count) { m_vertexCount = count; }
    void setIndexCount(uint32_t count) { m_indexCount = count; }
    void setInstanceCount(uint32_t count) { m_instanceCount = count; }

private:
    void rebuildInputState()
    {
        m_bindings.clear();
        m_attributes.clear();

        vk::VertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = m_binding;
        bindingDesc.stride = sizeof(Vertex);
        bindingDesc.inputRate = vk::VertexInputRate::eVertex;
        m_bindings.push_back(bindingDesc);

        uint32_t location = 0;
        for (auto component : m_components) {
            std::apply(
                [&](auto &&...row) {
                    ((row.component == component
                          ? (m_attributes.push_back(vk::VertexInputAttributeDescription(location++, m_binding,
                                                                                         row.format, row.offset)),
                             true)
                          : false),
                     ...);
                },
                VertexComponentTable);
        }

        if (!m_instanceComponents.empty()) {
            // Binding 1 is per-instance.  Locations continue right after the
            // per-vertex ones, so the shader declares the same flat layout.
            constexpr uint32_t kInstanceBinding = 1;
            vk::VertexInputBindingDescription instanceBindingDesc{};
            instanceBindingDesc.binding = kInstanceBinding;
            instanceBindingDesc.stride = sizeof(InstanceData);
            instanceBindingDesc.inputRate = vk::VertexInputRate::eInstance;
            m_bindings.push_back(instanceBindingDesc);

            for (auto component : m_instanceComponents) {
                std::apply(
                    [&](auto &&...row) {
                        ((row.component == component
                              ? (m_attributes.push_back(vk::VertexInputAttributeDescription(
                                     location++, kInstanceBinding, row.format, row.offset)),
                                 true)
                              : false),
                         ...);
                    },
                    VertexInstanceComponentTable);
            }
        }

        m_inputState = vk::PipelineVertexInputStateCreateInfo{};
        m_inputState.setVertexBindingDescriptionCount(static_cast<uint32_t>(m_bindings.size()))
            .setPVertexBindingDescriptions(m_bindings.data())
            .setVertexAttributeDescriptionCount(static_cast<uint32_t>(m_attributes.size()))
            .setPVertexAttributeDescriptions(m_attributes.data());
    }

    uint32_t m_binding = 0;
    std::vector<VertexComponent> m_components;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    DataTypes m_indexType = DataTypes::eUnsignedInt;
    bool m_emptyInput = false;
    uint32_t m_drawVertexCount = 0;
    uint32_t m_instanceCount = 1;
    std::vector<VertexInstanceComponent> m_instanceComponents;
    uint32_t m_instanceVertexCount = 1;

    std::vector<vk::VertexInputBindingDescription> m_bindings;
    std::vector<vk::VertexInputAttributeDescription> m_attributes;
    vk::PipelineVertexInputStateCreateInfo m_inputState{};
};
