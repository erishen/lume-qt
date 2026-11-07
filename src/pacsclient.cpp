#include "pacsclient.h"

#include "pacstls.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/dcmnet/dstorscp.h>
#include <dcmtk/dcmnet/scp.h>
#include <dcmtk/dcmnet/scu.h>
// DcmSCU::useSecureConnection() 在本机 DCMTK 3.7.1 上会报
// "ASC Caller passed in a NULL key"（层里 key/cert 都加载成功、单独探针也匹配），
// 只有 DcmTLSSCU 这条老路径能真正建连；DCMTK 已标记它为 deprecated，
// 这里明确接受这点并用 pragma 抑制告警，等上游修好再换回新 API。
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#ifdef WITH_OPENSSL
#include <dcmtk/dcmtls/tlsscu.h>
#endif

namespace {

constexpr QFileDevice::Permissions kOwnerDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions kOwnerFile =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner;

/// PACS 落盘是完整 DICOM（含姓名/ID/检查信息），按 PHI 处理：
/// 检查目录 700；如果父目录就是应用的 pacs/ 根，也一并收成 700。
void securePacsDir(const QString &destDir)
{
    if (destDir.isEmpty())
        return;
    QFile::setPermissions(destDir, kOwnerDir);
    const QFileInfo info(destDir);
    const QDir parent = info.dir();
    if (parent.dirName() == QStringLiteral("pacs"))
        QFile::setPermissions(parent.absolutePath(), kOwnerDir);
}

void securePacsFiles(const QString &destDir)
{
    const QDir dir(destDir);
    for (const QFileInfo &info : dir.entryInfoList(QDir::Files, QDir::Name)) {
        if (info.fileName().startsWith(QLatin1Char('.')))
            continue;
        QFile::setPermissions(info.absoluteFilePath(), kOwnerFile);
    }
}

/// C-MOVE 的接收端：PACS 会主动连过来把影像推给我们（我们在这里是 Storage SCP）。
/// 让它直接落盘并统一补 .dcm 后缀，这样下游解析器和 C-GET 走同一套命名。
class MoveSinkScp : public DcmStorageSCP
{
public:
    int received() const { return m_received.load(); }

    /// 外部（发 C-MOVE 的线程）在收到最终响应后调用，让监听循环尽快收尾。
    void requestStop() { (void)stopAfterCurrentAssociation(); }

protected:
    /// 非阻塞模式下没有关联进来时，按连接超时退出监听，避免线程永远挂着。
    OFBool stopAfterConnectionTimeout() override { return OFTrue; }

    void notifyInstanceStored(const OFString &filename, const OFString &sopClassUID,
                              const OFString &sopInstanceUID,
                              DcmDataset *dataset = nullptr) const override
    {
        DcmStorageSCP::notifyInstanceStored(filename, sopClassUID, sopInstanceUID, dataset);
        ++m_received;
    }

private:
    mutable std::atomic<int> m_received{0};
};

/// 按环境变量决定这次关联用普通 SCU 还是 TLS SCU。
/// DcmTLSSCU 是 DcmSCU 的子类，其余调用点不用分叉。
std::unique_ptr<DcmSCU> makeScu()
{
#ifdef WITH_OPENSSL
    if (qtrs::pacsTlsConfigFromEnv().enabled) return std::make_unique<DcmTLSSCU>();
#endif
    return std::make_unique<DcmSCU>();
}

/// 把 DcmSCU 会话建起来：注册 AE、设置超时、（可选）挂 TLS、发起关联。
/// 每个操作都开一个新的关联再关闭，避免长连接在术中网络抖动时留下半死状态。
bool openAssociation(DcmSCU &scu, const PacsClient::Endpoint &endpoint, QString *err)
{
    scu.setAETitle(endpoint.callingAe.toStdString());
    scu.setPeerHostName(endpoint.host.toStdString());
    scu.setPeerPort(static_cast<Uint16>(endpoint.port));
    scu.setPeerAETitle(endpoint.calledAe.toStdString());

    // DCMTK 的超时单位是秒；这里统一按毫秒配置后再换算，避免调用方记两套单位。
    const Uint16 seconds = static_cast<Uint16>(
        endpoint.timeoutMs > 0 ? (endpoint.timeoutMs + 999) / 1000 : 5);
    scu.setConnectionTimeout(static_cast<Sint32>(seconds));
    scu.setACSETimeout(static_cast<Uint32>(seconds));
    scu.setDIMSETimeout(static_cast<Uint32>(seconds));
    scu.setVerbosePCMode(OFFalse);

    const qtrs::PacsTlsConfig cfg = qtrs::pacsTlsConfigFromEnv();
#ifdef WITH_OPENSSL
    DcmTLSSCU *tlsScu = dynamic_cast<DcmTLSSCU *>(&scu);
#else
    DcmTLSSCU *tlsScu = nullptr;
#endif
    if (cfg.enabled) {
        if (!cfg.usable()) {
            if (err) *err = QStringLiteral("TLS 配置无效：%1").arg(cfg.error);
            return false;
        }
        if (!tlsScu) {
            if (err) *err = QStringLiteral("内部错误：启用了 TLS 但 SCU 不是 DcmTLSSCU");
            return false;
        }
        // 第三个参数必须给空串：DcmTLSSCU 默认（NULL）会从 stdin 要私钥口令，
        // 桌面 GUI 没有 stdin，会在建连前挂住。
        tlsScu->enableAuthentication(cfg.keyFile.toStdString(), cfg.certFile.toStdString(), "",
                                     DCF_Filetype_PEM, DCF_Filetype_PEM);
        if (cfg.verify != qtrs::PacsTlsConfig::Verify::Ignore) {
            tlsScu->addTrustedCertFile(cfg.caFile.toStdString());
        }
        switch (cfg.verify) {
        case qtrs::PacsTlsConfig::Verify::Ignore:
            tlsScu->setPeerCertVerification(DCV_ignoreCertificate);
            break;
        case qtrs::PacsTlsConfig::Verify::Check:
            tlsScu->setPeerCertVerification(DCV_checkCertificate);
            break;
        case qtrs::PacsTlsConfig::Verify::Require:
            tlsScu->setPeerCertVerification(DCV_requireCertificate);
            break;
        }
        qInfo().noquote() << QStringLiteral("[PACS] TLS 已启用：%1").arg(cfg.summary());
    }

    OFCondition cond = scu.initNetwork();
    if (cond.bad()) {
        if (err) *err = QStringLiteral("initNetwork 失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
    }
    if (tlsScu) {
        // DcmTLSSCU 要求套件配置在 initNetwork 之后（层已建好）再设。
        const OFCondition tcond = tlsScu->setTLSProfile(TSP_Profile_BCP_195_RFC_8996);
        if (tcond.bad()) {
            if (err) *err = QStringLiteral("设置 TLS 套件失败：%1").arg(QString::fromStdString(tcond.text()));
            return false;
        }
    }
    cond = scu.negotiateAssociation();
    if (cond.bad()) {
        if (err) *err = QStringLiteral("关联协商失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
    }
    return true;
}

} // namespace

bool PacsClient::parseEndpoint(const QString &spec, Endpoint *out, QString *err)
{
    if (!out) return false;
    const QStringList parts = spec.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.size() > 3) {
        if (err) *err = QStringLiteral("格式应为 host:port[:calledAE]，实际：%1").arg(spec);
        return false;
    }
    bool ok = false;
    const int port = parts.at(1).toInt(&ok);
    if (!ok || port <= 0 || port > 65535) {
        if (err) *err = QStringLiteral("端口无效：%1").arg(parts.at(1));
        return false;
    }
    out->host = parts.at(0).trimmed();
    out->port = port;
    if (parts.size() == 3 && !parts.at(2).trimmed().isEmpty()) {
        out->calledAe = parts.at(2).trimmed();
    }
    return true;
}

bool PacsClient::echo(const Endpoint &endpoint, QString *err, int *roundTripMs)
{
    std::unique_ptr<DcmSCU> scu = makeScu();
    // C-ECHO 也需要先声明 Verification 呈现上下文，否则 initNetwork 直接报
    // "No Presentation Contexts defined"——这是 DcmSCU 与 dcmnet 底层 API 的区别，
    // 不显式加就没有任何上下文可协商。
    OFList<OFString> echoXfer;
    echoXfer.push_back(UID_LittleEndianExplicitTransferSyntax);
    echoXfer.push_back(UID_LittleEndianImplicitTransferSyntax);
    const OFCondition pc = scu->addPresentationContext(UID_VerificationSOPClass, echoXfer);
    if (pc.bad()) {
        if (err) *err = QStringLiteral("注册 C-ECHO 呈现上下文失败：%1")
                             .arg(QString::fromStdString(pc.text()));
        return false;
    }
    if (!openAssociation(*scu, endpoint, err)) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    const OFCondition cond = scu->sendECHORequest(0);
    const int ms = static_cast<int>(timer.elapsed());
    scu->closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

    if (cond.bad()) {
        if (err) *err = QStringLiteral("C-ECHO 失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
    }
    if (roundTripMs) *roundTripMs = ms;
    return true;
}

bool PacsClient::isRetryableError(const QString &message)
{
    if (message.isEmpty()) return false;
    const QString lower = message.toLower();
    // 确定性失败：不存在 / 无匹配，重试没有意义
    if (lower.contains(QStringLiteral("no such")) || lower.contains(QStringLiteral("找不到"))
        || lower.contains(QStringLiteral("不存在")) || lower.contains(QStringLiteral("0xa900"))) {
        return false;
    }
    return lower.contains(QStringLiteral("tcp")) || lower.contains(QStringLiteral("timeout"))
        || lower.contains(QStringLiteral("超时")) || lower.contains(QStringLiteral("association"))
        || lower.contains(QStringLiteral("关联")) || lower.contains(QStringLiteral("dimse"))
        || lower.contains(QStringLiteral("connection")) || lower.contains(QStringLiteral("连接"));
}

bool PacsClient::moveStudy(const Endpoint &endpoint, const QString &studyUid,
                           const QString &destAe, int destPort, const QString &destDir,
                           int *fileCount, QString *err)
{
    if (studyUid.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("缺少 StudyInstanceUID，无法取回");
        return false;
    }
    if (destAe.trimmed().isEmpty() || destPort <= 0 || destPort > 65535) {
        if (err) *err = QStringLiteral("C-MOVE 目标 AE 或端口无效：%1:%2")
                             .arg(destAe).arg(destPort);
        return false;
    }
    if (!QDir().mkpath(destDir)) {
        if (err) *err = QStringLiteral("无法创建取回目录：%1").arg(destDir);
        return false;
    }
    securePacsDir(destDir);

    // 先把本机的存储 SCP 挂起来：C-MOVE 是「服务端反向连我们」，
    // 它必须先能接受关联，否则 PACS 那边只会报 move destination unreachable。
    MoveSinkScp scp;
    scp.setPort(static_cast<Uint16>(destPort));
    scp.setAETitle(destAe.toStdString());
    scp.setOutputDirectory(QDir(destDir).absolutePath().toStdString());
    scp.setFilenameExtension(".dcm");
    scp.setDatasetStorageMode(DcmStorageSCP::DGM_StoreBitPreserving);
    // DcmStorageSCP 的头文件写得很直白：不加载协商档，它只认 Verification，
    // 于是 openListenPort() 直接报 Invalid or non-existing SCP Association Profile。
    // 把存储类写进一份临时协商档，让接收端在关联协商阶段就能接住 PACS 推来的图。
    const QString assocPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                              + QStringLiteral("/qtrs-store-scp.cfg");
    {
        QFile assoc(assocPath);
        if (!assoc.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (err) *err = QStringLiteral("无法写入接收端协商档：%1").arg(assocPath);
            return false;
        }
        assoc.write(R"([[TransferSyntaxes]]
[Uncompressed]
TransferSyntax1 = LittleEndianExplicit
TransferSyntax2 = LittleEndianImplicit

[[PresentationContexts]]
[StorageSCP]
PresentationContext1 = VerificationSOPClass\Uncompressed
PresentationContext2 = MRImageStorage\Uncompressed
PresentationContext3 = CTImageStorage\Uncompressed
PresentationContext4 = EnhancedMRImageStorage\Uncompressed
PresentationContext5 = SecondaryCaptureImageStorage\Uncompressed

[[Profiles]]
[StorageSCP]
PresentationContexts = StorageSCP
)");
        assoc.close();
    }
    if (scp.loadAssociationConfiguration(assocPath.toStdString(), "StorageSCP").bad()) {
        if (err) *err = QStringLiteral("接收端协商档无效：%1").arg(assocPath);
        return false;
    }
    scp.setConnectionBlockingMode(DUL_NOBLOCK);
    // 监听循环在没有新关联时会一直等到连接超时才退出：这个值既是
    // 「两次关联之间最多等多久」，也决定了收完后多久能收工，取 3 s。
    scp.setConnectionTimeout(3);

    const OFCondition bindRc = scp.openListenPort();
    if (qtrs::pacsTlsConfigFromEnv().enabled) {
        // C-MOVE 是「服务端反向连回我们的存储 SCP」，那条链路也得 TLS + 双向认证。
        // 客户端这半边能加密、接收端还没挂 TLS 层，硬上就是半吊子，明确拒绝。
        if (err) *err = QStringLiteral("C-MOVE 在 TLS 模式下暂不支持（接收端 TLS 未实现），请用 C-GET");
        return false;
    }
    if (bindRc.bad()) {
        if (err) *err = QStringLiteral("无法监听本机接收端口 %1：%2")
                             .arg(destPort).arg(bindRc.text());
        return false;
    }

    std::thread scpThread([&scp]() { (void)scp.acceptAssociations(); });

    std::unique_ptr<DcmSCU> scu = makeScu();
    OFList<OFString> xfer;
    xfer.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfer.push_back(UID_LittleEndianImplicitTransferSyntax);

    bool ok = false;
    QString localErr;
    int completed = 0;
    auto finish = [&]() {
        // 取回结束（成功或失败）都要让接收线程退出并回收，
        // 否则下次取回会撞在同一端口上。
        scp.requestStop();
        if (scpThread.joinable()) scpThread.join();
    };

    if (scu->addPresentationContext(UID_MOVEStudyRootQueryRetrieveInformationModel, xfer,
                                   ASC_SC_ROLE_SCU)
            .bad()) {
        localErr = QStringLiteral("注册 C-MOVE 呈现上下文失败");
        finish();
        if (err) *err = localErr;
        return false;
    }

    if (!openAssociation(*scu, endpoint, &localErr)) {
        finish();
        if (err) *err = localErr;
        return false;
    }

    const T_ASC_PresentationContextID presId =
        scu->findPresentationContextID(UID_MOVEStudyRootQueryRetrieveInformationModel, "");
    if (presId == 0) {
        localErr = QStringLiteral("服务端未接受 C-MOVE 呈现上下文");
        (void)scu->releaseAssociation();
        finish();
        if (err) *err = localErr;
        return false;
    }

    DcmDataset query;
    query.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    query.putAndInsertString(DCM_StudyInstanceUID, studyUid.toUtf8().constData());

    OFList<RetrieveResponse *> responses;
    const OFCondition rc = scu->sendMOVERequest(presId, destAe.toStdString(), &query, &responses);

    // 取最终状态里的完成数：中间响应只会给「还剩几个」，最后一个才是结论。
    if (!responses.empty()) {
        completed = static_cast<int>(responses.back()->m_numberOfCompletedSubops);
    }
    const int failed = responses.empty() ? 0
                                         : static_cast<int>(responses.back()->m_numberOfFailedSubops);
    for (RetrieveResponse *r : responses) delete r;
    responses.clear();

    (void)scu->releaseAssociation();
    finish();

    const int landed = static_cast<int>(
        QDir(destDir).entryList({QStringLiteral("*.dcm")}, QDir::Files).size());
    securePacsFiles(destDir);
    if (fileCount) *fileCount = landed;

    if (landed < completed) {
        localErr = QStringLiteral("C-MOVE 部分失败：只收到 %1/%2 张（失败 %3）")
                       .arg(landed).arg(completed).arg(failed);
        if (err) *err = localErr;
        return false;
    }

    if (rc.bad() || landed == 0) {
        localErr = rc.bad() ? QStringLiteral("C-MOVE 失败：%1").arg(rc.text())
                            : QStringLiteral("C-MOVE 结束但没有文件落地（完成 %1，失败 %2）")
                                  .arg(completed).arg(failed);
        if (err) *err = localErr;
        return false;
    }

    ok = true;
    return ok;
}

bool PacsClient::retrieveStudy(const Endpoint &endpoint, const QString &studyUid,
                               const QString &destDir, int *fileCount, QString *err)
{
    if (studyUid.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("缺少 StudyInstanceUID，无法取回");
        return false;
    }
    if (!QDir().mkpath(destDir)) {
        if (err) *err = QStringLiteral("无法创建取回目录：%1").arg(destDir);
        return false;
    }
    securePacsDir(destDir);

    std::unique_ptr<DcmSCU> scu = makeScu();

    // C-GET 的协商要比 C-FIND 多两步：
    //  1) 除 C-GET 查询上下文外，还要声明愿意接收哪些存储类（否则服务端无法把
    //     影像推回来）；
    //  2) 打开落盘模式并指定目录，收到的每个对象由 DcmSCU 直接写文件。
    OFList<OFString> xfer;
    xfer.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfer.push_back(UID_LittleEndianImplicitTransferSyntax);

    auto addPc = [&scu, &xfer](const char *uid, T_ASC_SC_ROLE role) {
        OFList<OFString> syntaxes = xfer;
        return scu->addPresentationContext(uid, syntaxes, role);
    };

    if (addPc(UID_GETStudyRootQueryRetrieveInformationModel, ASC_SC_ROLE_SCU).bad()) {
        if (err) *err = QStringLiteral("注册 C-GET 呈现上下文失败");
        return false;
    }
    // 关键：存储类必须申请 **SCP 角色**。C-GET 里同一个关联上角色是反的——
    // 我们发检索请求（SCU），同时接收对方推来的 C-STORE（SCP）。只申请默认角色时
    // 服务端会报 "No presentation context with requestor SCP role"，所有子操作全失败。
    // 只声明本项目实际会渲染的存储类：MR / CT / 增强 MR / 二次捕获。
    for (const char *uid : {UID_MRImageStorage, UID_CTImageStorage,
                            UID_EnhancedMRImageStorage, UID_SecondaryCaptureImageStorage}) {
        if (addPc(uid, ASC_SC_ROLE_SCP).bad()) {
            if (err) *err = QStringLiteral("注册存储类呈现上下文失败：%1").arg(uid);
            return false;
        }
    }

    scu->setStorageMode(DCMSCU_STORAGE_DISK);
    scu->setStorageDir(destDir.toStdString());

    if (!openAssociation(*scu, endpoint, err)) {
        return false;
    }

    DcmDataset query;
    query.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    query.putAndInsertString(DCM_StudyInstanceUID, studyUid.toUtf8().constData());

    OFList<RetrieveResponse *> responses;
    const OFCondition cond = scu->sendCGETRequest(1, &query, &responses);

    // 最终状态要以响应里的失败子操作数为准：cond 只代表「对话本身没崩」，
    // 服务端可能一半成功一半失败。
    int failed = 0, completed = 0;
    for (RetrieveResponse *r : responses) {
        failed = std::max(failed, static_cast<int>(r->m_numberOfFailedSubops));
        completed = std::max(completed, static_cast<int>(r->m_numberOfCompletedSubops));
        delete r;
    }
    scu->closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

    if (cond.bad()) {
        if (err) *err = QStringLiteral("C-GET 失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
    }
    if (failed > 0) {
        if (err) *err = QStringLiteral("C-GET 部分失败：%1 个子操作未完成（已完成 %2）")
                            .arg(failed).arg(completed);
        return false;
    }

    // DcmSCU 落盘用的是 "MR.<SOPInstanceUID>"，没有扩展名；而下游解析器只认
    // *.dcm / *.dicom。两者之间的落差必须在取回这一层补上，不能让调用方猜。
    const QDir dir(destDir);
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)) {
        if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
        if (!fi.fileName().endsWith(QStringLiteral(".dcm"), Qt::CaseInsensitive)) {
            QFile::rename(fi.absoluteFilePath(), fi.absoluteFilePath() + QStringLiteral(".dcm"));
        }
    }
    securePacsFiles(destDir);

    const int landed = static_cast<int>(
        dir.entryList({QStringLiteral("*.dcm")}, QDir::Files).size());
    if (landed == 0) {
        if (err) *err = QStringLiteral("C-GET 结束但没有文件落地（检查存储类与写盘权限）");
        return false;
    }
    if (fileCount) *fileCount = landed;
    return true;
}
bool PacsClient::findStudies(const Endpoint &endpoint, QList<QVariantMap> *studies, QString *err)
{
    if (!studies) return false;

    std::unique_ptr<DcmSCU> scu = makeScu();

    // 呈现上下文必须在 initNetwork 之前注册，否则协商阶段没有任何可选的上下文，
    // initNetwork 直接报 "No Presentation Contexts defined"。
    // 传输语法列表用显式 VR 小端为主、隐式小端兜底老设备；DCMTK 不接受空列表。
    OFList<OFString> xferSyntaxes;
    xferSyntaxes.push_back(UID_LittleEndianExplicitTransferSyntax);
    xferSyntaxes.push_back(UID_LittleEndianImplicitTransferSyntax);
    const OFCondition pc = scu->addPresentationContext(
        UID_FINDStudyRootQueryRetrieveInformationModel, xferSyntaxes);
    if (pc.bad()) {
        if (err) *err = QStringLiteral("注册 C-FIND 呈现上下文失败：%1")
                             .arg(QString::fromStdString(pc.text()));
        return false;
    }
    if (!openAssociation(*scu, endpoint, err)) {
        return false;
    }

    // STUDY 级查询：空条件表示「列出全部」，这是接入期确认库里有东西的最直接办法。
    DcmDataset query;
    query.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    query.putAndInsertString(DCM_PatientName, "");
    query.putAndInsertString(DCM_StudyDate, "");
    query.putAndInsertString(DCM_StudyInstanceUID, "");
    query.putAndInsertString(DCM_PatientID, "");

    OFList<QRResponse *> responses;
    const OFCondition cond = scu->sendFINDRequest(1, &query, &responses);
    scu->closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

    if (cond.bad()) {
        if (err) *err = QStringLiteral("C-FIND 失败：%1").arg(QString::fromStdString(cond.text()));
        for (QRResponse *r : responses) delete r;
        return false;
    }

    for (QRResponse *r : responses) {
        if (r && r->m_dataset) {
            DcmDataset *ds = r->m_dataset;
            QVariantMap item;
            auto put = [&item, ds](const DcmTagKey &tag, const QString &key) {
                OFString value;
                if (ds->findAndGetOFStringArray(tag, value).good() && !value.empty()) {
                    item.insert(key, QString::fromUtf8(value.c_str()));
                }
            };
            put(DCM_PatientName, QStringLiteral("patientName"));
            put(DCM_PatientID, QStringLiteral("patientId"));
            put(DCM_StudyDate, QStringLiteral("studyDate"));
            put(DCM_StudyTime, QStringLiteral("studyTime"));
            put(DCM_StudyInstanceUID, QStringLiteral("studyInstanceUid"));
            put(DCM_AccessionNumber, QStringLiteral("accession"));
            put(DCM_Modality, QStringLiteral("modality"));

            Uint16 seriesCount = 0;
            if (ds->findAndGetUint16(DCM_NumberOfStudyRelatedSeries, seriesCount).good()) {
                item.insert(QStringLiteral("series"), static_cast<int>(seriesCount));
            }
            if (!item.isEmpty()) {
                studies->append(item);
            }
        }
        delete r;
    }
    return true;
}
