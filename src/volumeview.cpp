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
uniform vec2 uWindow;   // x = 窗位中心, y = 窗宽（归一化强度）
uniform int uSteps;

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
    vec3 texel = vec3(1.0 / 128.0);
    const float iso = 0.52;

    // 等值面 + 梯度光照 + 双色编码。
    // 不用半透明累积：非增强 CT 累积出来是一团雾，脑沟脑回全糊掉。
    // 颜色区分分两层意思：
    //   1) 按命中处强度——软组织琥珀、骨/对比剂冷白；
    //   2) 按命中深度——近处偏暖、深处偏冷，给出前后关系。
    for (int i = 0; i < uSteps; ++i) {
        vec3 q = p + 0.5;
        if (all(greaterThanEqual(q, vec3(0.0))) && all(lessThanEqual(q, vec3(1.0)))) {
            float raw = texture(uVolume, q).r;
            float v = clamp((raw - uWindow.x) / uWindow.y + 0.5, 0.0, 1.0);
            if (v > iso) {
                vec3 gx = vec3(texture(uVolume, q + vec3(texel.x, 0.0, 0.0)).r,
                               texture(uVolume, q + vec3(0.0, texel.y, 0.0)).r,
                               texture(uVolume, q + vec3(0.0, 0.0, texel.z)).r);
                vec3 gx0 = vec3(texture(uVolume, q - vec3(texel.x, 0.0, 0.0)).r,
                                texture(uVolume, q - vec3(0.0, texel.y, 0.0)).r,
                                texture(uVolume, q - vec3(0.0, 0.0, texel.z)).r);
                vec3 n = normalize(gx0 - gx);
                float diff = max(abs(dot(n, normalize(-rd))), 0.0);

                // 强度色：琥珀（软组织）→ 冷白（骨/对比剂）
                float k = clamp((v - iso) / (1.0 - iso), 0.0, 1.0);
                vec3 byValue = mix(vec3(1.0, 0.56, 0.22), vec3(0.88, 0.94, 1.0), k);
                // 深度色：命中越深越偏冷
                float depthT = clamp(float(i) / float(uSteps) * 2.2, 0.0, 1.0);
                vec3 byDepth = mix(vec3(1.0, 0.80, 0.52), vec3(0.45, 0.68, 1.0), depthT);
                vec3 base = mix(byValue, byDepth, 0.42);
                base *= (0.50 + 0.50 * diff);

                fragColor = vec4(base, 1.0);
                return;
            }
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
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_hasData) renderFrame();
    });
    m_timer->start();
}

void VolumeView::setVolume(const QByteArray &data, int dim, const QVariantMap &meta)
{
    if (dim <= 0 || data.size() != dim * dim * dim) {
        qWarning().noquote() << "[Volume] 体数据尺寸不符，忽略："
                             << "dim=" << dim << "bytes=" << data.size();
        return;
    }
    m_bytes = data;
    m_texDim = dim;
    m_meta = meta;
    m_hasData = true;
    m_textureDirty = true;
    m_firstFrameMs = -1;
    m_sinceSetVolume.start();

    // 窗宽窗位按体数据自身的 2%–98% 分位定，而不是写死 0.38/0.62：
    // 不同重建的 CT 强度范围差别很大，写死的窗口不是全黑就是过曝。
    {
        int hist[256] = {0};
        const int total = static_cast<int>(data.size());
        for (int i = 0; i < total; ++i) {
            hist[static_cast<unsigned char>(data.at(i))] += 1;
        }
        auto percentile = [&](int num, int den) {
            const int cut = static_cast<int>(
                static_cast<long long>(total) * num / den);
            int acc = 0;
            for (int i = 0; i < 256; ++i) {
                acc += hist[i];
                if (acc >= cut) return i;
            }
            return 255;
        };
        // CT 体数据里空气占绝大多数：2% 分位几乎总是 0，不能拿它当窗下限。
        // 用 p50（软组织起点）到 p99.9（骨/对比剂）作为显示窗口。
        const int lo = percentile(50, 100);
        const int hi = std::max(lo + 8, percentile(999, 1000));
        m_windowCenter = (static_cast<float>(lo) + static_cast<float>(hi)) / 2.0f / 255.0f;
        m_windowWidth = std::max(0.12f, (static_cast<float>(hi - lo)) / 255.0f);
        qInfo().noquote() << QStringLiteral("[Volume] 体数据分位窗：p50=%1 p99.9=%2 → 窗位 %3 窗宽 %4")
                                 .arg(lo).arg(hi)
                                 .arg(m_windowCenter, 0, 'f', 3)
                                 .arg(m_windowWidth, 0, 'f', 3);
    }

    qInfo().noquote() << QStringLiteral("[Volume] 原生体渲染收到数据：%1³，%2 KB，来源 %3")
                             .arg(dim).arg(data.size() / 1024)
                             .arg(meta.value(QStringLiteral("source")).toString());
    renderFrame();
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
    m_gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    m_gl->glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, m_texDim, m_texDim, m_texDim, 0,
                       GL_RED, GL_UNSIGNED_BYTE, m_bytes.constData());
    m_textureDirty = false;
    qInfo().noquote() << QStringLiteral("[Volume] 3D 纹理上传完成：%1³ = %2 KB")
                             .arg(m_texDim).arg(m_bytes.size() / 1024);
}

void VolumeView::renderFrame()
{
    if (!m_hasData) return;
    m_frameTimer.start();
    if (!ensureGl()) return;

    // 离屏尺寸跟控件走，但按 0.5 比例降采样：128³ 体数据在 400px 上下
    // 已经看不出差别，省下来的都是每帧的采样与回读带宽。
    const int targetW = std::clamp(
        static_cast<int>(width() * devicePixelRatioF() / 2.0), 160, 720);
    const int targetH = std::clamp(
        static_cast<int>(height() * devicePixelRatioF() / 2.0), 160, 720);

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
    m_program.setUniformValue("uWindow", QVector2D(m_windowCenter, m_windowWidth));
    m_program.setUniformValue("uSteps", 192);

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
    if (m_hasData) renderFrame();
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
    QWidget::wheelEvent(event);
}

void VolumeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_yaw = 25.0f;
    m_pitch = 18.0f;
    m_distance = 1.35f;
    QWidget::mouseDoubleClickEvent(event);
}
