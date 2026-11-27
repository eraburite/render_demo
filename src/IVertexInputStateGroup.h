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

    // Number of instances per draw (gl_InstanceIndex driven); always >= 1.
    virtual uint32_t drawInstanceCount() const = 0;

    // Per-frame override of the draw instance count.  Culling decides how many
    // instances survive, so the count is dynamic state, not pipeline state: the
    // pipeline owns a clone of this object and adjusts *that*, which is why the
    // setter lives on the interface instead of on one concrete atlas.
    virtual void setDrawInstanceCount(uint32_t count) = 0;

    // Optional second vertex binding at binding 1 with eInstance input rate.
    // Defaults keep every existing example on the single-binding path; only an
    // atlas built with instance components reports a non-zero count (see 28.3).
    virtual bool hasInstanceBinding() const { return false; }
    virtual uint32_t instanceVertexCount() const { return 0; }
    virtual uint32_t instanceVertexByteSize() const { return 0; }

    // Fresh, uniquely owned copy.  Non-const on purpose: the pipeline mutates the
    // clone (instance count) while readers only ever need the const view.
    virtual std::shared_ptr<IVertexInputStateGroup> sharedClone() const = 0;
};
