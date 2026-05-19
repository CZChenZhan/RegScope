#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QList>
#include <QMap>
#include <QDateTime>
#include <QMenuBar>
#include <QSplitter>
#include <QFrame>
#include <QLabel>
#include <QTextCursor>
#include <QVector>
#include <windows.h>
#include "regshot.h"
#include "registrymonitor.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
    class MainWindow;
}
QT_END_NAMESPACE

class QPlainTextEdit;
class QDialog;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QAction;
class QTreeWidget;
class QTreeWidgetItem;

// 为 .ini 配置文件的存储类型新命名
using ConfigMap = QMap<QString, QMap<QString, QString>>;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    Ui::MainWindow *ui;

    QList<QWidget *> allWidgets; // 私有成员列表，硬编码

    QMenuBar *mMenuBar; // 菜单栏

    QSplitter *mainSplitter; // 横分割线
    QSplitter *topSplitter;  // 竖分割线

    QFrame *leftPanel;                  // 左上区，展示注册表结构
    QFrame *rightDisplay;               // 右上区，展示执行结果
    QFrame *bottomLog;                  // 底部区，展示日志输出
    QTreeWidget *registryTree;          // 注册表结构树
    QPlainTextEdit *registryDetailView; // 右上区详情展示
    QPlainTextEdit *bottomLogView;      // 底部日志输出视图
    QDialog *registrySearchDialog;      // 注册表搜索浮窗
    QLineEdit *registrySearchInput;     // 注册表搜索输入框
    QPushButton *registrySearchFindButton;
    QPushButton *registrySearchNextButton;
    QPushButton *registrySearchPreviousButton;
    QDialog *registryFilterDialog;  // 注册表过滤浮窗
    QLineEdit *registryFilterInput; // 注册表过滤输入框
    QPushButton *registryFilterApplyButton;
    QLabel *leftPanelTitle;            // 左上区标题
    QLabel *leftPanelMeta;             // 左上区状态信息
    QLabel *rightDisplayTitle;         // 右上区标题
    QLabel *rightDisplayMeta;          // 右上区状态信息
    QLabel *bottomLogTitle;            // 底部日志区标题
    QProgressBar *snapshotProgressBar; // 快照进度条
    QLabel *snapshotProgressLabel;     // 快照进度标签
    QAction *saveResultAction;         // 保存结果菜单项
    QAction *startAnalysisAction;      // 开始分析菜单项
    QAction *stopAnalysisAction;       // 结束分析菜单项
    QAction *takeSnapshotAction;       // 拍摄快照菜单项
    int uiLanguage;                    // 界面语言，0-中文，1-英文
    QList<QTextCursor> registrySearchMatches;
    QString registrySearchKeyword;
    QString currentRegistryFilterRule;
    int currentRegistrySearchIndex;
    RegShotContent m_firstCompareSnapshot;
    RegShotContent m_secondCompareSnapshot;
    int m_compareSnapshotState; // 0: 未开始, 1: 已完成第一次, 2: 已完成第二次
    RegistryMonitor *m_registryMonitor;
    QVector<RegistryEtwEvent> m_analysisEvents;
    QDateTime m_analysisStartTime;
    QString m_analysisConfigPath;
    int m_analysisRiskLevel;
    bool m_analysisRunning;
    int m_saveResultContentType; // 0: 无, 1: 快照比较结果, 2: 分析结果
    QStringList m_analysisSummaryHeaderLines;
    QStringList m_analysisDetailBlocks;
    int m_analysisRenderedBlockCount;
    int m_analysisRecordedEventCount;
    bool m_analysisLazyLoading;
    // 初始化界面分区，私有成员列表赋值
    void initLayout();
    // 私有成员控件样式自识别加载
    bool setStyles();
    // 为单个控件加载对应样式
    bool applyWidgetStyle(QWidget *widget);
    // 读取 .ini 配置文件
    ConfigMap readIni(const QString &filePath);
    // 获取合并后的应用配置 (default.ini + user.ini)
    ConfigMap getAppConfig();
    // 确保用户配置文件存在
    void ensureUserConfigExists();
    // 获取配置文件路径
    QString resolveConfigPath(const QString &fileName) const;
    // 获取可写配置文件路径
    QString writableConfigPath(const QString &fileName) const;
    // 展开配置中的环境变量路径
    QString expandConfigPath(const QString &path) const;
    // 向菜单栏添加交互选项
    void addToMenuBar(ConfigMap config);
    // 打开结果文件
    void openResultFile();
    // 加载配置文件
    void loadConfigFile();
    // 保存右上区结果为 txt
    void saveResult();
    // 设置保存路径
    void setSavePath();
    // 拍摄快照并保存为 RGS
    void takeSnapshot();
    // 开始实时分析
    void startRegistryAnalysis();
    // 结束实时分析
    void stopRegistryAnalysis();
    // 比较两个快照文件
    void compareSnapshots();
    // 打开注册表搜索浮窗
    void showRegistrySearchDialog();
    // 执行注册表搜索
    void performRegistrySearch();
    // 跳转到下一个或上一个搜索结果
    void navigateRegistrySearch(int step);
    // 打开注册表过滤浮窗
    void showRegistryFilterDialog();
    // 应用注册表过滤规则
    void applyRegistryFilter();
    // 切换管理员权限
    void toggleAdministratorPrivileges();
    // 实现左上区，展示注册表结构树
    void leftPanelReady();
    // 实现右上区，展示注册表键值内容
    void rightDisplayReady();
    // 实现底部日志输出区
    void bottomLogReady();
    // 添加根注册表项
    void appendRegistryRoot(const QString &rootName, HKEY rootKey);
    // 懒加载展开节点的子项
    void populateRegistryChildren(QTreeWidgetItem *parentItem);
    // 判断注册表项是否包含子键
    bool registryKeyHasChildren(HKEY rootKey, const QString &subKeyPath) const;
    // 展示当前选中节点及其子节点的键值
    void displayRegistryNodeDetails(QTreeWidgetItem *currentItem);
    // 递归收集当前键及其子键的键值文本
    void appendRegistryKeyDump(HKEY rootKey, const QString &subKeyPath, QStringList &lines) const;
    // 格式化注册表值
    QString formatRegistryValue(DWORD valueType, const QByteArray &valueData) const;
    // 切换语言
    void switchLanguage(int lang);
    // 获取根键显示名称
    QString registryRootName(HKEY rootKey) const;
    // 构造键路径
    QString buildKeyPath(LPKeyContent key) const;
    // 构造值路径
    QString buildValuePath(LPValueContent value) const;
    // 更新左上区状态信息
    void updateLeftPanelMeta(const QString &summary);
    // 更新右上区状态信息
    void updateRightDisplayMeta(const QString &summary);
    // 刷新右上区搜索高亮
    void refreshRegistrySearchHighlights(bool logResult);
    // 定位到当前搜索结果
    void focusRegistrySearchResult(int index);
    // 清空右上区搜索高亮
    void clearRegistrySearchHighlights();
    // 递归应用左侧树过滤规则
    bool applyRegistryFilterToItem(QTreeWidgetItem *item, const QString &filterRule);
    // 当前进程是否具备管理员权限
    bool isRunningAsAdministrator() const;
    // 重新加载左侧注册表树
    void reloadRegistryTree(const QString &restoreRootName = QString(), const QString &restorePath = QString());
    // 恢复待处理的注册表显示状态
    void restorePendingRegistryState();
    // 向底部日志区追加日志
    void appendLogMessage(const QString &message);
    // 更新“保存结果”菜单项状态
    void setSaveResultEnabled(bool enabled);
    // 根据当前语言返回界面文案
    QString localizedText(const QString &zhText, const QString &enText) const;
    // 根据当前语言返回分析动作文案
    QString localizedAnalysisAction(const QString &actionKey) const;
    // 开始拍摄用于比较的内存快照
    void captureComparisonSnapshot(bool isFirstShot);
    // 比较内存中的两次快照
    void compareInMemorySnapshots();
    // 展示比较结果
    void renderComparisonResult(RegShot &regShot,
                                LPRegShotContent shot1,
                                LPRegShotContent shot2,
                                const QString &sourceOne,
                                const QString &sourceTwo);
    // 处理实时分析事件
    void handleRegistryAnalysisEvent(const RegistryEtwEvent &eventInfo);
    // 展示实时分析结果摘要
    void renderRegistryAnalysisSummary();
    // 重置实时分析结果的懒加载缓存
    void resetAnalysisSummaryCache();
    // 按批次向右上区追加实时分析明细
    void appendNextAnalysisResultChunk();
    // 构造单条实时分析事件的展示文本
    QString formatAnalysisEventBlock(const RegistryEtwEvent &eventInfo) const;
    // 构造完整实时分析结果文本
    QString buildFullAnalysisSummaryText() const;
    // 清理比较流程中的内存快照
    void resetInMemoryComparisonShots();
};
#endif // MAINWINDOW_H
