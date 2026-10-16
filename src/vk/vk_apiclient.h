#ifndef VK_APICLIENT_H
#define VK_APICLIENT_H

#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;

namespace vk {

/*!
 * \brief GPT-SoVITS api_v2.py 的同步 HTTP 客户端。
 *
 * 为什么是"同步"：合成本来就是一次一发的阻塞操作（引擎 workers=1），
 * 异步回调只会让调用方被迫写状态机。内部用 QEventLoop 等回复，
 * 事件循环仍在转，不会假死。
 *
 * 全部走 POST + JSON body / GET query，中文一律 UTF-8，
 * 避免长台词顶破 URL 长度上限。
 */
class VkApiClient : public QObject
{
    Q_OBJECT
public:
    explicit VkApiClient(QObject* parent = nullptr);
    ~VkApiClient() override;

    void setBaseUrl(const QUrl& url);
    QUrl baseUrl() const { return m_base; }

    // ── 引擎接口 ─────────────────────────────────────────
    bool ping(int timeoutMs = 3000);
    bool setGptWeights(const QString& weightsPath, QString* err = nullptr);
    bool setSovitsWeights(const QString& weightsPath, QString* err = nullptr);
    bool setReferAudio(const QString& refAudioPath, QString* err = nullptr);
    bool control(const QString& command, QString* err = nullptr);

    //! 合成。params 至少含 text / ref_audio_path / prompt_text……
    QByteArray tts(const QVariantMap& params, QString* err = nullptr,
                   int timeoutMs = 300000);

    /*! 拿监听端口的进程 pid —— 用它当"引擎会话 ID"。
     *  权重记忆必须绑定 pid：pid 变了说明引擎重启过，config 里的旧权重失效，
     *  必须重新切，否则会拿错音色合成（音色串的根源）。 */
    int servicePid();

    //! 引擎在线时返回权重文件基名，用于日志
    static QString describeHttpError(int status, const QByteArray& body);

private:
    QByteArray request(const QUrl& url, const QByteArray& jsonBody,
                       int timeoutMs, int* status, QString* err,
                       const QString& tag);

    QNetworkAccessManager* m_nam = nullptr;
    QUrl m_base;
};

} // namespace vk

#endif // VK_APICLIENT_H
