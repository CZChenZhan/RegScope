#include "regshot.h"
#include <qdatetime.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace
{
    int clampDangerLevel(int dangerLevel)
    {
        if (dangerLevel < 1)
        {
            return 1;
        }

        if (dangerLevel > 5)
        {
            return 5;
        }

        return dangerLevel;
    }

    QString builtInLevelConfigPath(int dangerLevel)
    {
        return QString(":/admin/configs/level%1.ini").arg(clampDangerLevel(dangerLevel));
    }

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

    QString resolveConfigPath(const QString &fileName)
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

    QString resolveSelectedConfigPath()
    {
        const QString controllerConfigPath = resolveConfigPath("default.ini");
        QSettings settings(controllerConfigPath, QSettings::IniFormat);
        const QString configuredPath = QDir::fromNativeSeparators(
            settings.value("System/active_config_file").toString());

        if (!configuredPath.isEmpty() && QFileInfo::exists(configuredPath))
        {
            return QFileInfo(configuredPath).absoluteFilePath();
        }

        return controllerConfigPath;
    }

    QString resolveFilterConfigPath(const QString &activeConfigPath)
    {
        const QFileInfo activeConfigInfo(activeConfigPath);
        if (activeConfigInfo.exists() && activeConfigInfo.isFile())
        {
            const QString siblingFilterPath =
                QDir(activeConfigInfo.absolutePath()).filePath("filter.ini");
            if (QFileInfo::exists(siblingFilterPath))
            {
                return QFileInfo(siblingFilterPath).absoluteFilePath();
            }
        }

        return resolveConfigPath("filter.ini");
    }
}

LPTSTR lpHKCR = TEXT("HKEY_CLASSES_ROOT");
LPTSTR lpHKCU = TEXT("HKEY_CURRENT_USER");
LPTSTR lpHKLM = TEXT("HKEY_LOCAL_MACHINE");
LPTSTR lpHKU = TEXT("HKEY_USERS");
LPTSTR lpHKCC = TEXT("HKEY_CURRENT_CONFIG");
QString IniPath = ":/admin/configs/default.ini";
QString FilterIniPath = ":/admin/configs/filter.ini";
LPTSTR lpIniSkipRegKey = TEXT("SkipRegKey");
LPTSTR lpSystem = TEXT("System");

RegShot::RegShot()
{
    lpStringBuffer = NULL;
    lpDataBuffer = NULL;
    dwSourceSize = 0;
    dwStringBufferSize = 0;
    dwDataBufferSize = 0;
    ZeroMemory(&CompareResult, sizeof(CompareResult));
    // 初始化通配符排除项树根节点
    WildcardSkipTreeRoot = new WildcardTreeNode();
    WildcardSkipTreeRoot->keyName = "";
    WildcardSkipTreeRoot->isWildcard = false;
    WildcardSkipTreeRoot->isActive = false;
    WildcardSkipTreeRoot->dangerLevel = 0;
}

RegShot::~RegShot()
{
    for (int i = 0; i < Shots.size(); i++)
    {
        LPRegShotContent shot = &Shots[i];
        FreeShot(shot);
    }
    FreeCompareResult();
    // 释放通配符排除项树
    FreeWildcardTree(WildcardSkipTreeRoot);
}

// 解析配置文件
QMap<QString, QString> RegShot::ParseIniSection(const QString &filePath, LPTSTR sectionName)
{
    QMap<QString, QString> result;
    // 打开配置文件
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return result;
    }
    // 逐行解析
    QString targetSection = QString::fromWCharArray(sectionName);
    QTextStream stream(&file);
    bool inTargetSection = false;
    while (!stream.atEnd())
    {
        QString line = stream.readLine().trimmed();
        // 检查是否进入目标节
        if (!inTargetSection && line.startsWith("["))
        {
            inTargetSection = (line.compare("[" + targetSection + "]", Qt::CaseInsensitive) == 0);
            continue;
        }
        // 读取完成直接退出
        if (inTargetSection && line.startsWith("["))
        {
            break;
        }
        // 解析目标节中的键值对
        if (inTargetSection && !line.isEmpty() && !line.startsWith(";"))
        {
            int pos = line.indexOf("=");
            if (pos > 0)
            {
                QString key = line.left(pos);
                QString value = line.mid(pos + 1);
                result[key] = value;
            }
        }
    }

    return result;
}
// 填充排除项链表
void RegShot::FillSkipList(const QMap<QString, QString> SkipRegKeyData)
{
    RegSkipList.clear();
    // 预分配空间，提升性能
    RegSkipList.reserve(SkipRegKeyData.size());  
    
    for (auto it = SkipRegKeyData.begin(); it != SkipRegKeyData.end(); it++)
    {
        int configuredDangerLevel = it.value().toInt();
        if (configuredDangerLevel < 0)
        {
            configuredDangerLevel = 0;
        }
        if (configuredDangerLevel > 5)
        {
            configuredDangerLevel = 5;
        }
        const QString normalizedPath = normalizeRegistryPathForMatching(it.key());
        if (!normalizedPath.isEmpty())
        {
            RegSkipList[normalizedPath] = configuredDangerLevel;
        }
    }
}

// 填充通配符排除项链表（构建树状结构）
void RegShot::FillWildcardSkipList(const QMap<QString, QString> WildcardSkipRegKeyData)
{
    for (auto it = WildcardSkipRegKeyData.begin(); it != WildcardSkipRegKeyData.end(); it++)
    {
        const QString &value = it.value();
        const QString pattern = normalizeRegistryPathForMatching(it.key());
        if (pattern.isEmpty())
        {
            continue;
        }
        
        // 解析值：危险等级（1-5，越高越危险）
        // 值为 "0" 表示不启用，"1-5" 表示启用且对应危险等级
        int dangerLevel = value.toInt();
        
        // 限制危险等级在1-5范围内
        if (dangerLevel < 0) dangerLevel = 0;
        if (dangerLevel > 5) dangerLevel = 5;
        
        // 如果危险等级为0，表示不启用排除
        if (dangerLevel == 0)
        {
            continue;
        }
        
        // 将通配符路径插入到树中
        QStringList pathParts = pattern.split('\\', Qt::SkipEmptyParts);
        WildcardTreeNode *current = WildcardSkipTreeRoot;
        
        for (const QString &part : pathParts)
        {
            bool isWildcard = (part == "*");
            QString keyName = isWildcard ? "" : part;
            
            // 查找或创建子节点
            WildcardTreeNode *child = nullptr;
            auto itChild = current->children.find(keyName);
            if (itChild != current->children.end())
            {
                child = itChild->second;
            }
            else
            {
                child = new WildcardTreeNode();
                child->keyName = keyName;
                child->isWildcard = isWildcard;
                child->isActive = false;
                child->dangerLevel = 0;
                current->children[keyName] = child;
            }
            
            current = child;
        }
        
        // 标记最后一个节点为活跃状态和危险等级
        current->isActive = true;
        current->dangerLevel = dangerLevel;
    }
}

// 释放通配符树
void RegShot::FreeWildcardTree(WildcardTreeNode *node)
{
    if (node == nullptr)
    {
        return;
    }
    
    for (auto &pair : node->children)
    {
        FreeWildcardTree(pair.second);
    }
    
    delete node;
}
// 加载系统默认数据
void RegShot::LoadSystemData(const QMap<QString, QString> SystemData)
{
    for (auto it = SystemData.begin(); it != SystemData.end(); it++)
    {
        // 默认快照名称
        if (it.key().compare("title", Qt::CaseInsensitive) == 0)
        {
            lpShotName = new TCHAR[it.value().length() + 1];
            _tcscpy(lpShotName, it.value().toStdWString().c_str());
        }
        // 默认快照保存目录
        if (it.key().compare("basedir", Qt::CaseInsensitive) == 0)
        {
            lpOutputPath = new TCHAR[it.value().length() + 1];
            _tcscpy(lpOutputPath, it.value().toStdWString().c_str());
        }
    }
}
// 加载配置文件
bool RegShot::LoadSettingsFromIni()
{
    IniPath = resolveSelectedConfigPath();
    FilterIniPath = resolveFilterConfigPath(IniPath);
    ResetFilterState();
    // 加载系统默认数据
    LoadSystemData(ParseIniSection(IniPath, lpSystem));
    // 加载排除项
    FillSkipList(ParseIniSection(FilterIniPath, lpIniSkipRegKey));
    // 加载通配符排除项（仿照user.ini读取方式）
    LoadWildcardExclusions();

    return true;
}

// 加载通配符排除项（仿照user.ini读取方式）
void RegShot::LoadWildcardExclusions()
{
    // 查找可写的配置目录
    QString configDirectory = findConfigDirectory();
    if (configDirectory.isEmpty())
    {
        return;
    }
    
    // 用户自定义通配符排除项文件路径
    QString userWildcardPath = QDir(configDirectory).filePath("wildcard.ini");
    
    // 如果文件不存在，尝试从资源加载默认配置
    if (!QFileInfo::exists(userWildcardPath))
    {
        userWildcardPath = QString(":/admin/configs/wildcard.ini");
    }
    
    // 使用QSettings读取配置（仿照mainwindow.cpp的readIni方式）
    QSettings settings(userWildcardPath, QSettings::IniFormat);
    
    // 获取[SkipRegKey]节下的所有键
    settings.beginGroup("SkipRegKey");
    QStringList keys = settings.childKeys();
    
    QMap<QString, QString> wildcardData;
    for (int i = 0; i < keys.size(); ++i)
    {
        const QString &key = keys[i];
        QString value = settings.value(key, "0").toString();
        wildcardData[key] = value;
    }
    
    settings.endGroup();
    
    // 填充通配符树
    FillWildcardSkipList(wildcardData);
}

void RegShot::LoadWildcardExclusions(const QString &filePath)
{
    if (filePath.isEmpty() || !QFile::exists(filePath))
    {
        return;
    }

    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup("SkipRegKey");

    const QStringList keys = settings.childKeys();
    QMap<QString, QString> wildcardData;
    for (const QString &key : keys)
    {
        wildcardData[key] = settings.value(key, "0").toString();
    }

    settings.endGroup();
    FillWildcardSkipList(wildcardData);
}

void RegShot::ResetFilterState()
{
    RegSkipList.clear();

    if (WildcardSkipTreeRoot != nullptr)
    {
        FreeWildcardTree(WildcardSkipTreeRoot);
    }

    WildcardSkipTreeRoot = new WildcardTreeNode();
    WildcardSkipTreeRoot->keyName = "";
    WildcardSkipTreeRoot->isWildcard = false;
    WildcardSkipTreeRoot->isActive = false;
    WildcardSkipTreeRoot->dangerLevel = 0;
}

QString RegShot::resolveAnalysisConfigPath(int dangerLevel, const QString &requestedConfigPath) const
{
    const QString normalizedPath = QDir::fromNativeSeparators(requestedConfigPath.trimmed());
    if (!normalizedPath.isEmpty() && QFile::exists(normalizedPath))
    {
        return normalizedPath;
    }

    return builtInLevelConfigPath(dangerLevel);
}

QString RegShot::normalizeRegistryPathForMatching(const QString &path) const
{
    QString normalized = path.trimmed();
    if (normalized.isEmpty())
    {
        return {};
    }

    normalized.replace('/', '\\');

    auto replaceRoot = [&normalized](const QString &shortRoot, const QString &fullRoot)
    {
        if (normalized.compare(shortRoot, Qt::CaseInsensitive) == 0)
        {
            normalized = fullRoot;
            return;
        }

        const QString shortPrefix = shortRoot + "\\";
        if (normalized.startsWith(shortPrefix, Qt::CaseInsensitive))
        {
            normalized = fullRoot + normalized.mid(shortRoot.size());
        }
    };

    replaceRoot("HKCR", "HKEY_CLASSES_ROOT");
    replaceRoot("HKCU", "HKEY_CURRENT_USER");
    replaceRoot("HKLM", "HKEY_LOCAL_MACHINE");
    replaceRoot("HKU", "HKEY_USERS");
    replaceRoot("HKCC", "HKEY_CURRENT_CONFIG");

    return normalized.toLower();
}

int RegShot::SkipListDangerLevel(const QString &keyPath) const
{
    const auto it = RegSkipList.find(normalizeRegistryPathForMatching(keyPath));
    if (it == RegSkipList.end())
    {
        return 0;
    }

    return it->second;
}

// 检查通配符树是否匹配（树状剪枝算法），返回危险等级（0表示不匹配）
int RegShot::CheckWildcardTreeMatch(LPCTSTR lpKeyName)
{
    QString keyPath = normalizeRegistryPathForMatching(QString::fromWCharArray(lpKeyName));
    
    // 如果通配符树为空，直接返回0（不匹配）
    if (WildcardSkipTreeRoot->children.empty())
    {
        return 0;
    }
    
    // 将路径分割成各个部分
    QStringList parts = keyPath.split('\\');
    
    // 递归检查路径是否匹配通配符树，返回最高危险等级
    return CheckWildcardTreeRecursive(WildcardSkipTreeRoot, parts, 0);
}

// 递归检查通配符树匹配，返回危险等级（0表示不匹配）
int RegShot::CheckWildcardTreeRecursive(WildcardTreeNode *node, const QStringList &parts, int index)
{
    // 如果已经到达路径末尾
    if (index >= parts.size())
    {
        // 检查当前节点是否标记为活跃
        return node->isActive ? node->dangerLevel : 0;
    }
    
    const QString &currentPart = parts[index];
    int maxDangerLevel = 0;
    
    // 精确匹配
    auto exactMatch = node->children.find(currentPart);
    if (exactMatch != node->children.end())
    {
        int level = CheckWildcardTreeRecursive(exactMatch->second, parts, index + 1);
        if (level > maxDangerLevel)
        {
            maxDangerLevel = level;
        }
    }
    
    // 通配符匹配
    auto wildcardMatch = node->children.find("");
    if (wildcardMatch != node->children.end() && wildcardMatch->second->isWildcard)
    {
        // 通配符可以匹配任意层级
        // 检查当前通配符节点是否标记为活跃（剪枝点）
        if (wildcardMatch->second->isActive)
        {
            if (wildcardMatch->second->dangerLevel > maxDangerLevel)
            {
                maxDangerLevel = wildcardMatch->second->dangerLevel;
            }
        }
        
        // 继续递归检查下一层
        int level = CheckWildcardTreeRecursive(wildcardMatch->second, parts, index + 1);
        if (level > maxDangerLevel)
        {
            maxDangerLevel = level;
        }
    }
    
    return maxDangerLevel;
}

// 是否在排除项链表中
bool RegShot::IsInSkipList(LPCTSTR lpKeyName)
{
    QString keyPath = normalizeRegistryPathForMatching(QString::fromWCharArray(lpKeyName));
    return SkipListDangerLevel(keyPath) > 0;
}

// 检查路径是否在指定危险等级或更高等级的排除项中
bool RegShot::IsPathDangerLevel(LPCTSTR lpKeyName, int dangerLevel)
{
    QString keyPath = normalizeRegistryPathForMatching(QString::fromWCharArray(lpKeyName));
    const int targetDangerLevel = clampDangerLevel(dangerLevel);

    const int exactDangerLevel = SkipListDangerLevel(keyPath);
    if (exactDangerLevel >= targetDangerLevel)
    {
        return true;
    }
    
    // 检查通配符匹配的危险等级
    int matchedDangerLevel = CheckWildcardTreeMatch(lpKeyName);
    if (matchedDangerLevel >= targetDangerLevel)
    {
        return true;
    }
    
    // 树状剪枝：检查所有父路径的危险等级
    QString currentPath = keyPath;
    while (!currentPath.isEmpty())
    {
        // 查找最后一个反斜杠的位置
        int lastBackslash = currentPath.lastIndexOf('\\');
        if (lastBackslash == -1)
        {
            // 已经到根键级别
            break;
        }
        
        // 截取父路径
        currentPath = currentPath.left(lastBackslash);
        
        // 检查父路径是否在精确排除列表中
        if (SkipListDangerLevel(currentPath) >= targetDangerLevel)
        {
            return true;
        }
        
        // 检查父路径是否匹配通配符模式且危险等级足够
        int parentDangerLevel = CheckWildcardTreeMatch(currentPath.toStdWString().c_str());
        if (parentDangerLevel >= targetDangerLevel)
        {
            return true;
        }
    }
    
    return false;
}

bool RegShot::InitializeAnalysisFilter(int dangerLevel, const QString &configFilePath)
{
    const QString effectiveConfigPath = resolveAnalysisConfigPath(dangerLevel, configFilePath);

    ResetFilterState();
    FillSkipList(ParseIniSection(effectiveConfigPath, lpIniSkipRegKey));
    LoadWildcardExclusions(effectiveConfigPath);

    return true;
}

bool RegShot::ShouldTraceRegistryEvent(const QString &registryPath, int dangerLevel)
{
    return !IsPathDangerLevel(registryPath.toStdWString().c_str(), dangerLevel);
}
// 获取键完整名称
LPTSTR RegShot::GetWholeKeyName(LPKeyContent lpCurrentKey)
{
    LPKeyContent lpKC;
    LPTSTR lpTail;
    DWORD KeyNameLen;
    LPTSTR lpFullKeyName;
    LPTSTR lpKeyName;
    // 计算完整键名的长度
    KeyNameLen = 0;
    for (lpKC = lpCurrentKey; lpKC != NULL; lpKC = lpKC->lpFatherKey)
    {
        if (lpKC->lpKeyName != NULL)
        {
            KeyNameLen += lpKC->dwKeyNameLen;
        }
        // 预留反斜杠位置
        KeyNameLen++;
    }

    if (KeyNameLen == 0)
    {
        KeyNameLen++;
    }

    lpFullKeyName = new TCHAR[KeyNameLen];
    if (lpFullKeyName)
    {
        lpTail = &lpFullKeyName[KeyNameLen - 1];
        lpTail[0] = (TCHAR)'\0';
    }
    // 填充键名
    for (lpKC = lpCurrentKey; lpKC != NULL; lpKC = lpKC->lpFatherKey)
    {
        if (lpKC->lpKeyName != NULL)
        {
            KeyNameLen = lpKC->dwKeyNameLen;
            lpKeyName = lpKC->lpKeyName;
            // 还没到根键
            if (lpTail - KeyNameLen != NULL)
            {
                _tcsncpy(lpTail -= KeyNameLen, lpKeyName, KeyNameLen);
                // 添加反斜杠
                if (lpTail > lpFullKeyName)
                {
                    *--lpTail = (TCHAR)'\\';
                }
            }
        }
    }
    return lpFullKeyName;
}
// 获取值完整名称
LPTSTR RegShot::GetWholeValueName(LPValueContent lpCurrentValue)
{
    LPTSTR lpFullValueName;
    LPTSTR lpKeyPath;
    DWORD ValueNameLen;
    DWORD FullNameLen;
    DWORD KeyNameLen;
    // 获取值对应键的完整路径
    lpKeyPath = (lpCurrentValue->lpFatherKey != NULL) ? GetWholeKeyName(lpCurrentValue->lpFatherKey) : NULL;
    // 值名称长度
    ValueNameLen = (lpCurrentValue->lpValueName != NULL) ? lpCurrentValue->dwValueNameLen : 0;
    // 计算完整长度
    KeyNameLen = (lpKeyPath != NULL) ? _tcslen(lpKeyPath) : 0;
    FullNameLen = KeyNameLen + 1 + ValueNameLen + 1;
    // 构建值的完整路径
    lpFullValueName = new TCHAR[FullNameLen];
    if (lpFullValueName == NULL)
    {
        delete[] lpKeyPath;
        return NULL;
    }

    lpFullValueName[0] = TEXT('\0');
    // 复制键路径
    if (lpKeyPath != NULL && KeyNameLen > 0)
    {
        _tcscpy(lpFullValueName, lpKeyPath);
        // 添加反斜杠
        if (ValueNameLen > 0)
        {
            _tcscat(lpFullValueName, TEXT("\\"));
        }
    }
    // 添加值名称
    if (ValueNameLen > 0 && lpCurrentValue->lpValueName != NULL)
    {
        _tcscat(lpFullValueName, lpCurrentValue->lpValueName);
    }

    return lpFullValueName;
}
// 调整缓冲区，模板函数
template <typename T>
DWORD RegShot::AdjustBuffer(T *&lpBuffer, DWORD dwCurrentSize, DWORD dwRequiredSize, DWORD dwBlockSize)
{
    // 缓冲区为空，则当前缓冲区大小为0
    if (lpBuffer == NULL)
    {
        dwCurrentSize = 0;
    }
    // 需要扩容
    if (dwRequiredSize > dwCurrentSize)
    {
        if (lpBuffer != NULL)
        {
            delete[] lpBuffer;
            lpBuffer = NULL;
        }
        // 计算大小
        if (dwBlockSize <= 1)
        {
            dwCurrentSize = dwRequiredSize;
        }
        else
        {
            dwCurrentSize = dwRequiredSize / dwBlockSize;
            dwCurrentSize *= dwBlockSize;
            if (dwRequiredSize > dwCurrentSize)
            {
                dwCurrentSize += dwBlockSize;
            }
        }
        // 分配空间
        lpBuffer = new T[dwCurrentSize];
    }

    return dwCurrentSize;
}
// 获取注册表快照
LPKeyContent RegShot::GetRegistrySnap(LPRegShotContent lpShot, HKEY hRegKey, LPTSTR lpRegKeyName, LPKeyContent lpFatherKey, LPKeyContent *lpCaller, LPCTSTR lpParentPath)
{
    LPKeyContent lpKC;
    DWORD SubKeys;
    DWORD MaxSubKeyNameLen;
    long nError;

    // 额外的块遍历键中包含的值
    {
        DWORD ValueCount;
        DWORD MaxValueNameLen;
        DWORD MaxValueDataLen;

        // 初始化
        lpKC = new KeyContent();
        lpKC->lpFatherKey = lpFatherKey;
        lpKC->lpKeyName = lpRegKeyName;
        lpKC->dwKeyNameLen = _tcslen(lpKC->lpKeyName);
        
        // 构建当前键的完整路径
        LPTSTR lpFullKeyName;
        // 根键
        if(lpParentPath == NULL || _tcslen(lpParentPath) == 0)
        {
            lpFullKeyName = new TCHAR[lpKC->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpRegKeyName);
            }
        }
        // 子键情况，在父路径基础上添加当前键名
        else
        {
            DWORD parentLen = _tcslen(lpParentPath);
            lpFullKeyName = new TCHAR[parentLen + 1 + lpKC->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpParentPath);
                _tcscat(lpFullKeyName, TEXT("\\"));
                _tcscat(lpFullKeyName, lpRegKeyName);
            }
        }
        
        // 当前排除项链表不为空
        if(!RegSkipList.empty())
        {
            // 完整键名在排除项链表中，释放当前键
            if((lpFullKeyName != NULL) && (IsInSkipList(lpFullKeyName)))
            {
                delete[] lpFullKeyName;
                FreeAllKeyContents(lpKC);
                return NULL;
            }
            delete[] lpFullKeyName;
        }
        // 检索该注册表键的信息
        nError = RegQueryInfoKeyW(
            hRegKey,
            NULL,
            NULL,
            NULL,
            &SubKeys,
            &MaxSubKeyNameLen,
            NULL,
            &ValueCount,
            &MaxValueNameLen,
            &MaxValueDataLen,
            NULL,
            NULL
            );
        // 失败则释放内存
        if(nError != ERROR_SUCCESS)
        {
            FreeAllKeyContents(lpKC);
            return NULL;
        }
        // 设置双重指针，将当前结点链接到父结点的兄弟链中
        if(lpCaller != NULL)
            *lpCaller = lpKC;
        // 遍历键数量增加
        lpShot->SumCounts.cKeys++;
        // 该键的值存在，遍历
        if(ValueCount > 0)
        {
            LPValueContent lpVC;
            DWORD i;
            DWORD bValueName;
            DWORD bValueData;
            DWORD ValueType;
            LPValueContent lpTail = NULL;

            // 为NULL终止符预留空间
            if(MaxValueNameLen > 0)
            {
                MaxValueNameLen++;
            }
            // 为最长的值名分配缓冲区
            dwSourceSize = MaxValueNameLen * sizeof(TCHAR);
            dwStringBufferSize = AdjustBuffer(lpStringBuffer, dwStringBufferSize, dwSourceSize, REGSHOT_BUFFER_BLOCKSIZE);
            // 为最大的值数据分配缓冲区
            dwDataBufferSize = AdjustBuffer(lpDataBuffer, dwDataBufferSize, MaxValueDataLen, REGSHOT_BUFFER_BLOCKSIZE);
            // 枚举键值
            for(i=0;;i++)
            {
                // 初始赋值为最大长度
                bValueName = dwStringBufferSize;
                bValueData = dwDataBufferSize;
                // 枚举该注册表键的值
                nError = RegEnumValue(
                    hRegKey,
                    i,
                    lpStringBuffer,
                    &bValueName,
                    NULL,
                    &ValueType,
                    lpDataBuffer,
                    &bValueData
                    );
                if(nError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }
                if(nError != ERROR_SUCCESS)
                {
                    continue;
                }
                // 结尾添加\0防止溢出
                lpStringBuffer[bValueName] = (TCHAR)'\0';

                lpVC = new ValueContent();
                lpVC->lpFatherKey = lpKC;
                // 值的名称
                if(bValueName > 0)
                {
                    lpVC->lpValueName = new TCHAR[bValueName+1];
                    _tcscpy(lpVC->lpValueName, lpStringBuffer);
                    lpVC->dwValueNameLen = _tcslen(lpVC->lpValueName);
                }
                // 构建当前值的完整路径
                LPTSTR lpFullValueName = NULL;
                // 根键
                if(lpParentPath == NULL || _tcslen(lpParentPath) == 0)
                {
                    DWORD valueNameLen = (lpVC->lpValueName != NULL) ? _tcslen(lpVC->lpValueName) : 0;
                    lpFullValueName = new TCHAR[lpKC->dwKeyNameLen + 1 + valueNameLen + 1];
                    if(lpFullValueName)
                    {
                        _tcscpy(lpFullValueName, lpRegKeyName);
                        if(valueNameLen > 0)
                        {
                            _tcscat(lpFullValueName, TEXT("\\"));
                            _tcscat(lpFullValueName, lpVC->lpValueName);
                        }
                    }
                }
                // 子键
                else
                {
                    DWORD parentLen = _tcslen(lpParentPath);
                    DWORD valueNameLen = (lpVC->lpValueName != NULL) ? _tcslen(lpVC->lpValueName) : 0;
                    lpFullValueName = new TCHAR[parentLen + 1 + lpKC->dwKeyNameLen + 1 + valueNameLen + 1];
                    // 在父路径基础上添加当前键名和值名
                    if(lpFullValueName)
                    {
                        _tcscpy(lpFullValueName, lpParentPath);
                        _tcscat(lpFullValueName, TEXT("\\"));
                        _tcscat(lpFullValueName, lpRegKeyName);
                        if(valueNameLen > 0)
                        {
                            _tcscat(lpFullValueName, TEXT("\\"));
                            _tcscat(lpFullValueName, lpVC->lpValueName);
                        }
                    }
                }
                
                // 当前排除项链表不为空
                if(!RegSkipList.empty())
                {
                    // 完整值名在排除项链表中，释放当前值
                    if((lpFullValueName != NULL) && (IsInSkipList(lpFullValueName)))
                    {
                        delete[] lpFullValueName;
                        FreeAllValueContents(lpVC);
                        continue;
                    }
                    delete[] lpFullValueName;
                }
                lpShot->SumCounts.cValues++;
                // 尾插法将该值放入链表中,O(1)
                // 空链表
                if(lpKC->lpFirstValue == NULL)
                {
                    lpKC->lpFirstValue = lpVC;
                    lpTail = lpVC;
                }
                else
                {
                    lpTail->lpBrotherValue = lpVC;
                    lpTail = lpVC;
                }
                // 确保下一个结点的指针为空
                lpVC->lpBrotherValue = NULL;
                lpVC->dwTypeCode = ValueType;
                lpVC->dwDataSize = bValueData;
                // 值的数据
                if(bValueData > 0)
                {
                    lpVC->lpValueData = new BYTE[bValueData];
                    memcpy(lpVC->lpValueData, lpDataBuffer, bValueData);
                }
            }
        }
    }

    // 存在子键
    if(SubKeys > 0)
    {
        LPKeyContent lpKCSub;
        LPKeyContent lpTailSub;
        DWORD i;
        LPTSTR lpSubKeyName;
        LPKeyContent *lpKCPrev;
        HKEY hRegSubKey;

        if(MaxSubKeyNameLen > 0)
            MaxSubKeyNameLen++;
        // 为最长的子键名分配缓冲区
        dwSourceSize = MaxSubKeyNameLen * sizeof(TCHAR);
        dwStringBufferSize = AdjustBuffer(lpStringBuffer, dwStringBufferSize, dwSourceSize, REGSHOT_BUFFER_BLOCKSIZE);

        lpKCPrev = &lpKC->lpFirstSubKey;
        // 枚举子键
        for(i=0;;i++)
        {
            // 额外的块获取子键信息
            {
                DWORD bSubKeyName;
                bSubKeyName = dwStringBufferSize;
                // 枚举子键信息
                nError = RegEnumKeyEx(
                    hRegKey,
                    i,
                    lpStringBuffer,
                    &bSubKeyName,
                    NULL,
                    NULL,
                    NULL,
                    NULL
                    );
                if(nError == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }
                if(nError != ERROR_SUCCESS)
                {
                    continue;
                }
                // 结尾添加\0防止溢出
                lpStringBuffer[bSubKeyName] = (TCHAR)'\0';
                // 子键名称
                lpSubKeyName = NULL;
                if(bSubKeyName > 0)
                {
                    lpSubKeyName = new TCHAR[bSubKeyName+1];
                    _tcscpy(lpSubKeyName, lpStringBuffer);
                }
            }

            // 打开子键用于递归拍摄快照
            nError = RegOpenKeyEx(
                hRegKey,
                lpSubKeyName,
                0,
                KEY_READ,
                &hRegSubKey
                );
            if(nError != ERROR_SUCCESS)
            {
                delete[] lpSubKeyName;
                continue;
            }

            // 构建子键的父路径
            LPTSTR lpChildParentPath = NULL;
            if(lpParentPath == NULL || _tcslen(lpParentPath) == 0)
            {
                // 根键情况
                lpChildParentPath = new TCHAR[lpKC->dwKeyNameLen + 1];
                if(lpChildParentPath)
                {
                    _tcscpy(lpChildParentPath, lpRegKeyName);
                }
            }
            else
            {
                // 子键情况，在父路径基础上添加当前键名
                DWORD parentLen = _tcslen(lpParentPath);
                lpChildParentPath = new TCHAR[parentLen + 1 + lpKC->dwKeyNameLen + 1];
                if(lpChildParentPath)
                {
                    _tcscpy(lpChildParentPath, lpParentPath);
                    _tcscat(lpChildParentPath, TEXT("\\"));
                    _tcscat(lpChildParentPath, lpRegKeyName);
                }
            }
            
            lpKCSub = GetRegistrySnap(lpShot, hRegSubKey, lpSubKeyName, lpKC, lpKCPrev, lpChildParentPath);
            
            // 释放子键父路径
            if(lpChildParentPath != NULL)
            {
                delete[] lpChildParentPath;
            }
            RegCloseKey(hRegSubKey);
            // 将双重指针设置为兄弟键，进行下一次循环
            if(lpKCSub != NULL)
            {
                lpKCPrev = &lpKCSub->lpBrotherKey;
            }
        }
    }

    return lpKC;
}
// 对注册表所有根键拍摄快照
void RegShot::Shot(LPRegShotContent lpShot)
{
    Shot(lpShot, std::function<void(int, const QString &)>());
}

void RegShot::Shot(LPRegShotContent lpShot, const std::function<void(int, const QString &)> &progressCallback)
{
    lpStringBuffer = NULL;
    lpDataBuffer = NULL;

    // 设置计算机名称
    lpShot->lpComputerName = new TCHAR[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerName(lpShot->lpComputerName, &length);
    // 设置用户名
    lpShot->lpUserName = new TCHAR[UNLEN + 1];
    length = UNLEN + 1;
    GetUserName(lpShot->lpUserName, &length);
    // 设置系统时间
    GetSystemTime(&lpShot->SystemTime);
    // 保存根键
    if (progressCallback)
    {
        progressCallback(0, QStringLiteral("HKEY_CLASSES_ROOT"));
    }
    QDateTime t1 = QDateTime::currentDateTime();
    GetRegistrySnap(lpShot, HKEY_CLASSES_ROOT, lpHKCR, NULL, &lpShot->lpHKCR, NULL);
    QDateTime t2 = QDateTime::currentDateTime();
    qDebug() << "HKCR 完成:" << t1.msecsTo(t2) << "ms";

    if (progressCallback)
    {
        progressCallback(20, QStringLiteral("HKEY_CURRENT_USER"));
    }
    t1 = QDateTime::currentDateTime();
    GetRegistrySnap(lpShot, HKEY_CURRENT_USER, lpHKCU, NULL, &lpShot->lpHKCU, NULL);
    t2 = QDateTime::currentDateTime();
    qDebug() << "HKCU 完成:" << t1.msecsTo(t2) << "ms";

    if (progressCallback)
    {
        progressCallback(40, QStringLiteral("HKEY_LOCAL_MACHINE"));
    }
    t1 = QDateTime::currentDateTime();
    GetRegistrySnap(lpShot, HKEY_LOCAL_MACHINE, lpHKLM, NULL, &lpShot->lpHKLM, NULL);
    t2 = QDateTime::currentDateTime();
    qDebug() << "HKLM 完成:" << t1.msecsTo(t2) << "ms";

    if (progressCallback)
    {
        progressCallback(60, QStringLiteral("HKEY_USERS"));
    }
    t1 = QDateTime::currentDateTime();
    GetRegistrySnap(lpShot, HKEY_USERS, lpHKU, NULL, &lpShot->lpHKU, NULL);
    t2 = QDateTime::currentDateTime();
    qDebug() << "HKU 完成:" << t1.msecsTo(t2) << "ms";

    if (progressCallback)
    {
        progressCallback(80, QStringLiteral("HKEY_CURRENT_CONFIG"));
    }
    t1 = QDateTime::currentDateTime();
    GetRegistrySnap(lpShot, HKEY_CURRENT_CONFIG, lpHKCC, NULL, &lpShot->lpHKCC, NULL);
    t2 = QDateTime::currentDateTime();
    qDebug() << "HKCC 完成:" << t1.msecsTo(t2) << "ms";

    lpShot->SumCounts.cAll = lpShot->SumCounts.cKeys + lpShot->SumCounts.cValues;
    lpShot->fFinished = true;
    lpShot->fLoaded = false;

    if (lpStringBuffer != NULL)
    {
        delete[] lpStringBuffer;
    }
    if (lpDataBuffer != NULL)
    {
        delete[] lpDataBuffer;
    }
    if (progressCallback)
    {
        progressCallback(100, QStringLiteral("完成"));
    }
}
// 拍摄快照API
DWORD RegShot::TakeSnapShot()
{
    return TakeSnapShot(std::function<void(int, const QString &)>());
}

DWORD RegShot::TakeSnapShot(const std::function<void(int, const QString &)> &progressCallback)
{
    RegShotContent newShot;
    ZeroMemory(&newShot, sizeof(RegShotContent));
    // 设置快照属性
    LoadSettingsFromIni();
    newShot.i = Shots.size();
    Shot(&newShot, progressCallback);
    // 添加到拍摄的快照链表
    Shots.append(newShot);

    return newShot.i;
}

// 写入缓冲区函数
void RegShot::WriteFileBuffer(QDataStream &out, qint64 ofsFile, const void *lpData, DWORD dwDataSize)
{
    if (dwDataSize == 0 || lpData == nullptr)
    {
        return;
    }

    if (ofsFile < 0)
    {
        out.device()->seek(out.device()->size());
    }
    else
    {
        out.device()->seek(ofsFile);
    }

    out.writeRawData(reinterpret_cast<const char *>(lpData), dwDataSize);
}
// 保存键内容
void RegShot::SaveRegKeys(QDataStream &out, LPRegShotContent lpShot, LPKeyContent lpCurrentKey, quint64 ofsFatherKey, quint64 ofsCaller)
{
    // 遍历保存当前键及其兄弟键
    for (; lpCurrentKey != NULL; lpCurrentKey = lpCurrentKey->lpBrotherKey)
    {
        // 获取当前文件位置作为键的起始位置
        quint64 ofsKey = out.device()->pos();

        // 将偏移量写回（回写到父键或上一个兄弟键的指定字段）
        if (ofsCaller > 0)
        {
            WriteFileBuffer(out, ofsCaller, &ofsKey, sizeof(ofsKey));
        }
        // 初始化，先设置为0，然后再回写
        memset(&SaveKC, 0, sizeof(SaveKC));
        SaveKC.ofsKeyName = ofsKey + sizeof(SaveKC);
        SaveKC.ofsFatherKey = ofsFatherKey;

        // 额外的块保存当前键
        {
            LPTSTR lpKeyName = lpCurrentKey->lpKeyName;
            // 获取键名称长度
            if (lpKeyName != NULL)
            {
                SaveKC.dwKeyNameLen = lpCurrentKey->dwKeyNameLen;
                // 额外增加空字符
                if (SaveKC.dwKeyNameLen > 0)
                {
                    SaveKC.dwKeyNameLen++;
                }
            }
            // 键结构体内容写入文件
            WriteFileBuffer(out, -1, &SaveKC, sizeof(SaveKC));
            // 写入键名称
            if (SaveKC.dwKeyNameLen > 0)
            {
                WriteFileBuffer(out, -1, lpKeyName, SaveKC.dwKeyNameLen * sizeof(TCHAR));
            }
        }

        // 保存当前键的值
        if (lpCurrentKey->lpFirstValue != NULL)
        {
            LPValueContent lpVC;
            qint64 ofsValue;
            // 写入所有值，通过ofsCaller回写
            ofsCaller = ofsKey + offsetof(SaveKeyContent, ofsFirstValue);
            for (lpVC = lpCurrentKey->lpFirstValue; lpVC != NULL; lpVC = lpVC->lpBrotherValue)
            {
                // 获取值的位置
                ofsValue = out.device()->pos();
                // 将值的位置写入值结构体
                if (ofsCaller > 0)
                {
                    WriteFileBuffer(out, ofsCaller, &ofsValue, sizeof(ofsValue));
                }
                // 初始化并复制信息
                memset(&SaveVC, 0, sizeof(SaveVC));
                SaveVC.dwTypeCode = lpVC->dwTypeCode;
                SaveVC.dwDataSize = lpVC->dwDataSize;
                SaveVC.ofsFatherKey = ofsKey;
                SaveVC.ofsValueName = ofsValue + sizeof(SaveVC);
                // 获取值名称长度
                if (lpVC->lpValueName != NULL)
                {
                    SaveVC.dwValueNameLen = lpVC->dwValueNameLen;
                    if (SaveVC.dwValueNameLen > 0)
                    {
                        SaveVC.dwValueNameLen++;
                    }
                }
                // 值内容写入文件
                WriteFileBuffer(out, -1, &SaveVC, sizeof(SaveVC));
                // 写入值名称
                if (SaveVC.dwValueNameLen > 0)
                {
                    WriteFileBuffer(out, -1, lpVC->lpValueName, SaveVC.dwValueNameLen * sizeof(TCHAR));
                }
                // 写入值数据
                if (SaveVC.dwDataSize > 0)
                {
                    // 获取值数据的位置
                    qint64 ofsValueData = out.device()->pos();
                    // 将值数据的位置写入值结构体
                    WriteFileBuffer(out, ofsValue + offsetof(SaveValueContent, ofsValueData), &ofsValueData, sizeof(ofsValueData));
                    // 写入值数据
                    WriteFileBuffer(out, -1, lpVC->lpValueData, SaveVC.dwDataSize);
                }
                // 设置下一个值的位置，回写兄弟值
                ofsCaller = ofsValue + offsetof(SaveValueContent, ofsBrotherValue);
            }
        }

        // 递归写子键
        if (lpCurrentKey->lpFirstSubKey != NULL)
        {
            SaveRegKeys(out, lpShot, lpCurrentKey->lpFirstSubKey, ofsKey, ofsKey + offsetof(SaveKeyContent, ofsFirstSubKey));
        }
        // 设置下一个键的位置，回写兄弟键
        ofsCaller = ofsKey + offsetof(SaveKeyContent, ofsBrotherKey);
    }
}
// 保存快照
void RegShot::SaveShot(LPRegShotContent lpShot, const QString &path)
{
    // 检查是否需要保存
    if ((lpShot->lpHKCR == NULL) && (lpShot->lpHKCU == NULL) && (lpShot->lpHKLM == NULL) && (lpShot->lpHKU == NULL) && (lpShot->lpHKCC == NULL))
    {
        return;
    }
    // 打开用户选择的保存路径
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return;
    }
    QDataStream out(&file);
    // 获取当前位置
    qint64 StartPos = out.device()->pos();
    // 初始化文件头部并占位
    ZeroMemory(&fileheader, sizeof(fileheader));
    // 设置头部签名
    strncpy(fileheader.signature, RegScopeFileSignature, MAX_SIGNATURE_LENGTH);
    // 设置字节序
    fileheader.dwEndianness = 0x12345678;
    // 设置快照保存时间
    GetSystemTime(&fileheader.SystemTime);
    // 设置计算机名长度
    if (lpShot->lpComputerName != NULL)
    {
        fileheader.dwComputerNameLen = _tcslen(lpShot->lpComputerName) + 1;
    }
    // 设置用户名长度
    if (lpShot->lpUserName != NULL)
    {
        fileheader.dwUserNameLen = _tcslen(lpShot->lpUserName) + 1;
    }
    // 先写入一个临时文件头，占位
    WriteFileBuffer(out, -1, &fileheader, sizeof(fileheader));

    // 写入计算机名并回写偏移量
    if (lpShot->lpComputerName != NULL && fileheader.dwComputerNameLen > 0)
    {
        DWORD nFPCurrent = out.device()->pos();
        WriteFileBuffer(out, StartPos + offsetof(FileHeader, ofsComputerName), &nFPCurrent, sizeof(nFPCurrent));
        WriteFileBuffer(out, -1, lpShot->lpComputerName, fileheader.dwComputerNameLen * sizeof(TCHAR));
    }
    // 写入用户名并回写偏移量
    if (lpShot->lpUserName != NULL && fileheader.dwUserNameLen > 0)
    {
        DWORD nFPCurrent = out.device()->pos();
        WriteFileBuffer(out, StartPos + offsetof(FileHeader, ofsUserName), &nFPCurrent, sizeof(nFPCurrent));
        WriteFileBuffer(out, -1, lpShot->lpUserName, fileheader.dwUserNameLen * sizeof(TCHAR));
    }

    // 保存根键，通过ofsCaller参数回写偏移量到文件头
    if (lpShot->lpHKCR != NULL)
    {
        SaveRegKeys(out, lpShot, lpShot->lpHKCR, 0, StartPos + offsetof(FileHeader, ofsHKCR));
    }
    if (lpShot->lpHKCU != NULL)
    {
        SaveRegKeys(out, lpShot, lpShot->lpHKCU, 0, StartPos + offsetof(FileHeader, ofsHKCU));
    }
    if (lpShot->lpHKLM != NULL)
    {
        SaveRegKeys(out, lpShot, lpShot->lpHKLM, 0, StartPos + offsetof(FileHeader, ofsHKLM));
    }
    if (lpShot->lpHKU != NULL)
    {
        SaveRegKeys(out, lpShot, lpShot->lpHKU, 0, StartPos + offsetof(FileHeader, ofsHKU));
    }
    if (lpShot->lpHKCC != NULL)
    {
        SaveRegKeys(out, lpShot, lpShot->lpHKCC, 0, StartPos + offsetof(FileHeader, ofsHKCC));
    }
}
// 保存快照API
void RegShot::SaveSnapShot(DWORD index, const QString &path)
{
    // 保存快照
    SaveShot(&Shots[index], path);
    // 添加到列表
    SaveShots.append(Shots[index]);
}

// 加载注册表
void RegShot::LoadRegKeys(QDataStream &in, LPRegShotContent lpShot, quint64 ofsKey, LPKeyContent lpFatherKey, LPKeyContent *lpCaller)
{
    LPKeyContent lpKC;
    quint64 ofsBrotherKey;
    // 加载当前键及其兄弟键
    for (; ofsKey != 0; ofsKey = ofsBrotherKey)
    {
        // 定位到键的位置
        in.device()->seek(ofsKey);
        // 读取SaveKeyContent结构
        if (in.readRawData(reinterpret_cast<char *>(&SaveKC), sizeof(SaveKC)) != sizeof(SaveKC))
        {
            break;
        }
        // 保存兄弟键偏移量
        ofsBrotherKey = SaveKC.ofsBrotherKey;
        // 创建新的键
        lpKC = new KeyContent();
        lpKC->lpFatherKey = lpFatherKey;
        // 保存键名
        if ((SaveKC.ofsKeyName > 0) && (SaveKC.dwKeyNameLen > 0))
        {
            // 定位并复制信息
            in.device()->seek(SaveKC.ofsKeyName);
            lpKC->lpKeyName = new TCHAR[SaveKC.dwKeyNameLen];
            if (in.readRawData(reinterpret_cast<char *>(lpKC->lpKeyName), SaveKC.dwKeyNameLen * sizeof(TCHAR)) == SaveKC.dwKeyNameLen * sizeof(TCHAR))
            {
                lpKC->dwKeyNameLen = _tcslen(lpKC->lpKeyName);
            }
        }
        // 检查排除项
        if (!RegSkipList.empty())
        {
            // 获取完整键名
            LPTSTR lpFullKeyName = GetWholeKeyName(lpKC);
            // 如果是排除项则释放
            if (IsInSkipList(lpFullKeyName))
            {
                delete[] lpFullKeyName;
                FreeAllKeyContents(lpKC);
                continue;
            }
            delete[] lpFullKeyName;
        }
        // 链接到父键/兄弟键
        if (lpCaller != NULL)
        {
            *lpCaller = lpKC;
        }
        // 增加计数
        lpShot->SumCounts.cKeys++;
        // 加载当前键的值
        if (SaveKC.ofsFirstValue != 0)
        {
            LPValueContent *lpCallerVC = &lpKC->lpFirstValue;
            for (quint64 ofsValue = SaveKC.ofsFirstValue; ofsValue != 0; ofsValue = SaveVC.ofsBrotherValue)
            {
                // 定位到当前值
                in.device()->seek(ofsValue);
                // 读取SaveValueContent结构
                if (in.readRawData(reinterpret_cast<char *>(&SaveVC), sizeof(SaveVC)) != sizeof(SaveVC))
                {
                    break;
                }

                LPValueContent lpVC = new ValueContent();
                lpVC->lpFatherKey = lpKC;
                // 保存值名
                if ((SaveVC.dwValueNameLen > 0) && (SaveVC.ofsValueName > 0))
                {
                    in.device()->seek(SaveVC.ofsValueName);
                    lpVC->lpValueName = new TCHAR[SaveVC.dwValueNameLen];
                    in.readRawData(reinterpret_cast<char *>(lpVC->lpValueName), SaveVC.dwValueNameLen * sizeof(TCHAR));
                    lpVC->dwValueNameLen = _tcslen(lpVC->lpValueName);
                }
                // 检查排除项
                if (!RegSkipList.empty())
                {
                    LPTSTR lpFullValueName = GetWholeValueName(lpVC);
                    if (IsInSkipList(lpFullValueName))
                    {
                        delete[] lpFullValueName;
                        FreeAllValueContents(lpVC);
                        continue;
                    }
                    delete[] lpFullValueName;
                }
                // 链接父键/兄弟值
                if (lpCallerVC != NULL)
                {
                    *lpCallerVC = lpVC;
                }
                // 增加计数
                lpShot->SumCounts.cValues++;
                // 保存基本信息
                lpVC->dwTypeCode = SaveVC.dwTypeCode;
                lpVC->dwDataSize = SaveVC.dwDataSize;
                // 读取值数据
                if ((SaveVC.ofsValueData > 0) && (SaveVC.dwDataSize > 0))
                {
                    in.device()->seek(SaveVC.ofsValueData);
                    lpVC->lpValueData = new BYTE[SaveVC.dwDataSize];
                    in.readRawData(reinterpret_cast<char *>(lpVC->lpValueData), SaveVC.dwDataSize);
                }
                // 设置指针
                lpCallerVC = &lpVC->lpBrotherValue;
            }
        }

        // 递归处理子键
        if (SaveKC.ofsFirstSubKey != 0)
        {
            LoadRegKeys(in, lpShot, SaveKC.ofsFirstSubKey, lpKC, &lpKC->lpFirstSubKey);
        }
        // 设置指针
        lpCaller = &lpKC->lpBrotherKey;
    }
}
// 加载快照
bool RegShot::LoadShot(LPRegShotContent lpShot, const QString &path)
{
    LoadSettingsFromIni();
    // 打开指定路径的快照文件
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_0);
    // 检查文件大小
    DWORD dwFileSize = file.size();
    if (sizeof(fileheader) > dwFileSize)
    {
        file.close();
        return false;
    }
    // 读取文件头
    ZeroMemory(&fileheader, sizeof(fileheader));
    if (file.read(reinterpret_cast<char *>(&fileheader), sizeof(fileheader)) != sizeof(fileheader))
    {
        file.close();
        return false;
    }
    // 验证签名
    if (strncmp(fileheader.signature, RegScopeFileSignature, MAX_SIGNATURE_LENGTH) != 0)
    {
        file.close();
        return false;
    }
    // 验证字节序
    if (fileheader.dwEndianness != 0x12345678)
    {
        file.close();
        return false;
    }
    // 清空并准备快照
    FreeShot(lpShot);
    // 读取计算机名
    if (fileheader.dwComputerNameLen > 0 && fileheader.ofsComputerName > 0)
    {
        file.seek(fileheader.ofsComputerName);
        TCHAR *lpComputerName = new TCHAR[fileheader.dwComputerNameLen];
        if (file.read(reinterpret_cast<char *>(lpComputerName), fileheader.dwComputerNameLen * sizeof(TCHAR)) == fileheader.dwComputerNameLen * sizeof(TCHAR))
        {
            lpShot->lpComputerName = lpComputerName;
            lpShot->dwComputerName = _tcslen(lpShot->lpComputerName);
        }
        else
        {
            delete[] lpComputerName;
        }
    }
    // 读取用户名
    if (fileheader.dwUserNameLen > 0 && fileheader.ofsUserName > 0)
    {
        file.seek(fileheader.ofsUserName);
        TCHAR *lpUserName = new TCHAR[fileheader.dwUserNameLen];
        if (file.read(reinterpret_cast<char *>(lpUserName), fileheader.dwUserNameLen * sizeof(TCHAR)) == fileheader.dwUserNameLen * sizeof(TCHAR))
        {
            lpShot->lpUserName = lpUserName;
            lpShot->dwUserName = _tcslen(lpShot->lpUserName);
        }
        else
        {
            delete[] lpUserName;
        }
    }
    // 保存系统时间
    CopyMemory(&lpShot->SystemTime, &fileheader.SystemTime, sizeof(SYSTEMTIME));
    // 清空键保存结果和值保存结果用于加载键和值
    ZeroMemory(&SaveKC, sizeof(SaveKC));
    ZeroMemory(&SaveVC, sizeof(SaveVC));
    // 加载注册表键
    if (fileheader.ofsHKCR != 0)
    {
        LoadRegKeys(in, lpShot, fileheader.ofsHKCR, NULL, &lpShot->lpHKCR);
    }
    if (fileheader.ofsHKCU != 0)
    {
        LoadRegKeys(in, lpShot, fileheader.ofsHKCU, NULL, &lpShot->lpHKCU);
    }
    if (fileheader.ofsHKLM != 0)
    {
        LoadRegKeys(in, lpShot, fileheader.ofsHKLM, NULL, &lpShot->lpHKLM);
    }
    if (fileheader.ofsHKU != 0)
    {
        LoadRegKeys(in, lpShot, fileheader.ofsHKU, NULL, &lpShot->lpHKU);
    }
    if (fileheader.ofsHKCC != 0)
    {
        LoadRegKeys(in, lpShot, fileheader.ofsHKCC, NULL, &lpShot->lpHKCC);
    }
    // 关闭文件
    file.close();

    return true;
}

// 创建新的比较结果
void RegShot::CreateNewResult(DWORD ActionType, LPVOID lpOld, LPVOID lpNew)
{
    // 初始化
    LPCompares lpCR = new Compares();
    if (lpCR == NULL)
    {
        return;
    }
    lpCR->lpOld = lpOld;
    lpCR->lpNew = lpNew;
    lpCR->lpNext = NULL;

    // 根据不同的修改类型添加到对应的链表
    switch (ActionType)
    {
    // 添加到删除的键链表，尾插法
    case KEYDEL:
        (CompareResult.CRHeads.lpKeyDeleted == NULL) ? CompareResult.CRHeads.lpKeyDeleted = lpCR : CompareResult.CRCurrent.lpKeyDeleted->lpNext = lpCR;
        CompareResult.CRCurrent.lpKeyDeleted = lpCR;
        break;
    // 添加到新增的键链表
    case KEYADD:
        (CompareResult.CRHeads.lpKeyAdded == NULL) ? CompareResult.CRHeads.lpKeyAdded = lpCR : CompareResult.CRCurrent.lpKeyAdded->lpNext = lpCR;
        CompareResult.CRCurrent.lpKeyAdded = lpCR;
        break;
    // 添加到删除的值链表
    case VALDEL:
        (CompareResult.CRHeads.lpValueDeleted == NULL) ? CompareResult.CRHeads.lpValueDeleted = lpCR : CompareResult.CRCurrent.lpValueDeleted->lpNext = lpCR;
        CompareResult.CRCurrent.lpValueDeleted = lpCR;
        break;
    // 添加到新增的值链表
    case VALADD:
        (CompareResult.CRHeads.lpValueAdded == NULL) ? CompareResult.CRHeads.lpValueAdded = lpCR : CompareResult.CRCurrent.lpValueAdded->lpNext = lpCR;
        CompareResult.CRCurrent.lpValueAdded = lpCR;
        break;
    // 添加到修改的值链表
    case VALMODI:
        (CompareResult.CRHeads.lpValueModified == NULL) ? CompareResult.CRHeads.lpValueModified = lpCR : CompareResult.CRCurrent.lpValueModified->lpNext = lpCR;
        CompareResult.CRCurrent.lpValueModified = lpCR;
        break;
    }
}
// 比较注册表
void RegShot::CompareRegKeys(LPKeyContent lpStartKey1, LPKeyContent lpStartKey2, LPCTSTR lpParentPath)
{
    // 构建第二次快照当前键的哈希表索引
    std::unordered_map<std::wstring, LPKeyContent> KeyIndex;
    for (LPKeyContent lpKC2 = lpStartKey2; lpKC2 != NULL; lpKC2 = lpKC2->lpBrotherKey)
    {
        // 构建当前键的完整路径
        LPTSTR lpFullKeyName;
        // 根键
        if(lpParentPath == NULL || _tcslen(lpParentPath) == 0)
        {
            lpFullKeyName = new TCHAR[lpKC2->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpKC2->lpKeyName);
            }
        }
        // 子键情况，在父路径基础上添加当前键名
        else
        {
            DWORD parentLen = _tcslen(lpParentPath);
            lpFullKeyName = new TCHAR[parentLen + 1 + lpKC2->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpParentPath);
                _tcscat(lpFullKeyName, TEXT("\\"));
                _tcscat(lpFullKeyName, lpKC2->lpKeyName);
            }
        }
        if (lpFullKeyName != NULL)
        {
            KeyIndex.emplace(lpFullKeyName, lpKC2);
            delete[] lpFullKeyName;
        }
    }

    // 比较当前键及其兄弟键，第一次快照
    for (LPKeyContent lpKC1 = lpStartKey1; lpKC1 != NULL; lpKC1 = lpKC1->lpBrotherKey)
    {
        // 构建当前键的完整路径
        LPTSTR lpFullKeyName;
        // 根键
        if(lpParentPath == NULL || _tcslen(lpParentPath) == 0)
        {
            lpFullKeyName = new TCHAR[lpKC1->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpKC1->lpKeyName);
            }
        }
        // 子键情况，在父路径基础上添加当前键名
        else
        {
            DWORD parentLen = _tcslen(lpParentPath);
            lpFullKeyName = new TCHAR[parentLen + 1 + lpKC1->dwKeyNameLen + 1];
            if(lpFullKeyName)
            {
                _tcscpy(lpFullKeyName, lpParentPath);
                _tcscat(lpFullKeyName, TEXT("\\"));
                _tcscat(lpFullKeyName, lpKC1->lpKeyName);
            }
        }
        // 检查是否为排除项
        if (IsInSkipList(lpFullKeyName))
        {
            delete[] lpFullKeyName;
            continue;
        }
        std::wstring fullKeyName(lpFullKeyName);
        // 增加已比较的键数量
        CompareResult.dwCompared.cKeys++;
        // 从哈希表中寻找和当前键对应的键
        auto it = KeyIndex.find(fullKeyName);
        // 找到相同的键
        if (it != KeyIndex.end())
        {
            LPKeyContent lpKC2 = it->second;
            lpKC2->fKeyMatch = ISMATCH;

            // 额外的块遍历比较当前键包含的值，减少递归
            {
                LPValueContent lpVC1;
                LPValueContent lpVC2;
                // 构建第二次快照当前键的值名哈希表索引
                std::unordered_map<std::wstring, LPValueContent> ValueIndex;
                for (lpVC2 = lpKC2->lpFirstValue; lpVC2 != NULL; lpVC2 = lpVC2->lpBrotherValue)
                {
                    if (lpVC2->lpValueName)
                    {
                        ValueIndex.emplace(lpVC2->lpValueName, lpVC2);
                    }
                    else
                    {
                        ValueIndex.emplace(L"", lpVC2);
                    }
                }

                // 遍历比较值
                for (lpVC1 = lpKC1->lpFirstValue; lpVC1 != NULL; lpVC1 = lpVC1->lpBrotherValue)
                {
                    // 增加已比较的值数量
                    CompareResult.dwCompared.cValues++;

                    // 寻找相同名称的值
                    std::wstring ValueName;
                    if (lpVC1->lpValueName != NULL)
                    {
                        ValueName = lpVC1->lpValueName;
                    }
                    auto ValueIt = ValueIndex.find(ValueName);
                    // 找到相同名称的值
                    if (ValueIt != ValueIndex.end())
                    {
                        lpVC2 = ValueIt->second;
                        // 类型不同视为修改
                        if (lpVC1->dwTypeCode != lpVC2->dwTypeCode)
                        {
                            lpVC2->fValueMatch = ISMODI;
                            CompareResult.dwModified.cValues++;
                            CreateNewResult(VALMODI, lpVC1, lpVC2);
                            // 移除已经比较的值
                            ValueIndex.erase(ValueIt);
                            continue;
                        }
                        // 找到数据相同的值，比较值数据的大小，指针地址和数据名称
                        if ((lpVC1->dwDataSize == lpVC2->dwDataSize) && ((lpVC1->lpValueData == lpVC2->lpValueData) || ((lpVC1->lpValueData != NULL) && (lpVC2->lpValueData != NULL) && (memcmp(lpVC1->lpValueData, lpVC2->lpValueData, lpVC1->dwDataSize) == 0))))
                        {
                            lpVC2->fValueMatch = ISMATCH;
                        }
                        // 数据不相同，值被修改
                        else
                        {
                            lpVC2->fValueMatch = ISMODI;
                            CompareResult.dwModified.cValues++;
                            // 创建比较结果
                            CreateNewResult(VALMODI, lpVC1, lpVC2);
                        }
                        // 移除已经比较的值
                        ValueIndex.erase(ValueIt);
                    }
                    // 遍历完没有找到匹配的值，则该值已经被删除
                    else
                    {
                        CompareResult.dwDeleted.cValues++;
                        CreateNewResult(VALDEL, lpVC1, NULL);
                    }
                }

                // 再次遍历第二次快照值索引表，未被标记的即为新增
                for (auto &pair : ValueIndex)
                {
                    lpVC2 = pair.second;
                    CompareResult.dwCompared.cValues++;
                    CompareResult.dwAdded.cValues++;
                    CreateNewResult(VALADD, NULL, lpVC2);
                }
            }
            
            // 比较子键
            if ((lpKC1->lpFirstSubKey != NULL) || (lpKC2->lpFirstSubKey != NULL))
            {
                CompareRegKeys(lpKC1->lpFirstSubKey, lpKC2->lpFirstSubKey, lpFullKeyName);
            }
            // 移除已经比较的键
            KeyIndex.erase(it);
        }
        // 遍历完没有找到匹配的键，则该键已经被删除
        else
        {
            CompareResult.dwDeleted.cKeys++;
            CreateNewResult(KEYDEL, lpKC1, NULL);

            // 额外的块将该键包含的值全部设置为删除
            {
                LPValueContent lpVC1;

                for (lpVC1 = lpKC1->lpFirstValue; lpVC1 != NULL; lpVC1 = lpVC1->lpBrotherValue)
                {
                    CompareResult.dwCompared.cValues++;
                    CompareResult.dwDeleted.cValues++;
                    CreateNewResult(VALDEL, lpVC1, NULL);
                }
            }

            // 递归标记子键
            if (lpKC1->lpFirstSubKey != NULL)
            {
                CompareRegKeys(lpKC1->lpFirstSubKey, NULL, lpFullKeyName);
            }
        }

        delete[] lpFullKeyName;
    }

    // 遍历键的索引表中其他键，未被标记即为新增，第二次快照
    for (auto &pair : KeyIndex)
    {
        LPKeyContent lpKC2 = pair.second;
        // 检查是否为排除项
        if (IsInSkipList(pair.first.c_str()))
        {
            continue;
        }
        // 增加项数
        CompareResult.dwCompared.cKeys++;
        CompareResult.dwAdded.cKeys++;
        CreateNewResult(KEYADD, NULL, lpKC2);

        // 额外的块将当前键包含的值标记为新增
        {
            LPValueContent lpVC2;

            for (lpVC2 = lpKC2->lpFirstValue; lpVC2 != NULL; lpVC2 = lpVC2->lpBrotherValue)
            {
                CompareResult.dwCompared.cValues++;
                CompareResult.dwAdded.cValues++;
                CreateNewResult(VALADD, NULL, lpVC2);
            }
        }

        // 递归标记子键
        if (lpKC2->lpFirstSubKey != NULL)
        {
            CompareRegKeys(NULL, lpKC2->lpFirstSubKey, pair.first.c_str());
        }
    }
}
// 清空比较标记
void RegShot::ClearRegKeyMatchFlags(LPKeyContent lpCurrentKey)
{
    LPValueContent lpVC;

    for (; lpCurrentKey != NULL; lpCurrentKey = lpCurrentKey->lpBrotherKey)
    {
        // 初始化为NOMATCH
        lpCurrentKey->fKeyMatch = NOMATCH;
        // 初始化键包含的值
        for (lpVC = lpCurrentKey->lpFirstValue; lpVC != NULL; lpVC = lpVC->lpBrotherValue)
        {
            lpVC->fValueMatch = NOMATCH;
        }
        // 递归初始化子键
        ClearRegKeyMatchFlags(lpCurrentKey->lpFirstSubKey);
    }
}
// 比较快照
void RegShot::CompareShots(LPRegShotContent lpShot1, LPRegShotContent lpShot2)
{
    LoadSettingsFromIni();
    // 初始化标记
    FreeCompareResult();
    CompareResult.lpShot1 = lpShot1;
    CompareResult.lpShot2 = lpShot2;
    // 清除比较标记
    ClearRegKeyMatchFlags(lpShot1->lpHKCR);
    ClearRegKeyMatchFlags(lpShot2->lpHKCR);
    ClearRegKeyMatchFlags(lpShot1->lpHKCU);
    ClearRegKeyMatchFlags(lpShot2->lpHKCU);
    ClearRegKeyMatchFlags(lpShot1->lpHKLM);
    ClearRegKeyMatchFlags(lpShot2->lpHKLM);
    ClearRegKeyMatchFlags(lpShot1->lpHKU);
    ClearRegKeyMatchFlags(lpShot2->lpHKU);
    ClearRegKeyMatchFlags(lpShot1->lpHKCC);
    ClearRegKeyMatchFlags(lpShot2->lpHKCC);

    // 比较根键
    if ((CompareResult.lpShot1->lpHKCR != NULL) || (CompareResult.lpShot2->lpHKCR != NULL))
    {
        CompareRegKeys(CompareResult.lpShot1->lpHKCR, CompareResult.lpShot2->lpHKCR);
    }
    if ((CompareResult.lpShot1->lpHKCU != NULL) || (CompareResult.lpShot2->lpHKCU != NULL))
    {
        CompareRegKeys(CompareResult.lpShot1->lpHKCU, CompareResult.lpShot2->lpHKCU);
    }
    if ((CompareResult.lpShot1->lpHKLM != NULL) || (CompareResult.lpShot2->lpHKLM != NULL))
    {
        CompareRegKeys(CompareResult.lpShot1->lpHKLM, CompareResult.lpShot2->lpHKLM);
    }
    if ((CompareResult.lpShot1->lpHKU != NULL) || (CompareResult.lpShot2->lpHKU != NULL))
    {
        CompareRegKeys(CompareResult.lpShot1->lpHKU, CompareResult.lpShot2->lpHKU);
    }
    if ((CompareResult.lpShot1->lpHKCC != NULL) || (CompareResult.lpShot2->lpHKCC != NULL))
    {
        CompareRegKeys(CompareResult.lpShot1->lpHKCC, CompareResult.lpShot2->lpHKCC);
    }

    // 更新计数
    CompareResult.dwCompared.cAll = CompareResult.dwCompared.cKeys + CompareResult.dwCompared.cValues;
    CompareResult.dwDeleted.cAll = CompareResult.dwDeleted.cKeys + CompareResult.dwDeleted.cValues;
    CompareResult.dwAdded.cAll = CompareResult.dwAdded.cKeys + CompareResult.dwAdded.cValues;
    CompareResult.dwModified.cAll = CompareResult.dwModified.cKeys + CompareResult.dwModified.cValues;
    // 完成快照比较
    CompareResult.fFinished = TRUE;
}

// 释放值
void RegShot::FreeAllValueContents(LPValueContent lpCurrentValue)
{
    LPValueContent lpBrotherVC;
    for (; lpCurrentValue != NULL; lpCurrentValue = lpBrotherVC)
    {
        lpBrotherVC = lpCurrentValue->lpBrotherValue;
        // 释放值名称
        if (lpCurrentValue->lpValueName != NULL)
        {
            delete[] lpCurrentValue->lpValueName;
            lpCurrentValue->lpValueName = NULL;
        }
        // 释放值的数据
        if (lpCurrentValue->lpValueData != NULL)
        {
            delete[] lpCurrentValue->lpValueData;
            lpCurrentValue->lpValueData = NULL;
        }
        // 释放值
        delete lpCurrentValue;
        lpCurrentValue = NULL;
    }
}
// 释放键
void RegShot::FreeAllKeyContents(LPKeyContent lpCurrentKey)
{
    LPKeyContent lpBrotherKC;
    for (; lpCurrentKey != NULL; lpCurrentKey = lpBrotherKC)
    {
        lpBrotherKC = lpCurrentKey->lpBrotherKey;
        // 释放除根键之外的其他键的名称
        if (lpCurrentKey->lpKeyName != NULL)
        {
            if ((lpCurrentKey->lpFatherKey != NULL) || ((lpCurrentKey->lpKeyName != lpHKCR) && (lpCurrentKey->lpKeyName != lpHKCU) && (lpCurrentKey->lpKeyName != lpHKLM) && (lpCurrentKey->lpKeyName != lpHKU) && (lpCurrentKey->lpKeyName != lpHKCC)))
            {
                delete[] lpCurrentKey->lpKeyName;
                lpCurrentKey->lpKeyName = NULL;
            }
        }
        // 释放键中包含的值
        if (lpCurrentKey->lpFirstValue != NULL)
        {
            FreeAllValueContents(lpCurrentKey->lpFirstValue);
        }
        // 递归释放子键
        if (lpCurrentKey->lpFirstSubKey != NULL)
        {
            FreeAllKeyContents(lpCurrentKey->lpFirstSubKey);
        }
        // 释放键本身
        delete lpCurrentKey;
        lpCurrentKey = NULL;
    }
}
// 释放快照
void RegShot::FreeShot(LPRegShotContent lpShot)
{
    if (lpShot->lpHKCR != NULL)
    {
        FreeAllKeyContents(lpShot->lpHKCR);
    }
    if (lpShot->lpHKCU != NULL)
    {
        FreeAllKeyContents(lpShot->lpHKCU);
    }
    if (lpShot->lpHKLM != NULL)
    {
        FreeAllKeyContents(lpShot->lpHKLM);
    }
    if (lpShot->lpHKU != NULL)
    {
        FreeAllKeyContents(lpShot->lpHKU);
    }
    if (lpShot->lpHKCC != NULL)
    {
        FreeAllKeyContents(lpShot->lpHKCC);
    }

    ZeroMemory(lpShot, sizeof(RegShotContent));
}
// 释放比较结果辅助函数
void RegShot::FreeAllCompare(LPCompares lpCR)
{
    LPCompares lpNextCR;

    for (; lpCR != NULL; lpCR = lpNextCR)
    {
        // 保存下一个指针
        lpNextCR = lpCR->lpNext;
        // 释放当前指针
        delete lpCR;
    }
}
// 释放比较结果
void RegShot::FreeCompareResult()
{
    // 释放各个链表
    FreeAllCompare(CompareResult.CRHeads.lpKeyDeleted);
    FreeAllCompare(CompareResult.CRHeads.lpKeyAdded);
    FreeAllCompare(CompareResult.CRHeads.lpValueDeleted);
    FreeAllCompare(CompareResult.CRHeads.lpValueAdded);
    FreeAllCompare(CompareResult.CRHeads.lpValueModified);
    // 清空内存
    ZeroMemory(&CompareResult, sizeof(CompareResult));
}
