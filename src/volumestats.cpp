#include "volumestats.h"

#include <algorithm>
#include <vector>

namespace qtrs {

WindowLevel ctPreset(int preset, const WindowLevel &adaptive)
{
    switch (preset) {
    case 1: // 骨窗
        return {1500.0, 300.0};
    case 2: // 血管 / 对比剂窗
        return {400.0, 100.0};
    case 3: // 数据自适应
        return adaptive;
    case 0:
    default: // 软组织窗（临床常用的 W80 / L40）
        return {80.0, 40.0};
    }
}

WindowLevel nonCtPreset(int preset, double tissueLo, double tissueHi)
{
    const double lo = tissueLo;
    const double span = std::max(20.0, tissueHi - tissueLo);
    switch (preset) {
    case 0: // 软组织窗 → 组织低段
        return {0.40 * span, lo + 0.20 * span};
    case 1: // 骨窗 → 组织高段
        return {0.30 * span, lo + 0.75 * span};
    default: // 血管窗 / 自适应 → 组织中段
        return {0.35 * span, lo + 0.50 * span};
    }
}

WindowLevel clampManualWindow(double width, double level, double huMin, double huMax)
{
    const double span = std::max(1.0, huMax - huMin);
    // 窗宽下限 1，避免除零/全噪声；上下限按数据值域留一倍余量
    const double w = std::clamp(width, 1.0, std::max(4000.0, span * 2.0));
    const double l = std::clamp(level, huMin - span, huMax + span);
    return {w, l};
}

TissueRange tissuePercentile(const std::int16_t *hu, std::size_t count, double huMin, double huMax,
                             bool isCt)
{
    constexpr int kBins = 1024;
    const double range = std::max(1e-6, huMax - huMin);
    // CT 剔空气（约 -1000）；MR 剔背景零（背景常占大半体素，会把 p50 拖到 0）。
    const double tissueFloor = isCt ? -500.0 : huMin + 0.001 * range;

    std::vector<int> hist(static_cast<std::size_t>(kBins), 0);
    int tissueCount = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(hu[i]);
        if (v < tissueFloor) continue;
        const int bin = std::clamp(
            static_cast<int>((v - huMin) / range * static_cast<double>(kBins - 1)),
            0, kBins - 1);
        hist[static_cast<std::size_t>(bin)] += 1;
        tissueCount += 1;
    }
    // 全是空气/背景（组织体素为 0）时没有可统计的组织，退回整个值域，
    // 否则分位会落到最后一个 bin，窗位被推到 huMax，画面全黑还以为是渲染坏了。
    if (tissueCount == 0) return {huMin, huMax};
    const int total = tissueCount;

    auto percentileBin = [&](int num, int den) {
        const int cut = static_cast<int>(static_cast<long long>(total) * num / den);
        int acc = 0;
        for (int i = 0; i < kBins; ++i) {
            acc += hist[static_cast<std::size_t>(i)];
            if (acc >= cut) return i;
        }
        return kBins - 1;
    };

    const double binToHu = range / static_cast<double>(kBins - 1);
    const double lo = huMin + static_cast<double>(percentileBin(isCt ? 50 : 5, 100)) * binToHu;
    const double hi = huMin + static_cast<double>(percentileBin(999, 1000)) * binToHu;
    return {lo, hi};
}

} // namespace qtrs
