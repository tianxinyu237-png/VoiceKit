#include "mainwindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QTextStream>
#include <QTimer>

/*!
 * 把 Qt 自己的日志（qDebug/qWarning）写进 work/gui/qt.log。
 *
 * 为什么需要：GUI 是 windows 子系统，没有控制台，Qt 的警告（比如
 * "Could not parse stylesheet of object ..."）默认被直接丢掉，
 * 于是样式表写错了会表现为"界面白白的、什么提示都没有"。
 * 有了这个文件，出问题一读就知道。
 */
static void vkMsgHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    static QString path;
    if (path.isEmpty()) {
        path = QDir::cleanPath(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/../work/gui/qt.log"));
        QDir().mkpath(QFileInfo(path).absolutePath());
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream ts(&f);
    ts.setCodec("UTF-8");     // 不设的话中文路径在 log 里是乱码（默认按本地码页写，却按 UTF-8 读）
    const char* lvl = "info";
    switch (type) {
    case QtDebugMsg:    lvl = "debug"; break;
    case QtInfoMsg:     lvl = "info";  break;
    case QtWarningMsg:  lvl = "WARN";  break;
    case QtCriticalMsg: lvl = "CRIT";  break;
    case QtFatalMsg:    lvl = "FATAL"; break;
    }
    ts << QDateTime::currentDateTime().toString(QStringLiteral("MM-dd hh:mm:ss"))
       << " [" << lvl << "] " << msg << "\n";
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VoiceKit"));
    app.setOrganizationName(QStringLiteral("VoiceKit"));

    qInstallMessageHandler(vkMsgHandler);

    // 中文界面用系统默认字体（微软雅黑），别让 Qt 挑到点阵宋体
    QFont f = app.font();
#ifdef Q_OS_WIN
    f.setFamily(QStringLiteral("Microsoft YaHei UI"));
#endif
    f.setPointSize(9);
    app.setFont(f);

    MainWindow w;
    w.show();

    // 调试用：--shot <file.png> 把窗口自己渲染成 PNG 然后退出。
    // 比系统截图可靠：不受窗口遮挡影响，抓的就是 Qt 真实渲染的结果。
    const QStringList args = app.arguments();
    const int si = args.indexOf(QStringLiteral("--shot"));
    if (si >= 0 && si + 1 < args.size()) {
        const QString out = args.at(si + 1);
        QTimer::singleShot(2500, [&w, out]() {
            const bool ok = w.grab().save(out);
            qDebug() << "shot" << (ok ? "ok" : "FAIL") << out;
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
