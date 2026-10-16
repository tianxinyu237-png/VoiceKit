# VoiceKit

GPT-SoVITS 音色工具链。目标：**一次做好语音包，以后任何项目 `include` 就能用，不用重写。**

- ⚙️ Qt 5.14.2 + MinGW 7.3 64位 + qmake
- 🎯 产物：`lib/libVoiceKit.a`（静态库，给别的 Qt 项目链接）+ `bin/vk.exe`（命令行）
- 📅 进度：**批次 0 + 批次 1 完成**（骨架 + 核心库 + CLI，已实测合成出真 wav）
- 📋 完整计划见上一级目录的 `VoiceKit_实施计划.md`

---

## 1. 目录结构

```
VoiceKit/
├── VoiceKit.pro              聚合工程（subdirs）
├── build.bat                 一键构建（会自动 subst 盘符，见下文）
├── config/voicekit.json      配置：引擎路径、端口、超时、别名表
├── src/vk/                   VoiceKit 库
│   ├── VoiceKit.h            ★ 对外唯一头文件，别的项目只 include 这个
│   ├── vk_types.h            CharInfo / VoiceParams
│   ├── vk_config.*           配置载入 + 引擎目录自动探测
│   ├── vk_apiclient.*        GPT-SoVITS HTTP 接口封装（同步）
│   ├── vk_engine.*           引擎进程管理（QProcess + 启动锁 + 脱离）
│   ├── vk_charindex.*        模型库扫描 → 354 个音色索引
│   ├── vk_script.*           剧本 → 配音任务表（说话人识别三级流水线）
│   ├── vk_pack.*             语音包烘制器（重采样/归一/ADPCM/打包）
│   └── vk_kit.*              ★ 对外门面（串行队列 + 权重记忆 + 缓存）
├── src/cli/main.cpp          vk.exe 命令行
├── bin/  vk.exe, run-vk.bat, console.bat
├── config/                   voicekit.json + tts_infer_cpu.yaml / tts_infer_gpu.yaml
└── work/ 运行时（缓存 / 日志 / 状态，可随便删）
    ├── cache/                合成结果缓存 + 角色索引缓存
    ├── logs/engine.log       引擎输出（引擎的 stdout 走这里，不走管道）
    └── weights_state.json    权重记忆（见坑 #4）
```

---

## 2. 构建

双击 `build.bat`。

> ⚠️ **必须用 build.bat，不要直接 qmake。** 项目路径含中文（`配音工具`），
> qmake 会把 Makefile 里的路径写成 `????` 导致编译失败。
> build.bat 里用 `subst Y: "路径"` 把项目映射成 ASCII 盘符后再编译 —— 这是实测过的唯一可行办法。

命令行手动构建（等价）：

```bash
subst Y: "D:\zhiyudalao123\配音工具"
export PATH=/f/Qt/Tools/mingw730_64/bin:/f/Qt/5.14.2/mingw73_64/bin:$PATH
cd /y/VoiceKit/build && qmake.exe ../VoiceKit.pro && mingw32-make -j4
```

运行 `vk.exe` 要用 `bin\run-vk.bat`（MinGW 版 Qt 必须把 Qt 的 bin 加进 PATH，否则缺 DLL）。

---

### 快捷键

| 键 | 动作 |
|---|---|
| `Ctrl+回车` / `Ctrl+小键盘回车` | 合成（跑着的时候 = 停止） |
| `Ctrl+P` | 试听 |
| `Ctrl+M` | 合并成一段 |
| `Ctrl+L` | 清空日志 |
| `F5` | 探活引擎 |

**双击角色名 = 直接听他的原声**（挑音色最快的路）。

---

## 3. 命令行用法

### 拼成一整段（`vk join`）

单句合成出来是一句一个文件，交付/做视频需要**一整段**。`vk join` 把多句拼成一条音轨：

```bat
:: 明确列出（顺序会按文件名里的数字自动排好，不用你操心）
vk join 0001_阿贝多.wav 0002_雷电将军.wav ... -o 整段.wav

:: 或者从目录里挑（--match 只取文件名含该子串的）
vk join --dir work\gui\batch -o 整段.wav --match 阿贝多 --gap 280
```

- `--gap 280` 是**句与句之间的静音毫秒**（默认 280，给对白留口气；`0` = 紧贴）
- **顺序按文件名里的数字排**（`0009` 在 `0010` 前），**这就是成品的顺序** ——
  先用 `vk script` + `vk synth` 生成带编号的音频，顺序天然就对
- **不重编码**：参数一致的 wav 直接拼 PCM 字节，**不损失任何音质**，秒级完成
  （实测 6 句 22.8s：时长 = 单句和 + 间隔，分毫不差）
- 参数不一致时**直接报错**，不会偷偷重采样（会掉音质，这事你得先知道）

**界面上**：右栏底部「**⛓ 合并成一段**」，列表里 **Ctrl+点 / Shift+点** 选几句
（一条不选 = 全部），输出 `整段_月日_时分秒.wav`，自动进列表、自动播放。

### 全部命令

```bat
vk info                                    :: 看环境 / 模型库
vk list                                    :: 列出所有角色
vk ping                                    :: 引擎在不在
vk engine start|stop|restart               :: 起停引擎
vk say -c 钟离 -t "契约，必须履行。" -o a.wav  :: 合成一句
vk script 剧本.txt -o 任务表.json --default-char 七七  :: 剧本 → 任务表
vk synth --table 任务表.json -o 音频目录    :: 批量合成（可断点续、按角色排序）
vk pack --table 任务表.json -o 输出目录 --spec embedded  :: 打包语音包
vk join a.wav b.wav -o 整段.wav --gap 280  :: 多句拼成一整段

```
vk info                              配置 + 索引概况（哪个作品多少音色）
vk list [关键词]                      列角色
vk ping                              引擎探活
vk engine start|stop|status          起停引擎（start 会等就绪，首次 2~5 分钟）
vk say -c 钟离 -t "契约，必须履行。"   合成一句话
vk say -c 钟离 -t "天动万象。" -o out.wav --speed 1.05 --no-cache

# 剧本 → 语音包 全链路
vk script 剧本.txt -o table.json     识别角色 → 任务表（可手工改）
vk synth  --table table.json -o 音频目录        批量合成（自动续做、按角色排序）
vk synth  --table table.json --dry-run          只看计划不合成
vk pack   --table table.json -o 输出包 --spec embedded    打包（单片机档）
vk pack   --dir wav目录 -o 输出包 --spec pc               打包（通用档）
```

**从剧本到单片机包，三条命令**：

```
vk script 剧本.txt -o table.json          # 每行是哪个角色 → 任务表
vk synth  --table table.json -o audio/    # 批量合成（可中断，重跑自动续）
vk pack   --table table.json -o pack/ --spec embedded   # → voice.bin + voice_index.h
```

`vk synth` 的两个细节：**按角色排序**（同一个角色连着做，省掉来回切权重的 40 秒）、
**每合成一句就写回任务表**（中途 Ctrl+C 不丢进度，重跑自动跳过已完成的）。

`--rescan` 强制重扫模型库；`-f <path>` 指定别的配置文件。

---

## 图形界面（`bin\工作台.bat`）

不想敲命令就双击 `bin\工作台.bat`（等于 `bin\vk_gui.exe`）。

- **左边**选角色（按作品分组，上面可以搜），**中间**写台词，**点合成**。
  合成完自动播放（可以关）。**Ctrl+点 / Shift+点可多选角色**（多人对话）
- 台词框**两种写法都认，程序自己判断**：
  - **同一个角色的多句台词**（每行一句）→ 直接批量合成
  - **剧本格式**（`甲：xxx` / `【场景：…】` / `（动作）`）→ 自动先识别角色、再接着合成。
    认不出的说话人（甲/乙 这类代称）用你当前选中的音色兜底念，**标注会被剥掉**
    （不会把"甲：""（叹气）"念出来），同时在日志里标出来给你看
- **跑批量时**：出现进度条 `3 / 7`，右上角"已用 1:23"每秒在走。
  **主按钮会原地变成红色的「■ 停止」** —— 点了就停，**已经生成的音频全部保留**，
  重新点合成会接着做（完成的自动跳过）
- **`🔊 听原声`**：播当前角色的参考音频。挑音色先听原声，比合成一句试快十几秒
- **`已合成的音频`（右栏）**：点一下就播，每条显示 `角色 · 「台词」· 5.0s · 148KB`。
  说明存在 `work/gui/audio_index.json`，**重启程序照样认得**（不然就退化成看不懂的文件名）
- 要精确控制就用工具栏「**打开剧本**」：认角色 → ① 识别 → ② 批量合成 → ③ 打包，一条流水线
- 上边工具栏是整条流水线：`打开剧本 → ① 识别角色 → ② 批量合成 → ③ 打包语音包`
- 语速滑块 0.60~1.60；情绪下拉走的是剧本同一套词表（`vk_script.cpp` 的 `kEmoTable`）

**界面样式在 `config/style.qss`**，改它不用重新编译，重开程序就生效。
深色 + 紫（`#c084fc`）/ 蓝（`#0ea5e9`）点缀。

GUI 是 `vk.exe` 的外壳（同一个 `bin` 目录里），所有耗时动作都丢给子进程跑，
所以**合成 35 秒的时候界面不会卡死**，日志边跑边刷。
GUI 崩了也不会把正在跑的任务带走。

**出问题先看 `work/gui/qt.log`** —— GUI 是 windows 子系统没有控制台，
Qt 自己的警告（比如样式表语法错）默认直接丢掉，这个文件把它们兜住了。
（曾经的表象：QSS 写错了 → 界面白白的 → 一点提示都没有。）

---

## 4. 在别的 Qt 项目里用（这是做这个库的意义）

```cpp
#include "VoiceKit.h"

vk::Kit &kit = vk::Kit::instance();
if (!kit.init())                       // 读配置 + 建索引（首次约 1s，之后走缓存）
    return;

if (!kit.ensureEngine())               // 已在跑就复用；没跑就拉起来等就绪
    return;

QString wav = kit.saySync("契约，必须履行。", "钟离");
// wav 是文件绝对路径，QSoundEffect / QMediaPlayer 直接能播
```

`.pro` 里：

```qmake
QT += core network
INCLUDEPATH += <VoiceKit>/src/vk
LIBS        += -L<VoiceKit>/lib -lVoiceKit
```

引擎目录和模型库目录：程序放在 `配音工具\` 下任意层级都能自动探测到；
放到别处就在 `config/voicekit.json` 里写绝对路径，或调 `kit.init("配置路径")`。

---

## 5. 实测性能（GTX1650 4G + 嵌入式 Python 3.11.10）

| 操作 | 耗时 | 说明 |
|---|---|---|
| 引擎冷启动（GPU 模式） | **180 ~ 300 s** | 加载 BERT-large + CNHuBERT + 权重 |
| 引擎冷启动（**CPU 模式**） | **~120 s** | 反而更快，省掉 CUDA 初始化 |
| 首次合成 | **~116 s** | 引擎刚起来时第一次合成还要额外加载 |
| 后续合成（同角色，GPU） | **4.4 s** | 正常速度 |
| 后续合成（同角色，**CPU**） | **~26 s** | 权重已切好、引擎已热身时的实测值 |
| 缓存命中 | **~0.6 s** | 完全不碰引擎 |
| 切换角色权重 | ~40 s | 所以同角色连续合成才划算 |

**结论：引擎必须常驻。** 每次冷启动两分钟起，用一次杀一次是自残。

### ⚠️ 当前默认跑 CPU 模式（2026-10-16 实测结论）

这台 GTX 1650 4G 上：

| 配置 | 结果 |
|---|---|
| `device: cuda` + `is_half: true`（原配置） | ❌ **输出全 NaN → 全零静音**，且不报任何错 |
| `device: cuda` + `is_half: false` | ❌ `CUDA out of memory`（差 16 MiB，被微信/腾讯会议/Edge 占掉的 387MB 卡的） |
| `device: cpu` + `is_half: false` | ✅ **正常出声**，启动 120s / 合成 ~26s |

所以 `config/voicekit.json` 的 `ttsConfigPath` 默认指向 `tts_infer_cpu.yaml`。
想要 GPU 速度：关掉微信 / 腾讯会议 / Edge，然后把 `ttsConfigPath` 改成 `tts_infer_gpu.yaml`。

---

## 6. 踩过的坑（都写进代码注释了，这里汇总）

1. **中文路径 → qmake 写坏 Makefile**。路径一律走 `subst` 映射成 ASCII 盘符。
2. **引擎输出不能走管道**。没人读管道 → 缓冲区满 → 引擎的 tqdm 阻塞；父进程退出 → 管道断 → Python 写 stdout 抛 BrokenPipeError → 引擎直接崩。**必须重定向到文件**。
3. **并发启动引擎会拖死机器**。两个 api_v2.py 同时加载模型互抢磁盘和 4G 显存，实测整机几十秒无响应。已有启动锁（`work/logs/engine.lock`）。
4. **权重切换是引擎全局状态，且拿不到"当前加载了哪套"**。所以做了权重记忆（`work/weights_state.json`），并用**引擎进程 pid** 当会话 ID —— pid 变了说明引擎重启过，记忆作废、强制重切。绝对不能用"猜"的，会音色串。
5. **零样本音色必须切回底模**。引擎里正加载着某个微调权重时，用别的参考音频做零样本，出来的还是被那个权重污染的音色。所以零样本角色会强制切到 `s1bert25hz-5kh` + `s2G2333k`。
6. **目录名是脏的**。`布洛妮娅----御姐`、`桂乃芬------------------女友` 得剥尾巴；`开拓者(男)`、`阮•梅`、`托帕&账账` 不能剥。判据：连续 2 个以上短横线。
7. **参考音频文件名带情绪前缀**。`害怕-我刚刚说的那些话…wav` 里，`害怕` 是情绪标签不是台词。判据：第一个 `-` 之前 ≤6 字且纯汉字才当标签。
8. **权重分散在两处**。原神/崩三/绝区零/蔚蓝档案的权重在角色目录里；星穹铁道的权重在引擎 `GPT_weights_v2`/`SoVITS_weights_v2`。两处都要找。
9. **同名多权重取 `_s` 最大值**。如 `鬼方佳世子` 有 e4_s96 和 e8_s192 两份，取训练步数多的。
10. **`is_half: true` 在这张 GTX 1650 上输出全 NaN → 全零静音**。这是最阴的一个：文件时长对、格式对、大小对，**一个错都不报**，只是没声音。`max_volume: -91.0 dB` 就是它的指纹。改成 `is_half: false` 即可，但 4G 显存扛不住全精度 → 只能走 CPU。
11. **引擎会反写 `tts_infer.yaml`**。每次切权重它都把当前权重路径存回去；而且会把当时的绝对路径（包括 subst 盘符 `Y:`）写进去，subst 一解除那些路径就失效，引擎静默 fallback 到底模。所以 VoiceKit 用 `-c` 传自己的配置副本，不碰原文件。
12. **引擎整包有个实现缺陷**：`api_v2.py` 的 `uvicorn.run(app="api_v2:APP")` 用字符串会重新 import 模块，而 `tts_pipeline = TTS(...)` 写在模块顶层 → **模型被加载两遍**，白吃一倍内存和启动时间。（改 `app=APP` 传对象可以省掉，但要动引擎源码，暂不动。）
13. **`.bat` 文件里绝对不能写中文**。cmd.exe 按 ANSI 码页（中文系统是 936）解析批处理文件，UTF-8 的中文注释会碎成乱码指令，表现是 `'st' is not recognized as an internal or external command`、`'本脚本里的盘符。' is not recognized` —— 脚本从那一行开始全乱，双击根本编译不了。**用 bash 跑看不出来，只有 cmd 里才炸**。三个 bat 现在都是纯 ASCII。
14. **重新编译前先把跑着的 exe 全杀掉**。否则链接器报 `cannot open output file ...\vk.exe: Permission denied`（`ld returned 1 exit status`），看着像代码错误，其实是文件被占用。工作台开着就编不了 `vk_gui.exe`，批处理跑着就编不了 `vk.exe`。
15. **引擎对"裸 `GET /`"会抛 500**（`AttributeError: 'NoneType' object has no attribute 'lower'`）—— 那个路由是 `POST /`（TTS），用 GET 打它等于参数全缺。**这不影响引擎继续服务**（单次请求失败而已），只是日志难看。VoiceKit 本身不会这么发（`vk_apiclient.cpp` 的全部请求路径都审计过：`/ping`、`/set_*`、`/control`、`POST /`），实测多半是有人在浏览器或调试工具里直接请求 `http://127.0.0.1:9880/`。验证方法：关掉所有浏览器，观察 `work/logs/engine.log` 还出不出 500。
16. **Qt 的 QSS 不支持 `letter-spacing`**（那是 CSS 属性）—— 写了不会报错，只是**静默忽略**。看在 `work/gui/qt.log` 里看到 `Unknown property letter-spacing` 才发现。同理其它 CSS 属性（`box-shadow`、`transition`）也别写。
17. **路径分隔符**：Qt 内部一律用正斜杠 `/`（`QFileInfo::absoluteFilePath()` 给的就是 `/`），而手写或用 Python 生成的 JSON 常带反斜杠 `\`。拿这种 JSON 当 key 查表**永远查不中** —— 而且特别难查：文件有数据、代码在跑、界面就是不显示。凡是跨工具交换的路径，读进来先归一化。
18. **`QStringLiteral` 编出来是 UTF-16**。想在 exe 二进制里搜中文/ASCII 字符串验证"代码有没有编进去"是搜不到的（我在这上面白折腾了一轮）。要验证就看运行时的日志输出。
19. **`QCommandLineParser::positionalArguments()` 里包含命令名本身**。别的命令都用 `--table` / `-o` 这类选项，没人碰过 positional，所以这个坑一直没暴露 —— 写 `vk join a.wav b.wav` 时第一个位置参数就是 `"join"`，会被当成文件名去打开，报"读不了"。要 `files.removeFirst()`。**凡是新命令直接读 positional 参数的，先想想第一个是不是命令名。**
20. **拼音频别上 ffmpeg**。同参数 wav 拼接是纯字节操作（搬 `data` 块 + 自造干净头），比调 ffmpeg 快几个数量级且**无重编码损失**。注意**别照抄源文件的头** —— 源里可能有 `fact`/`LIST` 块，长度字段会写错；老老实实自己写 44 字节标准头。静音间隔就是连续写 `0x00` 字节。

---

## 7. 待验证 / 待办

**待你在自己终端验证的一件事**：引擎"脱离常驻"。
`vk engine start` 跑完后 vk.exe 会退出，引擎应该还活着（下次 `vk say` 会发现它并复用）。
验证方法：开个 cmd 窗口跑 `bin\run-vk.bat engine start`，等它报就绪后关掉窗口，
再跑 `bin\run-vk.bat ping` —— 通了就说明常驻生效。
（在 Hermes 的受管终端里没法验证：它会连坐整个进程树，把引擎一起带走。）

**下一批（计划里写的）**：
- 批次 2：剧本自动识别角色（规则 + 别名 + 拼音 + LLM 兜底）
- 批次 3：语音包烘制器（重采样 / 音量归一 / 8bit / ADPCM / `voice.bin` + `voice_index.h`，给单片机用）
- 批次 4：Qt GUI 工作台
- 批次 5：LLM 兜底 + MCU 端实测播放
