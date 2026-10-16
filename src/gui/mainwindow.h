#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>

class VkRunner;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMediaPlayer;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;

/*!
 * \brief VoiceKit 配音工作台。
 *
 * 干的事：左边选角色 → 中间写台词 → 点合成。
 * 台词框里写多行就是批量（自动造任务表走 vk synth）。
 * 剧本 → 批量 → 打包 三个按钮在工具栏上，一条流水线跑完。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onSearchChanged(const QString& text);
    void onTreeSelectionChanged();
    void onTreeDoubleClicked(QTreeWidgetItem* item, int col);

    void onSay();
    void onStop();                       //!< 停止正在跑的任务
    void onPreview();
    void onPreviewRef();                 //!< 试听当前角色的参考音频（选音色时先听原声）
    void onMerge();                      //!< 把选中的多句拼成"一整段"
    void onElapsedTick();                //!< 每秒刷新"已用 xx 秒"
    void onSaveAs();
    void onClearLog();
    void refreshAudioList();
    QString pickedAudio() const;         //!< 当前该播/该存的那条（列表选中优先）
    void loadAudioMeta();                //!< 读回列表说明（否则重开程序就只剩文件名）
    void saveAudioMeta();
    void onAudioClicked(QListWidgetItem* item);

    void onOpenScript();
    void onRunScript();
    void onRunSynth();
    void onRunPack();
    void onOpenPackFolder();

    void onEngineCheck();
    void onEngineStart();

    void onRunnerLine(const QString& line);
    void onRunnerDone(int code, int elapsedSec);

private:
    void    buildUi();
    QWidget* buildCharPanel();
    QWidget* buildWorkPanel();
    QWidget* buildAudioPanel();
    void    buildMenus();
    void    applyStyle();
    void    loadIndex();
    void    log(const QString& s, const QString& kind = QString());
    void    setBusy(bool busy);
    QString currentChar() const;
    QStringList selectedChars() const;      //!< 树上所有选中的角色（支持多选）
    QString currentGame() const;
    void    playWav(const QString& path);
    QString quickTablePath() const;
    bool    writeQuickTable(const QString& charName, const QStringList& lines,
                            const QString& outPath, QString* err);

    // ── 配置 / 路径 ────────────────────────────────────────────
    QString m_vkExe;
    QString m_projectDir;
    QString m_workDir;
    QString m_guiDir;
    QString m_scriptPath;      // 打开的剧本
    QString m_tablePath;       // 剧本 → 任务表
    QString m_lastWav;         // 最近合成出来的音频，给"试听"用
    QList<QStringList> m_chain; //!< 排队要接着跑的命令（比如 script 完了自动接 synth）
    //! 角色 → 参考音频路径。选音色时先听原声，比合成一句试快 12 秒
    QHash<QString, QString> m_charRefAudio;
    //! wav 绝对路径 → 列表里显示的说明（角色 · 台词摘要 · 时长）
    QHash<QString, QString> m_audioMeta;
    //! 正在合成的这条（合成完写进 m_audioMeta）
    QString m_pendingText;
    QString m_pendingChar;
    QElapsedTimer m_runClock;   //!< 本次任务已跑多久（界面每秒刷新）

    // ── 零件 ──────────────────────────────────────────────────
    VkRunner*      m_runner = nullptr;

    QTreeWidget*   m_tree = nullptr;
    QLineEdit*     m_search = nullptr;
    QLabel*        m_treeStat = nullptr;
    QLabel*        m_heroName = nullptr;
    QLabel*        m_audioCount = nullptr;

    QPlainTextEdit* m_text = nullptr;
    QSlider*        m_speed = nullptr;
    QLabel*         m_speedVal = nullptr;
    QComboBox*      m_emotion = nullptr;
    QCheckBox*      m_autoPlay = nullptr;
    QPushButton*    m_btnSay = nullptr;
    QPushButton*    m_btnPreview = nullptr;

    QTextEdit*     m_log = nullptr;
    QListWidget*   m_audioList = nullptr;
    QProgressBar*  m_progress = nullptr;      //!< 批量合成进度（单句时隐藏）
    QLabel*        m_elapsedLabel = nullptr;  //!< "已用 12 秒"
    QTimer*        m_tickTimer = nullptr;     //!< 每秒刷新已用时间
    QLabel*        m_engineDot = nullptr;
    QLabel*        m_engineText = nullptr;
    QLabel*        m_status = nullptr;
};

#endif // MAINWINDOW_H
