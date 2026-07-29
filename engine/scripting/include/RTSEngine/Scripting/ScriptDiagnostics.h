#pragma once

#include <realscript/diagnostics/Diagnostic.h>

#include <string>
#include <utility>
#include <vector>

namespace rts::scripting {

enum class ScriptDiagnosticSeverity {
    Warning,
    Error
};

struct ScriptDiagnostic final {
    ScriptDiagnosticSeverity severity{ScriptDiagnosticSeverity::Error};
    std::string code;
    std::string message;
    std::string sourceName;
};

class ScriptDiagnostics final {
public:
    void add(std::string code,
             std::string message,
             ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error,
             std::string sourceName = {}) {
        items_.push_back({severity,
                          std::move(code),
                          std::move(message),
                          std::move(sourceName)});
    }

    void append(const realscript::diagnostics::DiagnosticBag& diagnostics) {
        for (const auto& diagnostic : diagnostics.items()) {
            add(diagnostic.code,
                diagnostic.message,
                diagnostic.severity ==
                        realscript::diagnostics::DiagnosticSeverity::Warning
                    ? ScriptDiagnosticSeverity::Warning
                    : ScriptDiagnosticSeverity::Error,
                diagnostic.sourceName);
        }
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        for (const auto& item : items_) {
            if (item.severity == ScriptDiagnosticSeverity::Error) return true;
        }
        return false;
    }

    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] const std::vector<ScriptDiagnostic>& items() const noexcept {
        return items_;
    }

private:
    std::vector<ScriptDiagnostic> items_;
};

} // namespace rts::scripting
