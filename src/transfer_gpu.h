#pragma once
#include "transfer_shared.h"
#include <QString>
#include <memory>
#include <vector>
// OptiX backend for runTransfer. Source geometry and its BVH live for the whole transfer.
class TransferGpu {
  public:
    TransferGpu(const std::vector<XferSrcTri> &src, const std::vector<unsigned char> &allowed,
                const std::vector<XferTri> &targets);
    ~TransferGpu();
    void setGrid(const std::vector<int> &offsets, const std::vector<int> &indices);
    // Uploads texel and owner data referenced by host pointers; owners match each set's first level.
    void setTextures(const std::vector<TexLevel> &levels, const std::vector<TexSet> &sets);
    // Launches params.texelCount texels starting at params.texelStart and copies results to out.
    void launch(const XferParams &params, XAccum *out);
    QString device() const;
    size_t peakBytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
