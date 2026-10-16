#ifndef VKRUNNER_H
#define VKRUNNER_H

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>

/*!
 * \brief 把 vk.exe 包成一个异步任务。
 *
 * GUI 里所有耗时动作（合成一句 8~35 秒）都走这里，这样界面不会冻住。
 *
 * 为什么不直接链接 libVoiceKit：
 *   1. 引擎本来就是独立的 Python 进程，多一层进程边界不损失什么；
 *   2. 日志可以逐行实时吐出来（合成 35 秒时用户能看见进度，而不是一个转圈）；
 *   3. GUI 崩了不影响正在跑的任务，反过来也一样；
 *   4. 省掉"Qt 网络对象必须在自己线程用"这类容易踩的坑。
 */
class VkRunner : public QObject
{
    Q_OBJECT
public:
    explicit VkRunner(QObject* parent = nullptr);

    void setProgram(const QString& exe) { m_exe = exe; }
    void setWorkDir(const QString& dir) { m_workDir = dir; }
    bool busy() const { return m_busy; }

    //! 启动一次任务；已经在跑就返回 false（不排队，避免堆一坨）
    bool run(const QStringList& args);

    /*! 掐掉正在跑的任务。
     *  用 kill 而不是 terminate：vk.exe 是控制台程序，terminate 发的
     *  WM_CLOSE 它不接，会一直挂着。已经生成的文件不受影响（vk synth
     *  本来就是每句落盘，重跑会自动跳过已完成的）。 */
    void cancel();

signals:
    void line(const QString& text);              //!< 逐行输出（已按 UTF-8 解好）
    void done(int exitCode, int elapsedSec);     //!< 结束（exitCode<0 = 启动失败）

private:
    void onReadyRead();

    QProcess      m_proc;
    QString       m_exe;
    QString       m_workDir;
    bool          m_busy = false;
    QElapsedTimer m_clock;
    QByteArray    m_buf;      // 半行缓冲
};

#endif // VKRUNNER_H
