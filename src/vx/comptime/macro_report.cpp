/**
 * @file macro_report.cpp
 * @brief Lo que el compilador informa de cada `@Macro`.
 * @see vx/comptime/macro_report.h
 */
#include "vx/comptime/macro_report.h"

#include "vx/comptime/comptime_vm.h" // ComptimeRuntime::ExpectationView
#include "vx/diag/diag_catalog.h"
#include "vx/type_checker.h"

namespace vx {

void collect_macro_expectations(const TypeChecker &tc, MacroExpectations &out) {
    const ComptimeRuntime &ctr = tc.comptime_runtime();
    const size_t n = ctr.expectation_count();
    out.reserve(out.size() + n);
    for (size_t i = 0; i < n; ++i) {
        const ComptimeRuntime::ExpectationView v = ctr.expectation_at(i);
        if (!v.macro_name || !v.args || !v.expected_str || !v.src_loc) continue;
        MacroExpectation e;
        e.macro_name = util::InternedName::intern(*v.macro_name);
        e.args.assign(v.args->begin(), v.args->end());
        e.expected_str = *v.expected_str;
        e.src_loc = *v.src_loc;
        out.push_back(std::move(e));
    }
}

std::string macro_skip_text(const MacroSkipReason &why) {
    const std::string text =
        diag::format(why.code, {why.subject.empty() ? std::string()
                                                    : why.subject.str()});
    if (why.via.empty()) return text;
    return diag::format("VXT131", {why.via.str(), text});
}

} // namespace vx
