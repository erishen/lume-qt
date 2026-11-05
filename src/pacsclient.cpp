#include "pacsclient.h"

#include <algorithm>

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmnet/dimse.h>
#include <dcmtk/dcmnet/scu.h>

namespace {

/// 把 DcmSCU 会话建起来：注册 AE、设置超时、发起关联。
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

    OFCondition cond = scu.initNetwork();
    if (cond.bad()) {
        if (err) *err = QStringLiteral("initNetwork 失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
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
    DcmSCU scu;
    // C-ECHO 也需要先声明 Verification 呈现上下文，否则 initNetwork 直接报
    // "No Presentation Contexts defined"——这是 DcmSCU 与 dcmnet 底层 API 的区别，
    // 不显式加就没有任何上下文可协商。
    OFList<OFString> echoXfer;
    echoXfer.push_back(UID_LittleEndianExplicitTransferSyntax);
    echoXfer.push_back(UID_LittleEndianImplicitTransferSyntax);
    const OFCondition pc = scu.addPresentationContext(UID_VerificationSOPClass, echoXfer);
    if (pc.bad()) {
        if (err) *err = QStringLiteral("注册 C-ECHO 呈现上下文失败：%1")
                             .arg(QString::fromStdString(pc.text()));
        return false;
    }
    if (!openAssociation(scu, endpoint, err)) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    const OFCondition cond = scu.sendECHORequest(0);
    const int ms = static_cast<int>(timer.elapsed());
    scu.closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

    if (cond.bad()) {
        if (err) *err = QStringLiteral("C-ECHO 失败：%1").arg(QString::fromStdString(cond.text()));
        return false;
    }
    if (roundTripMs) *roundTripMs = ms;
    return true;
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

    DcmSCU scu;

    // C-GET 的协商要比 C-FIND 多两步：
    //  1) 除 C-GET 查询上下文外，还要声明愿意接收哪些存储类（否则服务端无法把
    //     影像推回来）；
    //  2) 打开落盘模式并指定目录，收到的每个对象由 DcmSCU 直接写文件。
    OFList<OFString> xfer;
    xfer.push_back(UID_LittleEndianExplicitTransferSyntax);
    xfer.push_back(UID_LittleEndianImplicitTransferSyntax);

    auto addPc = [&scu, &xfer](const char *uid, T_ASC_SC_ROLE role) {
        OFList<OFString> syntaxes = xfer;
        return scu.addPresentationContext(uid, syntaxes, role);
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

    scu.setStorageMode(DCMSCU_STORAGE_DISK);
    scu.setStorageDir(destDir.toStdString());

    if (!openAssociation(scu, endpoint, err)) {
        return false;
    }

    DcmDataset query;
    query.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    query.putAndInsertString(DCM_StudyInstanceUID, studyUid.toUtf8().constData());

    OFList<RetrieveResponse *> responses;
    const OFCondition cond = scu.sendCGETRequest(1, &query, &responses);

    // 最终状态要以响应里的失败子操作数为准：cond 只代表「对话本身没崩」，
    // 服务端可能一半成功一半失败。
    int failed = 0, completed = 0;
    for (RetrieveResponse *r : responses) {
        failed = std::max(failed, static_cast<int>(r->m_numberOfFailedSubops));
        completed = std::max(completed, static_cast<int>(r->m_numberOfCompletedSubops));
        delete r;
    }
    scu.closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

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

    DcmSCU scu;

    // 呈现上下文必须在 initNetwork 之前注册，否则协商阶段没有任何可选的上下文，
    // initNetwork 直接报 "No Presentation Contexts defined"。
    // 传输语法列表用显式 VR 小端为主、隐式小端兜底老设备；DCMTK 不接受空列表。
    OFList<OFString> xferSyntaxes;
    xferSyntaxes.push_back(UID_LittleEndianExplicitTransferSyntax);
    xferSyntaxes.push_back(UID_LittleEndianImplicitTransferSyntax);
    const OFCondition pc = scu.addPresentationContext(
        UID_FINDStudyRootQueryRetrieveInformationModel, xferSyntaxes);
    if (pc.bad()) {
        if (err) *err = QStringLiteral("注册 C-FIND 呈现上下文失败：%1")
                             .arg(QString::fromStdString(pc.text()));
        return false;
    }
    if (!openAssociation(scu, endpoint, err)) {
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
    const OFCondition cond = scu.sendFINDRequest(1, &query, &responses);
    scu.closeAssociation(DCMSCU_RELEASE_ASSOCIATION);

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
