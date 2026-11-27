#pragma once
#include "ModelLoader.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// RAII self-registration (Plugin pattern).
//
//     namespace { ModelLoaderRegistrar<MyLoader> g_myLoader{"myfmt", {"foo"}}; }
//
// A translation unit that does this joins the registry at static-initialisation
// time without any framework file knowing it exists.  Built-ins deliberately do
// NOT rely on this: a static library only pulls in an object file when some
// referenced symbol lives there, so an unreferenced registrar would be silently
// dropped by the MSVC linker.  BuiltinModelLoaders.cpp is the explicit link
// anchor; this class is the extension point for out-of-tree / test plugins.
// ---------------------------------------------------------------------------
class ModelLoaderRegistrar {
public:
    ModelLoaderRegistrar(std::string name, std::vector<std::string> extensions,
                         ModelLoaderFactory factory, int priority = 0, bool magicProbe = false)
    {
        ModelLoaderDescriptor descriptor;
        descriptor.name = std::move(name);
        descriptor.extensions = std::move(extensions);
        descriptor.priority = priority;
        descriptor.magicProbe = magicProbe;
        descriptor.factory = std::move(factory);
        ModelLoaderRegistry::instance().registerLoader(std::move(descriptor));
    }
};

template <typename LoaderT>
ModelLoaderRegistrar makeLoaderRegistrar(std::string name, std::vector<std::string> extensions,
                                         int priority = 0)
{
    return ModelLoaderRegistrar(std::move(name), std::move(extensions),
                                []() -> std::unique_ptr<IModelLoader> { return std::make_unique<LoaderT>(); },
                                priority);
}
