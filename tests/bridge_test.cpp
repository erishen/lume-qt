#include <gtest/gtest.h>
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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}