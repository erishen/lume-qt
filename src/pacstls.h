#ifndef PACSTLS_H
#define PACSTLS_H

#include <QString>
#include <functional>

namespace qtrs {

/**
 * PACS 的 DICOM TLS 配置（DCMTK / OpenSSL）。
 *
 * 现场 PACS 的 TLS 有两种常见形态：
 *  - 双向认证（默认）：客户端要带自己的证书，服务端证书由本地 CA 校验；
 *  - 只加密不验客户端：`QTRS_PACS_TLS_VERIFY=ignore`（仅限受控网段调试）。
 *
 * 这里只负责「解析 + 校验配置」，不碰 OpenSSL；真正的传输层在 pacsclient.cpp
 * 里建。抽出来是为了能在单测里覆盖缺证书、验签级别写错这类配置错误——
 * 这些错误如果只在现场炸，表现是「连不上」，很难和网络问题分开。
 */
struct PacsTlsConfig {
    enum class Verify { Ignore, Check, Require };

    bool enabled = false;
    QString caFile;
    QString certFile;
    QString keyFile;
    Verify verify = Verify::Require;
    /// 配置不合法时的原因（enabled=false 时恒为空）。
    QString error;

    bool usable() const { return enabled && error.isEmpty(); }
    QString summary() const;
};

QString pacsTlsVerifyName(PacsTlsConfig::Verify verify);

/**
 * 从显式取值解析配置。`fileExists` 用于注入文件系统判断，单测不必真的造证书。
 * 规则：
 *  - 未启用直接返回 enabled=false；
 *  - 证书/私钥必填且必须存在；
 *  - `ignore` 以外的验签级别必须有 CA 文件且存在（否则等于没验）；
 *  - 验签级别只能是 ignore / check / require，默认 require。
 */
PacsTlsConfig pacsTlsConfigFromValues(bool enabled, const QString &caFile, const QString &certFile,
                                      const QString &keyFile, const QString &verify,
                                      const std::function<bool(const QString &)> &fileExists);

/// 读 QTRS_PACS_TLS / _CA / _CERT / _KEY / _VERIFY。
PacsTlsConfig pacsTlsConfigFromEnv();

} // namespace qtrs

#endif // PACSTLS_H
