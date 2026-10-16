#ifndef VK_ENGINE_H
#define VK_ENGINE_H

#include <QObject>
#include <QStringList>
#include <QString>

class QProcess;
class QTimer;

namespace vk {

struct VkConfig;
class VkApiClient;

/*!
 * \brief 管理 GPT-SoVITS 引擎进程（runtime\python.exe api_v2.py）。
 *
 * 踩过的坑，都写在这里了：
 *  1. QProcess 工作目录必须是引擎目录 —— api_v2.py 里 StaticFiles("音频输出")
 *     和所有权重路径都是相对的，CWD 不对直接起不来。
 *  2. 必须清掉 PYTHONHOME / PYTHONPATH —— 否则系统 Python 会污染嵌入式运行时。
 *  3. 必须消费 stdout —— 引擎启动日志很长，管道写满会把引擎卡死。
 *  4. 停引擎不用 /control?restart（内部 os.execl 会把自己的父子关系撕掉），
 *     先发 /control?command=exit 求优雅退出，超时再 terminate，最后 kill。
 *  5. 引擎可能已经在跑（手工开的、或 Python 版配音pc 开的），
 *      start() 先 ping，通则直接复用，绝不重复拉起。
 */
class VkEngine : public QObject
{
    Q_OBJECT
public:
    VkEngine(const VkConfig* cfg, VkApiClient* api, QObject* parent = nullptr);
    ~VkEngine() override;

    //! 确保引擎可用：已就绪直接 true；没跑就拉起来并等就绪
    bool ensureReady(QString* err = nullptr);
    //! 拉起进程（不等待就绪）
    bool launch(QString* err = nullptr);
    //! 轮询 /ping 直到就绪
    bool waitReady(int timeoutMs, QString* err = nullptr);
    //! 停引擎：优雅 -> terminate -> kill
    void stop();
    //! 只关掉"我们自己拉起来的"那个进程，不动别人开的服务
    void stopIfOwned();

    /*! 脱离管理：让引擎常驻，本进程退出也不影响它。
     *  为什么需要：QProcess 析构会 TerminateProcess 掉子进程，
     *  而引擎冷启动实测要 180s —— CLI 跑完就杀掉它等于自残。 */
    void detach();

    bool isProcessRunning() const;
    bool isServiceAlive();
    bool ownedByUs() const { return m_owned; }

    QStringList logTail(int maxLines = 80) const;
    QString     lastError() const { return m_lastError; }

signals:
    void logLine(const QString& line);
    void logRaw(const QString& text);
    void ready();
    void stopped();

private slots:
    void onStdout();
    void pollLogFile();
    void onProcessFinished(int exitCode);

private:
    void ingestOutput(const QByteArray& chunk);

    const VkConfig* m_cfg = nullptr;
    VkApiClient*    m_api = nullptr;
    QProcess*       m_proc = nullptr;
    QTimer*         m_logTimer = nullptr;   //!< 轮询日志文件（输出走文件不走管道）
    QString         m_logPath;
    qint64          m_logPos = 0;
    QStringList     m_log;
    QString         m_lastError;
    bool            m_owned = false;   //!< 进程是不是我们拉起来的
};

} // namespace vk

#endif // VK_ENGINE_H
