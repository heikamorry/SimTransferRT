#pragma once
#include "sources/AuroraCodec.h"
#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rt::model {
// Numeric payloads are little-endian IEEE/two's complement. Ragged ends do not include a leading
// zero.
enum class Type { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64 };
QString typeName(Type);
Type typeOf(QString);
int width(Type);
bool integer(Type);
constexpr qint64 budget = 2LL * 1024 * 1024 * 1024;
constexpr qint64 maxTuples = 64LL * 1024 * 1024;
[[noreturn]] void fail(const QString &);
qint64 checked(qint64 a, qint64 b, qint64 limit = budget);
struct Cancelled : std::exception {
    const char *what() const noexcept override {
        return "任务已取消";
    }
};
struct Task {
    std::function<bool()> stopped;
    std::function<void(int, const QString &)> progress;
    std::function<void()> enterCommit;
    void check() const;
    void report(int, const QString &) const;
};
struct Field {
    QString section, name, role;
    Type type = Type::U8;
    int components = 1, piece = 0;
    bool list = false;
    QByteArray data;
    std::vector<qint64> ends;
    QJsonObject declaration;
    qint64 size() const {
        return data.size() / (width(type) * qint64(components));
    }
    qint64 tuples() const {
        return list ? qint64(ends.size()) : size();
    }
};
struct Dataset {
    QString path, sourceHash, loadedFormat;
    QJsonObject previousJob;
    QJsonObject descriptor;
    std::vector<Field> fields; // Stable ID is position in the ordered schema.
    QByteArray bx;
    std::optional<aurora::Reply> tracking;
    int pieces() const {
        return descriptor.value("pieces").toInt(1);
    }
    const Field *find(QString section, int piece = 0) const;
    qint64 points(int piece) const;
    qint64 cells(int piece) const;
};
using Snapshot = std::shared_ptr<const Dataset>;
bool topology(const QString &);
QStringList cellKinds();
QJsonObject manifest(const Dataset &);
QByteArray json(const QJsonObject &);
QJsonObject object(const QByteArray &);
Field description(const QJsonObject &);
void applyManifest(Dataset &, const QJsonObject &, const std::vector<int> &wireToLogical);
void validate(const Dataset &, const Task &);
QString digest(const QByteArray &);
QString fileHash(const QString &, const Task &);
QString fieldHash(const Field &, const Task &);
void compare(const Dataset &, const Dataset &, const Task &);
void writeAll(QIODevice &, const char *, qint64);
inline void writeAll(QIODevice &out, const QByteArray &b) {
    writeAll(out, b.constData(), b.size());
}
void scalarText(Type, QByteArrayView, char *);
QByteArray scalarString(Type, const char *);
qint64 indexValue(Type, const char *);
void storeIndex(Type, qint64, char *);
Type topologyValueType(const Field &);
Type topologyOffsetType(const Field &);
QByteArray encodeIndices(const QByteArray &canonical, Type target, const Task &);
double displayNumber(Type, const char *);
void endianCopy(char *, const char *, int, bool);
struct Cursor {
    const char *begin, *p, *end;
    explicit Cursor(const QByteArray &b) : begin(b.constData()), p(begin), end(begin + b.size()) {}
    QByteArrayView token();
    QByteArray line();
    const char *take(qint64);
    void spaces();
    qint64 remaining() const {
        return end - p;
    }
    qint64 position() const {
        return p - begin;
    }
};
struct Assessment {
    QStringList preserved, mapping, omitted, impossible, unavailable;
    bool allowed() const {
        return impossible.isEmpty() && unavailable.isEmpty();
    }
    QString summary() const;
};
Assessment assess(const Dataset &, const QString &format);
struct BxWriteStats {
    qint64 points = 0, frames = 0;
    double scaleToMm = 1, maxRoundingMm = 0;
    bool assumedUnits = false;
    QString sourceUnits;
};

Dataset readPly(const QByteArray &, const Task &);
Dataset readVtp(const QByteArray &, const Task &);
Dataset readLegacy(const QByteArray &, const Task &);
Dataset readBx(const QByteArray &, const Task &, const QString &source = "local");
Snapshot loadBxObject(const QByteArray &, const Task &, const QString &label);
void writeBx(QIODevice &, const Dataset &, const Task &, BxWriteStats * = nullptr);
void writePly(QIODevice &, const Dataset &, const Task &);
void writeVtp(QIODevice &, const Dataset &, const Task &);
void writeLegacy(QIODevice &, const Dataset &, const Task &);
QString formatForPath(const QString &);
Snapshot load(const QString &, const Task &, const QString &expected = {});
struct JobResult {
    QString id, status, message, output, archive;
};
JobResult convert(Snapshot, const QString &output, const QString &archiveDir, const Task &);
void archiveWrite(const QString &, const Dataset &, const QJsonObject &record, const Task &);
Dataset archiveRead(const QString &, const Task &, QJsonObject *record = nullptr);
} // namespace rt::model
