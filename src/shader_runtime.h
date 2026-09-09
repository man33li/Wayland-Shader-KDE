#pragma once

#include <epoxy/egl.h>

#include <QList>
#include <QMap>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace WaylandShader {

struct ShaderParameter {
    QString name;
    QString description;
    double minimum;
    double maximum;
    double step;
    double defaultValue;
    double value;
};

struct ShaderFrame {
    // Effect-exclusive GL_TEXTURE_2D with a complete mip chain. librashader
    // may regenerate its mipmaps; it never takes ownership of either texture.
    uint32_t source;
    // Keep destination alive while this output's chain can retain its FBO
    // attachment (until another destination is rendered or runtime destroyed).
    uint32_t destination;
    uint32_t format;
    QSize size;
    uint64_t number;
    float framesPerSecond;
    // Time since this output's previous frame, not time since program startup.
    uint32_t elapsedMilliseconds;
    bool clearHistory;
};

// Calls are serialized by the owner. load() may run on a worker; the released
// private context then migrates to the rendering thread on the first render().
class ShaderRuntime {
public:
    // With no selected outputs, validate the preset and retain its parameter
    // metadata without retaining any GPU filter chains.
    static std::unique_ptr<ShaderRuntime> load(EGLDisplay display, EGLContext shareContext,
        const QString& presetPath,
        const std::vector<uintptr_t>& outputs,
        const QMap<QString, double>& overrides,
        QString& error);
    ~ShaderRuntime();
    ShaderRuntime(const ShaderRuntime&) = delete;
    ShaderRuntime& operator=(const ShaderRuntime&) = delete;

    const QList<ShaderParameter>& parameters() const;
    bool render(uintptr_t output, const ShaderFrame& frame, QString& error);
    bool setParameter(const QString& name, double value, QString& error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    explicit ShaderRuntime(std::unique_ptr<Impl> impl);
};

} // namespace WaylandShader
