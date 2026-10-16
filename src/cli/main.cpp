/*!
 * \file main.cpp
 * \brief VoiceKit 命令行工具 —— 第一批交付的验证入口。
 *
 * 用法：
 *   vk info                          显示配置 / 索引概况
 *   vk list [关键词]                  列出角色
 *   vk ping                          引擎探活
 *   vk engine start|stop|status      引擎进程管理
 *   vk say -c 钟离 -t "契约，必须履行。" [-o out.wav] [--speed 1.05] [--no-cache]
 */

#include "VoiceKit.h"
#include "vk_script.h"
#include "vk_pack.h"
#include "vk_join.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static void out(const QString& s)
{
    fputs(s.toUtf8().constData(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void errOut(const QString& s)
{
    fputs(s.toUtf8().constData(), stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 2);
}

//! 是不是宽字符（汉字/全角）—— Qt5 没有现成的，按 unicode 区间判
static bool isWideChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x1100 && u <= 0x115F)
        || (u >= 0x2E80 && u <= 0xA4CF)
        || (u >= 0xAC00 && u <= 0xD7A3)
        || (u >= 0xF900 && u <= 0xFAFF)
        || (u >= 0xFE30 && u <= 0xFE6F)
        || (u >= 0xFF00 && u <= 0xFF60)
        || (u >= 0xFFE0 && u <= 0xFFE6);
}

//! 终端对齐：汉字按 2 个字符宽算
static int displayWidth(const QString& s)
{
    int w = 0;
    for (const QChar& c : s)
        w += isWideChar(c) ? 2 : 1;
    return w;
}

static QString pad(const QString& s, int width)
{
    QString o = s;
    for (int need = width - displayWidth(s); need > 0; --need)
        o += QLatin1Char(' ');
    return o;
}

// ---------------------------------------------------------------------------

static int cmdInfo(vk::Kit& kit)
{
    const vk::VkConfig c = kit.config();
    out(QStringLiteral("VoiceKit %1").arg(QLatin1String(VOICEKIT_VERSION)));
    out(QStringLiteral("  引擎目录  : %1").arg(c.engineDir));
    out(QStringLiteral("  模型库    : %1").arg(c.modelRoot));
    out(QStringLiteral("  工作目录  : %1").arg(c.workDir));
    out(QStringLiteral("  服务地址  : %1").arg(c.apiBase().toString()));
    out(QStringLiteral("  Python    : %1").arg(c.resolvedPython()));
    out(QStringLiteral("  音色数量  : %1").arg(kit.characters().size()));
    out(QStringLiteral("  缓存命中  : %1   合成次数: %2").arg(kit.cacheHits()).arg(kit.synthCount()));

    const QStringList games = kit.index()->games();
    for (const QString& g : games) {
        int n = 0;
        int zs = 0;
        for (const vk::CharInfo& ci : kit.characters()) {
            if (ci.game == g) {
                ++n;
                if (ci.zeroShot())
                    ++zs;
            }
        }
        out(QStringLiteral("    %1: %2 个音色（其中零样本 %3）").arg(g).arg(n).arg(zs));
    }
    return 0;
}

static int cmdList(vk::Kit& kit, const QString& keyword)
{
    const QList<vk::CharInfo> list = keyword.isEmpty()
                                         ? kit.characters()
                                         : kit.searchCharacters(keyword, 500);
    if (list.isEmpty()) {
        errOut(QStringLiteral("没有匹配的角色: %1").arg(keyword));
        return 2;
    }
    QString curGame;
    for (const vk::CharInfo& c : list) {
        if (c.game != curGame) {
            curGame = c.game;
            out(QStringLiteral("\n== %1 ==").arg(curGame));
        }
        out(QStringLiteral("  %1%2  [%3]")
                .arg(c.name, c.zeroShot() ? QStringLiteral("(零样本)") : QString(),
                     QFileInfo(c.refAudio).fileName()));
    }
    out(QStringLiteral("\n共 %1 个").arg(list.size()));
    return 0;
}

static int cmdPing(vk::Kit& kit)
{
    const bool ok = kit.api()->ping(3000);
    out(ok ? QStringLiteral("引擎在线: %1").arg(kit.config().apiBase().toString())
           : QStringLiteral("引擎离线: %1").arg(kit.config().apiBase().toString()));
    return ok ? 0 : 1;
}

static int cmdEngine(vk::Kit& kit, const QString& action)
{
    if (action == QLatin1String("stop")) {
        kit.engine()->stop();
        out(QStringLiteral("已停止。"));
        return 0;
    }
    if (action == QLatin1String("status")) {
        out(QStringLiteral("进程状态 : %1")
                .arg(kit.engine()->isProcessRunning() ? QStringLiteral("running")
                                                      : QStringLiteral("not running")));
        out(QStringLiteral("服务探活 : %1")
                .arg(kit.engine()->isServiceAlive() ? QStringLiteral("online")
                                                    : QStringLiteral("offline")));
        out(QStringLiteral("归属     : %1")
                .arg(kit.engine()->ownedByUs() ? QStringLiteral("本进程拉起")
                                               : QStringLiteral("外部服务")));
        return 0;
    }

    QObject::connect(kit.engine(), &vk::VkEngine::logLine,
                     [](const QString& l) { out(QStringLiteral("  [engine] %1").arg(l)); });

    QString err;
    if (!kit.ensureEngine(&err)) {
        errOut(QStringLiteral("引擎启动失败: %1").arg(err));
        return 3;
    }
    out(QStringLiteral("引擎就绪: %1").arg(kit.config().apiBase().toString()));
    return 0;
}

static int cmdSay(vk::Kit& kit, const QCommandLineParser& p)
{
    const QString charName = p.value(QStringLiteral("char"));
    const QString text = p.value(QStringLiteral("text"));
    if (charName.isEmpty() || text.isEmpty()) {
        errOut(QStringLiteral("用法: vk say -c <角色> -t <台词> [-o out.wav]"));
        return 2;
    }

    vk::VoiceParams vp;
    // 情绪先打底（走剧本那套词表，映射成语速/温度/topK 等），
    // 显式传的 --speed/--temp/--seed 再覆盖上去
    if (p.isSet(QStringLiteral("emotion")))
        vp = vk::ScriptParser::paramsForEmotion(p.value(QStringLiteral("emotion")));
    if (p.isSet(QStringLiteral("speed")))
        vp.speed = p.value(QStringLiteral("speed")).toFloat();
    if (p.isSet(QStringLiteral("temp")))
        vp.temperature = p.value(QStringLiteral("temp")).toFloat();
    if (p.isSet(QStringLiteral("seed")))
        vp.seed = p.value(QStringLiteral("seed")).toInt();

    QObject::connect(&kit, &vk::Kit::logLine,
                     [](const QString& l) { out(QStringLiteral("  [vk] %1").arg(l)); });

    QString err;
    const QString wav = p.isSet(QStringLiteral("out"))
                            ? kit.sayToFile(text, charName, p.value(QStringLiteral("out")), vp, &err)
                            : kit.saySync(text, charName, vp, &err, !p.isSet(QStringLiteral("no-cache")));
    if (wav.isEmpty()) {
        errOut(QStringLiteral("合成失败: %1").arg(err));
        return 3;
    }
    out(QStringLiteral("wav: %1  (%2)").arg(wav, humanSize(QFileInfo(wav).size())));
    return 0;
}

static int cmdScript(vk::Kit& kit, const QCommandLineParser& p, const QString& file)
{
    if (file.isEmpty()) {
        errOut(QStringLiteral("用法: vk script <剧本.txt> [-o 任务表.json] [--llm] [--narration]"));
        return 2;
    }

    vk::ScriptParseOptions opt;
    opt.useLlm           = p.isSet(QStringLiteral("llm"));
    opt.includeNarration = p.isSet(QStringLiteral("narration"));
    opt.llmApiKey        = p.value(QStringLiteral("llm-key"));
    opt.useContext       = !p.isSet(QStringLiteral("no-context"));
    opt.defaultChar      = p.value(QStringLiteral("default-char"));

    vk::ScriptParser parser(kit.index(), nullptr);
    QString err;
    const vk::ScriptParseResult r = parser.parseFile(file, opt, &err);
    if (!r.isOk()) {
        errOut(QStringLiteral("解析失败: %1").arg(err));
        return 3;
    }

    out(QStringLiteral("%1 %2 %3 %4 %5")
            .arg(QStringLiteral("#"), 3)
            .arg(pad(QStringLiteral("角色"), 20))
            .arg(pad(QStringLiteral("情绪"), 8))
            .arg(pad(QStringLiteral("来源"), 6))
            .arg(QStringLiteral("台词")));

    for (const vk::ScriptLine& l : r.lines) {
        if (l.isNarration && !opt.includeNarration)
            continue;
        QString who;
        if (l.isNarration)
            who = QStringLiteral("（旁白）");
        else if (l.resolved)
            who = (l.lowConfidence ? QStringLiteral("? ") : QString()) + l.charName;
        else
            who = QStringLiteral("⚠ ") + (l.speaker.isEmpty() ? QStringLiteral("未标注") : l.speaker);

        out(QStringLiteral("%1 %2 %3 %4 %5")
                .arg(l.index, 3)
                .arg(pad(who, 20))
                .arg(pad(l.emotion, 8))
                .arg(pad(vk::speakerSourceName(l.source), 6))
                .arg(l.text));
        if (!l.matchNote.isEmpty() && (!l.resolved || l.lowConfidence))
            out(QStringLiteral("      ↳ %1").arg(l.matchNote));
    }

    out(QString(64, QLatin1Char('-')));
    out(QStringLiteral("共 %1 行 → 匹配 %2 / 未匹配 %3 / 旁白 %4")
            .arg(r.total).arg(r.matched).arg(r.unresolved).arg(r.narration));
    if (r.contextInferred || r.llmInferred) {
        out(QStringLiteral("其中推断得来：上下文 %1 条、LLM %2 条（带 ? 的请人工过一眼）")
                .arg(r.contextInferred).arg(r.llmInferred));
    }
    for (const QString& w : r.warnings)
        out(QStringLiteral("⚠ %1").arg(w));

    if (p.isSet(QStringLiteral("out"))) {
        const QString path = p.value(QStringLiteral("out"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            errOut(QStringLiteral("写不了 %1: %2").arg(path, f.errorString()));
            return 4;
        }
        f.write(QJsonDocument(vk::ScriptParser::toJson(r, file)).toJson(QJsonDocument::Indented));
        out(QStringLiteral("任务表已保存: %1").arg(path));
    }
    return r.unresolved > 0 ? 1 : 0;   // 有没匹配上的返回 1，方便脚本判断
}

/*! 把多句单声音频拼成"一整段"。
 *  参数一致的 wav 直接拼 PCM（不重编码、不掉音质、秒级完成）。 */
static int cmdJoin(vk::Kit& kit, const QCommandLineParser& p)
{
    Q_UNUSED(kit);
    const QString outPath = p.value(QStringLiteral("out"));
    const QString dir     = p.value(QStringLiteral("dir"));
    const QString match   = p.value(QStringLiteral("match"));
    int gap = p.value(QStringLiteral("gap")).toInt();
    if (gap < 0)
        gap = 0;

    QStringList files = p.positionalArguments();
    // 第一个位置参数是命令名本身（"join"），要去掉
    if (!files.isEmpty() && files.first().compare(QStringLiteral("join"), Qt::CaseInsensitive) == 0)
        files.removeFirst();

    if (!dir.isEmpty()) {
        QDir d(dir);
        if (!d.exists()) {
            errOut(QStringLiteral("目录不存在: %1").arg(dir));
            return 3;
        }
        const QStringList names = d.entryList(QStringList{ QStringLiteral("*.wav") },
                                              QDir::Files, QDir::Name);
        for (const QString& n : names) {
            if (!match.isEmpty() && !n.contains(match, Qt::CaseInsensitive))
                continue;
            files << d.absoluteFilePath(n);
        }
    }

    if (files.isEmpty()) {
        errOut(QStringLiteral("用法: vk join a.wav b.wav c.wav -o 整段.wav [--gap 280]"));
        errOut(QStringLiteral("      vk join --dir wav目录 -o 整段.wav [--match 阿贝多] [--gap 280]"));
        errOut(QStringLiteral("说明: 顺序按文件名里的数字排（0009 在 0010 前），这就是成品的顺序"));
        return 2;
    }
    if (outPath.isEmpty()) {
        errOut(QStringLiteral("要用 -o 指定输出文件，例如 -o work/gui/整段.wav"));
        return 2;
    }

    files = vk::VoiceJoiner::naturalSort(files);      // 顺序 = 成品顺序

    out(QStringLiteral("要合并 %1 个（顺序即成品顺序）:").arg(files.size()));
    for (const QString& f : files)
        out(QStringLiteral("  %1").arg(QFileInfo(f).fileName()));

    vk::JoinResult r;
    QString err;
    if (!vk::VoiceJoiner::join(files, outPath, gap, &r, &err)) {
        errOut(QStringLiteral("合并失败: %1").arg(err));
        return 4;
    }

    const int sec = r.totalMs / 1000;
    out(QString());
    out(QStringLiteral("合并完成 → %1").arg(outPath));
    out(QStringLiteral("  %1 句 · 总长 %2:%3 · %4 · %5Hz/%6声道/%7bit · 句间 %8ms")
            .arg(r.fileCount)
            .arg(sec / 60).arg(sec % 60, 2, 10, QLatin1Char('0'))
            .arg(humanSize(r.totalBytes))
            .arg(r.sampleRate).arg(r.channels).arg(r.bits).arg(gap));
    out(QStringLiteral("wav: %1").arg(outPath));      // 这行让 GUI 认得出来、自动进列表
    return 0;
}

static int cmdPack(vk::Kit& kit, const QCommandLineParser& p)
{
    const QString table = p.value(QStringLiteral("table"));
    const QString dir   = p.value(QStringLiteral("dir"));
    const QString outDir = p.value(QStringLiteral("out"));

    if (outDir.isEmpty()) {
        errOut(QStringLiteral("用法: vk pack --table 任务表.json -o 输出目录 [--spec embedded]"));
        errOut(QStringLiteral("      vk pack --dir   wav目录    -o 输出目录 [--spec embedded]"));
        errOut(QStringLiteral("可用规格: %1").arg(vk::PackSpec::knownNames().join(QStringLiteral(" | "))));
        return 2;
    }
    if (table.isEmpty() && dir.isEmpty()) {
        errOut(QStringLiteral("要指定 --table（配音任务表）或 --dir（wav 目录）"));
        return 2;
    }

    // 注意：config() 返回的是值，必须存成局部变量再取地址（&kit.config() 是临时对象）
    const vk::VkConfig cfg = kit.config();
    vk::VoicePackBuilder builder(&cfg);

    vk::PackSpec spec = vk::PackSpec::fromName(p.value(QStringLiteral("spec")));
    if (p.isSet(QStringLiteral("no-normalize")))
        spec.normalize = false;
    out(QStringLiteral("规格: %1").arg(spec.describe()));

    QString err;
    vk::PackResult r;
    if (!table.isEmpty()) {
        out(QStringLiteral("输入: 任务表 %1").arg(table));
        r = builder.buildFromTable(table, outDir, spec, &err);
    } else {
        QDir d(dir);
        if (!d.exists()) {
            errOut(QStringLiteral("目录不存在: %1").arg(dir));
            return 3;
        }
        const QStringList files = d.entryList(QStringList{ QStringLiteral("*.wav") },
                                              QDir::Files, QDir::Name);
        QStringList wavs;
        for (const QString& f : files)
            wavs << d.absoluteFilePath(f);
        out(QStringLiteral("输入: 目录 %1（%2 个 wav）").arg(dir).arg(wavs.size()));
        r = builder.buildFromWavs(wavs, outDir, spec, &err);
    }

    for (const QString& w : r.warnings)
        out(QStringLiteral("  ⚠ %1").arg(w));

    if (!r.ok) {
        errOut(QStringLiteral("打包失败: %1").arg(err));
        return 4;
    }

    out(QString());
    out(QStringLiteral("打包完成 → %1").arg(r.outDir));
    out(QStringLiteral("  条目        : %1 条（失败 %2）").arg(r.count).arg(r.failed));
    if (!r.binFile.isEmpty())
        out(QStringLiteral("  voice.bin   : %1").arg(humanSize(r.binBytes)));
    out(QStringLiteral("  源音频合计  : %1").arg(humanSize(r.srcBytes)));
    if (r.srcBytes > 0)
        out(QStringLiteral("  压缩到      : %1%")
                .arg(double(r.binBytes) / double(r.srcBytes) * 100.0, 0, 'f', 1));
    out(QStringLiteral("  索引头      : %1").arg(r.indexPath));
    out(QStringLiteral("  清单        : %1").arg(r.manifestPath));
    out(QStringLiteral("  通用档 wav  : %1/wav/").arg(r.outDir));
    return 0;
}

static int cmdSynth(vk::Kit& kit, const QCommandLineParser& p)
{
    const QString tablePath = p.value(QStringLiteral("table"));
    if (tablePath.isEmpty()) {
        errOut(QStringLiteral("用法: vk synth --table 任务表.json [-o 音频目录] [--dry-run]"));
        return 2;
    }

    QFile f(tablePath);
    if (!f.open(QIODevice::ReadOnly)) {
        errOut(QStringLiteral("打不开任务表: %1").arg(tablePath));
        return 3;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        errOut(QStringLiteral("任务表不是合法 JSON"));
        return 3;
    }

    QString perr;
    vk::ScriptParseResult table = vk::ScriptParser::fromJson(doc.object(), &perr);
    if (!table.isOk()) {
        errOut(QStringLiteral("任务表解析失败: %1").arg(perr));
        return 3;
    }

    const bool dry = p.isSet(QStringLiteral("dry-run"));
    const QString outDir = p.value(QStringLiteral("out"));

    QList<int> todo;
    int skipNarr = 0, skipUnres = 0, skipDone = 0;
    for (int i = 0; i < table.lines.size(); ++i) {
        const vk::ScriptLine& l = table.lines.at(i);
        if (l.isNarration) {
            ++skipNarr;
            continue;
        }
        if (!l.resolved || l.charName.isEmpty()) {
            ++skipUnres;
            continue;
        }
        if (!l.audioFile.isEmpty() && QFileInfo::exists(l.audioFile)) {
            ++skipDone;
            continue;
        }
        todo << i;
    }

    if (todo.isEmpty()) {
        out(QStringLiteral("没有要合成的行（旁白 %1 / 未匹配 %2 / 已有音频 %3）")
                .arg(skipNarr).arg(skipUnres).arg(skipDone));
        return 0;
    }

    // 按角色排序 —— 同一个角色连着做，省掉来回切权重的 40 秒
    std::stable_sort(todo.begin(), todo.end(), [&table](int a, int b) {
        const QString& ca = table.lines.at(a).charName;
        const QString& cb = table.lines.at(b).charName;
        if (ca != cb)
            return ca < cb;
        return a < b;
    });

    out(QStringLiteral("待合成 %1 句（跳过：旁白 %2 / 未匹配 %3 / 已有音频 %4）")
            .arg(todo.size()).arg(skipNarr).arg(skipUnres).arg(skipDone));
    out(QStringLiteral("已按角色排序，同一个角色连着做（少切权重）："));
    QString lastChar;
    for (int i : todo) {
        const vk::ScriptLine& l = table.lines.at(i);
        if (l.charName != lastChar) {
            lastChar = l.charName;
            out(QStringLiteral("  ▸ %1").arg(lastChar));
        }
        out(QStringLiteral("        #%1  %2").arg(l.index, 3).arg(l.text.left(40)));
    }

    if (dry) {
        out(QStringLiteral("\n--dry-run：以上只是计划，没有真的合成"));
        return 0;
    }

    out(QString());
    QElapsedTimer clock;
    clock.start();
    int ok = 0, fail = 0;
    for (int n = 0; n < todo.size(); ++n) {
        vk::ScriptLine& l = table.lines[todo.at(n)];
        const vk::VoiceParams vp = vk::ScriptParser::paramsForEmotion(l.emotion);
        QString err;
        const QString wav = kit.saySync(l.text, l.charName, vp, &err);
        const int el = int(clock.elapsed() / 1000);

        if (wav.isEmpty()) {
            ++fail;
            out(QStringLiteral("[%1/%2] %3  ✗ %4")
                    .arg(n + 1).arg(todo.size()).arg(l.charName, err.left(70)));
            continue;
        }
        ++ok;
        QString finalWav = wav;
        if (!outDir.isEmpty()) {
            QDir().mkpath(outDir);
            const QString dst = QStringLiteral("%1/%2_%3.wav")
                                    .arg(outDir)
                                    .arg(l.index, 4, 10, QLatin1Char('0'))
                                    .arg(l.charName);
            QFile::remove(dst);
            if (QFile::copy(wav, dst))
                finalWav = dst;
        }
        l.audioFile = finalWav;
        out(QStringLiteral("[%1/%2] %3「%4」✓  (%5s)")
                .arg(n + 1).arg(todo.size()).arg(l.charName, l.text.left(14)).arg(el));

        // 每句都写回任务表：中途 Ctrl+C 也不丢已完成的进度
        QFile wf(tablePath);
        if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            wf.write(QJsonDocument(vk::ScriptParser::toJson(table, tablePath))
                         .toJson(QJsonDocument::Indented));
            wf.close();
        }
    }

    out(QString());
    out(QStringLiteral("合成完成：成功 %1 / 失败 %2，共耗时 %3 秒")
            .arg(ok).arg(fail).arg(int(clock.elapsed() / 1000)));
    out(QStringLiteral("任务表已更新（audio 字段）: %1").arg(tablePath));
    if (fail > 0)
        out(QStringLiteral("有失败的句子，重跑一次会自动续做（已完成的会跳过）"));
    return fail > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
    SetConsoleOutputCP(65001);   // 控制台按 UTF-8 解码，否则中文输出全是乱码
#endif

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("vk"));
    QCoreApplication::setApplicationVersion(QStringLiteral(VOICEKIT_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("VoiceKit —— GPT-SoVITS 音色工具链命令行"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption optChar({ QStringLiteral("c"), QStringLiteral("char") },
                                     QStringLiteral("角色名"), QStringLiteral("name"));
    const QCommandLineOption optText({ QStringLiteral("t"), QStringLiteral("text") },
                                     QStringLiteral("要合成的台词"), QStringLiteral("text"));
    const QCommandLineOption optOut({ QStringLiteral("o"), QStringLiteral("out") },
                                    QStringLiteral("输出 wav 路径"), QStringLiteral("path"));
    const QCommandLineOption optSpeed(QStringLiteral("speed"),
                                      QStringLiteral("语速倍率 (默认 1.0)"),
                                      QStringLiteral("x"), QStringLiteral("1.0"));
    const QCommandLineOption optTemp(QStringLiteral("temp"),
                                     QStringLiteral("采样温度 (默认 1.0)"),
                                     QStringLiteral("x"), QStringLiteral("1.0"));
    const QCommandLineOption optSeed(QStringLiteral("seed"),
                                     QStringLiteral("随机种子 (-1 = 随机)"),
                                     QStringLiteral("n"), QStringLiteral("-1"));
    const QCommandLineOption optNoCache(QStringLiteral("no-cache"),
                                        QStringLiteral("忽略缓存重新合成"));
    const QCommandLineOption optRescan(QStringLiteral("rescan"),
                                       QStringLiteral("强制重扫模型库"));
    const QCommandLineOption optConfig({ QStringLiteral("f"), QStringLiteral("config") },
                                       QStringLiteral("配置文件路径"), QStringLiteral("path"));
    const QCommandLineOption optLlm(QStringLiteral("llm"),
                                    QStringLiteral("剧本：用 LLM 兜底识别没标注的说话人"));
    const QCommandLineOption optLlmKey(QStringLiteral("llm-key"),
                                       QStringLiteral("剧本：DeepSeek API Key（默认读环境变量或 ~/.deepseek_key）"),
                                       QStringLiteral("key"));
    const QCommandLineOption optNarration(QStringLiteral("narration"),
                                          QStringLiteral("剧本：表格里也列出旁白行"));
    const QCommandLineOption optNoContext(QStringLiteral("no-context"),
                                          QStringLiteral("剧本：关闭上下文交替推断"));
    const QCommandLineOption optTable(QStringLiteral("table"),
                                      QStringLiteral("打包：配音任务表 JSON（vk script 的产物）"),
                                      QStringLiteral("json"));
    const QCommandLineOption optDir(QStringLiteral("dir"),
                                    QStringLiteral("打包：直接拿一个目录里的 wav"),
                                    QStringLiteral("path"));
    const QCommandLineOption optSpec(QStringLiteral("spec"),
                                     QStringLiteral("打包：规格 embedded|embedded8k16|embedded8bit|pc|mp3"),
                                     QStringLiteral("name"), QStringLiteral("embedded"));
    const QCommandLineOption optNoNorm(QStringLiteral("no-normalize"),
                                       QStringLiteral("打包：不做音量归一"));
    const QCommandLineOption optGap(QStringLiteral("gap"),
                                     QStringLiteral("合并：句与句之间插入的静音毫秒（默认 280，0 = 紧贴）"),
                                     QStringLiteral("ms"), QStringLiteral("280"));
    const QCommandLineOption optMatch(QStringLiteral("match"),
                                       QStringLiteral("合并：配合 --dir，只取文件名含该子串的音频"),
                                       QStringLiteral("text"));
    const QCommandLineOption optDryRun(QStringLiteral("dry-run"),
                                       QStringLiteral("批量合成：只列计划，不真合成"));
    const QCommandLineOption optEmotion(QStringLiteral("emotion"),
                                        QStringLiteral("情绪词（生气/开心/温柔/悲伤…），映射成合成参数"),
                                        QStringLiteral("word"));
    const QCommandLineOption optDefaultChar(
        QStringLiteral("default-char"),
        QStringLiteral("剧本：没匹配上角色的行用这个角色兜底（甲/乙 这类代称很有用）"),
        QStringLiteral("name"));

    parser.addOptions({ optChar, optText, optOut, optSpeed, optTemp, optSeed,
                        optNoCache, optRescan, optConfig, optEmotion, optDefaultChar,
                        optLlm, optLlmKey, optNarration, optNoContext,
                        optTable, optDir, optSpec, optNoNorm, optDryRun,
                        optGap, optMatch });
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("info | list | ping | engine | say | script | synth | pack | join"));
    parser.addPositionalArgument(QStringLiteral("args"), QStringLiteral("命令附加参数"),
                                 QStringLiteral("[args...]"));
    parser.process(app);

    const QStringList pos = parser.positionalArguments();
    const QString cmd = pos.isEmpty() ? QStringLiteral("info") : pos.first().toLower();

    // help / version 已由 parser 处理
    vk::Kit& kit = vk::Kit::instance();
    QString err;
    if (!kit.init(parser.value(QStringLiteral("config")),
                  parser.isSet(QStringLiteral("rescan")), &err)) {
        errOut(QStringLiteral("初始化失败: %1").arg(err));
        return 4;
    }

    if (cmd == QLatin1String("info"))
        return cmdInfo(kit);
    if (cmd == QLatin1String("list"))
        return cmdList(kit, pos.size() > 1 ? pos.at(1) : QString());
    if (cmd == QLatin1String("ping"))
        return cmdPing(kit);
    if (cmd == QLatin1String("engine"))
        return cmdEngine(kit, pos.size() > 1 ? pos.at(1).toLower() : QStringLiteral("start"));
    if (cmd == QLatin1String("say"))
        return cmdSay(kit, parser);
    if (cmd == QLatin1String("script"))
        return cmdScript(kit, parser, pos.size() > 1 ? pos.at(1) : QString());
    if (cmd == QLatin1String("synth"))
        return cmdSynth(kit, parser);
    if (cmd == QLatin1String("pack"))
        return cmdPack(kit, parser);
    if (cmd == QLatin1String("join"))
        return cmdJoin(kit, parser);

    errOut(QStringLiteral("未知命令: %1").arg(cmd));
    parser.showHelp(2);
    return 2;
}
