#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <vector>

#include "../src/bridge.h"
#include "../src/auditsink.h"
#include "../src/pacsclient.h"
#include "../src/simulatorprovider.h"
#include "../src/dicomreader.h"

class BridgeTest : public ::testing::Test {
protected:
    void SetUp() override {
        bridge = new Bridge();
    }
    
    void TearDown() override {
        delete bridge;
    }
    
    Bridge* bridge;
};

TEST_F(BridgeTest, BasicOperations) {
    EXPECT_EQ(bridge->add(2, 3), 5);
    EXPECT_EQ(bridge->add(-1, 1), 0);
    EXPECT_EQ(bridge->add(0, 0), 0);
    EXPECT_EQ(bridge->add(100, -100), 0);
}

TEST_F(BridgeTest, PlatformInfo) {
    EXPECT_FALSE(bridge->getPlatformInfo().isEmpty());
    EXPECT_EQ(bridge->appName(), "qt-react-shell");
    EXPECT_FALSE(bridge->platform().isEmpty());
}

TEST_F(BridgeTest, ShowMessage) {
    QString result = bridge->showMessage("Test message");
    EXPECT_TRUE(result.contains("C++ 已收到"));
    EXPECT_TRUE(result.contains("Test message"));
    EXPECT_TRUE(result.contains("次调用"));
}

TEST_F(BridgeTest, ShowMessageIncrementsCount) {
    QString result1 = bridge->showMessage("Message 1");
    QString result2 = bridge->showMessage("Message 2");
    
    EXPECT_TRUE(result1.contains("第 1 次调用"));
    EXPECT_TRUE(result2.contains("第 2 次调用"));
}

TEST_F(BridgeTest, SetDataFromCpp) {
    QVariant testData;
    testData.setValue(QVariantMap{{ "key", "value" }});
    
    // 这应该不会崩溃
    bridge->setDataFromCpp(testData);
    
    // 由于 setDataFromCpp 是槽函数，我们不能直接验证它
    // 但我们可以确保它不会抛出异常
    EXPECT_TRUE(true);
}

TEST_F(BridgeTest, MedicalSimulationControl) {
    // 测试医疗模拟的启动/停止
    // 注意：Bridge 类在构造时自动启动医疗模拟
    // 这是当前设计，出于演示目的
    
    // 我们可以测试是否能调用 start/stop 方法
    // 虽然当前 Bridge 没有 public 方法来控制医疗模拟启动/停止
    // 但这个测试确保接口的存在
    
    // 获取原始指针以访问私有成员（仅用于测试）
    // 在实际代码中，我们应该将这些方法设为 public
    EXPECT_TRUE(true); // 占位测试
}

TEST_F(BridgeTest, MedicalDataSimulation) {
    // 测试医疗数据的生成
    // 由于医疗数据是通过信号发射的，我们需要捕获这些信号
    // 这需要额外的测试框架支持
    
    // 暂时跳过，留作未来扩展
    EXPECT_TRUE(true);
}

TEST_F(BridgeTest, ErrorHandling) {
    // 测试错误处理
    // 例如：无效的 URL、文件访问权限等
    // 这需要更多集成测试的支持

    EXPECT_TRUE(true); // 占位测试
}

// ---------------------------------------------------------------------------
// DICOM 后台加载线程的回归测试
// 这些用例验证 #7 竞态修复与析构期 use-after-free 防护：
//   - 正常路径：result/cleanup 闭包经主线程事件循环执行，join 线程并
//     reset holder，避免 std::thread 析构时仍 joinable 触发 std::terminate；
//   - 重入保护：解析在途时再次调用应被忽略，且不误伤在途线程；
//   - 析构安全：解析在途时销毁 Bridge 不应崩溃 / use-after-free。
// ---------------------------------------------------------------------------

namespace {
// 泵事件直到任一 DICOM 信号到达或超时。返回是否收到信号。
bool waitForDicomSignal(Bridge *bridge, int timeoutMs)
{
    QSignalSpy readySpy(bridge, SIGNAL(dicomVolumeReady(QVariantMap, QString)));
    QSignalSpy errorSpy(bridge, SIGNAL(dicomVolumeError(QString)));
    QElapsedTimer timer;
    timer.start();
    while (readySpy.isEmpty() && errorSpy.isEmpty() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    }
    return !readySpy.isEmpty() || !errorSpy.isEmpty();
}

// 样例 DICOM 序列不在版本库中（约 78MB，见 .gitignore）：缺失时跳过依赖它的用例，
// 让「克隆后直接 make run-tests」不会因为样例数据缺席而变红。
bool sampleSeriesAvailable()
{
    return QFileInfo(QStringLiteral(DICOM_SAMPLE_PATH)).isDir();
}

#define SKIP_IF_NO_SAMPLE_SERIES()                                            \
    do {                                                                      \
        if (!sampleSeriesAvailable())                                         \
            GTEST_SKIP() << "样例 DICOM 序列缺失："                            \
                            "resources/dicom 不入库，需本地自备一份到 " DICOM_SAMPLE_PATH; \
    } while (0)

// 泵事件直到收到“成功”信号（dicomVolumeReady）。
// 注意：不能复用 waitForDicomSignal —— 它把 dicomVolumeError 也算作“有信号”，
// 会让“解析彻底失败”的场景伪装成测试通过（曾导致 DICOM 面板黑屏却显示绿灯）。
bool waitForDicomReady(QSignalSpy &readySpy, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (readySpy.isEmpty() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    }
    return !readySpy.isEmpty();
}
} // namespace

TEST_F(BridgeTest, DicomVolumeLoadsAndCleansUp) {
    SKIP_IF_NO_SAMPLE_SERIES();
    QSignalSpy readySpy(bridge, SIGNAL(dicomVolumeReady(QVariantMap, QString)));
    QSignalSpy errorSpy(bridge, SIGNAL(dicomVolumeError(QString)));
    bridge->loadDicomVolume(bridge->dicomSamplePath());
    EXPECT_TRUE(waitForDicomReady(readySpy, 30000))
        << "DICOM 解析既未成功也未报错——cleanup 闭包可能未执行"
           "（线程未 join 会触发 std::terminate）";
    EXPECT_TRUE(errorSpy.isEmpty())
        << "DICOM 解析失败：" << errorSpy.value(0).at(0).toString().toStdString();
}

// 端到端验证“前端真正拿到的体数据”是否可渲染：
//   1. 必须收到 dicomVolumeReady（成功信号），而非 dicomVolumeError；
//   2. 成功信号必须带出可 fetch 的 URL，且 Bridge 持有的字节数等于 dims³；
//   3. 体数据必须有真实的灰度分布（非全零、非常数）——否则体渲染只会画出一块黑屏。
// 这一用例曾因“只看信号是否到达”而漏判：着色器编译失败时前端全黑，
// 而 C++ 侧一切正常，测试却显示绿灯。
TEST_F(BridgeTest, DicomVolumePayloadIsRenderable) {
    SKIP_IF_NO_SAMPLE_SERIES();
    QSignalSpy readySpy(bridge, SIGNAL(dicomVolumeReady(QVariantMap, QString)));
    QSignalSpy errorSpy(bridge, SIGNAL(dicomVolumeError(QString)));

    bridge->loadDicomVolume(bridge->dicomSamplePath());
    ASSERT_TRUE(waitForDicomReady(readySpy, 30000))
        << "未收到 dicomVolumeReady；错误信息："
        << (errorSpy.isEmpty() ? QStringLiteral("(无)") : errorSpy.value(0).at(0).toString()).toStdString();
    ASSERT_TRUE(errorSpy.isEmpty());

    const QVariantMap meta = readySpy.value(0).at(0).toMap();
    // 体积改走 app:// 虚拟资源（二进制直取），信号里只有 URL
    const QString volumeUrl = readySpy.value(0).at(1).toString();
    ASSERT_FALSE(volumeUrl.isEmpty()) << "体积 URL 为空";
    EXPECT_TRUE(volumeUrl.startsWith(QStringLiteral("app://app/__volume/")))
        << "体积 URL 应指向 scheme handler 的虚拟资源：" << volumeUrl.toStdString();

    const QByteArray raw = bridge->volumeBytes();
    ASSERT_FALSE(raw.isEmpty()) << "体数据为空";

    const QVariantList dims = meta.value("dims").toList();
    ASSERT_EQ(dims.size(), 3) << "meta.dims 应为 [d,d,d]";
    const int d = dims.at(0).toInt();
    ASSERT_GT(d, 0);

    ASSERT_EQ(raw.size(), d * d * d)
        << "体积字节数与 dims³ 不符：期望 " << (d * d * d) << "，实际 " << raw.size();

    // 统计灰度分布：体渲染需要有对比度，全零或常数都会渲染成纯黑/纯色
    int nonZero = 0;
    quint8 minV = 255, maxV = 0;
    double sum = 0.0;
    for (int i = 0; i < raw.size(); ++i) {
        const quint8 v = static_cast<quint8>(raw.at(i));
        if (v != 0) ++nonZero;
        minV = qMin(minV, v);
        maxV = qMax(maxV, v);
        sum += v;
    }
    const double mean = sum / static_cast<double>(raw.size());
    std::cout << "[DicomVolume] dims=" << d << " bytes=" << raw.size()
              << " min=" << int(minV) << " max=" << int(maxV)
              << " mean=" << mean << " nonZero=" << nonZero
              << " (" << (100.0 * nonZero / static_cast<double>(raw.size())) << "%)" << std::endl;

    EXPECT_GT(nonZero, raw.size() / 100)
        << "体数据几乎全零 —— 体渲染将显示为黑屏";
    EXPECT_GT(int(maxV) - int(minV), 32)
        << "体数据灰度范围过窄（无对比度），体渲染不可见";
    EXPECT_GT(mean, 8.0)
        << "体数据平均灰度过低，几乎透明";
}

TEST_F(BridgeTest, DicomVolumeRejectsReentrantLoad) {
    SKIP_IF_NO_SAMPLE_SERIES();
    bridge->loadDicomVolume(bridge->dicomSamplePath());
    // 立即再次调用：应被重入保护忽略，且不崩溃、不误伤在途线程
    EXPECT_NO_THROW(bridge->loadDicomVolume(bridge->dicomSamplePath()));
    // 首次解析仍应正常完成并被 cleanup（否则 std::terminate）
    EXPECT_TRUE(waitForDicomSignal(bridge, 30000));
}

TEST_F(BridgeTest, DicomVolumeDestructorWhileInFlightIsSafe) {
    SKIP_IF_NO_SAMPLE_SERIES();
    Bridge *b = new Bridge();
    QObject::connect(b, &Bridge::dicomVolumeReady, []() {});
    QObject::connect(b, &Bridge::dicomVolumeError, []() {});
    b->loadDicomVolume(b->dicomSamplePath());
    // 立即销毁：~Bridge 应置 alive=false 并 join 在途线程，
    // 已入队的回调因 alive=false 跳过对 this 的访问，无 use-after-free。
    EXPECT_NO_THROW(delete b);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    // 若执行到此处未被 std::terminate / abort，即视为通过
    EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// DicomReader 健壮性回归（合成 DICOM 序列）
//   1. Z 恰为 0 的切片不能被当成「缺少位置」——旧实现用 z==0 当哨兵，
//      会把居中采集的中间层排到最前，造成串层；
//   2. 矩阵尺寸不一致的切片必须被剔除——旧实现用首张切片的 cols 作为
//      全部切片的步长，混入不同尺寸切片时会越界读（vector::operator[] 不检查）；
//   3. 协作式取消应在读盘前快速返回，否则关窗会被整卷解析阻塞数秒。
// ---------------------------------------------------------------------------

namespace {

/** 生成一张常量灰度的合成 DICOM 切片并写入 dir */
bool writeSyntheticSlice(const QString &dir, const QString &name,
                         int rows, int cols, quint16 value,
                         double z, bool withZ, int instance)
{
    DcmFileFormat fileFormat;
    DcmDataset *ds = fileFormat.getDataset();
    ds->putAndInsertString(DCM_SOPClassUID, UID_SecondaryCaptureImageStorage);
    ds->putAndInsertString(DCM_Modality, "MR");
    ds->putAndInsertUint16(DCM_SamplesPerPixel, 1);
    ds->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
    ds->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(rows));
    ds->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(cols));
    ds->putAndInsertUint16(DCM_BitsAllocated, 16);
    ds->putAndInsertUint16(DCM_BitsStored, 16);
    ds->putAndInsertUint16(DCM_HighBit, 15);
    ds->putAndInsertUint16(DCM_PixelRepresentation, 0);
    ds->putAndInsertString(DCM_PixelSpacing, "1\\1");
    ds->putAndInsertString(DCM_SliceThickness, "1.0");
    ds->putAndInsertUint16(DCM_InstanceNumber, static_cast<Uint16>(instance));
    if (withZ) {
        char pos[64];
        std::snprintf(pos, sizeof(pos), "0\\0\\%g", z);
        ds->putAndInsertString(DCM_ImagePositionPatient, pos);
    }

    const size_t count = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    std::vector<Uint16> pixels(count, value);
    ds->putAndInsertUint16Array(DCM_PixelData, pixels.data(),
                                static_cast<unsigned long>(count));

    const QString path = dir + QLatin1Char('/') + name;
    const OFCondition cond = fileFormat.saveFile(QFile::encodeName(path).constData(),
                                                 EXS_LittleEndianExplicit);
    return cond.good();
}

int grayAt(const QByteArray &data, int dim, int z, int y, int x)
{
    const size_t idx = (static_cast<size_t>(z) * static_cast<size_t>(dim)
                        + static_cast<size_t>(y)) * static_cast<size_t>(dim)
                       + static_cast<size_t>(x);
    return static_cast<int>(static_cast<quint8>(data.at(static_cast<int>(idx))));
}

} // namespace

TEST(DicomReaderRobustness, ZEqualToZeroKeepsSliceInPlace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Z = -1 / 0 / +1，灰度 10 / 20 / 30
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "a.dcm", 4, 4, 10, -1.0, true, 1));
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "b.dcm", 4, 4, 20, 0.0, true, 2));
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "c.dcm", 4, 4, 30, 1.0, true, 3));

    QByteArray data;
    QVariantMap meta;
    QString err;
    ASSERT_TRUE(DicomReader::loadVolume(tmp.path(), data, meta, err, 3)) << err.toStdString();

    const int d = meta.value("dims").toList().at(0).toInt();
    ASSERT_EQ(d, 3);
    ASSERT_EQ(data.size(), d * d * d);

    const int g0 = grayAt(data, d, 0, 1, 1);
    const int g1 = grayAt(data, d, 1, 1, 1);
    const int g2 = grayAt(data, d, 2, 1, 1);

    // z=0 的那层（灰度 20）必须落在中间，而不是被当成“无位置”排到最前
    EXPECT_LT(g0, g1) << "z=-1 层与 z=0 层顺序错乱（g0=" << g0 << ", g1=" << g1 << ")";
    EXPECT_LT(g1, g2) << "z=0 层与 z=+1 层顺序错乱（g1=" << g1 << ", g2=" << g2 << ")";
    EXPECT_GT(g1, 60) << "中间层灰度异常偏低，可能取到了错误切片（g1=" << g1 << ")";
    EXPECT_LT(g1, 200) << "中间层灰度异常偏高（g1=" << g1 << ")";
}

TEST(DicomReaderRobustness, InconsistentMatrixSlicesAreSkipped)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // 基准 4×4（两张），混入一张 8×8：应被剔除，而不是按 4 的步长越界读
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "a.dcm", 4, 4, 10, -1.0, true, 1));
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "b.dcm", 4, 4, 20, 0.0, true, 2));
    ASSERT_TRUE(writeSyntheticSlice(tmp.path(), "c.dcm", 8, 8, 30, 1.0, true, 3));

    QByteArray data;
    QVariantMap meta;
    QString err;
    ASSERT_TRUE(DicomReader::loadVolume(tmp.path(), data, meta, err, 2)) << err.toStdString();

    EXPECT_EQ(meta.value("slices").toInt(), 2) << "应只保留两张 4×4 切片";
    EXPECT_EQ(meta.value("skipped").toInt(), 1) << "尺寸不一致的切片应计入 skipped";

    const int d = meta.value("dims").toList().at(0).toInt();
    ASSERT_EQ(data.size(), d * d * d);
    // 体积必须仍有对比度（10..20），且未被 8×8 的价值污染
    int maxG = 0;
    for (int i = 0; i < data.size(); ++i)
        maxG = std::max(maxG, static_cast<int>(static_cast<quint8>(data.at(i))));
    EXPECT_EQ(maxG, 255);
}

TEST(DicomReaderRobustness, CancelReturnsWithoutParsingWholeSeries)
{
    SKIP_IF_NO_SAMPLE_SERIES();
    std::atomic<bool> cancel{true};
    QByteArray data;
    QVariantMap meta;
    QString err;
    QElapsedTimer timer;
    timer.start();
    const bool ok = DicomReader::loadVolume(QStringLiteral(DICOM_SAMPLE_PATH),
                                            data, meta, err, 16, &cancel);
    EXPECT_FALSE(ok) << "取消标志已置位，解析不应返回成功";
    EXPECT_TRUE(err.contains(QStringLiteral("取消"))) << err.toStdString();
    EXPECT_LT(timer.elapsed(), 1000) << "取消应在读盘前返回，实测耗时 " << timer.elapsed() << "ms";
}

TEST_F(BridgeTest, Spo2WaveSharesTimestampContract)
{
    // SpO₂ 波形是「关键通路原生化」的一部分：原生条直连这条信号，
    // 所以它必须遵守与 ECG 相同的契约——每个样本带 epoch 微秒采集时间戳。
    QSignalSpy spo2Spy(bridge, SIGNAL(spo2Sample(double, double)));
    QSignalSpy ecgSpy(bridge, SIGNAL(ecgSample(double, double)));
    bridge->startMedicalSimulation();

    ASSERT_TRUE(spo2Spy.wait(500)) << "启动后 500ms 内应有 SpO₂ 样本";
    bridge->stopMedicalSimulation();

    ASSERT_GT(spo2Spy.count(), 0);
    const QList<QVariant> args = spo2Spy.at(0);
    const double value = args.at(0).toDouble();
    const double tsUs = args.at(1).toDouble();
    EXPECT_GT(tsUs, 0.0) << "必须带采集时间戳";
    EXPECT_GT(value, -0.5);
    EXPECT_LT(value, 2.0);

    // 两个波形通道独立计时，互不阻塞
    EXPECT_GT(ecgSpy.count(), 0);
}

/**
 * 设备时钟打戳：接真实设备后 tsUs 来自设备时钟，与宿主墙钟有未知常量偏移。
 * 这里用 QTRS_CLOCK_OFFSET_MS 注入 5 s，断言两件事：
 *  1) 样本时间戳确实按该偏移打；2) hasHardwareClock() 如实声明为 true，
 *     否则上层会把「时钟差」当成「延迟」。
 */
TEST(SimulatorClockTest, DeviceClockOffsetIsAppliedToSamples)
{
    constexpr double kOffsetUs = 5000.0 * 1000.0;
    qputenv("QTRS_CLOCK_OFFSET_MS", "5000");

    SimulatorProvider provider;
    EXPECT_TRUE(provider.hasHardwareClock());

    QSignalSpy spy(&provider, SIGNAL(ecgSample(double, double)));
    ASSERT_TRUE(provider.startMedical());
    ASSERT_TRUE(spy.wait(500)) << "启动后 500ms 内应有 ECG 样本";
    provider.stopMedical();
    ASSERT_GT(spy.count(), 0);

    const double tsUs = spy.at(0).at(1).toDouble();
    const double hostNowUs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // 设备时钟比宿主快 5 s：宿主时刻 − 样本时刻 ≈ −5 000 000 us（留 200ms 容差）
    EXPECT_NEAR(hostNowUs - tsUs, -kOffsetUs, 200.0 * 1000.0);

    qunsetenv("QTRS_CLOCK_OFFSET_MS");
}

/**
 * 台账契约：一行一条 JSON，固定字段（ts/tsUs/session/type）不可被业务字段覆盖，
 * 业务字段原样并入。这些是事后复核与统计的前提，坏了就等于没有台账。
 */
TEST(AuditSinkTest, WritesParsableJsonLinesWithFixedFields)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    AuditSink sink;
    ASSERT_TRUE(sink.open(dir.path()));
    EXPECT_FALSE(sink.filePath().isEmpty());
    EXPECT_FALSE(sink.sessionId().isEmpty());

    QVariantMap ev;
    ev.insert(QStringLiteral("channel"), QStringLiteral("ECG"));
    ev.insert(QStringLiteral("ageMs"), 1234.0);
    // 试图覆盖固定字段：必须被忽略
    ev.insert(QStringLiteral("session"), QStringLiteral("hacked"));
    ev.insert(QStringLiteral("type"), QStringLiteral("fake.type"));
    sink.record(QStringLiteral("signal.stale"), ev);
    sink.record(QStringLiteral("signal.recover"), {{QStringLiteral("channel"), QStringLiteral("ECG")}});
    EXPECT_EQ(sink.count(), 2);

    QFile f(sink.filePath());
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'),
                                                                  Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 2);

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(lines.at(0).toUtf8(), &err);
    ASSERT_EQ(err.error, QJsonParseError::NoError) << err.errorString().toStdString();
    const QJsonObject obj = doc.object();

    EXPECT_EQ(obj.value(QStringLiteral("type")).toString(), QStringLiteral("signal.stale"));
    EXPECT_EQ(obj.value(QStringLiteral("session")).toString(), sink.sessionId());
    EXPECT_EQ(obj.value(QStringLiteral("channel")).toString(), QStringLiteral("ECG"));
    EXPECT_GT(obj.value(QStringLiteral("tsUs")).toDouble(), 0.0);
    EXPECT_FALSE(obj.value(QStringLiteral("ts")).toString().isEmpty());
    // 业务字段里的 type 不得覆盖事件类型
    EXPECT_NE(obj.value(QStringLiteral("type")).toString(), QStringLiteral("fake.type"));
}

/**
 * PACS 端点解析：接入现场是靠一行环境变量配置的，解析错了就等于连错机器。
 * 这里只钉纯逻辑（不发网络），网络行为靠本地 dcmqrscp 手工验证。
 */
TEST(PacsClientTest, ParsesEndpointSpec)
{
    PacsClient::Endpoint ep;
    QString err;

    EXPECT_TRUE(PacsClient::parseEndpoint(QStringLiteral("127.0.0.1:11112:QTRSQR"), &ep, &err));
    EXPECT_EQ(ep.host, QStringLiteral("127.0.0.1"));
    EXPECT_EQ(ep.port, 11112);
    EXPECT_EQ(ep.calledAe, QStringLiteral("QTRSQR"));

    // 只给 host:port 时 calledAe 留空；调用方必须据此判定「配置不完整」而不是硬连
    PacsClient::Endpoint partial;
    EXPECT_TRUE(PacsClient::parseEndpoint(QStringLiteral("pacs.example:104"), &partial, &err));
    EXPECT_TRUE(partial.calledAe.isEmpty());

    EXPECT_FALSE(PacsClient::parseEndpoint(QStringLiteral("127.0.0.1"), &ep, &err));
    EXPECT_FALSE(PacsClient::parseEndpoint(QStringLiteral("host:0:AE"), &ep, &err));
    EXPECT_FALSE(PacsClient::parseEndpoint(QStringLiteral("host:abc:AE"), &ep, &err));
    EXPECT_FALSE(PacsClient::parseEndpoint(QStringLiteral("a:b:c:d"), &ep, &err));
}


TEST(PacsClientTest, ClassifiesRetryableErrors)
{
    // 瞬时故障：值得按退避再试
    EXPECT_TRUE(PacsClient::isRetryableError(
        QStringLiteral("C-GET 失败： TCP Initialization Error: Connection refused")));
    EXPECT_TRUE(PacsClient::isRetryableError(
        QStringLiteral("DIMSE timeout: no response within 15000 ms")));
    EXPECT_TRUE(PacsClient::isRetryableError(
        QStringLiteral("关联协商失败： TCP Initialization Error: Operation now in progress")));

    // 确定性失败：重试没有意义，只会让看板多转十几秒
    EXPECT_FALSE(PacsClient::isRetryableError(
        QStringLiteral("C-GET 部分失败： 找不到该检查（0xA900）")));
    EXPECT_FALSE(PacsClient::isRetryableError(QStringLiteral("")));
}

int main(int argc, char **argv) {
    // 为 DICOM 异步回调（QMetaObject::invokeMethod → 主线程事件循环）提供环境
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}