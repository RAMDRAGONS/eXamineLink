#pragma once

#include <string>
#include <vector>

#include "model/document.hpp"

namespace xl {

// Versions a document can be saved as: its own family's writable versions.
std::vector<const Version *> conversion_targets(const Document &doc);

// A copy of `doc` ready to write as `target`'s version in its byte order and
// encoding. `losses` describes everything the target cannot hold, which the
// copy no longer carries.
Document convert(const Document &doc, const Target &target, std::vector<std::string> &losses);

// `version` written by default: in the document's own byte order and encoding
// when the version stays the same, otherwise in those of a preset using it,
// the one sharing the document's byte order and then encoding if several do,
// falling back to the document's own.
Target default_output(const Version &version, const Document &doc);

} // namespace xl
