#include "grhsim/pass/pass.hpp"
#include "grhsim/pass/reg_to_mem.hpp"
#include "grhsim/pass/canonicalize_compute.hpp"
#include "grhsim/pass/clone_shared_compute.hpp"
#include "grhsim/pass/bitwise_predicates.hpp"
#include "grhsim/pass/bitwise_muxes.hpp"
#include "grhsim/pass/mux_chain_fold.hpp"
#include "grhsim/pass/pack_bit_registers.hpp"
#include "grhsim/backend/cpu.hpp"

#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/ir/verifier.hpp"

#include <algorithm>
#include <exception>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        class VerifyPass final : public Pass
        {
        public:
            VerifyPass() : Pass("grhsim.verify", PassKind::Analysis) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                return PassResult{verifyGrhSimModel(model, defaultDialectRegistry(), diagnostics),
                                  false, {}};
            }
        };

    } // namespace

    bool PassRegistry::registerPass(std::string name, PassKind kind,
                                    PassFactory factory, std::string &error)
    {
        if (name.empty() || name.find('.') == std::string::npos)
        {
            error = "GrhSIM pass names must use <owner>.<name>: " + name;
            return false;
        }
        if (!factory)
        {
            error = "GrhSIM pass factory is empty: " + name;
            return false;
        }
        auto [it, inserted] = entries_.emplace(std::move(name), Entry{kind, std::move(factory)});
        if (!inserted)
        {
            error = "GrhSIM pass is already registered: " + it->first;
            return false;
        }
        return true;
    }

    std::unique_ptr<Pass> PassRegistry::create(std::string_view name,
                                               std::span<const std::string_view> args,
                                               std::string &error) const
    {
        auto it = entries_.find(std::string(name));
        if (it == entries_.end())
        {
            error = "unknown GrhSIM pass: " + std::string(name);
            return nullptr;
        }
        std::unique_ptr<Pass> pass = it->second.factory(args, error);
        if (pass && pass->kind() != it->second.kind)
        {
            error = "GrhSIM pass factory returned a mismatched PassKind: " + std::string(name);
            return nullptr;
        }
        return pass;
    }

    std::vector<std::string> PassRegistry::names() const
    {
        std::vector<std::string> result;
        result.reserve(entries_.size());
        for (const auto &[name, entry] : entries_)
        {
            (void)entry;
            result.push_back(name);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    void PassManager::addPass(std::unique_ptr<Pass> pass)
    {
        if (pass) passes_.push_back(std::move(pass));
    }

    PassManagerResult PassManager::run(GrhSimModel &model,
                                       diag::Diagnostics &diagnostics)
    {
        PassManagerResult managerResult;
        if (!verifyGrhSimModel(model, *dialects_, diagnostics))
        {
            managerResult.success = false;
            return managerResult;
        }

        for (auto &pass : passes_)
        {
            PassResult result;
            try
            {
                result = pass->run(model, diagnostics);
            }
            catch (const std::exception &ex)
            {
                diagnostics.error(ex.what(), pass->name());
                result.success = false;
            }
            if (!result.success || diagnostics.hasError())
            {
                if (result.changed && pass->kind() != PassKind::Analysis &&
                    pass->kind() != PassKind::Emit)
                    model.poison();
                managerResult.success = false;
                return managerResult;
            }
            if (result.changed)
            {
                switch (pass->kind())
                {
                case PassKind::Analysis:
                case PassKind::Emit:
                    diagnostics.error("read-only GrhSIM pass reported an in-place mutation",
                                      pass->name());
                    model.poison();
                    managerResult.success = false;
                    return managerResult;
                case PassKind::MetadataTransform:
                    model.commitMetadataMutation();
                    break;
                case PassKind::SemanticTransform:
                    model.commitSemanticMutation();
                    break;
                case PassKind::BackendMapping:
                    break;
                }
            }
            if (!verifyGrhSimModel(model, *dialects_, diagnostics))
            {
                if (result.changed) model.poison();
                managerResult.success = false;
                return managerResult;
            }
            managerResult.changed = managerResult.changed || result.changed;
            managerResult.artifacts.insert(managerResult.artifacts.end(),
                                           result.artifacts.begin(), result.artifacts.end());
        }
        return managerResult;
    }

    PassRegistry makeDefaultPassRegistry()
    {
        PassRegistry registry;
        registerCpuPasses(registry);
        registerRegToMemPass(registry);
        registerCanonicalizeComputePass(registry);
        registerCloneSharedComputePass(registry);
        registerBitwisePredicatesPass(registry);
        registerBitwiseMuxesPass(registry);
        registerMuxChainFoldPass(registry);
        registerPackBitRegistersPass(registry);
        std::string error;
        registry.registerPass(
            "grhsim.verify", PassKind::Analysis,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.verify does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<VerifyPass>());
            }, error);
        return registry;
    }

    const PassRegistry &defaultPassRegistry()
    {
        static const PassRegistry registry = makeDefaultPassRegistry();
        return registry;
    }

} // namespace wolvrix::lib::grhsim
