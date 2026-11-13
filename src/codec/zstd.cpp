#include "codec/zstd.hpp"

#include <memory>

#include <zstd.h>

namespace xl::zstd {

namespace {

struct DCtxDeleter {
  void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

struct CCtxDeleter {
  void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};

void check(std::size_t ret) {
  if (ZSTD_isError(ret))
    throw Error(std::string("Zstandard: ") + ZSTD_getErrorName(ret));
}

}  // namespace

bool probe(Bytes data) {
  return Reader(data, Endian::Little).magic_at(0, "\x28\xB5\x2F\xFD");
}

u32 frame_dictionary(Bytes data) {
  return ZSTD_getDictID_fromFrame(data.data(), data.size());
}

u32 dictionary_id(Bytes dictionary) {
  return ZSTD_getDictID_fromDict(dictionary.data(), dictionary.size());
}

// Streams rather than trusting the declared content size, which a frame may
// omit, and decodes every frame in sequence.
Buffer decompress(Bytes data, Bytes dictionary) {
  std::unique_ptr<ZSTD_DCtx, DCtxDeleter> ctx(ZSTD_createDCtx());
  if (!ctx)
    throw Error("Zstandard: out of memory");
  if (!dictionary.empty())
    check(ZSTD_DCtx_loadDictionary(ctx.get(), dictionary.data(), dictionary.size()));
  Buffer out;
  const unsigned long long declared = ZSTD_getFrameContentSize(data.data(), data.size());
  if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != ZSTD_CONTENTSIZE_ERROR && declared < (1ull << 31))
    out.reserve(static_cast<std::size_t>(declared));
  ZSTD_inBuffer in{data.data(), data.size(), 0};
  Buffer chunk(ZSTD_DStreamOutSize());
  for (;;) {
    ZSTD_outBuffer o{chunk.data(), chunk.size(), 0};
    const std::size_t ret = ZSTD_decompressStream(ctx.get(), &o, &in);
    check(ret);
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(o.pos));
    if (in.pos < in.size)
      continue;
    if (ret == 0)
      return out;
    // A full output buffer may only mean more is waiting to be flushed.
    if (o.pos < o.size)
      throw Error("Zstandard: stream is truncated");
  }
}

Buffer compress(Bytes data, int level, Bytes dictionary) {
  std::unique_ptr<ZSTD_CCtx, CCtxDeleter> ctx(ZSTD_createCCtx());
  if (!ctx)
    throw Error("Zstandard: out of memory");
  check(ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_compressionLevel, level));
  if (!dictionary.empty())
    check(ZSTD_CCtx_loadDictionary(ctx.get(), dictionary.data(), dictionary.size()));
  Buffer out(ZSTD_compressBound(data.size()));
  const std::size_t n = ZSTD_compress2(ctx.get(), out.data(), out.size(), data.data(), data.size());
  check(n);
  out.resize(n);
  return out;
}

}  // namespace xl::zstd
