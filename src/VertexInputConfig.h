#pragma once
#include "typedefs.h"
#include <cstdint>
#include <vector>
#include <functional>
#include <iostream>
#include <type_traits>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

struct Vertex {
    // position
    glm::vec3 position;
    // color
    glm::vec3 color;
    // texCoords
    glm::vec2 texCoord;
    // normal
    glm::vec3 normal;
    // tangent
    glm::vec4 tangent;
    // bitangent
    glm::vec4 bitangent;
    // bone indexes which will influence this vertex
    glm::vec4 joint0;
    // weights from each bone
    glm::vec4 weight0;
    bool operator==(const Vertex &other) const
    {
        return position == other.position && color == other.color && texCoord == other.texCoord;
    }
};

class VertexInputParam {
public:
    enum class VertexComponent {
        ePosition,
        eColor,
        eUV,
        eNormal,
        eTangent,
        eBitangent,
        eJoint0,
        eWeight0
    }; // location list

    VertexInputParam(uint32_t binding, const std::vector<VertexComponent> &components) :
        m_vertexMeta({}), m_indexMeta({})
    {
        buildBindingDescription(binding);
        uint32_t location = 0;
        for (auto &component : components) {
            m_attributeDescriptions.push_back(buildAttributeDescription(binding, location, component));
            location++;
        }
        buildPipelineVertexInputStateCreateInfo();
    }

    ~VertexInputParam() = default;

    void SetSpec(ResourceTypes type, GBufferSpec spec)
    {
        if (!IsInputResource(type)) {
            return;
        }
        if (type == ResourceTypes::eVertex) {
            m_vertexMeta = std::move(spec);
        } else if (type == ResourceTypes::eIndex) {
            m_indexMeta = std::move(spec);
        }
    }

    uint32_t getIndexCount() const
    {
        return m_indexMeta.elementCount;
    }

    uint32_t getVertexCount() const
    {
        return m_vertexMeta.elementCount;
    }

    template <typename F>
    void dispatch(F &&callback) const
    {
        std::invoke(callback, ResourceTypes::eVertex, m_vertexMeta);
        std::invoke(callback, ResourceTypes::eIndex, m_indexMeta);
    }

    vk::PipelineVertexInputStateCreateInfo getPipelineVertexInputStateCreateInfo() const
    {
        return m_vertexInputInfo;
    }

private:
    void buildPipelineVertexInputStateCreateInfo()
    {
        m_vertexInputInfo.vertexBindingDescriptionCount = 1;
        m_vertexInputInfo.pVertexBindingDescriptions = &m_bindingDescription;
        m_vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_attributeDescriptions.size());
        m_vertexInputInfo.pVertexAttributeDescriptions = m_attributeDescriptions.data();
    }

    void buildBindingDescription(uint32_t binding)
    {
        m_bindingDescription.binding = binding;
        m_bindingDescription.stride = sizeof(Vertex);
        m_bindingDescription.inputRate = vk::VertexInputRate::eVertex;
    }

    vk::VertexInputAttributeDescription buildAttributeDescription(uint32_t binding, uint32_t location,
                                                                  VertexComponent component) const
    {
        vk::VertexInputAttributeDescription attributeDescription{};
        bool found = false;
        std::apply(
            [&](auto &&...args) {
                ((args.component == component ?
                      (attributeDescription = {location, binding, args.format, args.offset}, found = true) :
                      false),
                 ...);
            },
            sVertexAttrTable);
        if (!found) {
            throw std::runtime_error(" failed to create VertexInputAttributeDescription!");
        }
        return attributeDescription;
    }

private:
    GBufferSpec m_vertexMeta{};
    GBufferSpec m_indexMeta{};
    vk::VertexInputBindingDescription m_bindingDescription{};
    std::vector<vk::VertexInputAttributeDescription> m_attributeDescriptions{};
    vk::PipelineVertexInputStateCreateInfo m_vertexInputInfo{};

    struct VertexAttrEntry {
        VertexComponent component;
        vk::Format format;
        uint32_t offset;
    };

    static constexpr auto sVertexAttrTable = std::make_tuple(
        VertexAttrEntry{VertexComponent::ePosition, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position)},
        VertexAttrEntry{VertexComponent::eColor, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, color)},
        VertexAttrEntry{VertexComponent::eUV, vk::Format::eR32G32Sfloat, offsetof(Vertex, texCoord)},
        VertexAttrEntry{VertexComponent::eNormal, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal)},
        VertexAttrEntry{VertexComponent::eTangent, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, tangent)},
        VertexAttrEntry{VertexComponent::eBitangent, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, bitangent)},
        VertexAttrEntry{VertexComponent::eJoint0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, joint0)},
        VertexAttrEntry{VertexComponent::eWeight0, vk::Format::eR32G32B32A32Sfloat, offsetof(Vertex, weight0)});
};
