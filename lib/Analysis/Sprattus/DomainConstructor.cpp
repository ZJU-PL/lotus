#include "Analysis/Sprattus/DomainConstructor.h"

#include "Analysis/Sprattus/AbstractValue.h"
#include "Analysis/Sprattus/ResultStore.h"
#include "Analysis/Sprattus/Config.h"
#include "Analysis/Sprattus/ParamStrategy.h"
#include "Analysis/Sprattus/domains/Product.h"

#include <map>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

namespace sprattus
{
namespace configparser
{

// Specialization of Config::get for DomainConstructor that understands
// AbstractDomain and ParamStrategy.* entries from the plain-text .conf files.
template <>
DomainConstructor Config::get(const char* module, const char* key,
                              DomainConstructor default_value) const
{
    // Helper to split comma-separated domain lists
    auto splitCommaSeparated = [](const std::string& s)
        -> std::vector<std::string> {
        std::vector<std::string> out;
        llvm::StringRef sr(s);
        llvm::SmallVector<llvm::StringRef, 4> pieces;
        sr.split(pieces, ',');
        for (auto piece : pieces) {
            auto trimmed = piece.trim();
            if (!trimmed.empty())
                out.push_back(trimmed.str());
        }
        return out;
    };

    // Helper to look up a registered domain by name
    auto findDomainByName = [](const std::string& name)
        -> DomainConstructor {
        const auto& all = DomainConstructor::all();
        for (const auto& d : all) {
            if (d.name() == name)
                return d;
        }
        return DomainConstructor();
    };

    // Very small parser for ParamStrategy strings used in the .conf files.
    // Supported:
    //   NonPointerPairs(symmetric=true|false)
    //   NonPointers
    //   AllValuePairs(symmetric=true|false)
    auto parseParamStrategy = [](const std::string& spec) -> ParamStrategy {
        llvm::StringRef sr(spec);
        sr = sr.trim();

        auto parseSymmetric = [](llvm::StringRef args) {
            // Look for "symmetric=true" (ignoring whitespace and case)
            std::string lower = args.lower();
            return llvm::StringRef(lower).contains("symmetric=true");
        };

        if (sr.startswith("NonPointerPairs")) {
            bool symmetric = false;
            auto lparen = sr.find('(');
            auto rparen = sr.rfind(')');
            if (lparen != llvm::StringRef::npos && rparen != llvm::StringRef::npos &&
                rparen > lparen + 1) {
                auto args = sr.slice(lparen + 1, rparen);
                symmetric = parseSymmetric(args);
            }
            return ParamStrategy::NonPointerPairs(symmetric);
        }

        if (sr.startswith("NonPointers")) {
            return ParamStrategy::NonPointers();
        }

        if (sr.startswith("AllValuePairs")) {
            bool symmetric = false;
            auto lparen = sr.find('(');
            auto rparen = sr.rfind(')');
            if (lparen != llvm::StringRef::npos && rparen != llvm::StringRef::npos &&
                rparen > lparen + 1) {
                auto args = sr.slice(lparen + 1, rparen);
                symmetric = parseSymmetric(args);
            }
            return ParamStrategy::AllValuePairs(symmetric);
        }

        // Fallback: use a very generic strategy
        return ParamStrategy::AllValues();
    };

    // Build the key used in the config map
    std::string fullKey = std::string(module) + "." + std::string(key);

    // Special-case: for domains we also support plain "AbstractDomain = ..."
    // in addition to "AbstractDomain.Variant = ...".
    std::string value;
    auto it = ConfigDict_->find(fullKey);
    if (it != ConfigDict_->end()) {
        value = it->second;
    } else if (std::string(module) == "AbstractDomain") {
        auto it2 = ConfigDict_->find("AbstractDomain");
        if (it2 != ConfigDict_->end())
            value = it2->second;
    }

    if (value.empty()) {
        return default_value;
    }

    // Parse the domain list
    auto domainNames = splitCommaSeparated(value);
    if (domainNames.empty())
        return default_value;

    // Collect per-domain parametrization strategies from the config.
    // Keys look like:
    //   ParamStrategy.NumRels.Signed = NonPointerPairs(symmetric=true)
    //   ParamStrategy.NumRels.Zero   = NonPointers
    std::map<std::string, ParamStrategy> paramStrategies;
    for (const auto& kv : *ConfigDict_) {
        llvm::StringRef k(kv.first);
        if (!k.startswith("ParamStrategy."))
            continue;

        llvm::StringRef rest = k.drop_front(strlen("ParamStrategy."));
        rest = rest.trim(); // e.g. "NumRels.Signed"
        if (rest.empty())
            continue;

        ParamStrategy ps = parseParamStrategy(kv.second);
        auto keyStr = rest.str();
        auto itPs = paramStrategies.find(keyStr);
        if (itPs == paramStrategies.end()) {
            paramStrategies.emplace(keyStr, ps);
        } else {
            itPs->second = ps;
        }
    }

    std::vector<DomainConstructor> domains;
    domains.reserve(domainNames.size());

    for (const auto& name : domainNames) {
        DomainConstructor dom = findDomainByName(name);
        if (dom.isInvalid()) {
            llvm::errs() << "Warning: Unknown abstract domain '" << name
                         << "' in configuration; ignoring.\n";
            continue;
        }

        // Apply parametrization if there is a matching ParamStrategy entry.
        auto itPsExact = paramStrategies.find(name);
        if (itPsExact != paramStrategies.end()) {
            dom = dom.parameterize(itPsExact->second);
        } else {
            // Also allow prefixes like "NumRels" to match "NumRels.Unsigned"
            // or "NumRels.Signed".
            for (const auto& kv : paramStrategies) {
                llvm::StringRef keyName(kv.first);
                if (keyName == name)
                    continue;
                // If the param-strategy key is a prefix of the domain name,
                // treat it as applicable.
                if (name.size() > kv.first.size() &&
                    llvm::StringRef(name).startswith(keyName) &&
                    name[keyName.size()] == '.') {
                    dom = dom.parameterize(kv.second);
                    break;
                }
            }
        }

        domains.push_back(dom);
    }

    if (domains.empty())
        return default_value;
    if (domains.size() == 1)
        return domains.front();

    return DomainConstructor::product(domains);
}

} // namespace configparser
std::vector<DomainConstructor>* DomainConstructor::KnownDomains_;

DomainConstructor::DomainConstructor(const configparser::Config& config)
{
    *this = config.get<DomainConstructor>("AbstractDomain", "Variant",
                                          DomainConstructor());

    assert(!isInvalid()); // default value is never used in config.get
}

unique_ptr<AbstractValue>
DomainConstructor::makeBottom(const DomainConstructor::args& args) const
{
    DomainConstructor dc = autoParameterize(0);
    return dc.FactoryFunc_(args);
}

unique_ptr<AbstractValue>
DomainConstructor::makeBottom(const FunctionContext& fctx,
                              llvm::BasicBlock* loc, bool after) const
{
    DomainConstructor dc = autoParameterize(0);
    DomainConstructor::args args;
    args.fctx = &fctx;
    args.location = loc;
    args.is_after_bb = after;
    return dc.FactoryFunc_(args);
}

DomainConstructor DomainConstructor::autoParameterize(int desired_arity) const
{
    assert(Arity_ >= desired_arity && desired_arity >= 0);
    DomainConstructor dc = *this;

    while (dc.Arity_ > desired_arity) {
        if (dc.Arity_ >= desired_arity + 2)
            dc = dc.parameterize(ParamStrategy::AllValuePairs());
        else
            dc = dc.parameterize(ParamStrategy::AllValues());
    }

    assert(dc.Arity_ == desired_arity);
    return dc;
}

DomainConstructor
DomainConstructor::parameterize(const ParamStrategy& pstrategy)
{
    factory_func_t factory_func = FactoryFunc_;

    auto f = [pstrategy, factory_func](const DomainConstructor::args& args) {
        auto result = make_unique<domains::Product>(*args.fctx);

        for (auto& pvec : pstrategy.generateParams(args)) {
            DomainConstructor::args local_args = args;

            assert((int)pvec.size() == pstrategy.arity());
            for (Expression& p : pvec) {
                local_args.parameters.push_back(p);
            }

            result->add(factory_func(local_args));
        }

        result->finalize();
        return unique_ptr<AbstractValue>(std::move(result));
    };

    int new_arity = Arity_ - pstrategy.arity();
    assert(new_arity >= 0);
    return DomainConstructor(Name_, Description_, new_arity, f);
}

DomainConstructor
DomainConstructor::product(std::vector<DomainConstructor> doms)
{
    assert(doms.size() > 0);

    // arity of the result is the minimum of arities of components
    int arity = INT_MAX;
    for (auto& d : doms) {
        if (d.arity() < arity)
            arity = d.arity();
    }

    // components with greater arity need to be parameterized with default
    // strategies
    for (auto& d : doms) {
        d = d.autoParameterize(arity);
    }

    auto f = [doms](const DomainConstructor::args& args) {
        auto prod = make_unique<domains::Product>(*args.fctx);
        for (auto& d : doms) {
            prod->add(d.FactoryFunc_(args));
        }
        prod->finalize();
        return unique_ptr<AbstractValue>(std::move(prod));
    };

    // TODO: provide a more informative name based on names of components
    return DomainConstructor("product", "", arity, f);
}

DomainConstructor::DomainConstructor(std::string name, std::string desc,
                                     alt_ffunc_0 factory_func)
    : DomainConstructor(
          name, desc, 0, [factory_func](const DomainConstructor::args& args) {
              return factory_func(*args.fctx, args.location, args.is_after_bb);
          })
{
}

DomainConstructor::DomainConstructor(std::string name, std::string desc,
                                     alt_ffunc_1 factory_func)
    : DomainConstructor(name, desc, 1,
                        [factory_func](const DomainConstructor::args& args) {
                            return factory_func(args.parameters[0], args);
                        })
{
}

DomainConstructor::DomainConstructor(std::string name, std::string desc,
                                     alt_ffunc_2 factory_func)
    : DomainConstructor(
          name, desc, 2, [factory_func](const DomainConstructor::args& args) {
              return factory_func(args.parameters[0], args.parameters[1], args);
          })
{
}
} // namespace sprattus
