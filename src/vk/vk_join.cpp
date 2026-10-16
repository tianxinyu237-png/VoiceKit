#include "vk_join.h"
#include "vk_pack.h"          // 复用 VoicePackBuilder::probeWav

#include <QCollator>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QVector>

#include <algorithm>

namespace vk {

QStringList VoiceJoiner::naturalSort(const QStringList& paths)
{
    QStringList out = paths;
    QCollator col;
    col.setNumericMode(true);        // 关键：让 0009 排在 0010 之前，而不是字符串序
    col.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&col](const QString& a, const QString& b) {
        return col.compare(QFileInfo(a).fileName(), QFileInfo(b).fileName()) < 0;
    });
    return out;
}

bool VoiceJoiner::join(const QStringList& files, const QString& outPath,
                       int gapMs, JoinResult* res, QString* err)
{
    auto fail = [&](const QString& m) { if (err) *err = m; return false; };

    if (files.isEmpty())
        return fail(QStringLiteral("没有要合并的文件"));

    struct Seg { QString path; qint64 off; qint64 len; };
    QVector<Seg> segs;
    int rate = 0, ch = 0, bits = 0;
    qint64 total = 0;

    for (int i = 0; i < files.size(); ++i) {
        int r = 0, c = 0, b = 0, dur = 0;
        qint64 off = 0, len = 0;
        QString e;
        if (!VoicePackBuilder::probeWav(files[i], &r, &c, &b, &off, &len, &dur, &e))
            return fail(QStringLiteral("读不了 %1：%2").arg(QFileInfo(files[i]).fileName(), e));
        if (i == 0) {
            rate = r; ch = c; bits = b;
        } else if (r != rate || c != ch || b != bits) {
            return fail(QStringLiteral("参数不一致：%1 是 %2Hz/%3声道/%4bit，第一个是 %5Hz/%6声道/%7bit。"
                                       "不同参数不能直接拼（重采样会掉音质），先把它们统一成同一档再合。")
                            .arg(QFileInfo(files[i]).fileName())
                            .arg(r).arg(c).arg(b).arg(rate).arg(ch).arg(bits));
        }
        if (len <= 0)
            return fail(QStringLiteral("%1 里没有音频数据").arg(QFileInfo(files[i]).fileName()));
        segs.append(Seg{ files[i], off, len });
        total += len;
    }

    const int    bytesPerSample = bits / 8;
    const qint64 frameBytes     = qint64(ch) * bytesPerSample;
    const qint64 gapBytes       = gapMs > 0 ? (qint64(rate) * gapMs / 1000) * frameBytes : 0;
    if (gapBytes > 0)
        total += gapBytes * (segs.size() - 1);          // 句间静音，末尾不加

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("写不了 %1：%2").arg(outPath, out.errorString()));

    // 自造一个干净的 44 字节头。源文件可能带 fact/LIST 块，
    // 照抄头会让长度字段对不上（播放器会截断或报错）。
    const quint32 dataSize = quint32(total);
    {
        QDataStream ds(&out);
        ds.setByteOrder(QDataStream::LittleEndian);
        out.write("RIFF", 4);
        ds << quint32(36 + dataSize);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        ds << quint32(16);
        ds << quint16(1);                                // PCM
        ds << quint16(ch);
        ds << quint32(rate);
        ds << quint32(rate * frameBytes);                // byteRate
        ds << quint16(frameBytes);                       // blockAlign
        ds << quint16(bits);
        out.write("data", 4);
        ds << dataSize;
    }

    // 静音缓冲（一次分配，循环复用）
    QByteArray silence;
    if (gapBytes > 0)
        silence = QByteArray(int(qMin<qint64>(gapBytes, 1 << 20)), '\0');

    qint64 written = 0;
    QByteArray buf(1 << 16, '\0');
    for (int i = 0; i < segs.size(); ++i) {
        QFile in(segs[i].path);
        if (!in.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("读不了 %1：%2").arg(segs[i].path, in.errorString()));
        in.seek(segs[i].off);
        qint64 left = segs[i].len;
        while (left > 0) {
            const qint64 n = in.read(buf.data(), qMin<qint64>(buf.size(), left));
            if (n <= 0)
                break;
            if (out.write(buf.constData(), n) != n)
                return fail(QStringLiteral("写 %1 时出错：%2").arg(outPath, out.errorString()));
            left -= n;
            written += n;
        }
        in.close();

        if (gapBytes > 0 && i != segs.size() - 1) {
            qint64 lg = gapBytes;
            while (lg > 0) {
                const qint64 n = qMin<qint64>(silence.size(), lg);
                out.write(silence.constData(), n);
                lg -= n;
                written += n;
            }
        }
    }
    out.close();

    if (res) {
        res->fileCount  = segs.size();
        res->dataBytes  = written;
        res->totalBytes = 44 + written;
        res->sampleRate = rate;
        res->channels   = ch;
        res->bits       = bits;
        res->totalMs    = (rate > 0 && frameBytes > 0)
                              ? int(qint64(written) * 1000 / (qint64(rate) * frameBytes)) : 0;
    }
    return true;
}

} // namespace vk
