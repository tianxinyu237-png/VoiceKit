#include "vk_pack.h"
#include "vk_config.h"
#include "vk_script.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

namespace vk {

// ---------------------------------------------------------------------------
// 规格
// ---------------------------------------------------------------------------
PackSpec PackSpec::fromName(const QString& name)
{
    const QString n = name.trimmed().toLower();

    if (n == QLatin1String("pc") || n == QLatin1String("16k")) {
        PackSpec s;
        s.name = QStringLiteral("pc");
        s.sampleRate = 16000;
        s.bits = 16;
        s.adpcm = false;
        return s;
    }
    if (n == QLatin1String("embedded8k16") || n == QLatin1String("8k16")) {
        PackSpec s;
        s.name = QStringLiteral("embedded8k16");
        s.sampleRate = 8000;
        s.bits = 16;
        s.adpcm = false;
        return s;
    }
    if (n == QLatin1String("embedded8bit") || n == QLatin1String("16k8")) {
        PackSpec s;
        s.name = QStringLiteral("embedded8bit");
        s.sampleRate = 16000;
        s.bits = 8;
        s.adpcm = false;
        return s;
    }
    if (n == QLatin1String("mp3") || n == QLatin1String("dfplayer")) {
        PackSpec s;
        s.name = QStringLiteral("mp3");
        s.sampleRate = 16000;
        s.bits = 16;
        s.mp3 = true;
        return s;
    }
    // 默认 embedded：8k / IMA ADPCM / 体积最小
    PackSpec s;
    s.name = QStringLiteral("embedded");
    s.sampleRate = 8000;
    s.bits = 16;
    s.adpcm = true;
    return s;
}

QStringList PackSpec::knownNames()
{
    return { QStringLiteral("embedded"), QStringLiteral("embedded8k16"),
             QStringLiteral("embedded8bit"), QStringLiteral("pc"),
             QStringLiteral("mp3") };
}

QString PackSpec::describe() const
{
    QString codec;
    if (mp3)
        codec = QStringLiteral("MP3 32kbps");
    else if (adpcm)
        codec = QStringLiteral("IMA ADPCM 4bit");
    else
        codec = QStringLiteral("PCM %1bit").arg(bits);

    return QStringLiteral("%1 Hz / 单声道 / %2%3")
        .arg(sampleRate).arg(codec)
        .arg(normalize ? QStringLiteral(" / 峰值归一 %1dB").arg(targetDb, 0, 'f', 1) : QString());
}

QStringList PackSpec::ffmpegOutputArgs() const
{
    QStringList a;
    a << QStringLiteral("-ar") << QString::number(sampleRate) << QStringLiteral("-ac") << QStringLiteral("1");
    if (mp3)
        a << QStringLiteral("-c:a") << QStringLiteral("libmp3lame") << QStringLiteral("-b:a") << QStringLiteral("32k");
    else if (adpcm)
        a << QStringLiteral("-c:a") << QStringLiteral("adpcm_ima_wav");
    else if (bits == 8)
        a << QStringLiteral("-c:a") << QStringLiteral("pcm_u8");
    else
        a << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le");
    return a;
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
QString VoicePackBuilder::findFfmpeg(const VkConfig* cfg)
{
    // 1) 引擎 runtime 自带的 ffmpeg（最稳，一定配套）
    if (cfg && !cfg->engineDir.isEmpty()) {
        const QString local = QDir::cleanPath(cfg->engineDir + QStringLiteral("/runtime/ffmpeg/bin/ffmpeg.exe"));
        if (QFileInfo::exists(local))
            return local;
    }
    // 2) PATH 里的
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!fromPath.isEmpty())
        return fromPath;
    return QString();
}

bool VoicePackBuilder::probeWav(const QString& path, int* sampleRate, int* channels, int* bits,
                                qint64* dataOffset, qint64* dataLength, int* durationMs,
                                QString* err)
{
    if (sampleRate) *sampleRate = 0;
    if (channels)   *channels = 0;
    if (bits)       *bits = 0;
    if (dataOffset) *dataOffset = -1;
    if (dataLength) *dataLength = 0;
    if (durationMs) *durationMs = 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("打不开 %1: %2").arg(path, f.errorString());
        return false;
    }
    const QByteArray riff = f.read(12);
    if (riff.size() < 12 || riff.left(4) != "RIFF" || riff.mid(8, 4) != "WAVE") {
        if (err)
            *err = QStringLiteral("不是标准 wav: %1").arg(path);
        return false;
    }

    int sr = 0, ch = 0, bp = 0, fmtTag = 0, byteRate = 0;
    int factSamples = 0;                 //!< ADPCM 的权威样本数，就在 fact 块里
    qint64 dOff = -1, dLen = 0;

    while (!f.atEnd()) {
        const QByteArray ck = f.read(8);
        if (ck.size() < 8)
            break;
        const QByteArray id = ck.left(4);
        const quint32 sz = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(ck.constData() + 4));
        const qint64 next = f.pos() + sz + (sz & 1);   // chunk 按偶数对齐

        if (id == "fmt ") {
            const QByteArray d = f.read(qMin<qint64>(sz, 64));
            if (d.size() >= 16) {
                fmtTag   = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData()));
                ch       = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + 2));
                sr       = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + 4)));
                byteRate = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + 8)));
                bp       = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + 14));
            }
        } else if (id == "fact") {
            // ADPCM 必须靠这个算时长。用 byteRate 会偏得离谱
            // （实测 8k/ADPCM：byteRate 读出来 16000，但真值是 4013，
            //   算出来 512ms，实际 2020ms —— 差 4 倍）
            const QByteArray d = f.read(qMin<qint64>(sz, 12));
            if (d.size() >= 4)
                factSamples = int(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(d.constData())));
        } else if (id == "data") {
            dOff = f.pos();
            dLen = sz;
        }
        if (!f.seek(next))
            break;
    }

    if (sr <= 0) {
        if (err)
            *err = QStringLiteral("wav 里没有有效的 fmt 块: %1").arg(path);
        return false;
    }
    // IMA ADPCM 的 wBitsPerSample 是 4，但一个样本块按 4bit 算；这里如实报告
    if (fmtTag == 0x0011 && bp == 0)
        bp = 4;

    if (sampleRate) *sampleRate = sr;
    if (channels)   *channels = ch;
    if (bits)       *bits = bp;
    if (dataOffset) *dataOffset = dOff;
    if (dataLength) *dataLength = dLen;
    if (durationMs) {
        if (factSamples > 0 && sr > 0)
            *durationMs = int(double(factSamples) / sr * 1000.0);       // ADPCM：权威来源
        else if (byteRate > 0)
            *durationMs = int(double(dLen) / byteRate * 1000.0);
        else if (ch > 0 && bp > 0 && fmtTag == 1)
            *durationMs = int(double(dLen) / (sr * ch * (bp / 8.0)) * 1000.0);
    }
    return true;
}

bool VoicePackBuilder::measurePeakDb(const QString& wav, double* peakDb, QString* err)
{
    const QString ff = findFfmpeg(m_cfg);
    if (ff.isEmpty()) {
        if (err)
            *err = QStringLiteral("找不到 ffmpeg");
        return false;
    }
    QProcess p;
    p.start(ff, { QStringLiteral("-hide_banner"), QStringLiteral("-i"), wav,
                  QStringLiteral("-af"), QStringLiteral("volumedetect"),
                  QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-") });
    if (!p.waitForFinished(120000)) {
        p.kill();
        if (err)
            *err = QStringLiteral("ffmpeg volumedetect 超时");
        return false;
    }
    const QString out = QString::fromLocal8Bit(p.readAllStandardError());
    // mean_volume: -19.1 dB / max_volume: -1.1 dB
    const QRegularExpression re(QStringLiteral("max_volume:\\s*(-?[0-9.]+|[-+]?inf)\\s*dB"));
    const QRegularExpressionMatch m = re.match(out);
    if (!m.hasMatch()) {
        if (err)
            *err = QStringLiteral("ffmpeg 没给出 max_volume");
        return false;
    }
    bool okNum = false;
    const double v = m.captured(1).toDouble(&okNum);
    if (!okNum) {
        // -inf = 全静音。这种情况直接报告 -91dB，别让归一化把它放大成一堆噪声
        if (peakDb)
            *peakDb = -91.0;
        return true;
    }
    if (peakDb)
        *peakDb = v;
    return true;
}

VoicePackBuilder::VoicePackBuilder(const VkConfig* cfg)
    : m_cfg(cfg)
{
}

// ---------------------------------------------------------------------------
// 转换单个文件
// ---------------------------------------------------------------------------
QString VoicePackBuilder::convertOne(const QString& src, const QString& dst, const PackSpec& spec,
                                     int* durationMs, QString* err)
{
    const QString ff = findFfmpeg(m_cfg);
    if (ff.isEmpty()) {
        if (err)
            *err = QStringLiteral("找不到 ffmpeg（引擎 runtime/ffmpeg/bin 和 PATH 里都没有）");
        return QString();
    }

    QDir().mkpath(QFileInfo(dst).absolutePath());

    QStringList args{ QStringLiteral("-y"), QStringLiteral("-hide_banner"),
                      QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-i"), src };

    // 峰值归一化：先量再补差。用峰值而不是 loudnorm，是为了保住语音原来的动态。
    if (spec.normalize) {
        double peak = 0;
        if (measurePeakDb(src, &peak, nullptr) && peak > -90.0) {
            double gain = double(spec.targetDb) - peak;
            if (gain > 24.0)
                gain = 24.0;          // 别把底噪一起放大
            if (qAbs(gain) > 0.2)
                args << QStringLiteral("-af") << QStringLiteral("volume=%1dB").arg(gain, 0, 'f', 2);
        }
    }

    args << spec.ffmpegOutputArgs() << dst;

    QProcess p;
    p.start(ff, args);
    if (!p.waitForFinished(180000)) {
        p.kill();
        p.waitForFinished(5000);
        if (err)
            *err = QStringLiteral("ffmpeg 转换超时: %1").arg(QFileInfo(src).fileName());
        return QString();
    }
    if (p.exitCode() != 0) {
        if (err) {
            const QString se = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
            *err = QStringLiteral("ffmpeg 失败 (%1): %2")
                       .arg(QFileInfo(src).fileName(),
                            se.isEmpty() ? QStringLiteral("无错误输出") : se.left(300));
        }
        return QString();
    }

    int sr = 0, ch = 0, bp = 0; qint64 off = 0, len = 0;
    probeWav(dst, &sr, &ch, &bp, &off, &len, durationMs, nullptr);
    return dst;
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------
static QString makeId(const PackSpec& spec, int index)
{
    if (spec.idPrefix.isEmpty())
        return QStringLiteral("%1").arg(index, 4, 10, QLatin1Char('0'));
    return QStringLiteral("%1%2").arg(spec.idPrefix).arg(index + 1, 4, 10, QLatin1Char('0'));
}

PackResult VoicePackBuilder::buildFromWavs(const QStringList& wavs, const QString& outDir,
                                           const PackSpec& spec, QString* err)
{
    PackResult r;
    r.specText = spec.describe();
    if (wavs.isEmpty()) {
        if (err)
            *err = QStringLiteral("没有输入 wav");
        return r;
    }

    r.outDir = QDir::cleanPath(outDir);
    QDir().mkpath(r.outDir + QStringLiteral("/wav"));
    QDir().mkpath(r.outDir + QStringLiteral("/mcu"));

    const QString binFile = r.outDir + QStringLiteral("/mcu/voice.bin");
    QFile bin(binFile);
    if (!bin.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("写不了 %1: %2").arg(binFile, bin.errorString());
        return r;
    }

    const QString tmpDir = r.outDir + QStringLiteral("/.tmp");
    QDir().mkpath(tmpDir);

    QList<PackEntry> entries;
    int idx = 0;
    for (const QString& w : wavs) {
        if (!QFileInfo::exists(w)) {
            r.warnings << QStringLiteral("跳过不存在的文件: %1").arg(w);
            ++r.failed;
            continue;
        }
        PackEntry e;
        e.srcWav = w;
        e.id = makeId(spec, idx);

        // 文本 = 文件名去扩展名（去情绪前缀）
        QString base = QFileInfo(w).completeBaseName();
        e.text = base;

        // ① 通用档 16k/16bit（PC 直接能播）
        PackSpec pcSpec;
        pcSpec.sampleRate = 16000;
        pcSpec.bits = 16;
        pcSpec.normalize = spec.normalize;
        pcSpec.targetDb = spec.targetDb;
        QString cerr;
        e.outWav = convertOne(w, r.outDir + QStringLiteral("/wav/") + e.id + QStringLiteral(".wav"),
                              pcSpec, nullptr, &cerr);
        if (e.outWav.isEmpty()) {
            r.warnings << cerr;
            ++r.failed;
            continue;
        }

        // ② 嵌入式档（可以是 mp3 —— 那时不进 bin，单独放 mcu/mp3/）
        if (spec.mp3) {
            const QString mp3 = r.outDir + QStringLiteral("/mcu/mp3/") + e.id + QStringLiteral(".mp3");
            if (convertOne(w, mp3, spec, &e.durationMs, &cerr).isEmpty()) {
                r.warnings << cerr;
                ++r.failed;
                continue;
            }
            e.length = quint32(QFileInfo(mp3).size());
            e.offset = 0;
        } else {
            const QString mid = tmpDir + QStringLiteral("/") + e.id + QStringLiteral(".wav");
            if (convertOne(w, mid, spec, &e.durationMs, &cerr).isEmpty()) {
                r.warnings << cerr;
                ++r.failed;
                continue;
            }
            // 剥 wav 头，取裸数据
            qint64 dOff = -1, dLen = 0;
            if (!probeWav(mid, nullptr, nullptr, nullptr, &dOff, &dLen, nullptr, &cerr)) {
                r.warnings << cerr;
                ++r.failed;
                continue;
            }
            QFile mf(mid);
            if (!mf.open(QIODevice::ReadOnly) || !mf.seek(dOff)) {
                r.warnings << QStringLiteral("读不了中间文件: %1").arg(mid);
                ++r.failed;
                continue;
            }
            const QByteArray raw = mf.read(dLen);
            mf.close();

            e.offset = quint32(bin.pos());
            e.length = quint32(raw.size());
            if (bin.write(raw) != raw.size()) {
                if (err)
                    *err = QStringLiteral("写 voice.bin 失败");
                return r;
            }
        }

        r.srcBytes += QFileInfo(w).size();
        entries << e;
        ++idx;
    }

    bin.close();
    QDir(tmpDir).removeRecursively();

    r.binBytes = QFileInfo(binFile).size();
    r.count = entries.size();
    r.binFile = spec.mp3 ? QString() : binFile;

    // ── voice_index.h ─────────────────────────────────────
    const QString headerPath = r.outDir + QStringLiteral("/mcu/voice_index.h");
    QFile hf(headerPath);
    if (hf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray h;
        h += "/* 自动生成 by VoiceKit —— 请勿手改 */\n";
        h += "#ifndef VOICE_INDEX_H\n#define VOICE_INDEX_H\n\n#include <stdint.h>\n\n";
        h += QStringLiteral("#define VOICE_SAMPLE_RATE %1\n").arg(spec.sampleRate).toUtf8();
        h += QStringLiteral("#define VOICE_BITS        %1\n").arg(spec.adpcm ? 4 : spec.bits).toUtf8();
        h += QStringLiteral("#define VOICE_ADPCM       %1\n").arg(spec.adpcm ? 1 : 0).toUtf8();
        h += QStringLiteral("#define VOICE_COUNT       %1\n\n").arg(entries.size()).toUtf8();
        h += "typedef struct {\n";
        h += "    const char* id;          /* \"v0001\" */\n";
        h += "    uint32_t    offset;      /* 在 voice.bin 中的字节偏移 */\n";
        h += "    uint32_t    length;      /* 字节数 */\n";
        h += "    uint16_t    duration_ms;\n";
        h += "    uint8_t     flags;       /* bit0: ADPCM */\n";
        h += "} voice_entry_t;\n\n";
        h += "#ifndef VOICE_DATA_EMBEDDED\n";
        h += "extern const uint8_t       voice_data[];\n";
        h += "extern const uint32_t      voice_data_len;\n";
        h += "#endif\n";
        h += "extern const voice_entry_t voice_index[];\n";
        h += "extern const uint16_t      voice_count;\n\n";
        h += "/* 中文名 / 台词对照表（只为人看，MCU 里可以不用） */\n";
        for (const PackEntry& e : entries) {
            h += QStringLiteral("/* %1  %2 */\n").arg(e.id, e.text).toUtf8();
        }
        h += "\n#endif /* VOICE_INDEX_H */\n";
        hf.write(h);
        hf.close();
        r.indexPath = headerPath;
    }

    // ── manifest.json ─────────────────────────────────────
    QJsonArray arr;
    for (const PackEntry& e : entries) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), e.id);
        o.insert(QStringLiteral("char"), e.charName);
        o.insert(QStringLiteral("text"), e.text);
        o.insert(QStringLiteral("emotion"), e.emotion);
        o.insert(QStringLiteral("durationMs"), e.durationMs);
        o.insert(QStringLiteral("offset"), double(e.offset));
        o.insert(QStringLiteral("length"), double(e.length));
        o.insert(QStringLiteral("wav"), QStringLiteral("wav/%1.wav").arg(e.id));
        o.insert(QStringLiteral("src"), e.srcWav);
        arr.append(o);
    }
    QJsonObject specObj;
    specObj.insert(QStringLiteral("name"), spec.name);
    specObj.insert(QStringLiteral("sampleRate"), spec.sampleRate);
    specObj.insert(QStringLiteral("bits"), spec.adpcm ? 4 : spec.bits);
    specObj.insert(QStringLiteral("adpcm"), spec.adpcm);
    specObj.insert(QStringLiteral("mp3"), spec.mp3);
    specObj.insert(QStringLiteral("normalizeTargetDb"), double(spec.targetDb));

    QJsonObject root;
    root.insert(QStringLiteral("pack"), QFileInfo(r.outDir).fileName());
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("generatedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("spec"), specObj);
    root.insert(QStringLiteral("count"), entries.size());
    root.insert(QStringLiteral("binBytes"), double(r.binBytes));
    root.insert(QStringLiteral("srcBytes"), double(r.srcBytes));
    root.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(r.warnings));
    root.insert(QStringLiteral("entries"), arr);

    const QString man = r.outDir + QStringLiteral("/manifest.json");
    QFile mf(man);
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        mf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        mf.close();
        r.manifestPath = man;
    }

    r.ok = (entries.size() > 0);
    if (!r.ok && err)
        *err = QStringLiteral("一条都没成功（看 warnings）");
    return r;
}

PackResult VoicePackBuilder::buildFromTable(const QString& tableJson, const QString& outDir,
                                            const PackSpec& spec, QString* err)
{
    QFile f(tableJson);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("打不开任务表: %1 (%2)").arg(tableJson, f.errorString());
        return PackResult();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        if (err)
            *err = QStringLiteral("任务表不是合法 JSON");
        return PackResult();
    }

    QString cerr;
    ScriptParseResult table = ScriptParser::fromJson(doc.object(), &cerr);
    if (!table.isOk()) {
        if (err)
            *err = cerr.isEmpty() ? QStringLiteral("任务表里没有可用的行") : cerr;
        return PackResult();
    }

    QStringList wavs;
    QList<ScriptLine> picked;
    for (const ScriptLine& l : table.lines) {
        if (l.isNarration)
            continue;
        if (l.audioFile.isEmpty() || !QFileInfo::exists(l.audioFile))
            continue;
        wavs << l.audioFile;
        picked << l;
    }
    if (wavs.isEmpty()) {
        if (err)
            *err = QStringLiteral("任务表里没有已合成好的音频（audio 字段为空或文件不存在）");
        return PackResult();
    }

    PackResult r = buildFromWavs(wavs, outDir, spec, err);

    // 把角色/台词/情绪补回 manifest
    if (r.ok && !r.manifestPath.isEmpty()) {
        QFile mf(r.manifestPath);
        if (mf.open(QIODevice::ReadOnly)) {
            QJsonObject root = QJsonDocument::fromJson(mf.readAll()).object();
            mf.close();
            QJsonArray arr = root.value(QStringLiteral("entries")).toArray();
            for (int i = 0; i < arr.size() && i < picked.size(); ++i) {
                QJsonObject o = arr.at(i).toObject();
                o.insert(QStringLiteral("char"), picked.at(i).charName.isEmpty()
                                                    ? picked.at(i).speaker
                                                    : picked.at(i).charName);
                o.insert(QStringLiteral("text"), picked.at(i).text);
                o.insert(QStringLiteral("emotion"), picked.at(i).emotion);
                arr.replace(i, o);
            }
            root.insert(QStringLiteral("entries"), arr);
            if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                mf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                mf.close();
            }
        }
    }
    return r;
}

} // namespace vk
