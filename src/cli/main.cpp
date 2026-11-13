// Command-line front end: conversions, XML export and import, validation, and
// the round-trip self test the build's tests run.

#include <array>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/crc32.hpp"
#include "model/bridge.hpp"
#include "model/convert.hpp"
#include "model/document.hpp"
#include "model/dslink_schema.hpp"
#include "model/edit.hpp"
#include "model/validate.hpp"
#include "model/value_text.hpp"
#include "model/xml_io.hpp"
#include "model/xlink_schema.hpp"

using namespace xl;

namespace {

int usage() {
  std::fputs(
      "usage: examinelink-cli <command> ...\n"
      "  info FILE...                  summarize databases\n"
      "  check FILE...                 verify binary, XML and per-user XML round trips\n"
      "  validate FILE                 list problems\n"
      "  to-xml FILE OUT.xml           export the whole database\n"
      "  from-xml IN.xml OUT           build a database; .szs and .zs outputs are compressed\n"
      "  convert FILE OUT VERSION|PRESET [big|little] [shift-jis|utf-8]\n"
      "                                save as another version, byte order or text encoding;\n"
      "                                a version takes the byte order and encoding of a\n"
      "                                preset using it, the one nearest the input's, or the\n"
      "                                input's own when the version stays the same\n"
      "  presets                       list the game presets convert accepts\n"
      "  matches FILE REFERENCE        convert FILE to REFERENCE's version, byte order and\n"
      "                                encoding and compare the two byte for byte\n"
      "  export-user FILE USER OUT.xml export one user by name or hash\n"
      "  import-user FILE IN.xml OUT   add or replace a user from its XML file\n"
      "  bridge-check TARGET SOURCE    import every user of SOURCE into TARGET and check the\n"
      "                                result reads back; an EffectLink or SoundLink source's\n"
      "                                users must then come back playing the same\n"
      "  import-users FILE SOURCE OUT [USER...] [--replace] [--slot NAME]\n"
      "               [--conditions P1,P2,P3]\n"
      "                                copy users (all when none is named) from another\n"
      "                                database of the same link system: any XLink version,\n"
      "                                or EffectLink to and from ELink2 and SoundLink to and\n"
      "                                from SLink2. --replace overwrites users of the same\n"
      "                                name; --slot names the action slot older users' actions\n"
      "                                go in; --conditions names the XLink properties for the\n"
      "                                three EffectLink conditions. Both default to what the\n"
      "                                databases suggest.\n",
      stderr);
  return 2;
}

std::string summary(const Document& d) {
  std::string s = format_name(d.format) + " v" + std::to_string(d.version()) + ", " +
                  (d.endian == Endian::Big ? "big" : "little") + "-endian, " +
                  (d.encoding == Encoding::ShiftJis ? "Shift-JIS" : "UTF-8") + ", " + describe(d.layers);
  const NodeList& users = d.root->list(d.root->list_count() - 1);
  s += ", " + std::to_string(users.size()) + " users";
  for (const Preset& p : presets())
    if (preset_version(p, d.format, d.system()) == d.version() && p.endian == d.endian && p.encoding == d.encoding) {
      s += " (as " + p.name + ")";
      break;
    }
  return s;
}

Document load_any(const std::string& path) {
  if (path.size() > 4 && path.ends_with(".xml")) {
    std::vector<std::string> warnings;
    Document d = xml::load_database(path, warnings);
    for (const auto& w : warnings)
      std::fprintf(stderr, "warning: %s\n", w.c_str());
    return d;
  }
  return load_file(path);
}

void save_any(const std::string& path, Document& d) {
  d.layers = layers_for_path(path, d);
  save_file(path, d);
}

// Path to the first node whose visible content differs, for diagnostics.
std::string first_difference(const Node& a, const Node& b, Encoding enc) {
  const Schema& sc = a.schema();
  if (&sc != &b.schema())
    return "schema " + sc.name + " vs " + b.schema().name;
  for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f)
    if (!sc.fields[f].hidden && !(a.value(f) == b.value(f)))
      return to_utf8(a.label(), enc) + "." + sc.fields[f].name + ": " + value_to_text(a, f, enc) + " vs " +
             value_to_text(b, f, enc);
  for (int l = 0; l < a.list_count(); ++l) {
    if (a.list(l).size() != b.list(l).size())
      return to_utf8(a.label(), enc) + "." + a.list(l).def().name + " size " + std::to_string(a.list(l).size()) +
             " vs " + std::to_string(b.list(l).size());
    for (int k = 0; k < a.list(l).size(); ++k) {
      const std::string d = first_difference(a.list(l).at(k), b.list(l).at(k), enc);
      if (!d.empty())
        return d;
    }
  }
  return {};
}

// Compares an edited model with the database it saved to, allowing for what
// a format normalizes on save: parameter sets written in definition order
// (and, in version 19, filled out with defaults), users sorted by hash and
// known only by hash in later XLink versions, untyped version 19 conditions.
class CanonCompare {
public:
  CanonCompare(const Document& a, const Document& b) : A(a), B(b) {
    xlink_ = a.format == Format::XLink;
    v19_ = xlink_ && a.version() == 19;
  }

  std::string run() {
    if (!node(*A.root, *B.root))
      return why;
    return {};
  }

private:
  bool fail(const std::string& w) {
    if (why.empty())
      why = w;
    return false;
  }
  bool is_param_list(const NodeList& l) const {
    return xlink_ && l.def().element == &xlink::schemas().param;
  }
  u32 user_hash(const Node& u) const {
    return u.s(xlink::user::Name).empty() ? static_cast<u32>(u.i(xlink::user::Hash)) : crc32(u.s(xlink::user::Name));
  }

  bool field(const Node& a, const Node& b, int f) {
    const FieldDef& fd = a.schema().fields[f];
    if (fd.hidden)
      return true;
    if (xlink_ && &a.schema() == &xlink::schemas().user) {
      if (f == xlink::user::Name && !v19_)
        return true;
      if (f == xlink::user::Hash)
        return v19_ || user_hash(a) == user_hash(b) || fail("user hash differs");
      if (f == xlink::user::BaseName && !v19_)
        return true;
    }
    if (v19_ && &a.schema() == &xlink::schemas().cond &&
        (f == xlink::cond::PropertyType || f == xlink::cond::LocalPropertyEnumNameIdx || f == xlink::cond::IsSolved ||
         f == xlink::cond::IsGlobal))
      return true;
    if (xlink_ && &a.schema() == &xlink::schemas().call && f == xlink::call::KeyNameHash && v19_)
      return true;
    const bool ref = fd.target && field_kind(a, f) == DynKind::Ref;
    const NodeList* ta = ref ? fd.target(a) : nullptr;
    // Version 19 condition tables are rebuilt from references, so they are
    // compared through what points at them.
    if (ta && v19_ && ta->owner() == A.root.get() && ta->index() == xlink::file::Conditions) {
      const NodeList* tb = fd.target(b);
      const s64 ia = a.i(f), ib = b.i(f);
      if ((ia < 0) != (ib < 0))
        return fail(a.label() + "." + fd.name + " set on one side only");
      if (ia < 0)
        return true;
      return cond(ta->at(static_cast<int>(ia)), tb->at(static_cast<int>(ib)));
    }
    if (!(a.value(f) == b.value(f)))
      return fail(to_utf8(a.label(), A.encoding) + "." + fd.name + ": " + value_to_text(a, f, A.encoding) + " vs " +
                  value_to_text(b, f, B.encoding));
    return true;
  }

  bool cond(const Node& a, const Node& b) {
    using namespace xlink;
    if (a.i(cond::ParentType) != b.i(cond::ParentType))
      return fail("condition type differs");
    if (a.i(cond::ParentType) == CT_Switch) {
      if (a.i(cond::CompareType) != b.i(cond::CompareType))
        return fail("condition compare differs");
      const bool ea = a.i(cond::PropertyType) == PR_Enum, eb = b.i(cond::PropertyType) == PR_Enum;
      if (ea && eb)
        return a.s(cond::Value) == b.s(cond::Value) || fail("condition enum differs");
      if (!ea && !eb)
        return a.i(cond::Value) == b.i(cond::Value) || fail("condition value differs");
      return true;
    }
    for (const int f : {cond::Weight, cond::BlendMin, cond::ContinueOnFade})
      if (a.i(f) != b.i(f))
        return fail("condition differs");
    return true;
  }

  bool params(const NodeList& a, const NodeList& b) {
    std::map<s64, const Node*> mb;
    for (const auto& p : b)
      mb.emplace(p->i(xlink::param::Def), p.get());
    std::set<s64> seen;
    for (const auto& p : a) {
      const s64 d = p->i(xlink::param::Def);
      if (!seen.insert(d).second)
        continue;
      const auto it = mb.find(d);
      if (it == mb.end())
        return fail("parameter " + std::to_string(d) + " lost");
      const Node& q = *it->second;
      if (p->i(xlink::param::RefType) != q.i(xlink::param::RefType) ||
          !(p->value(xlink::param::Value) == q.value(xlink::param::Value)))
        return fail("parameter " + std::to_string(d) + " value differs");
    }
    if (!v19_ && mb.size() != seen.size() && &a.owner()->schema() != &xlink::schemas().user)
      return fail("parameter set gained entries");
    return true;
  }

  bool node(const Node& a, const Node& b) {
    if (&a.schema() != &b.schema())
      return fail("schema differs");
    for (int f = 0; f < static_cast<int>(a.schema().fields.size()); ++f)
      if (!field(a, b, f))
        return false;
    for (int l = 0; l < a.list_count(); ++l) {
      const NodeList& la = a.list(l);
      const NodeList& lb = b.list(l);
      if (is_param_list(la)) {
        if (!params(la, lb))
          return false;
        continue;
      }
      if (v19_ && &a == A.root.get() && l == xlink::file::Conditions)
        continue;
      if (la.size() != lb.size())
        return fail(to_utf8(a.label(), A.encoding) + "." + la.def().name + " size " + std::to_string(la.size()) +
                    " vs " + std::to_string(lb.size()));
      std::vector<int> order(la.size());
      for (int k = 0; k < la.size(); ++k)
        order[k] = k;
      if (xlink_ && !v19_ && &a == A.root.get() && l == xlink::file::Users)
        std::stable_sort(order.begin(), order.end(),
                         [&](int x, int y) { return user_hash(la.at(x)) < user_hash(la.at(y)); });
      for (int k = 0; k < la.size(); ++k)
        if (!node(la.at(order[k]), lb.at(k)))
          return false;
    }
    return true;
  }

  const Document& A;
  const Document& B;
  bool xlink_ = false, v19_ = false;
  std::string why;
};

// Random structural edits. With `keep_valid`, only edits that cannot break
// a valid database: copies of entries that no count depends on, removal of
// entries nothing references, and moves in lists no range covers.
int fuzz(Document& doc, EditStack& edits, std::mt19937& rng, int count, bool keep_valid) {
  const auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
  int ops = 0, serial = 0;
  for (int k = 0; k < count; ++k) {
    std::vector<NodeList*> lists;
    walk(*doc.root, [&](Node& n) {
      for (int l = 0; l < n.list_count(); ++l)
        lists.push_back(&n.list(l));
    });
    NodeList& list = *lists[pick(static_cast<int>(lists.size()))];
    const int size = list.size();
    const Schema* el = list.def().element;
    const std::string& lname = list.def().name;
    const bool counted = el == &xlink::schemas().param || lname == "Params" || lname == "Axis1" ||
                         lname == "Axis2" || lname == "Cells";
    // Games binary-search local property names, so these lists stay sorted
    // and free of repeats.
    const bool sorted = doc.format == Format::XLink && (lname == "LocalProperties" || lname == "LocalPropertyNames");
    const int op = pick(3);
    if (keep_valid && sorted && op != 1)
      continue;
    if (op == 0 && size > 0) {
      if (keep_valid && counted)
        continue;
      const int src = pick(size);
      auto copy = list.at(src).clone();
      // XLink users are keyed by name (or its hash) and must stay distinct.
      if (doc.format == Format::XLink && el == &xlink::schemas().user)
        copy->set_s(xlink::user::Name, "FuzzUser" + std::to_string(serial++));
      edits.insert(list, keep_valid ? src + 1 : pick(size + 1), std::move(copy));
    } else if (op == 1 && size > 0) {
      const int at = pick(size);
      if (keep_valid && (lname == "Params" || lname == "Axis1" || lname == "Axis2" || lname == "Cells" ||
                         count_references(*doc.root, list, at) > 0))
        continue;
      edits.remove(list, at);
    } else if (op == 2 && size > 1) {
      if (keep_valid) {
        bool ranged = counted;
        walk_const(*list.owner(), [&](const Node& n) {
          const Schema& sc = n.schema();
          for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f)
            if (sc.fields[f].type == FieldType::RangeStart && sc.fields[f].target && sc.fields[f].target(n) == &list)
              ranged = true;
        });
        if (ranged)
          continue;
      }
      edits.move(list, pick(size), pick(size));
    } else {
      continue;
    }
    ++ops;
  }
  return ops;
}

// Compares two users as the game sees them: references into database-wide
// tables are compared by the content they reach, not by index.
// On a mismatch `why` names the first field or list that differs.
bool equivalent(const Node& a, const Node& ra, const Node& b, const Node& rb, std::string& why, int depth = 0) {
  if (&a.schema() != &b.schema()) {
    why = "schema " + a.schema().name + " vs " + b.schema().name;
    return false;
  }
  const Schema& sc = a.schema();
  for (int f = 0; f < static_cast<int>(sc.fields.size()); ++f) {
    const FieldDef& fd = sc.fields[f];
    if (fd.hidden)
      continue;
    const std::string where = sc.name + "." + fd.name;
    const bool ref = fd.target && field_kind(a, f) == DynKind::Ref;
    const NodeList* ta = ref ? fd.target(a) : nullptr;
    if (ta && ta->owner() == &ra && fd.type != FieldType::RangeEnd && fd.type != FieldType::RangeStart) {
      const NodeList* tb = fd.target(b);
      const s64 ia = a.i(f), ib = b.i(f);
      if ((ia < 0) != (ib < 0)) {
        why = where + " is set on one side only";
        return false;
      }
      if (ia < 0)
        continue;
      if (!tb || ia >= ta->size() || ib >= tb->size() || depth > 4) {
        why = where + " points outside its table";
        return false;
      }
      if (!equivalent(ta->at(static_cast<int>(ia)), ra, tb->at(static_cast<int>(ib)), rb, why, depth + 1)) {
        why = where + " > " + why;
        return false;
      }
      continue;
    }
    if (!(a.value(f) == b.value(f))) {
      why = where;
      return false;
    }
  }
  for (int l = 0; l < a.list_count(); ++l) {
    const std::string where = sc.name + "." + a.list(l).def().name;
    if (a.list(l).size() != b.list(l).size()) {
      why = where + " size " + std::to_string(a.list(l).size()) + " vs " + std::to_string(b.list(l).size());
      return false;
    }
    for (int k = 0; k < a.list(l).size(); ++k)
      if (!equivalent(a.list(l).at(k), ra, b.list(l).at(k), rb, why, depth)) {
        why = where + "[" + std::to_string(k) + "] > " + why;
        return false;
      }
  }
  return true;
}

int check(const std::vector<std::string>& files) {
  int failures = 0;
  QTemporaryDir tmp;
  for (const std::string& path : files) {
    std::printf("%s\n", path.c_str());
    const auto step = [&](const char* what, const std::function<std::string()>& fn) {
      try {
        const std::string r = fn();
        const bool ok = r.empty();
        std::printf("  %-34s %s\n", what, ok ? "ok" : r.c_str());
        failures += !ok;
      } catch (const std::exception& e) {
        std::printf("  %-34s FAILED: %s\n", what, e.what());
        ++failures;
      }
    };
    Buffer original;
    Document doc;
    step("load", [&] {
      const Buffer data = read_all(path);
      Unwrapped u = unwrap(data, [](Bytes b) { return is_database(b); }, folder_dictionaries());
      original = u.payload;
      doc = read_database(original);
      doc.layers = std::move(u.layers);
      return std::string();
    });
    if (!doc.root)
      continue;
    std::printf("  %s\n", summary(doc).c_str());
    step("binary round trip is byte-identical", [&] {
      return write_database(doc) == original ? std::string() : std::string("output differs");
    });
    step("container round trip", [&] {
      const Buffer packed = wrap(doc.layers, write_database(doc));
      Unwrapped u = unwrap(packed, [](Bytes b) { return is_database(b); }, folder_dictionaries());
      return u.payload == original ? std::string() : std::string("payload differs after repacking");
    });
    const std::string xml_path = tmp.filePath("db.xml").toStdString();
    step("XML round trip is byte-identical", [&] {
      xml::save_database(doc, xml_path);
      std::vector<std::string> warnings;
      Document back = xml::load_database(xml_path, warnings);
      if (!warnings.empty())
        return "warning: " + warnings.front();
      return write_database(back) == original ? std::string() : std::string("output differs");
    });
    step("per-user XML export and import", [&] {
      Document copy = doc.clone();
      EditStack edits(copy.root.get());
      NodeList& users = copy.root->list(copy.root->list_count() - 1);
      const std::string user_path = tmp.filePath("user.xml").toStdString();
      // An even sample keeps the check quick on databases with thousands of users.
      const int step_size = std::max(1, users.size() / 60);
      for (int k = 0; k < users.size(); k += step_size) {
        const Node& original_user = doc.root->list(doc.root->list_count() - 1).at(k);
        xml::export_user(copy, users.at(k), user_path);
        xml::UserImport imp = xml::read_user(copy, user_path);
        if (!imp.warnings.empty())
          return to_utf8(original_user.label(), doc.encoding) + ": " + imp.warnings.front();
        const int at = xml::apply_user_import(copy, edits, std::move(imp), k);
        if (at != k)
          return std::string("user moved");
        std::string why;
        if (!equivalent(original_user, *doc.root, users.at(k), *copy.root, why))
          return "user " + to_utf8(original_user.label(), doc.encoding) + " changed: " + why;
      }
      write_database(copy);
      edits.undo();
      return std::string();
    });
    step("structural edits, undo and redo", [&] {
      // Clone-inserts at the front of every list of a few users and of the
      // database tables shift every reference; the result must still write,
      // read back identically, and undo to the original bytes.
      Document copy = doc.clone();
      EditStack edits(copy.root.get());
      std::vector<NodeList*> lists;
      for (int l = 0; l < copy.root->list_count(); ++l)
        lists.push_back(&copy.root->list(l));
      NodeList& users = copy.root->list(copy.root->list_count() - 1);
      for (int k = 0; k < users.size(); k += std::max(1, users.size() / 8))
        for (int l = 0; l < users.at(k).list_count(); ++l)
          lists.push_back(&users.at(k).list(l));
      int n = 0;
      // Parameter masks hold 64 asset and 32 trigger definitions; a database
      // already at that limit has no room for another.
      const auto full = [&](const NodeList* list) {
        if (copy.format != Format::XLink)
          return false;
        if (list == &copy.root->list(xlink::file::AssetParamDefs))
          return list->size() >= 64;
        return list == &copy.root->list(xlink::file::TriggerParamDefs) && list->size() >= 32;
      };
      for (NodeList* list : lists) {
        if (list->empty() || list == &users || full(list))
          continue;
        edits.insert(*list, 0, list->at(list->size() - 1).clone());
        ++n;
      }
      const Buffer edited = write_database(copy);
      Document reread = read_database(edited);
      if (write_database(reread) != edited) {
        // Find the first insertion that breaks the round trip.
        for (int k = 0; k < n; ++k)
          edits.undo();
        for (int k = 0; k < n; ++k) {
          edits.redo();
          const Buffer b = write_database(copy);
          Document rb = read_database(b);
          const Buffer b2 = write_database(rb);
          if (b2 != b) {
            write_all("/tmp/xl/fail_a.bin", b);
            write_all("/tmp/xl/fail_b.bin", b2);
          }
          if (b2 != b)
            return "edited file does not round trip after edit " + std::to_string(k) + " (" + edits.undo_text() +
                   "): " + first_difference(*copy.root, *rb.root, doc.encoding);
        }
        return std::string("edited file does not round trip");
      }
      for (int k = 0; k < n; ++k)
        edits.undo();
      if (write_database(copy) != original)
        return std::string("undo does not restore the original");
      for (int k = 0; k < n; ++k)
        edits.redo();
      if (write_database(copy) != edited)
        return std::string("redo does not reproduce the edit");
      return std::string();
    });
    // Arbitrary edits may leave a database invalid; saving must then be
    // refused only where validation already says why, and whatever saves
    // must save identically again. Undo restores the original bytes.
    step("random edits: save or explain", [&] {
      std::mt19937 rng(0x5EED);
      for (int round = 0; round < 3; ++round) {
        Document copy = doc.clone();
        EditStack edits(copy.root.get());
        const int ops = fuzz(copy, edits, rng, 100, false);
        int errors = 0;
        std::string first_error;
        for (const Issue& i : validate(copy, false))
          if (i.severity >= Severity::Error && !errors++)
            first_error = i.text;
        Buffer edited;
        try {
          edited = write_database(copy);
        } catch (const std::exception& e) {
          if (!errors)
            return std::string("save failed without a validation error: ") + e.what();
        }
        if (!edited.empty()) {
          const Buffer again = write_database(read_database(edited));
          if (again != edited)
            return "round " + std::to_string(round) + ": saved file does not save identically again";
        }
        for (int k = 0; k < ops; ++k)
          edits.undo();
        if (write_database(copy) != original)
          return "round " + std::to_string(round) + ": undo does not restore the original";
        std::printf("    round %d: %d edits, %s\n", round, ops,
                    errors ? ("refused or flagged: " + first_error.substr(0, 80)).c_str() : "saved");
      }
      return std::string();
    });
    // Edits that keep a database valid must save, and read back as exactly
    // what was edited.
    step("random valid edits read back exactly", [&] {
      std::mt19937 rng(0xC0FFEE);
      const auto error_count = [](const Document& d) {
        int n = 0;
        for (const Issue& i : validate(d, false))
          n += i.severity >= Severity::Error;
        return n;
      };
      const int baseline = error_count(doc);
      for (int round = 0; round < 4; ++round) {
        Document copy = doc.clone();
        EditStack edits(copy.root.get());
        const int ops = fuzz(copy, edits, rng, 400, true);
        const int errors = error_count(copy);
        if (errors > baseline)
          return "round " + std::to_string(round) + ": valid edits produced " + std::to_string(errors - baseline) +
                 " validation errors";
        const Buffer edited = write_database(copy);
        Document reread = read_database(edited);
        reread.encoding = copy.encoding;
        if (write_database(reread) != edited)
          return "round " + std::to_string(round) + ": saved file does not save identically again";
        if (const std::string d = CanonCompare(copy, reread).run(); !d.empty())
          return "round " + std::to_string(round) + ": saved file differs from the edit: " + d;
        for (int k = 0; k < ops; ++k)
          edits.undo();
        if (write_database(copy) != original)
          return "round " + std::to_string(round) + ": undo does not restore the original";
        std::printf("    round %d: %d valid edits saved and read back\n", round, ops);
      }
      return std::string();
    });
    const Endian other = doc.endian == Endian::Big ? Endian::Little : Endian::Big;
    step("byte order swap and back is lossless", [&] {
      Target swap{doc.format, doc.version(), other, doc.encoding};
      std::vector<std::string> losses;
      const Buffer swapped = write_database(convert(doc, swap, losses));
      const Document reread = read_database(swapped);
      if (reread.endian != other)
        return std::string("written file does not read as the other byte order");
      if (!losses.empty())
        return "reported losses: " + losses.front();
      Target back = swap;
      back.endian = doc.endian;
      return write_database(convert(reread, back, losses)) == original ? std::string()
                                                                        : std::string("swapping back changes bytes");
    });
    step("text encoding swap and back keeps every string", [&] {
      const Encoding enc = doc.encoding == Encoding::Utf8 ? Encoding::ShiftJis : Encoding::Utf8;
      Target swap{doc.format, doc.version(), doc.endian, enc};
      std::vector<std::string> losses;
      Document there = read_database(write_database(convert(doc, swap, losses)));
      // Characters the other encoding lacks cannot come back.
      if (!losses.empty())
        return std::string();
      // Detection cannot tell the encodings apart in all-ASCII tables.
      there.encoding = enc;
      Target back = swap;
      back.encoding = doc.encoding;
      Document again = read_database(write_database(convert(there, back, losses)));
      again.encoding = doc.encoding;
      return CanonCompare(doc, again).run();
    });
    for (const Version* t : conversion_targets(doc)) {
      if (t->version == doc.version())
        continue;
      const std::string imported = "users from v" + std::to_string(t->version) + " XML import";
      step(imported.c_str(), [&] {
        // Users exported from the converted database come back through
        // conversion on import; the result must save and read back stably.
        std::vector<std::string> losses;
        const Document there = convert(doc, default_output(*t, doc), losses);
        const NodeList& their_users = there.root->list(there.root->list_count() - 1);
        Document copy = doc.clone();
        EditStack edits(copy.root.get());
        const std::string upath = tmp.filePath("foreign.xml").toStdString();
        for (int k = 0; k < their_users.size(); k += std::max(1, their_users.size() / 6)) {
          xml::export_user(there, their_users.at(k), upath);
          xml::UserImport imp = xml::read_user(copy, upath);
          const int replace = xml::find_matching_user(copy, imp);
          xml::apply_user_import(copy, edits, std::move(imp), replace);
        }
        const Buffer bytes = write_database(copy);
        return write_database(read_database(bytes)) == bytes ? std::string()
                                                             : std::string("database with imported users does not round trip");
      });
      for (const Endian e : {doc.endian, other}) {
        const std::string what = "convert to v" + std::to_string(t->version) +
                                 (e == Endian::Big ? " big-endian" : " little-endian");
        step(what.c_str(), [&] {
          Target to = default_output(*t, doc);
          to.endian = e;
          std::vector<std::string> losses;
          const Buffer bytes = write_database(convert(doc, to, losses));
          const Document reread = read_database(bytes);
          if (reread.endian != e || reread.version() != t->version)
            return std::string("converted file reads as another version or byte order");
          if (write_database(reread) != bytes)
            return std::string("converted file does not round trip");
          return std::string();
        });
      }
    }
  }
  std::printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}

const Node* find_user(const Document& d, const std::string& key) {
  const NodeList& users = d.root->list(d.root->list_count() - 1);
  for (const auto& u : users) {
    if (to_utf8(u->label(), d.encoding) == key)
      return u.get();
  }
  // XLink users may also be named by their hash, written any way strtoul
  // reads, or by a name the database stores only as its hash.
  if (d.format == Format::XLink && !key.empty()) {
    const u32 named = crc32(from_utf8(key, d.encoding));
    for (const auto& u : users)
      if (static_cast<u32>(u->i(xlink::user::Hash)) == named)
        return u.get();
    char* end = nullptr;
    const unsigned long h = std::strtoul(key.c_str(), &end, 0);
    if (end && *end == '\0')
      for (const auto& u : users)
        if (static_cast<u32>(u->i(xlink::user::Hash)) == static_cast<u32>(h))
          return u.get();
  }
  return nullptr;
}

// Values of the named fields of a record, for comparing records of two databases.
std::vector<Value> values_of(const Node& n, const std::vector<std::string>& fields) {
  std::vector<Value> out;
  for (const auto& f : fields) {
    const int i = n.schema().field_index(f);
    out.push_back(i >= 0 ? n.value(i) : Value{});
  }
  return out;
}

// Differences in what users of an EffectLink or SoundLink database play,
// between `a` and the same users in `b`: the record each call table picks for
// every combination of condition values, and each action's triggers.
std::vector<std::string> playback_differences(const Document& a, const Document& b) {
  using namespace dslink;
  const bool sound = a.format == Format::SoundLink;
  const FamilySchemas& fa = *family_of(*a.root);
  const FamilySchemas& fb = *family_of(*b.root);
  const std::vector<std::string> kept_asset =
      sound ? std::vector<std::string>{"Label", "Volume", "VolumeMax", "Pitch", "PitchMax", "Lpf", "LpfMax", "Pan",
                                       "PanMax", "SurroundPan", "SurroundPanMax", "DelayMin", "DelayMax", "Bone"}
            : std::vector<std::string>{"EmitterSet", "Bone", "Group", "Field0x30", "Field0x32", "Field0x37",
                                       "Field0x38", "Scale", "PositionX", "PositionY", "PositionZ", "RotationX",
                                       "RotationY", "RotationZ", "Red", "Green", "Blue", "Alpha"};
  const std::vector<std::string> kept_trigger =
      sound ? std::vector<std::string>{"StartFrame", "EndFrame", "Volume", "VolumeMax", "Pitch", "PitchMax", "Lpf",
                                       "LpfMax", "DelayMin", "DelayMax"}
            : std::vector<std::string>{"StartFrame", "EndFrame", "Bone", "Scale", "PositionX", "PositionY",
                                       "PositionZ", "RotationX", "RotationY", "RotationZ", "Flags0"};
  const char* start = sound ? "AssetStart" : "EffectStart";
  // A user's record k, through the shared pool for pooled users.
  const auto record = [](const Node& user, bool assets, s64 k) -> const Node* {
    const NodeList* l = assets ? asset_list_of(user) : trigger_list_of(user);
    if (k < 0 || k >= l->size())
      return nullptr;
    const Node& n = l->at(static_cast<int>(k));
    if (&n.schema() != &(assets ? family_of(*n.root())->pool_ref_asset : family_of(*n.root())->pool_ref_trigger))
      return &n;
    const NodeList& pool = n.root()->list(assets ? file::SharedAssets : file::SharedTriggers);
    const s64 i = n.i(pool_ref::Index);
    return i >= 0 && i < pool.size() ? &pool.at(static_cast<int>(i)) : nullptr;
  };
  std::vector<std::string> out;
  std::map<std::string, const Node*> others;
  for (const auto& u : b.root->list(file::Users))
    others[u->s(user::Name)] = u.get();
  for (const auto& u : a.root->list(file::Users)) {
    const auto it = others.find(u->s(user::Name));
    if (it == others.end() || u->i(user::Type) != UT_User) {
      if (u->i(user::Type) == UT_User)
        out.push_back("user " + u->s(user::Name) + " is missing");
      continue;
    }
    const Node& v = *it->second;
    const auto tables = [&](const Node& user, const FamilySchemas& fs) {
      std::map<std::string, const Node*> m;
      for (const auto& c : user.list(user::CallTables))
        m.emplace(c->s(fs.call.schema.field_index("Name")), c.get());
      return m;
    };
    const auto ta = tables(*u, fa), tb = tables(v, fb);
    const auto records = [&](const Node& user, const Node& ct, const FamilySchemas& fs) {
      std::vector<const Node*> r;
      const int s = fs.call.schema.field_index(start);
      s64 first = ct.i(s), last = ct.i(s + 1);
      // A SoundLink call table that does not pick at random plays its first asset.
      if (sound && !(ct.i(fs.call.schema.field_index("Flags2")) & 1))
        last = std::min(last, first);
      for (s64 k = first; k <= last; ++k)
        if (const Node* n = record(user, true, k))
          r.push_back(n);
      return r;
    };
    for (const auto& [name, ct] : ta) {
      const auto jt = tb.find(name);
      if (jt == tb.end()) {
        out.push_back(u->s(user::Name) + "/" + name + ": call table missing");
        continue;
      }
      const auto ra = records(*u, *ct, fa), rb = records(v, *jt->second, fb);
      if (sound) {
        std::vector<std::vector<Value>> va, vb;
        for (const Node* n : ra)
          va.push_back(values_of(*n, kept_asset));
        for (const Node* n : rb)
          vb.push_back(values_of(*n, kept_asset));
        if (va != vb)
          out.push_back(u->s(user::Name) + "/" + name + ": assets differ");
        continue;
      }
      std::array<std::vector<std::string>, 3> seen;
      for (int k = 0; k < 3; ++k) {
        seen[static_cast<std::size_t>(k)] = {"", "\x01"};
        for (const Node* n : ra) {
          const std::string& c = n->s(n->schema().field_index("Condition" + std::to_string(k + 1)));
          auto& vals = seen[static_cast<std::size_t>(k)];
          if (!c.empty() && std::find(vals.begin(), vals.end(), c) == vals.end())
            vals.push_back(c);
        }
      }
      const auto pick = [&](const std::vector<const Node*>& r, const std::array<std::string, 3>& vals) -> const Node* {
        const Node* best = nullptr;
        int best_n = -1;
        for (const Node* n : r) {
          int set = 0;
          bool match = true;
          for (int k = 0; k < 3; ++k) {
            const std::string& c = n->s(n->schema().field_index("Condition" + std::to_string(k + 1)));
            if (!c.empty()) {
              ++set;
              match = match && c == vals[static_cast<std::size_t>(k)];
            }
          }
          if (match && set > best_n) {
            best = n;
            best_n = set;
          }
        }
        return best;
      };
      for (const auto& c1 : seen[0])
        for (const auto& c2 : seen[1])
          for (const auto& c3 : seen[2]) {
            const Node* x = pick(ra, {c1, c2, c3});
            const Node* y = pick(rb, {c1, c2, c3});
            if ((x == nullptr) != (y == nullptr) || (x && values_of(*x, kept_asset) != values_of(*y, kept_asset))) {
              out.push_back(u->s(user::Name) + "/" + name + ": picks a different effect for " + c1 + "," + c2 + "," + c3);
              goto next_table;
            }
          }
    next_table:;
    }
    // Triggers by action, naming their call table; those naming none played nothing.
    const auto triggers = [&](const Node& user, const Node& action, const FamilySchemas& fs) {
      std::vector<std::vector<Value>> r;
      const int ts = fs.action.schema.field_index("TriggerStart");
      std::vector<std::string> names;
      for (const auto& c : user.list(user::CallTables))
        names.push_back(c->s(fs.call.schema.field_index("Name")));
      for (s64 k = action.i(ts); k <= action.i(ts + 1); ++k) {
        const Node* tp = record(user, false, k);
        if (!tp)
          continue;
        const Node& t = *tp;
        std::string call;
        if (const int f = t.schema().field_index("CallName"); f >= 0)
          call = t.s(f);
        else if (const s64 c = t.i(t.schema().field_index("Call")); c >= 0 && c < static_cast<s64>(names.size()))
          call = names[static_cast<std::size_t>(c)];
        if (std::find(names.begin(), names.end(), call) == names.end())
          continue;
        auto row = values_of(t, kept_trigger);
        row.push_back(Value{0, call});
        r.push_back(std::move(row));
      }
      std::sort(r.begin(), r.end(), [](auto& x, auto& y) {
        for (std::size_t k = 0; k < x.size(); ++k)
          if (!(x[k] == y[k]))
            return std::tie(x[k].i, x[k].s) < std::tie(y[k].i, y[k].s);
        return false;
      });
      return r;
    };
    const NodeList& aa = u->list(user::Actions);
    const NodeList& ab = v.list(user::Actions);
    if (aa.size() != ab.size()) {
      out.push_back(u->s(user::Name) + ": action count differs");
      continue;
    }
    for (int k = 0; k < aa.size(); ++k)
      if (triggers(*u, aa.at(k), fa) != triggers(v, ab.at(k), fb))
        out.push_back(u->s(user::Name) + ": triggers of action " + std::to_string(k) + " differ");
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  dictionary_folder();
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty())
    return usage();
  const std::string cmd = args[0];
  args.erase(args.begin());
  try {
    if (cmd == "info") {
      for (const auto& p : args)
        std::printf("%s: %s\n", p.c_str(), summary(load_any(p)).c_str());
      return 0;
    }
    if (cmd == "check" && !args.empty())
      return check(args);
    if (cmd == "validate" && args.size() == 1) {
      const Document d = load_any(args[0]);
      int errors = 0;
      for (const Issue& i : validate(d, true)) {
        std::printf("%s: %s\n", severity_name(i.severity).c_str(), i.text.c_str());
        errors += i.severity >= Severity::Error;
      }
      return errors ? 1 : 0;
    }
    if (cmd == "to-xml" && args.size() == 2) {
      xml::save_database(load_any(args[0]), args[1]);
      return 0;
    }
    if (cmd == "from-xml" && args.size() == 2) {
      Document d = load_any(args[0]);
      save_any(args[1], d);
      return 0;
    }
    if (cmd == "convert" && args.size() >= 3 && args.size() <= 5) {
      const Document d = load_any(args[0]);
      Target t{d.format, d.version(), d.endian, d.encoding};
      if (const Preset* p = find_preset(args[2])) {
        const u32 v = preset_version(*p, d.format, d.system());
        if (!v)
          throw Error(p->name + " has no " + format_name(d.format) + " database of this kind");
        t = {d.format, v, p->endian, p->encoding};
      } else {
        char* end = nullptr;
        const u32 version = static_cast<u32>(std::strtoul(args[2].c_str(), &end, 10));
        if (!end || *end || args[2].empty())
          throw Error("no version or preset named " + args[2] + "; see examinelink-cli presets");
        const Version* found = find_version(d.format, version, d.system());
        if (!found && version != d.version())
          throw Error("no such version for " + format_name(d.format));
        if (found)
          t = default_output(*found, d);
      }
      for (std::size_t k = 3; k < args.size(); ++k) {
        if (args[k] == "big" || args[k] == "little")
          t.endian = args[k] == "big" ? Endian::Big : Endian::Little;
        else if (args[k] == "shift-jis" || args[k] == "utf-8")
          t.encoding = args[k] == "utf-8" ? Encoding::Utf8 : Encoding::ShiftJis;
        else
          throw Error("unknown option " + args[k] + "; expected big, little, shift-jis or utf-8");
      }
      std::vector<std::string> losses;
      Document out = convert(d, t, losses);
      for (const auto& l : losses)
        std::fprintf(stderr, "loss: %s\n", l.c_str());
      save_any(args[1], out);
      return 0;
    }
    if (cmd == "presets" && args.empty()) {
      for (const Preset& p : presets()) {
        std::string versions;
        for (const Preset::Entry& e : p.entries) {
          const Version* v = find_version(e.format, e.version, e.system);
          versions += (versions.empty() ? "" : ", ") + (v ? v->label : format_name(e.format) + " v" + std::to_string(e.version));
        }
        std::printf("%-14s %s: %s, %s-endian, %s\n", p.key.c_str(), p.name.c_str(), versions.c_str(),
                    p.endian == Endian::Big ? "big" : "little", p.encoding == Encoding::Utf8 ? "UTF-8" : "Shift-JIS");
      }
      return 0;
    }
    if (cmd == "matches" && args.size() == 2) {
      const Document d = load_any(args[0]);
      const Document ref = load_any(args[1]);
      std::vector<std::string> losses;
      const Buffer mine = write_database(convert(d, Target{ref.format, ref.version(), ref.endian, ref.encoding}, losses));
      const Buffer theirs = write_database(ref);
      std::size_t at = 0;
      while (at < mine.size() && at < theirs.size() && mine[at] == theirs[at])
        ++at;
      if (at == mine.size() && at == theirs.size()) {
        std::printf("identical: %s\n", summary(ref).c_str());
        return 0;
      }
      std::printf("first difference at 0x%zX (sizes 0x%zX and 0x%zX)\n", at, mine.size(), theirs.size());
      return 1;
    }
    if (cmd == "export-user" && args.size() == 3) {
      const Document d = load_any(args[0]);
      const Node* u = find_user(d, args[1]);
      if (!u)
        throw Error("no user " + args[1]);
      xml::export_user(d, *u, args[2]);
      return 0;
    }
    if (cmd == "import-user" && args.size() == 3) {
      Document d = load_any(args[0]);
      EditStack edits(d.root.get());
      xml::UserImport imp = xml::read_user(d, args[1]);
      for (const auto& w : imp.warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
      const int replace = xml::find_matching_user(d, imp);
      xml::apply_user_import(d, edits, std::move(imp), replace);
      save_any(args[2], d);
      return 0;
    }
    if (cmd == "bridge-check" && args.size() == 2) {
      // Users of SOURCE go into TARGET, are written and read back, and, for
      // an EffectLink or SoundLink source, return to it playing the same.
      const Document target = load_any(args[0]);
      const Document source = load_any(args[1]);
      const auto trip = [](const Document& into, const Document& from, bool& ok) {
        std::vector<std::string> warnings;
        Document d = into.clone();
        EditStack edits(d.root.get());
        auto imports = bridge_users(d, from, {}, default_bridge_options(d, from), warnings);
        const std::size_t n = imports.size();
        if (apply_bridged_users(d, edits, std::move(imports), true, warnings) != static_cast<int>(n)) {
          std::printf("  not every user was imported\n");
          ok = false;
        }
        Document back = read_database(write_database(d));
        std::printf("  %zu users into %s, %zu warnings\n", n, summary(back).c_str(), warnings.size());
        return back;
      };
      bool ok = true;
      const Document there = trip(target, source, ok);
      if (source.format != Format::XLink) {
        const Document back = trip(source, there, ok);
        const auto diffs = playback_differences(source, back);
        for (std::size_t k = 0; k < diffs.size() && k < 20; ++k)
          std::printf("  %s\n", to_utf8(diffs[k], source.encoding).c_str());
        std::printf("  back in %s: %zu differences in what users play\n", format_name(source.format).c_str(),
                    diffs.size());
        ok = ok && diffs.empty();
      }
      return ok ? 0 : 1;
    }
    if (cmd == "import-users" && args.size() >= 3) {
      Document d = load_any(args[0]);
      const Document src = load_any(args[1]);
      if (const std::string p = bridge_problem(d, src); !p.empty())
        throw Error(p);
      BridgeOptions opt = default_bridge_options(d, src);
      bool replace = false;
      std::vector<int> which;
      for (std::size_t k = 3; k < args.size(); ++k) {
        if (args[k] == "--replace") {
          replace = true;
        } else if (args[k] == "--slot" && k + 1 < args.size()) {
          opt.action_slot = from_utf8(args[++k], d.encoding);
        } else if (args[k] == "--conditions" && k + 1 < args.size()) {
          const std::string list = args[++k];
          std::size_t at = 0;
          for (int c = 0; c < 3; ++c) {
            const std::size_t comma = list.find(',', at);
            opt.conditions[static_cast<std::size_t>(c)] =
                from_utf8(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at), d.encoding);
            at = comma == std::string::npos ? list.size() : comma + 1;
          }
        } else {
          const Node* u = find_user(src, args[k]);
          if (!u)
            throw Error("no user " + args[k] + " in " + args[1]);
          which.push_back(u->row());
        }
      }
      if (bridge_crosses_formats(d, src)) {
        if (d.format == Format::XLink)
          std::fprintf(stderr, "action slot: %s\n", to_utf8(opt.action_slot, d.encoding).c_str());
        if (src.format == Format::EffectLink || d.format == Format::EffectLink)
          for (int c = 0; c < 3; ++c)
            std::fprintf(stderr, "Condition%d: %s\n", c + 1,
                         to_utf8(opt.conditions[static_cast<std::size_t>(c)], d.encoding).c_str());
      }
      std::vector<std::string> warnings;
      auto imports = bridge_users(d, src, which, opt, warnings);
      EditStack edits(d.root.get());
      const int n = apply_bridged_users(d, edits, std::move(imports), replace, warnings);
      for (const auto& w : warnings)
        std::fprintf(stderr, "warning: %s\n", w.c_str());
      std::fprintf(stderr, "%d users imported\n", n);
      save_any(args[2], d);
      return 0;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return usage();
}
