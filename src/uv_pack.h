#pragma once
#include "core.h"
#include <atomic>
#include <vector>
struct PackPoint {
    double x = 0, y = 0;
};
struct PackIsland {
    std::vector<PackPoint> triangles; // three source-UV corners per triangle
    double surfaceArea = 0;           // world-space area; drives uniform texel density
};
struct PackSettings {
    int resolution = 2048; // output texture size; islands snap to its texel grid
    int spacing = 4;       // minimum gap between islands in output pixels
    bool allowRotation = false;
};
struct PackPlacement {
    double scale = 0; // source UV units -> output UV units
    bool rotated = false;
    PackPoint sourceMin, offset;
    double span = 0; // scaled source height, needed to keep rotated islands in the positive quadrant
    PackPoint apply(PackPoint p) const {
        double x = (p.x - sourceMin.x) * scale, y = (p.y - sourceMin.y) * scale;
        return rotated ? PackPoint{offset.x + span - y, offset.y + x} : PackPoint{offset.x + x, offset.y + y};
    }
};
struct PackResult {
    std::vector<PackPlacement> placements;
    double utilization = 0; // island area / 0-1 tile area
    int rotated = 0, densityClamped = 0;
};
PackResult packIslands(const std::vector<PackIsland> &, const PackSettings &, std::atomic_bool &cancel,
                       const Progress &progress = {});
