#include "vk_kit.h"
#include "vk_apiclient.h"
#include "vk_charindex.h"
#include "vk_engine.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>

namespace vk {

Kit::Kit(QObject* parent)
    : QObject(parent)
{
}

Kit::~Kit()
{
    // 默认让引擎常驻：冷启动实测 180s，杀掉它下次还得再等三分钟。
    // 想真停就 stopEngine()（CLI: vk engine stop）。
    if (m_engine) {
        if (m_keepEngineOnExit)
            m_engine->detach();
        else
            m_engine->stopIfOwned();
    }
}

Kit& Kit::instance()
{
    static Kit inst;
    return inst;
}

void Kit::log(const QString& s)
{
    if (!s.isEmpty())
        emit logLine(s);
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool Kit::init(const QString& configPath, bool forceRescan, QString* err)
{
    if (m_inited && configPath.isEmpty())
        return true;

    m_cfg = VkConfig();

    if (!configPath.isEmpty()) {
        if (!m_cfg.loadFromFile(configPath, err))
            return false;
        log(QStringLiteral("[config] 已载入 %1").arg(configPath));
    } else {
        // 从 exe 目录向上找 config/voicekit.json
        QString found;
        QDir d(QCoreApplication::applicationDirPath());
        for (int i = 0; i < 4; ++i) {
            const QString p = d.absolutePath() + QStringLiteral("/config/voicekit.json");
            if (QFileInfo::exists(p)) {
                found = p;
                break;
            }
            if (!d.cdUp())
                break;
        }

        if (!found.isEmpty()) {
            if (!m_cfg.loadFromFile(found, err))
                return false;
            log(QStringLiteral("[config] 已载入 %1").arg(found));
        } else {
            QString note;
            if (!m_cfg.autoDiscover(&note)) {
                if (err)
                    *err = note;
                return false;
            }
            log(QStringLiteral("[config] %1").arg(note));
        }
    }

    const QStringList probs = m_cfg.problems();
    for (const QString& s : probs)
        log(QStringLiteral("[config] ⚠ %1").arg(s));
    if (!probs.isEmpty() && probs.first().startsWith(QStringLiteral("engineDir"))) {
        if (err)
            *err = probs.first();
        return false;
    }

    if (!m_api)
        m_api = new VkApiClient(this);
    m_api->setBaseUrl(m_cfg.apiBase());

    if (!m_engine) {
        m_engine = new VkEngine(&m_cfg, m_api, this);
        connect(m_engine, &VkEngine::logLine, this, &Kit::logLine);
        connect(m_engine, &VkEngine::ready, this, [this]() {
            emit statusChanged(QStringLiteral("引擎就绪"));
        });
    }

    if (!m_index)
        m_index = new VkCharIndex(&m_cfg);

    QString ierr;
    if (!m_index->load(forceRescan, &ierr)) {
        if (err)
            *err = ierr;
        return false;
    }
    log(QStringLiteral("[index] %1 个音色%2")
            .arg(m_index->count())
            .arg(m_index->isFromCache() ? QStringLiteral("（缓存）") : QStringLiteral("（已重扫）")));

    m_keepEngineOnExit = m_cfg.keepEngineOnExit;

    // 读回权重记忆（绑定引擎 pid，引擎重启过就自动作废）
    loadWeightState();
    if (m_cfg.rememberWeights && m_statePid != 0)
        log(QStringLiteral("[weights] 记忆: 引擎 pid %1 / %2")
                .arg(m_statePid).arg(QFileInfo(m_curGpt).fileName().isEmpty()
                                         ? QStringLiteral("(空)") : QFileInfo(m_curGpt).fileName()));

    m_inited = true;
    return true;
}

void Kit::shutdown()
{
    // 只标记未初始化；引擎去留见 ~Kit 与 stopEngine()
    m_inited = false;
}

void Kit::stopEngine()
{
    if (m_engine)
        m_engine->stop();
}

bool Kit::ensureEngine(QString* err)
{
    if (!m_inited) {
        QString e;
        if (!init(QString(), false, &e)) {
            if (err)
                *err = e;
            return false;
        }
    }
    emit statusChanged(QStringLiteral("正在准备引擎…"));
    const bool ok = m_engine->ensureReady(err);
    return ok;
}

// ---------------------------------------------------------------------------
// 角色
// ---------------------------------------------------------------------------
QList<CharInfo> Kit::characters() const
{
    return m_index ? m_index->all() : QList<CharInfo>();
}

QList<CharInfo> Kit::searchCharacters(const QString& keyword, int limit) const
{
    return m_index ? m_index->search(keyword, limit) : QList<CharInfo>();
}

CharInfo Kit::findCharacter(const QString& name, QString* diag) const
{
    return m_index ? m_index->find(name, diag) : CharInfo();
}

// ---------------------------------------------------------------------------
// 合成
// ---------------------------------------------------------------------------
QString Kit::cacheKeyFor(const QString& text, const QString& charName, const VoiceParams& p)
{
    const QString s = QStringLiteral("%1|%2|sp%3|tp%4|pp%5|tk%6|sd%7|rp%8|%9")
                          .arg(text, charName)
                          .arg(double(p.speed), 0, 'f', 3)
                          .arg(double(p.temperature), 0, 'f', 3)
                          .arg(double(p.topP), 0, 'f', 3)
                          .arg(p.topK)
                          .arg(p.seed)
                          .arg(double(p.repetitionPenalty), 0, 'f', 3)
                          .arg(p.splitMethod);
    return QString::fromLatin1(
        QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Md5).toHex());
}

bool Kit::ensureWeights(const CharInfo& c, QString* err)
{
    QString gpt;
    QString sov;

    if (c.zeroShot()) {
        // 零样本必须切回底模：否则会带上引擎里上一个微调角色的音色（真坑）
        gpt = m_cfg.baseGptRel;
        sov = m_cfg.baseSovitsRel;
    } else {
        gpt = c.gptCkpt;
        sov = c.sovitsPth;
    }

    // ── 权重记忆：引擎进程没换过 + 记的就是这一套 -> 跳过切换（省 40~60s）──
    if (m_cfg.rememberWeights) {
        const int pid = engineSessionId();
        if (pid != 0 && pid == m_statePid && m_curGpt == gpt && m_curSovits == sov) {
            log(QStringLiteral("[weights] 引擎(pid %1)已是「%2」，跳过切换").arg(pid).arg(c.name));
            return true;
        }
    }

    if (m_curGpt != gpt) {
        if (err)
            *err = QString();
        if (!m_api->setGptWeights(gpt, err))
            return false;
        m_curGpt = gpt;
        log(QStringLiteral("[weights] GPT -> %1").arg(QFileInfo(gpt).fileName()));
    }
    if (m_curSovits != sov) {
        if (!m_api->setSovitsWeights(sov, err))
            return false;
        m_curSovits = sov;
        log(QStringLiteral("[weights] SoVITS -> %1").arg(QFileInfo(sov).fileName()));
    }

    m_statePid = engineSessionId();
    saveWeightState();
    emit weightsSwitched(c.name);
    return true;
}

// ---------------------------------------------------------------------------
// 权重记忆落盘
// ---------------------------------------------------------------------------
QString Kit::statePath() const
{
    if (m_cfg.workDir.isEmpty())
        return QString();
    QDir().mkpath(m_cfg.workDir);
    return QDir::cleanPath(m_cfg.workDir + QStringLiteral("/weights_state.json"));
}

int Kit::engineSessionId()
{
    return m_api ? m_api->servicePid() : 0;
}

void Kit::loadWeightState()
{
    const QString p = statePath();
    if (p.isEmpty())
        return;
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();
    m_statePid  = o.value(QStringLiteral("enginePid")).toInt();
    m_curGpt    = o.value(QStringLiteral("gpt")).toString();
    m_curSovits = o.value(QStringLiteral("sovits")).toString();
}

void Kit::saveWeightState() const
{
    const QString p = statePath();
    if (p.isEmpty())
        return;
    QJsonObject o;
    o.insert(QStringLiteral("enginePid"), m_statePid);
    o.insert(QStringLiteral("gpt"), m_curGpt);
    o.insert(QStringLiteral("sovits"), m_curSovits);
    o.insert(QStringLiteral("port"), m_cfg.port);
    o.insert(QStringLiteral("savedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

QString Kit::saySync(const QString& text, const QString& charName,
                     const VoiceParams& params, QString* err, bool useCache)
{
    if (!m_inited) {
        QString e;
        if (!init(QString(), false, &e)) {
            if (err)
                *err = e;
            return QString();
        }
    }
    if (text.trimmed().isEmpty()) {
        if (err)
            *err = QStringLiteral("台词为空");
        return QString();
    }

    QString diag;
    const CharInfo c = m_index->find(charName, &diag);
    if (!c.isValid()) {
        if (err)
            *err = diag.isEmpty() ? QStringLiteral("找不到角色: %1").arg(charName) : diag;
        return QString();
    }
    if (!diag.isEmpty())
        log(QStringLiteral("[char] %1").arg(diag));

    if (!c.refAudio.isEmpty() && !QFileInfo::exists(c.refAudio)) {
        if (err)
            *err = QStringLiteral("参考音频不存在: %1").arg(c.refAudio);
        return QString();
    }

    const QString key = cacheKeyFor(text, c.name, params);
    const QString cacheFile = m_cfg.cacheDir() + QLatin1Char('/') + key + QStringLiteral(".wav");

    // 缓存命中就完全不碰引擎 —— 这才是"快速调用"的意义所在：
    // 游戏里播固定台词、循环同一个音效，不该为它启动一个要等三分钟的引擎。
    if (useCache && QFileInfo::exists(cacheFile) && QFileInfo(cacheFile).size() > 1024) {
        ++m_cacheHits;
        log(QStringLiteral("[cache] 命中 %1").arg(key.left(8)));
        return cacheFile;
    }

    // 真需要合成时才确保引擎在线
    if (!ensureEngine(err))
        return QString();

    // 串行化：权重是引擎全局状态，并发 = 音色串
    QMutexLocker locker(&m_lock);

    if (useCache && QFileInfo::exists(cacheFile) && QFileInfo(cacheFile).size() > 1024) {
        ++m_cacheHits;
        return cacheFile;
    }

    if (!ensureWeights(c, err))
        return QString();

    QVariantMap req;
    req.insert(QStringLiteral("text"), text);
    req.insert(QStringLiteral("text_lang"), params.textLang);
    req.insert(QStringLiteral("ref_audio_path"), c.refAudio);
    req.insert(QStringLiteral("prompt_text"), c.promptText);
    req.insert(QStringLiteral("prompt_lang"), params.promptLang);
    req.insert(QStringLiteral("text_split_method"), params.splitMethod);
    req.insert(QStringLiteral("batch_size"), 1);
    req.insert(QStringLiteral("speed_factor"), double(params.speed));
    req.insert(QStringLiteral("temperature"), double(params.temperature));
    req.insert(QStringLiteral("top_k"), params.topK);
    req.insert(QStringLiteral("top_p"), double(params.topP));
    req.insert(QStringLiteral("seed"), params.seed);
    req.insert(QStringLiteral("repetition_penalty"), double(params.repetitionPenalty));
    req.insert(QStringLiteral("media_type"), params.mediaType);
    req.insert(QStringLiteral("streaming_mode"), false);
    req.insert(QStringLiteral("parallel_infer"), true);

    const QByteArray wav = m_api->tts(req, err, m_cfg.ttsTimeoutMs);
    if (wav.isEmpty())
        return QString();

    QSaveFile sf(cacheFile);
    if (!sf.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("无法写入缓存: %1 (%2)").arg(cacheFile, sf.errorString());
        return QString();
    }
    sf.write(wav);
    if (!sf.commit()) {
        if (err)
            *err = QStringLiteral("缓存落盘失败: %1").arg(cacheFile);
        return QString();
    }

    ++m_synthCount;
    log(QStringLiteral("[tts] %1 「%2」 -> %3 (%4 KB)")
            .arg(c.name, text.left(12)).arg(key.left(8)).arg(wav.size() / 1024));
    return cacheFile;
}

QString Kit::sayToFile(const QString& text, const QString& charName,
                       const QString& outWavPath, const VoiceParams& params,
                       QString* err)
{
    const QString src = saySync(text, charName, params, err, true);
    if (src.isEmpty())
        return QString();

    if (outWavPath.isEmpty() || QFileInfo(src) == QFileInfo(outWavPath))
        return src;

    QDir().mkpath(QFileInfo(outWavPath).absolutePath());
    QFile::remove(outWavPath);
    if (!QFile::copy(src, outWavPath)) {
        if (err)
            *err = QStringLiteral("复制到 %1 失败").arg(outWavPath);
        return QString();
    }
    return outWavPath;
}

} // namespace vk
