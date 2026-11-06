#pragma once

#include <QString>

// 窗值偏好的键名规则。抽出来单独放，是为了能脱离 MainWindow 单测：
// 「CT 的 W1500/L300 被 MR 会话覆盖」这个 bug 的根因就是所有模态共用一套键。
namespace qtrs {

/// 已知模态时返回 "ct" / "mr"；未知时返回空（仍写旧的全局键，避免猜错桶）。
QString volumePreferenceScope(bool isCt, bool modalityKnown);

/// 例：("ct", true, "preset") → "volume/ct/preset"；
/// 模态未知时退回 "volume/preset"（升级前的旧键，保持兼容）。
QString volumePreferenceKey(bool isCt, bool modalityKnown, const QString &leaf);

} // namespace qtrs
