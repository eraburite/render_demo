#pragma once
#include <utility>
#include <vulkan/vulkan.hpp>

// Owning RAII wrapper for Vulkan handles.  The Deleter is a callable that
// receives the raw handle.  Move-only; converts implicitly to the raw handle
// for use in Vulkan-Hpp calls.
template <typename Handle, typename Deleter>
class VulkanHandle {
public:
    VulkanHandle() = default;
    VulkanHandle(Handle handle, Deleter deleter) : m_handle(handle), m_deleter(std::move(deleter)) {}
    ~VulkanHandle() { reset(); }

    VulkanHandle(const VulkanHandle &) = delete;
    VulkanHandle &operator=(const VulkanHandle &) = delete;

    VulkanHandle(VulkanHandle &&other) noexcept : m_handle(other.m_handle), m_deleter(std::move(other.m_deleter))
    {
        other.m_handle = Handle{};
    }
    VulkanHandle &operator=(VulkanHandle &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            m_deleter = std::move(other.m_deleter);
            other.m_handle = Handle{};
        }
        return *this;
    }

    operator Handle() const { return m_handle; }
    // Some call sites use the shared_ptr-style "*handle" spelling; returning
    // the underlying handle keeps those usages working without a wrapper.
    Handle operator*() const { return m_handle; }
    Handle get() const { return m_handle; }
    Handle *getPtr() { return &m_handle; }
    const Handle *getPtr() const { return &m_handle; }
    explicit operator bool() const { return m_handle != Handle{}; }

    void reset()
    {
        if (m_handle != Handle{}) {
            m_deleter(m_handle);
            m_handle = Handle{};
        }
    }

private:
    Handle m_handle{};
    Deleter m_deleter{};
};

// Convenience: non-owning view of a handle (empty deleter).  Used for members
// that would otherwise form a self-reference cycle through RenderManager.
template <typename Handle>
VulkanHandle<Handle, void (*)(Handle)> makeHandleView(Handle handle)
{
    return VulkanHandle<Handle, void (*)(Handle)>(handle, [](Handle) {});
}
