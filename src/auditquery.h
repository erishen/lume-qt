#pragma once

/**
 * AuditQuery — 台账事件流的过滤与检索（纯函数，不碰文件系统）。
 *
 * 术中复盘要回答的是「两点十七分那次血氧报警之前，信号断过几次」这类问题，
 * 而不是把几千行 JSONL 摊在界面上。过滤只作用于已经读进内存的事件数组，
 * 读写文件、按天取哪几个分片由调用方（Bridge）决定，便于单测。
 */

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

struct AuditFilter
{
    /// 空表示不限；否则按 type 前缀匹配（"alarm." 命中 alarm.on/alarm.off/alarm.config）。
    QString typePrefix;
    /// 不区分大小写的子串匹配，范围覆盖 type 与事件里的所有标量字段（含数字）。
    QString text;
    /// 返回条数上限；<=0 时取默认 50。
    int limit = 50;

    static constexpr int kDefaultLimit = 50;
    static constexpr int kMaxLimit = 200;
};

/// 单条事件是否命中过滤条件。空过滤器恒为 true。
bool auditEventMatches(const QJsonObject &event, const AuditFilter &filter);

/**
 * 对一批事件做过滤，按输入顺序（调用方应保证「旧→新」）取最新的 limit 条并反转为「新→旧」。
 * 返回值直接给 QWebChannel，字段保持台账原文，不做裁剪。
 */
QJsonArray filterAuditEvents(const QJsonArray &events, const AuditFilter &filter);

/// 把外部传入的 limit 收进合法区间：0/负数按默认处理，避免「查全部」把界面拖死。
int clampAuditLimit(int limit);
