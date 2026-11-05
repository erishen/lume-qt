#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QPoint>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QOffscreenSurface;
class QOpenGLContext;
class QTimer;

/**
 * VolumeView — DICOM 体数据的「原生绘制通道」。
 *
 * 与网页侧 Volume3D.tsx（Three.js / WebGL2）并排显示同一份 128³ 数据，
 * 但完全不经过 QWebEngine、不经过 IPC、不经过 Chromium 合成。
 *
 * 关键实现选择：**不用 QOpenGLWidget**。
 * Qt WebEngine 自己持有渲染上下文，把 QOpenGLWidget 放进同一个窗口里
 * 会让两条 GL 路径互踩——实测 WebView 与 GL 控件同时变黑。
 * 这里改成离屏渲染：独立 3.3 Core 上下文 + FBO，画完 glReadPixels 成
 * QImage，再用 QPainter 贴到普通 QWidget 上。窗口里没有任何 GL 子控件，
 * WebEngine 的合成路径保持原样。
 */
class VolumeView : public QWidget
{
    Q_OBJECT

public:
    explicit VolumeView(QWidget *parent = nullptr);
    ~VolumeView() override;

    /// 接收重采样后的体数据（dim³，8-bit 单通道）与元信息。
    void setVolume(const QByteArray &data, int dim, const QVariantMap &meta);
    bool hasVolume() const { return m_hasData; }

    /// 首帧耗时（从 setVolume 到第一帧画完并回读），没有数据时为 -1。
    int firstFrameMs() const { return m_firstFrameMs; }

signals:
    /// 原生侧首帧完成，供台账记录「采集 → 原生上屏」的时间。
    void firstFrameRendered(int ms, const QString &dims);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    bool ensureGl();
    void renderFrame();
    void uploadTexture();
    void reportFirstFrameIfNeeded();

    QOpenGLContext *m_ctx = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QOpenGLFunctions_3_3_Core *m_gl = nullptr;
    QOpenGLShaderProgram m_program;
    unsigned int m_texture = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_fbo = 0;
    unsigned int m_colorTex = 0;
    int m_fboWidth = 0;
    int m_fboHeight = 0;
    int m_texDim = 0;
    QString m_renderer;

    QTimer *m_timer = nullptr;
    QImage m_frame;

    QByteArray m_bytes;
    QVariantMap m_meta;
    bool m_hasData = false;
    bool m_textureDirty = false;
    bool m_glReady = false;
    bool m_glFailed = false;

    QElapsedTimer m_sinceSetVolume;
    QElapsedTimer m_frameTimer;
    int m_frameCount = 0;
    qint64 m_frameTotalMs = 0;
    int m_firstFrameMs = -1;

    float m_yaw = 25.0f;
    float m_pitch = 18.0f;
    float m_distance = 1.35f;
    float m_windowCenter = 0.38f;
    float m_windowWidth = 0.62f;
    QPoint m_lastPos;
    bool m_dragging = false;
};
