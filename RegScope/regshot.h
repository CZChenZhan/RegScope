#ifndef REGSHOT_H
#define REGSHOT_H
#include <QList>
#include <QDataStream>
#include <QFile>
#include <QString>
#include <functional>
#include <windows.h>
#include <tchar.h>
#include <lmcons.h>
#include <QRegularExpression>

// 计数结构体
struct _COUNTS
{
    DWORD cAll;
    DWORD cKeys;
    DWORD cValues;
};
typedef _COUNTS Counts, *LPCounts;

// 注册表值结构体
struct _VALUE
{
    LPTSTR lpValueName;            // 指向注册表值名称
    DWORD dwValueNameLen;          // 注册表值名称缓冲区大小
    DWORD dwTypeCode;              // 注册表值类型，REG_DWORD、REG_SZ..
    LPBYTE lpValueData;            // 注册表值数据
    DWORD dwDataSize;              // 注册表值数据大小
    struct _KEY *lpFatherKey;      // 指向注册表值的父键
    struct _VALUE *lpBrotherValue; // 指向兄弟结点注册表值
    DWORD fValueMatch;             // 该注册表值是否匹配
};
typedef struct _VALUE ValueContent, *LPValueContent;

// 注册表键结构体
struct _KEY
{
    LPTSTR lpKeyName;            // 指向注册表键名称
    DWORD dwKeyNameLen;          // 注册表键名称缓冲区大小
    LPValueContent lpFirstValue; // 指向该注册表键的第一个值
    struct _KEY *lpFirstSubKey;  // 指向第一个子键
    struct _KEY *lpBrotherKey;   // 指向兄弟结点注册表键
    struct _KEY *lpFatherKey;    // 指向父结点注册表键
    DWORD fKeyMatch;             // 该注册表键是否匹配
};
typedef struct _KEY KeyContent, *LPKeyContent;

// 注册表快照结构体
struct _REGSHOT
{
    LPKeyContent lpHKCR;    // 指向HKEY_CLASSES_ROOT
    LPKeyContent lpHKCU;    // 指向HKEY_CURRENT_USER
    LPKeyContent lpHKLM;    // 指向HKEY_LOCAL_MACHINE
    LPKeyContent lpHKU;     // 指向HKEY_USERS
    LPKeyContent lpHKCC;    // 指向HKEY_CURRENT_CONFIG
    bool fFinished;         // 是否拍摄快照完成
    bool fLoaded;           // 是否加载完成
    LPTSTR lpShotName;      // 指向快照名称
    LPTSTR lpComputerName;  // 计算机名称
    LPTSTR lpUserName;      // 用户名称
    quint32 dwComputerName; // 计算机名称长度
    quint32 dwUserName;     // 用户名称长度
    DWORD i;                // 快照编号
    Counts SumCounts;       // 遍历的总数
    SYSTEMTIME SystemTime;  // 快照保存时间
};
typedef struct _REGSHOT RegShotContent, *LPRegShotContent;

#pragma pack(push, 1)
// 保存值结构体
struct _SAVEVALUE
{
    quint64 ofsValueName;    // 值名称的偏移量
    quint64 ofsValueData;    // 值数据的偏移量
    quint64 ofsBrotherValue; // 兄弟结点注册表值的偏移量
    quint64 ofsFatherKey;    // 注册表值的父键的偏移量
    quint32 dwTypeCode;      // 值类型
    quint32 dwDataSize;      // 值数据的大小
    quint32 dwValueNameLen;  // 值名称的长度
};
typedef struct _SAVEVALUE SaveValueContent, *LPSaveValueContent;

// 保存键结构体
struct _SAVEKEY
{
    quint64 ofsKeyName;     // 键名称的偏移量
    quint64 ofsFirstValue;  // 第一个值的偏移量
    quint64 ofsFirstSubKey; // 第一个子键的偏移量
    quint64 ofsBrotherKey;  // 兄弟结点注册表键的偏移量
    quint64 ofsFatherKey;   // 父结点注册表键的偏移量
    quint32 dwKeyNameLen;   // 键名称的长度
};
typedef struct _SAVEKEY SaveKeyContent, *LPSaveKeyContent;
#pragma pack(pop)

// 文件头部信息结构体
struct _FILEHEADER
{
    char signature[8];         // 魔数
    quint64 ofsHKCR;           // HKCR数据位置
    quint64 ofsHKCU;           // HKCU数据位置
    quint64 ofsHKLM;           // HKLM数据位置
    quint64 ofsHKU;            // HKU数据位置
    quint64 ofsHKCC;           // HKCC数据位置
    quint32 dwEndianness;      // 字节序
    SYSTEMTIME SystemTime;     // 快照保存时间
    quint64 ofsComputerName;   // 计算机名称偏移量
    quint32 dwComputerNameLen; // 计算机名称长度
    quint64 ofsUserName;       // 用户名称偏移量
    quint32 dwUserNameLen;     // 用户名称长度
};
typedef _FILEHEADER FileHeader, *LPFileheader;

// 注册表排除项结构体
struct _REGSKIPITEM
{
    LPTSTR FullPath; // 排除项的完整路径
    bool Active;     // 是否启用排除项，默认为1
};
typedef _REGSKIPITEM RegSkipItem;

// 比较结果结构体
struct _COMPARES
{
    LPVOID lpOld;             // 旧的内容
    LPVOID lpNew;             // 新的内容
    struct _COMPARES *lpNext; // 指向下一个比较结果
};
typedef _COMPARES Compares, *LPCompares;

// 比较结果指针结构体
struct _COMPARERESULTSPOINTERS
{
    LPCompares lpKeyDeleted;    // 删除的注册表键
    LPCompares lpKeyAdded;      // 新增的注册表键
    LPCompares lpValueDeleted;  // 删除的注册表值
    LPCompares lpValueAdded;    // 新增的注册表值
    LPCompares lpValueModified; // 修改的注册表值
};
typedef _COMPARERESULTSPOINTERS CompareResultsPointers, *LPCompareResultsPointers;

// 统计比较结果结构体
struct _COMPARERESULTS
{
    LPRegShotContent lpShot1;         // 基准快照
    LPRegShotContent lpShot2;         // 比较快照
    Counts dwCompared;                // 比较的注册表项数量
    Counts dwDeleted;                 // 删除的注册表项数量
    Counts dwAdded;                   // 新增的注册表项数量
    Counts dwModified;                // 修改的注册表项数量
    CompareResultsPointers CRHeads;   // 链表头指针集合
    CompareResultsPointers CRCurrent; // 当前指针集合
    bool fFinished;                   // 是否比较完成
};
typedef _COMPARERESULTS CompareResults, *LPCompareResults;

extern LPTSTR lpHKCR;
extern LPTSTR lpHKCU;
extern LPTSTR lpHKLM;
extern LPTSTR lpHKU;
extern LPTSTR lpHKCC;
extern QString IniPath;
extern QString FilterIniPath;
extern LPTSTR lpIniSkipRegKey;
extern LPTSTR lpSystem;

class RegShot
{
public:
    RegShot();
    ~RegShot();
    void Shot(LPRegShotContent lpShot);
    void Shot(LPRegShotContent lpShot, const std::function<void(int, const QString &)> &progressCallback);
    void SaveShot(LPRegShotContent lpShot, const QString &path);
    bool LoadShot(LPRegShotContent lpShot, const QString &path);
    void FreeShot(LPRegShotContent lpShot);
    DWORD TakeSnapShot();
    DWORD TakeSnapShot(const std::function<void(int, const QString &)> &progressCallback);
    void SaveSnapShot(DWORD index, const QString &path);
    void CompareShots(LPRegShotContent lpShot1, LPRegShotContent lpShot2);
    bool InitializeAnalysisFilter(int dangerLevel, const QString &configFilePath);
    bool ShouldTraceRegistryEvent(const QString &registryPath, int dangerLevel);
    void ClearShots()
    {
        for (int i = 0; i < Shots.size(); ++i)
        {
            FreeShot(&Shots[i]);
        }
        Shots.clear();
    }
    QList<RegShotContent> Shots;     // 拍摄的快照链表
    QList<RegShotContent> SaveShots; // 保存的快照链表
    CompareResults CompareResult;    // 比较结果

private:
    LPTSTR lpStringBuffer;                        // 字符串缓冲区指针，用于接收键或值的名称
    LPBYTE lpDataBuffer;                          // 数据缓冲区指针，用于接收值的数据
    DWORD dwSourceSize;                           // 资源大小
    DWORD dwStringBufferSize;                     // 字符串缓冲区大小
    DWORD dwDataBufferSize;                       // 数据缓冲区大小
    SaveKeyContent SaveKC;                        // 键保存结果
    SaveValueContent SaveVC;                      // 值保存结果
    LPTSTR lpOutputPath;                          // 输出路径
    LPTSTR lpLastSaveDir;                         // 上次保存的目录
    LPTSTR lpShotName;                            // 默认快照名称
    FileHeader fileheader;                        // 文件头部
    std::unordered_map<QString, int> RegSkipList; // 注册表排除项链表与危险等级

    // 通配符排除项树节点结构
    struct WildcardTreeNode
    {
        QString keyName;                                          // 当前节点的键名
        bool isWildcard;                                          // 是否是通配符节点（*）
        bool isActive;                                            // 是否启用排除
        int dangerLevel;                                          // 危险等级（1-5，越高越危险）
        std::unordered_map<QString, WildcardTreeNode *> children; // 子节点
    };
    WildcardTreeNode *WildcardSkipTreeRoot; // 通配符排除项树根节点

    // 加载配置文件
    QMap<QString, QString> ParseIniSection(const QString &filePath, LPTSTR sectionName);
    void FillSkipList(const QMap<QString, QString> SkipRegKeyData);
    void FillWildcardSkipList(const QMap<QString, QString> WildcardSkipRegKeyData);
    void LoadSystemData(const QMap<QString, QString> SystemData);
    bool LoadSettingsFromIni();
    void LoadWildcardExclusions();
    void LoadWildcardExclusions(const QString &filePath);
    void ResetFilterState();
    QString resolveAnalysisConfigPath(int dangerLevel, const QString &requestedConfigPath) const;
    QString normalizeRegistryPathForMatching(const QString &path) const;
    int SkipListDangerLevel(const QString &keyPath) const;
    // 拍摄快照辅助函数
    bool IsInSkipList(LPCTSTR lpKeyName);
    bool IsPathDangerLevel(LPCTSTR lpKeyName, int dangerLevel);
    // 通配符树辅助函数
    void FreeWildcardTree(WildcardTreeNode *node);
    int CheckWildcardTreeMatch(LPCTSTR lpKeyName);
    int CheckWildcardTreeRecursive(WildcardTreeNode *node, const QStringList &parts, int index);
    LPKeyContent GetRegistrySnap(LPRegShotContent lpShot, HKEY hRegKey, LPTSTR lpRegKeyName, LPKeyContent lpFatherKey, LPKeyContent *lpCaller, LPCTSTR lpParentPath = NULL);
    LPTSTR GetWholeKeyName(LPKeyContent lpCurrentKey);
    LPTSTR GetWholeValueName(LPValueContent lpCurrentValue);
    template <typename T>
    DWORD AdjustBuffer(T *&lpBuffer, DWORD dwCurrentSize, DWORD dwRequiredSize, DWORD dwBlockSize);
    // 保存快照辅助函数
    void SaveRegKeys(QDataStream &out, LPRegShotContent lpShot, LPKeyContent lpCurrentKey, quint64 ofsFatherKey, quint64 ofsCaller);
    void WriteFileBuffer(QDataStream &out, qint64 ofsFile, const void *lpData, DWORD dwDataSize);
    // 加载快照辅助函数
    void LoadRegKeys(QDataStream &in, LPRegShotContent lpShot, quint64 ofsKey, LPKeyContent lpFatherKey, LPKeyContent *lpCaller);
    // 比较快照辅助函数
    void CreateNewResult(DWORD ActionType, LPVOID lpOld, LPVOID lpNew);
    void CompareRegKeys(LPKeyContent lpStartKey1, LPKeyContent lpStartKey2, const std::wstring& parentPath);
    void ClearRegKeyMatchFlags(LPKeyContent lpCurrentKey);
    // 释放快照辅助函数
    void FreeAllKeyContents(LPKeyContent lpCurrentKey);
    void FreeAllValueContents(LPValueContent lpCurrentValue);
    void FreeAllCompare(LPCompares lpCR);
    void FreeCompareResult();
};

#define REGSHOT_BUFFER_BLOCKSIZE 1024
#define MAX_SIGNATURE_LENGTH 8
#define RegScopeFileSignature "REGSCOPE"
#define MAX_INI_SECTION_CHARS 32768

#define NOMATCH 0
#define ISMATCH 1
#define ISDEL 2
#define ISADD 3
#define ISMODI 4

#define KEYDEL 1
#define KEYADD 2
#define VALDEL 3
#define VALADD 4
#define VALMODI 5

#endif // REGSHOT_H
