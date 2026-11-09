#include "pacstls.h"

#include <QFileInfo>
#include <QStringList>

namespace qtrs {

QString pacsTlsVerifyName(PacsTlsConfig::Verify verify)
{
    switch (verify) {
    case PacsTlsConfig::Verify::Ignore: return QStringLiteral("ignore");
    case PacsTlsConfig::Verify::Require: return QStringLiteral("require");
    }
    return QStringLiteral("require");
}

QString PacsTlsConfig::summary() const
{
    if (!enabled) return QStringLiteral("明文");
    if (verify == PacsTlsConfig::Verify::Ignore)
        return QStringLiteral("TLS ignore（不验服务端证书）");
    return QStringLiteral("TLS %1%2")
        .arg(pacsTlsVerifyName(verify),
             caFile.isEmpty() ? QString() : QStringLiteral("（CA %1）").arg(QFileInfo(caFile).fileName()));
}

PacsTlsConfig pacsTlsConfigFromValues(bool enabled, const QString &caFile, const QString &certFile,
                                      const QString &keyFile, const QString &verify,
                                      const std::function<bool(const QString &)> &fileExists)
{
    PacsTlsConfig cfg;
    cfg.enabled = enabled;
    cfg.caFile = caFile.trimmed();
    cfg.certFile = certFile.trimmed();
    cfg.keyFile = keyFile.trimmed();
    if (!enabled) return cfg;

    const QString level = verify.trimmed().toLower();
    if (level.isEmpty() || level == QStringLiteral("require")) {
        cfg.verify = PacsTlsConfig::Verify::Require;
    } else if (level == QStringLiteral("ignore")) {
        cfg.verify = PacsTlsConfig::Verify::Ignore;
    } else if (level == QStringLiteral("check")) {
        // DCMTK 的这个验签级别在对端不出示证书时直接放行：配了 CA 也不等于验过
        // 服务端身份。留着它等于给 PHI 链路留一个「看起来验了」的开关，直接禁用。
        cfg.error = QStringLiteral(
            "QTRS_PACS_TLS_VERIFY=check 已禁用：该级别在对端不出示证书时仍会放行，"
            "无法校验服务端证书；请改用 require（或明确知情的 ignore）");
        return cfg;
    } else {
        cfg.error = QStringLiteral("QTRS_PACS_TLS_VERIFY 只能是 ignore/require，实际：%1").arg(verify);
        return cfg;
    }

    QStringList problems;
    if (cfg.certFile.isEmpty()) problems << QStringLiteral("缺少 QTRS_PACS_TLS_CERT");
    else if (!fileExists(cfg.certFile)) problems << QStringLiteral("证书不存在：%1").arg(cfg.certFile);
    if (cfg.keyFile.isEmpty()) problems << QStringLiteral("缺少 QTRS_PACS_TLS_KEY");
    else if (!fileExists(cfg.keyFile)) problems << QStringLiteral("私钥不存在：%1").arg(cfg.keyFile);
    // ignore 模式下 CA 可有可无；require 没有 CA 就等于「连谁都信」，不允许。
    if (cfg.verify == PacsTlsConfig::Verify::Require) {
        if (cfg.caFile.isEmpty()) problems << QStringLiteral("缺少 QTRS_PACS_TLS_CA（verify=%1）")
                                                  .arg(pacsTlsVerifyName(cfg.verify));
        else if (!fileExists(cfg.caFile)) problems << QStringLiteral("CA 不存在：%1").arg(cfg.caFile);
    }
    if (!problems.isEmpty()) cfg.error = problems.join(QStringLiteral("；"));
    return cfg;
}

QString pacsTlsMoveBlockReason(const PacsTlsConfig &cfg)
{
    if (!cfg.enabled) return {};
    if (!cfg.error.isEmpty()) {
        return QStringLiteral("TLS 配置无效（%1），不允许回退到明文 C-MOVE").arg(cfg.error);
    }
    return QStringLiteral("C-MOVE 在 TLS 模式下暂不支持（接收端 TLS 未实现），请用 C-GET");
}

PacsTlsConfig pacsTlsConfigFromEnv()
{
    const QByteArray raw = qgetenv("QTRS_PACS_TLS");
    const bool enabled = !raw.isEmpty() && raw != "0" && raw.compare("false", Qt::CaseInsensitive) != 0;
    return pacsTlsConfigFromValues(enabled,
                                   qEnvironmentVariable("QTRS_PACS_TLS_CA"),
                                   qEnvironmentVariable("QTRS_PACS_TLS_CERT"),
                                   qEnvironmentVariable("QTRS_PACS_TLS_KEY"),
                                   qEnvironmentVariable("QTRS_PACS_TLS_VERIFY"),
                                   [](const QString &path) { return QFileInfo::exists(path); });
}

} // namespace qtrs
