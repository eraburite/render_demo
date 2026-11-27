#pragma once
#include "ModelLoader.h"

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Pipeline (a light Chain of Responsibility).
//
// The registry used to run post-processing as eight hard-coded `if` statements
// whose only specification of order was a comment.  A step is now a value:
// it declares when it is enabled and what it did, so the chain is queryable,
// extensible and printable.  New formats can inject a step without editing
// ModelLoaderRegistry::load(), and --list can show the exact chain.
// ---------------------------------------------------------------------------
namespace model_post {

struct Step {
    std::string name;
    std::function<bool(const ModelLoadOptions &, const PostProcessPlan &)> enabled;
    // Returns a short, human-readable detail for the run log (may be empty).
    std::function<std::string(Model &, const ModelLoadOptions &, const PostProcessPlan &)> apply;
};

struct StepResult {
    std::string name;
    std::string detail;
};

class Pipeline {
public:
    // Appends at the end; steps run in insertion order, which is now explicit
    // data rather than an implicit property of the source layout.
    void add(Step step) { m_steps.push_back(std::move(step)); }

    const std::vector<Step> &steps() const { return m_steps; }
    std::vector<std::string> stepNames() const;

    std::vector<StepResult> run(Model &model, const ModelLoadOptions &options,
                                const PostProcessPlan &plan) const;

private:
    std::vector<Step> m_steps;
};

// The canonical chain.  `loader` is captured by the final step, which is the
// Template Method hook (IModelLoader::postProcess) -- it is part of the chain
// and therefore visible in the log like every other step.
Pipeline makeDefaultPipeline(const IModelLoader &loader);

} // namespace model_post
