#include "codec/layers.hpp"

#include "codec/yaz.hpp"
#include "codec/zstd.hpp"

namespace xl {

Unwrapped
unwrap(Bytes data, const std::function<bool(Bytes)> &accept, const DictionaryLookup &dictionaries)
{
   Unwrapped u;
   Buffer current(data.begin(), data.end());
   for (;;) {
      if (yaz::probe(current)) {
         Layer l{LayerKind::Yaz};
         l.yaz_alignment = yaz::read_header(current).data_alignment;
         current = yaz::decompress(current);
         u.layers.push_back(std::move(l));
         continue;
      }
      if (zstd::probe(current)) {
         Layer l{LayerKind::Zstd};
         if (const u32 id = zstd::frame_dictionary(current)) {
            // Of dictionaries sharing the ID, the right one decodes into
            // something recognizable; a wrong one usually fails outright.
            Buffer decoded;
            for (const auto &candidate :
                 dictionaries ? dictionaries(id) : std::vector<std::shared_ptr<const Buffer>>{}) {
               Buffer out;
               try {
                  out = zstd::decompress(current, *candidate);
               } catch (const Error &) {
                  continue;
               }
               const bool known = accept(out) || yaz::probe(out) || zstd::probe(out) || sarc::probe(out);
               if (!l.zstd_dictionary || known) {
                  l.zstd_dictionary = candidate;
                  decoded = std::move(out);
               }
               if (known)
                  break;
            }
            if (!l.zstd_dictionary)
               throw Error("compressed with Zstandard dictionary " + std::to_string(id) +
                           ", which no available dictionary decodes");
            current = std::move(decoded);
         } else {
            current = zstd::decompress(current);
         }
         u.layers.push_back(std::move(l));
         continue;
      }
      if (sarc::probe(current)) {
         Layer l{LayerKind::Sarc};
         l.archive = sarc::read(current);
         for (std::size_t i = 0; i < l.archive.files.size(); ++i) {
            const Buffer &f = l.archive.files[i].data;
            // Members may themselves be compressed.
            bool ok = accept(f);
            if (!ok && (yaz::probe(f) || zstd::probe(f))) {
               try {
                  ok = accept(unwrap(f, accept, dictionaries).payload);
               } catch (const Error &) {
                  ok = false;
               }
            }
            if (ok) {
               l.sarc_index = static_cast<int>(i);
               break;
            }
         }
         if (l.sarc_index < 0)
            throw Error("the archive holds no link database");
         current = std::move(l.archive.files[l.sarc_index].data);
         l.archive.files[l.sarc_index].data.clear();
         u.layers.push_back(std::move(l));
         continue;
      }
      break;
   }
   u.payload = std::move(current);
   return u;
}

Buffer
wrap(const std::vector<Layer> &layers, Buffer payload)
{
   for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
      switch (it->kind) {
      case LayerKind::Yaz:
         payload = yaz::compress(payload, it->yaz_alignment);
         break;
      case LayerKind::Zstd:
         payload = it->zstd_dictionary ? zstd::compress(payload, it->zstd_level, *it->zstd_dictionary)
                                       : zstd::compress(payload, it->zstd_level);
         break;
      case LayerKind::Sarc: {
         sarc::Archive a = it->archive;
         a.files[it->sarc_index].data = std::move(payload);
         payload = sarc::write(a);
         break;
      }
      }
   }
   return payload;
}

std::string
describe(const std::vector<Layer> &layers)
{
   std::string s;
   for (const Layer &l : layers) {
      if (!s.empty())
         s += " > ";
      switch (l.kind) {
      case LayerKind::Yaz:
         s += "Yaz";
         break;
      case LayerKind::Zstd:
         s += "Zstandard";
         if (l.zstd_dictionary)
            s += "[dictionary " + std::to_string(zstd::dictionary_id(*l.zstd_dictionary)) + "]";
         break;
      case LayerKind::Sarc:
         s += "SARC[" + l.archive.files[l.sarc_index].name + "]";
         break;
      }
   }
   return s.empty() ? "uncompressed" : s;
}

} // namespace xl
