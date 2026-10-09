#pragma once
#include "core.h"
#include "uv_pack.h"
struct Uv2Report {
    int meshes = 0, islands = 0, rotated = 0, replaced = 0;
    double utilization = 0, seconds = 0;
    QStringList warnings;
};
// Packs the UV1 islands of every mesh in a binary FBX into one non-overlapping UV2 set with
// uniform texel density, and writes a copy of the file with that set added (or replaced).
// An empty name keeps the name of a replaced set, or uses "UV2".
Uv2Report generateUv2(const QString &input, const QString &output, const PackSettings &, const QString &name,
                      std::atomic_bool &cancel, const Progress &progress = {});
// Returns why a UV set name is unsafe for DCC import, or an empty string when it is valid.
QString uvSetNameError(const QString &name);
