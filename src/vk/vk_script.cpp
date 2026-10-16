#include "vk_script.h"
#include "vk_charindex.h"
#include "vk_config.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

namespace vk {

QString speakerSourceName(SpeakerSource s)
{
    switch (s) {
    case SpeakerSource::Rule:    return QStringLiteral("标注");
    case SpeakerSource::Context: return QStringLiteral("推断");
    case SpeakerSource::Llm:     return QStringLiteral("LLM");
    case SpeakerSource::User:    return QStringLiteral("人工");
    default:                     return QStringLiteral("无");
    }
}

// ---------------------------------------------------------------------------
// 第 1 步：规范化 + 切行
// ---------------------------------------------------------------------------
QStringList ScriptParser::normalizeAndSplit(const QString& raw)
{
    QString s = raw;
    if (s.startsWith(QChar(0xFEFF)))
        s.remove(0, 1);                       // 去 BOM（记事本存的 UTF-8 常带）
    s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList out;
    const QStringList raw_lines = s.split(QLatin1Char('\n'));
    for (const QString& l : raw_lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 第 2 步：说话人抽取
// ---------------------------------------------------------------------------

//! 旁白类的"说话人"——不算角色，不进合成
static bool isNarrationWord(const QString& w)
{
    static const QStringList kWords{
        QStringLiteral("旁白"), QStringLiteral("画外音"), QStringLiteral("独白"),
        QStringLiteral("内心"), QStringLiteral("内心独白"), QStringLiteral("提示"),
        QStringLiteral("场景"), QStringLiteral("镜头"), QStringLiteral("备注"),
        QStringLiteral("说明"), QStringLiteral("os"), QStringLiteral("OS"),
        QStringLiteral("音效"), QStringLiteral("字幕")
    };
    return kWords.contains(w.trimmed());
}

//! 情绪标签判据：短、纯汉字、无标点
static bool looksLikeEmotion(const QString& t)
{
    const QString s = t.trimmed();
    if (s.isEmpty() || s.size() > 6)
        return false;
    static const QRegularExpression punct(
        QStringLiteral("[，。！？、,.!?;:：；\"'`「」『』（）()\\[\\]<>《》…—～~]"));
    if (punct.match(s).hasMatch())
        return false;
    bool hasHan = false;
    for (const QChar& c : s) {
        if (c.script() == QChar::Script_Han)
            hasHan = true;
        else if (c.isLetterOrNumber())
            return false;
    }
    return hasHan;
}

//! 说话人名判据：短、不含句末标点和引号括号
static bool looksLikeSpeakerName(const QString& t)
{
    const QString s = t.trimmed();
    if (s.isEmpty() || s.size() > 12)
        return false;
    static const QRegularExpression bad(
        QStringLiteral("[。！？；，、\"'`「」『』（）()\\[\\]\\[\\]…—～~]"));
    return !bad.match(s).hasMatch();
}

bool ScriptParser::splitSpeaker(const QString& line, QString* speaker, QString* emotion,
                               QString* text, QString* stageNote, bool* narration)
{
    if (speaker)   speaker->clear();
    if (emotion)   emotion->clear();
    if (text)      text->clear();
    if (stageNote) stageNote->clear();
    if (narration) *narration = false;

    const QString s = line.trimmed();
    if (s.isEmpty())
        return false;

    // ── 模式 1：【X】rest  或  [X] rest ──────────────────────
    static const QRegularExpression bracketRe(
        QStringLiteral("^[【\\[]([^】\\]]{1,12})[】\\]]\\s*(.*)$"));
    QRegularExpressionMatch m = bracketRe.match(s);
    if (m.hasMatch()) {
        const QString who = m.captured(1).trimmed();
        const QString rest = m.captured(2).trimmed();
        if (isNarrationWord(who)) {
            if (narration) *narration = true;
            if (speaker)   *speaker = who;
            if (text)      *text = rest.isEmpty() ? who : rest;
            if (stageNote) *stageNote = who;
            return true;
        }
        if (looksLikeSpeakerName(who) && !rest.isEmpty()) {
            if (speaker) *speaker = who;
            if (text)    *text = rest;
            return true;
        }
        // 【转身离开】 这种没有台词的纯提示
        if (rest.isEmpty()) {
            if (narration) *narration = true;
            if (stageNote) *stageNote = who;
            if (text)      *text = who;
            return true;
        }
    }

    // ── 模式 2：X（情绪）：rest  /  X：rest ──────────────────
    // 逐个试探第一个冒号，且要求冒号前面像个名字
    const int colonFull = s.indexOf(QChar(0xFF1A));   // ：
    const int colonHalf = s.indexOf(QLatin1Char(':'));
    int colon = -1;
    if (colonFull >= 0 && colonHalf >= 0)
        colon = qMin(colonFull, colonHalf);
    else
        colon = qMax(colonFull, colonHalf);

    if (colon > 0 && colon <= 14) {
        QString head = s.left(colon).trimmed();
        const QString rest = s.mid(colon + 1).trimmed();

        // 剥出 head 末尾的（情绪）
        QString emo;
        static const QRegularExpression emoAtEnd(
            QStringLiteral("[（(]\\s*([^）)]{1,8})\\s*[）)]\\s*$"));
        const QRegularExpressionMatch em = emoAtEnd.match(head);
        if (em.hasMatch()) {
            emo = em.captured(1).trimmed();
            head = head.left(em.capturedStart()).trimmed();
        }

        if (!rest.isEmpty() && looksLikeSpeakerName(head)) {
            if (isNarrationWord(head)) {
                if (narration) *narration = true;
                if (speaker)   *speaker = head;
                if (text)      *text = rest;
                if (stageNote) *stageNote = head;
                return true;
            }
            if (speaker) *speaker = head;
            if (emotion && looksLikeEmotion(emo)) *emotion = emo;
            QString body = rest;
            // 台词开头的（情绪）：`甲：（叹气）xxx` 这种写法太常见了。
            // 不剥掉的话"叹气"两个字会被念出来（实测踩过）。
            // 只在括号里那词确实像情绪时才剥，"（街边）"这种留着。
            if (body.startsWith(QStringLiteral("（")) || body.startsWith(QStringLiteral("("))) {
                static const QRegularExpression leadEmo(
                    QStringLiteral("^[（(]\\s*([^）)]{1,8})\\s*[）)]\\s*"));
                const QRegularExpressionMatch lm = leadEmo.match(body);
                if (lm.hasMatch() && looksLikeEmotion(lm.captured(1).trimmed())) {
                    if (emotion && emotion->isEmpty())
                        *emotion = lm.captured(1).trimmed();
                    body = body.mid(lm.capturedEnd()).trimmed();
                }
            }
            if (text)    *text = body;
            return true;
        }
    }

    // ── 模式 3：X「rest」 / X“rest” ────────────────────────
    static const QRegularExpression quoteRe(
        QStringLiteral("^([^「『\"“]{1,12})\\s*[「『\"“]\\s*(.*?)\\s*[」』\"”]\\s*$"));
    m = quoteRe.match(s);
    if (m.hasMatch()) {
        const QString who = m.captured(1).trimmed();
        const QString rest = m.captured(2).trimmed();
        if (looksLikeSpeakerName(who) && !rest.isEmpty()) {
            if (speaker) *speaker = who;
            if (text)    *text = rest;
            return true;
        }
    }

    // ── 模式 4：整行就是（舞台提示） ─────────────────────────
    static const QRegularExpression wholeStage(
        QStringLiteral("^[（(]([^）)]{1,20})[）)]$"));
    m = wholeStage.match(s);
    if (m.hasMatch()) {
        if (narration) *narration = true;
        if (stageNote) *stageNote = m.captured(1).trimmed();
        if (text)      *text = m.captured(1).trimmed();
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// 第 4 步：情绪 → 合成参数
// ---------------------------------------------------------------------------
struct EmoEntry { const char* key; float speed; float temp; };

static const EmoEntry kEmoTable[] = {
    { "平静",     1.00f, 1.00f }, { "正常",     1.00f, 1.00f },
    { "冷静",     0.98f, 0.95f }, { "冷淡",     0.98f, 0.95f },
    { "激动",     1.06f, 1.05f }, { "兴奋",     1.06f, 1.06f },
    { "高兴",     1.05f, 1.04f }, { "开心",     1.05f, 1.04f },
    { "快乐",     1.05f, 1.04f }, { "大笑",     1.06f, 1.06f },
    { "轻笑",     1.02f, 1.02f }, { "冷笑",     0.98f, 1.00f },
    { "笑",       1.04f, 1.05f }, { "愤怒",     1.04f, 1.08f },
    { "生气",     1.04f, 1.08f }, { "大喊",     1.08f, 1.06f },
    { "大叫",     1.08f, 1.06f }, { "喊",       1.07f, 1.05f },
    { "无奈",     0.97f, 0.98f }, { "叹气",     0.94f, 0.95f },
    { "难过",     0.95f, 0.95f }, { "悲伤",     0.94f, 0.94f },
    { "低落",     0.95f, 0.95f }, { "委屈",     0.96f, 0.96f },
    { "哭",       0.93f, 0.96f }, { "疑惑",     1.00f, 1.00f },
    { "迟疑",     0.97f, 0.98f }, { "惊讶",     1.02f, 1.02f },
    { "害怕",     0.98f, 1.00f }, { "恐惧",     0.97f, 1.02f },
    { "紧张",     1.02f, 1.03f }, { "温柔",     0.96f, 0.95f },
    { "耳语",     0.95f, 0.94f }, { "小声",     0.96f, 0.94f },
    { "戏谑",     1.02f, 1.02f }, { "嘲讽",     1.01f, 1.03f },
    { "认真",     0.99f, 0.97f }, { "严肃",     0.98f, 0.96f },
    { "疲惫",     0.93f, 0.95f }, { "慵懒",     0.94f, 0.96f },
};

VoiceParams ScriptParser::paramsForEmotion(const QString& emotion)
{
    VoiceParams p;
    const QString e = emotion.trimmed();
    if (e.isEmpty())
        return p;
    for (const EmoEntry& en : kEmoTable) {
        const QString key = QString::fromUtf8(en.key);
        if (e == key || e.contains(key)) {
            p.speed = en.speed;
            p.temperature = en.temp;
            return p;
        }
    }
    return p;   // 认不出的情绪就按默认参数，别乱调
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
ScriptParser::ScriptParser(const VkCharIndex* index, const VkConfig* cfg)
    : m_index(index)
    , m_cfg(cfg)
    , m_nam(new QNetworkAccessManager(nullptr))   // ScriptParser 不是 QObject，不能当 parent
{
}

ScriptParser::~ScriptParser()
{
    delete m_nam;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
ScriptParseResult ScriptParser::parseText(const QString& raw, const ScriptParseOptions& opt)
{
    ScriptParseResult r;
    const QStringList lines = normalizeAndSplit(raw);

    for (const QString& line : lines) {
        ScriptLine sl;
        sl.index = r.lines.size();
        QString sp, emo, text, note;
        bool narration = false;
        if (splitSpeaker(line, &sp, &emo, &text, &note, &narration)) {
            sl.speaker = sp;
            sl.emotion = emo;
            sl.text = text;
            sl.stageNote = note;
            sl.isNarration = narration;
            sl.source = (sp.isEmpty() || narration) ? SpeakerSource::None : SpeakerSource::Rule;
        } else {
            sl.text = line;
            sl.source = SpeakerSource::None;
        }
        // 旁白行也收进表里；计数在下面统一做，输出时按 includeNarration 过滤
        r.lines << sl;
    }

    // 第 3 步：角色归一化（含 L1 结果）
    resolveCharacters(r, opt);

    // L2：上下文交替推断
    if (opt.useContext)
        inferByContext(r, opt);

    // L3：LLM 兜底（只补 L1/L2 都没搞定的行）
    if (opt.useLlm) {
        QStringList warn;
        if (inferByLlm(r, opt, &warn))
            resolveCharacters(r, opt);
        r.warnings << warn;
    }

    // 统计
    r.total = r.lines.size();
    for (const ScriptLine& l : r.lines) {
        if (l.isNarration) {
            ++r.narration;
        } else if (l.resolved) {
            ++r.matched;
        } else {
            ++r.unresolved;
        }
    }
    return r;
}

ScriptParseResult ScriptParser::parseFile(const QString& path, const ScriptParseOptions& opt,
                                          QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("打不开剧本文件: %1 (%2)").arg(path, f.errorString());
        return ScriptParseResult();
    }
    const QByteArray data = f.readAll();
    // 尽力猜编码：UTF-8 优先，失败按本地 GBK
    QString text = QString::fromUtf8(data);
    if (text.contains(QChar(0xFFFD)))
        text = QString::fromLocal8Bit(data);
    return parseText(text, opt);
}

// ---------------------------------------------------------------------------
// 角色归一化
// ---------------------------------------------------------------------------
void ScriptParser::resolveCharacters(ScriptParseResult& r, const ScriptParseOptions& opt) const
{
    if (!m_index)
        return;

    // 兜底角色可以给多个（逗号分隔）。多人对话就靠它：
    // 未匹配的说话人（甲/乙/路人A）按"首次出现顺序"依次分到这些角色上，
    // 同一个说话人整段剧本固定用同一个音色 —— 这比每句随机换人有意义得多。
    QStringList defaults = opt.defaultChar.split(
        QRegularExpression(QStringLiteral("[,，]")), QString::SkipEmptyParts);
    for (QString& s : defaults)
        s = s.trimmed();
    QList<QPair<QString, QString>> assignLog;   // 保持首次出现顺序
    int nextIdx = 0;

    for (ScriptLine& l : r.lines) {
        if (l.isNarration || l.resolved || l.speaker.isEmpty())
            continue;
        QString diag;
        const CharInfo c = m_index->find(l.speaker, &diag);
        if (c.isValid()) {
            l.charName = c.name;
            l.resolved = true;
            l.matchNote = diag;
            // 有 diag = 走了别名/模糊/歧义，属于"我猜的"，UI 要标黄
            if (!diag.isEmpty())
                l.lowConfidence = true;
        } else {
            if (!defaults.isEmpty()) {
                // 认不出说话人但有兜底角色 —— 按说话人固定分配，标黄提醒
                QString assigned;
                for (const auto& p : assignLog) {
                    if (p.first == l.speaker) {
                        assigned = p.second;
                        break;
                    }
                }
                if (assigned.isEmpty()) {
                    assigned = defaults.at(nextIdx % defaults.size());
                    ++nextIdx;
                    assignLog << qMakePair(l.speaker, assigned);
                }
                l.charName = assigned;
                l.resolved = true;
                l.lowConfidence = true;
                l.source = SpeakerSource::User;
                l.matchNote = diag.isEmpty()
                                  ? QStringLiteral("未匹配，分到「%1」").arg(assigned)
                                  : QStringLiteral("%1；未匹配，分到「%2」").arg(diag, assigned);
            } else {
                l.resolved = false;
                l.matchNote = diag;
            }
        }
    }

    if (assignLog.size() > 1) {
        QStringList pairs;
        for (const auto& p : assignLog)
            pairs << QStringLiteral("%1→%2").arg(p.first, p.second);
        r.warnings << QStringLiteral("多人分配：%1").arg(pairs.join(QStringLiteral("、")));
    }
}

// ---------------------------------------------------------------------------
// L2：上下文交替推断
// ---------------------------------------------------------------------------
void ScriptParser::inferByContext(ScriptParseResult& r, const ScriptParseOptions& opt) const
{
    QString prevA;      // 较早的说话人
    QString prevB;      // 最近的说话人
    int chain = 0;

    for (ScriptLine& l : r.lines) {
        if (l.isNarration)
            continue;

        if (!l.speaker.isEmpty()) {
            if (l.speaker != prevB) {
                prevA = prevB;
                prevB = l.speaker;
            }
            chain = 0;
            continue;
        }

        // 未标注：只有"前文恰好两个人"时才敢按交替推断
        if (prevA.isEmpty() || prevB.isEmpty() || prevA == prevB) {
            chain = 0;
            continue;
        }
        if (chain >= opt.contextChainLimit)
            continue;

        // 刚说完话的是 prevB，接话的应该是 prevA
        const QString guess = prevA;
        l.speaker = guess;
        l.source = SpeakerSource::Context;
        l.lowConfidence = true;          // 推断的都要标黄，别装成确定
        ++chain;
        ++r.contextInferred;

        if (guess != prevB) {
            prevA = prevB;
            prevB = guess;
        }

        if (m_index) {
            QString diag;
            const CharInfo c = m_index->find(guess, &diag);
            if (c.isValid()) {
                l.charName = c.name;
                l.resolved = true;
                l.matchNote = diag;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// L3：LLM 兜底
// ---------------------------------------------------------------------------
QString ScriptParser::readLlmKey(const QString& explicitKey)
{
    if (!explicitKey.trimmed().isEmpty())
        return explicitKey.trimmed();

    const QByteArray env = qgetenv("DEEPSEEK_API_KEY");
    if (!env.isEmpty())
        return QString::fromUtf8(env).trimmed();

    const QStringList candidates{
        QDir::homePath() + QStringLiteral("/.deepseek_key"),
        QDir::homePath() + QStringLiteral("/.config/deepseek_key"),
        QDir::homePath() + QStringLiteral("/.deepseek"),
    };
    for (const QString& p : candidates) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString k = QString::fromUtf8(f.readAll()).trimmed().split(QRegularExpression(
            QStringLiteral("[\\r\\n]"))).first().trimmed();
        if (!k.isEmpty())
            return k;
    }
    return QString();
}

bool ScriptParser::inferByLlm(ScriptParseResult& r, const ScriptParseOptions& opt,
                              QStringList* warn)
{
    // 收集待识别行
    QList<int> pending;
    for (int i = 0; i < r.lines.size(); ++i) {
        const ScriptLine& l = r.lines.at(i);
        if (!l.isNarration && l.speaker.isEmpty())
            pending << i;
    }
    if (pending.isEmpty())
        return false;

    if (pending.size() > opt.llmMaxLines) {
        if (warn)
            *warn << QStringLiteral("待识别行 %1 条，超过上限 %2，跳过 LLM 兜底（防烧钱）")
                         .arg(pending.size()).arg(opt.llmMaxLines);
        return false;
    }

    const QString key = readLlmKey(opt.llmApiKey);
    if (key.isEmpty()) {
        if (warn)
            *warn << QStringLiteral("没找到 DeepSeek API Key（环境变量 DEEPSEEK_API_KEY 或 ~/.deepseek_key），跳过 LLM 兜底");
        return false;
    }

    // 候选角色名（全部 354 个塞进去也就 1~2 KB，比让模型自由发挥强）
    QStringList cand;
    if (m_index) {
        for (const CharInfo& c : m_index->all())
            cand << c.name;
        cand.removeDuplicates();
    }

    QString script;
    for (int i : pending) {
        const ScriptLine& l = r.lines.at(i);
        script += QStringLiteral("%1: %2\n").arg(i).arg(l.text);
    }

    const QString sys =
        QStringLiteral("你是剧本角色识别器。给每行台词判断说话人。\n"
                       "规则：只能从【候选角色】里选；判不出来就填 \"unknown\"。\n"
                       "只输出 JSON，不要解释，格式：{\"assignments\":[{\"i\":0,\"speaker\":\"钟离\"}]}");
    const QString user =
        QStringLiteral("【候选角色】%1\n\n【剧本】\n%2").arg(cand.join(QStringLiteral(", ")), script);

    QJsonObject msgSys{ { QStringLiteral("role"), QStringLiteral("system") },
                        { QStringLiteral("content"), sys } };
    QJsonObject msgUser{ { QStringLiteral("role"), QStringLiteral("user") },
                         { QStringLiteral("content"), user } };
    QJsonObject body;
    body.insert(QStringLiteral("model"), opt.llmModel);
    body.insert(QStringLiteral("messages"), QJsonArray{ msgSys, msgUser });
    body.insert(QStringLiteral("temperature"), 0.1);
    body.insert(QStringLiteral("response_format"),
                QJsonObject{ { QStringLiteral("type"), QStringLiteral("json_object") } });

    QUrl url(opt.llmBaseUrl + QStringLiteral("/chat/completions"));
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=utf-8"));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + key.toUtf8());

    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(opt.llmTimeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        if (warn)
            *warn << QStringLiteral("LLM 请求超时 (%1 ms)").arg(opt.llmTimeoutMs);
        return false;
    }
    timer.stop();

    const QByteArray data = reply->readAll();
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString netErr = reply->errorString();
    const bool ok = (reply->error() == QNetworkReply::NoError);
    reply->deleteLater();

    if (!ok || http >= 400) {
        if (warn)
            *warn << QStringLiteral("LLM 调用失败 (HTTP %1): %2").arg(http).arg(netErr);
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonObject obj = doc.object();
    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (warn)
            *warn << QStringLiteral("LLM 返回里没有 choices");
        return false;
    }
    const QString content = choices.first().toObject()
                                .value(QStringLiteral("message")).toObject()
                                .value(QStringLiteral("content")).toString();

    const QJsonDocument inner = QJsonDocument::fromJson(content.toUtf8());
    const QJsonArray assigns = inner.object().value(QStringLiteral("assignments")).toArray();
    if (assigns.isEmpty()) {
        if (warn)
            *warn << QStringLiteral("LLM 返回的 JSON 里没有 assignments");
        return false;
    }

    int applied = 0;
    for (const QJsonValue& v : assigns) {
        const QJsonObject o = v.toObject();
        const int i = o.value(QStringLiteral("i")).toInt(-1);
        const QString who = o.value(QStringLiteral("speaker")).toString().trimmed();
        if (i < 0 || i >= r.lines.size() || who.isEmpty()
            || who.compare(QStringLiteral("unknown"), Qt::CaseInsensitive) == 0)
            continue;
        ScriptLine& l = r.lines[i];
        if (!l.speaker.isEmpty())
            continue;
        l.speaker = who;
        l.source = SpeakerSource::Llm;
        l.lowConfidence = true;
        ++applied;
        ++r.llmInferred;
    }
    if (applied == 0 && warn)
        *warn << QStringLiteral("LLM 没能给出可用的分配结果");
    return applied > 0;
}

// ---------------------------------------------------------------------------
// 任务表 JSON
// ---------------------------------------------------------------------------
QJsonObject ScriptParser::toJson(const ScriptParseResult& r, const QString& source)
{
    QJsonArray arr;
    for (const ScriptLine& l : r.lines) {
        QJsonObject o;
        o.insert(QStringLiteral("i"), l.index);
        o.insert(QStringLiteral("speaker"), l.speaker);
        o.insert(QStringLiteral("emotion"), l.emotion);
        o.insert(QStringLiteral("text"), l.text);
        o.insert(QStringLiteral("stageNote"), l.stageNote);
        o.insert(QStringLiteral("narration"), l.isNarration);
        o.insert(QStringLiteral("char"), l.charName);
        o.insert(QStringLiteral("resolved"), l.resolved);
        o.insert(QStringLiteral("lowConfidence"), l.lowConfidence);
        o.insert(QStringLiteral("source"), speakerSourceName(l.source));
        o.insert(QStringLiteral("matchNote"), l.matchNote);
        o.insert(QStringLiteral("audio"), l.audioFile);
        o.insert(QStringLiteral("durationMs"), double(l.durationMs));
        o.insert(QStringLiteral("packId"), l.packId);
        arr.append(o);
    }

    QJsonObject stats;
    stats.insert(QStringLiteral("total"), r.total);
    stats.insert(QStringLiteral("matched"), r.matched);
    stats.insert(QStringLiteral("unresolved"), r.unresolved);
    stats.insert(QStringLiteral("narration"), r.narration);
    stats.insert(QStringLiteral("contextInferred"), r.contextInferred);
    stats.insert(QStringLiteral("llmInferred"), r.llmInferred);

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("source"), source);
    root.insert(QStringLiteral("stats"), stats);
    root.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(r.warnings));
    root.insert(QStringLiteral("lines"), arr);
    return root;
}

ScriptParseResult ScriptParser::fromJson(const QJsonObject& o, QString* err)
{
    ScriptParseResult r;
    if (o.value(QStringLiteral("lines")).toArray().isEmpty()) {
        if (err)
            *err = QStringLiteral("任务表里没有 lines 字段");
        return r;
    }
    const QJsonArray arr = o.value(QStringLiteral("lines")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o2 = v.toObject();
        ScriptLine l;
        l.index        = o2.value(QStringLiteral("i")).toInt();
        l.speaker      = o2.value(QStringLiteral("speaker")).toString();
        l.emotion      = o2.value(QStringLiteral("emotion")).toString();
        l.text         = o2.value(QStringLiteral("text")).toString();
        l.stageNote    = o2.value(QStringLiteral("stageNote")).toString();
        l.isNarration  = o2.value(QStringLiteral("narration")).toBool();
        l.charName     = o2.value(QStringLiteral("char")).toString();
        l.resolved     = o2.value(QStringLiteral("resolved")).toBool();
        l.lowConfidence = o2.value(QStringLiteral("lowConfidence")).toBool();
        l.matchNote    = o2.value(QStringLiteral("matchNote")).toString();
        l.audioFile    = o2.value(QStringLiteral("audio")).toString();
        l.durationMs   = qint64(o2.value(QStringLiteral("durationMs")).toDouble());
        l.packId       = o2.value(QStringLiteral("packId")).toString();
        r.lines << l;
    }
    r.total = r.lines.size();
    for (const ScriptLine& l : r.lines) {
        if (l.isNarration)      ++r.narration;
        else if (l.resolved)    ++r.matched;
        else                    ++r.unresolved;
    }
    const QJsonArray w = o.value(QStringLiteral("warnings")).toArray();
    for (const QJsonValue& v : w)
        r.warnings << v.toString();
    return r;
}

} // namespace vk
