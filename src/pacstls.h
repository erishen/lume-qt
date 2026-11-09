#ifndef PACSTLS_H
#define PACSTLS_H

#include <QString>
#include <functional>

namespace qtrs {

/**
 * PACS 的 DICOM TLS 配置（DCMTK / OpenSSL）。
 *
 * 客户端证书/私钥由 enableAuthentication() 无条件装载；`Verify` 只控制
 * **对端（PACS 服务端）证书**怎么验，与「验不验客户端」无关：
 *  - `require`（默认）：服务端必须出示由本地 CA 签发的证书，否则握手失败；
 *  - `ignore`：只加密、完全不验服务端证书，客户端证书照发。仅限受控网段排障，
 *    中间人可冒充 PACS 接收 PHI，运行时会打 warning。
 *
 * `check` 已从合法取值中移除：DCMTK 的该级别在「对端不出示证书」时直接放行，
 * 配了 CA 也不等于验过服务端身份，属于 fail-open，不能出现在 PHI 链路上。
 *
 * 这里只负责「解析 + 校验配置」，不碰 OpenSSL；真正的传输层在 pacsclient.cpp
 * 里建。抽出来是为了能在单测里覆盖缺证书、验签级别写错这类配置错误——
 * 这些错误如果只在现场炸，表现是「连不上」，很难和网络问题分开。
 */
struct PacsTlsConfig {
    enum class Verify { Ignore, Require };

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
 *  - `require` 必须有 CA 文件且存在（否则等于没验）；
 *  - 验签级别只能是 ignore / require，默认 require；`check` 明确拒绝。
 */
PacsTlsConfig pacsTlsConfigFromValues(bool enabled, const QString &caFile, const QString &certFile,
                                      const QString &keyFile, const QString &verify,
                                      const std::function<bool(const QString &)> &fileExists);

/**
 * C-MOVE 的前置拒绝理由；返回空串表示允许走明文接收 SCP。
 *
 * 本机临时 Storage SCP 没有 TLS 层，所以只要声明启用 TLS 就必须拒绝，
 * 且判定必须早于创建取回目录和 openListenPort()——不能「先开明文入口再报错」。
 * 配置本身不合法时同样拒绝：不能因为证书配错就悄悄退回明文 C-MOVE。
 */
QString pacsTlsMoveBlockReason(const PacsTlsConfig &cfg);

/// 读 QTRS_PACS_TLS / _CA / _CERT / _KEY / _VERIFY。
PacsTlsConfig pacsTlsConfigFromEnv();

} // namespace qtrs

#endif // PACSTLS_H
