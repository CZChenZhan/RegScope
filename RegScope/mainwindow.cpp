#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "regshot.h"
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QSettings>
#include <QTextDocument>
#include <QTextEdit>
#include <QStringList>
#include <QTextStream>
#include <QStringConverter>
#include <QTextBlock>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <shellapi.h>

namespace
{
    int clampAnalysisRiskLevel(int riskLevel)
    {
        if (riskLevel < 1)
        {
            return 1;
        }

        if (riskLevel > 5)
        {
            return 5;
        }

        return riskLevel;
    }

    QString builtInAnalysisConfigPath(int riskLevel)
    {
        return QString(":/admin/configs/level%1.ini").arg(clampAnalysisRiskLevel(riskLevel));
    }

    constexpr int kCustomAnalysisConfigId = 6;

    bool isBuiltInAnalysisConfigPath(const QString &path)
    {
        const QString normalizedPath = QDir::fromNativeSeparators(path.trimmed());
        for (int level = 1; level <= 5; ++level)
        {
            if (normalizedPath == builtInAnalysisConfigPath(level))
            {
                return true;
            }
        }

        return false;
    }

    enum RegistryTreeRole
    {
        RootHandleRole = Qt::UserRole,
        RelativePathRole,
        ChildrenLoadedRole
    };

    QString findConfigDirectory()
    {
        const QStringList searchRoots = {
            QDir::currentPath(),
            QCoreApplication::applicationDirPath()};

        for (const QString &rootPath : searchRoots)
        {
            QDir dir(rootPath);
            for (int depth = 0; depth < 6; ++depth)
            {
                const QString configDirPath = dir.filePath("configs");
                QFileInfo configDirInfo(configDirPath);
                if (configDirInfo.exists() && configDirInfo.isDir())
                {
                    return configDirInfo.absoluteFilePath();
                }

                if (!dir.cdUp())
                {
                    break;
                }
            }
        }

        return QString();
    }

    int contentLineCount(const QString &content)
    {
        return content.isEmpty() ? 0 : content.count('\n') + 1;
    }

    class CompareSelectionDialog : public QDialog
    {
    public:
        enum RequestedAction
        {
            NoAction,
            CompareFiles,
            CaptureDirectSnapshot
        };

        CompareSelectionDialog(QWidget *parent, int lang, const QString &initialDir, int directSnapshotState)
            : QDialog(parent, Qt::WindowTitleHint | Qt::WindowCloseButtonHint), m_lang(lang), m_initialDir(initialDir)
        {
            setWindowTitle(text("选择快照比较方式", "Select Snapshot Comparison Mode"));
            setMinimumWidth(600);
            setStyleSheet(
                "QDialog { background-color: #1f232a; color: #d7dae0; }"
                "QLabel { color: #d7dae0; font-family: 'Segoe UI', 'Microsoft YaHei UI'; }"
                "QPushButton { min-height: 36px; padding: 0 16px; border-radius: 6px; border: 1px solid #3a404a; background-color: #2a2f38; color: #d7dae0; }"
                "QPushButton:hover { background-color: #343b46; }"
                "QPushButton#compareBtn { background-color: #2f65ca; border: 1px solid #2f65ca; color: white; font-weight: bold; }"
                "QPushButton#compareBtn:hover { background-color: #3a73df; }"
                "QPushButton#compareBtn:disabled { background-color: #40444b; border: 1px solid #40444b; color: #72767d; }"
                "QPushButton#directCaptureBtn { background-color: #27895b; border: 1px solid #27895b; color: white; font-weight: bold; }"
                "QPushButton#directCaptureBtn:hover { background-color: #2e9a67; }"
                "QLineEdit { background-color: #181b20; color: #d7dae0; border: 1px solid #3a404a; border-radius: 6px; padding: 8px; }");

            QVBoxLayout *mainLayout = new QVBoxLayout(this);
            mainLayout->setContentsMargins(24, 24, 24, 24);
            mainLayout->setSpacing(16);

            QLabel *fileModeTitle = new QLabel(text("上传本地快照文件", "Compare Local Snapshot Files"));
            fileModeTitle->setStyleSheet("font-weight: bold; font-size: 11pt; color: #4c8dff;");
            QLabel *fileModeHint = new QLabel(text(
                "选取两个本地 .rgs 快照文件进行比较。",
                "Select two local .rgs snapshot files to compare."));
            fileModeHint->setWordWrap(true);

            QHBoxLayout *selectorsLayout = new QHBoxLayout();
            selectorsLayout->setSpacing(30);

            // Left side: First Snapshot
            QVBoxLayout *leftLayout = new QVBoxLayout();
            QLabel *leftTitle = new QLabel(text("第一次快照 (基准)", "First Snapshot (Base)"));
            leftTitle->setStyleSheet("font-weight: bold; font-size: 11pt; color: #4c8dff;");
            m_leftPathEdit = new QLineEdit();
            m_leftPathEdit->setReadOnly(true);
            m_leftPathEdit->setPlaceholderText(text("尚未选取...", "Not selected..."));
            QPushButton *leftBtn = new QPushButton(text("选取文件", "Select File"));
            leftLayout->addWidget(leftTitle);
            leftLayout->addWidget(m_leftPathEdit);
            leftLayout->addWidget(leftBtn);

            // Right side: Second Snapshot
            QVBoxLayout *rightLayout = new QVBoxLayout();
            QLabel *rightTitle = new QLabel(text("第二次快照 (改动)", "Second Snapshot (Changes)"));
            rightTitle->setStyleSheet("font-weight: bold; font-size: 11pt; color: #d19a66;");
            m_rightPathEdit = new QLineEdit();
            m_rightPathEdit->setReadOnly(true);
            m_rightPathEdit->setPlaceholderText(text("尚未选取...", "Not selected..."));
            QPushButton *rightBtn = new QPushButton(text("选取文件", "Select File"));
            rightLayout->addWidget(rightTitle);
            rightLayout->addWidget(m_rightPathEdit);
            rightLayout->addWidget(rightBtn);

            selectorsLayout->addLayout(leftLayout);
            selectorsLayout->addLayout(rightLayout);

            m_compareBtn = new QPushButton(text("开始比较", "Compare Now"));
            m_compareBtn->setObjectName("compareBtn");
            m_compareBtn->setEnabled(false);

            QFrame *separator = new QFrame(this);
            separator->setFrameShape(QFrame::HLine);
            separator->setStyleSheet("color: #3a404a;");

            QLabel *directModeTitle = new QLabel(text("直接拍摄并比较", "Capture Two Snapshots and Compare"));
            directModeTitle->setStyleSheet("font-weight: bold; font-size: 11pt; color: #7bd88f;");

            m_directStateLabel = new QLabel(this);
            m_directStateLabel->setWordWrap(true);
            m_directCaptureBtn = new QPushButton(this);
            m_directCaptureBtn->setObjectName("directCaptureBtn");
            updateDirectCaptureState(directSnapshotState);

            mainLayout->addWidget(fileModeTitle);
            mainLayout->addWidget(fileModeHint);
            mainLayout->addLayout(selectorsLayout);
            mainLayout->addWidget(m_compareBtn);
            mainLayout->addSpacing(4);
            mainLayout->addWidget(separator);
            mainLayout->addWidget(directModeTitle);
            mainLayout->addWidget(m_directStateLabel);
            mainLayout->addWidget(m_directCaptureBtn);

            connect(leftBtn, &QPushButton::clicked, this, [this]()
                    { selectFile(true); });
            connect(rightBtn, &QPushButton::clicked, this, [this]()
                    { selectFile(false); });
            connect(m_compareBtn, &QPushButton::clicked, this, [this]()
                    {
                        m_requestedAction = CompareFiles;
                        accept(); });
            connect(m_directCaptureBtn, &QPushButton::clicked, this, [this]()
                    {
                        m_requestedAction = CaptureDirectSnapshot;
                        accept(); });
        }

        QString firstPath() const { return m_leftPath; }
        QString secondPath() const { return m_rightPath; }
        RequestedAction requestedAction() const { return m_requestedAction; }

    private:
        QString text(const QString &zhText, const QString &enText) const
        {
            return m_lang == 0 ? zhText : enText;
        }

        void updateDirectCaptureState(int directSnapshotState)
        {
            if (directSnapshotState <= 0)
            {
                m_directStateLabel->setText(text(
                    "当前状态：尚未拍摄第一次快照。",
                    "Current state: the first snapshot has not been captured yet."));
                m_directCaptureBtn->setText(text("拍摄第一次快照", "Capture First Snapshot"));
                return;
            }

            m_directStateLabel->setText(text(
                "当前状态：第一次快照已在内存中，拍摄第二次快照后将自动比较。",
                "Current state: the first snapshot is stored in memory. Capture the second snapshot to compare automatically."));
            m_directCaptureBtn->setText(text("拍摄第二次快照", "Capture Second Snapshot"));
        }

        void selectFile(bool isFirst)
        {
            QString path = QFileDialog::getOpenFileName(
                this,
                text("选择快照文件", "Select Snapshot File"),
                m_initialDir,
                text("RegScope 快照 (*.rgs)", "RegScope Snapshot (*.rgs)"));
            if (!path.isEmpty())
            {
                if (isFirst)
                {
                    m_leftPath = path;
                    m_leftPathEdit->setText(QFileInfo(path).fileName());
                    m_leftPathEdit->setToolTip(path);
                }
                else
                {
                    m_rightPath = path;
                    m_rightPathEdit->setText(QFileInfo(path).fileName());
                    m_rightPathEdit->setToolTip(path);
                }
                m_compareBtn->setEnabled(!m_leftPath.isEmpty() && !m_rightPath.isEmpty());
            }
        }

        int m_lang;
        QString m_initialDir;
        QString m_leftPath;
        QString m_rightPath;
        QLineEdit *m_leftPathEdit;
        QLineEdit *m_rightPathEdit;
        QPushButton *m_compareBtn;
        QLabel *m_directStateLabel;
        QPushButton *m_directCaptureBtn;
        RequestedAction m_requestedAction = NoAction;
    };

    class RegistryDetailView;

    class AnalysisOptionsDialog : public QDialog
    {
    public:
        AnalysisOptionsDialog(QWidget *parent, int lang, int initialRiskLevel, const QString &initialConfigPath, const QString &initialDir)
            : QDialog(parent, Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
              m_lang(lang),
              m_initialDir(initialDir),
              m_customConfigPath(initialConfigPath),
              m_initialRiskLevel(clampAnalysisRiskLevel(initialRiskLevel)),
              m_lastPresetRiskLevel(clampAnalysisRiskLevel(initialRiskLevel))
        {
            setWindowTitle(text("开始实时分析", "Start Real-Time Analysis"));
            setMinimumWidth(560);
            setStyleSheet(
                "QDialog { background-color: #1f232a; color: #d7dae0; }"
                "QLabel { color: #d7dae0; font-family: 'Segoe UI', 'Microsoft YaHei UI'; }"
                "QPushButton { min-height: 34px; padding: 0 14px; border-radius: 6px; border: 1px solid #3a404a; background-color: #2a2f38; color: #d7dae0; }"
                "QPushButton:hover { background-color: #343b46; }"
                "QPushButton#startBtn { background-color: #2f65ca; border: 1px solid #2f65ca; color: white; font-weight: bold; }"
                "QPushButton#startBtn:hover { background-color: #3a73df; }"
                "QLineEdit { background-color: #181b20; color: #d7dae0; border: 1px solid #3a404a; border-radius: 6px; padding: 8px; }"
                "QRadioButton { spacing: 8px; }");

            QVBoxLayout *mainLayout = new QVBoxLayout(this);
            mainLayout->setContentsMargins(24, 24, 24, 24);
            mainLayout->setSpacing(16);

            QLabel *titleLabel = new QLabel(text("请选择危险等级与配置文件", "Select the risk level and configuration file"), this);
            titleLabel->setStyleSheet("font-weight: bold; font-size: 12pt; color: #4c8dff;");
            QLabel *hintLabel = new QLabel(text(
                                               "运行后将持续监听注册表新增、删除、修改事件。",
                                               "The monitor will keep listening for added, deleted, and modified registry events after it starts."),
                                           this);
            hintLabel->setWordWrap(true);

            QLabel *riskLabel = new QLabel(text("分析规则", "Analysis Rule"), this);
            riskLabel->setStyleSheet("font-weight: bold;");

            QVBoxLayout *riskLayout = new QVBoxLayout();
            riskLayout->setSpacing(10);
            m_riskGroup = new QButtonGroup(this);
            for (int level = 1; level <= 5; ++level)
            {
                QRadioButton *button = new QRadioButton(
                    text(QString("危险等级 %1").arg(level), QString("Risk Level %1").arg(level)),
                    this);
                m_riskGroup->addButton(button, level);
                riskLayout->addWidget(button);
            }
            QRadioButton *customButton = new QRadioButton(text("自定义", "Custom"), this);
            m_riskGroup->addButton(customButton, kCustomAnalysisConfigId);
            riskLayout->addWidget(customButton);

            m_configLabel = new QLabel(text("配置文件", "Configuration File"), this);
            m_configLabel->setStyleSheet("font-weight: bold;");

            QHBoxLayout *configLayout = new QHBoxLayout();
            configLayout->setSpacing(10);
            m_configEdit = new QLineEdit(this);
            m_configEdit->setReadOnly(true);
            m_configEdit->setPlaceholderText(text("请选择 .ini 配置文件", "Please select a .ini configuration file"));
            m_browseButton = new QPushButton(text("选择文件", "Browse"), this);
            configLayout->addWidget(m_configEdit, 1);
            configLayout->addWidget(m_browseButton);

            QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
            m_startButton = buttonBox->addButton(text("开始分析", "Start Analysis"), QDialogButtonBox::AcceptRole);
            m_startButton->setObjectName("startBtn");
            buttonBox->button(QDialogButtonBox::Cancel)->setText(text("取消", "Cancel"));

            mainLayout->addWidget(titleLabel);
            mainLayout->addWidget(hintLabel);
            mainLayout->addWidget(riskLabel);
            mainLayout->addLayout(riskLayout);
            mainLayout->addWidget(m_configLabel);
            mainLayout->addLayout(configLayout);
            mainLayout->addWidget(buttonBox);

            connect(m_browseButton, &QPushButton::clicked, this, [this]()
                    {
                        const QString selectedPath = QFileDialog::getOpenFileName(
                            this,
                            text("选择配置文件", "Select Configuration File"),
                            m_initialDir,
                            text("配置文件 (*.ini);;INI Files (*.ini)", "Configuration Files (*.ini);;INI Files (*.ini)"));
                        if (!selectedPath.isEmpty())
                        {
                            m_customConfigPath = QFileInfo(selectedPath).absoluteFilePath();
                            refreshConfigDisplay();
                        } });
            connect(m_riskGroup, &QButtonGroup::idClicked, this, [this](int)
                    {
                        const int checkedId = m_riskGroup->checkedId();
                        if (checkedId >= 1 && checkedId <= 5)
                        {
                            m_lastPresetRiskLevel = checkedId;
                        }
                        refreshConfigDisplay(); });
            connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
            connect(m_startButton, &QPushButton::clicked, this, [this]()
                    { accept(); });

            const bool useCustomConfig = !m_customConfigPath.isEmpty() && !isBuiltInAnalysisConfigPath(m_customConfigPath);
            if (QAbstractButton *initialButton = m_riskGroup->button(useCustomConfig ? kCustomAnalysisConfigId : m_initialRiskLevel))
            {
                initialButton->setChecked(true);
            }

            refreshConfigDisplay();
        }

        int riskLevel() const
        {
            const int checkedId = m_riskGroup->checkedId();
            if (checkedId >= 1 && checkedId <= 5)
            {
                return checkedId;
            }

            return m_lastPresetRiskLevel;
        }

        QString configPath() const
        {
            return effectiveConfigPath();
        }

    private:
        QString text(const QString &zhText, const QString &enText) const
        {
            return m_lang == 0 ? zhText : enText;
        }

        QString effectiveConfigPath() const
        {
            if (isCustomSelected())
            {
                if (!m_customConfigPath.isEmpty() && QFile::exists(m_customConfigPath))
                {
                    return m_customConfigPath;
                }

                return QString();
            }

            return builtInAnalysisConfigPath(riskLevel());
        }

        bool isCustomSelected() const
        {
            return m_riskGroup->checkedId() == kCustomAnalysisConfigId;
        }

        void refreshConfigDisplay()
        {
            const bool showCustomConfig = isCustomSelected();
            m_configLabel->setVisible(showCustomConfig);
            m_configEdit->setVisible(showCustomConfig);
            m_browseButton->setVisible(showCustomConfig);

            const QString path = effectiveConfigPath();
            if (showCustomConfig)
            {
                m_configEdit->setText(path.isEmpty() ? QString() : QFileInfo(path).fileName());
                m_configEdit->setToolTip(path);
            }
            updateStartButtonState();
        }

        void updateStartButtonState()
        {
            m_startButton->setEnabled(!effectiveConfigPath().isEmpty() && QFile::exists(effectiveConfigPath()));
        }

        int m_lang;
        QString m_initialDir;
        QString m_customConfigPath;
        int m_initialRiskLevel;
        int m_lastPresetRiskLevel;
        QButtonGroup *m_riskGroup = nullptr;
        QLabel *m_configLabel = nullptr;
        QLineEdit *m_configEdit = nullptr;
        QPushButton *m_browseButton = nullptr;
        QPushButton *m_startButton = nullptr;
    };

    class LineNumberArea : public QWidget
    {
    public:
        explicit LineNumberArea(QWidget *parent, RegistryDetailView *editor)
            : QWidget(parent), m_editor(editor)
        {
        }

        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        RegistryDetailView *m_editor;
    };

    class RegistryDetailView : public QPlainTextEdit
    {
    public:
        explicit RegistryDetailView(QWidget *parent = nullptr)
            : QPlainTextEdit(parent), m_lineNumberArea(new LineNumberArea(this, this))
        {
            connect(this, &QPlainTextEdit::blockCountChanged, this,
                    [this](int)
                    {
                        updateLineNumberAreaWidth();
                    });
            connect(this, &QPlainTextEdit::updateRequest, this,
                    [this](const QRect &rect, int dy)
                    {
                        if (dy != 0)
                        {
                            m_lineNumberArea->scroll(0, dy);
                        }
                        else
                        {
                            m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
                        }

                        if (rect.contains(viewport()->rect()))
                        {
                            updateLineNumberAreaWidth();
                        }
                    });
            connect(this, &QPlainTextEdit::cursorPositionChanged, this,
                    [this]()
                    {
                        m_lineNumberArea->update();
                    });

            updateLineNumberAreaWidth();
        }

        int lineNumberAreaWidth() const
        {
            int digits = 1;
            int maxLineCount = qMax(1, blockCount());
            while (maxLineCount >= 10)
            {
                maxLineCount /= 10;
                ++digits;
            }

            return 18 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
        }

        void lineNumberAreaPaintEvent(QPaintEvent *event)
        {
            QPainter painter(m_lineNumberArea);
            painter.fillRect(event->rect(), QColor("#252526"));
            painter.setPen(QColor("#6b7280"));

            QTextBlock block = firstVisibleBlock();
            int blockNumber = block.blockNumber();
            int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
            int bottom = top + qRound(blockBoundingRect(block).height());
            const int currentLine = textCursor().blockNumber();

            while (block.isValid() && top <= event->rect().bottom())
            {
                if (block.isVisible() && bottom >= event->rect().top())
                {
                    const QString lineNumber = QString::number(blockNumber + 1);
                    const QRect numberRect(0, top, m_lineNumberArea->width() - 6, fontMetrics().height());
                    painter.setPen(blockNumber == currentLine ? QColor("#d7dae0") : QColor("#6b7280"));
                    painter.drawText(numberRect, Qt::AlignRight | Qt::AlignVCenter, lineNumber);
                }

                block = block.next();
                top = bottom;
                bottom = top + qRound(blockBoundingRect(block).height());
                ++blockNumber;
            }
        }

        void updateLineNumberAreaWidth()
        {
            setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
        }

    protected:
        void resizeEvent(QResizeEvent *event) override
        {
            QPlainTextEdit::resizeEvent(event);

            const QRect contentRect = contentsRect();
            m_lineNumberArea->setGeometry(
                QRect(contentRect.left(), contentRect.top(), lineNumberAreaWidth(), contentRect.height()));
        }

    private:
        QWidget *m_lineNumberArea;
    };

    QSize LineNumberArea::sizeHint() const
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

    void LineNumberArea::paintEvent(QPaintEvent *event)
    {
        m_editor->lineNumberAreaPaintEvent(event);
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      ui(new Ui::MainWindow),
      registryTree(nullptr),
      registryDetailView(nullptr),
      bottomLogView(nullptr),
      registrySearchDialog(nullptr),
      registrySearchInput(nullptr),
      registrySearchFindButton(nullptr),
      registrySearchNextButton(nullptr),
      registrySearchPreviousButton(nullptr),
      registryFilterDialog(nullptr),
      registryFilterInput(nullptr),
      registryFilterApplyButton(nullptr),
      leftPanelTitle(nullptr),
      leftPanelMeta(nullptr),
      rightDisplayTitle(nullptr),
      rightDisplayMeta(nullptr),
      bottomLogTitle(nullptr),
      snapshotProgressBar(nullptr),
      snapshotProgressLabel(nullptr),
      saveResultAction(nullptr),
      startAnalysisAction(nullptr),
      stopAnalysisAction(nullptr),
      takeSnapshotAction(nullptr),
      uiLanguage(0),
      currentRegistryFilterRule(),
      currentRegistrySearchIndex(-1),
      m_compareSnapshotState(0),
      m_registryMonitor(nullptr),
      m_analysisRiskLevel(3),
      m_analysisRunning(false),
      m_saveResultContentType(0),
      m_analysisRenderedBlockCount(0),
      m_analysisRecordedEventCount(0),
      m_analysisLazyLoading(false)
{
    ZeroMemory(&m_firstCompareSnapshot, sizeof(RegShotContent));
    ZeroMemory(&m_secondCompareSnapshot, sizeof(RegShotContent));
    ui->setupUi(this);
    // 初始化窗口样式：全屏、程序图标、程序名字
    this->setWindowTitle("RegScope");
    this->setWindowIcon(QIcon(":/icos/pictures/app.ico"));
    this->showMaximized();
    // 初始化界面分区：菜单栏、分割线、三个分区
    initLayout();
    // 加载私有成员列表中所有控件的样式
    setStyles();
    // 加载默认配置
    ConfigMap config = getAppConfig();
    uiLanguage = config["System"]["language"].toInt();
    // 菜单栏交互完善
    addToMenuBar(config);
    rightDisplayReady();
    // 底部日志区初始化
    bottomLogReady();
    // 左区功能完善
    leftPanelReady();
    m_registryMonitor = new RegistryMonitor(this);
    connect(m_registryMonitor, &RegistryMonitor::registryChanged, this, &MainWindow::handleRegistryAnalysisEvent);
    connect(m_registryMonitor, &RegistryMonitor::monitorStarted, this, [this]()
            {
                if (startAnalysisAction != nullptr)
                {
                    startAnalysisAction->setEnabled(false);
                }
                appendLogMessage(localizedText(
                    "ETW 实时分析已启动，正在监听注册表改动。",
                    "ETW real-time analysis has started and is now listening for registry changes."));
                updateRightDisplayMeta(localizedText("实时分析进行中", "Real-time analysis is running")); });
    connect(m_registryMonitor, &RegistryMonitor::monitorStopped, this, [this]()
            {
                if (startAnalysisAction != nullptr)
                {
                    startAnalysisAction->setEnabled(true);
                }
                updateRightDisplayMeta(localizedText("实时分析已结束", "Real-time analysis has stopped")); });
    connect(m_registryMonitor, &RegistryMonitor::monitorError, this, [this](const QString &details)
            {
                m_analysisRunning = false;
                if (startAnalysisAction != nullptr)
                {
                    startAnalysisAction->setEnabled(true);
                }
                appendLogMessage(localizedText("ETW 分析启动失败：", "Failed to start ETW analysis: ") + details);
                updateRightDisplayMeta(localizedText("实时分析未运行", "Real-time analysis is not running")); });
    // 恢复因权限切换而待处理的注册表显示状态
    restorePendingRegistryState();
}

MainWindow::~MainWindow()
{
    if (m_registryMonitor != nullptr)
    {
        m_registryMonitor->stopMonitoring();
    }
    resetInMemoryComparisonShots();
    delete ui;
}

void MainWindow::initLayout()
{
    // 初始化菜单栏并置顶
    mMenuBar = new QMenuBar(this);
    this->setMenuBar(mMenuBar);
    // 初始化分割器
    mainSplitter = new QSplitter(Qt::Vertical, this);
    topSplitter = new QSplitter(Qt::Horizontal, mainSplitter);
    // 初始化三个 QFrame 容器并组装到分割器
    leftPanel = new QFrame(topSplitter);
    rightDisplay = new QFrame(topSplitter);
    bottomLog = new QFrame(mainSplitter);
    topSplitter->addWidget(leftPanel);
    topSplitter->addWidget(rightDisplay);
    mainSplitter->addWidget(topSplitter);
    mainSplitter->addWidget(bottomLog);
    this->setCentralWidget(mainSplitter);

    // 设置对象名，以便通过成员指针获取成员变量名
    mMenuBar->setObjectName("mMenuBar");
    mainSplitter->setObjectName("mainSplitter");
    topSplitter->setObjectName("topSplitter");
    leftPanel->setObjectName("leftPanel");
    rightDisplay->setObjectName("rightDisplay");
    bottomLog->setObjectName("bottomLog");
    // 硬编码私有成员列表，以指针形式存储
    allWidgets << mMenuBar
               << mainSplitter
               << topSplitter
               << leftPanel
               << rightDisplay
               << bottomLog;
}

bool MainWindow::setStyles()
{
    // 初始化返回值，所有样式文件加载完成则返回 true，否则报错并返回 false
    bool allSuccess = true;
    // 对私有成员列表中的每一个私有成员指针做操作
    for (int i = 0; i < allWidgets.size(); ++i)
    {
        // 提取成员变量
        QWidget *widget = allWidgets[i];
        // 获取成员变量的变量名和类名
        QString objectName = widget->objectName();
        QString className = widget->metaObject()->className();
        if (objectName.isEmpty())
        {
            qWarning() << "Widget" << className << "has no object name, skipping";
            continue;
        }
        allSuccess = applyWidgetStyle(widget) && allSuccess;
    }
    // 获取窗口大小
    int totalHeight = this->centralWidget()->height();
    int totalWidth = this->centralWidget()->width();
    // 计算 mainSplitter (上下 7:3) 的像素分配
    int topHeight = totalHeight * 0.7;
    int bottomHeight = totalHeight - topHeight;
    mainSplitter->setSizes(QList<int>() << topHeight << bottomHeight);
    // 计算 topSplitter (左右 3:7) 的像素分配
    int leftWidth = totalWidth * 0.3;
    int rightWidth = totalWidth - leftWidth;
    topSplitter->setSizes(QList<int>() << leftWidth << rightWidth);
    // 返回样式加载结果
    return allSuccess;
}

bool MainWindow::applyWidgetStyle(QWidget *widget)
{
    if (widget == nullptr)
    {
        return false;
    }

    const QString objectName = widget->objectName();
    const QMetaObject *metaObject = widget->metaObject();
    QString stylePath;
    while (metaObject != nullptr)
    {
        const QString candidatePath = QString(":%1/styles/%2.qss")
                                          .arg(metaObject->className(), objectName);
        QFileInfo fileInfo(candidatePath);
        if (fileInfo.exists() && fileInfo.isFile())
        {
            stylePath = candidatePath;
            break;
        }
        metaObject = metaObject->superClass();
    }

    if (stylePath.isEmpty())
    {
        qWarning() << "Style file not found for:" << objectName << widget->metaObject()->className();
        return false;
    }

    QFile styleFile(stylePath);
    if (!styleFile.open(QFile::ReadOnly | QFile::Text))
    {
        qWarning() << "Failed to open style file:" << stylePath;
        return false;
    }

    widget->setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    styleFile.close();
    return true;
}

void MainWindow::leftPanelReady()
{
    if (registryTree != nullptr)
    {
        return;
    }

    QVBoxLayout *layout = new QVBoxLayout(leftPanel);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    QFrame *treeHeader = new QFrame(leftPanel);
    treeHeader->setObjectName("leftPanelHeader");
    QHBoxLayout *headerLayout = new QHBoxLayout(treeHeader);
    headerLayout->setContentsMargins(16, 10, 16, 10);
    headerLayout->setSpacing(12);

    leftPanelTitle = new QLabel(localizedText("注册表结构", "Registry Tree"), treeHeader);
    leftPanelTitle->setObjectName("leftPanelTitle");
    leftPanelMeta = new QLabel(localizedText("正在加载根节点", "Loading root keys"), treeHeader);
    leftPanelMeta->setObjectName("leftPanelMeta");
    leftPanelMeta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addWidget(leftPanelTitle);
    headerLayout->addStretch(1);
    headerLayout->addWidget(leftPanelMeta);

    registryTree = new QTreeWidget(leftPanel);

    registryTree->setObjectName("registryTree");

    registryTree->setHeaderHidden(true);
    registryTree->setRootIsDecorated(true);
    registryTree->setAnimated(false);
    registryTree->setIndentation(18);
    registryTree->setUniformRowHeights(true);
    registryTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    registryTree->setSelectionMode(QAbstractItemView::SingleSelection);
    registryTree->setExpandsOnDoubleClick(true);
    registryTree->setIconSize(QSize(16, 16));
    registryTree->header()->setStretchLastSection(true);

    layout->addWidget(treeHeader, 0);
    layout->addWidget(registryTree, 1);

    allWidgets << registryTree;
    applyWidgetStyle(registryTree);

    appendRegistryRoot(QString::fromWCharArray(lpHKCR), HKEY_CLASSES_ROOT);
    appendRegistryRoot(QString::fromWCharArray(lpHKCU), HKEY_CURRENT_USER);
    appendRegistryRoot(QString::fromWCharArray(lpHKLM), HKEY_LOCAL_MACHINE);
    appendRegistryRoot(QString::fromWCharArray(lpHKU), HKEY_USERS);
    appendRegistryRoot(QString::fromWCharArray(lpHKCC), HKEY_CURRENT_CONFIG);

    updateLeftPanelMeta(localizedText(
        QString("%1 个根项").arg(registryTree->topLevelItemCount()),
        QString("%1 root keys").arg(registryTree->topLevelItemCount())));

    connect(registryTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item)
            { populateRegistryChildren(item); });
    connect(registryTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *)
            {
                if (current == nullptr)
                {
                    updateLeftPanelMeta(localizedText(
                        QString("%1 个根项").arg(registryTree->topLevelItemCount()),
                        QString("%1 root keys").arg(registryTree->topLevelItemCount())));
                }
                else
                {
                    const HKEY rootKey = reinterpret_cast<HKEY>(
                        static_cast<quintptr>(current->data(0, RootHandleRole).toULongLong()));
                    const QString subKeyPath = current->data(0, RelativePathRole).toString();
                    updateLeftPanelMeta(registryRootName(rootKey) +
                                        (subKeyPath.isEmpty() ? QString() : "\\" + subKeyPath));
                }
                displayRegistryNodeDetails(current);
            });
}

void MainWindow::rightDisplayReady()
{
    if (registryDetailView != nullptr)
    {
        return;
    }

    QVBoxLayout *layout = new QVBoxLayout(rightDisplay);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    QFrame *detailHeader = new QFrame(rightDisplay);
    detailHeader->setObjectName("rightDisplayHeader");
    QHBoxLayout *headerLayout = new QHBoxLayout(detailHeader);
    headerLayout->setContentsMargins(16, 10, 16, 10);
    headerLayout->setSpacing(12);

    rightDisplayTitle = new QLabel(localizedText("注册表详情", "Registry Detail"), detailHeader);
    rightDisplayTitle->setObjectName("rightDisplayTitle");
    rightDisplayMeta = new QLabel(localizedText("未选择节点", "No node selected"), detailHeader);
    rightDisplayMeta->setObjectName("rightDisplayMeta");
    rightDisplayMeta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addWidget(rightDisplayTitle);
    headerLayout->addStretch(1);
    headerLayout->addWidget(rightDisplayMeta);

    registryDetailView = new RegistryDetailView(rightDisplay);
    registryDetailView->setObjectName("registryDetailView");
    registryDetailView->setReadOnly(true);
    registryDetailView->setLineWrapMode(QPlainTextEdit::NoWrap);
    registryDetailView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    registryDetailView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    registryDetailView->setPlaceholderText(localizedText(
        "请选择左侧注册表节点以查看该键及其子键中的键:值。",
        "Select a registry node on the left to inspect the key:value pairs under it."));
    connect(registryDetailView, &QPlainTextEdit::textChanged, this,
            [this]()
            {
                if (!registrySearchKeyword.isEmpty())
                {
                    refreshRegistrySearchHighlights(false);
                }
            });
    connect(registryDetailView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value)
            {
                if (m_saveResultContentType != 2 || m_analysisLazyLoading || registryDetailView == nullptr)
                {
                    return;
                }

                QScrollBar *scrollBar = registryDetailView->verticalScrollBar();
                if (scrollBar == nullptr || m_analysisRenderedBlockCount >= m_analysisDetailBlocks.size())
                {
                    return;
                }

                const int preloadThreshold = qMax(2, scrollBar->pageStep() / 5);
                if (value >= scrollBar->maximum() - preloadThreshold)
                {
                    appendNextAnalysisResultChunk();
                }
            });

    layout->addWidget(detailHeader, 0);
    layout->addWidget(registryDetailView, 1);

    allWidgets << registryDetailView;
    applyWidgetStyle(registryDetailView);
}

void MainWindow::bottomLogReady()
{
    if (bottomLogView != nullptr)
    {
        return;
    }

    QVBoxLayout *layout = new QVBoxLayout(bottomLog);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    QFrame *logHeader = new QFrame(bottomLog);
    logHeader->setObjectName("bottomLogHeader");
    QHBoxLayout *headerLayout = new QHBoxLayout(logHeader);
    headerLayout->setContentsMargins(16, 10, 16, 10);
    headerLayout->setSpacing(12);

    bottomLogTitle = new QLabel(localizedText("运行日志", "Runtime Logs"), logHeader);
    bottomLogTitle->setObjectName("bottomLogTitle");

    snapshotProgressLabel = new QLabel(logHeader);
    snapshotProgressLabel->setStyleSheet("color: #4c8dff; font-weight: bold; margin-left: 20px;");
    snapshotProgressLabel->hide();

    snapshotProgressBar = new QProgressBar(logHeader);
    snapshotProgressBar->setMaximumWidth(200);
    snapshotProgressBar->setRange(0, 100);
    snapshotProgressBar->setTextVisible(false);
    snapshotProgressBar->setStyleSheet(
        "QProgressBar { background-color: #181b20; border: 1px solid #3a404a; border-radius: 4px; height: 12px; }"
        "QProgressBar::chunk { background-color: #2f65ca; border-radius: 3px; }");
    snapshotProgressBar->hide();

    headerLayout->addWidget(bottomLogTitle);
    headerLayout->addWidget(snapshotProgressLabel);
    headerLayout->addWidget(snapshotProgressBar);
    headerLayout->addStretch(1);

    bottomLogView = new QPlainTextEdit(bottomLog);
    bottomLogView->setObjectName("bottomLogView");
    bottomLogView->setReadOnly(true);
    bottomLogView->setLineWrapMode(QPlainTextEdit::NoWrap);
    bottomLogView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bottomLogView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bottomLogView->setPlaceholderText(localizedText(
        "日志输出将在此显示。",
        "Runtime log messages will appear here."));

    layout->addWidget(logHeader, 0);
    layout->addWidget(bottomLogView, 1);

    allWidgets << bottomLogView;
    applyWidgetStyle(bottomLogView);

    appendLogMessage(localizedText("程序初始化完毕！", "Program initialization completed!"));
}

void MainWindow::appendRegistryRoot(const QString &rootName, HKEY rootKey)
{
    QTreeWidgetItem *rootItem = new QTreeWidgetItem(registryTree);
    rootItem->setText(0, rootName);
    rootItem->setIcon(0, style()->standardIcon(QStyle::SP_DirClosedIcon));
    rootItem->setData(0, RootHandleRole, static_cast<qulonglong>(reinterpret_cast<quintptr>(rootKey)));
    rootItem->setData(0, RelativePathRole, QString());
    rootItem->setData(0, ChildrenLoadedRole, false);
    rootItem->setChildIndicatorPolicy(registryKeyHasChildren(rootKey, QString())
                                          ? QTreeWidgetItem::ShowIndicator
                                          : QTreeWidgetItem::DontShowIndicatorWhenChildless);
}

void MainWindow::populateRegistryChildren(QTreeWidgetItem *parentItem)
{
    if (parentItem == nullptr || parentItem->data(0, ChildrenLoadedRole).toBool())
    {
        return;
    }

    const HKEY rootKey = reinterpret_cast<HKEY>(
        static_cast<quintptr>(parentItem->data(0, RootHandleRole).toULongLong()));
    const QString parentPath = parentItem->data(0, RelativePathRole).toString();

    HKEY currentKey = rootKey;
    bool shouldCloseKey = false;
    if (!parentPath.isEmpty())
    {
        const LONG openResult = RegOpenKeyExW(rootKey,
                                              reinterpret_cast<LPCWSTR>(parentPath.utf16()),
                                              0,
                                              KEY_READ,
                                              &currentKey);
        if (openResult != ERROR_SUCCESS)
        {
            parentItem->setData(0, ChildrenLoadedRole, true);
            parentItem->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
            return;
        }
        shouldCloseKey = true;
    }

    DWORD subKeyCount = 0;
    DWORD maxSubKeyNameLen = 0;
    const LONG queryResult = RegQueryInfoKeyW(currentKey,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              &subKeyCount,
                                              &maxSubKeyNameLen,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr);
    if (queryResult == ERROR_SUCCESS && subKeyCount > 0)
    {
        QVector<wchar_t> nameBuffer(static_cast<int>(maxSubKeyNameLen) + 1);
        for (DWORD i = 0; i < subKeyCount; ++i)
        {
            DWORD nameLength = maxSubKeyNameLen + 1;
            const LONG enumResult = RegEnumKeyExW(currentKey,
                                                  i,
                                                  nameBuffer.data(),
                                                  &nameLength,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr);
            if (enumResult != ERROR_SUCCESS)
            {
                continue;
            }

            const QString childName = QString::fromWCharArray(nameBuffer.constData(), static_cast<int>(nameLength));
            const QString childPath = parentPath.isEmpty() ? childName : parentPath + "\\" + childName;

            QTreeWidgetItem *childItem = new QTreeWidgetItem(parentItem);
            childItem->setText(0, childName);
            childItem->setIcon(0, style()->standardIcon(QStyle::SP_DirClosedIcon));
            childItem->setData(0, RootHandleRole, static_cast<qulonglong>(reinterpret_cast<quintptr>(rootKey)));
            childItem->setData(0, RelativePathRole, childPath);
            childItem->setData(0, ChildrenLoadedRole, false);
            childItem->setChildIndicatorPolicy(registryKeyHasChildren(rootKey, childPath)
                                                   ? QTreeWidgetItem::ShowIndicator
                                                   : QTreeWidgetItem::DontShowIndicatorWhenChildless);
        }
    }

    if (shouldCloseKey)
    {
        RegCloseKey(currentKey);
    }

    parentItem->setData(0, ChildrenLoadedRole, true);
    if (parentItem->childCount() == 0)
    {
        parentItem->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }

    if (!currentRegistryFilterRule.isEmpty())
    {
        for (int i = 0; i < registryTree->topLevelItemCount(); ++i)
        {
            applyRegistryFilterToItem(registryTree->topLevelItem(i), currentRegistryFilterRule);
        }
    }
}

bool MainWindow::registryKeyHasChildren(HKEY rootKey, const QString &subKeyPath) const
{
    HKEY currentKey = rootKey;
    bool shouldCloseKey = false;

    if (!subKeyPath.isEmpty())
    {
        const LONG openResult = RegOpenKeyExW(rootKey,
                                              reinterpret_cast<LPCWSTR>(subKeyPath.utf16()),
                                              0,
                                              KEY_READ,
                                              &currentKey);
        if (openResult != ERROR_SUCCESS)
        {
            return false;
        }
        shouldCloseKey = true;
    }

    DWORD subKeyCount = 0;
    const LONG queryResult = RegQueryInfoKeyW(currentKey,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              &subKeyCount,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              nullptr);

    if (shouldCloseKey)
    {
        RegCloseKey(currentKey);
    }

    return queryResult == ERROR_SUCCESS && subKeyCount > 0;
}

void MainWindow::displayRegistryNodeDetails(QTreeWidgetItem *currentItem)
{
    if (registryDetailView == nullptr)
    {
        return;
    }

    if (rightDisplayTitle != nullptr)
    {
        rightDisplayTitle->setText(localizedText("注册表详情", "Registry Detail"));
    }
    resetAnalysisSummaryCache();
    m_saveResultContentType = 0;
    setSaveResultEnabled(false);

    if (currentItem == nullptr)
    {
        registryDetailView->clear();
        updateRightDisplayMeta(localizedText("未选择节点", "No node selected"));
        return;
    }

    const HKEY rootKey = reinterpret_cast<HKEY>(
        static_cast<quintptr>(currentItem->data(0, RootHandleRole).toULongLong()));
    const QString subKeyPath = currentItem->data(0, RelativePathRole).toString();

    if (registryKeyHasChildren(rootKey, subKeyPath))
    {
        registryDetailView->clear();
        updateRightDisplayMeta(localizedText(
            "当前节点包含子项，请继续展开定位叶子节点",
            "This node has child items. Expand it further to reach a leaf node."));
        return;
    }

    QStringList lines;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    appendRegistryKeyDump(rootKey, subKeyPath, lines);
    QApplication::restoreOverrideCursor();

    if (lines.isEmpty())
    {
        registryDetailView->setPlainText(localizedText(
            "当前节点没有可展示的键:值。",
            "There are no key:value pairs available for the current node."));
        updateRightDisplayMeta(localizedText("1 行", "1 line"));
        return;
    }

    registryDetailView->setPlainText(lines.join('\n'));
    registryDetailView->moveCursor(QTextCursor::Start);
    updateRightDisplayMeta(QString("%1 | %2 %3").arg(registryRootName(rootKey) + (subKeyPath.isEmpty() ? QString() : "\\" + subKeyPath), QString::number(lines.size()), localizedText("行", "lines")));
}

void MainWindow::appendRegistryKeyDump(HKEY rootKey, const QString &subKeyPath, QStringList &lines) const
{
    HKEY currentKey = rootKey;
    bool shouldCloseKey = false;

    if (!subKeyPath.isEmpty())
    {
        const LONG openResult = RegOpenKeyExW(rootKey,
                                              reinterpret_cast<LPCWSTR>(subKeyPath.utf16()),
                                              0,
                                              KEY_READ,
                                              &currentKey);
        if (openResult != ERROR_SUCCESS)
        {
            lines << QString("[%1]").arg(registryRootName(rootKey) +
                                         (subKeyPath.isEmpty() ? QString() : "\\" + subKeyPath));
            lines << "  <读取失败>";
            lines << "";
            return;
        }
        shouldCloseKey = true;
    }

    const QString fullPath = registryRootName(rootKey) +
                             (subKeyPath.isEmpty() ? QString() : "\\" + subKeyPath);
    lines << QString("[%1]").arg(fullPath);

    DWORD valueCount = 0;
    DWORD maxValueNameLen = 0;
    DWORD maxValueDataLen = 0;
    DWORD subKeyCount = 0;
    DWORD maxSubKeyNameLen = 0;
    const LONG queryResult = RegQueryInfoKeyW(currentKey,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              &subKeyCount,
                                              &maxSubKeyNameLen,
                                              nullptr,
                                              &valueCount,
                                              &maxValueNameLen,
                                              &maxValueDataLen,
                                              nullptr,
                                              nullptr);

    if (queryResult != ERROR_SUCCESS)
    {
        lines << "  <读取失败>";
        lines << "";
        if (shouldCloseKey)
        {
            RegCloseKey(currentKey);
        }
        return;
    }

    if (valueCount == 0)
    {
        lines << "  (无值)";
    }
    else
    {
        QVector<wchar_t> nameBuffer(static_cast<int>(maxValueNameLen) + 2, L'\0');
        QByteArray dataBuffer(static_cast<int>(maxValueDataLen), '\0');

        for (DWORD i = 0; i < valueCount; ++i)
        {
            DWORD nameLength = maxValueNameLen + 1;
            DWORD dataLength = maxValueDataLen;
            DWORD valueType = REG_NONE;

            const LONG enumResult = RegEnumValueW(currentKey,
                                                  i,
                                                  nameBuffer.data(),
                                                  &nameLength,
                                                  nullptr,
                                                  &valueType,
                                                  reinterpret_cast<LPBYTE>(dataBuffer.data()),
                                                  &dataLength);
            if (enumResult != ERROR_SUCCESS)
            {
                continue;
            }

            const QString valueName = nameLength == 0
                                          ? QStringLiteral("(默认)")
                                          : QString::fromWCharArray(nameBuffer.constData(), static_cast<int>(nameLength));
            const QByteArray valueData = dataBuffer.left(static_cast<int>(dataLength));
            lines << QString("  %1: %2").arg(valueName, formatRegistryValue(valueType, valueData));
        }
    }

    lines << "";

    if (subKeyCount > 0)
    {
        QVector<wchar_t> subKeyNameBuffer(static_cast<int>(maxSubKeyNameLen) + 2, L'\0');
        for (DWORD i = 0; i < subKeyCount; ++i)
        {
            DWORD subKeyNameLength = maxSubKeyNameLen + 1;
            const LONG enumResult = RegEnumKeyExW(currentKey,
                                                  i,
                                                  subKeyNameBuffer.data(),
                                                  &subKeyNameLength,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr);
            if (enumResult != ERROR_SUCCESS)
            {
                continue;
            }

            const QString childName = QString::fromWCharArray(subKeyNameBuffer.constData(),
                                                              static_cast<int>(subKeyNameLength));
            const QString childPath = subKeyPath.isEmpty() ? childName : subKeyPath + "\\" + childName;
            appendRegistryKeyDump(rootKey, childPath, lines);
        }
    }

    if (shouldCloseKey)
    {
        RegCloseKey(currentKey);
    }
}

QString MainWindow::formatRegistryValue(DWORD valueType, const QByteArray &valueData) const
{
    auto bytesToHex = [](const QByteArray &bytes)
    {
        QStringList hexParts;
        hexParts.reserve(bytes.size());
        for (unsigned char byte : bytes)
        {
            hexParts << QString("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
        }
        return hexParts.join(' ');
    };

    switch (valueType)
    {
    case REG_SZ:
    case REG_EXPAND_SZ:
        return valueData.isEmpty()
                   ? QString()
                   : QString::fromWCharArray(reinterpret_cast<const wchar_t *>(valueData.constData()));
    case REG_MULTI_SZ:
    {
        QStringList parts;
        const wchar_t *cursor = reinterpret_cast<const wchar_t *>(valueData.constData());
        const wchar_t *end = reinterpret_cast<const wchar_t *>(valueData.constData() + valueData.size());
        while (cursor < end && *cursor != L'\0')
        {
            const QString item = QString::fromWCharArray(cursor);
            parts << item;
            cursor += item.length() + 1;
        }
        return parts.join(" | ");
    }
    case REG_DWORD:
        if (valueData.size() >= static_cast<int>(sizeof(quint32)))
        {
            const quint32 number = *reinterpret_cast<const quint32 *>(valueData.constData());
            return QString("0x%1 (%2)").arg(number, 8, 16, QLatin1Char('0')).arg(number);
        }
        break;
    case REG_QWORD:
        if (valueData.size() >= static_cast<int>(sizeof(quint64)))
        {
            const quint64 number = *reinterpret_cast<const quint64 *>(valueData.constData());
            return QString("0x%1 (%2)").arg(number, 16, 16, QLatin1Char('0')).arg(number);
        }
        break;
    case REG_BINARY:
        return valueData.isEmpty()
                   ? QStringLiteral("<空二进制>")
                   : QString("[%1 bytes] %2").arg(valueData.size()).arg(bytesToHex(valueData));
    default:
        return valueData.isEmpty()
                   ? QStringLiteral("<空值>")
                   : QString("[type=%1] %2").arg(valueType).arg(bytesToHex(valueData));
    }

    return QStringLiteral("<无法解析>");
}

QString MainWindow::registryRootName(HKEY rootKey) const
{
    if (rootKey == HKEY_CLASSES_ROOT)
    {
        return QString::fromWCharArray(lpHKCR);
    }
    if (rootKey == HKEY_CURRENT_USER)
    {
        return QString::fromWCharArray(lpHKCU);
    }
    if (rootKey == HKEY_LOCAL_MACHINE)
    {
        return QString::fromWCharArray(lpHKLM);
    }
    if (rootKey == HKEY_USERS)
    {
        return QString::fromWCharArray(lpHKU);
    }
    if (rootKey == HKEY_CURRENT_CONFIG)
    {
        return QString::fromWCharArray(lpHKCC);
    }
    return QStringLiteral("UNKNOWN_ROOT");
}

QString MainWindow::buildKeyPath(LPKeyContent key) const
{
    QStringList parts;
    for (LPKeyContent current = key; current != nullptr; current = current->lpFatherKey)
    {
        if (current->lpKeyName != nullptr && current->lpKeyName[0] != L'\0')
        {
            parts.prepend(QString::fromWCharArray(current->lpKeyName));
        }
    }
    return parts.join("\\");
}

QString MainWindow::buildValuePath(LPValueContent value) const
{
    const QString keyPath = value != nullptr ? buildKeyPath(value->lpFatherKey) : QString();
    const QString valueName =
        (value == nullptr || value->lpValueName == nullptr || value->lpValueName[0] == L'\0')
            ? localizedText("(默认)", "(Default)")
            : QString::fromWCharArray(value->lpValueName);
    return keyPath.isEmpty() ? valueName : keyPath + "\\" + valueName;
}

void MainWindow::updateLeftPanelMeta(const QString &summary)
{
    if (leftPanelMeta != nullptr)
    {
        leftPanelMeta->setText(summary);
    }
}

void MainWindow::updateRightDisplayMeta(const QString &summary)
{
    if (rightDisplayMeta != nullptr)
    {
        rightDisplayMeta->setText(summary);
    }
}

void MainWindow::refreshRegistrySearchHighlights(bool logResult)
{
    if (registryDetailView == nullptr)
    {
        return;
    }

    registrySearchMatches.clear();
    currentRegistrySearchIndex = -1;

    if (registrySearchKeyword.isEmpty())
    {
        clearRegistrySearchHighlights();
        if (logResult)
        {
            appendLogMessage(localizedText("请输入搜索内容。", "Please enter search text."));
        }
        return;
    }

    QTextDocument *document = registryDetailView->document();
    QTextCursor cursor(document);
    while (true)
    {
        cursor = document->find(registrySearchKeyword, cursor, QTextDocument::FindCaseSensitively);
        if (cursor.isNull())
        {
            break;
        }
        registrySearchMatches << cursor;
    }

    if (registrySearchMatches.isEmpty())
    {
        clearRegistrySearchHighlights();
        if (logResult)
        {
            appendLogMessage(localizedText(
                QString("未搜索到“%1”，共 0 个结果。").arg(registrySearchKeyword),
                QString("No matches found for \"%1\", 0 results.").arg(registrySearchKeyword)));
        }
        return;
    }

    currentRegistrySearchIndex = 0;
    focusRegistrySearchResult(currentRegistrySearchIndex);

    if (logResult)
    {
        appendLogMessage(localizedText(
            QString("已搜索到“%1”，共 %2 个结果。")
                .arg(registrySearchKeyword, QString::number(registrySearchMatches.size())),
            QString("Found %2 matches for \"%1\".")
                .arg(registrySearchKeyword, QString::number(registrySearchMatches.size()))));
    }
}

void MainWindow::focusRegistrySearchResult(int index)
{
    if (registryDetailView == nullptr || index < 0 || index >= registrySearchMatches.size())
    {
        return;
    }

    QList<QTextEdit::ExtraSelection> selections;
    for (int i = 0; i < registrySearchMatches.size(); ++i)
    {
        QTextEdit::ExtraSelection selection;
        selection.cursor = registrySearchMatches[i];
        selection.format.setBackground(i == index ? QColor("#d19a66") : QColor("#f1c40f"));
        selection.format.setForeground(QColor("#1f2229"));
        selections << selection;
    }

    registryDetailView->setExtraSelections(selections);
    QTextCursor activeCursor = registrySearchMatches[index];
    registryDetailView->setTextCursor(activeCursor);
    registryDetailView->centerCursor();
}

void MainWindow::clearRegistrySearchHighlights()
{
    registrySearchMatches.clear();
    registrySearchKeyword.clear();
    currentRegistrySearchIndex = -1;

    if (registryDetailView != nullptr)
    {
        registryDetailView->setExtraSelections({});
    }
}

bool MainWindow::applyRegistryFilterToItem(QTreeWidgetItem *item, const QString &filterRule)
{
    if (item == nullptr)
    {
        return false;
    }

    bool childVisible = false;
    for (int i = 0; i < item->childCount(); ++i)
    {
        childVisible = applyRegistryFilterToItem(item->child(i), filterRule) || childVisible;
    }

    const QString itemPath = item->data(0, RelativePathRole).toString();
    const bool selfVisible = filterRule.isEmpty() ||
                             item->text(0).contains(filterRule, Qt::CaseInsensitive) ||
                             itemPath.contains(filterRule, Qt::CaseInsensitive);
    const bool visible = selfVisible || childVisible;
    item->setHidden(!visible);
    return visible;
}

void MainWindow::appendLogMessage(const QString &message)
{
    if (bottomLogView == nullptr)
    {
        return;
    }

    bottomLogView->appendPlainText(message);
    bottomLogView->moveCursor(QTextCursor::End);
}

void MainWindow::setSaveResultEnabled(bool enabled)
{
    if (saveResultAction != nullptr)
    {
        saveResultAction->setEnabled(enabled);
    }
}

void MainWindow::resetAnalysisSummaryCache()
{
    m_analysisSummaryHeaderLines.clear();
    m_analysisDetailBlocks.clear();
    m_analysisRenderedBlockCount = 0;
    m_analysisRecordedEventCount = 0;
    m_analysisLazyLoading = false;
}

QString MainWindow::localizedText(const QString &zhText, const QString &enText) const
{
    return uiLanguage == 0 ? zhText : enText;
}

QString MainWindow::localizedAnalysisAction(const QString &actionKey) const
{
    if (actionKey == "added")
    {
        return localizedText("新增", "Added");
    }

    if (actionKey == "deleted")
    {
        return localizedText("删除", "Deleted");
    }

    if (actionKey == "modified")
    {
        return localizedText("修改", "Modified");
    }

    return localizedText("未知", "Unknown");
}

void MainWindow::startRegistryAnalysis()
{
    if (startAnalysisAction != nullptr && !startAnalysisAction->isEnabled())
    {
        return;
    }

    if (m_registryMonitor != nullptr && m_registryMonitor->isRunning())
    {
        appendLogMessage(localizedText(
            "ETW 实时分析已在运行中。",
            "ETW real-time analysis is already running."));
        return;
    }

    if (!isRunningAsAdministrator())
    {
        appendLogMessage(localizedText(
            "实时分析启动失败：需要管理员权限，请先切换到管理员权限后重试。",
            "Unable to start real-time analysis: administrator privileges are required. Switch to administrator mode and try again."));
        updateRightDisplayMeta(localizedText("实时分析需要管理员权限", "Administrator privileges required"));
        QMessageBox::warning(
            this,
            localizedText("需要管理员权限", "Administrator Privileges Required"),
            localizedText(
                "开始实时分析需要管理员权限。\n请先通过“编辑 -> 切换管理员权限”完成提权后再重试。",
                "Starting real-time analysis requires administrator privileges.\nUse \"Edit -> Toggle Administrator Privileges\" first, then try again."));
        return;
    }

    QString initialConfigPath;
    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        initialConfigPath = QDir::fromNativeSeparators(settings.value("Runtime/analysis_config_file").toString());
        m_analysisRiskLevel = settings.value("Runtime/analysis_risk_level", m_analysisRiskLevel).toInt();
    }
    if (!initialConfigPath.isEmpty() && !QFile::exists(initialConfigPath))
    {
        initialConfigPath.clear();
    }

    const QFileInfo initialConfigInfo(initialConfigPath);
    const QString initialDirectory = initialConfigInfo.exists() ? initialConfigInfo.absolutePath() : findConfigDirectory();
    AnalysisOptionsDialog dialog(this, uiLanguage, m_analysisRiskLevel, initialConfigPath, initialDirectory);
    if (dialog.exec() != QDialog::Accepted)
    {
        appendLogMessage(localizedText(
            "已取消实时分析配置。",
            "Real-time analysis configuration was canceled."));
        return;
    }

    m_analysisEvents.clear();
    resetAnalysisSummaryCache();
    m_analysisStartTime = QDateTime::currentDateTime();
    m_analysisConfigPath = dialog.configPath();
    m_analysisRiskLevel = dialog.riskLevel();
    m_analysisRunning = true;
    if (startAnalysisAction != nullptr)
    {
        startAnalysisAction->setEnabled(false);
    }

    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("Runtime/analysis_config_file", m_analysisConfigPath);
        settings.setValue("Runtime/analysis_risk_level", m_analysisRiskLevel);
        settings.sync();
    }

    if (rightDisplayTitle != nullptr)
    {
        rightDisplayTitle->setText(localizedText("实时分析结果", "Real-Time Analysis"));
    }
    m_saveResultContentType = 0;
    setSaveResultEnabled(false);
    if (registryDetailView != nullptr)
    {
        registryDetailView->setPlainText(localizedText(
            QString("实时分析已启动。\n危险等级：%1\n配置文件：%2\n\n正在等待注册表改动事件...")
                .arg(QString::number(m_analysisRiskLevel), QDir::toNativeSeparators(m_analysisConfigPath)),
            QString("Real-time analysis started.\nRisk level: %1\nConfiguration file: %2\n\nWaiting for registry change events...")
                .arg(QString::number(m_analysisRiskLevel), QDir::toNativeSeparators(m_analysisConfigPath))));
    }
    updateRightDisplayMeta(localizedText("正在初始化 ETW 监听", "Initializing ETW listener"));
    appendLogMessage(localizedText(
        QString("开始 ETW 实时分析，危险等级：%1，配置文件：%2")
            .arg(QString::number(m_analysisRiskLevel), QDir::toNativeSeparators(m_analysisConfigPath)),
        QString("Starting ETW real-time analysis. Risk level: %1, configuration file: %2")
            .arg(QString::number(m_analysisRiskLevel), QDir::toNativeSeparators(m_analysisConfigPath))));

    if (m_registryMonitor != nullptr && !m_registryMonitor->startMonitoring(m_analysisRiskLevel, m_analysisConfigPath))
    {
        m_analysisRunning = false;
        if (startAnalysisAction != nullptr)
        {
            startAnalysisAction->setEnabled(true);
        }
        appendLogMessage(localizedText(
            "启动 ETW 实时分析失败。",
            "Failed to start ETW real-time analysis."));
    }
}

void MainWindow::stopRegistryAnalysis()
{
    if (m_registryMonitor == nullptr || !m_registryMonitor->isRunning())
    {
        appendLogMessage(localizedText(
            "当前没有正在进行的 ETW 实时分析。",
            "There is no ETW real-time analysis currently running."));
        if (!m_analysisEvents.isEmpty())
        {
            renderRegistryAnalysisSummary();
        }
        return;
    }

    m_registryMonitor->stopMonitoring();
    m_analysisRunning = false;
    renderRegistryAnalysisSummary();
    appendLogMessage(localizedText(
        "ETW 实时分析已结束，汇总结果已展示到右上区。",
        "ETW real-time analysis has ended. The summary is now shown in the upper-right panel."));
}

void MainWindow::handleRegistryAnalysisEvent(const RegistryEtwEvent &eventInfo)
{
    m_analysisEvents.append(eventInfo);

    if (rightDisplayMeta != nullptr)
    {
        updateRightDisplayMeta(localizedText(
            QString("实时分析中 | 已记录 %1 条事件").arg(QString::number(m_analysisEvents.size())),
            QString("Real-time analysis running | %1 events recorded").arg(QString::number(m_analysisEvents.size()))));
    }
}

void MainWindow::renderRegistryAnalysisSummary()
{
    if (rightDisplayTitle != nullptr)
    {
        rightDisplayTitle->setText(localizedText("实时分析结果", "Real-Time Analysis"));
    }

    resetAnalysisSummaryCache();

    if (m_analysisEvents.isEmpty())
    {
        if (registryDetailView != nullptr)
        {
            registryDetailView->setPlainText(localizedText(
                "本次实时分析未捕获到注册表改动事件。",
                "No registry change events were captured during this analysis session."));
        }
        updateRightDisplayMeta(localizedText("0 条事件", "0 events"));
        m_saveResultContentType = 2;
        setSaveResultEnabled(true);
        return;
    }

    int addedCount = 0;
    int deletedCount = 0;
    int modifiedCount = 0;
    int filteredCount = 0;
    int unfilteredCount = 0;

    for (int index = 0; index < m_analysisEvents.size(); ++index)
    {
        const RegistryEtwEvent &eventInfo = m_analysisEvents.at(index);
        if (eventInfo.actionKey == "added")
        {
            ++addedCount;
        }
        else if (eventInfo.actionKey == "deleted")
        {
            ++deletedCount;
        }
        else if (eventInfo.actionKey == "modified")
        {
            ++modifiedCount;
        }

        if (eventInfo.filtered)
        {
            ++filteredCount;
        }
        else
        {
            ++unfilteredCount;
        }

        m_analysisDetailBlocks << formatAnalysisEventBlock(eventInfo);
    }

    m_analysisSummaryHeaderLines << localizedText("ETW 实时分析摘要", "ETW Real-Time Analysis Summary");
    m_analysisSummaryHeaderLines << QString();
    m_analysisSummaryHeaderLines << localizedText("[统计概览]", "[Summary]");
    m_analysisSummaryHeaderLines << localizedText(
        QString("开始时间：%1").arg(m_analysisStartTime.toString("yyyy-MM-dd HH:mm:ss")),
        QString("Start time: %1").arg(m_analysisStartTime.toString("yyyy-MM-dd HH:mm:ss")));
    m_analysisSummaryHeaderLines << localizedText(
        QString("结束时间：%1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")),
        QString("End time: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    m_analysisSummaryHeaderLines << localizedText(
        QString("危险等级：%1").arg(QString::number(m_analysisRiskLevel)),
        QString("Risk level: %1").arg(QString::number(m_analysisRiskLevel)));
    m_analysisSummaryHeaderLines << localizedText(
        QString("配置文件：%1").arg(QDir::toNativeSeparators(m_analysisConfigPath)),
        QString("Configuration file: %1").arg(QDir::toNativeSeparators(m_analysisConfigPath)));
    m_analysisSummaryHeaderLines << localizedText(
        QString("总事件数：%1").arg(QString::number(m_analysisEvents.size())),
        QString("Total events: %1").arg(QString::number(m_analysisEvents.size())));
    m_analysisSummaryHeaderLines << localizedText(
        QString("新增：%1 | 删除：%2 | 修改：%3").arg(QString::number(addedCount), QString::number(deletedCount), QString::number(modifiedCount)),
        QString("Added: %1 | Deleted: %2 | Modified: %3").arg(QString::number(addedCount), QString::number(deletedCount), QString::number(modifiedCount)));
    m_analysisSummaryHeaderLines << localizedText(
        QString("已过滤：%1 | 已记录：%2").arg(QString::number(filteredCount), QString::number(unfilteredCount)),
        QString("Filtered: %1 | Recorded: %2").arg(QString::number(filteredCount), QString::number(unfilteredCount)));
    m_analysisSummaryHeaderLines << QString();
    m_analysisSummaryHeaderLines << localizedText("[事件明细]", "[Events]");
    m_analysisRecordedEventCount = unfilteredCount;

    if (registryDetailView != nullptr)
    {
        registryDetailView->setPlainText(m_analysisSummaryHeaderLines.join('\n'));
        registryDetailView->moveCursor(QTextCursor::Start);
        appendNextAnalysisResultChunk();

        while (registryDetailView->verticalScrollBar() != nullptr &&
               registryDetailView->verticalScrollBar()->maximum() == 0 &&
               m_analysisRenderedBlockCount < m_analysisDetailBlocks.size())
        {
            appendNextAnalysisResultChunk();
        }
    }

    m_saveResultContentType = 2;
    setSaveResultEnabled(true);
}

void MainWindow::appendNextAnalysisResultChunk()
{
    if (registryDetailView == nullptr || m_analysisRenderedBlockCount >= m_analysisDetailBlocks.size())
    {
        return;
    }

    m_analysisLazyLoading = true;

    const int chunkSize = 50;
    const int nextBlockCount = qMin(m_analysisRenderedBlockCount + chunkSize, m_analysisDetailBlocks.size());
    const QString chunkText = m_analysisDetailBlocks.mid(
                                                        m_analysisRenderedBlockCount,
                                                        nextBlockCount - m_analysisRenderedBlockCount)
                                  .join("\n\n");

    QTextCursor cursor = registryDetailView->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText((m_analysisRenderedBlockCount == 0 ? "\n" : "\n\n") + chunkText);

    m_analysisRenderedBlockCount = nextBlockCount;
    updateRightDisplayMeta(localizedText(
        QString("%1 条事件 | %2 条已记录 | 已加载 %3/%4")
            .arg(QString::number(m_analysisEvents.size()),
                 QString::number(m_analysisRecordedEventCount),
                 QString::number(m_analysisRenderedBlockCount),
                 QString::number(m_analysisDetailBlocks.size())),
        QString("%1 events | %2 recorded | loaded %3/%4")
            .arg(QString::number(m_analysisEvents.size()),
                 QString::number(m_analysisRecordedEventCount),
                 QString::number(m_analysisRenderedBlockCount),
                 QString::number(m_analysisDetailBlocks.size()))));
    m_analysisLazyLoading = false;
}

QString MainWindow::formatAnalysisEventBlock(const RegistryEtwEvent &eventInfo) const
{
    const QString timestamp = eventInfo.timestamp.isValid() ? eventInfo.timestamp.toString("MM-dd HH:mm:ss")
                                                            : "--";
    const QString prefix = eventInfo.filtered
                               ? localizedText("[已过滤]", "[Filtered]")
                               : localizedText("[已记录]", "[Recorded]");
    const QString processText = eventInfo.filtered
                                    ? localizedText("已跳过进程查询", "Process lookup skipped")
                                    : QString("%1 / PID %2")
                                          .arg(eventInfo.processName.isEmpty() ? localizedText("未知进程", "Unknown process") : eventInfo.processName,
                                               QString::number(eventInfo.processId));
    const QString displayKeyPath = eventInfo.registryKeyPath.isEmpty() ? eventInfo.registryPath : eventInfo.registryKeyPath;
    const QString displayKeyName = eventInfo.registryKeyName.isEmpty()
                                       ? localizedText("(未知键名)", "(Unknown key)")
                                       : eventInfo.registryKeyName;
    const QString displayValueName = !eventInfo.hasRegistryValue
                                         ? localizedText("(无值)", "(No value)")
                                         : (eventInfo.registryValueName.compare("(Default)", Qt::CaseInsensitive) == 0
                                                ? localizedText("(默认)", "(Default)")
                                                : eventInfo.registryValueName);

    QString displayValueData = localizedText("(不适用)", "(Not applicable)");
    if (eventInfo.hasRegistryValue)
    {
        if (eventInfo.registryValueCaptured)
        {
            displayValueData = eventInfo.registryValueData.isEmpty()
                                   ? localizedText("(空字符串)", "(Empty string)")
                                   : eventInfo.registryValueData;
        }
        else if (eventInfo.actionKey == "deleted")
        {
            displayValueData = localizedText("(删除后无法读取)", "(Unavailable after deletion)");
        }
        else
        {
            displayValueData = localizedText("(未读取到值内容)", "(Value data unavailable)");
        }
    }

    QStringList lines;
    lines << QString("%1 %2 %3").arg(prefix, timestamp, localizedAnalysisAction(eventInfo.actionKey));
    lines << localizedText(QString("  键路径：%1").arg(displayKeyPath),
                           QString("  Key Path: %1").arg(displayKeyPath));
    lines << localizedText(QString("  键名称：%1").arg(displayKeyName),
                           QString("  Key Name: %1").arg(displayKeyName));
    lines << localizedText(QString("  值名称：%1").arg(displayValueName),
                           QString("  Value Name: %1").arg(displayValueName));
    lines << localizedText(QString("  值内容：%1").arg(displayValueData),
                           QString("  Value Data: %1").arg(displayValueData));
    lines << localizedText(QString("  进程：%1").arg(processText),
                           QString("  Process: %1").arg(processText));
    if (!eventInfo.filtered)
    {
        lines << localizedText(
            QString("  命令：%1").arg(eventInfo.commandLine.isEmpty() ? localizedText("未获取到命令行", "Command line unavailable") : eventInfo.commandLine),
            QString("  Command: %1").arg(eventInfo.commandLine.isEmpty() ? localizedText("未获取到命令行", "Command line unavailable") : eventInfo.commandLine));
    }

    return lines.join('\n');
}

QString MainWindow::buildFullAnalysisSummaryText() const
{
    QStringList lines = m_analysisSummaryHeaderLines;
    if (!m_analysisDetailBlocks.isEmpty())
    {
        lines << m_analysisDetailBlocks.join("\n\n");
    }
    return lines.join('\n');
}

void MainWindow::resetInMemoryComparisonShots()
{
    RegShot cleanup;
    cleanup.FreeShot(&m_firstCompareSnapshot);
    cleanup.FreeShot(&m_secondCompareSnapshot);
    m_compareSnapshotState = 0;
}

void MainWindow::captureComparisonSnapshot(bool isFirstShot)
{
    const QString phaseText = localizedText(
        isFirstShot ? "第一次快照" : "第二次快照",
        isFirstShot ? "first snapshot" : "second snapshot");
    appendLogMessage(localizedText(
        QString("开始拍摄用于比较的%1...").arg(phaseText),
        QString("Starting capture for the %1 comparison snapshot...").arg(phaseText)));

    if (snapshotProgressBar && snapshotProgressLabel)
    {
        snapshotProgressBar->setValue(0);
        snapshotProgressBar->show();
        snapshotProgressLabel->setText(localizedText(
            QString("正在准备%1...").arg(phaseText),
            QString("Preparing %1...").arg(phaseText)));
        snapshotProgressLabel->show();
    }

    QApplication::setOverrideCursor(Qt::BusyCursor);
    RegShot *regShot = new RegShot();

    auto callback = [this, phaseText](int percent, const QString &hiveName)
    {
        QMetaObject::invokeMethod(this, [this, percent, phaseText, hiveName]()
                                  {
                                      if (snapshotProgressBar && snapshotProgressLabel)
                                      {
                                          snapshotProgressBar->setValue(percent);
                                          snapshotProgressLabel->setText(localizedText(
                                              QString("正在拍摄%1: %2").arg(phaseText, hiveName),
                                              QString("Capturing %1: %2").arg(phaseText, hiveName)));
                                      } }, Qt::QueuedConnection);
    };

    QFuture<DWORD> future = QtConcurrent::run([regShot, callback]()
                                              { return regShot->TakeSnapShot(callback); });

    QFutureWatcher<DWORD> *watcher = new QFutureWatcher<DWORD>(this);
    connect(watcher, &QFutureWatcher<DWORD>::finished, this, [this, watcher, regShot, isFirstShot]()
            {
                const DWORD snapshotIndex = watcher->result();
                const bool isValidSnapshot = snapshotIndex < static_cast<DWORD>(regShot->Shots.size());
                bool reopenCompareDialog = false;
                bool runInMemoryCompare = false;

                if (!isValidSnapshot)
                {
                    appendLogMessage(localizedText(
                        "拍摄比较快照失败！未生成有效的内存快照。",
                        "Comparison snapshot capture failed: no valid in-memory snapshot was generated."));
                }
                else
                {
                    RegShotContent capturedShot = regShot->Shots.takeAt(static_cast<int>(snapshotIndex));
                    RegShot cleanup;
                    if (isFirstShot)
                    {
                        cleanup.FreeShot(&m_firstCompareSnapshot);
                        m_firstCompareSnapshot = capturedShot;
                        m_compareSnapshotState = 1;
                        appendLogMessage(localizedText(
                            "第一次快照拍摄完成，已保存在内存中。",
                            "The first snapshot has been captured and stored in memory."));
                        appendLogMessage(localizedText(
                            "请继续拍摄第二次快照，完成后将自动比较。",
                            "Capture the second snapshot next. Comparison will start automatically when it finishes."));
                        reopenCompareDialog = true;
                    }
                    else
                    {
                        cleanup.FreeShot(&m_secondCompareSnapshot);
                        m_secondCompareSnapshot = capturedShot;
                        m_compareSnapshotState = 2;
                        appendLogMessage(localizedText(
                            "第二次快照拍摄完成，开始比较两次内存快照...",
                            "The second snapshot has been captured. Comparing the two in-memory snapshots..."));
                        runInMemoryCompare = true;
                    }
                }

                QApplication::restoreOverrideCursor();
                if (snapshotProgressBar && snapshotProgressLabel)
                {
                    snapshotProgressBar->hide();
                    snapshotProgressLabel->hide();
                }

                delete regShot;
                watcher->deleteLater();

                if (runInMemoryCompare)
                {
                    compareInMemorySnapshots();
                }
                else if (reopenCompareDialog)
                {
                    QMetaObject::invokeMethod(this, [this]()
                                              { compareSnapshots(); }, Qt::QueuedConnection);
                } });

    watcher->setFuture(future);
}

void MainWindow::renderComparisonResult(RegShot &regShot,
                                        LPRegShotContent shot1,
                                        LPRegShotContent shot2,
                                        const QString &sourceOne,
                                        const QString &sourceTwo)
{
    regShot.CompareShots(shot1, shot2);

    const CompareResults &compareResult = regShot.CompareResult;
    QStringList lines;
    const QString overviewLine = localizedText(
        QString("总计 %1 项 | 新增 %2 | 删除 %3 | 修改 %4")
            .arg(compareResult.dwCompared.cAll)
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwModified.cAll),
        QString("Total %1 items | Added %2 | Deleted %3 | Modified %4")
            .arg(compareResult.dwCompared.cAll)
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwModified.cAll));

    lines << QString(64, '=');
    lines << localizedText("快照比较结果", "Snapshot Comparison Result");
    lines << overviewLine;
    lines << QString(64, '=');
    lines << localizedText(
        QString("基准快照: %1").arg(sourceOne),
        QString("Base snapshot: %1").arg(sourceOne));
    lines << localizedText(
        QString("对比快照: %1").arg(sourceTwo),
        QString("Compared snapshot: %1").arg(sourceTwo));
    lines << "";
    lines << localizedText("[统计概览]", "[Summary]");
    lines << QString(64, '-');
    lines << localizedText(
        QString("已比较: %1 项 (键 %2 / 值 %3)")
            .arg(compareResult.dwCompared.cAll)
            .arg(compareResult.dwCompared.cKeys)
            .arg(compareResult.dwCompared.cValues),
        QString("Compared: %1 items (keys %2 / values %3)")
            .arg(compareResult.dwCompared.cAll)
            .arg(compareResult.dwCompared.cKeys)
            .arg(compareResult.dwCompared.cValues));
    lines << localizedText(
        QString("新增: %1 项 (键 %2 / 值 %3)")
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwAdded.cKeys)
            .arg(compareResult.dwAdded.cValues),
        QString("Added: %1 items (keys %2 / values %3)")
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwAdded.cKeys)
            .arg(compareResult.dwAdded.cValues));
    lines << localizedText(
        QString("删除: %1 项 (键 %2 / 值 %3)")
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwDeleted.cKeys)
            .arg(compareResult.dwDeleted.cValues),
        QString("Deleted: %1 items (keys %2 / values %3)")
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwDeleted.cKeys)
            .arg(compareResult.dwDeleted.cValues));
    lines << localizedText(
        QString("修改: %1 项 (键 %2 / 值 %3)")
            .arg(compareResult.dwModified.cAll)
            .arg(compareResult.dwModified.cKeys)
            .arg(compareResult.dwModified.cValues),
        QString("Modified: %1 items (keys %2 / values %3)")
            .arg(compareResult.dwModified.cAll)
            .arg(compareResult.dwModified.cKeys)
            .arg(compareResult.dwModified.cValues));

    auto appendSection = [&lines](const QString &title)
    {
        lines << "";
        lines << title;
        lines << QString(64, '-');
    };

    auto appendKeyList = [this, &lines, &appendSection](LPCompares head, const QString &sectionTitle, bool useNewPointer)
    {
        appendSection(sectionTitle);
        if (head == nullptr)
        {
            lines << localizedText("  (无)", "  (none)");
            return;
        }

        int index = 1;
        for (LPCompares current = head; current != nullptr; current = current->lpNext)
        {
            LPKeyContent key = reinterpret_cast<LPKeyContent>(useNewPointer ? current->lpNew : current->lpOld);
            lines << QString("  %1. %2").arg(index++).arg(buildKeyPath(key));
        }
    };

    auto appendValueList = [this, &lines, &appendSection](LPCompares head, const QString &sectionTitle, bool useNewPointer)
    {
        appendSection(sectionTitle);
        if (head == nullptr)
        {
            lines << localizedText("  (无)", "  (none)");
            return;
        }

        int index = 1;
        for (LPCompares current = head; current != nullptr; current = current->lpNext)
        {
            LPValueContent value = reinterpret_cast<LPValueContent>(useNewPointer ? current->lpNew : current->lpOld);
            const QByteArray valueData(
                reinterpret_cast<const char *>(value->lpValueData),
                static_cast<int>(value->dwDataSize));
            lines << QString("  %1. %2").arg(index++).arg(buildValuePath(value));
            lines << QString("      %1").arg(formatRegistryValue(value->dwTypeCode, valueData));
        }
    };

    appendKeyList(compareResult.CRHeads.lpKeyAdded,
                  localizedText("[新增键]", "[Added Keys]"),
                  true);
    appendKeyList(compareResult.CRHeads.lpKeyDeleted,
                  localizedText("[删除键]", "[Deleted Keys]"),
                  false);
    appendValueList(compareResult.CRHeads.lpValueAdded,
                    localizedText("[新增值]", "[Added Values]"),
                    true);
    appendValueList(compareResult.CRHeads.lpValueDeleted,
                    localizedText("[删除值]", "[Deleted Values]"),
                    false);

    appendSection(localizedText("[修改值]", "[Modified Values]"));
    if (compareResult.CRHeads.lpValueModified == nullptr)
    {
        lines << localizedText("  (无)", "  (none)");
    }
    else
    {
        int index = 1;
        for (LPCompares current = compareResult.CRHeads.lpValueModified; current != nullptr; current = current->lpNext)
        {
            LPValueContent oldValue = reinterpret_cast<LPValueContent>(current->lpOld);
            LPValueContent newValue = reinterpret_cast<LPValueContent>(current->lpNew);
            const QByteArray oldData(
                reinterpret_cast<const char *>(oldValue->lpValueData),
                static_cast<int>(oldValue->dwDataSize));
            const QByteArray newData(
                reinterpret_cast<const char *>(newValue->lpValueData),
                static_cast<int>(newValue->dwDataSize));

            lines << QString("  %1. %2").arg(index++).arg(buildValuePath(newValue));
            lines << localizedText(
                QString("      旧值: %1").arg(formatRegistryValue(oldValue->dwTypeCode, oldData)),
                QString("      Old: %1").arg(formatRegistryValue(oldValue->dwTypeCode, oldData)));
            lines << localizedText(
                QString("      新值: %1").arg(formatRegistryValue(newValue->dwTypeCode, newData)),
                QString("      New: %1").arg(formatRegistryValue(newValue->dwTypeCode, newData)));
        }
    }

    if (rightDisplayTitle != nullptr)
    {
        rightDisplayTitle->setText(localizedText("快照比较结果", "Snapshot Comparison Result"));
    }
    if (registryDetailView != nullptr)
    {
        registryDetailView->setPlainText(lines.join('\n'));
        registryDetailView->moveCursor(QTextCursor::Start);
    }
    resetAnalysisSummaryCache();
    updateRightDisplayMeta(localizedText(
        QString("比较完成 | 新增 %1 | 删除 %2 | 修改 %3")
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwModified.cAll),
        QString("Comparison complete | added %1 | deleted %2 | modified %3")
            .arg(compareResult.dwAdded.cAll)
            .arg(compareResult.dwDeleted.cAll)
            .arg(compareResult.dwModified.cAll)));
    m_saveResultContentType = 1;
    setSaveResultEnabled(true);
}

void MainWindow::compareInMemorySnapshots()
{
    if (!m_firstCompareSnapshot.fFinished || !m_secondCompareSnapshot.fFinished)
    {
        appendLogMessage(localizedText(
            "内存快照未准备完成，无法执行比较。",
            "The in-memory snapshots are not ready, so comparison cannot start."));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    RegShot regShot;
    renderComparisonResult(regShot,
                           &m_firstCompareSnapshot,
                           &m_secondCompareSnapshot,
                           localizedText("第一次内存快照", "First in-memory snapshot"),
                           localizedText("第二次内存快照", "Second in-memory snapshot"));
    QApplication::restoreOverrideCursor();

    appendLogMessage(localizedText(
        "两次内存快照比较完毕！",
        "The two in-memory snapshots have been compared."));
    resetInMemoryComparisonShots();
}

ConfigMap MainWindow::readIni(const QString &filePath)
{
    ConfigMap config;
    QSettings settings(filePath, QSettings::IniFormat);
    // 获取所有节
    QStringList sections = settings.childGroups();
    // for循环遍历节
    for (int i = 0; i < sections.size(); ++i)
    {
        const QString &section = sections[i];
        settings.beginGroup(section);
        // 获取当前节的所有键
        QStringList keys = settings.childKeys();
        // 传统for循环遍历键
        for (int j = 0; j < keys.size(); ++j)
        {
            const QString &key = keys[j];
            config[section][key] = settings.value(key).toString();
        }
        settings.endGroup();
    }
    return config;
}

ConfigMap MainWindow::getAppConfig()
{
    // 确保 user.ini 存在
    ensureUserConfigExists();

    // 1. 加载默认配置
    ConfigMap config = readIni(resolveConfigPath("default.ini"));

    // 2. 加载用户配置并覆盖
    const QString userIniPath = writableConfigPath("user.ini");
    if (!userIniPath.isEmpty() && QFileInfo::exists(userIniPath))
    {
        ConfigMap userConfig = readIni(userIniPath);
        for (auto sectionIt = userConfig.begin(); sectionIt != userConfig.end(); ++sectionIt)
        {
            const QString &sectionName = sectionIt.key();
            for (auto keyIt = sectionIt.value().begin(); keyIt != sectionIt.value().end(); ++keyIt)
            {
                config[sectionName][keyIt.key()] = keyIt.value();
            }
        }
    }
    return config;
}

void MainWindow::ensureUserConfigExists()
{
    const QString userIniPath = writableConfigPath("user.ini");
    if (userIniPath.isEmpty())
    {
        return;
    }

    QFileInfo fileInfo(userIniPath);
    if (!fileInfo.exists())
    {
        // 确保目录存在
        QDir().mkpath(fileInfo.absolutePath());

        // 创建一个带有基础结构的空文件
        QFile file(userIniPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream stream(&file);
            stream << "[System]\n";
            file.close();
        }
    }
}

QString MainWindow::resolveConfigPath(const QString &fileName) const
{
    const QString configDirectory = findConfigDirectory();
    if (!configDirectory.isEmpty())
    {
        const QString diskPath = QDir(configDirectory).filePath(fileName);
        if (QFileInfo::exists(diskPath))
        {
            return diskPath;
        }
    }

    return QString(":/admin/configs/%1").arg(fileName);
}

QString MainWindow::writableConfigPath(const QString &fileName) const
{
    const QString configDirectory = findConfigDirectory();
    return configDirectory.isEmpty() ? QString() : QDir(configDirectory).filePath(fileName);
}

QString MainWindow::expandConfigPath(const QString &path) const
{
    QString expandedPath = path;
    const QRegularExpression envPattern("%([^%]+)%");
    QRegularExpressionMatchIterator iterator = envPattern.globalMatch(expandedPath);
    while (iterator.hasNext())
    {
        const QRegularExpressionMatch match = iterator.next();
        const QByteArray variableName = match.captured(1).toUtf8();
        const QString variableValue = qEnvironmentVariable(variableName.constData());
        if (!variableValue.isEmpty())
        {
            expandedPath.replace(match.captured(0), variableValue);
        }
    }

    return QDir::fromNativeSeparators(expandedPath);
}

void MainWindow::openResultFile()
{
    const ConfigMap config = getAppConfig();

    QString initialDirectory = expandConfigPath(config["System"].value("open_result_dir"));
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = QDir::homePath();
    }

    const QString selectedFilePath = QFileDialog::getOpenFileName(
        this,
        localizedText("打开结果文件", "Open Result File"),
        initialDirectory,
        localizedText(
            "结果文件 (*.xml *.html);;XML 文件 (*.xml);;HTML 文件 (*.html)",
            "Result Files (*.xml *.html);;XML Files (*.xml);;HTML Files (*.html)"));

    if (selectedFilePath.isEmpty())
    {
        return;
    }

    QFile resultFile(selectedFilePath);
    if (!resultFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        appendLogMessage(localizedText("加载文件失败：", "Failed to load file: ") +
                         QDir::toNativeSeparators(selectedFilePath));
        return;
    }

    QTextStream stream(&resultFile);
    const QString fileContent = stream.readAll();
    resultFile.close();

    if (rightDisplayTitle != nullptr)
    {
        rightDisplayTitle->setText(localizedText("结果文件内容", "Result File Content"));
    }
    if (registryDetailView != nullptr)
    {
        registryDetailView->setPlainText(fileContent);
        registryDetailView->moveCursor(QTextCursor::Start);
    }

    const QString displayPath = QDir::toNativeSeparators(selectedFilePath);
    updateRightDisplayMeta(QString("%1 | %2 %3").arg(displayPath, QString::number(contentLineCount(fileContent)), localizedText("行", "lines")));
    appendLogMessage(localizedText("已加载文件：", "Loaded file: ") + displayPath);
    resetAnalysisSummaryCache();
    m_saveResultContentType = 0;
    setSaveResultEnabled(false);

    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("System/open_result_dir", QFileInfo(selectedFilePath).absolutePath());
        settings.sync();
    }
}

void MainWindow::loadConfigFile()
{
    const ConfigMap config = getAppConfig();

    QString initialDirectory = expandConfigPath(config["System"].value("load_config_dir"));
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = QDir::homePath();
    }

    const QString selectedFilePath = QFileDialog::getOpenFileName(
        this,
        localizedText("加载配置文件", "Load Config File"),
        initialDirectory,
        localizedText("配置文件 (*.ini);;INI 文件 (*.ini)",
                      "Config Files (*.ini);;INI Files (*.ini)"));

    if (selectedFilePath.isEmpty())
    {
        return;
    }

    QFile configFile(selectedFilePath);
    if (!configFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        appendLogMessage(localizedText("加载文件失败：", "Failed to load file: ") +
                         QDir::toNativeSeparators(selectedFilePath));
        return;
    }
    configFile.close();

    const QString displayPath = QDir::toNativeSeparators(selectedFilePath);
    appendLogMessage(localizedText("已加载文件：", "Loaded file: ") + displayPath);
    appendLogMessage(localizedText("请重启以生效。", "Please restart to take effect."));

    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("System/load_config_dir", QFileInfo(selectedFilePath).absolutePath());
        settings.setValue("System/active_config_file", selectedFilePath);
        settings.sync();
    }
}

void MainWindow::setSavePath()
{
    const ConfigMap config = getAppConfig();

    QString initialDirectory = expandConfigPath(config["System"].value("save_path_dir"));
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = expandConfigPath(config["System"].value("result_dir"));
    }
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = QDir::homePath();
    }

    const QString selectedDirectory = QFileDialog::getExistingDirectory(
        this,
        localizedText("设置保存路径", "Set Save Path"),
        initialDirectory,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (selectedDirectory.isEmpty())
    {
        return;
    }

    const QString displayPath = QDir::toNativeSeparators(selectedDirectory);
    appendLogMessage(localizedText("已设置保存路径：", "Save path set to: ") + displayPath);

    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("System/result_dir", selectedDirectory);
        settings.setValue("System/save_path_dir", selectedDirectory);
        settings.sync();
    }
}

void MainWindow::saveResult()
{
    if (registryDetailView == nullptr || m_saveResultContentType == 0)
    {
        appendLogMessage(localizedText(
            "当前右上区没有可保存的比较结果或分析结果。",
            "There is no comparison result or analysis result available to save in the upper-right panel."));
        return;
    }

    const QString effectiveContent = (m_saveResultContentType == 2)
                                         ? buildFullAnalysisSummaryText()
                                         : registryDetailView->toPlainText();
    if (effectiveContent.trimmed().isEmpty())
    {
        appendLogMessage(localizedText(
            "保存结果失败：当前结果内容为空。",
            "Failed to save result: the current result content is empty."));
        return;
    }

    const ConfigMap config = getAppConfig();
    QString saveDirectory = expandConfigPath(config["System"].value("result_dir"));
    if (saveDirectory.isEmpty())
    {
        saveDirectory = expandConfigPath(config["System"].value("save_path_dir"));
    }
    if (saveDirectory.isEmpty())
    {
        saveDirectory = QDir(QCoreApplication::applicationDirPath()).filePath("results");
    }
    if (QDir(saveDirectory).isRelative())
    {
        saveDirectory = QDir(QCoreApplication::applicationDirPath()).filePath(saveDirectory);
    }

    QDir saveDir(saveDirectory);
    if (!saveDir.exists() && !QDir().mkpath(saveDirectory))
    {
        appendLogMessage(localizedText(
            QString("保存结果失败：无法创建目录 %1").arg(QDir::toNativeSeparators(saveDirectory)),
            QString("Failed to save result: unable to create directory %1").arg(QDir::toNativeSeparators(saveDirectory))));
        return;
    }

    const QString prefix = (m_saveResultContentType == 1)
                               ? QStringLiteral("comparison_result_")
                               : QStringLiteral("analysis_result_");
    const QString outputFilePath = saveDir.filePath(
        prefix + QDateTime::currentDateTime().toString("yyyyMMddHHmmss") + ".txt");

    QFile outputFile(outputFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        appendLogMessage(localizedText(
            QString("保存结果失败：无法写入文件 %1").arg(QDir::toNativeSeparators(outputFilePath)),
            QString("Failed to save result: unable to write file %1").arg(QDir::toNativeSeparators(outputFilePath))));
        return;
    }

    QTextStream stream(&outputFile);
    stream.setEncoding(QStringConverter::Utf8);
    stream << effectiveContent;
    outputFile.close();

    appendLogMessage(localizedText(
        QString("结果已保存为 TXT 文件：%1").arg(QDir::toNativeSeparators(outputFilePath)),
        QString("The result has been saved as a TXT file: %1").arg(QDir::toNativeSeparators(outputFilePath))));
}

void MainWindow::takeSnapshot()
{
    const ConfigMap config = getAppConfig();

    if (takeSnapshotAction != nullptr)
    {
        takeSnapshotAction->setEnabled(false);
    }

    QString saveDirectory = expandConfigPath(config["System"].value("save_path_dir"));
    if (saveDirectory.isEmpty() || !QFileInfo::exists(saveDirectory))
    {
        saveDirectory = expandConfigPath(config["System"].value("result_dir"));
    }
    if (saveDirectory.isEmpty())
    {
        saveDirectory = QDir(QCoreApplication::applicationDirPath()).filePath("results");
    }
    if (QDir(saveDirectory).isRelative())
    {
        saveDirectory = QDir(QCoreApplication::applicationDirPath()).filePath(saveDirectory);
    }

    QDir saveDir(saveDirectory);
    if (!saveDir.exists() && !QDir().mkpath(saveDirectory))
    {
        if (takeSnapshotAction != nullptr)
        {
            takeSnapshotAction->setEnabled(true);
        }
        appendLogMessage(localizedText(
            "快照拍摄失败！无法创建保存目录。",
            "Snapshot failed: unable to create target directory."));
        return;
    }

    // 显示进度条和标签
    if (snapshotProgressBar && snapshotProgressLabel)
    {
        snapshotProgressBar->setValue(0);
        snapshotProgressBar->show();
        snapshotProgressLabel->setText(localizedText("正在准备快照...", "Preparing snapshot..."));
        snapshotProgressLabel->show();
    }

    // 禁用拍摄按钮以防重复触发 (这里可以遍历菜单或简单处理)
    // 为简单起见，我们使用 WaitCursor 的视觉反馈
    QApplication::setOverrideCursor(Qt::BusyCursor);

    // 在堆上创建 RegShot 对象，以便在后台线程中存活
    RegShot *regShot = new RegShot();

    // 定义进度回调
    auto callback = [this](int percent, const QString &hiveName)
    {
        QMetaObject::invokeMethod(this, [this, percent, hiveName]()
                                  {
            if (snapshotProgressBar && snapshotProgressLabel) {
                snapshotProgressBar->setValue(percent);
                snapshotProgressLabel->setText(localizedText(
                    QString("正在拍摄: %1").arg(hiveName),
                    QString("Taking: %1").arg(hiveName)));
            } }, Qt::QueuedConnection);
    };

    // 使用 QtConcurrent 在后台运行
    QFuture<DWORD> future = QtConcurrent::run([regShot, callback]()
                                              { return regShot->TakeSnapShot(callback); });

    // 使用 QFutureWatcher 监听完成
    QFutureWatcher<DWORD> *watcher = new QFutureWatcher<DWORD>(this);
    connect(watcher, &QFutureWatcher<DWORD>::finished, this, [this, watcher, regShot, saveDirectory]()
            {
        DWORD snapshotIndex = watcher->result();
        
        const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMddHHmmss");
        const QString outputFilePath = QDir(saveDirectory).filePath(timestamp + ".rgs");

        if (snapshotIndex >= static_cast<DWORD>(regShot->Shots.size()))
        {
            appendLogMessage(localizedText(
                "快照拍摄失败！未生成有效快照。",
                "Snapshot failed: no valid snapshot was generated."));
        }
        else
        {
            regShot->SaveSnapShot(snapshotIndex, outputFilePath);
            const QFileInfo savedFileInfo(outputFilePath);
            if (!savedFileInfo.exists() || savedFileInfo.size() <= 0)
            {
                appendLogMessage(localizedText(
                    "快照拍摄失败！无法写入保存文件。",
                    "Snapshot failed: unable to write output file."));
            }
            else
            {
                appendLogMessage(localizedText(
                    QString("快照拍摄完毕！已保存于：%1").arg(QDir::toNativeSeparators(outputFilePath)),
                    QString("Snapshot completed! Saved to: %1").arg(QDir::toNativeSeparators(outputFilePath))));
            }
        }

        // 恢复 UI 状态
        QApplication::restoreOverrideCursor();
        if (snapshotProgressBar && snapshotProgressLabel) {
            snapshotProgressBar->hide();
            snapshotProgressLabel->hide();
        }
        if (takeSnapshotAction != nullptr)
        {
            takeSnapshotAction->setEnabled(true);
        }

        // 清理
        delete regShot;
        watcher->deleteLater(); });

    watcher->setFuture(future);
}

void MainWindow::compareSnapshots()
{
    const ConfigMap config = getAppConfig();

    QString initialDirectory = expandConfigPath(config["System"].value("compare_snapshot_dir"));
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = expandConfigPath(config["System"].value("save_path_dir"));
    }
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = expandConfigPath(config["System"].value("result_dir"));
    }
    if (initialDirectory.isEmpty() || !QFileInfo::exists(initialDirectory))
    {
        initialDirectory = QDir::homePath();
    }

    CompareSelectionDialog dialog(this, uiLanguage, initialDirectory, m_compareSnapshotState);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    if (dialog.requestedAction() == CompareSelectionDialog::CaptureDirectSnapshot)
    {
        captureComparisonSnapshot(m_compareSnapshotState == 0);
        return;
    }

    const QString firstPath = dialog.firstPath();
    const QString secondPath = dialog.secondPath();

    if (firstPath.isEmpty() || secondPath.isEmpty())
    {
        appendLogMessage(localizedText(
            "快照比较已取消：请选择两个快照文件。",
            "Snapshot comparison cancelled: please select two snapshot files."));
        return;
    }

    // 记录最后一次选取路径到 user.ini
    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        // 这里记录第二次快照选取时的目录作为“最后一次”
        settings.setValue("System/compare_snapshot_dir", QFileInfo(secondPath).absolutePath());
        settings.sync();
    }

    appendLogMessage(localizedText(
        QString("开始比较文件快照：%1 与 %2")
            .arg(QFileInfo(firstPath).fileName(), QFileInfo(secondPath).fileName()),
        QString("Comparing snapshot files: %1 and %2")
            .arg(QFileInfo(firstPath).fileName(), QFileInfo(secondPath).fileName())));

    QApplication::setOverrideCursor(Qt::WaitCursor);
    RegShot regShot;
    RegShotContent shot1 = {};
    RegShotContent shot2 = {};

    const bool loadFirst = regShot.LoadShot(&shot1, firstPath);
    const bool loadSecond = regShot.LoadShot(&shot2, secondPath);
    if (!loadFirst || !loadSecond)
    {
        QApplication::restoreOverrideCursor();
        regShot.FreeShot(&shot1);
        regShot.FreeShot(&shot2);
        appendLogMessage(localizedText(
            "快照比较失败！无法加载选中的快照文件。",
            "Snapshot comparison failed: unable to load the selected snapshot files."));
        return;
    }

    renderComparisonResult(regShot,
                           &shot1,
                           &shot2,
                           QDir::toNativeSeparators(firstPath),
                           QDir::toNativeSeparators(secondPath));
    QApplication::restoreOverrideCursor();

    appendLogMessage(localizedText("快照比较完毕！", "Snapshot comparison completed!"));

    regShot.FreeShot(&shot1);
    regShot.FreeShot(&shot2);
}

void MainWindow::showRegistrySearchDialog()
{
    if (registrySearchDialog == nullptr)
    {
        registrySearchDialog = new QDialog(this, Qt::Tool | Qt::WindowCloseButtonHint);
        registrySearchDialog->setModal(false);
        registrySearchDialog->setWindowTitle(localizedText("在注册表中搜索", "Search In Registry"));
        registrySearchDialog->setMinimumWidth(420);
        registrySearchDialog->setStyleSheet(
            "QDialog {"
            "  background-color: #1f232a;"
            "  border: 1px solid #31343b;"
            "  border-radius: 10px;"
            "}"
            "QLabel#dialogTitle {"
            "  color: #f0f3f8;"
            "  font-size: 13pt;"
            "  font-weight: 600;"
            "  font-family: 'Segoe UI', 'Microsoft YaHei UI';"
            "}"
            "QLineEdit {"
            "  background-color: #181b20;"
            "  color: #d7dae0;"
            "  border: 1px solid #3a404a;"
            "  border-radius: 8px;"
            "  padding: 10px 12px;"
            "  selection-background-color: #2f65ca;"
            "}"
            "QLineEdit:focus {"
            "  border: 1px solid #4c8dff;"
            "}"
            "QPushButton {"
            "  min-height: 34px;"
            "  padding: 0 14px;"
            "  border-radius: 8px;"
            "  border: 1px solid #3a404a;"
            "  background-color: #2a2f38;"
            "  color: #d7dae0;"
            "  font-family: 'Segoe UI', 'Microsoft YaHei UI';"
            "}"
            "QPushButton:hover {"
            "  background-color: #343b46;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #222730;"
            "}"
            "QPushButton#primaryAction {"
            "  background-color: #2f65ca;"
            "  border: 1px solid #2f65ca;"
            "  color: white;"
            "}"
            "QPushButton#primaryAction:hover {"
            "  background-color: #3a73df;"
            "}");

        QVBoxLayout *dialogLayout = new QVBoxLayout(registrySearchDialog);
        dialogLayout->setContentsMargins(16, 16, 16, 16);
        dialogLayout->setSpacing(12);

        QLabel *titleLabel = new QLabel(localizedText("在注册表中搜索", "Search In Registry"), registrySearchDialog);
        titleLabel->setObjectName("dialogTitle");

        registrySearchInput = new QLineEdit(registrySearchDialog);
        registrySearchInput->setPlaceholderText(localizedText("请输入搜索字符串", "Enter search text"));
        registrySearchInput->setClearButtonEnabled(true);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(8);

        registrySearchFindButton = new QPushButton(localizedText("查找", "Find"), registrySearchDialog);
        registrySearchFindButton->setObjectName("primaryAction");
        registrySearchNextButton = new QPushButton(localizedText("下一个", "Next"), registrySearchDialog);
        registrySearchPreviousButton = new QPushButton(localizedText("上一个", "Previous"), registrySearchDialog);
        registrySearchFindButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
        registrySearchNextButton->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
        registrySearchPreviousButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));

        buttonLayout->addWidget(registrySearchFindButton);
        buttonLayout->addWidget(registrySearchNextButton);
        buttonLayout->addWidget(registrySearchPreviousButton);

        dialogLayout->addWidget(titleLabel);
        dialogLayout->addWidget(registrySearchInput);
        dialogLayout->addLayout(buttonLayout);

        connect(registrySearchFindButton, &QPushButton::clicked, this, &MainWindow::performRegistrySearch);
        connect(registrySearchNextButton, &QPushButton::clicked, this,
                [this]()
                {
                    navigateRegistrySearch(1);
                });
        connect(registrySearchPreviousButton, &QPushButton::clicked, this,
                [this]()
                {
                    navigateRegistrySearch(-1);
                });
        connect(registrySearchInput, &QLineEdit::returnPressed, this, &MainWindow::performRegistrySearch);
        connect(registrySearchDialog, &QDialog::finished, this,
                [this](int)
                {
                    clearRegistrySearchHighlights();
                });
    }

    registrySearchDialog->show();
    registrySearchDialog->raise();
    registrySearchDialog->activateWindow();
    if (registrySearchInput != nullptr)
    {
        registrySearchInput->setFocus();
        registrySearchInput->selectAll();
    }
}

void MainWindow::performRegistrySearch()
{
    if (registrySearchInput == nullptr)
    {
        return;
    }

    registrySearchKeyword = registrySearchInput->text().trimmed();
    refreshRegistrySearchHighlights(true);
}

void MainWindow::navigateRegistrySearch(int step)
{
    if (registrySearchKeyword.isEmpty())
    {
        performRegistrySearch();
        return;
    }

    if (registrySearchMatches.isEmpty())
    {
        refreshRegistrySearchHighlights(true);
        return;
    }

    currentRegistrySearchIndex =
        (currentRegistrySearchIndex + step + registrySearchMatches.size()) % registrySearchMatches.size();
    focusRegistrySearchResult(currentRegistrySearchIndex);
}

void MainWindow::showRegistryFilterDialog()
{
    if (registryFilterDialog == nullptr)
    {
        registryFilterDialog = new QDialog(this, Qt::Tool | Qt::WindowCloseButtonHint);
        registryFilterDialog->setModal(false);
        registryFilterDialog->setWindowTitle(localizedText("设置过滤器", "Set Filter"));
        registryFilterDialog->setMinimumWidth(400);
        registryFilterDialog->setStyleSheet(
            "QDialog {"
            "  background-color: #1f232a;"
            "  border: 1px solid #31343b;"
            "  border-radius: 10px;"
            "}"
            "QLabel#dialogTitle {"
            "  color: #f0f3f8;"
            "  font-size: 13pt;"
            "  font-weight: 600;"
            "  font-family: 'Segoe UI', 'Microsoft YaHei UI';"
            "}"
            "QLineEdit {"
            "  background-color: #181b20;"
            "  color: #d7dae0;"
            "  border: 1px solid #3a404a;"
            "  border-radius: 8px;"
            "  padding: 10px 12px;"
            "  selection-background-color: #2f65ca;"
            "}"
            "QLineEdit:focus {"
            "  border: 1px solid #4c8dff;"
            "}"
            "QPushButton {"
            "  min-height: 36px;"
            "  min-width: 44px;"
            "  border-radius: 8px;"
            "  border: 1px solid #2f65ca;"
            "  background-color: #2f65ca;"
            "  color: white;"
            "}"
            "QPushButton:hover {"
            "  background-color: #3a73df;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #2759b1;"
            "}");

        QVBoxLayout *dialogLayout = new QVBoxLayout(registryFilterDialog);
        dialogLayout->setContentsMargins(16, 16, 16, 16);
        dialogLayout->setSpacing(12);

        QLabel *titleLabel = new QLabel(localizedText("设置过滤器", "Set Filter"), registryFilterDialog);
        titleLabel->setObjectName("dialogTitle");

        registryFilterInput = new QLineEdit(registryFilterDialog);
        registryFilterInput->setPlaceholderText(localizedText("请输入过滤规则", "Enter filter rule"));
        registryFilterInput->setClearButtonEnabled(true);

        QHBoxLayout *buttonLayout = new QHBoxLayout();
        buttonLayout->setSpacing(8);

        registryFilterApplyButton = new QPushButton(registryFilterDialog);
        registryFilterApplyButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
        registryFilterApplyButton->setToolTip(localizedText("应用过滤器", "Apply Filter"));
        registryFilterApplyButton->setText(localizedText("过滤", "Apply"));

        buttonLayout->addWidget(registryFilterInput, 1);
        buttonLayout->addWidget(registryFilterApplyButton);

        dialogLayout->addWidget(titleLabel);
        dialogLayout->addLayout(buttonLayout);

        connect(registryFilterApplyButton, &QPushButton::clicked, this, &MainWindow::applyRegistryFilter);
        connect(registryFilterInput, &QLineEdit::returnPressed, this, &MainWindow::applyRegistryFilter);
    }

    registryFilterDialog->show();
    registryFilterDialog->raise();
    registryFilterDialog->activateWindow();
    if (registryFilterInput != nullptr)
    {
        registryFilterInput->setText(currentRegistryFilterRule);
        registryFilterInput->setFocus();
        registryFilterInput->selectAll();
    }
}

void MainWindow::applyRegistryFilter()
{
    if (registryTree == nullptr || registryFilterInput == nullptr)
    {
        return;
    }

    currentRegistryFilterRule = registryFilterInput->text().trimmed();
    for (int i = 0; i < registryTree->topLevelItemCount(); ++i)
    {
        applyRegistryFilterToItem(registryTree->topLevelItem(i), currentRegistryFilterRule);
    }

    if (currentRegistryFilterRule.isEmpty())
    {
        appendLogMessage(localizedText("设置过滤器结果：当前过滤器：无", "Filter updated: current filter: none"));
        return;
    }

    appendLogMessage(localizedText(
        QString("设置过滤器结果：当前过滤器：%1").arg(currentRegistryFilterRule),
        QString("Filter updated: current filter: %1").arg(currentRegistryFilterRule)));
}

bool MainWindow::isRunningAsAdministrator() const
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;

    if (AllocateAndInitializeSid(&ntAuthority,
                                 2,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0,
                                 &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin == TRUE;
}

void MainWindow::reloadRegistryTree(const QString &restoreRootName, const QString &restorePath)
{
    if (registryTree == nullptr)
    {
        return;
    }

    const bool previousUpdatesState = registryTree->updatesEnabled();
    registryTree->setUpdatesEnabled(false);
    registryTree->blockSignals(true);
    registryTree->clear();
    registryTree->blockSignals(false);

    appendRegistryRoot(QString::fromWCharArray(lpHKCR), HKEY_CLASSES_ROOT);
    appendRegistryRoot(QString::fromWCharArray(lpHKCU), HKEY_CURRENT_USER);
    appendRegistryRoot(QString::fromWCharArray(lpHKLM), HKEY_LOCAL_MACHINE);
    appendRegistryRoot(QString::fromWCharArray(lpHKU), HKEY_USERS);
    appendRegistryRoot(QString::fromWCharArray(lpHKCC), HKEY_CURRENT_CONFIG);

    if (!currentRegistryFilterRule.isEmpty())
    {
        for (int i = 0; i < registryTree->topLevelItemCount(); ++i)
        {
            applyRegistryFilterToItem(registryTree->topLevelItem(i), currentRegistryFilterRule);
        }
    }

    registryTree->setCurrentItem(nullptr);
    updateLeftPanelMeta(localizedText(
        QString("%1 个根项").arg(registryTree->topLevelItemCount()),
        QString("%1 root keys").arg(registryTree->topLevelItemCount())));

    if (!restoreRootName.isEmpty())
    {
        QTreeWidgetItem *currentItem = nullptr;
        for (int i = 0; i < registryTree->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem *candidate = registryTree->topLevelItem(i);
            if (candidate->text(0) == restoreRootName)
            {
                currentItem = candidate;
                break;
            }
        }

        if (currentItem != nullptr && !restorePath.isEmpty())
        {
            const QStringList pathSegments = restorePath.split('\\', Qt::SkipEmptyParts);
            for (const QString &segment : pathSegments)
            {
                populateRegistryChildren(currentItem);
                currentItem->setExpanded(true);

                QTreeWidgetItem *nextItem = nullptr;
                for (int i = 0; i < currentItem->childCount(); ++i)
                {
                    if (currentItem->child(i)->text(0) == segment)
                    {
                        nextItem = currentItem->child(i);
                        break;
                    }
                }

                if (nextItem == nullptr)
                {
                    currentItem = nullptr;
                    break;
                }
                currentItem = nextItem;
            }
        }

        if (currentItem != nullptr)
        {
            registryTree->setCurrentItem(currentItem);
            registryTree->scrollToItem(currentItem);
        }
    }

    registryTree->setUpdatesEnabled(previousUpdatesState);
}

void MainWindow::restorePendingRegistryState()
{
    const QString writablePath = writableConfigPath("user.ini");
    if (writablePath.isEmpty())
    {
        return;
    }

    QSettings settings(writablePath, QSettings::IniFormat);
    const bool shouldRestore = settings.value("Runtime/pending_admin_reload", false).toBool();
    const QString restoreRootName = settings.value("Runtime/restore_root").toString();
    const QString restorePath = settings.value("Runtime/restore_path").toString();

    if (!shouldRestore)
    {
        return;
    }

    settings.remove("Runtime/pending_admin_reload");
    settings.remove("Runtime/restore_root");
    settings.remove("Runtime/restore_path");
    settings.sync();

    reloadRegistryTree(restoreRootName, restorePath);
    appendLogMessage(localizedText(
        "当前权限：管理员",
        "Current privileges: Administrator"));
    appendLogMessage(localizedText(
        "已切换管理员权限并重新加载注册表。",
        "Administrator privileges enabled. Registry tree reloaded."));
}

void MainWindow::toggleAdministratorPrivileges()
{
    const int result = QMessageBox::question(
        this,
        localizedText("切换管理员权限", "Toggle Administrator Privileges"),
        localizedText(
            "将向系统申请管理员权限并重新加载左侧注册表。是否继续？",
            "The application will request administrator privileges and reload the registry tree. Continue?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);

    if (result != QMessageBox::Yes)
    {
        return;
    }

    QString restoreRootName;
    QString restorePath;
    QTreeWidgetItem *currentItem = registryTree == nullptr ? nullptr : registryTree->currentItem();
    const bool hasCurrentItem = currentItem != nullptr;

    if (hasCurrentItem)
    {
        const HKEY rootKey = reinterpret_cast<HKEY>(
            static_cast<quintptr>(currentItem->data(0, RootHandleRole).toULongLong()));
        const QString subKeyPath = currentItem->data(0, RelativePathRole).toString();
        if (!registryKeyHasChildren(rootKey, subKeyPath))
        {
            restoreRootName = registryRootName(rootKey);
            restorePath = subKeyPath;
            if (registryDetailView != nullptr)
            {
                registryDetailView->clear();
            }
            updateRightDisplayMeta(localizedText(
                "正在切换权限并重新加载注册表...",
                "Switching privileges and reloading registry..."));
        }
    }

    if (isRunningAsAdministrator())
    {
        reloadRegistryTree(restoreRootName, restorePath);
        appendLogMessage(localizedText(
            "当前权限：管理员",
            "Current privileges: Administrator"));
        appendLogMessage(localizedText(
            "当前已具备管理员权限，已重新加载注册表。",
            "Already running as administrator. Registry tree reloaded."));
        return;
    }

    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("Runtime/pending_admin_reload", true);
        settings.setValue("Runtime/restore_root", restoreRootName);
        settings.setValue("Runtime/restore_path", restorePath);
        settings.sync();
    }

    const QString programPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const HINSTANCE shellResult = ShellExecuteW(
        reinterpret_cast<HWND>(winId()),
        L"runas",
        reinterpret_cast<LPCWSTR>(programPath.utf16()),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
    {
        if (!writablePath.isEmpty())
        {
            QSettings settings(writablePath, QSettings::IniFormat);
            settings.remove("Runtime/pending_admin_reload");
            settings.remove("Runtime/restore_root");
            settings.remove("Runtime/restore_path");
            settings.sync();
        }

        if (!restoreRootName.isEmpty() && currentItem != nullptr)
        {
            displayRegistryNodeDetails(currentItem);
        }
        appendLogMessage(localizedText(
            "未获取管理员权限，操作已取消。",
            "Administrator privileges were not granted. Operation canceled."));
        return;
    }

    QApplication::quit();
}

void MainWindow::addToMenuBar(ConfigMap config)
{
    // 获取语言设置
    int lang = config["System"]["language"].toInt();
    // 定义多语言名称
    QString fileText = (lang == 0) ? "文件(&F)" : "File(&F)";
    QString editText = (lang == 0) ? "编辑(&E)" : "Edit(&E)";
    QString analysisText = (lang == 0) ? "运行(&R)" : "Run(&R)";
    QString langText = (lang == 0) ? "语言(&L)" : "Language(&L)";
    QString helpText = (lang == 0) ? "帮助(&H)" : "Help(&H)";
    // 文件菜单
    QMenu *fileMenu = mMenuBar->addMenu(fileText);
    QAction *openResultFileAction = fileMenu->addAction((lang == 0) ? "打开结果文件" : "Open Result File");
    QAction *loadConfigFileAction = fileMenu->addAction((lang == 0) ? "加载配置文件" : "Load Config File");
    QAction *setSavePathAction = fileMenu->addAction((lang == 0) ? "设置保存路径" : "Set Save Path");
    saveResultAction = fileMenu->addAction((lang == 0) ? "保存结果" : "Save Result");
    connect(openResultFileAction, &QAction::triggered, this, &MainWindow::openResultFile);
    connect(loadConfigFileAction, &QAction::triggered, this, &MainWindow::loadConfigFile);
    connect(setSavePathAction, &QAction::triggered, this, &MainWindow::setSavePath);
    connect(saveResultAction, &QAction::triggered, this, &MainWindow::saveResult);
    setSaveResultEnabled(false);
    // 编辑菜单
    QMenu *editMenu = mMenuBar->addMenu(editText);
    QAction *searchInRegistryAction = editMenu->addAction((lang == 0) ? "在注册表中搜索" : "Search In Registry");
    QAction *setFilterAction = editMenu->addAction((lang == 0) ? "设置过滤器" : "Set Filter");
    QAction *toggleAdminAction = editMenu->addAction((lang == 0) ? "切换管理员权限" : "Toggle Administrator Privileges");
    connect(searchInRegistryAction, &QAction::triggered, this, &MainWindow::showRegistrySearchDialog);
    connect(setFilterAction, &QAction::triggered, this, &MainWindow::showRegistryFilterDialog);
    connect(toggleAdminAction, &QAction::triggered, this, &MainWindow::toggleAdministratorPrivileges);
    // 分析菜单
    QMenu *analysisMenu = mMenuBar->addMenu(analysisText);
    startAnalysisAction = analysisMenu->addAction((lang == 0) ? "开始分析" : "Start Analysis");
    stopAnalysisAction = analysisMenu->addAction((lang == 0) ? "结束分析" : "End Analysis");
    takeSnapshotAction = analysisMenu->addAction((lang == 0) ? "拍摄快照" : "Take Snapshot");
    QAction *compareSnapshotsAction = analysisMenu->addAction((lang == 0) ? "比较快照" : "Compare Snapshots");
    connect(startAnalysisAction, &QAction::triggered, this, &MainWindow::startRegistryAnalysis);
    connect(stopAnalysisAction, &QAction::triggered, this, &MainWindow::stopRegistryAnalysis);
    connect(takeSnapshotAction, &QAction::triggered, this, &MainWindow::takeSnapshot);
    connect(compareSnapshotsAction, &QAction::triggered, this, &MainWindow::compareSnapshots);
    if (stopAnalysisAction != nullptr)
    {
        stopAnalysisAction->setEnabled(true);
    }
    // 语言菜单
    QMenu *languageMenu = mMenuBar->addMenu(langText);
    QActionGroup *langGroup = new QActionGroup(this);
    QAction *cnAction = languageMenu->addAction("中文");
    QAction *enAction = languageMenu->addAction("English");
    cnAction->setCheckable(true);
    enAction->setCheckable(true);
    langGroup->addAction(cnAction);
    langGroup->addAction(enAction);
    langGroup->setExclusive(true);

    // 根据当前配置勾选
    if (lang == 0)
        cnAction->setChecked(true);
    else
        enAction->setChecked(true);

    connect(cnAction, &QAction::triggered, this, [this]()
            { switchLanguage(0); });
    connect(enAction, &QAction::triggered, this, [this]()
            { switchLanguage(1); });

    // 帮助菜单
    QMenu *helpMenu = mMenuBar->addMenu(helpText);
    helpMenu->addAction((lang == 0) ? "关于 RegScope" : "About RegScope");
}

void MainWindow::switchLanguage(int lang)
{
    if (uiLanguage == lang)
        return;

    uiLanguage = lang;

    // 保存到 user.ini
    const QString writablePath = writableConfigPath("user.ini");
    if (!writablePath.isEmpty())
    {
        QSettings settings(writablePath, QSettings::IniFormat);
        settings.setValue("System/language", lang);
        settings.sync();
    }

    appendLogMessage(localizedText("语言已切换，请重启以应用完整更改。", "Language switched, please restart to apply full changes."));
}
