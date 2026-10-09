#pragma once
#include <QByteArray>
#include <cstdint>
#include <string>
#include <vector>
// Lossless binary FBX node tree: untouched property records are kept as raw bytes and
// written back verbatim, so only explicitly edited nodes change.
namespace fbx {
struct Node {
    std::string name;
    uint64_t propertyCount = 0;
    QByteArray properties;
    std::vector<Node> children;
    bool terminated = false;
    Node *child(const char *name);
    const Node *child(const char *name) const;
};
struct Document {
    QByteArray header;
    uint32_t version = 7400;
    std::vector<Node> nodes;
    QByteArray footerId;
    int versionAlignment = 4;
    Node *node(const char *name);
};
bool isBinary(const QByteArray &bytes);
Document parse(const QByteArray &bytes);
QByteArray serialize(const Document &);

char propertyType(const Node &, int index);
int64_t integer(const Node &, int index);
QByteArray string(const Node &, int index);
std::vector<int32_t> intArray(const Node &, int index);

class Properties {
  public:
    Properties &integer(int32_t);
    Properties &longInteger(int64_t);
    Properties &string(const QByteArray &);
    Properties &doubles(const std::vector<double> &);
    Properties &ints(const std::vector<int32_t> &);
    Node node(const char *name) const;

  private:
    QByteArray data_;
    uint64_t count_ = 0;
};
} // namespace fbx
