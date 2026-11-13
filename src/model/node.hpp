#pragma once

#include <bit>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/bytes.hpp"

// A schema-described tree: every database parses into Nodes whose fields and
// child lists are named by a Schema, so the editor, the YAML exporter and the
// reference bookkeeping work the same way for every format.
namespace xl {

class Node;
class NodeList;

enum class FieldType {
  Bool,
  U8,
  U16,
  U32,
  S16,
  S32,
  S64,
  F32,
  F64,
  Hex8,
  Hex16,
  Hex32,
  String,
  Enum,
  // Index of an element in another list; -1 for none.
  Ref,
  // Inclusive index range into another list; edits inside the range grow or
  // shrink it rather than leaving it pointing at the wrong elements.
  RangeStart,
  RangeEnd,
  // Interpreted by a schema hook, for values whose meaning depends on
  // sibling fields (a parameter's reference type, a definition's type).
  Dynamic,
};

// Numbers, floats included, live in `i` as raw bits so every value writes
// back exactly; strings in `s` as raw bytes in the file's encoding.
struct Value {
  s64 i = 0;
  std::string s;
  bool operator==(const Value&) const = default;
};

// How a Dynamic field reads at the moment: its kind decides which member of
// Value holds the data and how it is shown.
// Int is a 32-bit word shown signed and kept as its raw bits; Int64 is a
// full signed value.
enum class DynKind { Int, Int64, UInt, Hex, Float32, Float64, Bool, String, Ref };

struct FieldDef {
  std::string name;
  FieldType type = FieldType::U32;
  const std::vector<std::string>* enum_names = nullptr;
  // Ref, RangeStart, RangeEnd and Dynamic fields of kind Ref: the list the
  // index points into, resolved from the node holding the field.
  std::function<NodeList*(const Node&)> target;
  std::function<DynKind(const Node&)> dyn_kind;
  // Neither shown in the editor nor exported; carries layout hints that let
  // an unedited file write back byte for byte.
  bool hidden = false;
  bool read_only = false;
  std::string doc;
};

struct ListDef {
  std::string name;
  const struct Schema* element = nullptr;
  bool hidden = false;
  std::string doc;
};

struct Schema {
  std::string name;
  std::vector<FieldDef> fields;
  std::vector<ListDef> lists;
  std::function<std::string(const Node&)> label;

  int field_index(std::string_view n) const;
  int list_index(std::string_view n) const;
};

class Node {
public:
  explicit Node(const Schema* s);

  const Schema& schema() const { return *m_schema; }
  NodeList* parent_list() const { return m_parent; }
  Node* parent() const;
  int row() const { return m_row; }

  const Value& value(int k) const { return m_values[k]; }
  Value& value(int k) { return m_values[k]; }
  s64 i(int k) const { return m_values[k].i; }
  f32 f32v(int k) const { return bits_f32(static_cast<u32>(m_values[k].i)); }
  f64 f64v(int k) const { return std::bit_cast<f64>(static_cast<u64>(m_values[k].i)); }
  const std::string& s(int k) const { return m_values[k].s; }
  void set_i(int k, s64 v) { m_values[k].i = v; }
  void set_f32(int k, f32 v) { m_values[k].i = f32_bits(v); }
  void set_f64(int k, f64 v) { m_values[k].i = static_cast<s64>(std::bit_cast<u64>(v)); }
  void set_s(int k, std::string v) { m_values[k].s = std::move(v); }

  NodeList& list(int l) { return *m_lists[l]; }
  const NodeList& list(int l) const { return *m_lists[l]; }
  int list_count() const { return static_cast<int>(m_lists.size()); }

  std::unique_ptr<Node> clone() const;
  std::string label() const;
  // Nearest ancestor (or this node) built from `s`.
  const Node* ancestor(const Schema* s) const;
  Node* ancestor(const Schema* s);
  const Node* root() const;

private:
  friend class NodeList;
  const Schema* m_schema;
  std::vector<Value> m_values;
  std::vector<std::unique_ptr<NodeList>> m_lists;
  NodeList* m_parent = nullptr;
  int m_row = 0;
};

class NodeList {
public:
  NodeList(Node* owner, int index);

  Node* owner() const { return m_owner; }
  int index() const { return m_index; }
  const ListDef& def() const;
  int size() const { return static_cast<int>(m_items.size()); }
  bool empty() const { return m_items.empty(); }
  Node& at(int i) { return *m_items[i]; }
  const Node& at(int i) const { return *m_items[i]; }

  // Raw structure edits; reference bookkeeping lives in Document.
  Node& append(std::unique_ptr<Node> n);
  Node& append_new();
  Node& insert(int at, std::unique_ptr<Node> n);
  std::unique_ptr<Node> take(int at);
  void move(int from, int to);
  void clear();

  auto begin() { return m_items.begin(); }
  auto end() { return m_items.end(); }
  auto begin() const { return m_items.begin(); }
  auto end() const { return m_items.end(); }

private:
  void renumber(int from);
  Node* m_owner;
  int m_index;
  std::vector<std::unique_ptr<Node>> m_items;
};

// Visits every node below and including `n`.
void walk(Node& n, const std::function<void(Node&)>& fn);
void walk_const(const Node& n, const std::function<void(const Node&)>& fn);

// Helpers for schema construction.
FieldDef field(std::string name, FieldType t, std::string doc = {});
FieldDef enum_field(std::string name, const std::vector<std::string>* names, std::string doc = {});
FieldDef ref_field(std::string name, FieldType t, std::function<NodeList*(const Node&)> target, std::string doc = {});
FieldDef hidden_field(std::string name, FieldType t = FieldType::S64);

// Target resolver: list `list` of the nearest ancestor built from `owner`.
std::function<NodeList*(const Node&)> ancestor_list(const Schema* owner, int list);

}  // namespace xl
