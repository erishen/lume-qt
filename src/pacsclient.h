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

    /// STUDY 级 C-GET：把整例检查取回本地目录（同关联 C-STORE，无需另开监听端口）。
    /// 成功后 fileCount 为落地文件数；失败时 err 带 DCMTK 原始原因。
    /**
     * 判断一次失败是否值得重试。
     *
     * 只有「连不上 / 超时 / 关联被打断」这类瞬时故障才重试；「库里没有这例」
     * 是确定性结论，重试只会让看板多转十几秒还给不出答案。
     */
    static bool isRetryableError(const QString &message);

    /**
     * C-MOVE 取回：适用于只支持 C-MOVE 的 PACS。
     *
     * 与 C-GET 相反，这里由服务端反向连到我们，所以本函数会在 `destPort` 上临时
     * 起一个存储 SCP（AE = `destAe`），收到的对象直接落盘到 `destDir`。
     * 前提：PACS 的 AE 表里必须登记了 `destAe` 与它可达的 host/port。
     */
    static bool moveStudy(const Endpoint &endpoint, const QString &studyUid,
                          const QString &destAe, int destPort, const QString &destDir,
                          int *fileCount, QString *err);

    static bool retrieveStudy(const Endpoint &endpoint, const QString &studyUid,
                              const QString &destDir, int *fileCount, QString *err);
};
