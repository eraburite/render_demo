#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class IVertexInputStateGroup {
public:
    virtual ~IVertexInputStateGroup() = default;
    virtual vk::PipelineVertexInputStateCreateInfo pipelineVertexInputState() const = 0;
    virtual uint32_t vertexByteSize() const = 0;
    virtual uint32_t vertexCount() const = 0;
    virtual uint32_t indexCount() const = 0;
    virtual DataTypes indexType() const = 0;
    virtual bool hasIndexBuffer() const = 0;
    virtual bool isEmptyInputState() const = 0;

    // Number of vertices to draw when there is no vertex input (SSBO-driven).
    virtual uint32_t getVertexCount() const = 0;

    virtual std::shared_ptr<const IVertexInputStateGroup> sharedClone() const = 0;
};
