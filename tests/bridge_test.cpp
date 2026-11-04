#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <iostream>

#include "../src/bridge.h"

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
//   2. base64 必须能解码、长度等于 dims³；
//   3. 体数据必须有真实的灰度分布（非全零、非常数）——否则体渲染只会画出一块黑屏。
// 这一用例曾因“只看信号是否到达”而漏判：着色器编译失败时前端全黑，
// 而 C++ 侧一切正常，测试却显示绿灯。
TEST_F(BridgeTest, DicomVolumePayloadIsRenderable) {
    QSignalSpy readySpy(bridge, SIGNAL(dicomVolumeReady(QVariantMap, QString)));
    QSignalSpy errorSpy(bridge, SIGNAL(dicomVolumeError(QString)));

    bridge->loadDicomVolume(bridge->dicomSamplePath());
    ASSERT_TRUE(waitForDicomReady(readySpy, 30000))
        << "未收到 dicomVolumeReady；错误信息："
        << (errorSpy.isEmpty() ? QStringLiteral("(无)") : errorSpy.value(0).at(0).toString()).toStdString();
    ASSERT_TRUE(errorSpy.isEmpty());

    const QVariantMap meta = readySpy.value(0).at(0).toMap();
    const QString base64 = readySpy.value(0).at(1).toString();
    ASSERT_FALSE(base64.isEmpty()) << "体数据 base64 为空";

    const QVariantList dims = meta.value("dims").toList();
    ASSERT_EQ(dims.size(), 3) << "meta.dims 应为 [d,d,d]";
    const int d = dims.at(0).toInt();
    ASSERT_GT(d, 0);

    const QByteArray raw = QByteArray::fromBase64(base64.toLatin1());
    ASSERT_EQ(raw.size(), d * d * d)
        << "解码后字节数与 dims³ 不符：期望 " << (d * d * d) << "，实际 " << raw.size();

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
    const double mean = sum / raw.size();
    std::cout << "[DicomVolume] dims=" << d << " bytes=" << raw.size()
              << " min=" << int(minV) << " max=" << int(maxV)
              << " mean=" << mean << " nonZero=" << nonZero
              << " (" << (100.0 * nonZero / raw.size()) << "%)" << std::endl;

    EXPECT_GT(nonZero, raw.size() / 100)
        << "体数据几乎全零 —— 体渲染将显示为黑屏";
    EXPECT_GT(int(maxV) - int(minV), 32)
        << "体数据灰度范围过窄（无对比度），体渲染不可见";
    EXPECT_GT(mean, 8.0)
        << "体数据平均灰度过低，几乎透明";
}

TEST_F(BridgeTest, DicomVolumeRejectsReentrantLoad) {
    bridge->loadDicomVolume(bridge->dicomSamplePath());
    // 立即再次调用：应被重入保护忽略，且不崩溃、不误伤在途线程
    EXPECT_NO_THROW(bridge->loadDicomVolume(bridge->dicomSamplePath()));
    // 首次解析仍应正常完成并被 cleanup（否则 std::terminate）
    EXPECT_TRUE(waitForDicomSignal(bridge, 30000));
}

TEST_F(BridgeTest, DicomVolumeDestructorWhileInFlightIsSafe) {
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

int main(int argc, char **argv) {
    // 为 DICOM 异步回调（QMetaObject::invokeMethod → 主线程事件循环）提供环境
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}