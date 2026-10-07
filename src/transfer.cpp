#include "transfer.h"
#include "transfer_gpu.h"
#include "transfer_shared.h"
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mikktspace.h>
#include <mutex>
#include <numeric>
#include <set>
#include <stb_image.h>
#include <stdexcept>
#include <thread>
#include <unordered_map>

static void fail(const QString &s) {
    throw std::runtime_error(s.toStdString());
}
template <class F> static void parallelFor(int n, F f) {
    int threads = std::max(1, std::min(n, (int)std::thread::hardware_concurrency()));
    if (threads <= 1) {
        for (int i = 0; i < n; i++)
            f(i);
        return;
    }
    std::atomic_int next{0};
    std::vector<std::thread> pool;
    std::exception_ptr error;
    std::mutex errorLock;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&] {
            try {
                for (int i; (i = next++) < n;)
                    f(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(errorLock);
                error = std::current_exception();
                next = n;
            }
        });
    for (auto &t : pool)
        t.join();
    if (error)
        std::rethrow_exception(error);
}

// ---------------------------------------------------------------------------------------------
// Job model
// ---------------------------------------------------------------------------------------------
QList<SuffixRule> defaultSuffixes() {
    auto r = [](const char *s, MapType t, const char *f) { return SuffixRule{s, t, f}; };
    return {r("ALB", MapType::Color, "#808080"),   r("BC", MapType::Color, "#808080"),
            r("BASECOLOR", MapType::Color, "#808080"), r("COL", MapType::Color, "#808080"),
            r("DIFF", MapType::Color, "#808080"),  r("NRM", MapType::Normal, "#8080FF"),
            r("NORMAL", MapType::Normal, "#8080FF"), r("RGH", MapType::Linear, "#808080"),
            r("ROUGH", MapType::Linear, "#808080"), r("MET", MapType::Linear, "#000000"),
            r("METAL", MapType::Linear, "#000000"), r("OCC", MapType::Linear, "#FFFFFF"),
            r("AO", MapType::Linear, "#FFFFFF"),   r("ORM", MapType::Packed, "#FF8000"),
            r("HGT", MapType::Linear, "#808080"),  r("OPA", MapType::Linear, "#FFFFFF"),
            r("ALPHA", MapType::Linear, "#FFFFFF"), r("EMI", MapType::Color, "#000000"),
            r("SSS", MapType::Color, "#000000"),   r("SPC", MapType::Linear, "#808080"),
            r("MSK", MapType::Linear, "#000000")};
}
QString mapTypeName(MapType t) {
    switch (t) {
    case MapType::Color:
        return "color";
    case MapType::Normal:
        return "normal";
    case MapType::Packed:
        return "packed";
    default:
        return "linear";
    }
}
MapType mapTypeFromName(const QString &s) {
    QString n = s.trimmed().toLower();
    if (n == "color" || n == "colour" || n == "srgb")
        return MapType::Color;
    if (n == "normal")
        return MapType::Normal;
    if (n == "packed")
        return MapType::Packed;
    return MapType::Linear;
}
static int suffixIndex(const QList<SuffixRule> &rules, const QString &suffix) {
    for (int i = 0; i < rules.size(); i++)
        if (rules[i].suffix.compare(suffix, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}
QList<SourceSet> discoverSets(const QStringList &files, const QList<SuffixRule> &rules, QStringList *unknown) {
    QList<SourceSet> sets;
    for (const QString &file : files) {
        QString base = QFileInfo(file).completeBaseName();
        int best = -1;
        for (int i = 0; i < rules.size(); i++) {
            const QString &s = rules[i].suffix;
            if (s.isEmpty() || base.size() <= s.size() + 1 || !base.endsWith(s, Qt::CaseInsensitive))
                continue;
            QChar sep = base[base.size() - s.size() - 1];
            if ((sep == '_' || sep == '-' || sep == '.') && (best < 0 || s.size() > rules[best].suffix.size()))
                best = i;
        }
        if (best < 0) {
            if (unknown)
                *unknown << file;
            continue;
        }
        QString name = base.left(base.size() - rules[best].suffix.size() - 1);
        auto it = std::find_if(sets.begin(), sets.end(), [&](const SourceSet &s) { return s.name == name; });
        if (it == sets.end()) {
            sets.push_back({name, {}});
            it = sets.end() - 1;
        }
        it->files[rules[best].suffix] = QFileInfo(file).absoluteFilePath();
    }
    std::sort(sets.begin(), sets.end(),
              [](const SourceSet &a, const SourceSet &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return sets;
}
QList<SourceSet> mergeSets(QList<SourceSet> existing, const QList<SourceSet> &added) {
    for (const SourceSet &a : added) {
        auto it = std::find_if(existing.begin(), existing.end(), [&](const SourceSet &s) { return s.name == a.name; });
        if (it == existing.end())
            existing.push_back(a);
        else
            for (auto f = a.files.begin(); f != a.files.end(); ++f)
                it->files[f.key()] = f.value();
    }
    return existing;
}
static QString normalized(QString s) {
    s = s.toLower();
    s.remove(QRegularExpression("\\.\\d+$"));
    // Drop naming tokens that mark a material, so "body_MAT_01" matches texture set "body_01".
    static const QSet<QString> noise{"mat", "mtl", "material", "shader", "sg", "shd"};
    QStringList kept;
    for (const QString &token : s.split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts))
        if (!noise.contains(token))
            kept << token;
    return kept.join("");
}
QMap<QString, QString> guessMaterialSets(const QStringList &materials, const QStringList &sets) {
    QMap<QString, QString> out;
    for (const QString &m : materials) {
        QString best;
        int bestScore = 0;
        QString nm = normalized(m);
        for (const QString &s : sets) {
            QString ns = normalized(s);
            int score = 0;
            if (nm.isEmpty() || ns.isEmpty())
                score = 0;
            else if (nm == ns)
                score = 1000;
            else if (nm.contains(ns) || ns.contains(nm))
                score = std::min(nm.size(), ns.size());
            if (score > bestScore)
                bestScore = score, best = s;
        }
        if (best.isEmpty() && sets.size() == 1)
            best = sets[0];
        out[m] = best;
    }
    return out;
}
static QString objectKey(QString s) {
    s = s.toLower();
    s.remove(QRegularExpression("\\.\\d+$"));
    static const QRegularExpression tag("[_\\-. ](low|high|lo|hi|src|source|dst|target|tgt|old|new)$");
    while (true) {
        QString before = s;
        s.remove(tag);
        if (s == before)
            break;
    }
    return s;
}
QMap<QString, QString> guessPairs(const QStringList &targets, const QStringList &sources) {
    QMap<QString, QString> out;
    for (const QString &t : targets) {
        QString match;
        for (const QString &s : sources)
            if (s == t) {
                match = s;
                break;
            }
        if (match.isEmpty())
            for (const QString &s : sources)
                if (objectKey(s) == objectKey(t)) {
                    match = s;
                    break;
                }
        if (match.isEmpty() && sources.size() == 1)
            match = sources[0];
        out[t] = match.isEmpty() ? "*" : match;
    }
    return out;
}
static QString safeFileName(QString s) {
    s.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"), "_");
    return s.trimmed();
}
QString suggestPrefix(const QStringList &names) {
    if (names.isEmpty())
        return "transfer";
    if (names.size() == 1)
        return safeFileName(names[0]);
    QString common = names[0];
    for (const QString &n : names) {
        int i = 0;
        while (i < common.size() && i < n.size() && common[i] == n[i])
            i++;
        common.truncate(i);
    }
    while (!common.isEmpty() && common.back().isDigit())
        common.chop(1);
    if (common.isEmpty())
        return safeFileName(names.join('-'));
    QStringList parts{names[0]};
    for (int i = 1; i < names.size(); i++)
        parts << names[i].mid(common.size());
    return safeFileName(parts.join('-'));
}
QList<OutputRule> defaultOutputs(const QStringList &materials, const QStringList &sets) {
    QList<OutputRule> out;
    for (const QString &m : materials)
        out.push_back({m, true, materials.size() == 1 ? suggestPrefix(sets) : safeFileName(m), 0});
    return out;
}

// JSON -----------------------------------------------------------------------------------------
QJsonObject TransferJob::toJson() const {
    QJsonObject o;
    o["source"] = QJsonObject{{"mesh", sourceMesh}, {"uv", settings.sourceUV}};
    o["target"] = QJsonObject{{"mesh", targetMesh}, {"uv", settings.targetUV}};
    o["output_dir"] = outputDir;
    QJsonArray sx;
    for (const SuffixRule &r : suffixes)
        sx.append(QJsonObject{{"suffix", r.suffix}, {"type", mapTypeName(r.type)}, {"fill", r.fill}});
    o["suffixes"] = sx;
    QJsonArray st;
    for (const SourceSet &s : sets) {
        QJsonObject files;
        for (auto it = s.files.begin(); it != s.files.end(); ++it)
            files[it.key()] = it.value();
        st.append(QJsonObject{{"name", s.name}, {"files", files}});
    }
    o["sets"] = st;
    QJsonObject mats;
    for (auto it = materialSets.begin(); it != materialSets.end(); ++it)
        mats[it.key()] = it.value();
    o["materials"] = mats;
    QJsonObject pr;
    for (auto it = pairs.begin(); it != pairs.end(); ++it)
        pr[it.key()] = it.value();
    o["pairing"] = QJsonObject{{"enabled", settings.pairing}, {"pairs", pr}};
    QJsonArray outs;
    for (const OutputRule &r : outputs)
        outs.append(QJsonObject{{"material", r.material},
                                {"enabled", r.enabled},
                                {"prefix", r.prefix},
                                {"resolution", r.resolution}});
    o["outputs"] = outs;
    const TransferSettings &s = settings;
    o["settings"] = QJsonObject{{"mode", s.mode ? "projection" : "auto"},
                                {"supersampling", s.ss},
                                {"filter", s.filter == FilterBilinear ? "bilinear"
                                           : s.filter == FilterBicubic ? "bicubic"
                                                                       : "auto"},
                                {"padding", s.padding},
                                {"bits", s.bits},
                                {"island_aware", s.islandAware},
                                {"rebuild_source_padding", s.rebuildSourcePadding},
                                {"average_overlaps", s.averageOverlaps},
                                {"source_normal_directx", s.sourceNormalDX},
                                {"target_normal_directx", s.targetNormalDX},
                                {"coverage_map", s.coverageMap},
                                {"overwrite", s.overwrite},
                                {"front_distance", s.front},
                                {"back_distance", s.back},
                                {"weld_tolerance", s.tolerance}};
    return o;
}
TransferJob TransferJob::fromJson(const QJsonObject &o, const QString &baseDir) {
    QDir base(baseDir);
    auto path = [&](const QString &p) { return p.isEmpty() ? p : QDir::cleanPath(base.absoluteFilePath(p)); };
    TransferJob j;
    j.sourceMesh = path(o["source"].toObject()["mesh"].toString());
    j.targetMesh = path(o["target"].toObject()["mesh"].toString());
    j.settings.sourceUV = o["source"].toObject()["uv"].toInt();
    j.settings.targetUV = o["target"].toObject()["uv"].toInt();
    j.outputDir = path(o["output_dir"].toString());
    if (o.contains("suffixes"))
        for (const auto &v : o["suffixes"].toArray()) {
            QJsonObject r = v.toObject();
            j.suffixes.push_back(
                {r["suffix"].toString().trimmed(), mapTypeFromName(r["type"].toString()), r["fill"].toString("#808080")});
        }
    else
        j.suffixes = defaultSuffixes();
    for (const auto &v : o["sets"].toArray()) {
        QJsonObject s = v.toObject();
        SourceSet set{s["name"].toString(), {}};
        QJsonObject files = s["files"].toObject();
        for (auto it = files.begin(); it != files.end(); ++it) {
            int r = suffixIndex(j.suffixes, it.key());
            set.files[r >= 0 ? j.suffixes[r].suffix : it.key()] = path(it.value().toString());
        }
        j.sets.push_back(set);
    }
    if (!j.sourceMesh.isEmpty())
        j.source = std::make_shared<Scene>(loadScene(j.sourceMesh));
    if (!j.targetMesh.isEmpty())
        j.target = std::make_shared<Scene>(loadScene(j.targetMesh));
    QStringList setNames;
    for (const SourceSet &s : j.sets)
        setNames << s.name;
    if (o.contains("materials")) {
        QJsonObject m = o["materials"].toObject();
        for (auto it = m.begin(); it != m.end(); ++it)
            j.materialSets[it.key()] = it.value().toString();
    } else if (j.source)
        j.materialSets = guessMaterialSets(j.source->materials, setNames);
    QJsonObject pairing = o["pairing"].toObject();
    j.settings.pairing = pairing["enabled"].toBool(true);
    if (pairing.contains("pairs")) {
        QJsonObject p = pairing["pairs"].toObject();
        for (auto it = p.begin(); it != p.end(); ++it)
            j.pairs[it.key()] = it.value().toString();
    } else if (j.source && j.target)
        j.pairs = guessPairs(j.target->objects, j.source->objects);
    if (o.contains("outputs"))
        for (const auto &v : o["outputs"].toArray()) {
            QJsonObject r = v.toObject();
            j.outputs.push_back({r["material"].toString(), r["enabled"].toBool(true), r["prefix"].toString(),
                                 r["resolution"].toInt()});
        }
    else if (j.target)
        j.outputs = defaultOutputs(j.target->materials, setNames);
    QJsonObject s = o["settings"].toObject();
    TransferSettings &t = j.settings;
    t.mode = s["mode"].toString() == "projection" ? 1 : 0;
    t.ss = s["supersampling"].toInt(t.ss);
    QString filter = s["filter"].toString("auto");
    t.filter = filter == "bilinear" ? FilterBilinear : filter == "bicubic" ? FilterBicubic : FilterAuto;
    t.padding = s["padding"].toInt(t.padding);
    t.bits = s["bits"].toInt(t.bits);
    t.islandAware = s["island_aware"].toBool(t.islandAware);
    t.rebuildSourcePadding = s["rebuild_source_padding"].toBool(t.rebuildSourcePadding);
    t.averageOverlaps = s["average_overlaps"].toBool(t.averageOverlaps);
    t.sourceNormalDX = s["source_normal_directx"].toBool(t.sourceNormalDX);
    t.targetNormalDX = s["target_normal_directx"].toBool(t.targetNormalDX);
    t.coverageMap = s["coverage_map"].toBool(t.coverageMap);
    t.overwrite = s["overwrite"].toBool(t.overwrite);
    t.front = (float)s["front_distance"].toDouble(t.front);
    t.back = (float)s["back_distance"].toDouble(t.back);
    t.tolerance = (float)s["weld_tolerance"].toDouble(t.tolerance);
    return j;
}

// Validation -----------------------------------------------------------------------------------
struct OutputGroup {
    QString prefix;
    QStringList materials;
    int resolution = 0;
};
static QList<OutputGroup> outputGroups(const TransferJob &job, QStringList *errors) {
    QList<OutputGroup> groups;
    for (const OutputRule &r : job.outputs) {
        if (!r.enabled)
            continue;
        QString prefix = safeFileName(r.prefix);
        if (prefix.isEmpty()) {
            if (errors)
                *errors << QString("Output for target material '%1' has no file prefix.").arg(r.material);
            continue;
        }
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const OutputGroup &g) { return g.prefix.compare(prefix, Qt::CaseInsensitive) == 0; });
        if (it == groups.end()) {
            groups.push_back({prefix, {}, r.resolution});
            it = groups.end() - 1;
        } else if (r.resolution && it->resolution && r.resolution != it->resolution && errors)
            *errors << QString("Materials sharing prefix '%1' request different resolutions.").arg(prefix);
        if (r.resolution)
            it->resolution = r.resolution;
        it->materials << r.material;
    }
    return groups;
}
static QSize imageSize(const QString &path) {
    QImageReader reader(path);
    QSize s = reader.size();
    if (!s.isValid() && path.endsWith(".tga", Qt::CaseInsensitive)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto bytes = f.read(1 << 16);
            int w, h, c;
            if (stbi_info_from_memory((const stbi_uc *)bytes.data(), (int)bytes.size(), &w, &h, &c))
                s = {w, h};
        }
    }
    return s;
}
// Suffixes in table order that appear in at least one used set.
static QStringList usedSuffixes(const TransferJob &job, const QList<int> &usedSets) {
    QStringList out;
    for (const SuffixRule &r : job.suffixes)
        for (int s : usedSets)
            if (job.sets[s].files.contains(r.suffix)) {
                out << r.suffix;
                break;
            }
    return out;
}
static QList<int> usedSets(const TransferJob &job) {
    QList<int> out;
    for (int i = 0; i < job.sets.size(); i++)
        for (auto it = job.materialSets.begin(); it != job.materialSets.end(); ++it)
            if (it.value() == job.sets[i].name && job.source && job.source->materials.contains(it.key())) {
                out << i;
                break;
            }
    return out;
}
static QString outputPath(const TransferJob &job, const QString &prefix, const QString &suffix) {
    return QDir::cleanPath(QDir(job.outputDir).absoluteFilePath(prefix + "_" + suffix + ".png"));
}
QStringList checkJob(const TransferJob &job, QStringList *warnings) {
    QStringList errors, warn;
    const TransferSettings &s = job.settings;
    if (!job.source)
        errors << "Load a source mesh.";
    if (!job.target)
        errors << "Load a target mesh.";
    if (job.source && (s.sourceUV < 0 || s.sourceUV >= job.source->uvNames.size()))
        errors << "The selected source UV set does not exist.";
    if (job.target && (s.targetUV < 0 || s.targetUV >= job.target->uvNames.size()))
        errors << "The selected target UV set does not exist.";
    if (job.outputDir.trimmed().isEmpty())
        errors << "Choose an output folder.";
    if (s.ss < 1 || s.ss > 4)
        errors << "Supersampling must be between 1 and 4.";
    if (s.padding < -1 || s.padding > 256)
        errors << "Padding must be between 0 and 256 texels (or -1 to fill the texture).";
    if (s.bits != 0 && s.bits != 8 && s.bits != 16)
        errors << "Bit depth must be automatic, 8 or 16.";
    if (!std::isfinite(s.front) || !std::isfinite(s.back) || s.front < 0 || s.back < 0 ||
        !std::isfinite(s.tolerance) || s.tolerance < 0)
        errors << "Projection distances and weld tolerance must be zero (automatic) or positive.";
    QSet<QString> suffixNames;
    for (const SuffixRule &r : job.suffixes) {
        if (r.suffix.isEmpty() || r.suffix.contains(QRegularExpression("[^A-Za-z0-9]")))
            errors << QString("Suffix '%1' must be alphanumeric.").arg(r.suffix);
        if (suffixNames.contains(r.suffix.toUpper()))
            errors << QString("Suffix '%1' is listed twice.").arg(r.suffix);
        suffixNames.insert(r.suffix.toUpper());
        if (!QColor(r.fill).isValid())
            errors << QString("Default value '%1' for %2 is not a colour (#RRGGBB or #AARRGGBB).").arg(r.fill, r.suffix);
    }
    QList<int> used = usedSets(job);
    if (job.source && used.isEmpty())
        errors << "Assign a texture set to at least one source material.";
    QStringList suffixes = usedSuffixes(job, used);
    QSet<QString> sourceFiles;
    for (int i : used) {
        const SourceSet &set = job.sets[i];
        for (auto it = set.files.begin(); it != set.files.end(); ++it) {
            if (suffixIndex(job.suffixes, it.key()) < 0)
                warn << QString("%1: suffix %2 is not in the suffix table and is skipped.").arg(set.name, it.key());
            if (!QFileInfo::exists(it.value()))
                errors << QString("Missing texture: %1").arg(QDir::toNativeSeparators(it.value()));
            else if (!imageSize(it.value()).isValid())
                errors << QString("Unreadable texture: %1").arg(QDir::toNativeSeparators(it.value()));
            sourceFiles.insert(QFileInfo(it.value()).absoluteFilePath().toLower());
        }
        for (const QString &sx : suffixes)
            if (!set.files.contains(sx))
                warn << QString("%1 has no %2 map; its default %3 is used.")
                            .arg(set.name, sx, job.suffixes[suffixIndex(job.suffixes, sx)].fill);
    }
    for (const QString &sx : suffixes) {
        QSize first;
        for (int i : used)
            if (job.sets[i].files.contains(sx)) {
                QSize z = imageSize(job.sets[i].files[sx]);
                if (first.isValid() && z.isValid() && z != first) {
                    warn << QString("%1 maps have different resolutions; each set is sampled at its own size.").arg(sx);
                    break;
                }
                if (z.isValid())
                    first = z;
            }
    }
    for (int i = 0; i < job.sets.size(); i++)
        if (!used.contains(i))
            warn << QString("Texture set %1 is not assigned to any source material.").arg(job.sets[i].name);
    if (job.source)
        for (const QString &m : job.source->materials)
            if (job.materialSets.value(m).isEmpty())
                warn << QString("Source material '%1' has no texture set; its surface transfers default values.").arg(m);
    QList<OutputGroup> groups = outputGroups(job, &errors);
    if (job.target && groups.isEmpty())
        errors << "Enable at least one target material output.";
    for (const OutputGroup &g : groups) {
        if (g.resolution < 0 || g.resolution > 16384)
            errors << QString("Output %1 has an invalid resolution.").arg(g.prefix);
        if (job.target)
            for (const QString &m : g.materials)
                if (!job.target->materials.contains(m))
                    errors << QString("Target material '%1' does not exist in the target mesh.").arg(m);
        for (const QString &sx : suffixes)
            if (sourceFiles.contains(QFileInfo(outputPath(job, g.prefix, sx)).absoluteFilePath().toLower()))
                errors << QString("Output %1_%2.png would overwrite a source texture.").arg(g.prefix, sx);
    }
    if (job.settings.pairing && job.source && job.target)
        for (auto it = job.pairs.begin(); it != job.pairs.end(); ++it)
            if (!it.value().isEmpty() && it.value() != "*" && !job.source->objects.contains(it.value()))
                errors << QString("Pairing for '%1' names unknown source object '%2'.").arg(it.key(), it.value());
    if (job.source && job.target) {
        float d = sqrtf(dot(job.source->center - job.target->center, job.source->center - job.target->center));
        float scale = std::max(job.source->scale, job.target->scale);
        if (d > scale * .05f || std::abs(job.source->scale - job.target->scale) > scale * .05f)
            warn << "The meshes do not occupy the same space; transfer requires both meshes in the same position.";
    }
    if (warnings)
        *warnings << warn;
    return errors;
}
QStringList existingOutputs(const TransferJob &job) {
    QStringList out;
    QStringList suffixes = usedSuffixes(job, usedSets(job));
    for (const OutputGroup &g : outputGroups(job, nullptr)) {
        for (const QString &sx : suffixes)
            if (QFileInfo::exists(outputPath(job, g.prefix, sx)))
                out << outputPath(job, g.prefix, sx);
        if (job.settings.coverageMap && QFileInfo::exists(outputPath(job, g.prefix, "COVERAGE")))
            out << outputPath(job, g.prefix, "COVERAGE");
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Geometry preparation
// ---------------------------------------------------------------------------------------------
struct MikkMesh {
    const std::vector<Triangle> *tris;
    int uv;
    std::vector<V3> *tangents;
    std::vector<float> *signs;
};
static MikkMesh *mikk(const SMikkTSpaceContext *c) {
    return (MikkMesh *)c->m_pUserData;
}
void meshTangents(const Scene &s, int uv, std::vector<V3> &tangents, std::vector<float> &signs) {
    tangents.assign(s.tris.size() * 3, {1, 0, 0});
    signs.assign(s.tris.size() * 3, 1.f);
    MikkMesh mesh{&s.tris, uv, &tangents, &signs};
    SMikkTSpaceInterface i{};
    i.m_getNumFaces = [](const SMikkTSpaceContext *c) { return (int)mikk(c)->tris->size(); };
    i.m_getNumVerticesOfFace = [](const SMikkTSpaceContext *, int) { return 3; };
    i.m_getPosition = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
        V3 p = (*mikk(c)->tris)[f].p[v];
        out[0] = p.x, out[1] = p.y, out[2] = p.z;
    };
    i.m_getNormal = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
        V3 n = (*mikk(c)->tris)[f].n[v];
        out[0] = n.x, out[1] = n.y, out[2] = n.z;
    };
    i.m_getTexCoord = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
        const Triangle &t = (*mikk(c)->tris)[f];
        V2 uv = mikk(c)->uv < (int)t.uv.size() ? t.uv[mikk(c)->uv][v] : V2{};
        out[0] = uv.x, out[1] = uv.y;
    };
    i.m_setTSpaceBasic = [](const SMikkTSpaceContext *c, const float t[], float sign, int f, int v) {
        (*mikk(c)->tangents)[f * 3 + v] = {t[0], t[1], t[2]};
        (*mikk(c)->signs)[f * 3 + v] = sign;
    };
    SMikkTSpaceContext ctx{&i, &mesh};
    if (!s.tris.empty() && !genTangSpaceDefault(&ctx))
        fail("Tangent generation failed.");
}
struct Words5 {
    uint32_t w[5];
    bool operator==(const Words5 &o) const { return std::equal(w, w + 5, o.w); }
};
struct Words5Hash {
    size_t operator()(const Words5 &k) const {
        uint32_t h = 2166136261u;
        for (uint32_t x : k.w)
            h = hash32(h ^ x);
        return h;
    }
};
static uint32_t bits(float f) {
    if (f == 0)
        f = 0; // fold -0
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
// UV islands: faces connected through vertices that share both position and UV.
static std::vector<int> uvIslands(const Scene &s, int uv) {
    std::vector<int> parent(s.tris.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    std::unordered_map<Words5, int, Words5Hash> first;
    first.reserve(s.tris.size() * 2);
    for (int i = 0; i < (int)s.tris.size(); i++)
        for (int k = 0; k < 3; k++) {
            const Triangle &t = s.tris[i];
            V2 c = uv < (int)t.uv.size() ? t.uv[uv][k] : V2{};
            Words5 key{{bits(t.p[k].x), bits(t.p[k].y), bits(t.p[k].z), bits(c.x), bits(c.y)}};
            auto [it, added] = first.emplace(key, i);
            if (!added)
                parent[find(i)] = find(it->second);
        }
    std::vector<int> island(s.tris.size());
    std::unordered_map<int, int> ids;
    for (int i = 0; i < (int)s.tris.size(); i++) {
        auto [it, added] = ids.emplace(find(i), (int)ids.size());
        island[i] = it->second;
    }
    return island;
}
static bool validUV(const V2 *uv) {
    for (int k = 0; k < 3; k++)
        if (!std::isfinite(uv[k].x) || !std::isfinite(uv[k].y))
            return false;
    return std::abs(orient(uv[0], uv[1], uv[2])) > 1e-14f;
}
struct Geometry {
    std::vector<XferTri> tris;
    std::vector<XferSrcTri> src;
    std::vector<unsigned char> allowed;
    std::vector<char> valid;
    int srcObjects = 1, dstObjects = 1, exact = 0;
};
static uint64_t cellHash(long long x, long long y, long long z) {
    return (uint64_t)x * 73856093ull ^ (uint64_t)y * 19349663ull ^ (uint64_t)z * 83492791ull;
}
static void matchTopology(const TransferJob &job, Geometry &g, float tol) {
    const Scene &S = *job.source, &T = *job.target;
    std::vector<V3> canon;
    std::unordered_map<uint64_t, std::vector<int>> grid;
    auto cell = [&](float v) { return (long long)std::floor(v / tol); };
    auto find = [&](V3 p) {
        long long cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
        int best = -1;
        float bestD = tol * tol;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    auto it = grid.find(cellHash(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end())
                        continue;
                    for (int id : it->second) {
                        V3 d = canon[id] - p;
                        float d2 = dot(d, d);
                        if (d2 <= bestD)
                            best = id, bestD = d2;
                    }
                }
        return best;
    };
    std::vector<int> srcIds(S.tris.size() * 3);
    for (size_t i = 0; i < S.tris.size(); i++)
        for (int k = 0; k < 3; k++) {
            V3 p = S.tris[i].p[k];
            int id = find(p);
            if (id < 0) {
                id = (int)canon.size();
                canon.push_back(p);
                grid[cellHash(cell(p.x), cell(p.y), cell(p.z))].push_back(id);
            }
            srcIds[i * 3 + k] = id;
        }
    std::map<std::array<int, 3>, std::vector<int>> faces;
    for (int i = 0; i < (int)S.tris.size(); i++) {
        std::array<int, 3> key{srcIds[i * 3], srcIds[i * 3 + 1], srcIds[i * 3 + 2]};
        std::sort(key.begin(), key.end());
        if (key[0] != key[1] && key[1] != key[2])
            faces[key].push_back(i);
    }
    std::vector<char> used(S.tris.size());
    for (size_t i = 0; i < T.tris.size(); i++) {
        XferTri &t = g.tris[i];
        int ids[3];
        for (int k = 0; k < 3; k++)
            ids[k] = find(T.tris[i].p[k]);
        if (ids[0] < 0 || ids[1] < 0 || ids[2] < 0 || ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])
            continue;
        std::array<int, 3> key{ids[0], ids[1], ids[2]};
        std::sort(key.begin(), key.end());
        auto it = faces.find(key);
        if (it == faces.end())
            continue;
        for (int c : it->second) {
            if (used[c] || !g.allowed[(size_t)t.object * g.srcObjects + g.src[c].object])
                continue;
            for (int k = 0; k < 3; k++)
                for (int m = 0; m < 3; m++)
                    if (srcIds[c * 3 + m] == ids[k])
                        t.perm[k] = m;
            t.match = c;
            used[c] = 1;
            if (g.valid[i])
                g.exact++;
            break;
        }
    }
}
static Geometry prepareGeometry(const TransferJob &job, float tol) {
    const Scene &S = *job.source, &T = *job.target;
    const TransferSettings &st = job.settings;
    Geometry g;
    g.srcObjects = std::max(1, (int)S.objects.size());
    g.dstObjects = std::max(1, (int)T.objects.size());
    std::vector<V3> tan;
    std::vector<float> sign;
    meshTangents(S, st.sourceUV, tan, sign);
    std::vector<int> islands = uvIslands(S, st.sourceUV);
    QMap<QString, int> setIndex;
    for (int i = 0; i < job.sets.size(); i++)
        setIndex[job.sets[i].name] = i;
    QList<int> used = usedSets(job);
    int unassigned = job.sets.size();
    g.src.resize(S.tris.size());
    for (size_t i = 0; i < S.tris.size(); i++) {
        const Triangle &t = S.tris[i];
        XferSrcTri &x = g.src[i];
        for (int k = 0; k < 3; k++) {
            x.uv[k] = t.uv[st.sourceUV][k];
            x.p[k] = t.p[k];
            x.n[k] = t.n[k];
            x.t[k] = tan[i * 3 + k];
            x.sign[k] = sign[i * 3 + k];
        }
        QString material = t.material >= 0 && t.material < S.materials.size() ? S.materials[t.material] : "";
        int set = setIndex.value(job.materialSets.value(material), unassigned);
        x.set = used.contains(set) ? set : unassigned;
        x.island = islands[i];
        x.object = std::max(0, t.object);
    }
    g.allowed.assign((size_t)g.dstObjects * g.srcObjects, 1);
    if (st.pairing)
        for (int i = 0; i < T.objects.size(); i++) {
            QString p = job.pairs.value(T.objects[i], "*");
            if (p == "*")
                continue;
            for (int j = 0; j < g.srcObjects; j++)
                g.allowed[(size_t)i * g.srcObjects + j] = j < S.objects.size() && S.objects[j] == p;
        }
    meshTangents(T, st.targetUV, tan, sign);
    islands = uvIslands(T, st.targetUV);
    g.tris.resize(T.tris.size());
    g.valid.resize(T.tris.size());
    for (size_t i = 0; i < T.tris.size(); i++) {
        const Triangle &t = T.tris[i];
        XferTri &x = g.tris[i];
        for (int k = 0; k < 3; k++) {
            x.uv[k] = t.uv[st.targetUV][k];
            x.p[k] = t.p[k];
            x.n[k] = t.n[k];
            x.t[k] = tan[i * 3 + k];
            x.sign[k] = sign[i * 3 + k];
            x.perm[k] = k;
        }
        x.match = -1;
        x.object = std::max(0, t.object);
        x.island = islands[i];
        g.valid[i] = validUV(x.uv);
    }
    if (st.mode == 0)
        matchTopology(job, g, tol);
    return g;
}
static void buildGrid(const std::vector<XferTri> &tris, const std::vector<int> &list, int grid,
                      std::vector<int> &offsets, std::vector<int> &indices) {
    offsets.assign(grid * grid + 1, 0);
    auto range = [&](const XferTri &t, int &x0, int &x1, int &y0, int &y1) {
        float lx = std::min({t.uv[0].x, t.uv[1].x, t.uv[2].x}), hx = std::max({t.uv[0].x, t.uv[1].x, t.uv[2].x});
        float ly = std::min({t.uv[0].y, t.uv[1].y, t.uv[2].y}), hy = std::max({t.uv[0].y, t.uv[1].y, t.uv[2].y});
        if (hx < 0 || hy < 0 || lx >= 1 || ly >= 1)
            return false;
        x0 = std::clamp((int)std::floor(lx * grid), 0, grid - 1);
        x1 = std::clamp((int)std::floor(hx * grid), 0, grid - 1);
        y0 = std::clamp((int)std::floor(ly * grid), 0, grid - 1);
        y1 = std::clamp((int)std::floor(hy * grid), 0, grid - 1);
        return true;
    };
    for (int pass = 0; pass < 2; pass++) {
        std::vector<int> cursor;
        if (pass) {
            for (int i = 0; i < grid * grid; i++)
                offsets[i + 1] += offsets[i];
            indices.resize(offsets.back());
            cursor.assign(offsets.begin(), offsets.end() - 1);
        }
        for (int i : list) {
            int x0, x1, y0, y1;
            if (!range(tris[i], x0, x1, y0, y1))
                continue;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    if (pass)
                        indices[cursor[y * grid + x]++] = i;
                    else
                        offsets[y * grid + x + 1]++;
                }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------
static int kindOf(MapType t) {
    return t == MapType::Color ? KindColor : t == MapType::Normal ? KindNormal : KindLinear;
}
static const float *srgbTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; i++)
            t[i] = srgbToLinear(i / 65535.f);
        return t;
    }();
    return table.data();
}
static uint16_t unorm16(float v) {
    return (uint16_t)std::lround(clamp01(v) * 65535.f);
}
static V4 fileToFilter(V4 v, int kind) {
    if (kind != KindColor)
        return v;
    return {srgbToLinear(v.x) * v.w, srgbToLinear(v.y) * v.w, srgbToLinear(v.z) * v.w, v.w};
}
static V4 fillValue(const SuffixRule &r) {
    QColor c(r.fill);
    if (!c.isValid())
        c = QColor(128, 128, 128);
    return {(float)c.redF(), (float)c.greenF(), (float)c.blueF(), (float)c.alphaF()};
}
struct Loaded {
    int w = 0, h = 0, bits = 8, channels = 4;
    std::vector<uint16_t> rgba; // filter space
};
QImage readTextureImage(const QString &path, QString *error) {
    QImageReader::setAllocationLimit(4096);
    QImageReader reader(path);
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull() && path.endsWith(".tga", Qt::CaseInsensitive)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto bytes = f.readAll();
            int w, h, c;
            auto data = stbi_load_from_memory((const stbi_uc *)bytes.data(), (int)bytes.size(), &w, &h, &c, 4);
            if (data) {
                image = QImage(data, w, h, w * 4, QImage::Format_RGBA8888).copy();
                if (c == 1)
                    image = image.convertToFormat(QImage::Format_Grayscale8);
                else if (c == 3)
                    image = image.convertToFormat(QImage::Format_RGB888);
                stbi_image_free(data);
            }
        }
    }
    if (image.isNull() && error)
        *error = reader.errorString();
    return image;
}
static Loaded loadTexture(const QString &path, int kind) {
    QString error;
    QImage image = readTextureImage(path, &error);
    if (image.isNull())
        fail(QString("Cannot read texture %1: %2").arg(QDir::toNativeSeparators(path), error));
    Loaded l;
    l.w = image.width();
    l.h = image.height();
    QImage::Format f = image.format();
    l.bits = (f == QImage::Format_Grayscale16 || f == QImage::Format_RGBA64 || f == QImage::Format_RGBX64 ||
              f == QImage::Format_RGBA64_Premultiplied || image.depth() >= 48)
                 ? 16
                 : 8;
    bool gray = f == QImage::Format_Grayscale8 || f == QImage::Format_Grayscale16;
    bool alpha = image.hasAlphaChannel();
    if (!gray && alpha && image.depth() <= 32 && image.allGray())
        l.channels = 2;
    else
        l.channels = gray ? 1 : alpha ? 4 : 3;
    image = image.convertToFormat(QImage::Format_RGBA64);
    l.rgba.resize((size_t)l.w * l.h * 4);
    const float *lut = srgbTable();
    parallelFor(l.h, [&](int y) {
        const uint16_t *in = (const uint16_t *)image.constScanLine(y);
        uint16_t *out = l.rgba.data() + (size_t)y * l.w * 4;
        for (int x = 0; x < l.w * 4; x += 4) {
            if (kind == KindColor) {
                float a = in[x + 3] / 65535.f;
                for (int c = 0; c < 3; c++)
                    out[x + c] = unorm16(lut[in[x + c]] * a);
                out[x + 3] = in[x + 3];
            } else
                std::copy(in + x, in + x + 4, out + x);
        }
    });
    return l;
}
static float pointTriangleDistance(V2 p, const V2 *t) {
    float a = orient(t[0], t[1], p), b = orient(t[1], t[2], p), c = orient(t[2], t[0], p);
    if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
        return 0;
    float best = 1e30f;
    for (int k = 0; k < 3; k++) {
        V2 s = t[k], e = t[(k + 1) % 3], d = sub(e, s), q = sub(p, s);
        float l2 = length2(d), h = l2 > 0 ? std::clamp((q.x * d.x + q.y * d.y) / l2, 0.f, 1.f) : 0;
        best = std::min(best, length2(sub(q, d * h)));
    }
    return std::sqrt(best);
}
// Source island ownership at the texture's resolution: texels covered by (or within half a texel of) a face.
static std::vector<int> sourceOwners(const std::vector<XferSrcTri> &src, int set, int w, int h) {
    std::vector<int> owners((size_t)w * h, -1);
    for (const XferSrcTri &t : src) {
        if (t.set != set || !validUV(t.uv))
            continue;
        V2 p[3];
        for (int k = 0; k < 3; k++)
            p[k] = {t.uv[k].x * w - .5f, (1 - t.uv[k].y) * h - .5f};
        float lx = std::min({p[0].x, p[1].x, p[2].x}) - .5f, hx = std::max({p[0].x, p[1].x, p[2].x}) + .5f;
        float ly = std::min({p[0].y, p[1].y, p[2].y}) - .5f, hy = std::max({p[0].y, p[1].y, p[2].y}) + .5f;
        if (hx - lx > 4.f * w || hy - ly > 4.f * h)
            continue;
        for (int y = (int)std::ceil(ly); y <= (int)std::floor(hy); y++)
            for (int x = (int)std::ceil(lx); x <= (int)std::floor(hx); x++) {
                if (pointTriangleDistance({(float)x, (float)y}, p) > .5f)
                    continue;
                int &o = owners[(size_t)wrapIndex(y, h) * w + wrapIndex(x, w)];
                o = o == -1 ? t.island : o == t.island ? o : OwnerShared;
            }
    }
    return owners;
}
// Grows owners (and optionally texels) outward so mips and wide filters never read stale padding.
static void dilate(std::vector<int> &owners, std::vector<uint16_t> *rgba, int w, int h) {
    std::vector<int> frontier;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (owners[(size_t)y * w + x] != -1)
                frontier.push_back(y * w + x);
    if (frontier.empty())
        return;
    std::vector<char> done(owners.size());
    for (int i : frontier)
        done[i] = 1;
    while (!frontier.empty()) {
        std::vector<int> next;
        for (int i : frontier) {
            int x = i % w, y = i / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    int j = ny * w + nx;
                    if (done[j])
                        continue;
                    done[j] = 2;
                    next.push_back(j);
                }
        }
        for (int j : next) {
            int x = j % w, y = j / w, owner = 0x7fffffff;
            float acc[4] = {}, n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || done[ny * w + nx] != 1)
                        continue;
                    int k = ny * w + nx;
                    owner = std::min(owner, owners[k] == OwnerShared ? 0x7ffffffe : owners[k]);
                    if (rgba) {
                        for (int c = 0; c < 4; c++)
                            acc[c] += (*rgba)[(size_t)k * 4 + c];
                        n++;
                    }
                }
            owners[j] = owner == 0x7ffffffe ? OwnerShared : owner;
            if (rgba && n > 0)
                for (int c = 0; c < 4; c++)
                    (*rgba)[(size_t)j * 4 + c] = (uint16_t)std::lround(acc[c] / n);
        }
        for (int j : next)
            done[j] = 1;
        frontier.swap(next);
    }
}
static std::vector<std::vector<uint16_t>> buildMips(std::vector<uint16_t> base, int w, int h,
                                                    std::vector<std::pair<int, int>> &sizes) {
    std::vector<std::vector<uint16_t>> levels;
    sizes = {{w, h}};
    levels.push_back(std::move(base));
    while (w > 1 || h > 1) {
        int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::vector<uint16_t> &in = levels.back();
        std::vector<uint16_t> out((size_t)nw * nh * 4);
        int fx = w / nw, fy = h / nh;
        parallelFor(nh, [&](int y) {
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    uint32_t sum = 0;
                    for (int j = 0; j < fy; j++)
                        for (int i = 0; i < fx; i++)
                            sum += in[((size_t)(y * fy + j) * w + x * fx + i) * 4 + c];
                    out[((size_t)y * nw + x) * 4 + c] = (uint16_t)((sum + fx * fy / 2) / (fx * fy));
                }
        });
        levels.push_back(std::move(out));
        w = nw;
        h = nh;
        sizes.push_back({w, h});
    }
    return levels;
}
struct TexturePass {
    std::vector<std::vector<uint16_t>> data;
    std::vector<std::vector<int>> owners;
    std::vector<TexLevel> levels;
    std::vector<TexSet> sets;
    int bits = 8, channels = 1;
};
static TexturePass loadPass(const TransferJob &job, const Geometry &g, const QList<int> &used, const QString &suffix,
                            const SuffixRule &rule) {
    int kind = kindOf(rule.type);
    int nSets = job.sets.size();
    TexturePass p;
    p.sets.resize(nSets + 1);
    V4 fill = fileToFilter(fillValue(rule), kind);
    for (TexSet &s : p.sets)
        s = {0, 0, nullptr, 0, fill};
    std::vector<Loaded> loaded(nSets);
    std::vector<int> list;
    for (int i : used)
        if (job.sets[i].files.contains(suffix))
            list.push_back(i);
    std::vector<std::vector<int>> owners(nSets);
    parallelFor((int)list.size(), [&](int k) {
        int i = list[k];
        loaded[i] = loadTexture(job.sets[i].files[suffix], kind);
        owners[i] = sourceOwners(g.src, i, loaded[i].w, loaded[i].h);
        dilate(owners[i], job.settings.rebuildSourcePadding ? &loaded[i].rgba : nullptr, loaded[i].w, loaded[i].h);
    });
    p.bits = 8;
    p.channels = kind == KindNormal ? 3 : 1;
    for (int i : list) {
        Loaded &l = loaded[i];
        p.bits = std::max(p.bits, l.bits);
        p.channels = std::max(p.channels, l.channels);
        std::vector<std::pair<int, int>> sizes;
        auto levels = buildMips(std::move(l.rgba), l.w, l.h, sizes);
        p.sets[i].firstLevel = (int)p.data.size();
        p.sets[i].levels = (int)levels.size();
        p.sets[i].present = 1;
        for (size_t k = 0; k < levels.size(); k++) {
            p.data.push_back(std::move(levels[k]));
            p.levels.push_back({nullptr, sizes[k].first, sizes[k].second});
        }
        p.owners.push_back(std::move(owners[i]));
    }
    // Resolve host pointers after all vectors stop growing.
    for (size_t k = 0; k < p.levels.size(); k++)
        p.levels[k].data = p.data[k].data();
    int o = 0;
    for (int i : list)
        p.sets[i].owners = p.owners[o++].data();
    if (job.settings.bits)
        p.bits = job.settings.bits;
    return p;
}

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------
static bool rayTriangle(V3 o, V3 d, const V3 *p, float tmax, float &t, float &u, float &v) {
    V3 e1 = p[1] - p[0], e2 = p[2] - p[0], q = cross(d, e2);
    float det = dot(e1, q);
    if (std::abs(det) < 1e-20f)
        return false;
    float inv = 1 / det;
    V3 s = o - p[0];
    u = dot(s, q) * inv;
    if (u < 0 || u > 1)
        return false;
    V3 r = cross(s, e1);
    v = dot(d, r) * inv;
    if (v < 0 || u + v > 1)
        return false;
    t = dot(e2, r) * inv;
    return t >= 0 && t <= tmax;
}
struct CpuTracer {
    const XferParams *P;
    int count;
    bool operator()(V3 o, V3 n, int object, int &src, float &su, float &sv) const {
        float best = 1e30f;
        src = -1;
        for (int dir = 0; dir < 2; dir++) {
            V3 origin = dir ? o - n * P->eps : o + n * P->eps, d = dir ? n : n * -1.f;
            float tmax = (dir ? P->front : P->back) + P->eps;
            for (int i = 0; i < count; i++) {
                const XferSrcTri &s = P->src[i];
                if (!P->allowed[(size_t)object * P->srcObjects + s.object])
                    continue;
                float t, u, v;
                if (rayTriangle(origin, d, s.p, tmax, t, u, v) && std::abs(t - P->eps) < best) {
                    best = std::abs(t - P->eps);
                    src = i;
                    su = u;
                    sv = v;
                }
            }
        }
        return src >= 0;
    }
};
static V4 resolve(const XAccum &a, int kind) {
    V4 v = a.v * (1 / a.weight);
    if (kind == KindColor) {
        float al = clamp01(v.w);
        if (al <= 1e-6f)
            return {0, 0, 0, 0};
        return {linearToSrgb(clamp01(v.x / al)), linearToSrgb(clamp01(v.y / al)), linearToSrgb(clamp01(v.z / al)), al};
    }
    if (kind == KindNormal) {
        V3 n = unit({v.x, v.y, v.z});
        return {n.x * .5f + .5f, n.y * .5f + .5f, n.z * .5f + .5f, clamp01(v.w)};
    }
    return {clamp01(v.x), clamp01(v.y), clamp01(v.z), clamp01(v.w)};
}
// Ring-by-ring dilation in file space. A ring texel copies the lowest island among its finished neighbours.
static void padOutput(std::vector<V4> &px, std::vector<int> &owners, int res, int padding) {
    std::vector<int> ring(owners.size(), -1), frontier;
    for (size_t i = 0; i < owners.size(); i++)
        if (owners[i] >= 0) {
            ring[i] = 0;
            frontier.push_back((int)i);
        }
    for (int r = 1; !frontier.empty() && (padding < 0 || r <= padding); r++) {
        std::vector<int> next;
        for (int i : frontier) {
            int x = i % res, y = i / res;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= res || ny >= res || ring[ny * res + nx] != -1)
                        continue;
                    ring[ny * res + nx] = -2;
                    next.push_back(ny * res + nx);
                }
        }
        for (int j : next) {
            int x = j % res, y = j / res, owner = 0x7fffffff;
            for (int pass = 0; pass < 2; pass++) {
                V4 acc;
                float n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                            continue;
                        int k = ny * res + nx;
                        if (ring[k] < 0 || ring[k] >= r)
                            continue;
                        if (!pass)
                            owner = std::min(owner, owners[k]);
                        else if (owners[k] == owner) {
                            // Orthogonal neighbours weigh more than diagonals for smoother fronts.
                            float w = dx && dy ? .7071f : 1.f;
                            acc = acc + px[k] * w;
                            n += w;
                        }
                    }
                if (pass && n > 0)
                    px[j] = acc * (1 / n);
            }
            owners[j] = owner;
        }
        for (int j : next)
            ring[j] = r;
        frontier.swap(next);
    }
}
static QImage previewImage(const std::vector<V4> &px, int res, int channels) {
    int step = std::max(1, (res + 2047) / 2048), size = res / step;
    QImage image(size, size, QImage::Format_RGBA8888);
    for (int y = 0; y < size; y++) {
        uchar *line = image.scanLine(y);
        for (int x = 0; x < size; x++) {
            V4 acc;
            for (int j = 0; j < step; j++)
                for (int i = 0; i < step; i++)
                    acc = acc + px[(size_t)(y * step + j) * res + x * step + i];
            acc = acc * (1.f / (step * step));
            float r = acc.x, g = channels < 3 ? acc.x : acc.y, b = channels < 3 ? acc.x : acc.z;
            float a = channels == 2 || channels == 4 ? acc.w : 1;
            line[x * 4] = (uchar)std::lround(clamp01(r) * 255);
            line[x * 4 + 1] = (uchar)std::lround(clamp01(g) * 255);
            line[x * 4 + 2] = (uchar)std::lround(clamp01(b) * 255);
            line[x * 4 + 3] = (uchar)std::lround(clamp01(a) * 255);
        }
    }
    return image;
}
static void writeOutput(const QString &path, const std::vector<V4> &px, int res, int channels, int bits, bool srgb) {
    std::vector<uint16_t> q((size_t)res * res * channels);
    float scale = bits == 16 ? 65535.f : 255.f;
    parallelFor(res, [&](int y) {
        for (int x = 0; x < res; x++) {
            const V4 &v = px[(size_t)y * res + x];
            float c[4] = {v.x, v.y, v.z, v.w};
            int map[4] = {0, 1, 2, 3};
            if (channels == 2)
                map[1] = 3;
            uint16_t *o = q.data() + ((size_t)y * res + x) * channels;
            for (int k = 0; k < channels; k++)
                o[k] = (uint16_t)std::lround(clamp01(c[map[k]]) * scale);
        }
    });
    writePng(path, res, res, channels, bits, q.data(), srgb ? PngSpace::Srgb : PngSpace::Untagged);
}
static int autoResolution(const TransferJob &job, const QList<int> &used) {
    int r = 0;
    for (int i : used)
        for (const QString &f : job.sets[i].files) {
            QSize s = imageSize(f);
            if (s.isValid())
                r = std::max({r, s.width(), s.height()});
        }
    return r ? r : 2048;
}
TransferReport runTransfer(const TransferJob &job, TransferBackend backend, std::atomic_bool &cancel,
                           Progress progress) {
    QElapsedTimer timer;
    timer.start();
    TransferReport report;
    QStringList errors = checkJob(job, &report.warnings);
    if (!errors.isEmpty())
        fail(errors.join("\n"));
    if (!job.settings.overwrite) {
        QStringList existing = existingOutputs(job);
        if (!existing.isEmpty())
            fail("Output files already exist (enable overwrite):\n" + existing.join("\n"));
    }
    if (!QDir().mkpath(job.outputDir))
        fail("Cannot create the output folder " + QDir::toNativeSeparators(job.outputDir));
    auto report01 = [&](float f, const QString &s) {
        if (progress)
            progress(f, s);
    };
    const Scene &S = *job.source, &T = *job.target;
    const TransferSettings &st = job.settings;
    float scale = std::max(S.scale, T.scale);
    float tol = st.tolerance > 0 ? st.tolerance : scale * 1e-5f;
    float front = st.front > 0 ? st.front : scale * .01f, back = st.back > 0 ? st.back : scale * .01f;
    report01(0, "Preparing geometry");
    Geometry g = prepareGeometry(job, tol);
    QList<int> used = usedSets(job);
    QStringList suffixes = usedSuffixes(job, used);
    QList<OutputGroup> groups = outputGroups(job, nullptr);
    std::vector<std::vector<int>> groupTris(groups.size());
    std::vector<int> groupRes(groups.size());
    int autoRes = autoResolution(job, used);
    for (int k = 0; k < groups.size(); k++) {
        for (size_t i = 0; i < T.tris.size(); i++) {
            int m = T.tris[i].material;
            QString name = m >= 0 && m < T.materials.size() ? T.materials[m] : "";
            if (g.valid[i] && groups[k].materials.contains(name))
                groupTris[k].push_back((int)i);
        }
        groupRes[k] = groups[k].resolution ? groups[k].resolution : autoRes;
        report.targetTriangles += (int)groupTris[k].size();
        int outside = 0;
        for (int i : groupTris[k])
            for (const V2 &uv : g.tris[i].uv)
                if (uv.x < -1e-4f || uv.y < -1e-4f || uv.x > 1.0001f || uv.y > 1.0001f) {
                    outside++;
                    break;
                }
        if (outside)
            report.warnings << QString("%1: %2 target faces extend outside the 0-1 UV range and are clipped.")
                                   .arg(groups[k].prefix)
                                   .arg(outside);
        if (groupTris[k].empty())
            report.warnings << QString("%1: the selected target materials have no valid UV faces.").arg(groups[k].prefix);
    }
    for (int k = 0; k < groups.size(); k++)
        for (int i : groupTris[k])
            report.exactTriangles += g.tris[i].match >= 0;
    int invalid = (int)std::count(g.valid.begin(), g.valid.end(), 0);
    if (invalid)
        report.warnings << QString("%1 target faces have degenerate UVs and are skipped.").arg(invalid);
    std::unique_ptr<TransferGpu> gpu;
    if (backend == TransferBackend::Gpu) {
        report01(.02f, "Building source acceleration structure");
        gpu = std::make_unique<TransferGpu>(g.src, g.allowed, g.tris);
        report.device = gpu->device();
    } else
        report.device = QString("CPU reference (%1 threads)").arg(std::thread::hardware_concurrency());
    const int tile = 512 * 512;
    double totalWork = 0, doneWork = 0;
    for (int k = 0; k < groups.size(); k++)
        totalWork += (double)groupRes[k] * groupRes[k] * suffixes.size();
    std::vector<int> offsets, indices;
    for (int sx = 0; sx < suffixes.size(); sx++) {
        const SuffixRule &rule = job.suffixes[suffixIndex(job.suffixes, suffixes[sx])];
        int kind = kindOf(rule.type);
        report01(float(doneWork / std::max(1.0, totalWork)), "Loading " + rule.suffix + " textures");
        TexturePass pass = loadPass(job, g, used, rule.suffix, rule);
        if (cancel)
            fail("Cancelled.");
        if (gpu)
            gpu->setTextures(pass.levels, pass.sets);
        V4 fileFill = fillValue(rule);
        for (int k = 0; k < groups.size(); k++) {
            int res = groupRes[k];
            buildGrid(g.tris, groupTris[k], 64, offsets, indices);
            if (gpu)
                gpu->setGrid(offsets, indices);
            XferParams P{};
            P.tris = g.tris.data();
            P.src = g.src.data();
            P.offsets = offsets.data();
            P.indices = indices.data();
            P.allowed = g.allowed.data();
            P.sets = pass.sets.data();
            P.levels = pass.levels.data();
            P.grid = 64;
            P.srcObjects = g.srcObjects;
            P.resolution = res;
            P.ss = st.ss;
            P.kind = kind;
            P.filter = st.filter;
            P.islandAware = st.islandAware;
            P.averageOverlaps = st.averageOverlaps;
            P.projection = 1;
            P.srcGreenDX = st.sourceNormalDX;
            P.dstGreenDX = st.targetNormalDX;
            P.front = front;
            P.back = back;
            P.eps = scale * 1e-6f;
            size_t texels = (size_t)res * res;
            std::vector<V4> px(texels);
            std::vector<int> owners(texels, -1);
            std::vector<XAccum> acc(tile);
            std::vector<uint8_t> coverage(sx == 0 && st.coverageMap ? texels * 3 : 0);
            for (size_t start = 0; start < texels; start += tile) {
                if (cancel)
                    fail("Cancelled.");
                int count = (int)std::min<size_t>(tile, texels - start);
                P.texelStart = (int)start;
                P.texelCount = count;
                if (gpu)
                    gpu->launch(P, acc.data());
                else {
                    CpuTracer tracer{&P, (int)g.src.size()};
                    parallelFor((count + 255) / 256, [&](int b) {
                        for (int i = b * 256; i < std::min(count, b * 256 + 256); i++)
                            shadeTexel(P, (int)start + i, tracer, acc[i]);
                    });
                }
                for (int i = 0; i < count; i++) {
                    const XAccum &a = acc[i];
                    size_t t = start + i;
                    if (a.weight > 0) {
                        px[t] = resolve(a, kind);
                        owners[t] = a.owner;
                    }
                    if (sx == 0) {
                        report.samples += a.weight + a.miss;
                        report.projectedSamples += a.projected;
                        report.coveredTexels += a.weight > 0;
                        report.missedTexels += a.weight <= 0 && a.miss > 0;
                    }
                    if (!coverage.empty()) {
                        float n = a.weight + a.miss;
                        uint8_t *c = coverage.data() + t * 3;
                        if (n > 0) {
                            float miss = a.miss / n, proj = a.projected / n;
                            c[0] = (uint8_t)std::lround(255 * clamp01(miss + proj));
                            c[1] = (uint8_t)std::lround(255 * clamp01(1 - miss));
                            c[2] = 0;
                        }
                    }
                }
                doneWork += count;
                report01(float(doneWork / std::max(1.0, totalWork)),
                         QString("%1_%2  %3%").arg(groups[k].prefix, rule.suffix).arg(int(100.0 * (start + count) / texels)));
            }
            padOutput(px, owners, res, st.padding);
            for (size_t t = 0; t < texels; t++)
                if (owners[t] < 0)
                    px[t] = fileFill;
            int channels = pass.channels;
            if (kind == KindNormal)
                channels = std::max(channels, 3);
            QString path = outputPath(job, groups[k].prefix, rule.suffix);
            writeOutput(path, px, res, channels, pass.bits, kind == KindColor);
            report.maps.push_back({groups[k].prefix, rule.suffix, path, res, pass.bits, channels,
                                   previewImage(px, res, channels)});
            if (!coverage.empty()) {
                std::vector<uint16_t> q(coverage.begin(), coverage.end());
                writePng(outputPath(job, groups[k].prefix, "COVERAGE"), res, res, 3, 8, q.data(), PngSpace::Untagged);
            }
        }
    }
    if (gpu)
        report.gpuBytes = gpu->peakBytes();
    if (report.samples > 0 && report.missedTexels > 0)
        report.warnings << QString("%1 texels found no source surface; increase the projection distance or check "
                                   "the object pairing.")
                               .arg((qint64)report.missedTexels);
    if (report.targetTriangles && report.exactTriangles < report.targetTriangles)
        report.warnings << QString("%1 of %2 target faces matched the source topology exactly; the rest are projected.")
                               .arg(report.exactTriangles)
                               .arg(report.targetTriangles);
    report.seconds = timer.elapsed() / 1000.0;
    report01(1, "Done");
    return report;
}
