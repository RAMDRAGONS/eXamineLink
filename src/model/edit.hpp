#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model/node.hpp"

namespace xl {

// One field value an edit changed as a side effect, kept so undo can put it back.
struct FieldChange {
   Node *node;
   int field;
   Value before;
};

// Keeps every index that points into `list` aimed at the same elements after
// an insertion, removal or move. References to a removed element become -1;
// a range loses the element, or grows when an element is inserted inside it.
enum class ListOp { Insert, Remove, Move };
void fix_references(Node &root, const NodeList &list, ListOp op, int a, int b, std::vector<FieldChange> &changes);
// Applies an insertion's renumbering to the references inside the node just
// inserted, which was built (copied, pasted) against the old numbering.
void fix_inserted(Node &inserted, const NodeList &list, int at, std::vector<FieldChange> &changes);

// How many references (fields and ranges) point at element `index` of `list`.
int count_references(const Node &root, const NodeList &list, int index);

// Every edit goes through here so it can be undone and so references stay
// consistent.
class EditStack {
public:
   struct Command;
   explicit EditStack(Node *root = nullptr);
   ~EditStack();
   EditStack(const EditStack &) = delete;
   EditStack &operator=(const EditStack &) = delete;

   void reset(Node *root);
   Node *root() const
   {
      return m_root;
   }

   void set_value(Node &n, int field, Value v);
   Node &insert(NodeList &list, int at, std::unique_ptr<Node> n);
   void remove(NodeList &list, int at);
   void move(NodeList &list, int from, int to);

   // Groups the edits made until end_group into one undo step.
   void begin_group(std::string text);
   void end_group();

   bool can_undo() const
   {
      return !m_done.empty();
   }
   bool can_redo() const
   {
      return !m_undone.empty();
   }
   std::string undo_text() const;
   std::string redo_text() const;
   void undo();
   void redo();

   // Called after every change, undo and redo included.
   std::function<void()> on_change;
   // Monotonic counter of applied changes, for refreshing views.
   long revision() const
   {
      return m_revision;
   }
   // Identifies the point in the undo history; equal states mean equal data,
   // so comparing with the state at save time tells whether there are
   // unsaved changes even after undoing back.
   unsigned long long state() const;

private:
   void push(std::unique_ptr<Command> c);
   void changed();

   Node *m_root = nullptr;
   std::vector<std::unique_ptr<Command>> m_done, m_undone;
   std::unique_ptr<Command> m_group;
   int m_group_depth = 0;
   long m_revision = 0;
   unsigned long long m_serial = 0;
};

} // namespace xl
