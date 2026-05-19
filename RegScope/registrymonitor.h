#ifndef REGISTRYMONITOR_H
#define REGISTRYMONITOR_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <windows.h>
#include <evntrace.h>

class RegShot;

struct RegistryEtwEvent
{
    QDateTime timestamp;
    QString actionKey;
    QString registryPath;
    QString registryKeyPath;
    QString registryKeyName;
    QString registryValueName;
    QString registryValueData;
    QString processName;
    QString commandLine;
    QString configFilePath;
    DWORD processId = 0;
    int riskLevel = 1;
    bool filtered = false;
    bool hasRegistryValue = false;
    bool registryValueCaptured = false;
};

Q_DECLARE_METATYPE(RegistryEtwEvent)

class RegistryMonitor : public QObject
{
    Q_OBJECT

public:
    explicit RegistryMonitor(QObject *parent = nullptr);
    ~RegistryMonitor() override;

    bool startMonitoring(int riskLevel, const QString &configFilePath);
    void stopMonitoring();
    bool isRunning() const;

signals:
    void registryChanged(const RegistryEtwEvent &eventInfo);
    void monitorStarted();
    void monitorStopped();
    void monitorError(const QString &details);

private:
    void runTraceSession(int riskLevel, const QString &configFilePath);
    void handleEventRecord(EVENT_RECORD *eventRecord);
    static void WINAPI eventRecordCallback(EVENT_RECORD *eventRecord);

    mutable std::mutex m_stateMutex;
    std::thread m_worker;
    std::atomic_bool m_running;
    std::atomic_bool m_stopRequested;
    std::wstring m_sessionName;
    TRACEHANDLE m_sessionHandle;
    TRACEHANDLE m_traceHandle;
    int m_riskLevel;
    QString m_configFilePath;
    std::unique_ptr<RegShot> m_analysisFilter;
};

#endif // REGISTRYMONITOR_H
