#include "vkrunner.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

VkRunner::VkRunner(QObject* parent)
    : QObject(parent)
{
    m_proc.setProcessChannelMode(QProcess::MergedChannels);

#ifdef Q_OS_WIN
    // 关键：vk.exe 是控制台程序，不加这个每跑一次任务就闪一个黑窗口出来
    m_proc.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
#endif

    connect(&m_proc, &QProcess::readyRead, this, &VkRunner::onReadyRead);

    connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st) {
                onReadyRead();                 // 把最后没带换行的一截收掉
                m_busy = false;
                emit done(st == QProcess::NormalExit ? code : -1,
                          int(m_clock.elapsed() / 1000));
            });

    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_busy = false;
            emit line(QStringLiteral("✗ 起不了 vk.exe：%1").arg(m_proc.errorString()));
            emit done(-1, 0);
        }
    });
}

bool VkRunner::run(const QStringList& args)
{
    if (m_busy || m_exe.isEmpty())
        return false;

    m_buf.clear();
    m_busy = true;
    m_clock.start();
    if (!m_workDir.isEmpty())
        m_proc.setWorkingDirectory(m_workDir);
    m_proc.start(m_exe, args);
    return true;
}

void VkRunner::cancel()
{
    if (!m_busy || m_proc.state() == QProcess::NotRunning)
        return;
    emit line(QStringLiteral("⏹ 正在停止…（已经生成的文件会保留）"));
    m_proc.kill();
    m_proc.waitForFinished(5000);
}

void VkRunner::onReadyRead()
{
    m_buf += m_proc.readAll();

    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        // 变量别叫 line —— 会把上面的信号名 line 遮蔽掉，
        // 报错是莫名其妙的 "no match for call to '(QByteArray) (QString)'"
        QByteArray one = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        if (one.endsWith('\r'))
            one.chop(1);
        emit line(QString::fromUtf8(one));
    }
}
