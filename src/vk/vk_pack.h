#ifndef VK_PACK_H
#define VK_PACK_H

#include <QString>
#include <QStringList>
#include <QList>

namespace vk {

struct VkConfig;

/*!
 * \brief 语音包规格。
 *
 * 单片机不是 PC：Flash 几 MB、没解码器、文件名只能 ASCII。
 * 所以不是"存 wav"，而是"降规格 + 打包 + 生成索引"。
 *
 * 体积参考（3 秒一句，100 句）：
 *   32k/16bit  18.3 MB  ← 引擎原声，单片机装不下
 *   16k/16bit   9.2 MB  ← 通用档
 *   16k/8bit    4.6 MB
 *    8k/16bit   4.6 MB
 *    8k/8bit    2.3 MB
 *    8k/ADPCM   1.1 MB  ← 嵌入式首选（IMA ADPCM，4:1）
 *    MP3 32k    1.2 MB  ← DFPlayer 模块用
 */
struct PackSpec {
    QString name = QStringLiteral("embedded");
    int     sampleRate = 8000;
    int     bits = 16;            //!< 8 或 16
    bool    adpcm = false;        //!< IMA ADPCM（4bit/样本）
    bool    mp3 = false;          //!< 输出 MP3（给 DFPlayer 这类模块）
    bool    normalize = true;     //!< 峰值归一化
    float   targetDb = -3.0f;     //!< 归一化目标峰值
    QString idPrefix = QStringLiteral("v");

    //! 内置预设：pc / embedded / embedded8bit / embedded8k16 / mp3
    static PackSpec fromName(const QString& name);
    static QStringList knownNames();
    QString describe() const;
    //! 生成 ffmpeg 的输出参数（不含 -i / 输出文件）
    QStringList ffmpegOutputArgs() const;
};

//! 包里的一个条目 = 一句话
struct PackEntry {
    QString id;          //!< ASCII ID（v0001）
    QString charName;    //!< 角色（中文，只进 manifest）
    QString text;        //!< 台词
    QString emotion;
    QString srcWav;      //!< 源 wav
    QString outWav;      //!< 通用档 wav（16k/16bit，PC 能直接播）
    quint32 offset = 0;  //!< 在 voice.bin 里的偏移
    quint32 length = 0;  //!< 字节数
    int     durationMs = 0;
};

struct PackResult {
    bool    ok = false;
    QString outDir;
    QString binFile;
    QString indexPath;
    QString manifestPath;
    int     count = 0;
    int     failed = 0;
    qint64  binBytes = 0;
    qint64  srcBytes = 0;
    QStringList warnings;
    QString specText;
};

/*!
 * \brief 语音包烘制器。
 *
 * 流程：源 wav → ffmpeg 重采样（带抗混叠滤波）/峰值归一/位深或 ADPCM 转换
 *      → 剥 wav 头取裸数据 → 拼 voice.bin → 写 voice_index.h / manifest.json
 *
 * 重采样**必须用 ffmpeg**，不能用简单线性插值：32k→8k 不做低通会产生混叠，
 * 那种"嘶嘶"声听起来像模型坏了，实际是重采样写错了。
 */
class VoicePackBuilder
{
public:
    explicit VoicePackBuilder(const VkConfig* cfg);

    //! 从配音任务表（vk script 的产物）构建
    PackResult buildFromTable(const QString& tableJson, const QString& outDir,
                              const PackSpec& spec, QString* err = nullptr);
    //! 从一堆 wav 直接构建（wav 文件名当文本）
    PackResult buildFromWavs(const QStringList& wavs, const QString& outDir,
                             const PackSpec& spec, QString* err = nullptr);

    //! 读 wav 头：拿采样率/声道/位深/data 偏移与长度/时长
    static bool probeWav(const QString& path, int* sampleRate, int* channels, int* bits,
                         qint64* dataOffset, qint64* dataLength, int* durationMs,
                         QString* err = nullptr);
    //! 找 ffmpeg（配置 → PATH）
    static QString findFfmpeg(const VkConfig* cfg);

private:
    QString convertOne(const QString& src, const QString& dst, const PackSpec& spec,
                       int* durationMs, QString* err);
    bool    measurePeakDb(const QString& wav, double* peakDb, QString* err);

    const VkConfig* m_cfg = nullptr;
};

} // namespace vk

#endif // VK_PACK_H
