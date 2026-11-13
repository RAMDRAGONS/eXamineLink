#pragma once

#include <optional>
#include <string>

#include "core/text.hpp"
#include "model/node.hpp"

// Text forms of field values, shared by the XML files and the editor.
namespace xl {

    // The kind a field currently has: its declared type, or for Dynamic fields
    // the kind its siblings select. Enum fields report UInt.
    DynKind field_kind(const Node& n, int field);

    // Strings come back as UTF-8; `enc` is the database's own encoding.
    std::string value_to_text(const Node& n, int field, Encoding enc);
    // Parses `text` into `out` for the field; returns the error message on failure.
    std::optional<std::string> text_to_value(
        const Node& n, int field, const std::string& text, Encoding enc, Value& out);

    std::string float_text(f32 v);
    std::string double_text(f64 v);

} // namespace xl
