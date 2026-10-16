#ifndef VK_CHARINDEX_H
#define VK_CHARINDEX_H

#include "vk_types.h"

#include <QString>
#include <QList>
#include <QHash>

namespace vk {

struct VkConfig;

/*!
 * \brief 扫描模型库，建立"角色名 -> 权重 / 参考音频"索引。
 *
 * 模型库的目录名是脏的，实测样本：
 *   布洛妮娅----御姐        -> 布洛妮娅
 *   桂乃芬------------------女友 -> 桂乃芬
 *   波提欧-----粗狂男音     -> 波提欧
 *   砂金---男主的声音       -> 砂金
 *   开拓者(男)              -> 保持原样（性别是不同角色，不能剥）
 *   阮•梅 / 托帕&账账        -> 保持原样
 * 所以剥尾巴用"连续 2 个以上短横线"这一条判据，不用通用清洗。
 *
 * 参考音频文件名同样带情绪前缀：
 *   害怕-我刚刚说的那些话，你别告诉艾丝妲站长。.wav
 *   -> promptText = 我刚刚说的那些话，你别告诉艾丝妲站长。
 *   七七/还有，长生。她的鼻子，比狗狗灵.wav  （无前缀）-> 全取
 * 判据：第一个 '-' 之前若是 <= 6 个字且不含标点，才当情绪标签剥掉。
 *
 * 权重两处存放（别只扫一处）：
 *   - 原神 / 崩三 / 绝区零 / 蔚蓝档案：ckpt + pth 就在角色目录里
 *   - 星穹铁道：权重在引擎 GPT_weights_v2 / SoVITS_weights_v2，参考音频在模型库
 */
class VkCharIndex
{
public:
    explicit VkCharIndex(const VkConfig* cfg);

    //! 加载索引：命中缓存直接用，模型库变了自动重扫
    bool load(bool forceRescan = false, QString* err = nullptr);
    void clear();

    const QList<CharInfo>& all() const { return m_chars; }
    int  count() const { return m_chars.size(); }
    QStringList games() const;

    //! 严格查找：精确 -> 别名 -> 唯一模糊。有歧义时返回空并给出候选（绝不静默猜）
    CharInfo find(const QString& name, QString* diag = nullptr) const;

    //! 宽松搜索，供 UI 列候选
    QList<CharInfo> search(const QString& keyword, int limit = 50) const;

    bool   isFromCache() const { return m_fromCache; }
    QString cachePath() const;

    // ── 静态工具（可单独测）─────────────────────────────
    static QString normalizeName(const QString& rawDirName);
    static QString extractPromptText(const QString& wavFileName);
    static int     levenshtein(const QString& a, const QString& b);

private:
    bool scan();
    bool loadCache();
    bool saveCache() const;
    QString signature() const;      //!< 模型库指纹（一级目录 mtime 拼串）
    void appendFromModelRoot();
    void appendFromEngineWeights();

    const VkConfig* m_cfg = nullptr;
    QList<CharInfo> m_chars;
    QHash<QString, int> m_byName;   //!< 规范名 -> 下标（首个）
    bool m_fromCache = false;
    bool m_loaded = false;
};

} // namespace vk

#endif // VK_CHARINDEX_H
