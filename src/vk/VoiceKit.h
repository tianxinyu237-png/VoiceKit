/*!
 * \file VoiceKit.h
 * \brief VoiceKit 对外唯一入口。别的项目 #include 这一个头文件就够了。
 *
 * ── 给其他 Qt 项目用（三行）────────────────────────────
 *    #include "VoiceKit.h"
 *    vk::Kit& kit = vk::Kit::instance();
 *    kit.init();  kit.ensureEngine();
 *    QString wav = kit.saySync("契约，必须履行。", "钟离");
 *    // wav 是 wav 文件绝对路径，QSound / QMediaPlayer 直接能放
 *
 * ── 链接方式（.pro）──────────────────────────────────
 *    INCLUDEPATH += <VoiceKit>/src/vk
 *    LIBS        += -L<VoiceKit>/lib -lVoiceKit
 *    （客户端也要 QT += network）
 *
 * 前置条件：引擎目录（GPT-SoVITS-v2）与模型库（GPT-SoVITS模型）在不高于
 * 客户程序上四层的某个祖先目录中并列存在，或显式调用
 * init("<VoiceKit>/config/voicekit.json")。自动探测覆盖不了时用配置文件。
 */

#ifndef VOICEKIT_H
#define VOICEKIT_H

#include "vk_types.h"
#include "vk_config.h"
#include "vk_kit.h"
#include "vk_engine.h"
#include "vk_apiclient.h"
#include "vk_charindex.h"
#include "vk_script.h"
#include "vk_pack.h"

#define VOICEKIT_VERSION "0.1.0"

#endif // VOICEKIT_H
