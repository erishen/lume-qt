#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * AlarmThresholds — 报警阈值（原生判定用）。
 *
 * 之前阈值硬编码在 NativeVitalStrip::paintEvent 里，术中想按科室口径调就得重编。
 * 这里抽成纯数据 + 纯函数：
 *   - 默认值保持与改造前一致（SpO₂ 90/92、HR 50/55），不动现有行为；
 *   - env 只能做「离线兜底」，Lume 下发优先；
 *   - 任何越界值都不静默接受——越界会让报警变钝（漏报）或变吵（误报），
 *     两者在术中都不能接受，一律退回默认并留下 warning，由调用方记台账。
 *
 * 边界说明：这些数值是演示级工程口径，不是医学阈值声明；临床值必须由
 * 科室配置与设备说明书核定后再下发。
 */
struct AlarmThresholds
{
    /// SpO₂ 低于该值 → 高优先级
    double spo2High = 90.0;
    /// SpO₂ 低于该值（且不低于 spo2High）→ 中优先级
    double spo2Mid = 92.0;
    /// HR 低于该值 → 高优先级
    double hrHigh = 50.0;
    /// HR 低于该值（且不低于 hrHigh）→ 中优先级
    double hrMid = 55.0;
};

AlarmThresholds defaultAlarmThresholds();

/// 读取 QTRS_ALARM_SPO2_HIGH / SPO2_MID / HR_HIGH / HR_MID。
/// 任何解析失败或越界的项保持原值，并把原因写进 warnings（不抛异常）。
void applyAlarmEnv(AlarmThresholds &thresholds, QStringList *warnings);

/// 从 Lume 下发的 JSON 覆盖阈值（键：spo2High/spo2Mid/hrHigh/hrMid）。
/// 缺失或非法的项沿用 fallback；返回实际生效值，warnings 说明被拒的项。
AlarmThresholds alarmThresholdsFromJson(const QJsonObject &json,
                                        const AlarmThresholds &fallback,
                                        QStringList *warnings);

/// 判定级别：0 正常 / 1 中 / 2 高。无生命体征数据时恒为 0（不能凭 0 值报警）。
int alarmLevelFor(double spo2, double hr, bool hasVitals, const AlarmThresholds &thresholds);
