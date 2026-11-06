#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../src/volumestats.h"

TEST(CtPreset, ClinicalWindowValues)
{
    const qtrs::WindowLevel adaptive{1234.0, 567.0};
    const qtrs::WindowLevel soft = qtrs::ctPreset(0, adaptive);
    EXPECT_DOUBLE_EQ(soft.width, 80.0);
    EXPECT_DOUBLE_EQ(soft.level, 40.0);

    const qtrs::WindowLevel bone = qtrs::ctPreset(1, adaptive);
    EXPECT_DOUBLE_EQ(bone.width, 1500.0);
    EXPECT_DOUBLE_EQ(bone.level, 300.0);

    const qtrs::WindowLevel vessel = qtrs::ctPreset(2, adaptive);
    EXPECT_DOUBLE_EQ(vessel.width, 400.0);
    EXPECT_DOUBLE_EQ(vessel.level, 100.0);

    const qtrs::WindowLevel autoW = qtrs::ctPreset(3, adaptive);
    EXPECT_DOUBLE_EQ(autoW.width, 1234.0);
    EXPECT_DOUBLE_EQ(autoW.level, 567.0);

    // 越界 preset 走软组织窗，而不是未定义行为
    const qtrs::WindowLevel clampPreset = qtrs::ctPreset(99, adaptive);
    EXPECT_DOUBLE_EQ(clampPreset.width, 80.0);
    EXPECT_DOUBLE_EQ(clampPreset.level, 40.0);
}

TEST(NonCtPreset, MapsOntoTissueSpan)
{
    const qtrs::WindowLevel soft = qtrs::nonCtPreset(0, 100.0, 1100.0);
    EXPECT_DOUBLE_EQ(soft.width, 400.0);
    EXPECT_DOUBLE_EQ(soft.level, 300.0);

    const qtrs::WindowLevel bone = qtrs::nonCtPreset(1, 100.0, 1100.0);
    EXPECT_DOUBLE_EQ(bone.width, 300.0);
    EXPECT_DOUBLE_EQ(bone.level, 850.0);

    const qtrs::WindowLevel vessel = qtrs::nonCtPreset(2, 100.0, 1100.0);
    EXPECT_DOUBLE_EQ(vessel.width, 350.0);
    EXPECT_DOUBLE_EQ(vessel.level, 600.0);

    // 跨度不足 20 时按 20 兜底，避免窗宽塌成 0 得到全噪声
    const qtrs::WindowLevel tiny = qtrs::nonCtPreset(0, 5.0, 6.0);
    EXPECT_DOUBLE_EQ(tiny.width, 8.0);
    EXPECT_DOUBLE_EQ(tiny.level, 9.0);
}

TEST(ManualWindow, ClampsToDataRange)
{
    const qtrs::WindowLevel normal = qtrs::clampManualWindow(800.0, 150.0, -1000.0, 1200.0);
    EXPECT_DOUBLE_EQ(normal.width, 800.0);
    EXPECT_DOUBLE_EQ(normal.level, 150.0);

    // 窗宽下限 1：再小就是除零/全噪声
    const qtrs::WindowLevel widthFloor = qtrs::clampManualWindow(0.0, 0.0, -1000.0, 1200.0);
    EXPECT_DOUBLE_EQ(widthFloor.width, 1.0);

    // 窗宽上限 max(4000, 2*span)：span = 2200 → 4400
    const qtrs::WindowLevel widthCeil = qtrs::clampManualWindow(999999.0, 0.0, -1000.0, 1200.0);
    EXPECT_DOUBLE_EQ(widthCeil.width, 4400.0);

    // 窗位允许越出值域一倍，但不能无限拖走
    const qtrs::WindowLevel levelLow = qtrs::clampManualWindow(80.0, -99999.0, -1000.0, 1200.0);
    EXPECT_DOUBLE_EQ(levelLow.level, -3200.0);
    const qtrs::WindowLevel levelHigh = qtrs::clampManualWindow(80.0, 99999.0, -1000.0, 1200.0);
    EXPECT_DOUBLE_EQ(levelHigh.level, 3400.0);
}

TEST(TissuePercentile, CtIgnoresAir)
{
    // 一半空气 + 一半软组织：p50 必须落在软组织上，否则窗位被拖到 -1000，整屏发白。
    std::vector<std::int16_t> hu;
    for (int i = 0; i < 600; ++i) hu.push_back(-1000);
    for (int i = 0; i < 400; ++i) hu.push_back(40);

    const qtrs::TissueRange r = qtrs::tissuePercentile(
        hu.data(), hu.size(), -1000.0, 1000.0, true);
    EXPECT_NEAR(r.lo, 40.0, 3.0);
    EXPECT_NEAR(r.hi, 40.0, 3.0);
}

TEST(TissuePercentile, MrIgnoresBackgroundZero)
{
    std::vector<std::int16_t> hu;
    for (int i = 0; i < 800; ++i) hu.push_back(0);
    for (int i = 0; i < 200; ++i) hu.push_back(500);

    const qtrs::TissueRange r = qtrs::tissuePercentile(
        hu.data(), hu.size(), 0.0, 1000.0, false);
    EXPECT_NEAR(r.lo, 500.0, 3.0);
    EXPECT_NEAR(r.hi, 500.0, 3.0);
}

TEST(TissuePercentile, AllAirFallsBackToFullRange)
{
    std::vector<std::int16_t> hu(128, -1000);
    const qtrs::TissueRange r = qtrs::tissuePercentile(
        hu.data(), hu.size(), -1000.0, 1000.0, true);
    EXPECT_DOUBLE_EQ(r.lo, -1000.0);
    EXPECT_DOUBLE_EQ(r.hi, 1000.0);
}
