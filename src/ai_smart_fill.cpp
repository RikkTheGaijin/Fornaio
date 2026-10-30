#include "ai_smart_fill.h"
#include "ai_cloud.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <numeric>
#include <stdexcept>

namespace {
constexpr int requestSize = 1024, coreSize = 896, border = 64;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void check(std::atomic_bool *cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
}
QString png(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    require(!image.isNull() && image.save(&buffer, "PNG"), "Cannot encode Smart fill image");
    return QString::fromLatin1(bytes.toBase64());
}
QImage image(const QJsonValue &value, QImage::Format format) {
    if (value.isUndefined() || value.isNull())
        return {};
    QByteArray bytes = QByteArray::fromBase64(value.toString().toLatin1());
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const auto size = reader.size();
    require(!size.isEmpty() && size.width() <= 4096 && size.height() <= 4096,
            "Invalid Smart fill image dimensions");
    QImage result = reader.read();
    require(!result.isNull(), "Invalid Smart fill image");
    return result.convertToFormat(format);
}
QJsonArray rectJson(const QRect &r) {
    return {r.x(), r.y(), r.width(), r.height()};
}
QRect rect(const QJsonValue &value) {
    const auto a = value.toArray();
    require(a.size() == 4, "Invalid Smart fill crop mapping");
    return {a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt()};
}
QString chartData(const QVector<int> &charts) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    for (int chart : charts)
        stream << qint32(chart);
    return QString::fromLatin1(qCompress(bytes).toBase64());
}
QVector<int> charts(const QJsonObject &domain, const QSize &size) {
    const QByteArray packed = QByteArray::fromBase64(domain["charts"].toString().toLatin1());
    const qint64 expected = qint64(size.width()) * size.height() * 4;
    require(expected > 0 && expected <= 4096ll * 4096 * 4 && packed.size() >= 4,
            "Invalid Smart fill UV chart data");
    // Check the zlib header length before allocating or decompressing untrusted project data.
    quint32 length = (quint32(uchar(packed[0])) << 24) | (quint32(uchar(packed[1])) << 16) |
                     (quint32(uchar(packed[2])) << 8) | quint32(uchar(packed[3]));
    require(length == expected, "Invalid Smart fill UV chart dimensions");
    QByteArray bytes = qUncompress(packed);
    require(bytes.size() == expected, "Invalid Smart fill UV chart data");
    QDataStream stream(bytes);
    stream.setByteOrder(QDataStream::LittleEndian);
    QVector<int> result(size.width() * size.height());
    for (int &chart : result) {
        qint32 value;
        stream >> value;
        chart = value;
    }
    return result;
}
QJsonArray patches(const QJsonObject &plan) {
    QJsonArray result;
    const auto domains = plan["domains"].toObject();
    const auto masks = plan["masks"].toObject();
    for (auto it = masks.begin(); it != masks.end(); ++it) {
        const QImage mask = image(it.value(), QImage::Format_Grayscale8);
        const QVector<int> owners = charts(domains[it.key()].toObject(), mask.size());
        for (int y = 0; y < mask.height(); y += coreSize) {
            for (int x = 0; x < mask.width(); x += coreSize) {
                const QRect core(x, y, std::min(coreSize, mask.width() - x),
                                 std::min(coreSize, mask.height() - y));
                QSet<int> touched;
                for (int cy = core.top(); cy <= core.bottom(); ++cy)
                    for (int cx = core.left(); cx <= core.right(); ++cx)
                        if (mask.constScanLine(cy)[cx] && owners[cy * mask.width() + cx] >= 0)
                            touched.insert(owners[cy * mask.width() + cx]);
                auto ordered = touched.values();
                std::sort(ordered.begin(), ordered.end());
                for (int chart : ordered)
                    result.append(QJsonObject{
                        {"material", it.key()},
                        {"chart", chart},
                        {"core", rectJson(core)},
                        {"crop", rectJson(QRect(x - border, y - border, requestSize, requestSize))},
                        {"done", false}});
            }
        }
    }
    return result;
}
void planBoard(QJsonObject &plan, std::atomic_bool *cancel = nullptr) {
    const auto masks = plan["masks"].toObject();
    QStringList selected;
    int largest = 0;
    for (auto it = masks.begin(); it != masks.end(); ++it) {
        check(cancel);
        const QImage mask = image(it.value(), QImage::Format_Grayscale8);
        bool any = false;
        for (int y = 0; y < mask.height() && !any; ++y) {
            check(cancel);
            for (int x = 0; x < mask.width(); ++x)
                if (mask.constScanLine(y)[x]) {
                    any = true;
                    break;
                }
        }
        if (any) {
            selected.append(it.key());
            largest = std::max(largest, std::max(mask.width(), mask.height()));
        }
    }
    QJsonObject layout;
    QJsonArray requests;
    int boardSize = 0;
    if (!selected.isEmpty()) {
        int columns = 1;
        while (qint64(columns) * columns < selected.size())
            ++columns;
        boardSize = int(std::min<qint64>(4096, std::max<qint64>(1024, qint64(largest) * columns)));
        require(columns <= boardSize, "Too many materials for a single Smart fill atlas board");
        QJsonArray materialNames;
        for (int i = 0; i < selected.size(); ++i) {
            const int column = i % columns, row = i / columns;
            const int x = column * boardSize / columns, y = row * boardSize / columns;
            const int right = (column + 1) * boardSize / columns;
            const int bottom = (row + 1) * boardSize / columns;
            layout[selected[i]] = rectJson(QRect(x, y, right - x, bottom - y));
            materialNames.append(selected[i]);
        }
        requests.append(QJsonObject{{"kind", "uv_board"}, {"materials", materialNames}, {"done", false}});
    }
    plan["layout"] = layout;
    plan["boardSize"] = boardSize;
    plan["patches"] = requests;
}
QJsonObject savedMaterial(const QJsonObject &plan, const QString &material) {
    for (const auto &value : plan["source"].toArray())
        if (value.toObject()["material"].toString() == material)
            return value.toObject();
    throw std::runtime_error("Smart fill source material is missing");
}
using Color = std::array<float, 3>;
struct HarmonicGraph {
    QVector<std::array<int, 4>> neighbors;
    QVector<float> diagonal;
    QVector<std::array<int, 3>> positions; // x, y, independent connected component
};
struct HarmonicLevel {
    QVector<int> parent;
    QVector<float> diagonal;
    QVector<Color> workspace;
};
QVector<HarmonicLevel> harmonicLevels(const HarmonicGraph &graph, std::atomic_bool *cancel) {
    QVector<HarmonicLevel> levels(1);
    levels[0].diagonal = graph.diagonal;
    auto positions = graph.positions;
    QVector<int> root(positions.size());
    std::iota(root.begin(), root.end(), 0);
    QSet<int> components;
    for (const auto &position : positions)
        components.insert(position[2]);
    // Piecewise-constant aggregation supplies a symmetric positive multilevel
    // preconditioner. Component identities prevent even coarse-level chart coupling.
    for (int depth = 0; depth < 13 && positions.size() > components.size(); ++depth) {
        check(cancel);
        QHash<quint64, int> groups;
        QVector<std::array<int, 3>> coarse;
        QVector<int> parent(positions.size());
        for (int i = 0; i < positions.size(); ++i) {
            if ((i & 16383) == 0)
                check(cancel);
            const auto p = positions[i];
            const quint64 key = (quint64(p[2]) << 26) | (quint64(p[1] / 2) << 13) | quint64(p[0] / 2);
            auto found = groups.constFind(key);
            if (found == groups.cend()) {
                const int index = coarse.size();
                groups.insert(key, index);
                parent[i] = index;
                coarse.append({p[0] / 2, p[1] / 2, p[2]});
            } else
                parent[i] = found.value();
        }
        for (int &index : root)
            index = parent[index];
        QVector<double> diagonal(coarse.size(), 0);
        for (int i = 0; i < graph.neighbors.size(); ++i) {
            if ((i & 16383) == 0)
                check(cancel);
            const int group = root[i];
            diagonal[group] += graph.diagonal[i];
            for (int neighbor : graph.neighbors[i])
                if (neighbor >= 0 && root[neighbor] == group)
                    diagonal[group] -= 1;
        }
        HarmonicLevel next;
        next.diagonal.reserve(diagonal.size());
        for (double value : diagonal) {
            require(value > 0, "Invalid Smart fill boundary system");
            next.diagonal.append(float(value));
        }
        next.workspace.resize(coarse.size());
        levels.last().parent = std::move(parent);
        levels.append(std::move(next));
        positions = std::move(coarse);
    }
    return levels;
}
void harmonicMultiply(const HarmonicGraph &graph, const QVector<Color> &values, QVector<Color> &result,
                      std::atomic_bool *cancel) {
    for (int i = 0; i < values.size(); ++i) {
        if ((i & 16383) == 0)
            check(cancel);
        Color sum{values[i][0] * graph.diagonal[i], values[i][1] * graph.diagonal[i],
                  values[i][2] * graph.diagonal[i]};
        for (int neighbor : graph.neighbors[i])
            if (neighbor >= 0)
                for (int c = 0; c < 3; ++c)
                    sum[c] -= values[neighbor][c];
        result[i] = sum;
    }
}
void harmonicPrecondition(QVector<HarmonicLevel> &levels, const QVector<Color> &residual,
                          QVector<Color> &result, std::atomic_bool *cancel) {
    for (int l = 1; l < levels.size(); ++l) {
        check(cancel);
        auto &coarse = levels[l].workspace;
        std::fill(coarse.begin(), coarse.end(), Color{});
        const auto &fine = l == 1 ? residual : levels[l - 1].workspace;
        const auto &parent = levels[l - 1].parent;
        for (int i = 0; i < fine.size(); ++i) {
            if ((i & 16383) == 0)
                check(cancel);
            for (int c = 0; c < 3; ++c)
                coarse[parent[i]][c] += fine[i][c];
        }
    }
    for (int l = levels.size() - 1; l >= 1; --l) {
        check(cancel);
        auto &values = levels[l].workspace;
        for (int i = 0; i < values.size(); ++i) {
            if ((i & 16383) == 0)
                check(cancel);
            for (int c = 0; c < 3; ++c)
                values[i][c] = values[i][c] / levels[l].diagonal[i] +
                               (l + 1 < levels.size() ? levels[l + 1].workspace[levels[l].parent[i]][c] : 0);
        }
    }
    for (int i = 0; i < residual.size(); ++i) {
        if ((i & 16383) == 0)
            check(cancel);
        for (int c = 0; c < 3; ++c)
            result[i][c] = residual[i][c] / levels[0].diagonal[i] +
                           (levels.size() > 1 ? levels[1].workspace[levels[0].parent[i]][c] : 0);
    }
}
double harmonicDot(const QVector<Color> &a, const QVector<Color> &b) {
    double result = 0;
    for (int i = 0; i < a.size(); ++i)
        for (int c = 0; c < 3; ++c)
            result += double(a[i][c]) * b[i][c];
    return result;
}
void solveHarmonic(const HarmonicGraph &graph, const QVector<Color> &rhs, QVector<Color> &solution,
                   std::atomic_bool *cancel) {
    if (rhs.isEmpty())
        return;
    const int count = rhs.size();
    QVector<Color> residual(count), preconditioned(count), direction(count), product(count);
    harmonicMultiply(graph, solution, product, cancel);
    for (int i = 0; i < count; ++i)
        for (int c = 0; c < 3; ++c)
            residual[i][c] = rhs[i][c] - product[i][c];
    const double initial = harmonicDot(residual, residual);
    const double tolerance = std::max(1e-8 * count, initial * 1e-12);
    if (initial <= tolerance)
        return;
    auto levels = harmonicLevels(graph, cancel);
    harmonicPrecondition(levels, residual, preconditioned, cancel);
    direction = preconditioned;
    double previous = harmonicDot(residual, preconditioned);
    bool converged = false;
    for (int iteration = 0; iteration < 384; ++iteration) {
        check(cancel);
        harmonicMultiply(graph, direction, product, cancel);
        const double denominator = harmonicDot(direction, product);
        require(denominator > 0 && std::isfinite(previous), "Cannot solve Smart fill boundary matching");
        const double alpha = previous / denominator;
        for (int i = 0; i < count; ++i)
            for (int c = 0; c < 3; ++c) {
                solution[i][c] += float(alpha * direction[i][c]);
                residual[i][c] -= float(alpha * product[i][c]);
            }
        if (harmonicDot(residual, residual) <= tolerance) {
            converged = true;
            break;
        }
        harmonicPrecondition(levels, residual, preconditioned, cancel);
        const double next = harmonicDot(residual, preconditioned);
        const double beta = next / previous;
        for (int i = 0; i < count; ++i)
            for (int c = 0; c < 3; ++c)
                direction[i][c] = preconditioned[i][c] + float(beta * direction[i][c]);
        previous = next;
    }
    require(converged || harmonicDot(residual, residual) <= 2.5e-7 * count,
            "Smart fill boundary matching did not converge. Retry processing the retained generated image.");
    check(cancel);
}
QImage repairMaterial(const QJsonObject &plan, const QString &material, const QImage &candidate,
                      std::atomic_bool *cancel) {
    const auto saved = savedMaterial(plan, material);
    const QImage source = image(saved["color"], QImage::Format_RGB32);
    const QImage coverage = image(saved["coverage"], QImage::Format_Grayscale8);
    const QImage mask = image(plan["masks"].toObject()[material], QImage::Format_Grayscale8);
    const QImage eligible = image(plan["eligibleMasks"].toObject()[material], QImage::Format_Grayscale8);
    require(source.size() == coverage.size() && source.size() == mask.size() &&
                source.size() == eligible.size() && source.size() == candidate.size(),
            "Smart fill atlas dimensions changed");
    const QVector<int> owners = charts(plan["domains"].toObject()[material].toObject(), source.size());
    const int width = source.width(), height = source.height(), count = width * height;
    QImage repaired = image(plan["repairs"].toObject()[material], QImage::Format_RGBA8888);
    if (repaired.isNull()) {
        repaired = QImage(source.size(), QImage::Format_RGBA8888);
        repaired.fill(Qt::transparent);
    }
    require(repaired.size() == source.size(), "Smart fill repair dimensions changed");
    auto target = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < height && owners[y * width + x] >= 0 &&
               mask.constScanLine(y)[x] && eligible.constScanLine(y)[x] && !coverage.constScanLine(y)[x];
    };
    auto known = [&](int x, int y, int chart) {
        return x >= 0 && y >= 0 && x < width && y < height && owners[y * width + x] == chart &&
               coverage.constScanLine(y)[x];
    };
    QVector<int> indices(count, -1), pixels;
    for (int y = 0; y < height; ++y) {
        check(cancel);
        for (int x = 0; x < width; ++x)
            if (target(x, y)) {
                indices[y * width + x] = pixels.size();
                pixels.append(y * width + x);
            }
    }
    const int size = pixels.size();
    QVector<Color> correction(size, Color{});
    QVector<quint8> pinned(size, 0);
    QVector<int> component(size, -1), variable(size, -1), unknown;
    constexpr int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    // Fix an inner collar using accepted pixels only. Never compare against the AI's
    // redrawn known area: its layout and colors may differ arbitrarily from accepted paint.
    for (int i = 0; i < size; ++i) {
        if ((i & 4095) == 0)
            check(cancel);
        const int pixel = pixels[i], x = pixel % width, y = pixel / width, chart = owners[pixel];
        Color boundary{};
        float weight = 0;
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                if ((!ox && !oy) || !known(x + ox, y + oy, chart))
                    continue;
                const QRgb first = source.pixel(x + ox, y + oy);
                Color value{float(qRed(first)), float(qGreen(first)), float(qBlue(first))};
                // Continue a small, consistent local trend; noisy texture or a feature edge
                // instead uses the adjacent accepted color without amplifying it.
                if (known(x + 2 * ox, y + 2 * oy, chart) && known(x + 3 * ox, y + 3 * oy, chart)) {
                    const QRgb second = source.pixel(x + 2 * ox, y + 2 * oy);
                    const QRgb third = source.pixel(x + 3 * ox, y + 3 * oy);
                    const Color a{float(qRed(second)), float(qGreen(second)), float(qBlue(second))};
                    const Color b{float(qRed(third)), float(qGreen(third)), float(qBlue(third))};
                    for (int c = 0; c < 3; ++c) {
                        const float slope = value[c] - a[c];
                        if (std::abs(slope) <= 8 && std::abs(slope - (a[c] - b[c])) <= 2)
                            value[c] = std::clamp(value[c] + slope, 0.f, 255.f);
                    }
                }
                const float sampleWeight = ox && oy ? .70710678f : 1.f;
                for (int c = 0; c < 3; ++c)
                    boundary[c] += value[c] * sampleWeight;
                weight += sampleWeight;
            }
        if (weight > 0) {
            pinned[i] = 1;
            const QRgb generated = candidate.pixel(x, y);
            const Color generatedColor{float(qRed(generated)), float(qGreen(generated)),
                                       float(qBlue(generated))};
            for (int c = 0; c < 3; ++c)
                correction[i][c] = boundary[c] / weight - generatedColor[c];
        }
    }
    QVector<int> queue;
    int componentCount = 0;
    for (int start = 0; start < size; ++start) {
        if (component[start] >= 0)
            continue;
        check(cancel);
        queue.clear();
        queue.append(start);
        component[start] = componentCount;
        Color minimum{1000, 1000, 1000}, maximum{-1000, -1000, -1000};
        std::array<double, 3> sum{};
        int anchors = 0;
        for (int head = 0; head < queue.size(); ++head) {
            if ((head & 4095) == 0)
                check(cancel);
            const int i = queue[head], pixel = pixels[i], x = pixel % width, y = pixel / width;
            if (pinned[i]) {
                ++anchors;
                for (int c = 0; c < 3; ++c) {
                    sum[c] += correction[i][c];
                    minimum[c] = std::min(minimum[c], correction[i][c]);
                    maximum[c] = std::max(maximum[c], correction[i][c]);
                }
            }
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                    continue;
                const int nextPixel = ny * width + nx, next = indices[nextPixel];
                if (next < 0 || component[next] >= 0 || owners[nextPixel] != owners[pixel])
                    continue;
                component[next] = componentCount;
                queue.append(next);
            }
        }
        ++componentCount;
        // A blank disconnected chart supplies no color boundary: retain the generated
        // appearance rather than borrowing from a nearby chart or another material.
        if (!anchors)
            continue;
        const Color average{float(sum[0] / anchors), float(sum[1] / anchors), float(sum[2] / anchors)};
        const bool constant = maximum[0] - minimum[0] < .001f && maximum[1] - minimum[1] < .001f &&
                              maximum[2] - minimum[2] < .001f;
        for (int i : queue) {
            if (pinned[i])
                continue;
            correction[i] = average;
            if (!constant) {
                variable[i] = unknown.size();
                unknown.append(i);
            }
        }
    }
    HarmonicGraph graph;
    graph.neighbors.resize(unknown.size());
    graph.diagonal.resize(unknown.size());
    graph.diagonal.fill(0);
    graph.positions.resize(unknown.size());
    QVector<Color> rhs(unknown.size(), Color{}), solution(unknown.size());
    for (int row = 0; row < unknown.size(); ++row) {
        if ((row & 4095) == 0)
            check(cancel);
        const int i = unknown[row], pixel = pixels[i], x = pixel % width, y = pixel / width;
        graph.neighbors[row] = {-1, -1, -1, -1};
        graph.positions[row] = {x, y, component[i]};
        solution[row] = correction[i];
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                continue;
            const int nextPixel = ny * width + nx, next = indices[nextPixel];
            if (next < 0 || owners[nextPixel] != owners[pixel])
                continue;
            graph.diagonal[row] += 1;
            graph.neighbors[row][k] = variable[next];
            if (variable[next] < 0)
                for (int c = 0; c < 3; ++c)
                    rhs[row][c] += correction[next][c];
        }
    }
    solveHarmonic(graph, rhs, solution, cancel);
    for (int row = 0; row < unknown.size(); ++row)
        correction[unknown[row]] = solution[row];
    for (int i = 0; i < size; ++i) {
        if ((i & 4095) == 0)
            check(cancel);
        const int position = pixels[i], x = position % width, y = position / width;
        const QRgb generated = candidate.pixel(x, y);
        auto pixel = repaired.scanLine(y) + x * 4;
        pixel[0] = uchar(std::clamp(qRound(qRed(generated) + correction[i][0]), 0, 255));
        pixel[1] = uchar(std::clamp(qRound(qGreen(generated) + correction[i][1]), 0, 255));
        pixel[2] = uchar(std::clamp(qRound(qBlue(generated) + correction[i][2]), 0, 255));
        pixel[3] = 255;
    }
    return repaired;
}
struct Context {
    QImage source, coverage, repair, mask;
    QVector<int> owners;
    QRect core, crop;
    int chart = -1;
    bool contains(int x, int y) const {
        return x >= 0 && y >= 0 && x < source.width() && y < source.height() &&
               owners[y * source.width() + x] == chart;
    }
    bool known(int x, int y) const {
        return contains(x, y) &&
               (coverage.constScanLine(y)[x] || (!repair.isNull() && repair.constScanLine(y)[x * 4 + 3]));
    }
    QRgb color(int x, int y) const {
        if (coverage.constScanLine(y)[x])
            return source.pixel(x, y);
        const auto p = repair.constScanLine(y) + x * 4;
        return qRgb(p[0], p[1], p[2]);
    }
    bool target(int x, int y) const {
        return core.contains(x, y) && contains(x, y) && mask.constScanLine(y)[x] &&
               !coverage.constScanLine(y)[x];
    }
};
Context context(const QJsonObject &plan, const QJsonObject &patch) {
    Context c;
    const QString material = patch["material"].toString();
    QJsonObject saved;
    for (const auto &value : plan["source"].toArray())
        if (value.toObject()["material"].toString() == material) {
            saved = value.toObject();
            break;
        }
    require(!saved.isEmpty(), "Smart fill source material is missing");
    c.source = image(saved["color"], QImage::Format_RGB32);
    c.coverage = image(saved["coverage"], QImage::Format_Grayscale8);
    c.mask = image(plan["masks"].toObject()[material], QImage::Format_Grayscale8);
    c.repair = image(plan["repairs"].toObject()[material], QImage::Format_RGBA8888);
    require(c.source.size() == c.coverage.size() && c.source.size() == c.mask.size() &&
                (c.repair.isNull() || c.source.size() == c.repair.size()),
            "Smart fill atlas dimensions changed");
    c.owners = charts(plan["domains"].toObject()[material].toObject(), c.source.size());
    c.core = rect(patch["core"]);
    c.crop = rect(patch["crop"]);
    c.chart = patch["chart"].toInt(-1);
    require(c.chart >= 0 && c.crop.size() == QSize(requestSize, requestSize) &&
                QRect(QPoint(0, 0), c.source.size()).contains(c.core),
            "Invalid Smart fill patch");
    return c;
}
void hashImage(QCryptographicHash &hash, const QImage &input, QImage::Format format, int channels,
               std::atomic_bool *cancel) {
    const QImage im = input.convertToFormat(format);
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream << qint32(im.width()) << qint32(im.height());
    hash.addData(header);
    for (int y = 0; y < im.height(); ++y) {
        check(cancel);
        hash.addData(
            QByteArrayView(reinterpret_cast<const char *>(im.constScanLine(y)), im.width() * channels));
    }
}
} // namespace

QJsonObject aiPlanSmartFill(const Scene &, int uv, const AiAtlases &source, const AiUvDomains &domains,
                            const QSet<QString> &materials, const QByteArray &fingerprint,
                            std::atomic_bool *cancel) {
    QJsonObject savedDomains, masks;
    QJsonArray selected;
    for (auto it = source.begin(); it != source.end(); ++it) {
        check(cancel);
        if ((!materials.isEmpty() && !materials.contains(it.key())) || !domains.contains(it.key()))
            continue;
        const auto &domain = domains[it.key()];
        const QImage occupancy = domain.occupancy.convertToFormat(QImage::Format_Grayscale8);
        const QImage coverage = it->coverage.convertToFormat(QImage::Format_Grayscale8);
        require(coverage.size() == occupancy.size() && it->color.size() == coverage.size() &&
                    domain.charts.size() == coverage.width() * coverage.height(),
                "Invalid Smart fill UV domain");
        bool painted = false, missing = false;
        QImage mask(coverage.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < coverage.height(); ++y) {
            check(cancel);
            for (int x = 0; x < coverage.width(); ++x) {
                if (!occupancy.constScanLine(y)[x])
                    continue;
                if (coverage.constScanLine(y)[x])
                    painted = true;
                else if (domain.charts[y * coverage.width() + x] >= 0) {
                    mask.scanLine(y)[x] = 255;
                    missing = true;
                }
            }
        }
        // An entirely blank material has no accepted appearance to continue. Blank charts in a painted
        // material are intentionally included, even if no same-chart boundary reference exists yet.
        if (!painted || !missing)
            continue;
        masks[it.key()] = png(mask);
        selected.append(it.key());
        savedDomains[it.key()] =
            QJsonObject{{"occupancy", png(occupancy)}, {"charts", chartData(domain.charts)}};
    }
    // Projection accumulators are never needed for repair context. Retain the compact source colors,
    // coverage and provenance once, rather than a Float32 copy of every atlas in each pending plan.
    AiAtlases contextSource = source;
    for (auto &atlas : contextSource)
        atlas.projectionWeight.clear();
    const QJsonArray savedSource = aiSerializeAtlases(contextSource);
    QJsonObject result{{"version", 2},
                       {"kind", "uv_repair"},
                       {"uv", uv},
                       {"fingerprint", QString::fromLatin1(fingerprint.toHex())},
                       {"source", savedSource},
                       {"domains", savedDomains},
                       {"materials", selected},
                       {"eligibleMasks", masks},
                       {"masks", masks},
                       {"repairs", QJsonObject{}},
                       {"requestSize", requestSize}};
    planBoard(result, cancel);
    check(cancel);
    return result;
}

QJsonObject aiSmartFillSetMasks(QJsonObject plan, const QMap<QString, QImage> &input,
                                std::atomic_bool *cancel) {
    check(cancel);
    for (const auto &patch : plan["patches"].toArray())
        require(!patch.toObject()["done"].toBool(),
                "Completed Smart fill masks cannot be edited; keep or discard the candidate first");
    QJsonObject masks = plan["masks"].toObject();
    const auto eligible = plan["eligibleMasks"].toObject();
    for (auto it = input.begin(); it != input.end(); ++it) {
        check(cancel);
        if (!eligible.contains(it.key()))
            continue;
        QImage original = image(eligible[it.key()], QImage::Format_Grayscale8);
        const QImage requested = it->convertToFormat(QImage::Format_Grayscale8);
        require(requested.size() == original.size(), "Smart fill masks must keep their native resolution");
        for (int y = 0; y < original.height(); ++y) {
            check(cancel);
            for (int x = 0; x < original.width(); ++x)
                original.scanLine(y)[x] =
                    original.constScanLine(y)[x] && requested.constScanLine(y)[x] ? 255 : 0;
        }
        masks[it.key()] = png(original);
    }
    plan["masks"] = masks;
    if (plan["version"].toInt() >= 2)
        planBoard(plan, cancel);
    else
        plan["patches"] = patches(plan);
    return plan;
}

int aiSmartFillRemaining(const QJsonObject &plan) {
    int count = 0;
    for (const auto &patch : plan["patches"].toArray())
        count += !patch.toObject()["done"].toBool();
    return count;
}

QJsonObject aiSmartFillRequest(const QJsonObject &plan, int patchIndex, const QString &workDir,
                               const QImage &appearance, std::atomic_bool *cancel) {
    check(cancel);
    require(plan["version"].toInt() == 2,
            "This saved cropped Smart fill candidate can still be previewed or applied. Regenerate preview "
            "to fill all selected gaps with one AI request.");
    const auto list = plan["patches"].toArray();
    require(patchIndex == 0 && list.size() == 1, "Smart fill requires one combined atlas request");
    const auto patch = list[patchIndex].toObject();
    require(!patch["done"].toBool(), "Smart fill patch has already completed");
    const int boardSize = plan["boardSize"].toInt();
    require(boardSize >= 1 && boardSize <= 4096, "Invalid Smart fill atlas board dimensions");
    const auto layout = plan["layout"].toObject();
    require(!layout.isEmpty(), "Smart fill has no selected missing paint");
    QImage texture(boardSize, boardSize, QImage::Format_RGB32);
    QImage guidance(boardSize, boardSize, QImage::Format_RGB32);
    QImage editMask(boardSize, boardSize, QImage::Format_RGBA8888);
    texture.fill(Qt::white);
    guidance.fill(Qt::black);
    editMask.fill(Qt::white);
    for (auto it = layout.begin(); it != layout.end(); ++it) {
        check(cancel);
        const QRect panel = rect(it.value());
        require(!panel.isEmpty() && texture.rect().contains(panel), "Invalid Smart fill material layout");
        const auto saved = savedMaterial(plan, it.key());
        const QImage source = image(saved["color"], QImage::Format_RGB32);
        const QImage coverage = image(saved["coverage"], QImage::Format_Grayscale8);
        const QImage occupancy =
            image(plan["domains"].toObject()[it.key()].toObject()["occupancy"], QImage::Format_Grayscale8);
        const QImage mask = image(plan["masks"].toObject()[it.key()], QImage::Format_Grayscale8);
        const QImage eligible = image(plan["eligibleMasks"].toObject()[it.key()], QImage::Format_Grayscale8);
        require(source.size() == coverage.size() && source.size() == occupancy.size() &&
                    source.size() == mask.size() && source.size() == eligible.size(),
                "Smart fill atlas dimensions changed");
        QImage material(source.size(), QImage::Format_RGB32);
        material.fill(Qt::white);
        for (int y = 0; y < source.height(); ++y) {
            check(cancel);
            auto pixels = reinterpret_cast<QRgb *>(material.scanLine(y));
            for (int x = 0; x < source.width(); ++x) {
                if (!occupancy.constScanLine(y)[x])
                    continue;
                pixels[x] = coverage.constScanLine(y)[x] ? source.pixel(x, y) : qRgb(128, 128, 128);
                if (!mask.constScanLine(y)[x] || !eligible.constScanLine(y)[x] ||
                    coverage.constScanLine(y)[x])
                    continue;
                // OR every native repair texel into its entire board-pixel footprint. Tiny gaps
                // remain visible even when many material atlases must share a smaller board.
                const int left = panel.x() + x * panel.width() / source.width();
                const int right = panel.x() + ((x + 1) * panel.width() + source.width() - 1) / source.width();
                const int top = panel.y() + y * panel.height() / source.height();
                const int bottom =
                    panel.y() + ((y + 1) * panel.height() + source.height() - 1) / source.height();
                for (int by = top; by < bottom; ++by) {
                    auto guide = reinterpret_cast<QRgb *>(guidance.scanLine(by));
                    auto alpha = editMask.scanLine(by);
                    for (int bx = left; bx < right; ++bx) {
                        guide[bx] = qRgb(255, 255, 255);
                        alpha[bx * 4 + 3] = 0;
                    }
                }
            }
        }
        QPainter painter(&texture);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(panel, material);
    }
    QDir dir(workDir);
    require(dir.exists() || dir.mkpath("."), "Cannot create Smart fill request directory");
    const QString id = "smart_fill_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString cropPath = dir.absoluteFilePath(id + "_texture.png");
    const QString mapPath = dir.absoluteFilePath(id + "_repair_map.png");
    const QString maskPath = dir.absoluteFilePath(id + "_alpha_mask.png");
    const QString appearancePath = dir.absoluteFilePath(id + "_appearance.png");
    require(texture.save(cropPath), "Cannot save Smart fill texture image");
    check(cancel);
    require(guidance.save(mapPath), "Cannot save Smart fill repair map");
    check(cancel);
    require(editMask.save(maskPath), "Cannot save Smart fill alpha mask");
    check(cancel);
    require((appearance.isNull() ? texture : appearance).save(appearancePath),
            "Cannot save Smart fill appearance image");
    check(cancel);
    return {
        {"id", id},
        {"kind", "uv_repair"},
        {"purpose", "uv_repair"},
        {"images", QJsonArray{cropPath, mapPath, appearancePath}},
        {"mask", maskPath},
        {"prompt", aiGptUvRepairPrompt() +
                       " Material atlas panel mappings in image 1 (x, y, width, height in input pixels): " +
                       QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact)) + "."},
        {"output", dir.absoluteFilePath(id + "_result.png")},
        {"workDir", dir.absolutePath()},
        {"size", requestSize},
        {"inputSize", boardSize},
        {"patchIndex", patchIndex}};
}

QJsonObject aiSmartFillAcceptPatch(QJsonObject plan, int patchIndex, const QImage &generated,
                                   std::atomic_bool *cancel) {
    check(cancel);
    auto list = plan["patches"].toArray();
    require(patchIndex >= 0 && patchIndex < list.size() && !generated.isNull(),
            "Invalid Smart fill patch result");
    auto patch = list[patchIndex].toObject();
    require(!patch["done"].toBool(), "Smart fill patch has already completed");
    if (plan["version"].toInt() == 2) {
        require(patchIndex == 0 && list.size() == 1, "Smart fill requires one combined atlas result");
        const int boardSize = plan["boardSize"].toInt();
        require(boardSize >= 1 && boardSize <= 4096, "Invalid Smart fill atlas board dimensions");
        const QImage colors = generated.convertToFormat(QImage::Format_RGB32);
        const auto layout = plan["layout"].toObject();
        auto repairs = plan["repairs"].toObject();
        for (auto it = layout.begin(); it != layout.end(); ++it) {
            check(cancel);
            const QRect panel = rect(it.value());
            require(!panel.isEmpty() && QRect(0, 0, boardSize, boardSize).contains(panel),
                    "Invalid Smart fill material layout");
            const QImage source = image(savedMaterial(plan, it.key())["color"], QImage::Format_RGB32);
            // Crop the returned panel BEFORE filtering so another material cannot bleed through
            // the resampling kernel at a board-cell boundary.
            const int left =
                std::clamp(qRound(double(panel.x()) * colors.width() / boardSize), 0, colors.width() - 1);
            const int top =
                std::clamp(qRound(double(panel.y()) * colors.height() / boardSize), 0, colors.height() - 1);
            const int right =
                std::clamp(qRound(double(panel.x() + panel.width()) * colors.width() / boardSize), left + 1,
                           colors.width());
            const int bottom =
                std::clamp(qRound(double(panel.y() + panel.height()) * colors.height() / boardSize), top + 1,
                           colors.height());
            const QImage candidate =
                colors.copy(QRect(left, top, right - left, bottom - top))
                    .scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            repairs[it.key()] = png(repairMaterial(plan, it.key(), candidate, cancel));
        }
        check(cancel);
        patch["generated"] = png(generated.convertToFormat(QImage::Format_RGB32));
        patch["done"] = true;
        list[patchIndex] = patch;
        plan["repairs"] = repairs;
        plan["patches"] = list;
        plan["matchingVersion"] = 2;
        return plan;
    }
    const Context c = context(plan, patch);
    // Only returned colors are resampled. The frozen atlas, chart labels, core mapping and write mask never
    // are.
    const QImage colors =
        generated.scaled(requestSize, requestSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB32);
    QImage repaired = c.repair;
    if (repaired.isNull()) {
        repaired = QImage(c.source.size(), QImage::Format_RGBA8888);
        repaired.fill(Qt::transparent);
    }
    // Propagate a local color offset from already accepted boundary pixels through this chart's holes.
    // All correction is applied on the hole side; no accepted pixel is ever copied from the AI output.
    constexpr int reach = 32;
    QVector<int> distance(requestSize * requestSize, -1);
    QVector<std::array<float, 3>> delta(requestSize * requestSize);
    std::deque<int> queue;
    constexpr int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    for (int y = 0; y < requestSize; ++y) {
        check(cancel);
        for (int x = 0; x < requestSize; ++x) {
            const int ax = c.crop.x() + x, ay = c.crop.y() + y;
            if (!c.contains(ax, ay) || c.known(ax, ay))
                continue;
            std::array<float, 3> offset{};
            int samples = 0;
            for (int k = 0; k < 4; ++k) {
                const int nx = ax + dx[k], ny = ay + dy[k], gx = x + dx[k], gy = y + dy[k];
                if (gx < 0 || gy < 0 || gx >= requestSize || gy >= requestSize || !c.known(nx, ny))
                    continue;
                const QRgb accepted = c.color(nx, ny), candidate = colors.pixel(gx, gy);
                offset[0] += qRed(accepted) - qRed(candidate);
                offset[1] += qGreen(accepted) - qGreen(candidate);
                offset[2] += qBlue(accepted) - qBlue(candidate);
                ++samples;
            }
            if (samples) {
                const int i = y * requestSize + x;
                for (int channel = 0; channel < 3; ++channel)
                    delta[i][channel] = offset[channel] / samples;
                distance[i] = 0;
                queue.push_back(i);
            }
        }
    }
    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        if ((i & 4095) == 0)
            check(cancel);
        if (distance[i] >= reach)
            continue;
        const int x = i % requestSize, y = i / requestSize;
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= requestSize || ny >= requestSize)
                continue;
            const int next = ny * requestSize + nx;
            const int ax = c.crop.x() + nx, ay = c.crop.y() + ny;
            if (distance[next] >= 0 || !c.contains(ax, ay) || c.known(ax, ay))
                continue;
            distance[next] = distance[i] + 1;
            delta[next] = delta[i];
            queue.push_back(next);
        }
    }
    for (int ay = c.core.top(); ay <= c.core.bottom(); ++ay) {
        check(cancel);
        for (int ax = c.core.left(); ax <= c.core.right(); ++ax) {
            if (!c.target(ax, ay))
                continue;
            const int x = ax - c.crop.x(), y = ay - c.crop.y(), i = y * requestSize + x;
            const QRgb candidate = colors.pixel(x, y);
            auto out = repaired.scanLine(ay) + ax * 4;
            const float fade = distance[i] < 0 ? 0 : float(reach - distance[i]) / reach;
            out[0] = uchar(std::clamp(qRound(qRed(candidate) + delta[i][0] * fade), 0, 255));
            out[1] = uchar(std::clamp(qRound(qGreen(candidate) + delta[i][1] * fade), 0, 255));
            out[2] = uchar(std::clamp(qRound(qBlue(candidate) + delta[i][2] * fade), 0, 255));
            out[3] = 255;
        }
    }
    check(cancel);
    auto repairs = plan["repairs"].toObject();
    repairs[patch["material"].toString()] = png(repaired);
    patch["generated"] = png(colors);
    patch["done"] = true;
    list[patchIndex] = patch;
    plan["repairs"] = repairs;
    plan["patches"] = list;
    return plan;
}

QJsonObject aiSmartFillReprocess(QJsonObject plan, std::atomic_bool *cancel) {
    check(cancel);
    auto list = plan["patches"].toArray();
    require(plan["version"].toInt() == 2 && list.size() == 1,
            "Only a saved one-request Smart fill candidate can be reprocessed");
    auto patch = list.first().toObject();
    require(patch["generated"].isString(), "No generated Smart fill image is available to reprocess");
    const QImage generated = image(patch["generated"], QImage::Format_RGB32);
    patch["done"] = false;
    list[0] = patch;
    plan["patches"] = list;
    plan["repairs"] = QJsonObject{};
    check(cancel);
    return aiSmartFillAcceptPatch(std::move(plan), 0, generated, cancel);
}

QJsonObject aiSmartFillLayer(const QJsonObject &plan) {
    AiLayerContribution contribution;
    const auto repairs = plan["repairs"].toObject();
    int resolution = 0;
    for (auto it = repairs.begin(); it != repairs.end(); ++it) {
        QImage pixels = image(it.value(), QImage::Format_RGBA8888);
        if (resolution)
            require(pixels.size() == QSize(resolution, resolution), "Smart fill atlas dimensions differ");
        resolution = pixels.width();
        contribution.materials[it.key()] = pixels;
    }
    require(resolution > 0, "Smart fill has no completed patches to apply");
    return {{"kind", "uv_fill"},
            {"layerId", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"name", "Smart fill"},
            {"visible", true},
            {"uvLayer", aiEncodeProjectionLayer(contribution, resolution)},
            {"uvMasks", plan["masks"]},
            {"smartFill", plan},
            {"matchingVersion", plan["matchingVersion"]},
            {"sourceFingerprint", plan["fingerprint"]}};
}

AiAtlases aiSmartFillPreview(const AiAtlases &source, const QJsonObject &plan, std::atomic_bool *cancel) {
    check(cancel);
    AiAtlases result = source;
    const auto repairs = plan["repairs"].toObject();
    for (auto it = repairs.begin(); it != repairs.end(); ++it) {
        check(cancel);
        if (!result.contains(it.key()))
            continue;
        const QImage repaired = image(it.value(), QImage::Format_RGBA8888);
        auto &atlas = result[it.key()];
        require(atlas.color.size() == repaired.size() && atlas.coverage.size() == repaired.size(),
                "Smart fill preview is stale");
        atlas.color = atlas.color.convertToFormat(QImage::Format_RGB32);
        atlas.coverage = atlas.coverage.convertToFormat(QImage::Format_Grayscale8);
        if (atlas.confidence.isNull()) {
            atlas.confidence = QImage(repaired.size(), QImage::Format_Grayscale16);
            atlas.confidence.fill(0);
        }
        if (atlas.repair.isNull()) {
            atlas.repair = QImage(repaired.size(), QImage::Format_Grayscale8);
            atlas.repair.fill(0);
        }
        for (int y = 0; y < repaired.height(); ++y) {
            check(cancel);
            const auto pixels = repaired.constScanLine(y);
            for (int x = 0; x < repaired.width(); ++x) {
                if (!pixels[x * 4 + 3] || atlas.coverage.constScanLine(y)[x])
                    continue;
                reinterpret_cast<QRgb *>(atlas.color.scanLine(y))[x] =
                    qRgb(pixels[x * 4], pixels[x * 4 + 1], pixels[x * 4 + 2]);
                atlas.coverage.scanLine(y)[x] = 255;
                reinterpret_cast<quint16 *>(atlas.confidence.scanLine(y))[x] = 1638;
                atlas.repair.scanLine(y)[x] = 255;
            }
        }
    }
    check(cancel);
    return result;
}

QByteArray aiSmartFillFingerprint(const Scene &scene, int uv, const AiAtlases &source,
                                  const QList<QJsonObject> &records, bool parity, std::atomic_bool *cancel) {
    check(cancel);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray geometry;
    QDataStream stream(&geometry, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << qint32(uv) << parity << quint64(scene.tris.size()) << scene.materials;
    for (const auto &triangle : scene.tris) {
        check(cancel);
        stream << qint32(triangle.material) << qint32(triangle.object);
        for (int i = 0; i < 3; ++i) {
            stream << triangle.p[i].x << triangle.p[i].y << triangle.p[i].z;
            stream << triangle.n[i].x << triangle.n[i].y << triangle.n[i].z;
            const V2 texcoord = uv >= 0 && uv < int(triangle.uv.size()) ? triangle.uv[uv][i] : V2{};
            stream << texcoord.x << texcoord.y;
        }
    }
    hash.addData(geometry);
    for (auto it = source.begin(); it != source.end(); ++it) {
        check(cancel);
        hash.addData(it.key().toUtf8());
        hashImage(hash, it->color, QImage::Format_RGBA8888, 4, cancel);
        hashImage(hash, it->coverage, QImage::Format_Grayscale8, 1, cancel);
        hashImage(hash, it->repair, QImage::Format_Grayscale8, 1, cancel);
    }
    // Exclude presentation names, current inspection camera, timestamps, and external export paths.
    const QStringList keys{"layerId",   "kind",      "visible", "blending", "options",
                           "uvLayer",   "uvMasks",   "mask",    "userMask", "sourceImage",
                           "candidate", "materials", "camera"};
    for (const auto &record : records) {
        check(cancel);
        QJsonObject stable;
        for (const QString &key : keys)
            if (record.contains(key))
                stable[key] = record[key];
        hash.addData(QJsonDocument(stable).toJson(QJsonDocument::Compact));
    }
    return hash.result();
}
