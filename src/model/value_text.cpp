#include "model/value_text.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace xl {

namespace {

// Floats print in the shortest form that reads back to the same bits; values
// that have no such form (NaN payloads) print their bits instead.
constexpr const char *BitsPrefix = "bits:";

bool
parse_int(const std::string &t, s64 &out)
{
   if (t.empty())
      return false;
   errno = 0;
   char *end = nullptr;
   if (t[0] == '-') {
      const long long v = std::strtoll(t.c_str(), &end, 0);
      if (errno || *end)
         return false;
      out = v;
   } else {
      const unsigned long long v = std::strtoull(t.c_str(), &end, 0);
      if (errno || *end)
         return false;
      out = static_cast<s64>(v);
   }
   return true;
}

struct Limits {
   s64 lo, hi;
};

std::optional<Limits>
limits_of(FieldType t)
{
   switch (t) {
   case FieldType::U8:
   case FieldType::Hex8:
      return Limits{0, 0xFF};
   case FieldType::U16:
   case FieldType::Hex16:
      return Limits{0, 0xFFFF};
   case FieldType::U32:
   case FieldType::Hex32:
      return Limits{0, 0xFFFFFFFFll};
   case FieldType::S16:
      return Limits{-0x8000, 0x7FFF};
   case FieldType::S32:
   case FieldType::Ref:
   case FieldType::RangeStart:
   case FieldType::RangeEnd:
      return Limits{std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max()};
   default:
      return std::nullopt;
   }
}

} // namespace

// Shortest decimal that reads back to the same bits, in plain notation for
// everyday magnitudes; NaN payloads and the like print their bits.
template <typename F, typename U>
std::string
shortest(F v, U bits, int width)
{
   if (!std::isfinite(v))
      return BitsPrefix + hex(bits, width);
   char buf[64];
   const F mag = std::fabs(v);
   const bool plain = mag == 0 || (mag >= F(1e-5) && mag < F(1e15));
   const auto r = plain ? std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed)
                        : std::to_chars(buf, buf + sizeof buf, v);
   return std::string(buf, r.ptr);
}

std::string
float_text(f32 v)
{
   return shortest(v, f32_bits(v), 8);
}

std::string
double_text(f64 v)
{
   return shortest(v, std::bit_cast<u64>(v), 16);
}

DynKind
field_kind(const Node &n, int field)
{
   const FieldDef &d = n.schema().fields[field];
   switch (d.type) {
   case FieldType::Bool:
      return DynKind::Bool;
   case FieldType::U8:
   case FieldType::U16:
   case FieldType::U32:
   case FieldType::Enum:
      return DynKind::UInt;
   case FieldType::S16:
   case FieldType::S32:
   case FieldType::S64:
      return DynKind::Int64;
   case FieldType::F32:
      return DynKind::Float32;
   case FieldType::F64:
      return DynKind::Float64;
   case FieldType::Hex8:
   case FieldType::Hex16:
   case FieldType::Hex32:
      return DynKind::Hex;
   case FieldType::String:
      return DynKind::String;
   case FieldType::Ref:
   case FieldType::RangeStart:
   case FieldType::RangeEnd:
      return DynKind::Ref;
   case FieldType::Dynamic:
      return d.dyn_kind ? d.dyn_kind(n) : DynKind::Hex;
   }
   return DynKind::Hex;
}

std::string
value_to_text(const Node &n, int field, Encoding enc)
{
   const FieldDef &d = n.schema().fields[field];
   const Value &v = n.value(field);
   if (d.type == FieldType::Enum) {
      if (d.enum_names && v.i >= 0 && v.i < static_cast<s64>(d.enum_names->size()))
         return (*d.enum_names)[v.i];
      return std::to_string(v.i);
   }
   switch (field_kind(n, field)) {
   case DynKind::Bool:
      return v.i ? "true" : "false";
   case DynKind::UInt:
      return std::to_string(static_cast<u64>(v.i));
   case DynKind::Int:
      return std::to_string(static_cast<s32>(static_cast<u32>(v.i)));
   case DynKind::Int64:
      return std::to_string(v.i);
   case DynKind::Hex: {
      int width = 0;
      if (d.type == FieldType::Hex8)
         width = 2;
      else if (d.type == FieldType::Hex16)
         width = 4;
      return hex(static_cast<u64>(v.i) & (d.type == FieldType::Hex8    ? 0xFF
                                          : d.type == FieldType::Hex16 ? 0xFFFF
                                                                       : 0xFFFFFFFFull),
                 width);
   }
   case DynKind::Float32:
      return float_text(bits_f32(static_cast<u32>(v.i)));
   case DynKind::Float64:
      return double_text(std::bit_cast<f64>(static_cast<u64>(v.i)));
   case DynKind::String:
      return to_utf8(v.s, enc);
   case DynKind::Ref:
      return std::to_string(v.i);
   }
   return {};
}

std::optional<std::string>
text_to_value(const Node &n, int field, const std::string &text, Encoding enc, Value &out)
{
   const FieldDef &d = n.schema().fields[field];
   out = n.value(field);
   if (d.type == FieldType::Enum) {
      if (d.enum_names)
         for (std::size_t k = 0; k < d.enum_names->size(); ++k)
            if ((*d.enum_names)[k] == text) {
               out.i = static_cast<s64>(k);
               return std::nullopt;
            }
      s64 x;
      if (!parse_int(text, x))
         return "\"" + text + "\" is not one of the allowed names";
      out.i = x;
      return std::nullopt;
   }
   const DynKind kind = field_kind(n, field);
   switch (kind) {
   case DynKind::Bool:
      if (text == "true" || text == "1") {
         out.i = 1;
      } else if (text == "false" || text == "0") {
         out.i = 0;
      } else {
         return "expected true or false";
      }
      return std::nullopt;
   case DynKind::Float32:
   case DynKind::Float64: {
      if (text.rfind(BitsPrefix, 0) == 0) {
         s64 bits;
         if (!parse_int(text.substr(5), bits))
            return "malformed bit pattern";
         out.i = bits;
         return std::nullopt;
      }
      errno = 0;
      char *end = nullptr;
      const double x = std::strtod(text.c_str(), &end);
      if (text.empty() || *end || errno == ERANGE)
         return "\"" + text + "\" is not a number";
      if (kind == DynKind::Float32)
         out.i = f32_bits(static_cast<f32>(x));
      else
         out.i = static_cast<s64>(std::bit_cast<u64>(x));
      return std::nullopt;
   }
   case DynKind::String:
      out.s = from_utf8(text, enc);
      return std::nullopt;
   case DynKind::Int: {
      s64 x;
      if (!parse_int(text, x))
         return "\"" + text + "\" is not an integer";
      if (x < std::numeric_limits<s32>::min() || x > 0xFFFFFFFFll)
         return std::to_string(x) + " does not fit 32 bits";
      out.i = static_cast<u32>(x);
      return std::nullopt;
   }
   default: {
      s64 x;
      if (!parse_int(text, x))
         return "\"" + text + "\" is not an integer";
      if (const auto lim = limits_of(d.type)) {
         // Signed fields also accept their unsigned spelling and vice versa.
         if (lim->lo < 0 && x > lim->hi && x <= 2 * lim->hi + 1)
            x -= 2 * lim->hi + 2;
         if (lim->lo == 0 && x < 0 && x >= -(lim->hi / 2) - 1)
            x += lim->hi + 1;
         if (x < lim->lo || x > lim->hi)
            return std::to_string(x) + " is out of range (" + std::to_string(lim->lo) + " to " +
                   std::to_string(lim->hi) + ")";
      }
      out.i = x;
      return std::nullopt;
   }
   }
}

} // namespace xl
