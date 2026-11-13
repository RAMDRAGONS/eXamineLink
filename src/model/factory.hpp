#pragma once

#include <memory>

#include "model/document.hpp"

namespace xl {

// A fresh element for `list`, filled so the database stays valid where that
// is possible: new XLink users get every user parameter, new calls a random
// GUID, new parameters the first definition the set does not use yet, new
// EffectLink effects the parameter words their user expects.
std::unique_ptr<Node> make_element(const Document &doc, const NodeList &list);

// Keeps derived keys in step when a name changes: an XLink user's hash and a
// call's key-name hash follow their names when they matched them before.
// Returns the extra fields to set alongside the edit.
struct FieldUpdate {
   Node *node;
   int field;
   Value value;
};
std::vector<FieldUpdate> follow_up_edits(const Document &doc, Node &n, int field, const Value &next);

} // namespace xl
