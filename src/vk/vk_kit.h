#ifndef VK_KIT_H
#define VK_KIT_H

#include "vk_types.h"
#include "vk_config.h"

#include <QObject>
#include <QString>
#include <QList>
#include <QMutex>

namespace vk {

class VkApiClient;
class VkEngine;
class VkCharIndex;

/*!
 * \brief 对外唯一门面。别的项目只需要认识这一个类。
 *
 * 典型用法（三步）：
 *    vk::Kit& kit = vk::Kit::instance();
 *    if (kit.init()) {                       // 读配置 + 扫模型库（首次约 1s，之后走缓存）
 *        kit.ensureEngine();                 // 起引擎（已在跑就复用），冷启动 30~60s
 *        QString wav = kit.saySync("契约，必须履行。", "钟离");
 *    }
 *
 * 线程与并发：
 *   - saySync() 内部串行化（QMutex）。引擎 workers=1，并发请求只会排队，
 *     但权重切换是全局状态，并发 = 音色串，所以这里必须锁。
 *   - 引擎当前权重有记忆，同一角色连续合成不会重复切权重。
 *   - 零样本角色会自动切回底模（否则会带上上一个微调角色的音色 —— 真坑）。
 *
 * 缓存：
 *   缓存 key 含 text/char/语速/温度/topK/topP/seed，改参数不会命中旧音频。
 */
class Kit : public QObject
{
    Q_OBJECT
public:
    static Kit& instance();

    // ── 生命周期 ─────────────────────────────────────────
    //! 载入配置（configPath 为空则自动探测）+ 建角色索引
    bool init(const QString& configPath = QString(), bool forceRescan = false,
              QString* err = nullptr);
    bool isInited() const { return m_inited; }
    //! 收尾：默认让引擎常驻（冷启动 180s，杀掉是自残）。要真停请调 stopEngine()
    void shutdown();
    //! 显式停引擎（释放显存）
    void stopEngine();

    VkConfig        config() const { return m_cfg; }
    VkApiClient*    api() const { return m_api; }
    VkEngine*       engine() const { return m_engine; }
    VkCharIndex*    index() const { return m_index; }

    //! 确保引擎可用（未 init 会自动 init）
    bool ensureEngine(QString* err = nullptr);

    // ── 角色 ─────────────────────────────────────────────
    QList<CharInfo> characters() const;
    QList<CharInfo> searchCharacters(const QString& keyword, int limit = 50) const;
    CharInfo        findCharacter(const QString& name, QString* diag = nullptr) const;

    // ── 合成 ─────────────────────────────────────────────
    /*! 同步合成，返回 wav 绝对路径；失败返回空串并写 err。
     *  \param useCache 命中缓存直接返回（默认 true） */
    QString saySync(const QString& text, const QString& charName,
                    const VoiceParams& params = VoiceParams(),
                    QString* err = nullptr, bool useCache = true);

    //! 合成到指定文件（不做缓存读，只做缓存写）
    QString sayToFile(const QString& text, const QString& charName,
                      const QString& outWavPath,
                      const VoiceParams& params = VoiceParams(),
                      QString* err = nullptr);

    // ── 状态 ─────────────────────────────────────────────
    QString currentGptWeights() const { return m_curGpt; }
    QString currentSovitsWeights() const { return m_curSovits; }
    int     cacheHits() const { return m_cacheHits; }
    int     synthCount() const { return m_synthCount; }

    static QString cacheKeyFor(const QString& text, const QString& charName,
                               const VoiceParams& p);

signals:
    void logLine(const QString& line);
    void statusChanged(const QString& status);   //!< 给 UI 用的人类可读状态
    void weightsSwitched(const QString& charName);

private:
    explicit Kit(QObject* parent = nullptr);
    ~Kit() override;
    Kit(const Kit&) = delete;
    Kit& operator=(const Kit&) = delete;

    bool ensureWeights(const CharInfo& c, QString* err);
    void log(const QString& s);

    //! 权重记忆：落盘 + 读回（绑定引擎 pid，pid 变了就作废）
    QString statePath() const;
    void    loadWeightState();
    void    saveWeightState() const;
    //! 引擎会话 id；拿不到（非 Windows / netstat 失败）时返回 0，此时一律重切，宁慢不错
    int     engineSessionId();

    VkConfig         m_cfg;
    VkApiClient*     m_api    = nullptr;
    VkEngine*        m_engine = nullptr;
    VkCharIndex*     m_index  = nullptr;
    QMutex           m_lock;             //!< 串行化"切权重 + 合成"
    QString          m_curGpt;           //!< 引擎当前加载的 GPT 权重（防重复切换）
    QString          m_curSovits;
    int              m_statePid = 0;     //!< 权重记忆对应的引擎进程
    bool             m_keepEngineOnExit = true;   //!< 退出时是否让引擎常驻
    bool             m_inited = false;
    int              m_cacheHits = 0;
    int              m_synthCount = 0;
};

} // namespace vk

#endif // VK_KIT_H
