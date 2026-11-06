#pragma once

#include <cstddef>
#include <cstdint>

// 体渲染里「看哪一层组织」的纯计算部分。抽出来是为了能脱离 GL / Qt 单测：
// 窗位窗宽的预设换算、自适应分位、手动调窗的边界，都曾因为算错而整屏发白或全黑。
namespace qtrs {

struct WindowLevel {
    double width;
    double level;
};

struct TissueRange {
    double lo;
    double hi;
};

// CT 的四个临床档（0 软组织 / 1 骨 / 2 血管 / 3 自适应）。
// adaptive 由 tissuePercentile 的结果算出，预设 3 直接返回它。
WindowLevel ctPreset(int preset, const WindowLevel &adaptive);

// 非 CT 没有 HU 语义，把 CT 的四档换算到组织强度跨度上，保证「切得动」。
WindowLevel nonCtPreset(int preset, double tissueLo, double tissueHi);

// 手动调窗的边界：窗宽 [1, max(4000, 2*span)]，窗位 [min-span, max+span]。
WindowLevel clampManualWindow(double width, double level, double huMin, double huMax);

// 组织强度分位（CT 剔空气 < -500 HU；非 CT 剔背景）。返回 lo/hi 两端。
TissueRange tissuePercentile(const std::int16_t *hu, std::size_t count, double huMin, double huMax,
                             bool isCt);

} // namespace qtrs
