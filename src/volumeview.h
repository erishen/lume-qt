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

    /// 接收重采样后的体数据（dim³，int16 HU）与元信息。
    /// 传 8-bit 归一化数据会直接拒绝：按窗位/窗宽显示必须保住 HU。
    void setVolume(const QByteArray &data, int dim, const QVariantMap &meta);

    /// 临床窗预设：0 软组织 W80/L40，1 骨 W1500/L300，2 血管 W400/L100，3 数据自适应
    void setPreset(int preset);
    /// 手动窗（W/L）：Alt+左键拖动也走这里；单位为 HU（CT）或 a.u.（非 CT）
    void setManualWindow(double width, double level);
    /// 去骨：把 HU ≥ 300 的体素当透明（仅 CT 有意义；非 CT 由界面禁用）
    void setBoneRemoval(bool on);
    /// 切面：axis -1 关闭，0=x 矢状、1=y 冠状、2=z 横断；position 为 0..1
    void setClipPlane(int axis, float position);
    bool boneRemoval() const;
    int clipAxis() const;
    float clipPosition() const;
    double windowWidth() const;
    double windowLevel() const;
    bool isCtVolume() const;
    int preset() const { return m_preset; }

    /// 伪彩叠加：默认关闭，按诊断惯例走灰阶。
    void setColorOverlay(bool on);
    bool colorOverlay() const { return m_colorOverlay; }

    QString windowText() const;
    bool hasVolume() const { return m_hasData; }

    /// 首帧耗时（从 setVolume 到第一帧画完并回读），没有数据时为 -1。
    int firstFrameMs() const { return m_firstFrameMs; }

signals:
    /// 窗发生变化；manual=true 表示来自手动调窗（用于把下拉切到「自定义」）
    void windowChanged(double width, double level, bool manual);
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
    bool m_uploadPending = false;
    int m_uploadZ = 0;
    int m_uploadMaxChunkMs = 0;
    QElapsedTimer m_uploadTimer;
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
    /// 只有相机/窗/数据/尺寸发生变化才需要重绘；否则 30fps 定时器只是空转
    bool m_needsRender = false;

    float m_yaw = 25.0f;
    float m_pitch = 18.0f;
    float m_distance = 1.35f;
    float m_windowCenterHu = 40.0f;
    float m_windowWidthHu = 80.0f;
    float m_huMin = -1000.0f;
    float m_huMax = 1000.0f;
    float m_autoCenterHu = 40.0f;
    float m_autoWidthHu = 80.0f;
    float m_tissueLoHu = 0.0f;   // 自适应窗统计出来的组织下限/上限
    float m_tissueHiHu = 0.0f;
    int m_preset = 0;
    bool m_colorOverlay = false;
    bool m_isCt = false;
    QPoint m_lastPos;
    bool m_dragging = false;
    bool m_windowing = false;
    bool m_boneRemove = false;
    int m_clipAxis = -1;
    float m_clipPos = 1.0f;
};
