#include "uv_pack.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
// Shape-aware skyline packer. Every island is reduced to a per-column [bottom, top) cell
// profile on the output texel grid, dilated by half the spacing, then placed on a skyline by
// several greedy recipes (best-fit gap filling and ordered lowest-position passes). A parallel
// k-ary search finds the largest uniform scale at which any recipe fits.
namespace {
constexpr int kMaxGrid = 4096; // finer grids only add cost; 8K layouts snap to 2-texel cells
constexpr int kCandidates = 8; // scales tested per search round; fixed so output never depends on CPU count
constexpr int kMaxRounds = 24;
constexpr double kDensityClamp = 32;

struct Shape {
    PackPoint min, max;
    double density = 0, area = 0;
};
struct Profile {
    int cols = 0, height = 0, maxBottom = 0;
    bool dense = true; // every column is covered
    std::vector<int> bottom, top;
    std::vector<double> lo, hi;
};
struct Spot {
    int x = 0, y = 0;
    bool rotated = false;
};
enum Orientation : char { Keep, Turn, Either };
// One greedy packing recipe. Several are tried per scale; the first that fits wins, so the
// result is deterministic and never worse than the best single recipe.
struct Strategy {
    std::vector<int> order;
    std::vector<Orientation> orientation;
    bool wasteFirst = false; // rank spots by wasted area before height
    bool bestFit = false;    // pick islands per skyline gap instead of following `order`
};
// Vertical extent of a triangle restricted to the slab xa <= x <= xb.
bool slabRange(const PackPoint *t, double xa, double xb, double &ylo, double &yhi) {
    bool any = false;
    auto add = [&](double y) {
        ylo = any ? std::min(ylo, y) : y;
        yhi = any ? std::max(yhi, y) : y;
        any = true;
    };
    for (int i = 0; i < 3; ++i) {
        const PackPoint &p = t[i], &q = t[(i + 1) % 3];
        if (p.x >= xa && p.x <= xb)
            add(p.y);
        for (double x : {xa, xb})
            if ((p.x - x) * (q.x - x) < 0)
                add(p.y + (x - p.x) * (q.y - p.y) / (q.x - p.x));
    }
    return any;
}
class Packer {
  public:
    Packer(const std::vector<PackIsland> &islands, const std::vector<Shape> &shapes, int grid, double halfPad)
        : islands_(islands), shapes_(shapes), grid_(grid), halfPad_(halfPad) {}
    void rasterize(int index, double k, bool rotated, Profile &p) const {
        const Shape &s = shapes_[index];
        const double scale = k * s.density, pad = 2 * halfPad_;
        const double h = (s.max.y - s.min.y) * scale;
        const double width = rotated ? h : (s.max.x - s.min.x) * scale;
        p.cols = std::max(1, (int)std::ceil(width + pad));
        if (p.cols > grid_)
            return;
        p.lo.assign(p.cols, std::numeric_limits<double>::infinity());
        p.hi.assign(p.cols, -std::numeric_limits<double>::infinity());
        const auto &tris = islands_[index].triangles;
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            PackPoint t[3];
            for (int j = 0; j < 3; ++j) {
                double x = (tris[i + j].x - s.min.x) * scale, y = (tris[i + j].y - s.min.y) * scale;
                t[j] = rotated ? PackPoint{h - y, x} : PackPoint{x, y};
            }
            double x0 = std::min({t[0].x, t[1].x, t[2].x}), x1 = std::max({t[0].x, t[1].x, t[2].x});
            // Column c covers the source slab [c - pad, c + 1]: half spacing on each side.
            int c0 = std::max(0, (int)std::ceil(x0 - 1)), c1 = std::min(p.cols - 1, (int)std::floor(x1 + pad));
            for (int c = c0; c <= c1; ++c) {
                double ylo, yhi;
                if (slabRange(t, c - pad, c + 1, ylo, yhi)) {
                    p.lo[c] = std::min(p.lo[c], ylo);
                    p.hi[c] = std::max(p.hi[c], yhi);
                }
            }
        }
        p.bottom.resize(p.cols);
        p.top.resize(p.cols);
        p.height = 0;
        p.maxBottom = 0;
        p.dense = true;
        for (int c = 0; c < p.cols; ++c) {
            if (p.lo[c] > p.hi[c]) {
                p.bottom[c] = p.top[c] = 0;
                p.dense = false;
                continue;
            }
            p.bottom[c] = std::max(0, (int)std::floor(p.lo[c]));
            p.top[c] = std::max(p.bottom[c] + 1, (int)std::ceil(p.hi[c] + pad));
            p.height = std::max(p.height, p.top[c]);
            p.maxBottom = std::max(p.maxBottom, p.bottom[c]);
        }
    }
    // Best-fit skyline: always fill the lowest skyline gap with the widest island that fits it
    // (tallest on ties), standing it against the taller neighbour. Gaps nothing fits are raised to
    // their lower neighbour. This closes the slivers an order-driven pass leaves between strips.
    bool runBestFit(double k, const Strategy &strategy, std::vector<Spot> &spots, const std::atomic_bool &cancel) const {
        const int n = (int)islands_.size();
        std::vector<Profile> profiles[2];
        using Key = std::tuple<int, int, int>; // (-cols, -height, index)
        std::set<Key> free[2];
        for (int r = 0; r < 2; ++r) {
            profiles[r].resize(n);
            for (int i = 0; i < n; ++i) {
                const Orientation o = strategy.orientation[i];
                if ((r == 1 && o == Keep) || (r == 0 && o == Turn))
                    continue;
                Profile &p = profiles[r][i];
                rasterize(i, k, r == 1, p);
                p.lo.clear();
                p.hi.clear();
                if (p.cols <= grid_ && p.height <= grid_)
                    free[r].insert({-p.cols, -p.height, i});
                else
                    p.cols = 0; // keeps erase() below a no-op for this orientation
            }
        }
        if (free[0].size() + free[1].size() < (size_t)n)
            return false; // cheap reject; exact coverage is enforced by the loop running dry
        spots.assign(n, {});
        std::vector<int> sky(grid_, 0);
        for (int remaining = n; remaining > 0;) {
            if ((remaining & 63) == 0 && cancel)
                return false;
            int x0 = 0;
            for (int x = 1; x < grid_; ++x)
                if (sky[x] < sky[x0])
                    x0 = x;
            int x1 = x0;
            while (x1 < grid_ && sky[x1] == sky[x0])
                ++x1;
            const int gap = x1 - x0;
            int pick = -1, rot = 0;
            for (int r = 0; r < 2; ++r) {
                auto it = free[r].lower_bound({-gap, INT_MIN, INT_MIN});
                if (it == free[r].end())
                    continue;
                if (pick < 0 || *it < Key{-profiles[rot][pick].cols, -profiles[rot][pick].height, pick}) {
                    pick = std::get<2>(*it);
                    rot = r;
                }
            }
            if (pick < 0) {
                const int left = x0 > 0 ? sky[x0 - 1] : INT_MAX, right = x1 < grid_ ? sky[x1] : INT_MAX;
                const int level = std::min(left, right);
                if (level == INT_MAX)
                    return false; // nothing fits even the full width
                std::fill(sky.begin() + x0, sky.begin() + x1, level);
                continue;
            }
            const Profile &p = profiles[rot][pick];
            const int left = x0 > 0 ? sky[x0 - 1] : INT_MAX, right = x1 < grid_ ? sky[x1] : INT_MAX;
            const int x = left >= right ? x0 : x1 - p.cols;
            int y = 0;
            for (int c = 0; c < p.cols; ++c)
                if (p.top[c] > p.bottom[c])
                    y = std::max(y, sky[x + c] - p.bottom[c]);
            if (y + p.height > grid_)
                return false;
            for (int c = 0; c < p.cols; ++c)
                if (p.top[c] > p.bottom[c])
                    sky[x + c] = y + p.top[c];
            spots[pick] = {x, y, rot == 1};
            for (int r = 0; r < 2; ++r)
                free[r].erase({-profiles[r][pick].cols, -profiles[r][pick].height, pick});
            --remaining;
        }
        return true;
    }
    bool run(double k, const Strategy &strategy, std::vector<Spot> &spots, const std::atomic_bool &cancel) const {
        if (strategy.bestFit)
            return runBestFit(k, strategy, spots, cancel);
        std::vector<int> sky(grid_, 0);
        Profile profiles[2];
        spots.assign(islands_.size(), {});
        int placed = 0;
        for (int index : strategy.order) {
            if ((++placed & 63) == 0 && cancel)
                return false;
            int bestScore = INT_MAX;
            long long bestWaste = LLONG_MAX;
            Spot best;
            bool found = false;
            const Orientation orientation = strategy.orientation[index];
            for (int r = 0; r < 2; ++r) {
                if ((r == 1 && orientation == Keep) || (r == 0 && orientation == Turn))
                    continue;
                Profile &p = profiles[r];
                rasterize(index, k, r == 1, p);
                if (p.cols > grid_ || p.height > grid_)
                    continue;
                for (int x = 0; x + p.cols <= grid_;) {
                    // Height-first ranking can prune any spot that cannot beat the best top.
                    const int limit = strategy.wasteFirst || !found ? grid_ - p.height
                                                                     : std::min(bestScore - p.height, grid_ - p.height);
                    int y = 0, next = x + 1;
                    bool fits = true;
                    for (int c = 0; c < p.cols; ++c) {
                        if (p.top[c] <= p.bottom[c])
                            continue;
                        int v = sky[x + c] - p.bottom[c];
                        if (v > y) {
                            y = v;
                            if (y > limit) {
                                fits = false;
                                // This skyline column blocks every column of the island: jump past it.
                                if (p.dense && sky[x + c] - p.maxBottom > limit)
                                    next = x + c + 1;
                                break;
                            }
                        }
                    }
                    if (fits) {
                        long long waste = 0;
                        for (int c = 0; c < p.cols; ++c)
                            if (p.top[c] > p.bottom[c])
                                waste += y + p.bottom[c] - sky[x + c];
                        int score = y + p.height;
                        bool better = strategy.wasteFirst
                                          ? waste < bestWaste || (waste == bestWaste && score < bestScore)
                                          : score < bestScore || (score == bestScore && waste < bestWaste);
                        if (better) {
                            bestScore = score;
                            bestWaste = waste;
                            best = {x, y, r == 1};
                            found = true;
                        }
                    }
                    x = next;
                }
            }
            if (!found)
                return false;
            const Profile &p = profiles[best.rotated ? 1 : 0];
            for (int c = 0; c < p.cols; ++c)
                if (p.top[c] > p.bottom[c])
                    sky[best.x + c] = best.y + p.top[c];
            spots[index] = best;
        }
        return true;
    }

  private:
    const std::vector<PackIsland> &islands_;
    const std::vector<Shape> &shapes_;
    int grid_;
    double halfPad_;
};
[[noreturn]] void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}
} // namespace

PackResult packIslands(const std::vector<PackIsland> &islands, const PackSettings &settings, std::atomic_bool &cancel,
                       const Progress &progress) {
    if (islands.empty())
        fail("There are no UV islands to pack.");
    if (settings.resolution < 64 || settings.resolution > 16384)
        fail("UV layout resolution must be between 64 and 16384 pixels.");
    if (settings.spacing < 0 || settings.spacing * 4 >= settings.resolution)
        fail("UV island spacing is out of range for the layout resolution.");
    std::vector<Shape> shapes(islands.size());
    std::vector<double> known;
    for (size_t i = 0; i < islands.size(); ++i) {
        const auto &tris = islands[i].triangles;
        if (tris.empty() || tris.size() % 3)
            fail("Invalid UV island triangle list.");
        Shape &s = shapes[i];
        s.min = {INFINITY, INFINITY};
        s.max = {-INFINITY, -INFINITY};
        for (size_t j = 0; j < tris.size(); ++j) {
            const auto &p = tris[j];
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                fail("UV1 contains non-finite coordinates.");
            s.min = {std::min(s.min.x, p.x), std::min(s.min.y, p.y)};
            s.max = {std::max(s.max.x, p.x), std::max(s.max.y, p.y)};
            if (j % 3 == 2) {
                const auto &a = tris[j - 2], &b = tris[j - 1];
                s.area += std::fabs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * .5;
            }
        }
        double extent = std::max(s.max.x - s.min.x, s.max.y - s.min.y);
        double surface = islands[i].surfaceArea;
        // Collapsed UV1 islands have no meaningful density; they get the median instead.
        if (std::isfinite(surface) && surface > 0 && s.area > 1e-9 * extent * extent && s.area > 1e-30) {
            s.density = std::sqrt(surface / s.area);
            known.push_back(s.density);
        }
    }
    double median = 1;
    if (!known.empty()) {
        std::nth_element(known.begin(), known.begin() + known.size() / 2, known.end());
        median = known[known.size() / 2];
    }
    PackResult result;
    for (auto &s : shapes) {
        if (s.density <= 0)
            s.density = median;
        else if (s.density > median * kDensityClamp || s.density < median / kDensityClamp) {
            s.density = std::clamp(s.density, median / kDensityClamp, median * kDensityClamp);
            ++result.densityClamped;
        }
    }
    const int grid = std::min(settings.resolution, kMaxGrid);
    const double pad = settings.spacing * (double)grid / settings.resolution, halfPad = pad / 2;
    const size_t n = islands.size();
    std::vector<double> width(n), height(n), area(n);
    double totalArea = 0, hi = INFINITY;
    for (size_t i = 0; i < n; ++i) {
        const Shape &s = shapes[i];
        width[i] = (s.max.x - s.min.x) * s.density;
        height[i] = (s.max.y - s.min.y) * s.density;
        area[i] = s.area * s.density * s.density;
        totalArea += area[i];
        if (std::max(width[i], height[i]) > 0)
            hi = std::min(hi, (grid - pad - 1) / std::max(width[i], height[i]));
    }
    // Recipes, most layout-friendly first: upright islands (all identical orientation, like a
    // hand-made card sheet), then free rotation. Each is sorted tallest / widest / largest first.
    std::vector<Strategy> strategies;
    auto add = [&](Orientation policy, int key, bool wasteFirst, bool bestFit = false) {
        Strategy st;
        st.wasteFirst = wasteFirst;
        st.bestFit = bestFit;
        st.orientation.resize(n);
        std::vector<double> h(n), w(n);
        bool turnsAny = false;
        for (size_t i = 0; i < n; ++i) {
            Orientation o = policy;
            if (policy == Turn) // "upright": stand every island on its short side
                o = width[i] > height[i] ? Turn : Keep;
            st.orientation[i] = o;
            bool turned = o == Turn;
            turnsAny |= turned;
            h[i] = o == Either ? std::max(width[i], height[i]) : turned ? width[i] : height[i];
            w[i] = o == Either ? std::min(width[i], height[i]) : turned ? height[i] : width[i];
        }
        if (policy == Turn && !turnsAny)
            return; // already upright: identical to the unrotated recipe
        const std::vector<double> &first = key == 0 ? h : key == 1 ? w : area, &second = key == 0 ? w : h;
        st.order.resize(n);
        std::iota(st.order.begin(), st.order.end(), 0);
        std::sort(st.order.begin(), st.order.end(), [&](int a, int b) {
            if (first[a] != first[b])
                return first[a] > first[b];
            if (second[a] != second[b])
                return second[a] > second[b];
            return a < b;
        });
        strategies.push_back(std::move(st));
    };
    for (Orientation policy : settings.allowRotation ? std::vector<Orientation>{Keep, Turn, Either}
                                                     : std::vector<Orientation>{Keep}) {
        add(policy, 0, false, true);
        for (int key = 0; key < 3; ++key)
            for (bool wasteFirst : {false, true})
                add(policy, key, wasteFirst);
    }
    if (totalArea > 0)
        hi = std::min(hi, grid / std::sqrt(totalArea));
    if (!std::isfinite(hi))
        hi = grid;
    if (hi <= 0)
        fail("UV island spacing leaves no room for islands.");
    Packer packer(islands, shapes, grid, halfPad);
    const int strategyCount = (int)strategies.size(), tasks = kCandidates * strategyCount;
    const int threads = std::max(1, std::min(tasks, (int)std::thread::hardware_concurrency()));
    double lo = 0;
    std::vector<Spot> best;
    for (int round = 0; round < kMaxRounds; ++round) {
        double k[kCandidates];
        std::vector<Spot> spots[kCandidates];
        std::atomic_int firstStrategy[kCandidates], largestFit{-1};
        std::exception_ptr error[kCandidates];
        std::mutex lock;
        for (int j = 0; j < kCandidates; ++j) {
            k[j] = lo + (hi - lo) * (j + 1) / (kCandidates + 1);
            firstStrategy[j] = INT_MAX;
        }
        // Tasks are (strategy, scale) pairs. A task is skipped once its outcome can no longer change
        // the result (an earlier recipe or a larger scale already fits), which keeps the outcome
        // independent of thread timing.
        std::atomic_int next{0};
        auto work = [&] {
            std::vector<Spot> local;
            for (int t; (t = next++) < tasks;) {
                const int s = t / kCandidates, j = kCandidates - 1 - t % kCandidates;
                if (s > firstStrategy[j] || j < largestFit)
                    continue;
                try {
                    if (!packer.run(k[j], strategies[s], local, cancel))
                        continue;
                } catch (...) {
                    std::lock_guard<std::mutex> guard(lock);
                    error[j] = std::current_exception();
                    continue;
                }
                std::lock_guard<std::mutex> guard(lock);
                if (s < firstStrategy[j]) {
                    firstStrategy[j] = s;
                    spots[j].swap(local);
                }
                if (j > largestFit)
                    largestFit = j;
            }
        };
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t)
            pool.emplace_back(work);
        work();
        for (auto &t : pool)
            t.join();
        for (auto &e : error)
            if (e)
                std::rethrow_exception(e);
        if (cancel)
            fail("UV2 generation cancelled.");
        const int winner = largestFit;
        if (winner >= 0) {
            lo = k[winner];
            best = std::move(spots[winner]);
            if (winner + 1 < kCandidates)
                hi = k[winner + 1];
        } else
            hi = k[0];
        if (progress)
            progress(std::min(.95f, (round + 1) / 6.f), "Packing UV2 islands…");
        if (!best.empty() && hi - lo <= lo * 1e-3)
            break;
        if (best.empty() && round >= 12)
            break;
    }
    if (best.empty())
        fail(QString("%1 UV islands do not fit a %2 px layout with %3 px spacing. Increase the resolution or "
                     "reduce the spacing.")
                 .arg(islands.size())
                 .arg(settings.resolution)
                 .arg(settings.spacing));
    result.placements.resize(islands.size());
    for (size_t i = 0; i < islands.size(); ++i) {
        const Shape &s = shapes[i];
        PackPlacement &p = result.placements[i];
        p.scale = lo * s.density / grid;
        p.rotated = best[i].rotated;
        p.sourceMin = s.min;
        p.span = (s.max.y - s.min.y) * p.scale;
        p.offset = {(best[i].x + halfPad) / grid, (best[i].y + halfPad) / grid};
        result.utilization += s.area * p.scale * p.scale;
        result.rotated += p.rotated;
    }
    return result;
}
