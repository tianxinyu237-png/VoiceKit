#ifndef VK_TYPES_H
#define VK_TYPES_H

#include <QString>
#include <QMetaType>

namespace vk {

/*! 合成参数。默认值 = 引擎默认值，改哪个填哪个。 */
struct VoiceParams {
    float   speed             = 1.0f;   // 语速倍率
    float   temperature       = 1.0f;   // 采样温度：越大越"飘"，越小越稳
    float   topP              = 1.0f;
    int     topK              = 5;
    int     seed              = -1;     // -1 = 随机
    float   repetitionPenalty = 1.35f;
    QString textLang          = QStringLiteral("zh");
    QString promptLang        = QStringLiteral("zh");
    QString splitMethod       = QStringLiteral("cut5");
    QString mediaType         = QStringLiteral("wav");
};

/*! 一个音色（角色）的全部信息。 */
struct CharInfo {
    QString name;        //!< 规范名（已剥掉 "-----御姐" 这类目录尾巴）
    QString rawName;     //!< 原始目录名
    QString game;        //!< 所属作品（模型库一级目录）
    QString dir;         //!< 角色目录绝对路径
    QString refAudio;    //!< 参考音频绝对路径
    QString promptText;  //!< 参考音频对应的文本
    QString gptCkpt;     //!< GPT 权重绝对路径；空 = 走零样本
    QString sovitsPth;   //!< SoVITS 权重绝对路径

    bool isValid() const { return !name.isEmpty(); }
    //! 缺任一权重即为零样本音色（只有参考音频，能直接合成，音色贴合度低于微调）
    bool zeroShot() const { return gptCkpt.isEmpty() || sovitsPth.isEmpty(); }
};

} // namespace vk

Q_DECLARE_METATYPE(vk::CharInfo)

#endif // VK_TYPES_H
