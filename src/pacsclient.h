#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

/**
 * PacsClient — DICOM 网络（C-ECHO / C-FIND）的最小客户端。
 *
 * 前面几步把「设备」抽象成 DeviceProvider，但那只覆盖了波形源。术中的影像与病人信息
 * 来自 PACS：能不能连上（C-ECHO）、这台设备上有没有这例检查（C-FIND），是接入真实
 * 环境时最先要回答的两个问题。这里先把这两件事做成可验证的一小块。
 *
 * 为什么是同步接口：网络调用必须在工作线程里跑，由调用方（Bridge）负责线程与 UI 隔离，
 * 这样本类不依赖 Qt 事件循环，也就能在 gtest 里直接测（见 parseEndpoint）。
 *
 * 暂未实现（后续批次）：C-GET / C-MOVE 拉取影像、TLS、AE 白名单与超时重连策略。
 */
class PacsClient
{
public:
    struct Endpoint {
        QString host;
        int port = 104;
        QString callingAe = QStringLiteral("QTRS");
        QString calledAe;
        int timeoutMs = 5000;
    };

    /// 解析 "host:port:calledAe"（也接受 "host:port"）。失败返回 false 并给出原因。
    static bool parseEndpoint(const QString &spec, Endpoint *out, QString *err);

    /// C-ECHO 连通性验证。成功时给出往返耗时（毫秒）。
    static bool echo(const Endpoint &endpoint, QString *err, int *roundTripMs = nullptr);

    /// STUDY 级 C-FIND。返回每例检查的 PatientName / StudyDate / StudyInstanceUID /
    /// NumberOfStudyRelatedSeries 等字段；列表为空但 ok=true 表示连上了、库是空的。
    static bool findStudies(const Endpoint &endpoint, QList<QVariantMap> *studies, QString *err);
};
