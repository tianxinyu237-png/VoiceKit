#include "vk_apiclient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

namespace vk {

VkApiClient::VkApiClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

VkApiClient::~VkApiClient() = default;

void VkApiClient::setBaseUrl(const QUrl& url)
{
    m_base = url;
}

// ---------------------------------------------------------------------------
// 底层请求：同步等回复，事件循环不阻塞
// ---------------------------------------------------------------------------
QByteArray VkApiClient::request(const QUrl& url, const QByteArray& jsonBody,
                                int timeoutMs, int* status, QString* err,
                                const QString& tag)
{
    if (status)
        *status = 0;
    if (!m_base.isValid()) {
        if (err)
            *err = QStringLiteral("[%1] 未设置引擎地址").arg(tag);
        return QByteArray();
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json; charset=utf-8"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
#endif

    QNetworkReply* reply = jsonBody.isEmpty() ? m_nam->get(req) : m_nam->post(req, jsonBody);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (!timer.isActive()) {
        // 超时：主动 abort，否则连接会一直挂着
        const QString why = QStringLiteral("[%1] 请求超时 (%2 ms): %3")
                                .arg(tag).arg(timeoutMs).arg(url.toString());
        reply->abort();
        reply->deleteLater();
        if (err)
            *err = why;
        return QByteArray();
    }
    timer.stop();

    const QByteArray data = reply->readAll();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError ne = reply->error();
    const QString netErrStr = reply->errorString();
    reply->deleteLater();

    if (status)
        *status = httpStatus;

    if (ne != QNetworkReply::NoError || httpStatus >= 400) {
        const QString detail = describeHttpError(httpStatus, data);
        if (err) {
            *err = detail.isEmpty()
                       ? QStringLiteral("[%1] 网络错误: %2 (HTTP %3)").arg(tag, netErrStr).arg(httpStatus)
                       : QStringLiteral("[%1] %2").arg(tag, detail);
        }
        return QByteArray();
    }
    return data;
}

QString VkApiClient::describeHttpError(int status, const QByteArray& body)
{
    QString msg;
    if (status)
        msg = QStringLiteral("HTTP %1 ").arg(status);

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject o = doc.object();
        QString m = o.value(QStringLiteral("message")).toString();
        const QString ex = o.value(QStringLiteral("Exception")).toString();
        if (!ex.isEmpty())
            m += QStringLiteral(" | ") + ex;
        if (!m.isEmpty())
            return msg + m;
    }
    if (!body.isEmpty() && body.size() < 500)
        return msg + QString::fromUtf8(body).trimmed();
    return msg;
}

// ---------------------------------------------------------------------------
// 引擎接口
// ---------------------------------------------------------------------------
bool VkApiClient::ping(int timeoutMs)
{
    QUrl url = m_base;
    url.setPath(QStringLiteral("/ping"));
    int st = 0;
    const QByteArray d = request(url, QByteArray(), timeoutMs, &st, nullptr, QStringLiteral("ping"));
    return st == 200 && !d.isEmpty();
}

bool VkApiClient::setGptWeights(const QString& weightsPath, QString* err)
{
    if (weightsPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("GPT 权重路径为空");
        return false;
    }
    QUrl url = m_base;
    url.setPath(QStringLiteral("/set_gpt_weights"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("weights_path"), weightsPath);
    url.setQuery(q);

    int st = 0;
    // 切权重可能触发磁盘加载 + 显存分配，4G 卡上给足时间
    request(url, QByteArray(), 180000, &st, err, QStringLiteral("set_gpt_weights"));
    return st == 200;
}

bool VkApiClient::setSovitsWeights(const QString& weightsPath, QString* err)
{
    if (weightsPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("SoVITS 权重路径为空");
        return false;
    }
    QUrl url = m_base;
    url.setPath(QStringLiteral("/set_sovits_weights"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("weights_path"), weightsPath);
    url.setQuery(q);

    int st = 0;
    request(url, QByteArray(), 180000, &st, err, QStringLiteral("set_sovits_weights"));
    return st == 200;
}

bool VkApiClient::setReferAudio(const QString& refAudioPath, QString* err)
{
    QUrl url = m_base;
    url.setPath(QStringLiteral("/set_refer_audio"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("refer_audio_path"), refAudioPath);
    url.setQuery(q);

    int st = 0;
    request(url, QByteArray(), 60000, &st, err, QStringLiteral("set_refer_audio"));
    return st == 200;
}

bool VkApiClient::control(const QString& command, QString* err)
{
    QUrl url = m_base;
    url.setPath(QStringLiteral("/control"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("command"), command);
    url.setQuery(q);
    int st = 0;
    // exit/restart 会当场断开连接，超时短一点，失败也不重要
    request(url, QByteArray(), 5000, &st, err, QStringLiteral("control"));
    return true;
}

QByteArray VkApiClient::tts(const QVariantMap& params, QString* err, int timeoutMs)
{
    if (params.isEmpty()) {
        // 兜底：request() 的约定是"空 body 走 GET"，而这个接口是 POST /。
        // 真发出一个裸 GET / 的话引擎会抛 500 且报一句莫名其妙的
        // AttributeError: 'NoneType' object has no attribute 'lower'，
        // 把真正的原因藏起来。宁可在这里就拦住。
        if (err)
            *err = QStringLiteral("[tts] 合成参数为空，拒绝发送请求");
        return QByteArray();
    }

    QJsonObject o;
    for (auto it = params.begin(); it != params.end(); ++it)
        o.insert(it.key(), QJsonValue::fromVariant(it.value()));
    const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);

    QUrl url = m_base;
    url.setPath(QStringLiteral("/"));

    int st = 0;
    const QByteArray data = request(url, body, timeoutMs, &st, err, QStringLiteral("tts"));
    if (data.isEmpty())
        return QByteArray();

    // 必须确认拿到的真是 wav，否则 UI 层会拿到一段 JSON 当音频播
    if (!data.startsWith("RIFF")) {
        if (err)
            *err = QStringLiteral("[tts] 返回的不是 wav 数据 (HTTP %1): %2")
                       .arg(st).arg(QString::fromUtf8(data.left(300)));
        return QByteArray();
    }
    return data;
}

/*! 用 netstat 找监听端口的 pid。
 *  为什么要这么绕：api_v2.py 不提供"当前加载了哪套权重"的查询接口，
 *  引擎重启后内存里的权重会回到 tts_infer.yaml 的配置，
 *  所以我们只能靠"引擎进程没换过"来推断落盘的权重记忆仍然有效。 */
int VkApiClient::servicePid()
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("netstat"), QStringList{ QStringLiteral("-ano"),
                                                    QStringLiteral("-p"),
                                                    QStringLiteral("TCP") });
    if (!p.waitForFinished(5000))
        return 0;

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    const QString needle = QStringLiteral(":%1").arg(m_base.port());

    const QStringList lines = out.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                        QString::SkipEmptyParts);
    for (const QString& line : lines) {
        if (!line.contains(needle) || !line.contains(QStringLiteral("LISTENING")))
            continue;
        const QStringList f = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                         QString::SkipEmptyParts);
        if (f.size() >= 5 && f.at(1).endsWith(needle))
            return f.last().toInt();
    }
#endif
    return 0;
}

} // namespace vk
