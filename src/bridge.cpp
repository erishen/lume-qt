#include "bridge.h"
#include "deviceprovider.h"
#include "dicomreader.h"
#include "simulatorprovider.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QSysInfo>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// 随机数辅助
// ---------------------------------------------------------------------------

double Bridge::nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

Bridge::Bridge(QObject *parent)
    : QObject(parent)
{
    // 设备接入层：目前只有内置模拟器。换成串口监护仪 / 网口监护仪 / DICOM 网络时，
    // 实现同一个 DeviceProvider 接口并在这里替换即可，前端契约与原生条都不用动。
    auto *provider = new SimulatorProvider(this);
    m_provider = provider;

    // Bridge 只做转发：设备发出的每个样本原样进 QWebChannel 与原生条。
    // 时间戳由 provider 在采集时刻打，转发过程不重新计时。
    connect(provider, &DeviceProvider::ecgSample, this, &Bridge::ecgSample);
    connect(provider, &DeviceProvider::spo2Sample, this, &Bridge::spo2Sample);
    connect(provider, &DeviceProvider::vitalsReady, this, &Bridge::vitalsReady);
    connect(provider, &DeviceProvider::eegSample, this, &Bridge::eegSample);
    connect(provider, &DeviceProvider::emgSample, this, &Bridge::emgSample);
    connect(provider, &DeviceProvider::neuralSpike, this, &Bridge::neuralSpike);
    connect(provider, &DeviceProvider::interventionReady, this, &Bridge::interventionReady);

    qInfo().noquote() << "[Bridge] provider:" << provider->kind() << "·" << provider->description()
                      << "· 硬件时基:" << (provider->hasHardwareClock() ? "是" : "否（宿主墙钟）");
}

Bridge::~Bridge()
{
    // 若 DICOM 解析仍在进行：先置 alive=false（位于堆上、独立于 this），
    // 再置 cancel=true 让 worker 在下一个边界退出，最后 join。
    // 已入队的回调会因 alive=false 跳过对 this 的访问，避免 Bridge 释放后的
    // use-after-free；cancel 让 join 不必等整卷解析完（否则关窗会卡住数秒）。
    if (m_dicomHolder) {
        m_dicomHolder->alive = false;
        m_dicomHolder->cancel = true;
        if (m_dicomHolder->thread && m_dicomHolder->thread->joinable())
            m_dicomHolder->thread->join();
        // 释放本副本；若仍有已入队闭包持有 holder 副本，它会存活到那些
        // 闭包执行完毕（此时 alive 已为 false，不会访问 this）。
    }
}

void Bridge::loadDicomVolume(const QString &directory)
{
    // 重入保护：只允许一个在途 DICOM 解析（UI 实际只触发一次，此处仅为
    // 健壮性兜底；如需排队可改为任务队列实现）。
    if (m_dicomHolder) {
        qWarning() << "[Bridge] DICOM 解析已在进行中，忽略重复请求：" << directory;
        return;
    }

    qInfo().noquote() << "[Bridge] 开始后台解析 DICOM：" << directory;

    // holder 位于堆上，worker 闭包与收尾闭包各持一份 shared_ptr 副本：
    //  - 收尾闭包仅操作 holder->thread，绝不读成员指针，因此不会被之后
    //    新建的线程误伤（修 #7 竞态）；
    //  - alive 标志独立于 Bridge，Bridge 析构后仍可被已入队回调安全读取。
    auto holder = std::make_shared<DicomThreadHolder>();
    holder->alive = true;
    holder->thread = std::make_unique<std::thread>([this, directory, holder]() {
        QByteArray data;
        QVariantMap meta;
        QString err;
        const bool ok = DicomReader::loadVolume(directory, data, meta, err, 128, &holder->cancel);

        // 结果必须回到 Qt 主线程再发射信号（QWebChannel 依赖所在线程）。
        // 闭包通过 holder->alive 判断 Bridge 是否仍存活，避免析构后
        // 对已释放 this 的访问（use-after-free）。
        if (ok) {
            meta.insert(QStringLiteral("tsUs"), nowMicros());
            QMetaObject::invokeMethod(this, [this, holder, meta, data]() {
                if (!holder->alive) return;
                // 体积数据存进成员，由 AppSchemeHandler 以二进制供数；
                // 信号只传 URL，避免 2MB 二进制走文本序列化。
                m_volumeBytes = data;
                emit dicomVolumeReady(meta, QStringLiteral("app://app/__volume/current.bin"));
            });
        } else {
            QMetaObject::invokeMethod(this, [this, holder, err]() {
                if (holder->alive) emit dicomVolumeError(err);
            });
        }

        // 回到主线程收尾：仅 join/释放“本线程”对象（holder->thread），
        // 不读成员指针，因此即便期间又发起了新的 loadDicomVolume，
        // 也不会误伤新线程。
        QMetaObject::invokeMethod(this, [this, holder]() {
            if (!holder->alive) return;   // Bridge 已析构，跳过（不访问 this）
            if (holder->thread && holder->thread->joinable())
                holder->thread->join();
            if (m_dicomHolder == holder)
                m_dicomHolder.reset();
        });
    });

    m_dicomHolder = holder;
}

QString Bridge::appName() const
{
    return QStringLiteral("qt-react-shell");
}

QString Bridge::platform() const
{
    return QSysInfo::prettyProductName();
}

QString Bridge::getPlatformInfo() const
{
    return QStringLiteral("%1 — %2 (Qt %3)")
        .arg(platform(),
             QSysInfo::currentCpuArchitecture(),
             QString::fromLatin1(qVersion()));
}

QByteArray Bridge::volumeBytes() const
{
    return m_volumeBytes;
}

QString Bridge::dicomSamplePath() const
{
    return QStringLiteral(DICOM_SAMPLE_PATH);
}

QString Bridge::showMessage(const QString &message)
{
    ++m_callCount;
    qInfo() << "[Bridge] showMessage from JS:" << message;

    // 演示 JS → C++ 返回值（C++ 可加工后回传）
    return QStringLiteral("C++ 已收到（第 %1 次调用）: %2").arg(m_callCount).arg(message);
}

int Bridge::add(int a, int b)
{
    return a + b;
}

void Bridge::setDataFromCpp(const QVariant &data)
{
    qInfo() << "[Bridge] setDataFromCpp from JS:" << data;

    // 回推一个带时间戳的数据，演示 C++ → JS 主动推送
    QVariantMap payload;
    payload.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
    payload.insert(QStringLiteral("echo"), data);
    payload.insert(QStringLiteral("seq"), m_callCount);
    emit dataFromCpp(payload);
}

// ---------------------------------------------------------------------------
// 医疗影像实时数据模拟
// ---------------------------------------------------------------------------

void Bridge::startMedicalSimulation()
{
    if (!m_provider) return;
    if (m_provider->startMedical()) {
        qInfo().noquote() << "[Bridge] 启动医疗数据源:" << m_provider->description();
    }
}

void Bridge::stopMedicalSimulation()
{
    if (!m_provider) return;
    m_provider->stopMedical();
    qInfo() << "[Bridge] 停止医疗数据源";
}

// ---------------------------------------------------------------------------
// 神经内科介入数据模拟
// ---------------------------------------------------------------------------

void Bridge::startNeurologySimulation()
{
    if (!m_provider) return;
    if (m_provider->startNeurology()) {
        qInfo().noquote() << "[Bridge] 启动神经内科数据源:" << m_provider->description();
    }
}

void Bridge::stopNeurologySimulation()
{
    if (!m_provider) return;
    m_provider->stopNeurology();
    qInfo() << "[Bridge] 停止神经内科数据源";
}

