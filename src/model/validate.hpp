#pragma once

#include <string>
#include <vector>

#include "model/document.hpp"

namespace xl {

    // The grades the link libraries report errors in.
    enum class Severity { Information, Warning, Error, Fatal };

    struct Issue {
        Severity severity;
        std::string text;
        const Node* node;
    };

    // Checks references, counts and version limits. With `try_write`, also
    // serializes the database and reports anything the writer refuses as Fatal.
    std::vector<Issue> validate(const Document& doc, bool try_write);

    std::string severity_name(Severity s);

} // namespace xl
