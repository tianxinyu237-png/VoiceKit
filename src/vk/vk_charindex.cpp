#include "vk_charindex.h"
#include "vk_config.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

#include <climits>

namespace vk {

static const char* kIndexVersion = "1";

// ---------------------------------------------------------------------------
// 静态工具
// ---------------------------------------------------------------------------

/*! 剥掉目录名尾巴。
 *  判据：连续 2 个以上短横线及其后内容 —— 因为模型库里全是
 *  "布洛妮娅----御姐" / "桂乃芬------------------女友" / "砂金---男主的声音"，
 *  而正常角色名（托帕&账账 / 阮•梅 / 开拓者(男)）一个短横线都没有。 */
QString VkCharIndex::normalizeName(const QString& rawDirName)
{
    QString s = rawDirName.trimmed();
    static const QRegularExpression dashTail(QStringLiteral("[-—–]{2,}.*$"));
    s.remove(dashTail);
    s = s.trimmed();
    if (s.isEmpty())
        s = rawDirName.trimmed();
    return s;
}

//! 情绪标签长这样：害怕 / 激动说话 / 平静说话 / 委屈 / 疑惑 —— 短、纯汉字、无标点
static bool looksLikeEmotionTag(const QString& t)
{
    if (t.isEmpty() || t.size() > 6)
        return false;

    static const QRegularExpression punct(
        QStringLiteral("[，。！？、,.!?;:：；\"'`「」『』（）()\\[\\]<>《》…—～~]"));
    if (punct.match(t).hasMatch())
        return false;

    bool hasHan = false;
    for (const QChar& c : t) {
        if (c.script() == QChar::Script_Han) {
            hasHan = true;
        } else if (c.isLetterOrNumber()) {
            return false;   // 含英文字母/数字 -> 不当标签（避免把 "well-known" 的 well 剥掉）
        }
    }
    return hasHan;
}

QString VkCharIndex::extractPromptText(const QString& wavFileName)
{
    QString base = wavFileName;
    if (base.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
        base.chop(4);

    const int idx = base.indexOf(QLatin1Char('-'));
    if (idx > 0) {
        const QString tag = base.left(idx);
        if (looksLikeEmotionTag(tag))
            return base.mid(idx + 1).trimmed();
    }
    return base.trimmed();
}

int VkCharIndex::levenshtein(const QString& a, const QString& b)
{
    const int n = a.size(), m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    QVector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int cost = (a.at(i - 1) == b.at(j - 1)) ? 0 : 1;
            cur[j] = qMin(qMin(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev = cur;
    }
    return prev[m];
}

// ---------------------------------------------------------------------------
// 权重文件挑选
// ---------------------------------------------------------------------------

//! 通配符转义：角色名里理论上可能出现 [ ] * ?（目前没有，防御一下）
static QString wildcardEscape(const QString& s)
{
    QString o = s;
    o.replace(QLatin1Char('['), QStringLiteral("[[]"));
    o.replace(QLatin1Char(']'), QStringLiteral("[]]"));
    return o;
}

/*! 在 dir 里挑 SoVITS 权重：可能有多份（鬼方佳世子有 e4_s96 和 e8_s192），
 *  取 _s 数值最大的那份 —— 训练步数最多的权重质量最稳。 */
static QString bestSovitsIn(const QDir& dir, const QString& name)
{
    if (!dir.exists() || name.isEmpty())
        return QString();

    QStringList files = dir.entryList(QStringList{ wildcardEscape(name) + QStringLiteral("_e*_s*.pth") },
                                      QDir::Files, QDir::Name);
    if (files.isEmpty())
        files = dir.entryList(QStringList{ wildcardEscape(name) + QStringLiteral("*.pth") },
                              QDir::Files, QDir::Name);
    if (files.isEmpty())
        return QString();

    static const QRegularExpression sRe(QStringLiteral("_s(\\d+)"));
    int bestS = -1;
    QString best;
    for (const QString& f : files) {
        const QRegularExpressionMatch m = sRe.match(f);
        const int s = m.hasMatch() ? m.captured(1).toInt() : 0;
        if (s > bestS) {
            bestS = s;
            best = f;
        }
    }
    return dir.absoluteFilePath(best);
}

static QString bestGptIn(const QDir& dir, const QString& name)
{
    if (!dir.exists() || name.isEmpty())
        return QString();
    QStringList files = dir.entryList(QStringList{ wildcardEscape(name) + QStringLiteral("-e*.ckpt") },
                                      QDir::Files, QDir::Name);
    if (files.isEmpty())
        files = dir.entryList(QStringList{ wildcardEscape(name) + QStringLiteral("*.ckpt") },
                              QDir::Files, QDir::Name);
    if (files.isEmpty())
        return QString();
    return dir.absoluteFilePath(files.last());
}

// ---------------------------------------------------------------------------
// 索引构建
// ---------------------------------------------------------------------------

VkCharIndex::VkCharIndex(const VkConfig* cfg)
    : m_cfg(cfg)
{
}

QString VkCharIndex::cachePath() const
{
    if (!m_cfg)
        return QString();
    return m_cfg->cacheDir() + QStringLiteral("/charindex.json");
}

void VkCharIndex::clear()
{
    m_chars.clear();
    m_byName.clear();
    m_loaded = false;
    m_fromCache = false;
}

bool VkCharIndex::load(bool forceRescan, QString* err)
{
    m_loaded = false;
    m_fromCache = false;

    if (!m_cfg || m_cfg->modelRoot.isEmpty()) {
        if (err)
            *err = QStringLiteral("未配置模型库目录 (modelRoot)");
        return false;
    }

    if (!forceRescan && loadCache()) {
        m_loaded = true;
        m_fromCache = true;
        return true;
    }

    if (!scan()) {
        if (err)
            *err = QStringLiteral("扫描模型库失败: %1").arg(m_cfg->modelRoot);
        return false;
    }
    saveCache();
    m_loaded = true;
    return true;
}

void VkCharIndex::appendFromModelRoot()
{
    QDir root(m_cfg->modelRoot);
    const QFileInfoList games = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    static const QRegularExpression bracket(QStringLiteral("（[^）]*）"));

    for (const QFileInfo& g : games) {
        // "参考音频（AI分类的）" 是零样本素材池，不含微调权重，本批不纳入索引
        if (g.fileName().startsWith(QStringLiteral("参考音频")))
            continue;

        QString gameName = g.fileName();
        gameName.remove(bracket);   // 崩坏：星穹铁道（已更新2.4） -> 崩坏：星穹铁道

        const QDir gameDir(g.absoluteFilePath());
        const QFileInfoList chars = gameDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

        for (const QFileInfo& ci : chars) {
            CharInfo c;
            c.rawName = ci.fileName();
            c.name = normalizeName(c.rawName);
            c.game = gameName;
            c.dir = ci.absoluteFilePath();
            if (c.name.isEmpty())
                continue;

            const QDir cd(c.dir);

            // 参考音频：取目录里第一个 wav
            const QStringList wavs = cd.entryList(QStringList{ QStringLiteral("*.wav"),
                                                               QStringLiteral("*.WAV") },
                                                  QDir::Files, QDir::Name);
            if (!wavs.isEmpty()) {
                c.refAudio = cd.absoluteFilePath(wavs.first());
                c.promptText = extractPromptText(wavs.first());
            }

            // 权重：先在角色目录里找（原神/崩三/绝区零/蔚蓝档案都是这种）
            const QStringList ck = cd.entryList(QStringList{ QStringLiteral("*.ckpt") },
                                                QDir::Files, QDir::Name);
            if (!ck.isEmpty())
                c.gptCkpt = cd.absoluteFilePath(ck.first());
            c.sovitsPth = bestSovitsIn(cd, c.name);

            // 没参考音频也没权重 -> 不是可用音色，丢弃
            if (c.refAudio.isEmpty() && c.gptCkpt.isEmpty())
                continue;

            m_chars << c;
        }
    }
}

void VkCharIndex::appendFromEngineWeights()
{
    // 星穹铁道那种：权重拷在引擎目录，参考音频留在模型库
    const QDir gptDir(m_cfg->engineDir + QStringLiteral("/GPT_weights_v2"));
    const QDir sovDir(m_cfg->engineDir + QStringLiteral("/SoVITS_weights_v2"));

    for (CharInfo& c : m_chars) {
        if (c.gptCkpt.isEmpty()) {
            QString p = bestGptIn(gptDir, c.name);
            if (p.isEmpty() && c.rawName != c.name)
                p = bestGptIn(gptDir, c.rawName);
            c.gptCkpt = p;
        }
        if (c.sovitsPth.isEmpty()) {
            QString p = bestSovitsIn(sovDir, c.name);
            if (p.isEmpty() && c.rawName != c.name)
                p = bestSovitsIn(sovDir, c.rawName);
            c.sovitsPth = p;
        }
    }
}

bool VkCharIndex::scan()
{
    m_chars.clear();
    m_byName.clear();

    if (!QFileInfo(m_cfg->modelRoot).isDir())
        return false;

    appendFromModelRoot();
    appendFromEngineWeights();

    for (int i = 0; i < m_chars.size(); ++i) {
        if (!m_byName.contains(m_chars.at(i).name))
            m_byName.insert(m_chars.at(i).name, i);
    }
    return !m_chars.isEmpty();
}

QStringList VkCharIndex::games() const
{
    QStringList g;
    for (const CharInfo& c : m_chars) {
        if (!g.contains(c.game))
            g << c.game;
    }
    g.sort();
    return g;
}

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------

QString VkCharIndex::signature() const
{
    QStringList parts;
    QDir root(m_cfg->modelRoot);
    const QFileInfoList games = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : games)
        parts << QStringLiteral("%1:%2").arg(fi.fileName())
                     .arg(fi.lastModified().toSecsSinceEpoch());

    for (const QString& w : { QStringLiteral("GPT_weights_v2"), QStringLiteral("SoVITS_weights_v2") }) {
        const QFileInfo fi(m_cfg->engineDir + QLatin1Char('/') + w);
        parts << QStringLiteral("%1:%2").arg(w).arg(fi.lastModified().toSecsSinceEpoch());
    }
    return QString::fromLatin1(QCryptographicHash::hash(parts.join(QLatin1Char('|')).toUtf8(),
                                                       QCryptographicHash::Md5).toHex());
}

bool VkCharIndex::loadCache()
{
    const QString path = cachePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return false;

    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("version")).toString() != QLatin1String(kIndexVersion))
        return false;
    if (o.value(QStringLiteral("modelRoot")).toString() != m_cfg->modelRoot)
        return false;
    if (o.value(QStringLiteral("signature")).toString() != signature())
        return false;

    const QJsonArray arr = o.value(QStringLiteral("chars")).toArray();
    if (arr.isEmpty())
        return false;

    QList<CharInfo> chars;
    chars.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject co = v.toObject();
        CharInfo c;
        c.name       = co.value(QStringLiteral("name")).toString();
        c.rawName    = co.value(QStringLiteral("rawName")).toString();
        c.game       = co.value(QStringLiteral("game")).toString();
        c.dir        = co.value(QStringLiteral("dir")).toString();
        c.refAudio   = co.value(QStringLiteral("refAudio")).toString();
        c.promptText = co.value(QStringLiteral("promptText")).toString();
        c.gptCkpt    = co.value(QStringLiteral("gptCkpt")).toString();
        c.sovitsPth  = co.value(QStringLiteral("sovitsPth")).toString();

        if (c.name.isEmpty())
            continue;
        // 参考音频是合成的硬前提，文件没了就必须重扫
        if (c.refAudio.isEmpty() || !QFileInfo::exists(c.refAudio))
            return false;
        chars << c;
    }
    if (chars.isEmpty())
        return false;

    m_chars = chars;
    m_byName.clear();
    for (int i = 0; i < m_chars.size(); ++i) {
        if (!m_byName.contains(m_chars.at(i).name))
            m_byName.insert(m_chars.at(i).name, i);
    }
    return true;
}

bool VkCharIndex::saveCache() const
{
    const QString path = cachePath();
    if (path.isEmpty())
        return false;

    QJsonArray arr;
    for (const CharInfo& c : m_chars) {
        QJsonObject co;
        co.insert(QStringLiteral("name"), c.name);
        co.insert(QStringLiteral("rawName"), c.rawName);
        co.insert(QStringLiteral("game"), c.game);
        co.insert(QStringLiteral("dir"), c.dir);
        co.insert(QStringLiteral("refAudio"), c.refAudio);
        co.insert(QStringLiteral("promptText"), c.promptText);
        co.insert(QStringLiteral("gptCkpt"), c.gptCkpt);
        co.insert(QStringLiteral("sovitsPth"), c.sovitsPth);
        arr.append(co);
    }

    QJsonObject o;
    o.insert(QStringLiteral("version"), QLatin1String(kIndexVersion));
    o.insert(QStringLiteral("modelRoot"), m_cfg->modelRoot);
    o.insert(QStringLiteral("signature"), signature());
    o.insert(QStringLiteral("scannedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));
    o.insert(QStringLiteral("count"), m_chars.size());
    o.insert(QStringLiteral("chars"), arr);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// 查找
// ---------------------------------------------------------------------------

CharInfo VkCharIndex::find(const QString& name, QString* diag) const
{
    const QString key = name.trimmed();
    if (key.isEmpty()) {
        if (diag)
            *diag = QStringLiteral("角色名为空");
        return CharInfo();
    }

    // 1) 精确
    QList<int> exact;
    for (int i = 0; i < m_chars.size(); ++i) {
        if (m_chars.at(i).name == key)
            exact << i;
    }
    if (!exact.isEmpty()) {
        if (exact.size() > 1 && diag)
            *diag = QStringLiteral("有 %1 个同名角色，取第一个（%2）")
                        .arg(exact.size()).arg(m_chars.at(exact.first()).game);
        return m_chars.at(exact.first());
    }

    // 2) 别名表
    if (m_cfg && m_cfg->aliases.contains(key)) {
        const QString target = m_cfg->aliases.value(key);
        for (const CharInfo& c : m_chars) {
            if (c.name == target)
                return c;
        }
    }

    // 3) 包含关系
    QList<int> contains;
    for (int i = 0; i < m_chars.size(); ++i) {
        const QString n = m_chars.at(i).name;
        if (n.size() >= 2 && key.size() >= 2 && (n.contains(key) || key.contains(n)))
            contains << i;
    }
    if (contains.size() == 1)
        return m_chars.at(contains.first());
    if (contains.size() > 1) {
        int best = -1;
        int bestDiff = INT_MAX;
        for (int i : contains) {
            const int d = qAbs(m_chars.at(i).name.size() - key.size());
            if (d < bestDiff) {
                bestDiff = d;
                best = i;
            }
        }
        if (best >= 0) {
            if (diag)
                *diag = QStringLiteral("⚠ 模糊匹配命中 %1 个候选，取的「%2」——请确认")
                            .arg(contains.size()).arg(m_chars.at(best).name);
            return m_chars.at(best);
        }
    }

    // 4) 编辑距离 1 以内（两字名太容易撞，放开到 3 字以上才用）
    if (key.size() >= 3) {
        int best = -1;
        int bestD = INT_MAX;
        int tie = 0;
        for (int i = 0; i < m_chars.size(); ++i) {
            const int d = levenshtein(key, m_chars.at(i).name);
            if (d < bestD) {
                bestD = d;
                best = i;
                tie = 1;
            } else if (d == bestD) {
                ++tie;
            }
        }
        if (best >= 0 && bestD <= 1 && tie == 1) {
            if (diag)
                *diag = QStringLiteral("⚠ 没有「%1」，按编辑距离认成了「%2」")
                            .arg(key, m_chars.at(best).name);
            return m_chars.at(best);
        }
    }

    if (diag)
        *diag = QStringLiteral("索引里没有「%1」这个角色（共 %2 个）").arg(key).arg(m_chars.size());
    return CharInfo();
}

QList<CharInfo> VkCharIndex::search(const QString& keyword, int limit) const
{
    const QString key = keyword.trimmed();
    QList<CharInfo> out;
    for (const CharInfo& c : m_chars) {
        if (key.isEmpty() || c.name.contains(key) || c.game.contains(key)
            || c.rawName.contains(key)) {
            out << c;
            if (out.size() >= limit)
                break;
        }
    }
    return out;
}

} // namespace vk
