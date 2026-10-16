#include "vk_engine.h"
#include "vk_apiclient.h"
#include "vk_config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

namespace vk {

//! 等 ms 毫秒，期间保持事件循环（这样 stdout 日志和信号都能被处理）
static void pumpWait(int ms)
{
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(ms);
    loop.exec();
}

VkEngine::VkEngine(const VkConfig* cfg, VkApiClient* api, QObject* parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_api(api)
{
}

VkEngine::~VkEngine()
{
    stop();
}

bool VkEngine::isProcessRunning() const
{
    return m_proc && m_proc->state() != QProcess::NotRunning;
}

bool VkEngine::isServiceAlive()
{
    return m_api && m_api->ping(m_cfg ? m_cfg->pingTimeoutMs : 3000);
}

// ---------------------------------------------------------------------------
// 拉起引擎进程
// ---------------------------------------------------------------------------
bool VkEngine::launch(QString* err)
{
    if (!m_cfg) {
        if (err)
            *err = QStringLiteral("VkEngine 未绑定配置");
        return false;
    }
    if (isProcessRunning())
        return true;

    const QString py = m_cfg->resolvedPython();
    const QString script = m_cfg->resolvedScript();

    if (!QFileInfo::exists(py)) {
        if (err)
            *err = QStringLiteral("找不到嵌入式 Python: %1").arg(py);
        return false;
    }
    if (!QFileInfo::exists(script)) {
        if (err)
            *err = QStringLiteral("找不到引擎脚本: %1").arg(script);
        return false;
    }

    if (!m_proc) {
        m_proc = new QProcess(this);
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &VkEngine::onProcessFinished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            emit logLine(QStringLiteral("[engine] process error %1").arg(int(e)));
        });
#endif
    }

    // 环境：清掉 PYTHONHOME/PYTHONPATH（否则系统 Python 会污染嵌入式运行时），
    // 再把 runtime 及 Scripts、ffmpeg\bin 顶到 PATH 前面
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("PYTHONHOME"));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));

    const QString rt = QDir::cleanPath(m_cfg->engineDir + QStringLiteral("/runtime"));
    const QString oldPath = env.value(QStringLiteral("PATH"));
    env.insert(QStringLiteral("PATH"),
               QStringList{ rt, rt + QStringLiteral("/Scripts"), rt + QStringLiteral("/ffmpeg/bin"), oldPath }
                   .join(QLatin1Char(';')));
    m_proc->setProcessEnvironment(env);

    // 工作目录必须是引擎目录：api_v2.py 里权重路径和 StaticFiles("音频输出") 都是相对的
    m_proc->setWorkingDirectory(m_cfg->engineDir);

    // ── 输出重定向到文件，不要用管道 ──────────────────────
    // 两个坑一起踩过：
    //  1) 没人读管道 -> 缓冲区写满 -> 引擎的 tqdm/print 阻塞 -> 启动卡死
    //  2) 父进程退出 -> 管道断开 -> Python 写 stdout 抛 BrokenPipeError -> 引擎直接崩
    //     （这就是之前 `vk engine start` 之后引擎凭空消失的原因）
    // 走文件则两边都不成立，引擎可以脱离父进程独立活着。
    m_logPath = m_cfg->logDir() + QStringLiteral("/engine.log");
    if (QFileInfo(m_logPath).size() > 8 * 1024 * 1024)
        QFile::remove(m_logPath);   // 别让它无限长
    m_proc->setStandardOutputFile(m_logPath, QIODevice::Append);
    m_proc->setStandardErrorFile(m_logPath, QIODevice::Append);
    m_logPos = QFileInfo(m_logPath).size();

    if (!m_logTimer) {
        m_logTimer = new QTimer(this);
        m_logTimer->setInterval(500);
        connect(m_logTimer, &QTimer::timeout, this, &VkEngine::pollLogFile);
    }
    m_logTimer->start();

    m_proc->setProgram(py);

    // 脚本 + 可选的自定义推理配置。
    // 传 -c 的两个理由：① 引擎切权重时会反写配置文件，不碰原文件更干净；
    // ② 可以在这里一键切换 CPU / GPU 模式（GPU 半精度在这台机上出静音）。
    QStringList args;
    args << script;
    if (!m_cfg->ttsConfigPath.isEmpty() && QFileInfo::exists(m_cfg->ttsConfigPath)) {
        args << QStringLiteral("-c") << m_cfg->ttsConfigPath;
        emit logLine(QStringLiteral("[engine] 使用自定义推理配置: %1").arg(m_cfg->ttsConfigPath));
    } else if (!m_cfg->ttsConfigPath.isEmpty()) {
        emit logLine(QStringLiteral("[engine] ⚠ 自定义推理配置不存在，回退到引擎默认: %1")
                         .arg(m_cfg->ttsConfigPath));
    }
    m_proc->setArguments(args);

    emit logLine(QStringLiteral("[engine] starting: %1 %2").arg(py, args.join(QLatin1Char(' '))));
    emit logLine(QStringLiteral("[engine] cwd: %1").arg(m_cfg->engineDir));
    emit logLine(QStringLiteral("[engine] log: %1").arg(m_logPath));

    m_proc->start();
    if (!m_proc->waitForStarted(15000)) {
        if (err)
            *err = QStringLiteral("引擎进程启动失败: %1").arg(m_proc->errorString());
        return false;
    }
    m_owned = true;
    return true;
}

bool VkEngine::ensureReady(QString* err)
{
    const int readyMs = m_cfg ? m_cfg->readyTimeoutMs : 300000;

    // 服务已经在跑（手工开的 / Python 版工具开的）-> 直接复用，不要重复拉起
    if (isServiceAlive()) {
        m_owned = false;
        emit logLine(QStringLiteral("[engine] 已有引擎在 %1:%2 上运行，复用")
                         .arg(m_cfg->host).arg(m_cfg->port));
        emit ready();
        return true;
    }

    // ── 启动锁 ───────────────────────────────────────────
    // 两个实例同时拉引擎，会互抢磁盘和 4G 显存，实测能把整台机器拖到几十秒没响应。
    // （这条是踩出来的：我并发起了三份 api_v2.py，系统直接卡死。）
    const QString lockPath = m_cfg->logDir() + QStringLiteral("/engine.lock");
    if (QFileInfo::exists(lockPath)) {
        const qint64 age = QDateTime::currentSecsSinceEpoch()
                           - QFileInfo(lockPath).lastModified().toSecsSinceEpoch();
        if (age >= 0 && age < 600) {
            emit logLine(QStringLiteral("[engine] 启动锁存在（%1s 前，另一个实例正在启动），改为等待")
                             .arg(age));
            return waitReady(readyMs, err);
        }
        emit logLine(QStringLiteral("[engine] 启动锁已过期（%1s），清除后继续").arg(age));
        QFile::remove(lockPath);
    }
    {
        QFile lf(lockPath);
        if (lf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            lf.write(QByteArray::number(QCoreApplication::applicationPid()));
            lf.close();
        }
    }

    const bool ok = launch(err) && waitReady(readyMs, err);
    QFile::remove(lockPath);
    return ok;
}

bool VkEngine::waitReady(int timeoutMs, QString* err)
{
    if (isServiceAlive()) {
        emit ready();
        return true;
    }

    QElapsedTimer clock;
    clock.start();
    int lastNoteSec = -5;

    while (clock.elapsed() < timeoutMs) {
        if (m_api->ping(m_cfg ? m_cfg->pingTimeoutMs : 3000)) {
            emit logLine(QStringLiteral("[engine] ready in %1 ms").arg(clock.elapsed()));
            emit ready();
            return true;
        }

        if (m_proc && m_proc->state() == QProcess::NotRunning) {
            if (err) {
                *err = QStringLiteral("引擎进程提前退出 (exit=%1): %2")
                           .arg(m_proc->exitCode())
                           .arg(logTail(8).join(QStringLiteral(" | ")));
            }
            return false;
        }

        const int sec = int(clock.elapsed() / 1000);
        if (sec - lastNoteSec >= 10) {
            lastNoteSec = sec;
            // 冷启动要加载 BERT + CNHuBERT + 权重，30~60s 是正常速度
            emit logLine(QStringLiteral("[engine] 等待就绪… %1s / %2s")
                             .arg(sec).arg(timeoutMs / 1000));
        }
        pumpWait(500);
    }

    if (err)
        *err = QStringLiteral("等待引擎就绪超时 (%1 ms)").arg(timeoutMs);
    return false;
}

void VkEngine::stop()
{
    if (!isProcessRunning()) {
        m_owned = false;
        return;
    }
    // 不用 /control?command=restart：它内部 os.execl 会把自己的父子关系撕掉，
    // 变成孤儿进程，QProcess 再也管不住。restart 由调用方 stop 后再 launch 实现。
    if (m_api && m_api->ping(1500)) {
        m_api->control(QStringLiteral("exit"), nullptr);
        if (m_proc->waitForFinished(8000)) {
            emit logLine(QStringLiteral("[engine] 已优雅退出"));
            m_owned = false;
            emit stopped();
            return;
        }
    }
    emit logLine(QStringLiteral("[engine] 优雅退出超时，terminate()"));
    m_proc->terminate();
    if (m_proc->waitForFinished(5000)) {
        m_owned = false;
        emit stopped();
        return;
    }
    emit logLine(QStringLiteral("[engine] terminate 无效，kill()"));
    m_proc->kill();
    m_proc->waitForFinished(3000);
    m_owned = false;
    emit stopped();
}

void VkEngine::stopIfOwned()
{
    if (m_owned)
        stop();
    else
        emit logLine(QStringLiteral("[engine] 服务不是本进程拉起的，保持运行"));
}

void VkEngine::detach()
{
    m_owned = false;
    if (m_logTimer)
        m_logTimer->stop();
    if (m_proc) {
        // 故意"泄漏"这个 QProcess 对象。
        // QProcess 析构时会 TerminateProcess 掉子进程，而引擎冷启动要 180~300s，
        // 绝不能因为 CLI 用完退出就把它带走。进程退出时操作系统回收这点内存，代价为零。
        // 注意：引擎的 stdout 已经重定向到日志文件，所以父进程走了它也不会 BrokenPipe。
        m_proc->disconnect(this);
        m_proc = nullptr;
        emit logLine(QStringLiteral("[engine] 已脱离管理，引擎继续常驻（vk engine stop 可停）"));
    }
}

void VkEngine::ingestOutput(const QByteArray& chunk)
{
    if (chunk.isEmpty())
        return;
    const QString text = QString::fromUtf8(chunk);
    emit logRaw(text);

    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                         QString::SkipEmptyParts);
    for (const QString& l : lines) {
        m_log << l;
        emit logLine(l);
    }
    while (m_log.size() > 500)
        m_log.removeFirst();
}

void VkEngine::onStdout()
{
    if (!m_proc)
        return;
    ingestOutput(m_proc->readAllStandardOutput());
}

//! 轮询日志文件拿增量：引擎输出走文件，不占管道，父进程退出也不会把它带崩
void VkEngine::pollLogFile()
{
    if (m_logPath.isEmpty())
        return;
    QFile f(m_logPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (m_logPos > f.size())
        m_logPos = 0;          // 文件被清空/轮转
    if (!f.seek(m_logPos))
        return;
    const QByteArray chunk = f.readAll();
    m_logPos = f.pos();
    ingestOutput(chunk);
}

void VkEngine::onProcessFinished(int exitCode)
{
    if (m_logTimer)
        m_logTimer->stop();
    emit logLine(QStringLiteral("[engine] 进程结束 (exit=%1)").arg(exitCode));
    m_owned = false;
    emit stopped();
}

QStringList VkEngine::logTail(int maxLines) const
{
    if (m_log.size() <= maxLines)
        return m_log;
    return m_log.mid(m_log.size() - maxLines);
}

} // namespace vk
