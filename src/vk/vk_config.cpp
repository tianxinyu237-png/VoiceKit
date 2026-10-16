#include "vk_config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>

namespace vk {

VkConfig::VkConfig()
{
    baseGptRel = QStringLiteral(
        "GPT_SoVITS/pretrained_models/gsv-v2final-pretrained/"
        "s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt");
    baseSovitsRel = QStringLiteral(
        "GPT_SoVITS/pretrained_models/gsv-v2final-pretrained/s2G2333k.pth");
}

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------

//! 相对路径按 baseDir 解析成绝对路径；绝对路径原样规范化返回
static QString resolveAgainst(const QString& baseDir, const QString& p)
{
    if (p.isEmpty())
        return QString();
    QFileInfo fi(p);
    if (fi.isAbsolute())
        return QDir::cleanPath(fi.absoluteFilePath());
    return QDir::cleanPath(QDir(baseDir).absoluteFilePath(p));
}

// 直接把字符串转成绝对路径（不做文件系统探测，允许目录尚不存在）
static QString absFrom(const QString& baseDir, const QString& p)
{
    if (p.isEmpty())
        return QString();
    if (QDir::isAbsolutePath(p))
        return QDir::cleanPath(p);
    return QDir::cleanPath(baseDir + QLatin1Char('/') + p);
}

// ---------------------------------------------------------------------------
// 载入 / 保存
// ---------------------------------------------------------------------------

bool VkConfig::loadFromFile(const QString& jsonPath, QString* err)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("无法打开配置文件: %1 (%2)").arg(jsonPath, f.errorString());
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (err)
            *err = QStringLiteral("配置文件 JSON 语法错误 (offset %1): %2")
                   .arg(pe.offset).arg(pe.errorString());
        return false;
    }
    if (!doc.isObject()) {
        if (err)
            *err = QStringLiteral("配置文件根节点必须是 JSON 对象");
        return false;
    }

    const QJsonObject o = doc.object();
    // 相对路径基准 = 配置文件所在目录
    const QString baseDir = QFileInfo(jsonPath).absolutePath();

    if (o.contains(QStringLiteral("engineDir")))
        engineDir = resolveAgainst(baseDir, o.value(QStringLiteral("engineDir")).toString());
    if (o.contains(QStringLiteral("modelRoot")))
        modelRoot = resolveAgainst(baseDir, o.value(QStringLiteral("modelRoot")).toString());
    if (o.contains(QStringLiteral("workDir")))
        workDir = absFrom(baseDir, o.value(QStringLiteral("workDir")).toString());

    if (o.contains(QStringLiteral("host")))
        host = o.value(QStringLiteral("host")).toString(host);
    if (o.contains(QStringLiteral("port")))
        port = o.value(QStringLiteral("port")).toInt(port);

    readyTimeoutMs = o.value(QStringLiteral("readyTimeoutMs")).toInt(readyTimeoutMs);
    ttsTimeoutMs   = o.value(QStringLiteral("ttsTimeoutMs")).toInt(ttsTimeoutMs);
    pingTimeoutMs  = o.value(QStringLiteral("pingTimeoutMs")).toInt(pingTimeoutMs);
    rememberWeights = o.value(QStringLiteral("rememberWeights")).toBool(rememberWeights);
    keepEngineOnExit = o.value(QStringLiteral("keepEngineOnExit")).toBool(keepEngineOnExit);

    if (o.contains(QStringLiteral("pythonExe")))
        pythonExe = resolveAgainst(baseDir, o.value(QStringLiteral("pythonExe")).toString());
    if (o.contains(QStringLiteral("apiScript")))
        apiScript = resolveAgainst(baseDir, o.value(QStringLiteral("apiScript")).toString());
    if (o.contains(QStringLiteral("ttsConfigPath")))
        ttsConfigPath = resolveAgainst(baseDir, o.value(QStringLiteral("ttsConfigPath")).toString());

    if (o.contains(QStringLiteral("baseGptRel")))
        baseGptRel = o.value(QStringLiteral("baseGptRel")).toString(baseGptRel);
    if (o.contains(QStringLiteral("baseSovitsRel")))
        baseSovitsRel = o.value(QStringLiteral("baseSovitsRel")).toString(baseSovitsRel);

    const QJsonObject al = o.value(QStringLiteral("aliases")).toObject();
    for (auto it = al.begin(); it != al.end(); ++it)
        aliases.insert(it.key(), it.value().toString());

    // 配置里没写引擎目录 -> 尝试自动探测补齐
    if (engineDir.isEmpty() || !QFileInfo(engineDir + QStringLiteral("/api_v2.py")).exists()) {
        QString note;
        VkConfig probe = *this;
        if (probe.autoDiscover(&note)) {
            if (engineDir.isEmpty())
                engineDir = probe.engineDir;
            if (modelRoot.isEmpty())
                modelRoot = probe.modelRoot;
            if (workDir.isEmpty())
                workDir = probe.workDir;
        } else if (err && engineDir.isEmpty()) {
            *err = QStringLiteral("配置未指定 engineDir，且自动探测失败: %1").arg(note);
            return false;
        }
    }
    return true;
}

bool VkConfig::saveToFile(const QString& jsonPath, QString* err) const
{
    QJsonObject o;
    o.insert(QStringLiteral("engineDir"), engineDir);
    o.insert(QStringLiteral("modelRoot"), modelRoot);
    o.insert(QStringLiteral("workDir"), workDir);
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("port"), port);
    o.insert(QStringLiteral("readyTimeoutMs"), readyTimeoutMs);
    o.insert(QStringLiteral("ttsTimeoutMs"), ttsTimeoutMs);
    o.insert(QStringLiteral("pingTimeoutMs"), pingTimeoutMs);
    o.insert(QStringLiteral("rememberWeights"), rememberWeights);
    o.insert(QStringLiteral("keepEngineOnExit"), keepEngineOnExit);
    o.insert(QStringLiteral("pythonExe"), pythonExe);
    o.insert(QStringLiteral("apiScript"), apiScript);
    o.insert(QStringLiteral("ttsConfigPath"), ttsConfigPath);
    o.insert(QStringLiteral("baseGptRel"), baseGptRel);
    o.insert(QStringLiteral("baseSovitsRel"), baseSovitsRel);
    QJsonObject al;
    for (auto it = aliases.begin(); it != aliases.end(); ++it)
        al.insert(it.key(), it.value());
    o.insert(QStringLiteral("aliases"), al);

    QFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("无法写入配置: %1 (%2)").arg(jsonPath, f.errorString());
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

VkConfig VkConfig::loadOrCreate(const QString& jsonPath, QString* err)
{
    VkConfig c;
    if (QFileInfo::exists(jsonPath)) {
        if (!c.loadFromFile(jsonPath, err))
            return VkConfig();   // 返回空配置（调用方看 err）
        return c;
    }
    QString note;
    if (!c.autoDiscover(&note)) {
        if (err)
            *err = QStringLiteral("自动探测引擎目录失败: %1").arg(note);
        return VkConfig();
    }
    if (!jsonPath.isEmpty())
        c.saveToFile(jsonPath, nullptr);
    return c;
}

bool VkConfig::autoDiscover(QString* note)
{
    QDir d(QCoreApplication::applicationDirPath());
    QStringList roots;
    for (int i = 0; i < 5; ++i) {
        roots << d.absolutePath();
        if (!d.cdUp())
            break;
    }

    for (const QString& r : roots) {
        const QString eng = QDir::cleanPath(r + QStringLiteral("/GPT-SoVITS-v2"));
        if (!QFileInfo(eng + QStringLiteral("/api_v2.py")).exists())
            continue;

        engineDir = eng;

        const QString model = QDir::cleanPath(r + QStringLiteral("/GPT-SoVITS模型"));
        if (QFileInfo(model).isDir())
            modelRoot = model;

        const QString vkRoot = QDir::cleanPath(r + QStringLiteral("/VoiceKit"));
        workDir = QDir::cleanPath((QFileInfo(vkRoot).isDir() ? vkRoot : r) + QStringLiteral("/work"));

        if (note)
            *note = QStringLiteral("自动探测到引擎: %1").arg(engineDir);
        return true;
    }

    if (note)
        *note = QStringLiteral("从 %1 向上 5 层都没找到 GPT-SoVITS-v2/api_v2.py")
                .arg(QCoreApplication::applicationDirPath());
    return false;
}

// ---------------------------------------------------------------------------
// 派生路径
// ---------------------------------------------------------------------------

QString VkConfig::resolvedPython() const
{
    if (!pythonExe.isEmpty())
        return pythonExe;
    if (engineDir.isEmpty())
        return QString();
    return QDir::cleanPath(engineDir + QStringLiteral("/runtime/python.exe"));
}

QString VkConfig::resolvedScript() const
{
    if (!apiScript.isEmpty())
        return apiScript;
    if (engineDir.isEmpty())
        return QString();
    return QDir::cleanPath(engineDir + QStringLiteral("/api_v2.py"));
}

static QString ensureDir(const QString& p)
{
    if (!p.isEmpty())
        QDir().mkpath(p);
    return p;
}

QString VkConfig::cacheDir() const { return ensureDir(QDir::cleanPath(workDir + QStringLiteral("/cache"))); }
QString VkConfig::outDir()   const { return ensureDir(QDir::cleanPath(workDir + QStringLiteral("/out"))); }
QString VkConfig::logDir()   const { return ensureDir(QDir::cleanPath(workDir + QStringLiteral("/logs"))); }

QUrl VkConfig::apiBase() const
{
    return QUrl(QStringLiteral("http://%1:%2").arg(host).arg(port));
}

QString VkConfig::engineAbsolute(const QString& rel) const
{
    if (rel.isEmpty())
        return QString();
    return resolveAgainst(engineDir, rel);
}

QStringList VkConfig::problems() const
{
    QStringList p;
    if (engineDir.isEmpty())
        p << QStringLiteral("engineDir 未设置");
    else if (!QFileInfo(engineDir + QStringLiteral("/api_v2.py")).exists())
        p << QStringLiteral("引擎脚本不存在: %1").arg(engineDir + QStringLiteral("/api_v2.py"));

    const QString py = resolvedPython();
    if (py.isEmpty() || !QFileInfo(py).exists())
        p << QStringLiteral("嵌入式 Python 不存在: %1").arg(py);

    if (modelRoot.isEmpty())
        p << QStringLiteral("modelRoot 未设置（角色库为空）");
    else if (!QFileInfo(modelRoot).isDir())
        p << QStringLiteral("模型库目录不存在: %1").arg(modelRoot);

    if (workDir.isEmpty())
        p << QStringLiteral("workDir 未设置");
    return p;
}

} // namespace vk
