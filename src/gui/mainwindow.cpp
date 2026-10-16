#include "mainwindow.h"
#include "vkrunner.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmsystem.h>      // mciSendString —— 放 wav 用
#endif

// 与 vk_script.cpp 的 kEmoTable 对应（认不出的词会退回默认参数，不会出错）
static const char* kEmotions[] = {
    "默认",   "平静",  "开心",  "兴奋",  "激动",  "大笑",  "轻笑",  "冷笑",
    "愤怒",   "生气",  "大喊",  "无奈",  "叹气",  "难过",  "悲伤",  "委屈",
    "哭",     "疑惑",  "惊讶",  "害怕",  "紧张",  "温柔",  "耳语",  "戏谑",
    "嘲讽",   "认真",  "严肃",  "疲惫",  "慵懒"
};

/*! 粗略判断这几行是"剧本"还是"同一个角色的多句台词"。
 *
 *  判据：至少两行像 `角色：台词`，或者带【】/（）舞台提示。
 *  故意做得保守 —— 判错的代价是拿错角色念一整段，比多绕一步烦人得多。
 */
static bool looksLikeScript(const QStringList& lines)
{
    static const QRegularExpression roleRe(
        QStringLiteral("^[\\x{4e00}-\\x{9fa5}A-Za-z0-9_·]{1,12}[：:]"));
    int hits = 0;
    for (const QString& l : lines) {
        if (roleRe.match(l).hasMatch()
            || l.startsWith(QStringLiteral("【"))
            || l.contains(QStringLiteral("（")))
            ++hits;
        if (hits >= 2)
            return true;
    }
    return false;
}

static QString sizeText(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1048576)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

/*! 读 wav 头算时长。合成完把时长写进列表里，
 *  用户才分得清"5 秒那句"和"1.5 秒那句"。 */
static double wavSeconds(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray d = f.read(4096);
    f.close();
    if (d.size() < 44 || d.left(4) != "RIFF")
        return 0;
    int byteRate = 0; qint64 dataLen = 0;
    int pos = 12;
    while (pos + 8 <= d.size()) {
        const QByteArray id = d.mid(pos, 4);
        const quint32 sz = quint32(uchar(d.at(pos + 4))) | (quint32(uchar(d.at(pos + 5))) << 8)
                         | (quint32(uchar(d.at(pos + 6))) << 16) | (quint32(uchar(d.at(pos + 7))) << 24);
        if (id == "fmt " && pos + 8 + 16 <= d.size())
            byteRate = int(uchar(d.at(pos + 8 + 8))) | (int(uchar(d.at(pos + 9 + 8))) << 8)
                     | (int(uchar(d.at(pos + 10 + 8))) << 16) | (int(uchar(d.at(pos + 11 + 8))) << 24);
        if (id == "data")
            dataLen = sz;
        pos += 8 + int(sz) + (int(sz) & 1);
    }
    if (byteRate <= 0)
        return 0;
    return double(dataLen) / byteRate;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    const QString binDir = QCoreApplication::applicationDirPath();
    m_vkExe      = binDir + QStringLiteral("/vk.exe");
    m_projectDir = QDir::cleanPath(binDir + QStringLiteral("/.."));
    m_workDir    = QDir::cleanPath(m_projectDir + QStringLiteral("/work"));
    m_guiDir     = QDir::cleanPath(m_workDir + QStringLiteral("/gui"));
    QDir().mkpath(m_guiDir);

    m_runner = new VkRunner(this);
    m_runner->setProgram(m_vkExe);
    m_runner->setWorkDir(m_projectDir);
    connect(m_runner, &VkRunner::line, this, &MainWindow::onRunnerLine);
    connect(m_runner, &VkRunner::done, this, &MainWindow::onRunnerDone);


    buildUi();
    buildMenus();
    applyStyle();
    loadIndex();
    loadAudioMeta();                                                 // 先把说明读回来
    QTimer::singleShot(1200, this, &MainWindow::refreshAudioList);   // 窗口起来后再扫

    // 剪对白是高频操作，快捷键必须有（Ctrl+回车 合成、Ctrl+P 试听、
    // Ctrl+M 合并、Ctrl+L 清日志、F5 探活）
    // 剪对白是高频操作，快捷键必须有。
    // 注意：QShortcut 那个能直接接成员函数的构造是 Qt 5.15+ 才有的，5.14 得自己 connect。
    auto addKey = [this](const QKeySequence& k, void (MainWindow::*fn)()) {
        QShortcut* sc = new QShortcut(k, this);
        connect(sc, &QShortcut::activated, this, fn);
    };
    addKey(QKeySequence(Qt::CTRL + Qt::Key_Return), &MainWindow::onSay);
    addKey(QKeySequence(Qt::CTRL + Qt::Key_Enter),  &MainWindow::onSay);
    addKey(QKeySequence(QStringLiteral("Ctrl+P")),   &MainWindow::onPreview);
    addKey(QKeySequence(QStringLiteral("Ctrl+M")),   &MainWindow::onMerge);
    addKey(QKeySequence(QStringLiteral("Ctrl+L")),   &MainWindow::onClearLog);
    addKey(QKeySequence(QStringLiteral("F5")),       &MainWindow::onEngineCheck);

    // 每秒刷"已用 xx 秒" —— 合成 13 秒、批量 7 分钟，总得让人看见时间在走
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &MainWindow::onElapsedTick);

    if (!QFileInfo::exists(m_vkExe))
        log(QStringLiteral("✗ 找不到 vk.exe（%1），先跑 build.bat").arg(m_vkExe), "err");
    else
        log(QStringLiteral("vk.exe: %1").arg(m_vkExe), "dim");

    QTimer::singleShot(300, this, &MainWindow::onEngineCheck);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_runner->busy()) {
        const auto btn = QMessageBox::question(
            this, QStringLiteral("还有任务在跑"),
            QStringLiteral("有任务正在执行，现在关掉会中断它。\n（已经合成好的部分不会丢）\n\n确定关？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    e->accept();
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
// ═══════════════════════════════════════════════════════════════════════════
//  界面布局
//  三栏：角色（左） / 台词 + 输出（中） / 已合成音频（右）
//  上一版把日志和音频塞进 Tab、工具栏拉成一长排按钮，看着像原型，
//  这版按"聊天软件"的心智来：左边找人、中间干活、右边看结果。
// ═══════════════════════════════════════════════════════════════════════════
void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("VoiceKit 配音工作台"));
    resize(1300, 820);

    QWidget* central = new QWidget;
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶栏：品牌 + 引擎状态 + 引擎操作 ──────────────────────────────
    QFrame* top = new QFrame;
    top->setObjectName(QStringLiteral("topbar"));
    top->setFixedHeight(46);
    QHBoxLayout* tl = new QHBoxLayout(top);
    tl->setContentsMargins(16, 0, 14, 0);
    tl->setSpacing(10);

    QLabel* brand = new QLabel(QStringLiteral("VoiceKit"));
    brand->setObjectName(QStringLiteral("appTitle"));
    tl->addWidget(brand);
    QLabel* sub = new QLabel(QStringLiteral("配音工作台"));
    sub->setObjectName(QStringLiteral("sectionInfo"));
    tl->addWidget(sub);
    tl->addStretch(1);

    m_engineDot = new QLabel(QStringLiteral("●"));
    m_engineDot->setObjectName(QStringLiteral("engineDot"));
    m_engineDot->setProperty("state", "unknown");      // 颜色交给 QSS（换主题时跟着变）
    tl->addWidget(m_engineDot);
    m_engineText = new QLabel(QStringLiteral("检查引擎…"));
    m_engineText->setObjectName(QStringLiteral("sectionInfo"));
    tl->addWidget(m_engineText);
    tl->addSpacing(4);

    QPushButton* btnPing = new QPushButton(QStringLiteral("探活"));
    btnPing->setObjectName(QStringLiteral("ghost"));
    connect(btnPing, &QPushButton::clicked, this, &MainWindow::onEngineCheck);
    tl->addWidget(btnPing);

    QPushButton* btnEngine = new QPushButton(QStringLiteral("启动引擎"));
    btnEngine->setObjectName(QStringLiteral("ghost"));
    connect(btnEngine, &QPushButton::clicked, this, &MainWindow::onEngineStart);
    tl->addWidget(btnEngine);
    root->addWidget(top);

    // ── 工具条：剧本流水线 ────────────────────────────────────────────
    QFrame* bar = new QFrame;
    bar->setObjectName(QStringLiteral("toolbar"));
    bar->setFixedHeight(52);
    QHBoxLayout* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(16, 0, 14, 0);
    bl->setSpacing(8);

    QLabel* flowTitle = new QLabel(QStringLiteral("剧本流水线"));
    flowTitle->setObjectName(QStringLiteral("sectionTitle"));
    bl->addWidget(flowTitle);
    bl->addSpacing(8);

    QPushButton* b0 = new QPushButton(QStringLiteral("打开剧本"));
    b0->setObjectName(QStringLiteral("step"));
    connect(b0, &QPushButton::clicked, this, &MainWindow::onOpenScript);
    bl->addWidget(b0);

    QPushButton* b1 = new QPushButton(QStringLiteral("① 识别角色"));
    b1->setObjectName(QStringLiteral("step"));
    connect(b1, &QPushButton::clicked, this, &MainWindow::onRunScript);
    bl->addWidget(b1);

    QPushButton* b2 = new QPushButton(QStringLiteral("② 批量合成"));
    b2->setObjectName(QStringLiteral("step"));
    connect(b2, &QPushButton::clicked, this, &MainWindow::onRunSynth);
    bl->addWidget(b2);

    QPushButton* b3 = new QPushButton(QStringLiteral("③ 打包语音包"));
    b3->setObjectName(QStringLiteral("step"));
    connect(b3, &QPushButton::clicked, this, &MainWindow::onRunPack);
    bl->addWidget(b3);

    bl->addStretch(1);
    QPushButton* bOpen = new QPushButton(QStringLiteral("打开包目录"));
    bOpen->setObjectName(QStringLiteral("ghost"));
    connect(bOpen, &QPushButton::clicked, this, &MainWindow::onOpenPackFolder);
    bl->addWidget(bOpen);
    root->addWidget(bar);

    // ── 三栏主体 ──────────────────────────────────────────────────────
    QSplitter* split = new QSplitter(Qt::Horizontal);
    split->setContentsMargins(12, 12, 12, 6);
    split->addWidget(buildCharPanel());
    split->addWidget(buildWorkPanel());
    split->addWidget(buildAudioPanel());
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({ 252, 620, 340 });
    split->setChildrenCollapsible(false);
    root->addWidget(split, 1);

    setCentralWidget(central);

    // ── 状态栏 ────────────────────────────────────────────────────────
    m_status = new QLabel(QStringLiteral("就绪"));
    statusBar()->addWidget(m_status);
    statusBar()->setSizeGripEnabled(true);
}

// ── 左栏：角色 ────────────────────────────────────────────────────────
QWidget* MainWindow::buildCharPanel()
{
    QFrame* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    card->setMinimumWidth(210);
    QVBoxLayout* v = new QVBoxLayout(card);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(9);

    QHBoxLayout* h = new QHBoxLayout;
    QLabel* t = new QLabel(QStringLiteral("角色"));
    t->setObjectName(QStringLiteral("sectionTitle"));
    h->addWidget(t);
    h->addStretch(1);
    m_treeStat = new QLabel(QStringLiteral("…"));
    m_treeStat->setObjectName(QStringLiteral("sectionInfo"));
    h->addWidget(m_treeStat);
    v->addLayout(h);

    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("搜索角色…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
    v->addWidget(m_search);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(12);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 多选：多人对话
    m_tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::onTreeSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onTreeDoubleClicked);
    v->addWidget(m_tree, 1);

    QLabel* tip = new QLabel(QStringLiteral("Ctrl+点 可多选（多人对话）"));
    tip->setObjectName(QStringLiteral("sectionInfo"));
    tip->setWordWrap(true);
    v->addWidget(tip);

    return card;
}

// ── 中栏：台词 + 输出 ─────────────────────────────────────────────────
QWidget* MainWindow::buildWorkPanel()
{
    QWidget* w = new QWidget;
    QVBoxLayout* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    // ── 台词卡片 ──
    QFrame* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    QVBoxLayout* cv = new QVBoxLayout(card);
    cv->setContentsMargins(14, 12, 14, 14);
    cv->setSpacing(10);

    QHBoxLayout* h1 = new QHBoxLayout;
    h1->setSpacing(10);
    QLabel* t1 = new QLabel(QStringLiteral("台词"));
    t1->setObjectName(QStringLiteral("sectionTitle"));
    h1->addWidget(t1);
    m_heroName = new QLabel(QStringLiteral("未选角色"));
    m_heroName->setObjectName(QStringLiteral("heroName"));
    h1->addWidget(m_heroName);

    QPushButton* btnRef = new QPushButton(QStringLiteral("🔊 听原声"));
    btnRef->setObjectName(QStringLiteral("ghost"));
    btnRef->setToolTip(QStringLiteral("播放这个角色的参考音频。选音色时先听原声，"
                                      "比合成一句试快十几秒。"));
    connect(btnRef, &QPushButton::clicked, this, &MainWindow::onPreviewRef);
    h1->addWidget(btnRef);

    h1->addStretch(1);
    QLabel* hint = new QLabel(QStringLiteral("多行=批量；贴剧本会自动认角色"));
    hint->setObjectName(QStringLiteral("sectionInfo"));
    h1->addWidget(hint);
    cv->addLayout(h1);

    m_text = new QPlainTextEdit;
    m_text->setPlaceholderText(QStringLiteral("先在左边选角色，再写台词。\n\n"
                                              "多行 = 批量（一行一句）。\n"
                                              "贴剧本（甲：…／【场景】／（动作））会自动识别角色并剥掉标注。"));
    m_text->setMinimumHeight(110);
    cv->addWidget(m_text, 1);

    // 参数行
    QHBoxLayout* ph = new QHBoxLayout;
    ph->setSpacing(10);

    ph->addWidget(new QLabel(QStringLiteral("语速")));
    m_speed = new QSlider(Qt::Horizontal);
    m_speed->setRange(60, 160);
    m_speed->setValue(100);
    m_speed->setFixedWidth(120);
    m_speedVal = new QLabel(QStringLiteral("1.00"));
    m_speedVal->setObjectName(QStringLiteral("speedVal"));
    m_speedVal->setFixedWidth(36);
    connect(m_speed, &QSlider::valueChanged, this, [this](int val) {
        m_speedVal->setText(QString::number(val / 100.0, 'f', 2));
    });
    ph->addWidget(m_speed);
    ph->addWidget(m_speedVal);

    ph->addSpacing(14);
    ph->addWidget(new QLabel(QStringLiteral("情绪")));
    m_emotion = new QComboBox;
    for (const char* e : kEmotions)
        m_emotion->addItem(QString::fromUtf8(e));
    m_emotion->setFixedWidth(100);
    ph->addWidget(m_emotion);

    ph->addSpacing(14);
    m_autoPlay = new QCheckBox(QStringLiteral("合成后自动播放"));
    m_autoPlay->setChecked(true);
    ph->addWidget(m_autoPlay);
    ph->addStretch(1);
    cv->addLayout(ph);

    // 进度行：平时隐藏，跑批量时才出现
    QHBoxLayout* prh = new QHBoxLayout;
    prh->setSpacing(10);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFixedHeight(8);
    m_progress->setVisible(false);
    prh->addWidget(m_progress, 1);
    m_elapsedLabel = new QLabel(QString());
    m_elapsedLabel->setObjectName(QStringLiteral("sectionInfo"));
    m_elapsedLabel->setVisible(false);
    prh->addWidget(m_elapsedLabel);
    cv->addLayout(prh);

    // 操作行：合成是唯一的主按钮
    QHBoxLayout* bh = new QHBoxLayout;
    bh->setSpacing(8);
    m_btnSay = new QPushButton(QStringLiteral("合  成"));
    m_btnSay->setObjectName(QStringLiteral("primary"));
    m_btnSay->setCursor(Qt::PointingHandCursor);
    connect(m_btnSay, &QPushButton::clicked, this, &MainWindow::onSay);
    bh->addWidget(m_btnSay, 3);

    m_btnPreview = new QPushButton(QStringLiteral("▶  试听"));
    m_btnPreview->setMinimumHeight(38);
    m_btnPreview->setMaximumHeight(38);
    connect(m_btnPreview, &QPushButton::clicked, this, &MainWindow::onPreview);
    bh->addWidget(m_btnPreview, 2);

    QPushButton* btnSave = new QPushButton(QStringLiteral("另存为…"));
    btnSave->setMinimumHeight(38);
    btnSave->setMaximumHeight(38);
    connect(btnSave, &QPushButton::clicked, this, &MainWindow::onSaveAs);
    bh->addWidget(btnSave, 2);
    cv->addLayout(bh);

    v->addWidget(card, 3);

    // ── 输出卡片 ──
    QFrame* logCard = new QFrame;
    logCard->setObjectName(QStringLiteral("card"));
    QVBoxLayout* lv = new QVBoxLayout(logCard);
    lv->setContentsMargins(14, 12, 14, 12);
    lv->setSpacing(8);

    QHBoxLayout* h2 = new QHBoxLayout;
    QLabel* t2 = new QLabel(QStringLiteral("输出"));
    t2->setObjectName(QStringLiteral("sectionTitle"));
    h2->addWidget(t2);
    h2->addStretch(1);
    QPushButton* btnClr = new QPushButton(QStringLiteral("清空"));
    btnClr->setToolTip(QStringLiteral("只清这块日志（不影响已经合成的音频）"));
    btnClr->setObjectName(QStringLiteral("subtle"));
    connect(btnClr, &QPushButton::clicked, this, &MainWindow::onClearLog);
    h2->addWidget(btnClr);
    lv->addLayout(h2);

    m_log = new QTextEdit;
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(120);
    lv->addWidget(m_log, 1);

    v->addWidget(logCard, 2);
    return w;
}

// ── 右栏：已合成的音频 ────────────────────────────────────────────────
QWidget* MainWindow::buildAudioPanel()
{
    QFrame* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    card->setMinimumWidth(250);
    QVBoxLayout* v = new QVBoxLayout(card);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(9);

    QHBoxLayout* h = new QHBoxLayout;
    QLabel* t = new QLabel(QStringLiteral("已合成的音频"));
    t->setObjectName(QStringLiteral("sectionTitle"));
    h->addWidget(t);
    h->addStretch(1);
    m_audioCount = new QLabel(QString());
    m_audioCount->setObjectName(QStringLiteral("sectionInfo"));
    h->addWidget(m_audioCount);
    v->addLayout(h);

    m_audioList = new QListWidget;
    m_audioList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_audioList->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 多选：合并成整段
    m_audioList->setToolTip(QStringLiteral("单击播放。Ctrl+点 / Shift+点 可多选，"
                                           "选中后点下面「合并成一段」拼成整轨"));
    connect(m_audioList, &QListWidget::itemClicked, this, &MainWindow::onAudioClicked);
    v->addWidget(m_audioList, 1);

    QLabel* tip = new QLabel(QStringLiteral("单击即播放。新合成的声音排在最上面。"));
    tip->setObjectName(QStringLiteral("sectionInfo"));
    tip->setWordWrap(true);
    v->addWidget(tip);

    QHBoxLayout* b = new QHBoxLayout;
    b->setSpacing(8);
    // 合并成整段：这一排里最常用的动作，给它醒目的位置和文字
    QPushButton* btnMerge = new QPushButton(QStringLiteral("⛓  合并成一段"));
    btnMerge->setObjectName(QStringLiteral("accent"));
    btnMerge->setToolTip(QStringLiteral("把选中的几条按文件名顺序拼成一整段。\n"
                                        "Ctrl+点 / Shift+点 多选；一条都不选 = 全部。\n"
                                        "句间自动留 280ms 停顿。"));
    connect(btnMerge, &QPushButton::clicked, this, &MainWindow::onMerge);
    b->addWidget(btnMerge);
    QPushButton* btnRefresh = new QPushButton(QStringLiteral("刷新"));
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshAudioList);
    b->addWidget(btnRefresh);
    QPushButton* btnFolder = new QPushButton(QStringLiteral("打开目录"));
    connect(btnFolder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_guiDir));
    });
    b->addWidget(btnFolder);
    v->addLayout(b);

    return card;
}

void MainWindow::buildMenus()
{
    QMenu* mFile = menuBar()->addMenu(QStringLiteral("文件"));
    mFile->addAction(QStringLiteral("打开剧本…"), this, &MainWindow::onOpenScript);
    mFile->addSeparator();
    mFile->addAction(QStringLiteral("打开工作目录"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_workDir));
    });
    mFile->addAction(QStringLiteral("打开输出目录"), this, &MainWindow::onOpenPackFolder);
    mFile->addSeparator();
    mFile->addAction(QStringLiteral("退出"), this, &QWidget::close);

    QMenu* mFlow = menuBar()->addMenu(QStringLiteral("流水线"));
    mFlow->addAction(QStringLiteral("① 识别角色"), this, &MainWindow::onRunScript);
    mFlow->addAction(QStringLiteral("② 批量合成"), this, &MainWindow::onRunSynth);
    mFlow->addAction(QStringLiteral("③ 打包语音包"), this, &MainWindow::onRunPack);

    QMenu* mHelp = menuBar()->addMenu(QStringLiteral("帮助"));
    mHelp->addAction(QStringLiteral("关于"), this, [this] {
        QMessageBox::about(this, QStringLiteral("关于 VoiceKit"),
            QStringLiteral(
                "<b>VoiceKit 配音工作台</b><br><br>"
                "底层是 GPT-SoVITS 引擎（Python 进程，端口 9880），"
                "本程序是它的图形前端。<br><br>"
                "命令行等价物：<code>vk.exe</code>（同一个 bin 目录）<br>"
                "完整用法看项目里的 README.md"));
    });
}

void MainWindow::applyStyle()
{
    // 样式放在 config/style.qss 里，改它不用重新编译。
    // 读不到就退回内置的最小深色（至少不会白得刺眼）。
    const QString qssPath = m_projectDir + QStringLiteral("/config/style.qss");
    QFile f(qssPath);
    if (f.open(QIODevice::ReadOnly)) {
        const QString qss = QString::fromUtf8(f.readAll());
        setStyleSheet(qss);
        log(QStringLiteral("样式：%1（%2 字符）").arg(qssPath).arg(qss.size()), "dim");
        qDebug() << "[style] loaded" << qssPath << qss.size() << "chars";
    } else {
        log(QStringLiteral("⚠ 读不到样式 %1，用内置深色").arg(qssPath), "warn");
        qDebug() << "[style] FAILED to open" << qssPath << f.errorString();
        setStyleSheet(QStringLiteral(
            "QWidget{background:#16161d;color:#d4d4de;}"
            "QPushButton{background:#26263a;border:1px solid #34344a;padding:6px 16px;}"
            "QPushButton#primary{background:#c084fc;color:#14141c;font-weight:bold;}"));
    }
}

// ---------------------------------------------------------------------------
// 角色索引 → 树
// ---------------------------------------------------------------------------
void MainWindow::loadIndex()
{
    const QString idx = m_workDir + QStringLiteral("/cache/charindex.json");
    QFile f(idx);
    if (!f.open(QIODevice::ReadOnly)) {
        log(QStringLiteral("✗ 读不到角色索引 %1，先跑一次 vk info 生成").arg(idx), "err");
        m_treeStat->setText(QStringLiteral("没有索引"));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();

    const QJsonArray chars = root.value(QStringLiteral("chars")).toArray();
    m_tree->clear();

    QHash<QString, QTreeWidgetItem*> games;
    QStringList gameOrder;
    for (const QJsonValue& v : chars) {
        const QJsonObject o = v.toObject();
        const QString game = o.value(QStringLiteral("game")).toString();
        const QString name = o.value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        if (!games.contains(game)) {
            auto* gi = new QTreeWidgetItem(m_tree);
            gi->setText(0, game);
            gi->setFlags(gi->flags() & ~Qt::ItemIsSelectable);
            QFont gf = gi->font(0);
            gf.setBold(true);
            gi->setFont(0, gf);
            gi->setForeground(0, QBrush(QColor("#c084fc")));
            gi->setExpanded(true);
            games.insert(game, gi);
            gameOrder << game;
        }
        auto* ci = new QTreeWidgetItem(games.value(game));
        ci->setText(0, name);
        ci->setData(0, Qt::UserRole, game);
        ci->setData(0, Qt::UserRole + 1, name);
        const QString refAudio = o.value(QStringLiteral("refAudio")).toString();
        if (!refAudio.isEmpty())
            m_charRefAudio.insert(name, refAudio);
        // 参考音频的文本当 tooltip，方便判断这个音色是什么调
        const QString pt = o.value(QStringLiteral("promptText")).toString();
        if (!pt.isEmpty())
            ci->setToolTip(0, QStringLiteral("参考音频文本：%1").arg(pt.left(60)));
    }
    for (const QString& g : gameOrder) {
        QTreeWidgetItem* gi = games.value(g);
        gi->setText(0, QStringLiteral("%1  (%2)").arg(g).arg(gi->childCount()));
    }
    m_treeStat->setText(QStringLiteral("%1 个音色 / %2 个作品")
                            .arg(chars.size()).arg(gameOrder.size()));
    log(QStringLiteral("索引载入：%1 个音色").arg(chars.size()), "dim");
}

void MainWindow::onSearchChanged(const QString& text)
{
    const QString key = text.trimmed();
    int totalVisible = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* gi = m_tree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < gi->childCount(); ++j) {
            QTreeWidgetItem* ci = gi->child(j);
            const QString name = ci->data(0, Qt::UserRole + 1).toString();
            const bool hit = key.isEmpty() || name.contains(key, Qt::CaseInsensitive);
            ci->setHidden(!hit);
            if (hit)
                ++visible;
        }
        gi->setHidden(visible == 0);
        if (!key.isEmpty() && visible > 0)
            gi->setExpanded(true);
        totalVisible += visible;
    }

    // 搜不到的时候别让列表莫名其妙空着 —— 他会以为是程序卡了
    if (!key.isEmpty()) {
        if (totalVisible == 0)
            m_status->setText(QStringLiteral("没有匹配「%1」的角色").arg(key));
        else
            m_status->setText(QStringLiteral("匹配 %1 个角色").arg(totalVisible));
    }
}

void MainWindow::onTreeSelectionChanged()
{
    const QStringList sel = selectedChars();
    if (m_heroName) {
        if (sel.isEmpty())
            m_heroName->setText(QStringLiteral("未选角色"));
        else if (sel.size() == 1)
            m_heroName->setText(QStringLiteral("%1  ·  %2").arg(sel.first(), currentGame()));
        else
            m_heroName->setText(QStringLiteral("多人：%1").arg(sel.join(QStringLiteral(" / "))));
    }
    if (!sel.isEmpty())
        m_status->setText(QStringLiteral("已选 %1 个角色").arg(sel.size()));
}

void MainWindow::onTreeDoubleClicked(QTreeWidgetItem* item, int)
{
    if (!item || !item->parent())
        return;                       // 双击分组头（作品名）不管
    // 双击角色名，心里想的就是"让我听听他什么声音"，别再只是 setFocus 了
    onPreviewRef();
}

QString MainWindow::currentChar() const
{
    const QStringList s = selectedChars();
    return s.isEmpty() ? QString() : s.first();
}

/*! 树上所有选中的角色，按树的顺序。用于"多人对话"：
 *  未匹配的说话人（甲/乙）会按出现顺序分到这些角色上。 */
QStringList MainWindow::selectedChars() const
{
    QStringList out;
    const QList<QTreeWidgetItem*> sel = m_tree->selectedItems();
    for (QTreeWidgetItem* it : sel) {
        if (it->parent())          // 只取角色行，作品分组行不算
            out << it->data(0, Qt::UserRole + 1).toString();
    }
    return out;
}

QString MainWindow::currentGame() const
{
    QTreeWidgetItem* it = m_tree->currentItem();
    if (!it)
        return QString();
    if (it->parent())
        return it->parent()->text(0).section(QStringLiteral("  ("), 0, 0);
    return it->text(0).section(QStringLiteral("  ("), 0, 0);
}

// ---------------------------------------------------------------------------
// 合成 / 试听
// ---------------------------------------------------------------------------
void MainWindow::onSay()
{
    // 跑着的时候这个按钮显示的是"停止"，所以这里直接掐任务
    if (m_runner->busy()) {
        onStop();
        return;
    }
    const QString ch = currentChar();
    if (ch.isEmpty()) {
        log(QStringLiteral("⚠ 先在左边选一个角色"), "warn");
        return;
    }
    const QString raw = m_text->toPlainText().trimmed();
    if (raw.isEmpty()) {
        log(QStringLiteral("⚠ 台词是空的"), "warn");
        return;
    }

    QStringList lines = raw.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                  QString::SkipEmptyParts);
    for (QString& s : lines)
        s = s.trimmed();
    lines.removeAll(QString());

    const QString emo = (m_emotion->currentText() == QStringLiteral("默认"))
                            ? QString()
                            : m_emotion->currentText();
    const double speed = m_speed->value() / 100.0;

    if (lines.size() == 1) {
        // ── 单句：直接 say ─────────────────────────────
        const QString out = QStringLiteral("%1/%2_%3.wav")
                                .arg(m_guiDir,
                                     QDateTime::currentDateTime().toString(QStringLiteral("MMdd_hhmmss")),
                                     ch);
        QStringList args{ QStringLiteral("say"), QStringLiteral("-c"), ch,
                          QStringLiteral("-t"), lines.first(),
                          QStringLiteral("-o"), out };
        if (!emo.isEmpty())
            args << QStringLiteral("--emotion") << emo;
        if (qAbs(speed - 1.0) > 0.005)
            args << QStringLiteral("--speed") << QString::number(speed, 'f', 2);
        log(QStringLiteral("▶ %1「%2」%3").arg(ch, lines.first().left(40),
                                              emo.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(emo)));
        m_pendingText = lines.first();      // 合成完写进列表，方便认出这是哪句
        m_pendingChar = ch;
        m_runner->run(args);
    } else {
        // ── 多行 ────────────────────────────────────────────────
        const QString tblPath = m_guiDir + QStringLiteral("/quick_table.json");
        const QString batchDir = m_guiDir + QStringLiteral("/batch");

        if (looksLikeScript(lines)) {
            // 像剧本（角色：台词 / 【】 / （））→ 先识别角色再合成。
            // 注意这些行里"甲：""（叹气）""【场景…】"都不该被念出来，
            // 走 script 才会剥掉它们。
            const QString txtPath = m_guiDir + QStringLiteral("/quick_script.txt");
            QFile sf(txtPath);
            if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                log(QStringLiteral("✗ 写临时剧本失败：%1").arg(sf.errorString()), "err");
                return;
            }
            QByteArray raw;
            for (const QString& l : lines)
                raw += l.toUtf8() + '\n';
            sf.write(raw);
            sf.close();

            const QStringList chars = selectedChars();
            const QString dcs = chars.join(QStringLiteral(","));

            log(QStringLiteral("检测到剧本格式（%1 行）→ 先识别角色，再自动接着合成").arg(lines.size()));
            if (chars.size() > 1)
                log(QStringLiteral("多人模式：认不出的说话人按出现顺序分到 %1")
                        .arg(chars.join(QStringLiteral(" → "))));
            else
                log(QStringLiteral("认不出的说话人（甲/乙/路人）会用「%1」兜底念").arg(ch), "dim");
            log(QStringLiteral("标注（甲：／【场景】／（动作））会被剥掉，不会念出来"), "dim");

            m_chain.clear();
            m_chain << (QStringList{ QStringLiteral("synth"), QStringLiteral("--table"), tblPath,
                                     QStringLiteral("-o"), batchDir });
            m_runner->run(QStringList{ QStringLiteral("script"), txtPath,
                                       QStringLiteral("-o"), tblPath,
                                       QStringLiteral("--default-char"), dcs });
            return;
        }

        // 不是剧本：当成"同一个角色的多句台词"
        QString err;
        if (!writeQuickTable(ch, lines, tblPath, &err)) {
            log(QStringLiteral("✗ 写临时任务表失败：%1").arg(err), "err");
            return;
        }
        log(QStringLiteral("▶ 批量 %1 句（%2）").arg(lines.size()).arg(ch));
        m_runner->run(QStringList{ QStringLiteral("synth"), QStringLiteral("--table"), tblPath,
                                   QStringLiteral("-o"), batchDir });
    }
}

/*! 停止正在跑的任务。已生成的音频全部保留 ——
 *  vk synth 本来就是每句落盘，重新点合成会自动跳过已完成的。 */
void MainWindow::onStop()
{
    if (!m_runner->busy())
        return;
    m_runner->cancel();
    log(QStringLiteral("已停止。已经生成的文件都保留着，"
                       "重新点「合成」会接着做（完成的会自动跳过）。"), "warn");
}

void MainWindow::onElapsedTick()
{
    if (!m_elapsedLabel)
        return;
    m_elapsedLabel->setVisible(true);
    const int sec = int(m_runClock.elapsed() / 1000);
    m_elapsedLabel->setText(QStringLiteral("已用 %1:%2")
                                .arg(sec / 60).arg(sec % 60, 2, 10, QLatin1Char('0')));
}

/*! 把选中的多句拼成一整段。
 *  GUI 不自己拼（那要把 vk_join 链进 GUI），而是让 vk.exe 去干 ——
 *  和别的耗时动作一条路，日志能看见、能停止、崩了也不带走界面。 */
void MainWindow::onMerge()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 正在跑任务，等它结束（或点「停止」）"), "warn");
        return;
    }

    const QList<QListWidgetItem*> sel = m_audioList->selectedItems();
    const bool all = sel.isEmpty();
    QStringList picked;
    for (int i = 0; i < m_audioList->count(); ++i) {
        QListWidgetItem* it = m_audioList->item(i);
        if (!all && !it->isSelected())
            continue;
        const QString p = it->data(Qt::UserRole).toString();
        // 只收 wav，且排除以前合并出来的整段（免得整段套整段，越套越大）
        if (p.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)
            && !QFileInfo(p).fileName().startsWith(QStringLiteral("整段_")))
            picked << p;
    }

    if (picked.size() < 2) {
        log(QStringLiteral("⚠ 至少要 2 条才能合并。**Ctrl+点 / Shift+点** 选出这一场戏的几句，"
                           "或者一条都不选 = 全部。"), "warn");
        return;
    }

    const QString out = QStringLiteral("%1/整段_%2.wav")
                            .arg(m_guiDir,
                                 QDateTime::currentDateTime().toString(QStringLiteral("MMdd_HHmmss")));

    QStringList args{ QStringLiteral("join") };
    args << picked
         << QStringLiteral("-o")  << out
         << QStringLiteral("--gap") << QStringLiteral("280");

    log(QStringLiteral("▶ 合并 %1 条 → %2（按文件名顺序拼）")
            .arg(picked.size()).arg(QFileInfo(out).fileName()), "ok");
    m_pendingText.clear();          // 整段不挂"某一句"的说明
    m_pendingChar.clear();
    m_runner->run(args);
}

/*! 播当前角色的参考音频。挑音色时先听原声 —— 比合成一句试快十几秒。 */
void MainWindow::onPreviewRef()
{
    const QString ch = currentChar();
    if (ch.isEmpty()) {
        log(QStringLiteral("⚠ 先在左边选一个角色"), "warn");
        return;
    }
    const QString ref = m_charRefAudio.value(ch);
    if (ref.isEmpty() || !QFileInfo::exists(ref)) {
        log(QStringLiteral("「%1」没有可用的参考音频").arg(ch), "err");
        return;
    }
    log(QStringLiteral("▶ 原声参考「%1」：%2").arg(ch, QFileInfo(ref).fileName()), "dim");
    playWav(ref);
}

/*! 当前该播/该存哪一条：列表里选中的优先（单选 = 就是这条），
 *  没选就退回最近合成/最近点过的那条。 */
QString MainWindow::pickedAudio() const
{
    const QList<QListWidgetItem*> sel = m_audioList->selectedItems();
    if (sel.size() == 1)
        return sel.first()->data(Qt::UserRole).toString();
    return m_lastWav;
}

void MainWindow::onPreview()
{
    const QString w = pickedAudio();
    if (w.isEmpty() || !QFileInfo::exists(w)) {
        log(QStringLiteral("⚠ 没有可试听的音频（先合成，或在右边列表里点一条）"), "warn");
        return;
    }
    playWav(w);
}

void MainWindow::playWav(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        log(QStringLiteral("✗ 文件不在：%1").arg(path), "err");
        return;
    }
#ifdef Q_OS_WIN
    // 用 Win32 的 MCI 播 —— 不用 QMediaPlayer。
    // 原因：MinGW 上它依赖 DirectShow 后端插件，失败时是"点了没反应也不报错"，
    // 跟 QSS 静默失败一个德性，很难查。MCI 是系统自带，播 wav 稳。
    mciSendStringW(L"close vkvoice", nullptr, 0, nullptr);   // 先关上一首
    const QString openCmd = QStringLiteral("open \"%1\" type waveaudio alias vkvoice")
                                .arg(QDir::toNativeSeparators(path));
    const MCIERROR rc = mciSendStringW(reinterpret_cast<LPCWSTR>(openCmd.utf16()),
                                       nullptr, 0, nullptr);
    if (rc != 0) {
        wchar_t buf[256] = {0};
        mciGetErrorStringW(rc, buf, 255);
        log(QStringLiteral("✗ 播放失败：%1").arg(QString::fromWCharArray(buf)), "err");
        return;
    }
    mciSendStringW(L"play vkvoice", nullptr, 0, nullptr);
    m_status->setText(QStringLiteral("▶ %1").arg(QFileInfo(path).fileName()));
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
#endif
}

/*! 列表里那句"角色 · 「台词」· 5.0s"要活过重启 ——
 *  只放内存的话，重开程序就退回成一堆看不懂的文件名了。 */
void MainWindow::loadAudioMeta()
{
    QFile f(m_guiDir + QStringLiteral("/audio_index.json"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    m_audioMeta.clear();
    for (auto it = o.begin(); it != o.end(); ++it) {
        // 统一成正斜杠：Qt 的 QFileInfo::absoluteFilePath() 给的是 "/"，
        // 而手写/外部工具生成的 json 常常是 "\"，不归一化就永远查不中
        QString k = it.key();
        k.replace(QLatin1Char('\\'), QLatin1Char('/'));
        m_audioMeta.insert(k, it.value().toString());
    }
    if (!m_audioMeta.isEmpty())
        log(QStringLiteral("已载入 %1 条音频说明").arg(m_audioMeta.size()), "dim");
}

void MainWindow::saveAudioMeta()
{
    QJsonObject o;
    for (auto it = m_audioMeta.begin(); it != m_audioMeta.end(); ++it)
        o.insert(it.key(), it.value());
    QFile f(m_guiDir + QStringLiteral("/audio_index.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.close();
    }
}

/*! 扫 work/gui 下所有 wav（含子目录），按修改时间倒序。
 *  这是"先听听已经合成过什么"的入口 —— 会话跑完/中断后都不用去翻资源管理器。 */
void MainWindow::refreshAudioList()
{
    if (!m_audioList)
        return;

    QList<QFileInfo> files;
    QDirIterator it(m_guiDir, QStringList{ QStringLiteral("*.wav") },
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        files << it.fileInfo();
    }
    std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() > b.lastModified();
    });

    m_audioList->clear();
    for (const QFileInfo& fi : files) {
        const QString sub = QDir(m_guiDir).relativeFilePath(fi.absolutePath());
        const QString label = (sub == QStringLiteral("."))
                                  ? fi.fileName()
                                  : QStringLiteral("%1/%2").arg(sub, fi.fileName());
        const QString meta = m_audioMeta.value(fi.absoluteFilePath());
        auto* item = new QListWidgetItem(
            meta.isEmpty() ? QStringLiteral("%1 · %2").arg(label, sizeText(fi.size()))
                           : QStringLiteral("%1 · %2").arg(meta, sizeText(fi.size())));
        item->setData(Qt::UserRole, fi.absoluteFilePath());
        item->setToolTip(fi.absoluteFilePath());
        m_audioList->addItem(item);
    }
    if (m_audioCount)
        m_audioCount->setText(files.isEmpty() ? QString()
                                              : QStringLiteral("%1 个").arg(files.size()));
    if (files.isEmpty())
        log(QStringLiteral("（%1 下还没有 wav）").arg(m_guiDir), "dim");
}

void MainWindow::onAudioClicked(QListWidgetItem* item)
{
    if (!item)
        return;
    const QString path = item->data(Qt::UserRole).toString();
    m_lastWav = path;
    playWav(path);
}

void MainWindow::onSaveAs()
{
    const QString wav = pickedAudio();
    if (wav.isEmpty() || !QFileInfo::exists(wav)) {
        log(QStringLiteral("⚠ 没有可保存的音频（先合成，或在右边列表里点一条）"), "warn");
        return;
    }
    const QString def = QDir::homePath() + QStringLiteral("/")
                        + QStringLiteral("%1_%2.wav")
                              .arg(currentChar(),
                                   QDateTime::currentDateTime().toString(QStringLiteral("MMdd_hhmmss")));
    const QString to = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"),
                                                    def, QStringLiteral("WAV (*.wav)"));
    if (to.isEmpty())
        return;
    QFile::remove(to);
    if (QFile::copy(wav, to))
        log(QStringLiteral("已保存：%1").arg(to), "ok");
    else
        log(QStringLiteral("✗ 保存失败（目标可能被占用）"), "err");
}

void MainWindow::onClearLog()
{
    m_log->clear();
}

QString MainWindow::quickTablePath() const
{
    return m_guiDir + QStringLiteral("/quick.json");
}

bool MainWindow::writeQuickTable(const QString& charName, const QStringList& lines,
                                 const QString& outPath, QString* err)
{
    const QString emo = (m_emotion->currentText() == QStringLiteral("默认"))
                            ? QString()
                            : m_emotion->currentText();
    QJsonArray arr;
    for (int i = 0; i < lines.size(); ++i) {
        QJsonObject o;
        o.insert(QStringLiteral("i"), i);
        o.insert(QStringLiteral("speaker"), charName);
        o.insert(QStringLiteral("char"), charName);
        o.insert(QStringLiteral("resolved"), true);
        o.insert(QStringLiteral("text"), lines.at(i));
        o.insert(QStringLiteral("emotion"), emo);
        o.insert(QStringLiteral("narration"), false);
        o.insert(QStringLiteral("audio"), QString());
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("source"), QStringLiteral("gui-quick"));
    root.insert(QStringLiteral("lines"), arr);

    QFile f(outPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// 剧本流水线
// ---------------------------------------------------------------------------
void MainWindow::onOpenScript()
{
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开剧本"), m_guiDir,
        QStringLiteral("文本 (*.txt *.md *.csv);;所有文件 (*.*)"));
    if (f.isEmpty())
        return;
    m_scriptPath = f;
    m_tablePath  = m_guiDir + QStringLiteral("/") + QFileInfo(f).completeBaseName()
                   + QStringLiteral("_table.json");
    log(QStringLiteral("剧本：%1").arg(f), "ok");
    log(QStringLiteral("任务表将写到：%1").arg(m_tablePath), "dim");
    onRunScript();
}

void MainWindow::onRunScript()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 上一个任务还在跑"), "warn");
        return;
    }
    // 平时他是直接把剧本贴进台词框的，所以这里也得认 ——
    // 只认"打开剧本"选进来的文件路径，等于把他最顺的那条路掐死了。
    QString scriptPath = m_scriptPath;
    if (scriptPath.isEmpty()) {
        const QString pasted = m_text->toPlainText().trimmed();
        if (pasted.isEmpty()) {
            log(QStringLiteral("⚠ 还没打开剧本，台词框也是空的。"
                               "把剧本粘进台词框，或用「打开剧本」选个文件。"), "warn");
            return;
        }
        scriptPath = m_guiDir + QStringLiteral("/_inline_script.txt");
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            log(QStringLiteral("✗ 写不了临时剧本：%1").arg(f.errorString()), "err");
            return;
        }
        f.write(pasted.toUtf8());
        f.close();
        log(QStringLiteral("台词框里有内容，当成剧本识别（%1 行）")
                .arg(pasted.count(QLatin1Char('\n')) + 1), "dim");
    }
    if (m_tablePath.isEmpty())
        m_tablePath = m_guiDir + QStringLiteral("/_inline_table.json");

    log(QStringLiteral("① 识别角色 …"));
    m_runner->run({ QStringLiteral("script"), scriptPath,
                    QStringLiteral("-o"), m_tablePath });
}

void MainWindow::onRunSynth()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 上一个任务还在跑"), "warn");
        return;
    }
    if (m_tablePath.isEmpty() || !QFileInfo::exists(m_tablePath)) {
        log(QStringLiteral("⚠ 还没有任务表，先做 ① 识别角色"), "warn");
        return;
    }
    log(QStringLiteral("② 批量合成（会按角色排序，未匹配的自动跳过）…"));
    m_runner->run({ QStringLiteral("synth"), QStringLiteral("--table"), m_tablePath,
                    QStringLiteral("-o"), m_guiDir + QStringLiteral("/audio") });
}

void MainWindow::onRunPack()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 上一个任务还在跑"), "warn");
        return;
    }
    if (m_tablePath.isEmpty() || !QFileInfo::exists(m_tablePath)) {
        log(QStringLiteral("⚠ 还没有任务表"), "warn");
        return;
    }
    log(QStringLiteral("③ 打包语音包（PC 档 · 16k 通用）…"));
    log(QStringLiteral("    要单片机档（8k/ADPCM，体积最小）用命令行："
                       "vk pack --table … --spec embedded"), "dim");
    m_runner->run({ QStringLiteral("pack"), QStringLiteral("--table"), m_tablePath,
                    QStringLiteral("-o"), m_guiDir + QStringLiteral("/pack"),
                    QStringLiteral("--spec"), QStringLiteral("pc") });
}

void MainWindow::onOpenPackFolder()
{
    // 以前打开的是 work/gui（工作目录），按钮却叫"打开包目录" —— 名不副实。
    const QString packDir = m_guiDir + QStringLiteral("/pack");
    if (!QFileInfo::exists(packDir)) {
        log(QStringLiteral("⚠ 还没有打包过。先点「③ 打包语音包」。"), "warn");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(packDir));
}

// ---------------------------------------------------------------------------
// 引擎
// ---------------------------------------------------------------------------
void MainWindow::onEngineCheck()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 有任务在跑，等它结束（或点「停止」）再探活"), "warn");
        return;
    }
    m_runner->run({ QStringLiteral("ping") });
}

void MainWindow::onEngineStart()
{
    if (m_runner->busy()) {
        log(QStringLiteral("⚠ 有任务在跑"), "warn");
        return;
    }
    log(QStringLiteral("启动引擎…首次要 2~5 分钟（要加载模型），之后就快了"));
    m_runner->run({ QStringLiteral("engine"), QStringLiteral("start") });
}

// ---------------------------------------------------------------------------
// 任务回调
// ---------------------------------------------------------------------------
void MainWindow::onRunnerLine(const QString& s)
{
    const QString t = s.trimmed();
    if (t.isEmpty())
        return;

    QString kind;
    if (t.contains(QStringLiteral("✓")) || t.startsWith(QStringLiteral("打包完成"))
        || t.startsWith(QStringLiteral("剧本：")) || t.startsWith(QStringLiteral("已保存")))
        kind = QStringLiteral("ok");
    else if (t.contains(QStringLiteral("✗")) || t.contains(QStringLiteral("失败")))
        kind = QStringLiteral("err");
    else if (t.contains(QStringLiteral("⚠")))
        kind = QStringLiteral("warn");
    else if (s.startsWith(QStringLiteral(" ")) || t.startsWith(QStringLiteral("  ")))
        kind = QStringLiteral("dim");

    log(t, kind);

    // 批量进度：[3/7] xxx  → 进度条 + 分母
    static const QRegularExpression progRe(QStringLiteral("^\\[(\\d+)/(\\d+)\\]"));
    const QRegularExpressionMatch pm = progRe.match(t);
    if (pm.hasMatch() && m_progress) {
        const int cur = pm.captured(1).toInt();
        const int total = pm.captured(2).toInt();
        m_progress->setVisible(true);
        m_progress->setRange(0, total);
        m_progress->setValue(cur);
        m_progress->setFormat(QStringLiteral("%1 / %2").arg(cur).arg(total));
    }

    // 抓 wav 路径（say 的输出："wav: <path>  (123.4 KB)"）
    if (t.startsWith(QStringLiteral("wav: "))) {
        QString path = t.mid(5).trimmed();
        const int cut = path.indexOf(QStringLiteral("  ("));
        if (cut > 0)
            path = path.left(cut).trimmed();
        if (QFileInfo::exists(path)) {
            m_lastWav = path;
            if (!m_pendingText.isEmpty() && !m_pendingChar.isEmpty()) {
                QString t2 = m_pendingText;
                if (t2.size() > 16)
                    t2 = t2.left(16) + QStringLiteral("…");
                const double sec = wavSeconds(path);
                m_audioMeta.insert(path,
                    QStringLiteral("%1 · 「%2」%3")
                        .arg(m_pendingChar, t2,
                             sec > 0 ? QStringLiteral(" · %1s").arg(sec, 0, 'f', 1) : QString()));
                saveAudioMeta();
            }
            refreshAudioList();                 // 新出来的排在最上面
            if (m_autoPlay->isChecked())
                playWav(path);
        }
    }

    // 引擎状态灯
    if (t.startsWith(QStringLiteral("引擎在线"))) {
        m_engineDot->setProperty("state", "ok");
        m_engineDot->style()->unpolish(m_engineDot);
        m_engineDot->style()->polish(m_engineDot);
        m_engineText->setText(t.left(48));
        m_engineText->setProperty("state", "ok");
    m_engineText->style()->unpolish(m_engineText);
    m_engineText->style()->polish(m_engineText);
    } else if (t.contains(QStringLiteral("不在线")) || t.contains(QStringLiteral("离线"))
               || t.startsWith(QStringLiteral("✗ 起不了"))) {
        m_engineDot->setProperty("state", "off");
        m_engineDot->style()->unpolish(m_engineDot);
        m_engineDot->style()->polish(m_engineDot);
        m_engineText->setText(QStringLiteral("引擎离线（点顶部「启动引擎」）"));
        m_engineText->setProperty("state", "off");
    m_engineText->style()->unpolish(m_engineText);
    m_engineText->style()->polish(m_engineText);
    }

    // 状态栏跟进度
    if (t.startsWith(QStringLiteral("[1/")) || t.startsWith(QStringLiteral("["))) {
        // 形如 [3/17] 三月七「...」✓  (12s)
        const int rb = t.indexOf(QStringLiteral("]"));
        if (rb > 1 && rb < 12)
            m_status->setText(QStringLiteral("进度 %1").arg(t.left(rb + 1)));
    }
}

void MainWindow::onRunnerDone(int code, int elapsedSec)
{
    // 链式任务：上一步成功就接着跑下一步（script → synth）
    if (code == 0 && !m_chain.isEmpty()) {
        const QStringList next = m_chain.takeFirst();
        log(QStringLiteral("✔ 上一步完成（%1 秒）→ 接着跑：%2")
                .arg(elapsedSec).arg(next.join(QStringLiteral(" "))), "ok");
        m_status->setText(QStringLiteral("正在跑…"));
        m_runner->run(next);
        return;
    }
    m_chain.clear();
    if (m_progress) {
        m_progress->setVisible(false);
        m_progress->setValue(0);
    }

    setBusy(false);
    if (code == 0)
        log(QStringLiteral("✔ 完成（%1 秒）").arg(elapsedSec), "ok");
    else
        log(QStringLiteral("✗ 失败，退出码 %1（%2 秒）").arg(code).arg(elapsedSec), "err");
    m_status->setText(QStringLiteral("就绪"));
}

void MainWindow::setBusy(bool busy)
{
    m_btnPreview->setEnabled(!busy);
    if (busy) {
        // 主按钮原地变成"停止"。跑长剧本时最怕的就是停不下来
        m_btnSay->setText(QStringLiteral("■   停止"));
        m_btnSay->setObjectName(QStringLiteral("danger"));
        m_status->setText(QStringLiteral("正在跑…"));
        m_runClock.start();
        m_tickTimer->start();
    } else {
        m_btnSay->setText(QStringLiteral("合  成"));
        m_btnSay->setObjectName(QStringLiteral("primary"));
        m_tickTimer->stop();
        if (m_elapsedLabel)
            m_elapsedLabel->setVisible(false);
        if (m_status->text().startsWith(QStringLiteral("正在跑")))
            m_status->setText(QStringLiteral("就绪"));
    }
    m_btnSay->setEnabled(true);
    // objectName 换了必须重新 polish，否则按钮还是旧外观
    m_btnSay->style()->unpolish(m_btnSay);
    m_btnSay->style()->polish(m_btnSay);
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------
void MainWindow::log(const QString& s, const QString& kind)
{
    QString color = QStringLiteral("#d4d4de");
    if (kind == QStringLiteral("ok"))        color = QStringLiteral("#7ee787");
    else if (kind == QStringLiteral("err"))  color = QStringLiteral("#ff7b72");
    else if (kind == QStringLiteral("warn")) color = QStringLiteral("#e3b341");
    else if (kind == QStringLiteral("dim"))  color = QStringLiteral("#8b8b9a");

    const QString line = QStringLiteral("<span style='color:%1'>%2</span><br>")
                             .arg(color, s.toHtmlEscaped());
    m_log->moveCursor(QTextCursor::End);
    m_log->insertHtml(line);
    m_log->moveCursor(QTextCursor::End);
    m_log->ensureCursorVisible();
}
