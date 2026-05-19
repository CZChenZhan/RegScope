#include "registrymonitor.h"

#include "regshot.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QStringList>
#include <QVarLengthArray>
#include <algorithm>
#include <memory>
#include <oaidl.h>
#include <oleauto.h>
#include <sddl.h>
#include <tdh.h>
#include <wbemidl.h>

namespace
{
    constexpr wchar_t kRegistrySessionName[] = L"RegScopeRegistryEtwSession";
    const GUID kKernelRegistryProviderGuid = {0x70eb4f03, 0xc1de, 0x4f73, {0xa0, 0x51, 0x33, 0xd1, 0x3d, 0x54, 0x13, 0xbd}};

    thread_local class WmiProcessResolver *g_wmiResolver = nullptr;

    QString readInfoString(const TRACE_EVENT_INFO *eventInfo, ULONG offset)
    {
        if (eventInfo == nullptr || offset == 0)
        {
            return {};
        }

        const auto *base = reinterpret_cast<const BYTE *>(eventInfo);
        const auto *text = reinterpret_cast<const wchar_t *>(base + offset);
        return QString::fromWCharArray(text);
    }

    QString trimTrailingNulls(QString value)
    {
        while (!value.isEmpty() && value.endsWith(QChar(u'\0')))
        {
            value.chop(1);
        }
        return value;
    }

    QString currentUserSid()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            return {};
        }

        DWORD requiredSize = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &requiredSize);
        if (requiredSize == 0)
        {
            CloseHandle(token);
            return {};
        }

        QByteArray buffer(static_cast<int>(requiredSize), '\0');
        if (!GetTokenInformation(token, TokenUser, buffer.data(), requiredSize, &requiredSize))
        {
            CloseHandle(token);
            return {};
        }

        auto *tokenUser = reinterpret_cast<TOKEN_USER *>(buffer.data());
        LPWSTR sidString = nullptr;
        QString sid;
        if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidString))
        {
            sid = QString::fromWCharArray(sidString);
            LocalFree(sidString);
        }

        CloseHandle(token);
        return sid;
    }

    QString normalizeRegistryPath(QString path)
    {
        static const QString userSid = currentUserSid();

        path.replace('/', '\\');
        if (path.startsWith("\\REGISTRY\\MACHINE\\SOFTWARE\\CLASSES", Qt::CaseInsensitive))
        {
            return "HKCR" + path.mid(QStringLiteral("\\REGISTRY\\MACHINE\\SOFTWARE\\CLASSES").size());
        }

        if (path.compare("\\REGISTRY\\MACHINE", Qt::CaseInsensitive) == 0)
        {
            return "HKLM";
        }

        if (path.startsWith("\\REGISTRY\\MACHINE\\", Qt::CaseInsensitive))
        {
            return "HKLM\\" + path.mid(QStringLiteral("\\REGISTRY\\MACHINE\\").size());
        }

        if (path.compare("\\REGISTRY\\USER", Qt::CaseInsensitive) == 0)
        {
            return "HKU";
        }

        if (path.startsWith("\\REGISTRY\\USER\\", Qt::CaseInsensitive))
        {
            QString rest = path.mid(QStringLiteral("\\REGISTRY\\USER\\").size());
            if (!userSid.isEmpty() && rest.compare(userSid, Qt::CaseInsensitive) == 0)
            {
                return "HKCU";
            }

            if (!userSid.isEmpty() && rest.startsWith(userSid + "\\", Qt::CaseInsensitive))
            {
                return "HKCU\\" + rest.mid(userSid.size() + 1);
            }

            return "HKU\\" + rest;
        }

        return path;
    }

    QString joinRegistryPath(const QString &baseName, const QString &relativeName)
    {
        if (baseName.isEmpty())
        {
            return relativeName;
        }

        if (relativeName.isEmpty())
        {
            return baseName;
        }

        QString joined = baseName;
        if (!joined.endsWith('\\'))
        {
            joined += '\\';
        }
        joined += relativeName;
        return joined;
    }

    bool readPropertyRaw(EVENT_RECORD *eventRecord, TRACE_EVENT_INFO *eventInfo, const wchar_t *propertyName, QByteArray &data, USHORT &inType)
    {
        if (eventRecord == nullptr || eventInfo == nullptr || propertyName == nullptr)
        {
            return false;
        }

        const QString wantedName = QString::fromWCharArray(propertyName);
        bool found = false;
        inType = TDH_INTYPE_UNICODESTRING;

        for (ULONG index = 0; index < eventInfo->TopLevelPropertyCount; ++index)
        {
            const auto &propertyInfo = eventInfo->EventPropertyInfoArray[index];
            if (readInfoString(eventInfo, propertyInfo.NameOffset).compare(wantedName, Qt::CaseInsensitive) == 0)
            {
                inType = propertyInfo.nonStructType.InType;
                found = true;
                break;
            }
        }

        if (!found)
        {
            return false;
        }

        PROPERTY_DATA_DESCRIPTOR descriptor = {};
        descriptor.ArrayIndex = ULONG_MAX;
        descriptor.PropertyName = reinterpret_cast<ULONGLONG>(propertyName);

        ULONG propertySize = 0;
        if (TdhGetPropertySize(eventRecord, 0, nullptr, 1, &descriptor, &propertySize) != ERROR_SUCCESS || propertySize == 0)
        {
            return false;
        }

        data.resize(static_cast<int>(propertySize));
        if (TdhGetProperty(eventRecord, 0, nullptr, 1, &descriptor, propertySize, reinterpret_cast<PBYTE>(data.data())) != ERROR_SUCCESS)
        {
            data.clear();
            return false;
        }

        return true;
    }

    QString propertyToString(EVENT_RECORD *eventRecord, TRACE_EVENT_INFO *eventInfo, const wchar_t *propertyName)
    {
        QByteArray data;
        USHORT inType = TDH_INTYPE_UNICODESTRING;
        if (!readPropertyRaw(eventRecord, eventInfo, propertyName, data, inType))
        {
            return {};
        }

        switch (inType)
        {
        case TDH_INTYPE_UNICODESTRING:
        {
            const auto *value = reinterpret_cast<const wchar_t *>(data.constData());
            return trimTrailingNulls(QString::fromWCharArray(value, data.size() / static_cast<int>(sizeof(wchar_t))));
        }
        case TDH_INTYPE_ANSISTRING:
            return trimTrailingNulls(QString::fromLocal8Bit(data.constData(), data.size()));
        case TDH_INTYPE_UINT32:
        case TDH_INTYPE_HEXINT32:
            if (data.size() >= static_cast<int>(sizeof(quint32)))
            {
                return QString::number(*reinterpret_cast<const quint32 *>(data.constData()));
            }
            break;
        case TDH_INTYPE_UINT64:
        case TDH_INTYPE_HEXINT64:
            if (data.size() >= static_cast<int>(sizeof(quint64)))
            {
                return QString::number(*reinterpret_cast<const quint64 *>(data.constData()));
            }
            break;
        default:
            break;
        }

        return {};
    }

    quint32 propertyToUInt32(EVENT_RECORD *eventRecord, TRACE_EVENT_INFO *eventInfo, const wchar_t *propertyName, quint32 defaultValue = 0)
    {
        QByteArray data;
        USHORT inType = TDH_INTYPE_UINT32;
        if (!readPropertyRaw(eventRecord, eventInfo, propertyName, data, inType))
        {
            return defaultValue;
        }

        if (data.size() >= static_cast<int>(sizeof(quint32)))
        {
            return *reinterpret_cast<const quint32 *>(data.constData());
        }

        return defaultValue;
    }

    QString actionKeyFromOpcode(const QString &opcodeName)
    {
        const QString lowered = opcodeName.toLower();
        if (lowered.contains("createkey"))
        {
            return "added";
        }

        if (lowered.contains("deletekey") || lowered.contains("deletevaluekey"))
        {
            return "deleted";
        }

        if (lowered.contains("setvaluekey") || lowered.contains("setinformationkey"))
        {
            return "modified";
        }

        return {};
    }

    struct RegistryPathInfo
    {
        QString fullPath;
        QString keyPath;
        QString keyName;
        QString valueName;
        bool hasValue = false;
    };

    RegistryPathInfo buildRegistryPathInfo(EVENT_RECORD *eventRecord, TRACE_EVENT_INFO *eventInfo, const QString &actionKey)
    {
        RegistryPathInfo pathInfo;

        QString keyPath = propertyToString(eventRecord, eventInfo, L"KeyName");
        if (keyPath.isEmpty())
        {
            const QString baseName = propertyToString(eventRecord, eventInfo, L"BaseName");
            const QString relativeName = propertyToString(eventRecord, eventInfo, L"RelativeName");
            keyPath = joinRegistryPath(baseName, relativeName);
        }

        if (keyPath.isEmpty())
        {
            return pathInfo;
        }

        keyPath = normalizeRegistryPath(keyPath);
        const QString valueName = propertyToString(eventRecord, eventInfo, L"ValueName");

        pathInfo.keyPath = keyPath;
        const int lastSeparator = keyPath.lastIndexOf('\\');
        pathInfo.keyName = lastSeparator >= 0 ? keyPath.mid(lastSeparator + 1) : keyPath;

        if (!valueName.isNull() && !valueName.isEmpty())
        {
            pathInfo.hasValue = true;
            pathInfo.valueName = valueName;
            pathInfo.fullPath = keyPath + "\\" + valueName;
            return pathInfo;
        }

        if (!valueName.isNull() && (actionKey == "modified" || actionKey == "deleted"))
        {
            pathInfo.hasValue = true;
            pathInfo.valueName = QStringLiteral("(Default)");
            pathInfo.fullPath = keyPath + "\\(Default)";
            return pathInfo;
        }

        pathInfo.fullPath = keyPath;
        return pathInfo;
    }

    QString formatRegistryValueForDisplay(DWORD valueType, const QByteArray &valueData)
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
                       : trimTrailingNulls(QString::fromWCharArray(
                             reinterpret_cast<const wchar_t *>(valueData.constData()),
                             valueData.size() / static_cast<int>(sizeof(wchar_t))));
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
                       ? QStringLiteral("<empty binary>")
                       : QString("[%1 bytes] %2").arg(valueData.size()).arg(bytesToHex(valueData));
        default:
            return valueData.isEmpty()
                       ? QStringLiteral("<empty>")
                       : QString("[type=%1] %2").arg(valueType).arg(bytesToHex(valueData));
        }

        return QStringLiteral("<unresolved>");
    }

    bool resolveRegistryRoot(const QString &keyPath, HKEY &rootKey, QString &subKeyPath)
    {
        const struct RootMap
        {
            const char *prefix;
            HKEY key;
        } rootMappings[] = {
            {"HKCR", HKEY_CLASSES_ROOT},
            {"HKCU", HKEY_CURRENT_USER},
            {"HKLM", HKEY_LOCAL_MACHINE},
            {"HKU", HKEY_USERS},
            {"HKCC", HKEY_CURRENT_CONFIG}};

        for (const RootMap &mapping : rootMappings)
        {
            const QString rootPrefix = QString::fromLatin1(mapping.prefix);
            if (keyPath.compare(rootPrefix, Qt::CaseInsensitive) == 0)
            {
                rootKey = mapping.key;
                subKeyPath.clear();
                return true;
            }

            const QString prefixWithSeparator = rootPrefix + "\\";
            if (keyPath.startsWith(prefixWithSeparator, Qt::CaseInsensitive))
            {
                rootKey = mapping.key;
                subKeyPath = keyPath.mid(prefixWithSeparator.size());
                return true;
            }
        }

        return false;
    }

    bool queryRegistryValueData(const QString &keyPath, const QString &valueName, QString &valueData)
    {
        HKEY rootKey = nullptr;
        QString subKeyPath;
        if (!resolveRegistryRoot(keyPath, rootKey, subKeyPath))
        {
            return false;
        }

        HKEY openedKey = nullptr;
        const LONG openResult = RegOpenKeyExW(rootKey,
                                              subKeyPath.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(subKeyPath.utf16()),
                                              0,
                                              KEY_QUERY_VALUE,
                                              &openedKey);
        if (openResult != ERROR_SUCCESS)
        {
            return false;
        }

        LPCWSTR queryValueName = nullptr;
        if (!valueName.isEmpty() && valueName.compare(QStringLiteral("(Default)"), Qt::CaseInsensitive) != 0)
        {
            queryValueName = reinterpret_cast<LPCWSTR>(valueName.utf16());
        }

        DWORD valueType = REG_NONE;
        DWORD dataSize = 0;
        LONG queryResult = RegQueryValueExW(openedKey, queryValueName, nullptr, &valueType, nullptr, &dataSize);
        if (queryResult != ERROR_SUCCESS && queryResult != ERROR_MORE_DATA)
        {
            RegCloseKey(openedKey);
            return false;
        }

        QByteArray dataBuffer(static_cast<int>(dataSize), '\0');
        queryResult = RegQueryValueExW(openedKey,
                                       queryValueName,
                                       nullptr,
                                       &valueType,
                                       dataSize == 0 ? nullptr : reinterpret_cast<LPBYTE>(dataBuffer.data()),
                                       &dataSize);
        RegCloseKey(openedKey);
        if (queryResult != ERROR_SUCCESS)
        {
            return false;
        }

        dataBuffer.resize(static_cast<int>(dataSize));
        valueData = formatRegistryValueForDisplay(valueType, dataBuffer);
        return true;
    }

    QString fallbackProcessName(DWORD processId)
    {
        HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (processHandle == nullptr)
        {
            return {};
        }

        DWORD requiredSize = 32768;
        std::wstring buffer(requiredSize, L'\0');
        QString processName;
        if (QueryFullProcessImageNameW(processHandle, 0, buffer.data(), &requiredSize))
        {
            processName = QFileInfo(QString::fromWCharArray(buffer.data(), static_cast<int>(requiredSize))).fileName();
        }

        CloseHandle(processHandle);
        return processName;
    }

    struct ProcessMetadata
    {
        QString processName;
        QString commandLine;
    };

    class WmiProcessResolver
    {
    public:
        WmiProcessResolver() = default;

        bool initialize()
        {
            HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            m_comInitialized = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;

            result = CoInitializeSecurity(nullptr,
                                          -1,
                                          nullptr,
                                          nullptr,
                                          RPC_C_AUTHN_LEVEL_DEFAULT,
                                          RPC_C_IMP_LEVEL_IMPERSONATE,
                                          nullptr,
                                          EOAC_NONE,
                                          nullptr);
            if (FAILED(result) && result != RPC_E_TOO_LATE)
            {
                return false;
            }

            result = CoCreateInstance(CLSID_WbemLocator,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IWbemLocator,
                                      reinterpret_cast<LPVOID *>(&m_locator));
            if (FAILED(result) || m_locator == nullptr)
            {
                return false;
            }

            BSTR resource = SysAllocString(L"ROOT\\CIMV2");
            result = m_locator->ConnectServer(resource, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_services);
            SysFreeString(resource);
            if (FAILED(result) || m_services == nullptr)
            {
                return false;
            }

            result = CoSetProxyBlanket(m_services,
                                       RPC_C_AUTHN_WINNT,
                                       RPC_C_AUTHZ_NONE,
                                       nullptr,
                                       RPC_C_AUTHN_LEVEL_CALL,
                                       RPC_C_IMP_LEVEL_IMPERSONATE,
                                       nullptr,
                                       EOAC_NONE);
            return SUCCEEDED(result);
        }

        void shutdown()
        {
            if (m_services != nullptr)
            {
                m_services->Release();
                m_services = nullptr;
            }

            if (m_locator != nullptr)
            {
                m_locator->Release();
                m_locator = nullptr;
            }

            if (m_comInitialized)
            {
                CoUninitialize();
                m_comInitialized = false;
            }
        }

        ProcessMetadata query(DWORD processId)
        {
            if (const auto cached = m_cache.constFind(processId); cached != m_cache.constEnd())
            {
                return cached.value();
            }

            ProcessMetadata metadata;
            metadata.processName = fallbackProcessName(processId);
            if (m_services == nullptr)
            {
                m_cache.insert(processId, metadata);
                return metadata;
            }

            BSTR language = SysAllocString(L"WQL");
            BSTR query = SysAllocString(reinterpret_cast<const OLECHAR *>(
                QString("SELECT Name, CommandLine, ExecutablePath FROM Win32_Process WHERE ProcessId = %1")
                    .arg(processId)
                    .utf16()));

            IEnumWbemClassObject *enumerator = nullptr;
            HRESULT result = m_services->ExecQuery(language,
                                                   query,
                                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                                   nullptr,
                                                   &enumerator);
            SysFreeString(query);
            SysFreeString(language);

            if (SUCCEEDED(result) && enumerator != nullptr)
            {
                IWbemClassObject *object = nullptr;
                ULONG returned = 0;
                if (SUCCEEDED(enumerator->Next(WBEM_INFINITE, 1, &object, &returned)) && returned == 1 && object != nullptr)
                {
                    auto readStringProperty = [object](const wchar_t *propertyName) -> QString
                    {
                        VARIANT value;
                        VariantInit(&value);
                        QString text;
                        if (SUCCEEDED(object->Get(propertyName, 0, &value, nullptr, nullptr)))
                        {
                            if (value.vt == VT_BSTR && value.bstrVal != nullptr)
                            {
                                text = QString::fromWCharArray(value.bstrVal);
                            }
                        }
                        VariantClear(&value);
                        return text;
                    };

                    const QString imagePath = readStringProperty(L"ExecutablePath");
                    const QString name = readStringProperty(L"Name");
                    metadata.commandLine = readStringProperty(L"CommandLine");

                    if (metadata.processName.isEmpty())
                    {
                        metadata.processName = !imagePath.isEmpty() ? QFileInfo(imagePath).fileName() : name;
                    }

                    object->Release();
                }

                enumerator->Release();
            }

            if (metadata.processName.isEmpty())
            {
                metadata.processName = QString("PID %1").arg(processId);
            }

            m_cache.insert(processId, metadata);
            return metadata;
        }

    private:
        bool m_comInitialized = false;
        IWbemLocator *m_locator = nullptr;
        IWbemServices *m_services = nullptr;
        QHash<DWORD, ProcessMetadata> m_cache;
    };

    QDateTime fileTimeToDateTime(const LARGE_INTEGER &timeStamp)
    {
        FILETIME fileTime = {};
        fileTime.dwLowDateTime = timeStamp.LowPart;
        fileTime.dwHighDateTime = timeStamp.HighPart;

        SYSTEMTIME systemTime = {};
        if (!FileTimeToSystemTime(&fileTime, &systemTime))
        {
            return QDateTime::currentDateTime();
        }

        QDate date(systemTime.wYear, systemTime.wMonth, systemTime.wDay);
        QTime time(systemTime.wHour, systemTime.wMinute, systemTime.wSecond, systemTime.wMilliseconds);
        return QDateTime(date, time, Qt::UTC).toLocalTime();
    }
} // namespace

RegistryMonitor::RegistryMonitor(QObject *parent)
    : QObject(parent), m_running(false), m_stopRequested(false), m_sessionHandle(0), m_traceHandle(0), m_riskLevel(1)
{
    qRegisterMetaType<RegistryEtwEvent>("RegistryEtwEvent");
}

RegistryMonitor::~RegistryMonitor()
{
    stopMonitoring();
}

bool RegistryMonitor::startMonitoring(int riskLevel, const QString &configFilePath)
{
    if (m_running.load())
    {
        return false;
    }

    m_riskLevel = riskLevel;
    m_configFilePath = configFilePath;
    m_stopRequested.store(false);
    m_running.store(true);
    m_sessionName = kRegistrySessionName;
    m_worker = std::thread(&RegistryMonitor::runTraceSession, this, riskLevel, configFilePath);
    return true;
}

void RegistryMonitor::stopMonitoring()
{
    if (!m_running.load() && !m_worker.joinable())
    {
        return;
    }

    m_stopRequested.store(true);

    TRACEHANDLE traceHandle = 0;
    TRACEHANDLE sessionHandle = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        traceHandle = m_traceHandle;
        sessionHandle = m_sessionHandle;
    }

    if (traceHandle != 0 && traceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(traceHandle);
    }

    if (sessionHandle != 0)
    {
        ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kRegistrySessionName);
        auto properties = std::make_unique<BYTE[]>(bufferSize);
        ZeroMemory(properties.get(), bufferSize);
        auto *traceProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties.get());
        traceProperties->Wnode.BufferSize = bufferSize;
        traceProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(sessionHandle, m_sessionName.c_str(), traceProperties, EVENT_TRACE_CONTROL_STOP);
    }

    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

bool RegistryMonitor::isRunning() const
{
    return m_running.load();
}

void RegistryMonitor::runTraceSession(int riskLevel, const QString &configFilePath)
{
    WmiProcessResolver resolver;
    resolver.initialize();
    g_wmiResolver = &resolver;

    m_analysisFilter = std::make_unique<RegShot>();
    m_analysisFilter->InitializeAnalysisFilter(riskLevel, configFilePath);

    ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kRegistrySessionName);
    auto properties = std::make_unique<BYTE[]>(bufferSize);
    ZeroMemory(properties.get(), bufferSize);

    auto *traceProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties.get());
    traceProperties->Wnode.BufferSize = bufferSize;
    traceProperties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    traceProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    traceProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ControlTraceW(0, m_sessionName.c_str(), traceProperties, EVENT_TRACE_CONTROL_STOP);

    TRACEHANDLE sessionHandle = 0;
    ULONG status = StartTraceW(&sessionHandle, m_sessionName.c_str(), traceProperties);
    if (status != ERROR_SUCCESS)
    {
        m_running.store(false);
        emit monitorError(QString("StartTrace failed: %1").arg(status));
        m_analysisFilter.reset();
        resolver.shutdown();
        g_wmiResolver = nullptr;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_sessionHandle = sessionHandle;
    }

    ENABLE_TRACE_PARAMETERS enableParameters = {};
    enableParameters.Version = ENABLE_TRACE_PARAMETERS_VERSION;
    status = EnableTraceEx2(sessionHandle,
                            &kKernelRegistryProviderGuid,
                            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_VERBOSE,
                            0,
                            0,
                            0,
                            &enableParameters);
    if (status != ERROR_SUCCESS)
    {
        emit monitorError(QString("EnableTraceEx2 failed: %1").arg(status));
        ControlTraceW(sessionHandle, m_sessionName.c_str(), traceProperties, EVENT_TRACE_CONTROL_STOP);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_sessionHandle = 0;
        }
        m_running.store(false);
        m_analysisFilter.reset();
        resolver.shutdown();
        g_wmiResolver = nullptr;
        return;
    }

    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_REAL_TIME;
    logFile.EventRecordCallback = &RegistryMonitor::eventRecordCallback;
    logFile.Context = this;

    TRACEHANDLE traceHandle = OpenTraceW(&logFile);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        emit monitorError(QString("OpenTrace failed: %1").arg(GetLastError()));
        EnableTraceEx2(sessionHandle,
                       &kKernelRegistryProviderGuid,
                       EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                       TRACE_LEVEL_VERBOSE,
                       0,
                       0,
                       0,
                       nullptr);
        ControlTraceW(sessionHandle, m_sessionName.c_str(), traceProperties, EVENT_TRACE_CONTROL_STOP);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_sessionHandle = 0;
        }
        m_running.store(false);
        m_analysisFilter.reset();
        resolver.shutdown();
        g_wmiResolver = nullptr;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_traceHandle = traceHandle;
    }

    Q_UNUSED(riskLevel);
    Q_UNUSED(configFilePath);
    emit monitorStarted();
    ProcessTrace(&traceHandle, 1, nullptr, nullptr);

    EnableTraceEx2(sessionHandle,
                   &kKernelRegistryProviderGuid,
                   EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                   TRACE_LEVEL_VERBOSE,
                   0,
                   0,
                   0,
                   nullptr);
    ControlTraceW(sessionHandle, m_sessionName.c_str(), traceProperties, EVENT_TRACE_CONTROL_STOP);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_traceHandle = 0;
        m_sessionHandle = 0;
    }

    m_running.store(false);
    m_analysisFilter.reset();
    resolver.shutdown();
    g_wmiResolver = nullptr;
    emit monitorStopped();
}

void RegistryMonitor::handleEventRecord(EVENT_RECORD *eventRecord)
{
    if (eventRecord == nullptr || eventRecord->EventHeader.ProviderId != kKernelRegistryProviderGuid)
    {
        return;
    }

    ULONG bufferSize = 0;
    if (TdhGetEventInformation(eventRecord, 0, nullptr, nullptr, &bufferSize) != ERROR_INSUFFICIENT_BUFFER)
    {
        return;
    }

    QVarLengthArray<BYTE, 4096> buffer(static_cast<int>(bufferSize));
    auto *eventInfo = reinterpret_cast<TRACE_EVENT_INFO *>(buffer.data());
    if (TdhGetEventInformation(eventRecord, 0, nullptr, eventInfo, &bufferSize) != ERROR_SUCCESS)
    {
        return;
    }

    const QString opcodeName = readInfoString(eventInfo, eventInfo->OpcodeNameOffset);
    const QString actionKey = actionKeyFromOpcode(opcodeName);
    if (actionKey.isEmpty())
    {
        return;
    }

    if (propertyToUInt32(eventRecord, eventInfo, L"Status", 0) != 0)
    {
        return;
    }

    const RegistryPathInfo pathInfo = buildRegistryPathInfo(eventRecord, eventInfo, actionKey);
    if (pathInfo.fullPath.isEmpty())
    {
        return;
    }

    const bool shouldContinue = (m_analysisFilter == nullptr)
                                    ? true
                                    : m_analysisFilter->ShouldTraceRegistryEvent(pathInfo.fullPath, m_riskLevel);

    RegistryEtwEvent eventInfoForUi;
    eventInfoForUi.timestamp = fileTimeToDateTime(eventRecord->EventHeader.TimeStamp);
    eventInfoForUi.actionKey = actionKey;
    eventInfoForUi.registryPath = pathInfo.fullPath;
    eventInfoForUi.registryKeyPath = pathInfo.keyPath;
    eventInfoForUi.registryKeyName = pathInfo.keyName;
    eventInfoForUi.registryValueName = pathInfo.valueName;
    eventInfoForUi.hasRegistryValue = pathInfo.hasValue;
    eventInfoForUi.processId = eventRecord->EventHeader.ProcessId;
    eventInfoForUi.riskLevel = m_riskLevel;
    eventInfoForUi.configFilePath = m_configFilePath;
    eventInfoForUi.filtered = !shouldContinue;

    if (pathInfo.hasValue && actionKey != "deleted")
    {
        eventInfoForUi.registryValueCaptured = queryRegistryValueData(
            pathInfo.keyPath,
            pathInfo.valueName,
            eventInfoForUi.registryValueData);
    }

    if (shouldContinue)
    {
        const ProcessMetadata processMetadata = (g_wmiResolver != nullptr) ? g_wmiResolver->query(eventInfoForUi.processId)
                                                                           : ProcessMetadata{fallbackProcessName(eventInfoForUi.processId), {}};
        eventInfoForUi.processName = processMetadata.processName;
        eventInfoForUi.commandLine = processMetadata.commandLine;
    }

    emit registryChanged(eventInfoForUi);
}

void WINAPI RegistryMonitor::eventRecordCallback(EVENT_RECORD *eventRecord)
{
    if (eventRecord == nullptr || eventRecord->UserContext == nullptr)
    {
        return;
    }

    auto *monitor = static_cast<RegistryMonitor *>(eventRecord->UserContext);
    monitor->handleEventRecord(eventRecord);
}
