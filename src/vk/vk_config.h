#ifndef VK_CONFIG_H
#define VK_CONFIG_H

#include <QString>
#include <QMap>
#include <QUrl>

namespace vk {

/*!
 * \brief VoiceKit 运行配置。
 *
 * 来源优先级：显式 loadOrCreate(path) > 自动探测。
 * 自动探测规则：从 exe 所在目录向上最多 4 层找 "GPT-SoVITS-v2/api_v2.py"，
 * 找到就把同级目录当作根，不依赖写死的绝对路径（拷到别处也能跑）。
 */
struct VkConfig {
    QString engineDir;      //!< GPT-SoVITS-v2 目录（含 api_v2.py 与 runtime/）
    QString modelRoot;      //!< GPT-SoVITS模型 目录（角色权重 + 参考音频）
    QString workDir;        //!< 运行时目录（cache / out / logs）

    QString host = QStringLiteral("127.0.0.1");
    int     port = 9880;

    int readyTimeoutMs = 300000;   //!< 冷启动等就绪上限（实测 1650 4G 上要 180s！）
    int ttsTimeoutMs   = 300000;   //!< 首次合成还要加载 BERT，实测 116s
    int pingTimeoutMs  = 3000;

    /*! 权重记忆：把"引擎里现在加载的是哪套权重"落盘，
     *  配合引擎进程 pid 判断是否仍然有效（pid 变了说明引擎重启过，必须重切）。
     *  这是把单次 CLI 调用从 65s 降到 5s 的关键，默认开。 */
    bool rememberWeights = true;

    /*! 退出时是否让引擎继续常驻。默认 true：冷启动实测 180s，用完就杀是自残。
     *  GUI 工作台如果想让用户关窗即释放显存，把它设 false。 */
    bool keepEngineOnExit = true;

    QString pythonExe;      //!< 空 = engineDir/runtime/python.exe
    QString apiScript;      //!< 空 = engineDir/api_v2.py

    /*! 引擎启动时用 `-c` 指定的推理配置（空 = 让引擎用它自己的 tts_infer.yaml）。
     *  为什么要自定义：引擎每次切权重都会把配置反写回 yaml，会污染原文件，
     *  而且实测 tts_infer.yaml 被反写过 subst 盘符路径（subst 一解除就失效）。
     *  用自己的一份副本，既干净又能一键切 CPU / GPU。 */
    QString ttsConfigPath;

    QString baseGptRel;     //!< 零样本用的底模（相对 engineDir）
    QString baseSovitsRel;

    QMap<QString, QString> aliases;   //!< 别名表：小三月 -> 三月七

    // ── 构造 ──────────────────────────────────────────────
    VkConfig();
    static VkConfig loadOrCreate(const QString& jsonPath, QString* err = nullptr);
    //! 从 exe 目录向上探测引擎；返回是否探测成功
    bool autoDiscover(QString* note = nullptr);
    //! 加载/保存
    bool loadFromFile(const QString& jsonPath, QString* err = nullptr);
    bool saveToFile(const QString& jsonPath, QString* err = nullptr) const;

    // ── 派生路径 ──────────────────────────────────────────
    QString resolvedPython() const;
    QString resolvedScript() const;
    QString cacheDir() const;
    QString outDir() const;
    QString logDir() const;
    QUrl    apiBase() const;                 //!< http://host:port
    QString engineAbsolute(const QString& rel) const;  //!< 相对 engineDir 转绝对

    QStringList problems() const;            //!< 配置体检：返回人类可读的问题列表
};

} // namespace vk

#endif // VK_CONFIG_H
