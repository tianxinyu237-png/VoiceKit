#ifndef VK_SCRIPT_H
#define VK_SCRIPT_H

#include "vk_types.h"

#include <QString>
#include <QStringList>
#include <QList>
#include <QJsonObject>

class QNetworkAccessManager;

namespace vk {

struct VkConfig;
class VkCharIndex;

//! 说话人是怎么定出来的 —— 用于告诉用户"这条是我猜的"
enum class SpeakerSource {
    None = 0,   //!< 原文里就没有
    Rule,       //!< 显式标注（钟离：xxx）
    Context,    //!< 上下文交替推断（低置信度）
    Llm,        //!< LLM 推断（低置信度）
    User        //!< 人工指定
};

QString speakerSourceName(SpeakerSource s);

//! 任务表里的一行 = 一句台词
struct ScriptLine {
    int     index = 0;
    QString speaker;        //!< 原文里的说话人写法（可能是别名/错字）
    QString emotion;        //!< 情绪标签（无则空）
    QString text;           //!< 台词正文
    QString stageNote;      //!< 剥出来的舞台提示，如"转身"
    bool    isNarration = false;   //!< 旁白/场景说明，默认不合成

    QString charName;       //!< 匹配到的角色规范名（空 = 没匹配上）
    bool    resolved = false;
    bool    lowConfidence = false; //!< true 时 UI 要标黄让人确认
    SpeakerSource source = SpeakerSource::None;
    QString matchNote;      //!< 匹配过程说明（歧义、模糊命中…）

    QString audioFile;      //!< 合成后的 wav 路径
    qint64  durationMs = 0;
    QString packId;         //!< 进语音包后的 ID（v0001）
};

struct ScriptParseOptions {
    bool    useContext = true;    //!< L2：无标注时按 A/B 交替推断
    bool    useLlm = false;       //!< L3：规则和上下文都失败才问 LLM
    QString llmApiKey;            //!< 空则读环境变量 DEEPSEEK_API_KEY / ~/.deepseek_key
    QString llmBaseUrl = QStringLiteral("https://api.deepseek.com");
    QString llmModel = QStringLiteral("deepseek-chat");
    int     llmTimeoutMs = 90000;
    int     llmMaxLines = 300;    //!< 待识别行超过这个数就不问 LLM 了（防烧钱）
    int     contextChainLimit = 6; //!< 连续无标注行最多推断几句，防跑偏
    bool    includeNarration = false; //!< 输出表里是否保留旁白行
    /*! 没匹配上角色的行，用这个角色兜底（空 = 不兜底，保持"未匹配"）。
     *  用途很实际：剧本里写的是"甲/乙/路人A"这种代称，索引里根本没有这号人，
     *  用户其实只想让某个音色把整段念完。有兜底至少能出声，
     *  而且标注会被剥掉（不会把"甲："也念出来），同时仍标 lowConfidence 提醒。 */
    QString defaultChar;
};

struct ScriptParseResult {
    QList<ScriptLine> lines;
    QStringList warnings;
    int total = 0;
    int matched = 0;
    int unresolved = 0;
    int narration = 0;
    int contextInferred = 0;
    int llmInferred = 0;

    bool isOk() const { return !lines.isEmpty(); }
};

/*!
 * \brief 剧本 → 配音任务表。五步流水线：
 *   1 规范化（去 BOM、统一换行、剥舞台提示）
 *   2 说话人抽取：L1 显式标注 → L2 上下文交替 → L3 LLM 兜底
 *   3 角色名归一化：精确 → 别名 → 包含 → 编辑距离（**多候选不静默猜**，标黄）
 *   4 情绪标注：从括号提取并映射成合成参数
 *   5 产出任务表（JSON，可人工改后再合成）
 *
 * 设计原则：规则优先、LLM 只兜底；匹配不上宁可标黄，也不猜一个最像的 ——
 * 猜错音色比标黄恶心一百倍，而且你还找不出原因。
 */
class ScriptParser
{
public:
    ScriptParser(const VkCharIndex* index, const VkConfig* cfg);
    ~ScriptParser();

    ScriptParseResult parseText(const QString& raw, const ScriptParseOptions& opt = ScriptParseOptions());
    ScriptParseResult parseFile(const QString& path, const ScriptParseOptions& opt = ScriptParseOptions(),
                                QString* err = nullptr);

    // ── 单测友好的静态工具 ────────────────────────────────
    //! 规范化 + 切行（去掉纯空白行）
    static QStringList normalizeAndSplit(const QString& raw);
    /*! 从一行里拆出 说话人 / 情绪 / 台词。
     *  支持：钟离：xxx   钟离: xxx   【钟离】xxx   [钟离] xxx
     *        钟离（愤怒）：xxx   钟离「xxx」   （旁白）
     *  返回 false 表示这行没有显式说话人。 */
    static bool splitSpeaker(const QString& line, QString* speaker, QString* emotion,
                             QString* text, QString* stageNote, bool* narration);
    //! 情绪 → 合成参数
    static VoiceParams paramsForEmotion(const QString& emotion);

    // ── 任务表 JSON ──────────────────────────────────────
    static QJsonObject toJson(const ScriptParseResult& r, const QString& source = QString());
    static ScriptParseResult fromJson(const QJsonObject& o, QString* err = nullptr);

private:
    void resolveCharacters(ScriptParseResult& r, const ScriptParseOptions& opt) const;
    void inferByContext(ScriptParseResult& r, const ScriptParseOptions& opt) const;
    bool inferByLlm(ScriptParseResult& r, const ScriptParseOptions& opt, QStringList* warn);
    static QString readLlmKey(const QString& explicitKey);

    const VkCharIndex* m_index = nullptr;
    const VkConfig*    m_cfg = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
};

} // namespace vk

#endif // VK_SCRIPT_H
