#ifndef VK_JOIN_H
#define VK_JOIN_H

/*!
 * \file  vk_join.h
 * \brief 把一堆单句音频拼成"一整段"（对白整轨）。
 *
 * 为什么不用 ffmpeg：同一个引擎、同一组参数合成出来的 wav，
 * 采样率/声道/位深完全一致 —— 那就是**纯 PCM 拼接**，直接搬字节
 * 最快，而且一个样本都不损失（重编码是有损的）。
 *
 * 只有参数不一致时才需要 ffmpeg 转，那种情况这里直接报错，
 * 不偷偷给你重采样（重采样会掉音质，用户应该知道）。
 */

#include <QString>
#include <QStringList>

namespace vk {

//! 合并结果统计
struct JoinResult {
    int     fileCount = 0;   //!< 拼了几个文件
    qint64  dataBytes = 0;   //!< 纯音频字节数（不含头）
    qint64  totalBytes = 0;  //!< 输出文件总大小
    int     totalMs = 0;     //!< 总时长（毫秒）
    int     sampleRate = 0;
    int     channels = 0;
    int     bits = 0;
};

class VoiceJoiner
{
public:
    /*! 按给定顺序把 files 拼成一个 wav。
     *  \param files    要拼的音频（调用方负责排序 —— 顺序就是成品里的顺序）
     *  \param outPath  输出 wav 路径
     *  \param gapMs    句与句之间插入的静音毫秒（0 = 紧贴；默认 280ms 比较自然）
     *  \param res      统计结果
     *  \param err      失败原因
     */
    static bool join(const QStringList& files, const QString& outPath,
                     int gapMs, JoinResult* res, QString* err = nullptr);

    //! 文件名自然排序（0009 排在 0010 前面，而不是按字符串 "0009" > "0010"）
    static QStringList naturalSort(const QStringList& paths);
};

} // namespace vk

#endif // VK_JOIN_H
