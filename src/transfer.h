#pragma once
#include "core.h"
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <memory>
// Texture transfer between two copies of a mesh with different UV layouts.
enum class MapType { Color, Linear, Normal, Packed };
struct SuffixRule {
    QString suffix;
    MapType type = MapType::Linear;
    QString fill = "#808080"; // file-space default for texture sets that lack this map
};
struct SourceSet {
    QString name;
    QMap<QString, QString> files; // canonical suffix -> image path
};
struct OutputRule {
    QString material; // target material
    bool enabled = true;
    QString prefix;
    int resolution = 0; // 0 = largest source texture
};
struct TransferSettings {
    int sourceUV = 0, targetUV = 0, ss = 3, filter = 0, padding = 16, bits = 0;
    int mode = 0; // 0 = exact topology match with projection fallback, 1 = projection only
    bool islandAware = true, rebuildSourcePadding = true, averageOverlaps = true, pairing = true;
    bool sourceNormalDX = false, targetNormalDX = false, coverageMap = false, overwrite = false;
    float front = 0, back = 0, tolerance = 0; // model units, 0 = automatic
};
struct TransferJob {
    QString sourceMesh, targetMesh, outputDir;
    std::shared_ptr<const Scene> source, target;
    QList<SuffixRule> suffixes;
    QList<SourceSet> sets;
    QMap<QString, QString> materialSets; // source material -> set name, empty = unassigned
    QMap<QString, QString> pairs;        // target object -> source object, "*" = any, empty = excluded
    QList<OutputRule> outputs;
    TransferSettings settings;
    QJsonObject toJson() const;
    // Paths in the JSON are resolved relative to baseDir. Meshes are loaded.
    static TransferJob fromJson(const QJsonObject &, const QString &baseDir);
};
struct TransferMap {
    QString prefix, suffix, path;
    int resolution = 0, bits = 8, channels = 4;
    QImage preview;
};
struct TransferReport {
    QStringList warnings;
    QList<TransferMap> maps;
    int targetTriangles = 0, exactTriangles = 0;
    double coveredTexels = 0, missedTexels = 0, projectedSamples = 0, samples = 0;
    double seconds = 0;
    size_t gpuBytes = 0;
    QString device;
};
enum class TransferBackend { Gpu, Cpu };
QList<SuffixRule> defaultSuffixes();
QString mapTypeName(MapType);
MapType mapTypeFromName(const QString &);
// Split "doll01_ALB.png" into set "doll01" and a known suffix. Unknown files are returned separately.
QList<SourceSet> discoverSets(const QStringList &files, const QList<SuffixRule> &, QStringList *unknown = nullptr);
QList<SourceSet> mergeSets(QList<SourceSet> existing, const QList<SourceSet> &added);
QMap<QString, QString> guessMaterialSets(const QStringList &materials, const QStringList &sets);
QMap<QString, QString> guessPairs(const QStringList &targetObjects, const QStringList &sourceObjects);
QString suggestPrefix(const QStringList &setNames);
QList<OutputRule> defaultOutputs(const QStringList &targetMaterials, const QStringList &setNames);
// Errors block the transfer; warnings are informational.
QStringList checkJob(const TransferJob &, QStringList *warnings = nullptr);
QStringList existingOutputs(const TransferJob &);
// Reads PNG/TIFF/JPEG via Qt and TGA via stb. Returns a null image on failure.
QImage readTextureImage(const QString &path, QString *error = nullptr);
// MikkTSpace tangents per triangle corner; bitangent = sign * cross(normal, tangent).
void meshTangents(const Scene &, int uv, std::vector<V3> &tangents, std::vector<float> &signs);
TransferReport runTransfer(const TransferJob &, TransferBackend, std::atomic_bool &cancel, Progress = {});
