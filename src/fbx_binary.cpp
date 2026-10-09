#include "fbx_binary.h"
#include <QtEndian>
#include <cstring>
#include <stdexcept>
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN, "FBX arrays are copied as little-endian memory");
namespace fbx {
namespace {
constexpr char kMagic[] = "Kaydara FBX Binary  \0\x1a\0";
constexpr int kMagicSize = 23, kHeaderSize = 27, kMaxDepth = 128;
constexpr unsigned char kFooterMagic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e,
                                            0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
constexpr unsigned char kDefaultFooterId[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66,
                                                0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
[[noreturn]] void fail(const char *message) {
    throw std::runtime_error(std::string("FBX: ") + message);
}
int arrayElementSize(char type) {
    switch (type) {
    case 'b':
        return 1;
    case 'i':
    case 'f':
        return 4;
    case 'l':
    case 'd':
        return 8;
    }
    return 0;
}
template <class T> T read(const QByteArray &b, qsizetype at) {
    if (at < 0 || at + (qsizetype)sizeof(T) > b.size())
        fail("unexpected end of data");
    return qFromLittleEndian<T>(b.constData() + at);
}
// Returns the byte offset after the property starting at `at`, validating its bounds.
qsizetype skipProperty(const QByteArray &b, qsizetype at) {
    if (at >= b.size())
        fail("truncated property list");
    char type = b[at++];
    switch (type) {
    case 'C':
        return at + 1;
    case 'Y':
        return at + 2;
    case 'I':
    case 'F':
        return at + 4;
    case 'L':
    case 'D':
        return at + 8;
    case 'S':
    case 'R':
        return at + 4 + read<uint32_t>(b, at);
    default:
        if (!arrayElementSize(type))
            fail("unknown property type");
        return at + 12 + read<uint32_t>(b, at + 8);
    }
}
qsizetype propertyOffset(const Node &n, int index) {
    if (index < 0 || (uint64_t)index >= n.propertyCount)
        fail("missing property");
    qsizetype at = 0;
    for (int i = 0; i < index; ++i)
        at = skipProperty(n.properties, at);
    return at;
}
struct Parser {
    const QByteArray &b;
    bool wide;
    int headerSize() const { return wide ? 25 : 13; }
    uint64_t offset(qsizetype at) const { return wide ? read<uint64_t>(b, at) : read<uint32_t>(b, at); }
    bool isNullRecord(qsizetype at) const {
        if (at + headerSize() > b.size())
            return false;
        for (int i = 0; i < headerSize(); ++i)
            if (b[at + i])
                return false;
        return true;
    }
    Node node(qsizetype &at, qsizetype limit, int depth) const {
        if (depth > kMaxDepth)
            fail("node nesting is too deep");
        const int w = wide ? 8 : 4;
        if (at + headerSize() > limit)
            fail("truncated node header");
        uint64_t end = offset(at), count = offset(at + w), listSize = offset(at + 2 * w);
        int nameSize = (unsigned char)b[at + 3 * w];
        qsizetype props = at + headerSize() + nameSize;
        if (end > (uint64_t)limit || props > limit || listSize > (uint64_t)(limit - props) ||
            end < (uint64_t)(props + listSize))
            fail("corrupt node offsets");
        Node n;
        n.name.assign(b.constData() + at + headerSize(), nameSize);
        n.propertyCount = count;
        n.properties = b.mid(props, (qsizetype)listSize);
        qsizetype p = 0;
        for (uint64_t i = 0; i < count; ++i)
            p = skipProperty(n.properties, p);
        if (p != n.properties.size())
            fail("property list size mismatch");
        at = props + (qsizetype)listSize;
        while (at < (qsizetype)end) {
            if (isNullRecord(at)) {
                at += headerSize();
                if (at != (qsizetype)end)
                    fail("data after a child list terminator");
                n.terminated = true;
                break;
            }
            n.children.push_back(node(at, (qsizetype)end, depth + 1));
        }
        if (!n.children.empty() && !n.terminated)
            fail("unterminated child list");
        return n;
    }
};
struct Writer {
    QByteArray out;
    bool wide;
    int headerSize() const { return wide ? 25 : 13; }
    void putOffset(qsizetype at, uint64_t value) {
        if (wide)
            qToLittleEndian<uint64_t>(value, out.data() + at);
        else {
            if (value > UINT32_MAX)
                fail("file exceeds the 4 GiB limit of this FBX version");
            qToLittleEndian<uint32_t>((uint32_t)value, out.data() + at);
        }
    }
    void node(const Node &n) {
        if (n.name.size() > 255)
            fail("node name too long");
        const int w = wide ? 8 : 4;
        qsizetype start = out.size();
        out.append(headerSize() - 1, '\0');
        out.append((char)n.name.size());
        out.append(n.name.data(), (qsizetype)n.name.size());
        out.append(n.properties);
        putOffset(start + w, n.propertyCount);
        putOffset(start + 2 * w, (uint64_t)n.properties.size());
        for (const auto &c : n.children)
            node(c);
        if (n.terminated || !n.children.empty())
            out.append(headerSize(), '\0');
        putOffset(start, (uint64_t)out.size());
    }
};
} // namespace

Node *Node::child(const char *n) {
    for (auto &c : children)
        if (c.name == n)
            return &c;
    return nullptr;
}
const Node *Node::child(const char *n) const {
    return const_cast<Node *>(this)->child(n);
}
Node *Document::node(const char *n) {
    for (auto &c : nodes)
        if (c.name == n)
            return &c;
    return nullptr;
}
bool isBinary(const QByteArray &bytes) {
    return bytes.size() >= kHeaderSize && !memcmp(bytes.constData(), kMagic, kMagicSize);
}
Document parse(const QByteArray &bytes) {
    if (!isBinary(bytes))
        fail("not a binary FBX file");
    Document d;
    d.header = bytes.left(kHeaderSize);
    d.version = read<uint32_t>(bytes, kMagicSize);
    Parser p{bytes, d.version >= 7500};
    qsizetype at = kHeaderSize;
    bool ended = false;
    while (at < bytes.size()) {
        if (p.isNullRecord(at)) {
            at += p.headerSize();
            ended = true;
            break;
        }
        d.nodes.push_back(p.node(at, bytes.size(), 0));
    }
    if (ended && at + 16 <= bytes.size())
        d.footerId = bytes.mid(at, 16);
    qsizetype versionAt = bytes.size() - 140;
    if (ended && versionAt >= at + 20 &&
        !memcmp(bytes.constData() + bytes.size() - 16, kFooterMagic, sizeof(kFooterMagic)))
        d.versionAlignment = int(versionAt % 16);
    return d;
}
QByteArray serialize(const Document &d) {
    Writer w{{}, d.version >= 7500};
    w.out = d.header;
    qToLittleEndian<uint32_t>(d.version, w.out.data() + kMagicSize);
    for (const auto &n : d.nodes)
        w.node(n);
    w.out.append(w.headerSize(), '\0');
    w.out.append(d.footerId.size() == 16 ? d.footerId
                                         : QByteArray((const char *)kDefaultFooterId, sizeof(kDefaultFooterId)));
    w.out.append(4, '\0');
    w.out.append(int(((d.versionAlignment - w.out.size()) % 16 + 16) % 16), '\0');
    char version[4];
    qToLittleEndian<uint32_t>(d.version, version);
    w.out.append(version, 4);
    w.out.append(120, '\0');
    w.out.append((const char *)kFooterMagic, sizeof(kFooterMagic));
    return w.out;
}
char propertyType(const Node &n, int index) {
    return n.properties[propertyOffset(n, index)];
}
int64_t integer(const Node &n, int index) {
    qsizetype at = propertyOffset(n, index);
    switch (n.properties[at]) {
    case 'C':
        return (unsigned char)n.properties[at + 1];
    case 'Y':
        return read<int16_t>(n.properties, at + 1);
    case 'I':
        return read<int32_t>(n.properties, at + 1);
    case 'L':
        return read<int64_t>(n.properties, at + 1);
    }
    fail("expected an integer property");
}
QByteArray string(const Node &n, int index) {
    qsizetype at = propertyOffset(n, index);
    if (n.properties[at] != 'S' && n.properties[at] != 'R')
        fail("expected a string property");
    return n.properties.mid(at + 5, read<uint32_t>(n.properties, at + 1));
}
std::vector<int32_t> intArray(const Node &n, int index) {
    qsizetype at = propertyOffset(n, index);
    if (n.properties[at] != 'i')
        fail("expected an int32 array");
    uint32_t count = read<uint32_t>(n.properties, at + 1), encoding = read<uint32_t>(n.properties, at + 5),
             stored = read<uint32_t>(n.properties, at + 9);
    if (count > (uint32_t)INT32_MAX / 4)
        fail("array too large");
    QByteArray raw = n.properties.mid(at + 13, stored);
    if (encoding == 1) {
        QByteArray prefixed(4, '\0');
        qToBigEndian<uint32_t>(count * 4, prefixed.data());
        raw = qUncompress(prefixed + raw);
    } else if (encoding != 0)
        fail("unknown array encoding");
    if (raw.size() != qsizetype(count) * 4)
        fail("corrupt array data");
    std::vector<int32_t> values(count);
    if (count)
        memcpy(values.data(), raw.constData(), raw.size());
    return values;
}
static void appendArray(QByteArray &out, char type, const void *data, size_t count) {
    if (count > (size_t)INT32_MAX / 8)
        fail("array too large");
    QByteArray raw((const char *)data, qsizetype(count * arrayElementSize(type)));
    uint32_t encoding = 0;
    // The FBX SDK compresses large arrays; tiny ones are smaller stored raw.
    if (raw.size() >= 128) {
        raw = qCompress(raw, 6).mid(4);
        encoding = 1;
    }
    char header[12];
    qToLittleEndian<uint32_t>((uint32_t)count, header);
    qToLittleEndian<uint32_t>(encoding, header + 4);
    qToLittleEndian<uint32_t>((uint32_t)raw.size(), header + 8);
    out.append(type);
    out.append(header, 12);
    out.append(raw);
}
Properties &Properties::integer(int32_t v) {
    char b[4];
    qToLittleEndian(v, b);
    data_.append('I').append(b, 4);
    ++count_;
    return *this;
}
Properties &Properties::longInteger(int64_t v) {
    char b[8];
    qToLittleEndian(v, b);
    data_.append('L').append(b, 8);
    ++count_;
    return *this;
}
Properties &Properties::string(const QByteArray &s) {
    char b[4];
    qToLittleEndian<uint32_t>((uint32_t)s.size(), b);
    data_.append('S').append(b, 4).append(s);
    ++count_;
    return *this;
}
Properties &Properties::doubles(const std::vector<double> &v) {
    appendArray(data_, 'd', v.data(), v.size());
    ++count_;
    return *this;
}
Properties &Properties::ints(const std::vector<int32_t> &v) {
    appendArray(data_, 'i', v.data(), v.size());
    ++count_;
    return *this;
}
Node Properties::node(const char *name) const {
    Node n;
    n.name = name;
    n.propertyCount = count_;
    n.properties = data_;
    return n;
}
} // namespace fbx
