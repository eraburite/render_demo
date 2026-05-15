#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class MyRenderPass {
public:
    MyRenderPass(std::shared_ptr<vk::Device> device, vk::SampleCountFlagBits msaaSamples, const vk::Format &colorFormat,
                 const vk::Format &depthFormat);
    ~MyRenderPass();

    std::shared_ptr<vk::RenderPass> getRenderPass() const
    {
        return m_renderPass;
    }

private:
    void createRenderPass();

private:
    std::shared_ptr<vk::Device> m_device;
    vk::SampleCountFlagBits m_msaaSamples;
    vk::Format m_colorFormat;
    vk::Format m_depthFormat;
    std::shared_ptr<vk::RenderPass> m_renderPass;
};
