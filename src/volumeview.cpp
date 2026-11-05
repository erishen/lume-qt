#include "volumeview.h"

#include <QDebug>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace {

const char *kVertexShader = R"(
#version 150 core
in vec2 aPos;
out vec2 vNdc;
void main()
{
    vNdc = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char *kFragmentShader = R"(
#version 150 core
in vec2 vNdc;
out vec4 fragColor;

uniform sampler3D uVolume;
uniform vec3 uCamPos;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec3 uFwd;
uniform float uTanHalfFov;
uniform float uAspect;
uniform vec2 uHuRange;   // x = huMin, y = huMax - huMin
uniform vec2 uWindow;    // x = 窗位(HU), y = 窗宽(HU)
uniform int uColorMode;  // 0 = 灰阶（诊断惯例）, 1 = 伪彩叠加（非诊断）
uniform int uSteps;
uniform float uTexDim;

const vec3 kBoxMin = vec3(-0.5);
const vec3 kBoxMax = vec3(0.5);

bool hitBox(vec3 ro, vec3 rd, out float t0, out float t1)
{
    vec3 inv = 1.0 / rd;
    vec3 tmin = (kBoxMin - ro) * inv;
    vec3 tmax = (kBoxMax - ro) * inv;
    vec3 tnear = min(tmin, tmax);
    vec3 tfar = max(tmin, tmax);
    t0 = max(max(tnear.x, tnear.y), tnear.z);
    t1 = min(min(tfar.x, tfar.y), tfar.z);
    return t1 > t0;
}

float sampleHu(vec3 q)
{
    return uHuRange.x + texture(uVolume, q).r * uHuRange.y;
}

void main()
{
    vec3 rd = normalize(vNdc.x * uAspect * uTanHalfFov * uRight
                        + vNdc.y * uTanHalfFov * uUp
                        + uFwd);
    float t0 = 0.0;
    float t1 = 0.0;
    if (!hitBox(uCamPos, rd, t0, t1)) {
        fragColor = vec4(0.0);
        return;
    }
    t0 = max(t0, 0.0);
    float dt = (t1 - t0) / float(uSteps);
    vec3 p = uCamPos + rd * t0;
    vec3 texel = vec3(1.0 / uTexDim);

    // 窗位/窗宽直接作用在 HU 上（临床惯例：W/L，而不是先压缩成 0-255 再猜）。
    // 等值面取窗中心，落到窗外的体素透明；这样换预设就等于换「看哪一层组织」。
    float vPrev = -1.0;
    vec3 pPrev = p;
    for (int i = 0; i < uSteps; ++i) {
        vec3 q = p + 0.5;
        if (all(greaterThanEqual(q, vec3(0.0))) && all(lessThanEqual(q, vec3(1.0)))) {
            float v = clamp((sampleHu(q) - uWindow.x) / uWindow.y + 0.5, 0.0, 1.0);
            if (v > 0.5) {
                // 在跨越点做线性插值：不插值的话，等值面会落在采样格上，
                // 曲面（尤其颅骨这种大曲率薄壳）会出现一圈圈台阶纹。
                float tHit = 1.0;
                if (vPrev >= 0.0 && vPrev < 0.5) {
                    tHit = clamp((0.5 - vPrev) / (v - vPrev), 0.0, 1.0);
                }
                vec3 hp = mix(pPrev, p, tHit) + 0.5;
                vec3 gxp = vec3(sampleHu(hp + vec3(texel.x, 0.0, 0.0)),
                                sampleHu(hp + vec3(0.0, texel.y, 0.0)),
                                sampleHu(hp + vec3(0.0, 0.0, texel.z)));
                vec3 gxn = vec3(sampleHu(hp - vec3(texel.x, 0.0, 0.0)),
                                sampleHu(hp - vec3(0.0, texel.y, 0.0)),
                                sampleHu(hp - vec3(0.0, 0.0, texel.z)));
                vec3 n = normalize(gxn - gxp);
                float diff = max(abs(dot(n, normalize(-rd))), 0.0);
                float rim = pow(1.0 - diff, 2.0);
                // 主光 + 轮廓光；再按命中深度轻微压暗，给单色画面一点前后关系
                float shade = 0.42 + 0.50 * diff + 0.18 * rim;
                float depthFade = clamp(1.0 - 0.35 * float(i) / float(uSteps), 0.62, 1.0);

                if (uColorMode == 0) {
                    // 灰阶：亮度就是窗内位置，符合阅片习惯。等值面处约 0.5 灰。
                    fragColor = vec4(vec3(0.30 + 0.70 * v) * shade * depthFade, 1.0);
                    return;
                }
                // 伪彩叠加（仅定位用）：强度越高越冷白，命中越深越偏冷。
                float k = clamp((v - 0.5) * 2.0, 0.0, 1.0);
                vec3 byValue = mix(vec3(1.0, 0.56, 0.22), vec3(0.88, 0.94, 1.0), k);
                float depthT = clamp(float(i) / float(uSteps) * 2.2, 0.0, 1.0);
                vec3 byDepth = mix(vec3(1.0, 0.80, 0.52), vec3(0.45, 0.68, 1.0), depthT);
                fragColor = vec4(mix(byValue, byDepth, 0.42) * shade, 1.0);
                return;
            }
            vPrev = v;
            pPrev = p;
        }
        p += rd * dt;
    }
    fragColor = vec4(0.0);
}
)";

} // namespace

VolumeView::VolumeView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(220, 220);
    setToolTip(QStringLiteral("拖动旋转 · 滚轮缩放 · 双击复位"));

    m_timer = new QTimer(this);
    m_timer->setInterval(33); // 30 fps：术中要的是结构定位，不必 60
    // 事件驱动：没有脏标记就什么都不做。实测相机静止时 30fps 常转要吃掉约 10% CPU。
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_hasData && m_needsRender) renderFrame();
    });
    m_timer->start();
}

void VolumeView::setVolume(const QByteArray &data, int dim, const QVariantMap &meta)
{
    const int count = dim * dim * dim;
    if (dim <= 0 || data.size() != count * static_cast<int>(sizeof(int16_t))) {
        qWarning().noquote() << "[Volume] 体数据不是 int16 HU，拒绝显示："
                             << "dim=" << dim << "bytes=" << data.size();
        return;
    }
    const int16_t *hu = reinterpret_cast<const int16_t *>(data.constData());
    m_texDim = dim;
    m_meta = meta;
    // 只有 CT 的物理值才是 HU（RescaleSlope/Intercept 换算出来的）。
    // MR 是任意强度单位，拿「骨窗 W1500/L300」去套没有意义，只能用自适应强度窗。
    m_isCt = meta.value(QStringLiteral("modality")).toString().compare(
                 QStringLiteral("CT"), Qt::CaseInsensitive) == 0;

    // 记录真实 HU 值域：着色器需要把 16-bit 归一化纹理还原成 HU 才能做窗。
    m_huMin = static_cast<float>(meta.value(QStringLiteral("min"), -1000.0).toDouble());
    m_huMax = static_cast<float>(meta.value(QStringLiteral("max"), 1000.0).toDouble());
    if (!(m_huMax > m_huMin)) { m_huMin = -1000.0f; m_huMax = 1000.0f; }
    const float huRange = m_huMax - m_huMin;

    // 数据自适应窗：直方图的「组织」分位 p50 → p99.9。
    // CT 里空气（约 -1000 HU）通常占一半以上体素，直接取 p50 会把窗位拖到空气，
    // 整屏糊成白雾（这坑在 8-bit 时代踩过一次）。所以 CT 统计时先剔掉 < -500 HU。
    {
        const int kBins = 1024;
        int hist[kBins] = {0};
        // CT 剔空气（约 -1000）；MR 剔背景零（背景常占大半体素，会把 p50 拖到 0）。
        const float tissueFloor = m_isCt ? -500.0f : m_huMin + 0.001f * huRange;
        int tissueCount = 0;
        for (int i = 0; i < count; ++i) {
            if (static_cast<float>(hu[i]) < tissueFloor) continue;
            const int bin = std::clamp(
                static_cast<int>((static_cast<float>(hu[i]) - m_huMin) / huRange * (kBins - 1)),
                0, kBins - 1);
            hist[bin] += 1;
            tissueCount += 1;
        }
        const int total = tissueCount > 0 ? tissueCount : count;
        auto percentileBin = [&](int num, int den) {
            const int cut = static_cast<int>(static_cast<long long>(total) * num / den);
            int acc = 0;
            for (int i = 0; i < kBins; ++i) {
                acc += hist[i];
                if (acc >= cut) return i;
            }
            return kBins - 1;
        };
        const float binToHu = huRange / static_cast<float>(kBins - 1);
        const float loHu = m_huMin
            + static_cast<float>(percentileBin(m_isCt ? 50 : 5, 100)) * binToHu;
        const float hiHu = m_huMin
            + static_cast<float>(percentileBin(999, 1000)) * binToHu;
        m_tissueLoHu = loHu;
        m_tissueHiHu = hiHu;
        m_autoCenterHu = (loHu + hiHu) / 2.0f;
        m_autoWidthHu = std::max(20.0f, hiHu - loHu);
        qInfo().noquote() << QStringLiteral("[Volume] 自适应窗分位：低 %1 高 %2 → W%3 / L%4")
                                 .arg(static_cast<int>(loHu)).arg(static_cast<int>(hiHu))
                                 .arg(static_cast<int>(m_autoWidthHu))
                                 .arg(static_cast<int>(m_autoCenterHu));
    }

    // 打包成 16-bit 归一化纹理数据：GL_R16 支持线性过滤，且精度足够承载 HU。
    std::vector<uint16_t> packed(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const float t = std::clamp((static_cast<float>(hu[i]) - m_huMin) / huRange, 0.0f, 1.0f);
        packed[static_cast<size_t>(i)] =
            static_cast<uint16_t>(std::lround(t * 65535.0f));
    }
    m_bytes = QByteArray(reinterpret_cast<const char *>(packed.data()),
                         static_cast<int>(packed.size() * sizeof(uint16_t)));

    m_hasData = true;
    m_textureDirty = true;
    m_firstFrameMs = -1;
    m_sinceSetVolume.start();
    setPreset(m_preset); // 应用当前预设（默认软组织窗）

    qInfo().noquote() << QStringLiteral("[Volume] 原生体渲染收到 HU 数据：%1³，HU %2…%3，%4 KB，来源 %5")
                             .arg(dim)
                             .arg(static_cast<int>(m_huMin))
                             .arg(static_cast<int>(m_huMax))
                             .arg(data.size() / 1024)
                             .arg(meta.value(QStringLiteral("source")).toString());
    renderFrame();
}

void VolumeView::setPreset(int preset)
{
    m_preset = std::clamp(preset, 0, 3);
    if (!m_isCt && m_preset != 3) {
        // 非 CT 没有 HU，但预设仍应「切得动」：把 CT 的四个档换算成组织强度区间，
        // 让它在 a.u. 数据上也有可区分的窗，而不是把选项禁死。
        const float lo = m_tissueLoHu;
        const float span = std::max(20.0f, m_tissueHiHu - m_tissueLoHu);
        switch (m_preset) {
        case 0:  // 软组织窗 → 组织低段
            m_windowCenterHu = lo + 0.20f * span;
            m_windowWidthHu = 0.40f * span;
            break;
        case 1:  // 骨窗 → 组织高段
            m_windowCenterHu = lo + 0.75f * span;
            m_windowWidthHu = 0.30f * span;
            break;
        default: // 血管窗 → 组织中段
            m_windowCenterHu = lo + 0.50f * span;
            m_windowWidthHu = 0.35f * span;
            break;
        }
        qInfo().noquote() << "[Volume] 非 CT：按组织强度近似应用预设" << windowText();
        if (m_hasData) renderFrame();
        return;
    }
    switch (m_preset) {
    case 1: // 骨窗
        m_windowCenterHu = 300.0f;
        m_windowWidthHu = 1500.0f;
        break;
    case 2: // 血管/对比剂窗
        m_windowCenterHu = 100.0f;
        m_windowWidthHu = 400.0f;
        break;
    case 3: // 数据自适应
        m_windowCenterHu = m_autoCenterHu;
        m_windowWidthHu = m_autoWidthHu;
        break;
    case 0:
    default: // 软组织窗（临床常用的 W80 / L40）
        m_windowCenterHu = 40.0f;
        m_windowWidthHu = 80.0f;
        break;
    }
    if (m_hasData && m_windowCenterHu > m_huMax) {
        // 骨窗之类预设可能整段落在数据值域之外（去掉颅骨的脑实质数据 HU 上限只有 249）。
        // 这时画面本就该是空的——但不能让它看起来像渲染坏了。
        qWarning().noquote() << QStringLiteral("[Volume] 预设超出数据 HU 上限（%1 > %2），窗内无组织")
                                    .arg(static_cast<int>(m_windowCenterHu))
                                    .arg(static_cast<int>(m_huMax));
    }
    qInfo().noquote() << "[Volume] 窗预设切换：" << windowText();
    if (m_hasData) renderFrame();
}

void VolumeView::setColorOverlay(bool on)
{
    m_colorOverlay = on;
    qInfo().noquote() << "[Volume] 显示模式：" << (on ? "伪彩叠加（非诊断）" : "灰阶（诊断惯例）");
    if (m_hasData) renderFrame();
}

QString VolumeView::windowText() const
{
    return QStringLiteral("W%1 / L%2 %4 · %3")
        .arg(static_cast<int>(m_windowWidthHu))
        .arg(static_cast<int>(m_windowCenterHu))
        .arg(m_colorOverlay ? QStringLiteral("伪彩（非诊断）") : QStringLiteral("灰阶"))
        .arg(m_isCt ? QStringLiteral("HU") : QStringLiteral("a.u."));
}

VolumeView::~VolumeView()
{
    if (m_timer) m_timer->stop();
    if (m_ctx == nullptr) return;
    if (m_surface != nullptr && m_glReady) {
        m_ctx->makeCurrent(m_surface);
        m_gl->glDeleteTextures(1, &m_texture);
        m_gl->glDeleteTextures(1, &m_colorTex);
        m_gl->glDeleteFramebuffers(1, &m_fbo);
        m_gl->glDeleteVertexArrays(1, &m_vao);
        m_gl->glDeleteBuffers(1, &m_vbo);
        m_ctx->doneCurrent();
    }
    delete m_surface;
    delete m_ctx; // versionFunctions 返回的函数对象随 context 一起释放
}

bool VolumeView::ensureGl()
{
    if (m_glReady) return true;
    if (m_glFailed) return false;

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);

    m_ctx = new QOpenGLContext;
    m_ctx->setFormat(fmt);
    if (!m_ctx->create()) {
        qWarning() << "[Volume] 无法创建 OpenGL 3.3 Core 离屏上下文，原生体渲染不可用";
        m_glFailed = true;
        return false;
    }
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(fmt);
    m_surface->create();
    if (!m_ctx->makeCurrent(m_surface)) {
        qWarning() << "[Volume] 离屏 surface 无法 current，原生体渲染不可用";
        m_glFailed = true;
        return false;
    }
    m_gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_ctx);
    if (m_gl == nullptr || !m_gl->initializeOpenGLFunctions()) {
        qWarning() << "[Volume] 当前上下文不支持 OpenGL 3.3 Core，原生体渲染不可用";
        m_glFailed = true;
        m_ctx->doneCurrent();
        return false;
    }

    m_renderer = QString::fromLatin1(
        reinterpret_cast<const char *>(m_gl->glGetString(GL_RENDERER)));
    qInfo().noquote() << "[Volume] 离屏 OpenGL 上下文就绪：" << m_renderer;

    if (!m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
        qWarning().noquote() << "[Volume] 着色器编译失败：" << m_program.log();
        m_glFailed = true;
        m_ctx->doneCurrent();
        return false;
    }
    m_program.bindAttributeLocation("aPos", 0);
    if (!m_program.link()) {
        qWarning().noquote() << "[Volume] 着色器链接失败：" << m_program.log();
        m_glFailed = true;
        m_ctx->doneCurrent();
        return false;
    }

    static const float quad[] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
        -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    };
    m_gl->glGenVertexArrays(1, &m_vao);
    m_gl->glGenBuffers(1, &m_vbo);
    m_gl->glBindVertexArray(m_vao);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    m_gl->glBindVertexArray(0);

    m_gl->glGenTextures(1, &m_texture);
    m_gl->glBindTexture(GL_TEXTURE_3D, m_texture);
    m_gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    const unsigned char zero = 0;
    m_gl->glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, 1, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &zero);

    m_gl->glGenTextures(1, &m_colorTex);
    m_gl->glGenFramebuffers(1, &m_fbo);

    m_ctx->doneCurrent();
    m_glReady = true;
    return true;
}

void VolumeView::uploadTexture()
{
    if (!m_textureDirty || !m_hasData) return;
    m_gl->glBindTexture(GL_TEXTURE_3D, m_texture);
    m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    m_gl->glTexImage3D(GL_TEXTURE_3D, 0, GL_R16, m_texDim, m_texDim, m_texDim, 0,
                       GL_RED, GL_UNSIGNED_SHORT, m_bytes.constData());
    m_textureDirty = false;
    qInfo().noquote() << QStringLiteral("[Volume] 16-bit 3D 纹理上传完成：%1³ = %2 KB（R16，线性过滤）")
                             .arg(m_texDim).arg(m_bytes.size() / 1024);
}

void VolumeView::renderFrame()
{
    if (!m_hasData) return;
    m_needsRender = false;
    m_frameTimer.start();
    if (!ensureGl()) return;

    // 离屏按设备像素 1:1 渲染：曾经按 dpr/2 降采样，Retina 上等于把 808px 的画面
    // 用 404px 渲染再拉大，边界全部糊掉——这是「不清晰」的主要来源。
    // 1:1 设备像素。试过 1.5× 超采样再缩回控件尺寸，轮廓确实更平滑，但每像素
    // 平均掉 2.25 个样本，脑沟那种高频明暗被抹成一片——观感上反而「不如以前清楚」。
    // 保留 1:1，抗锯齿交给体数据本身的线性插值与轮廓光。
    const double superSample = 1.0;
    const int targetW = std::clamp(
        static_cast<int>(width() * devicePixelRatioF() * superSample), 320, 2400);
    const int targetH = std::clamp(
        static_cast<int>(height() * devicePixelRatioF() * superSample), 320, 2400);

    if (!m_ctx->makeCurrent(m_surface)) return;

    if (targetW != m_fboWidth || targetH != m_fboHeight) {
        m_fboWidth = targetW;
        m_fboHeight = targetH;
        m_gl->glBindTexture(GL_TEXTURE_2D, m_colorTex);
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_fboWidth, m_fboHeight, 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, m_colorTex, 0);
    }

    uploadTexture();

    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    m_gl->glViewport(0, 0, m_fboWidth, m_fboHeight);
    m_gl->glClearColor(0.043f, 0.059f, 0.078f, 1.0f);
    m_gl->glClear(GL_COLOR_BUFFER_BIT);

    const float tanHalfFov = 0.4142f; // 2*atan(0.4142) ≈ 45°
    const float aspect = static_cast<float>(m_fboWidth) / static_cast<float>(m_fboHeight);

    QMatrix4x4 rot;
    rot.rotate(m_pitch, 1.0f, 0.0f, 0.0f);
    rot.rotate(m_yaw, 0.0f, 1.0f, 0.0f);
    const QVector3D camPos = rot.map(QVector3D(0.0f, 0.0f, m_distance));
    const QVector3D fwd = rot.map(QVector3D(0.0f, 0.0f, -1.0f)).normalized();
    const QVector3D up = rot.map(QVector3D(0.0f, 1.0f, 0.0f)).normalized();
    const QVector3D right = QVector3D::crossProduct(fwd, up).normalized();

    m_program.bind();
    m_program.setUniformValue("uVolume", 0);
    m_program.setUniformValue("uCamPos", camPos);
    m_program.setUniformValue("uRight", right);
    m_program.setUniformValue("uUp", up);
    m_program.setUniformValue("uFwd", fwd);
    m_program.setUniformValue("uTanHalfFov", tanHalfFov);
    m_program.setUniformValue("uAspect", aspect);
    m_program.setUniformValue("uHuRange", QVector2D(m_huMin, m_huMax - m_huMin));
    m_program.setUniformValue("uWindow", QVector2D(m_windowCenterHu, m_windowWidthHu));
    m_program.setUniformValue("uColorMode", m_colorOverlay ? 1 : 0);
    // 128³ 体数据、相机距离 1.35 时，160 步足够让等值面跨越点被采到；
    // 分辨率翻倍后继续堆步数只会拖慢帧，对清晰度没有帮助。
    m_program.setUniformValue("uSteps", 160);
    m_program.setUniformValue("uTexDim", static_cast<float>(m_texDim));

    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_3D, m_texture);
    m_gl->glEnable(GL_BLEND);
    m_gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_gl->glBindVertexArray(m_vao);
    m_gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    m_gl->glBindVertexArray(0);
    m_gl->glDisable(GL_BLEND);
    m_program.release();

    // 回读：GL 原点在左下，QImage 在左上，镜像一次。
    QImage shot(m_fboWidth, m_fboHeight, QImage::Format_RGBA8888);
    m_gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    m_gl->glReadPixels(0, 0, m_fboWidth, m_fboHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                       shot.bits());
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_ctx->doneCurrent();

    m_frame = shot.flipped(Qt::Vertical);
    update();
    reportFirstFrameIfNeeded();

    // 稳态耗时要能自己报出来：首帧里含着上下文创建与着色器编译，
    // 不能拿它判断「每帧贵不贵」。每 60 帧（约 2 秒）打一次均值。
    const qint64 cost = m_frameTimer.elapsed();
    m_frameTotalMs += cost;
    m_frameCount += 1;
    if (m_frameCount >= 60) {
        qInfo().noquote() << QStringLiteral("[Volume] 原生渲染稳态：%1 帧均值 %2 ms（%3×%4 离屏）")
                                 .arg(m_frameCount)
                                 .arg(static_cast<double>(m_frameTotalMs) /
                                      static_cast<double>(m_frameCount), 0, 'f', 1)
                                 .arg(m_fboWidth).arg(m_fboHeight);
        m_frameCount = 0;
        m_frameTotalMs = 0;
    }
}

void VolumeView::reportFirstFrameIfNeeded()
{
    if (!m_hasData || m_firstFrameMs >= 0 || !m_sinceSetVolume.isValid()) return;
    m_firstFrameMs = static_cast<int>(m_sinceSetVolume.elapsed());
    const QVariantList dims = m_meta.value(QStringLiteral("dims")).toList();
    const QString dimText = dims.size() == 3
                                ? QStringLiteral("%1×%2×%3")
                                      .arg(dims.at(0).toInt())
                                      .arg(dims.at(1).toInt())
                                      .arg(dims.at(2).toInt())
                                : QStringLiteral("%1³").arg(m_texDim);
    qInfo().noquote() << QStringLiteral("[Volume] 原生首帧：%1 ms（%2）")
                             .arg(m_firstFrameMs).arg(dimText);
    emit firstFrameRendered(m_firstFrameMs, dimText);
}

void VolumeView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(11, 15, 20));
    if (m_frame.isNull()) {
        painter.setPen(QColor(140, 155, 175));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待体数据…"));
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), m_frame);
}

void VolumeView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_hasData) m_needsRender = true;
}

void VolumeView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastPos = event->pos();
    }
    QWidget::mousePressEvent(event);
}

void VolumeView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->pos() - m_lastPos;
        m_lastPos = event->pos();
        m_yaw += static_cast<float>(delta.x()) * 0.6f;
        m_pitch = std::clamp(m_pitch + static_cast<float>(delta.y()) * 0.6f, -89.0f, 89.0f);
        m_needsRender = true;
    }
    QWidget::mouseMoveEvent(event);
}

void VolumeView::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void VolumeView::wheelEvent(QWheelEvent *event)
{
    const float factor = event->angleDelta().y() > 0 ? 0.88f : 1.12f;
    m_distance = std::clamp(m_distance * factor, 0.95f, 3.0f);
    m_needsRender = true;
    QWidget::wheelEvent(event);
}

void VolumeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_yaw = 25.0f;
    m_pitch = 18.0f;
    m_distance = 1.35f;
    m_needsRender = true;
    QWidget::mouseDoubleClickEvent(event);
}
