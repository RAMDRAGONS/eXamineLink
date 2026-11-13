#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "model/document.hpp"
#include "model/edit.hpp"

// The editable text form: a whole database in one XML file, or one user in a
// file of its own, laid out like the per-user files the Nintendo link editors
// kept (Work/ELink2/UserFile/<User>.elink2.xml).
namespace xl::xml {

    void save_database(const Document& doc, const std::string& path);
    // Problems that did not stop the load (unknown attributes, missing
    // definitions) are added to `warnings`.
    Document load_database(const std::string& path, std::vector<std::string>& warnings);

    // File name the editors would give a user's file, such as "Player.elink2.xml".
    std::string user_file_name(const Document& doc, const Node& user);

    // A user file holds the user plus copies of every database-wide entry it
    // points at (parameter sets, conditions, curves, pool records...), so it can
    // be imported into another database of the same kind, in any version: a
    // user of another version is converted on reading, its losses reported as
    // warnings.
    void export_user(const Document& doc, const Node& user, const std::string& path);

    struct UserImport {
        std::unique_ptr<Node> user;
        // Database-wide entries the user refers to: root list, the id the file
        // gave the entry, and the entry.
        struct Shared {
            int list;
            s64 id;
            std::unique_ptr<Node> node;
        };
        std::vector<Shared> shared;
        // Values whose meaning depends on the target database (parameter
        // definitions by name), parsed once the nodes sit in it.
        std::map<const Node*, std::vector<std::pair<int, std::string>>> deferred;
        std::vector<std::string> warnings;
    };

    UserImport read_user(const Document& doc, const std::string& path);

    // A copy of `user` of `doc` with the database-wide entries it points at, for
    // apply_user_import into another database of the same format and version.
    UserImport user_import_from(const Document& doc, const Node& user);

    // Inserts the user (replacing users[replace] when replace >= 0), reusing
    // identical database-wide entries and appending the rest. Returns the user's
    // index; everything is one undo step.
    int apply_user_import(Document& doc, EditStack& edits, UserImport imp, int replace);

    // Index of the user `imp` would replace: same name, or same hash for XLink.
    int find_matching_user(const Document& doc, const UserImport& imp);

    // True when the two nodes hold the same visible data, recursively.
    bool same_content(const Node& a, const Node& b);

} // namespace xl::xml
