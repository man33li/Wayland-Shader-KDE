#include "shader_runtime.h"

#include <epoxy/gl.h>

#ifndef LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_OPENGL
#endif
#include <librashader.h>

#include <QByteArray>
#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace WaylandShader {
namespace {

    QString eglError(const char* operation)
    {
        return QStringLiteral("%1 failed (EGL error 0x%2)")
            .arg(QString::fromLatin1(operation))
            .arg(eglGetError(), 4, 16, QLatin1Char('0'));
    }

    bool checkLibra(libra_error_t failure, QString& error)
    {
        if (!failure) {
            return true;
        }
        char* message = nullptr;
        libra_error_write(failure, &message);
        error = message ? QString::fromUtf8(message)
                        : QStringLiteral("librashader failed (error %1)").arg(int(libra_error_errno(failure)));
        if (message) {
            libra_error_free_string(&message);
        }
        libra_error_free(&failure);
        return false;
    }

    void discardLibraError(libra_error_t failure)
    {
        if (failure) {
            QString error;
            checkLibra(failure, error);
            qWarning().noquote() << "WaylandShader cleanup:" << error;
        }
    }

    // Restores the EGL API as well as context and both surfaces, including callers
    // that had no current context. Never terminates the compositor's display.
    class CurrentContext {
    public:
        explicit CurrentContext(EGLDisplay target)
            : m_target(target)
            , m_display(eglGetCurrentDisplay())
            , m_context(eglGetCurrentContext())
            , m_draw(eglGetCurrentSurface(EGL_DRAW))
            , m_read(eglGetCurrentSurface(EGL_READ))
            , m_api(eglQueryAPI())
        {
        }

        ~CurrentContext()
        {
            QString error;
            if (!restore(error)) {
                qCritical().noquote() << "WaylandShader:" << error;
            }
        }

        bool enter(EGLContext context, EGLSurface surface, QString& error)
        {
            m_changed = true;
            if (!eglBindAPI(EGL_OPENGL_API)) {
                error = eglError("eglBindAPI(OpenGL)");
                return false;
            }
            if (!eglMakeCurrent(m_target, surface, surface, context)) {
                error = eglError("eglMakeCurrent(private shader context)");
                return false;
            }
            return true;
        }

        bool restore(QString& error)
        {
            if (!m_changed) {
                return true;
            }
            // Unbind with the private API first: the caller may have another API
            // selected and no context, as is usual on the loading worker.
            if (!eglBindAPI(EGL_OPENGL_API)
                || !eglMakeCurrent(m_target, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
                error = eglError("eglMakeCurrent(releasing shader context)");
                return false;
            }
            if (!eglBindAPI(m_api)) {
                error = eglError("eglBindAPI(restoring caller)");
                return false;
            }
            if (m_context != EGL_NO_CONTEXT
                && !eglMakeCurrent(m_display, m_draw, m_read, m_context)) {
                error = eglError("eglMakeCurrent(restoring caller)");
                return false;
            }
            m_changed = false;
            return true;
        }

    private:
        EGLDisplay m_target;
        EGLDisplay m_display;
        EGLContext m_context;
        EGLSurface m_draw;
        EGLSurface m_read;
        EGLenum m_api;
        bool m_changed = false;
    };

    struct Preset {
        libra_shader_preset_t handle = nullptr;
        ~Preset()
        {
            if (handle) {
                discardLibraError(libra_preset_free(&handle));
            }
        }
    };

    struct ParameterList {
        libra_preset_param_list_t value {};
        bool owned = false;
        ~ParameterList()
        {
            if (owned) {
                discardLibraError(libra_preset_free_runtime_params(value));
            }
        }
    };

    const void* loadGl(const char* name)
    {
        return reinterpret_cast<const void*>(eglGetProcAddress(name));
    }

    bool validateValue(const ShaderParameter& parameter, double value, QString& error)
    {
        const float shaderValue = static_cast<float>(value);
        if (!std::isfinite(shaderValue) || shaderValue < parameter.minimum || shaderValue > parameter.maximum) {
            error = QStringLiteral("Parameter '%1' must be finite and between %2 and %3")
                        .arg(parameter.name)
                        .arg(parameter.minimum, 0, 'g', 9)
                        .arg(parameter.maximum, 0, 'g', 9);
            return false;
        }
        return true;
    }

} // namespace

struct ShaderRuntime::Impl {
    struct Output {
        uintptr_t id = 0;
        libra_gl_filter_chain_t chain = nullptr;
    };

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext hostContext = EGL_NO_CONTEXT;
    bool surfaceless = false;
    GLint maximumTextureSize = 0;
    std::vector<Output> outputs;
    QList<ShaderParameter> parameters;
    std::vector<QByteArray> parameterNames;

    ~Impl()
    {
        if (context != EGL_NO_CONTEXT && (surfaceless || surface != EGL_NO_SURFACE)) {
            CurrentContext current(display);
            QString error;
            if (current.enter(context, surface, error)) {
                for (auto& output : outputs) {
                    if (output.chain) {
                        discardLibraError(libra_gl_filter_chain_free(&output.chain));
                    }
                }
                glFlush();
            } else {
                // Calling librashader's GL destructors in the host context is
                // unsafe. This only occurs after EGL context/display loss.
                qCritical().noquote() << "WaylandShader cannot release shader objects:" << error;
            }
            if (!current.restore(error)) {
                qCritical().noquote() << "WaylandShader:" << error;
            }
        }
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
    }

    bool createContext(QString& error)
    {
        EGLint hostApi = 0;
        if (!eglQueryContext(display, hostContext, EGL_CONTEXT_CLIENT_TYPE, &hostApi)) {
            error = eglError("eglQueryContext(host API)");
            return false;
        }
        if (hostApi != EGL_OPENGL_API) {
            error = QStringLiteral("WaylandShader requires KWin's desktop OpenGL EGL renderer, not OpenGL ES");
            return false;
        }
        const EGLenum previousApi = eglQueryAPI();
        if (!eglBindAPI(EGL_OPENGL_API)) {
            error = eglError("eglBindAPI(OpenGL)");
            return false;
        }

        // KWin's GBM EGL display commonly has no pbuffer configs. Prefer
        // surfaceless rendering; retain a tiny pbuffer for older EGL displays.
        surfaceless = epoxy_egl_version(display) >= 15
            || epoxy_has_egl_extension(display, "EGL_KHR_surfaceless_context");
        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, surfaceless ? 0 : EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_NONE
        };
        EGLConfig config = nullptr;
        EGLint count = 0;
        bool ok = eglChooseConfig(display, configAttributes, &config, 1, &count);
        if (!ok) {
            error = eglError("eglChooseConfig(OpenGL)");
        } else if (count == 0) {
            ok = false;
            error = QStringLiteral("KWin's EGL display has no suitable OpenGL %1 config")
                        .arg(surfaceless ? QStringLiteral("surfaceless") : QStringLiteral("pbuffer"));
        } else {
            // Prefer DSA-capable contexts, but retain librashader's GLSL 330
            // path on older drivers. EGL may return a newer compatible version.
            for (const auto version : { std::pair { 4, 6 }, std::pair { 4, 5 }, std::pair { 3, 3 } }) {
                // Shared contexts must use the same reset-notification policy.
                // KWin normally uses robust/lose-on-reset; other hosts may not.
                for (const bool robust : { false, true }) {
                    EGLint attributes[11] = {
                        EGL_CONTEXT_MAJOR_VERSION_KHR, version.first,
                        EGL_CONTEXT_MINOR_VERSION_KHR, version.second,
                        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                        EGL_NONE
                    };
                    if (robust) {
                        attributes[6] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR;
                        attributes[7] = EGL_LOSE_CONTEXT_ON_RESET_KHR;
                        attributes[8] = EGL_CONTEXT_FLAGS_KHR;
                        attributes[9] = EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR;
                        attributes[10] = EGL_NONE;
                    }
                    context = eglCreateContext(display, config, hostContext, attributes);
                    if (context != EGL_NO_CONTEXT) {
                        break;
                    }
                }
                if (context != EGL_NO_CONTEXT) {
                    break;
                }
            }
            ok = context != EGL_NO_CONTEXT;
            if (!ok) {
                error = eglError("eglCreateContext(shared OpenGL 3.3 or newer)");
            } else if (!surfaceless) {
                const EGLint attributes[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
                surface = eglCreatePbufferSurface(display, config, attributes);
                ok = surface != EGL_NO_SURFACE;
                if (!ok) {
                    error = eglError("eglCreatePbufferSurface");
                }
            }
        }
        if (!eglBindAPI(previousApi)) {
            error = eglError("eglBindAPI(restoring caller)");
            return false;
        }
        return ok;
    }
};

ShaderRuntime::ShaderRuntime(std::unique_ptr<Impl> impl)
    : d(std::move(impl))
{
}

ShaderRuntime::~ShaderRuntime() = default;

std::unique_ptr<ShaderRuntime> ShaderRuntime::load(EGLDisplay display, EGLContext shareContext,
    const QString& presetPath,
    const std::vector<uintptr_t>& outputs,
    const QMap<QString, double>& overrides,
    QString& error)
{
    error.clear();
    if (display == EGL_NO_DISPLAY || shareContext == EGL_NO_CONTEXT) {
        error = QStringLiteral("No desktop OpenGL EGL compositor context is available");
        return nullptr;
    }
    const QFileInfo file(presetPath);
    if (!file.isAbsolute() || !file.isFile() || !file.isReadable() || presetPath.contains(QChar::Null)) {
        error = QStringLiteral("Shader preset must be an absolute path to a readable file: %1").arg(presetPath);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->display = display;
    impl->hostContext = shareContext;
    impl->outputs.reserve(std::max(outputs.size(), size_t(1)));
    for (uintptr_t id : outputs) {
        if (std::ranges::any_of(impl->outputs, [id](const auto& output) { return output.id == id; })) {
            error = QStringLiteral("An output was supplied more than once while loading the shader");
            return nullptr;
        }
        impl->outputs.push_back(Impl::Output { .id = id });
    }
    if (outputs.empty()) {
        // Compile once to validate the preset and resolve defaults/overrides.
        // This validation chain is released before returning the prepared preset.
        impl->outputs.push_back({});
    }
    if (!impl->createContext(error)) {
        return nullptr;
    }
    CurrentContext current(display);
    if (!current.enter(impl->context, impl->surface, error)) {
        return nullptr;
    }
    const int version = epoxy_gl_version();
    if (version < 33 || (version < 42 && !epoxy_has_gl_extension("GL_ARB_texture_storage"))) {
        error = QStringLiteral("librashader requires desktop OpenGL 3.3 and GL_ARB_texture_storage (or OpenGL 4.2)");
        return nullptr;
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &impl->maximumTextureSize);
    const QByteArray path = file.canonicalFilePath().toUtf8();
    libra_preset_opt_t presetOptions {};
    presetOptions.version = LIBRASHADER_CURRENT_VERSION;
    presetOptions.original_aspect_uniforms = true;
    presetOptions.frametime_uniforms = true;
    filter_chain_gl_opt_t chainOptions {};
    chainOptions.version = LIBRASHADER_CURRENT_VERSION;
    chainOptions.glsl_version = uint16_t(version * 10);
    chainOptions.use_dsa = version >= 45;
    chainOptions.force_no_mipmaps = false;
    chainOptions.disable_cache = false;

    bool first = true;
    for (auto& output : impl->outputs) {
        Preset preset;
        if (!checkLibra(libra_preset_create_with_options(path.constData(), nullptr, &presetOptions, &preset.handle), error)) {
            error.prepend(QStringLiteral("Could not load '%1': ").arg(presetPath));
            return nullptr;
        }
        if (first) {
            ParameterList list;
            if (!checkLibra(libra_preset_get_runtime_params(&preset.handle, &list.value), error)) {
                return nullptr;
            }
            list.owned = true;
            for (uint64_t i = 0; i < list.value.length; ++i) {
                const auto& meta = list.value.parameters[i];
                ShaderParameter parameter { QString::fromUtf8(meta.name), QString::fromUtf8(meta.description),
                    meta.minimum, meta.maximum, meta.step, meta.initial, meta.initial };
                if (parameter.name.isEmpty() || !std::isfinite(parameter.minimum)
                    || !std::isfinite(parameter.maximum) || parameter.minimum > parameter.maximum
                    || !std::isfinite(parameter.step) || parameter.step < 0
                    || !validateValue(parameter, parameter.defaultValue, error)) {
                    error = QStringLiteral("Invalid shader metadata for parameter '%1': %2").arg(parameter.name, error);
                    return nullptr;
                }
                // Metadata is returned per pass, so common include files
                // repeat parameters. Match the runtime's last-pass-wins map.
                const auto existing = std::find_if(impl->parameters.begin(), impl->parameters.end(),
                    [&](const auto& p) { return p.name == parameter.name; });
                if (existing == impl->parameters.end()) {
                    impl->parameters.push_back(std::move(parameter));
                } else {
                    *existing = std::move(parameter);
                }
            }
            std::sort(impl->parameters.begin(), impl->parameters.end(),
                [](const auto& a, const auto& b) { return a.name < b.name; });
            impl->parameterNames.reserve(size_t(impl->parameters.size()));
            for (const auto& parameter : std::as_const(impl->parameters)) {
                impl->parameterNames.push_back(parameter.name.toUtf8());
            }
            for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
                const auto parameter = std::find_if(impl->parameters.cbegin(), impl->parameters.cend(),
                    [&](const auto& p) { return p.name == it.key(); });
                if (parameter == impl->parameters.cend()) {
                    error = QStringLiteral("Unknown shader parameter '%1'").arg(it.key());
                    return nullptr;
                }
                if (!validateValue(*parameter, it.value(), error)) {
                    return nullptr;
                }
            }
        }
        if (!checkLibra(libra_gl_filter_chain_create(&preset.handle, loadGl, &chainOptions, &output.chain), error)) {
            error.prepend(QStringLiteral("Could not compile '%1': ").arg(presetPath));
            return nullptr;
        }
        uint32_t passes = 0;
        if (!checkLibra(libra_gl_filter_chain_get_active_pass_count(&output.chain, &passes), error)) {
            return nullptr;
        }
        if (passes == 0) {
            error = QStringLiteral("Shader preset has no active passes");
            return nullptr;
        }
        for (qsizetype i = 0; i < impl->parameters.size(); ++i) {
            auto& parameter = impl->parameters[i];
            const char* name = impl->parameterNames[size_t(i)].constData();
            // Unlike preset_get_param, this includes pragma defaults as well
            // as .slangp overrides, and returns an error for an absent name.
            float value = 0;
            if (!checkLibra(libra_gl_filter_chain_get_param(&output.chain, name, &value), error)
                || !validateValue(parameter, value, error)) {
                return nullptr;
            }
            const auto override = overrides.constFind(parameter.name);
            if (override != overrides.cend()) {
                value = float(override.value());
                if (!checkLibra(libra_gl_filter_chain_set_param(&output.chain, name, value), error)) {
                    return nullptr;
                }
            }
            if (!first && parameter.value != double(value)) {
                error = QStringLiteral("Preset parameter '%1' changed while loading the output chains").arg(parameter.name);
                return nullptr;
            }
            parameter.value = value;
        }
        first = false;
    }
    if (outputs.empty()) {
        if (!checkLibra(libra_gl_filter_chain_free(&impl->outputs.front().chain), error)) {
            return nullptr;
        }
        impl->outputs.clear();
    }
    glFlush();
    if (!current.restore(error)) {
        return nullptr;
    }
    return std::unique_ptr<ShaderRuntime>(new ShaderRuntime(std::move(impl)));
}

const QList<ShaderParameter>& ShaderRuntime::parameters() const
{
    return d->parameters;
}

bool ShaderRuntime::setParameter(const QString& name, double value, QString& error)
{
    error.clear();
    const auto parameter = std::find_if(d->parameters.begin(), d->parameters.end(),
        [&](const auto& p) { return p.name == name; });
    if (parameter == d->parameters.end()) {
        error = QStringLiteral("Unknown shader parameter '%1'").arg(name);
        return false;
    }
    if (!validateValue(*parameter, value, error)) {
        return false;
    }
    const auto index = size_t(parameter - d->parameters.begin());
    const char* key = d->parameterNames[index].constData();
    const float requested = float(value);
    if (parameter->value == double(requested)) {
        return true;
    }
    // Preflight every chain: 0.10.1's setter actually inserts an unknown name
    // before returning an error. Never call it on an unverified parameter.
    std::vector<float> previous(d->outputs.size());
    for (size_t i = 0; i < d->outputs.size(); ++i) {
        if (!checkLibra(libra_gl_filter_chain_get_param(&d->outputs[i].chain, key, &previous[i]), error)) {
            return false;
        }
    }
    for (size_t i = 0; i < d->outputs.size(); ++i) {
        if (!checkLibra(libra_gl_filter_chain_set_param(&d->outputs[i].chain, key, requested), error)) {
            for (size_t j = 0; j <= i; ++j) {
                QString rollbackError;
                if (!checkLibra(libra_gl_filter_chain_set_param(&d->outputs[j].chain, key, previous[j]), rollbackError)) {
                    error += QStringLiteral("; restoring output %1 failed: %2").arg(j).arg(rollbackError);
                }
            }
            return false;
        }
    }
    parameter->value = requested;
    return true;
}

bool ShaderRuntime::render(uintptr_t outputId, const ShaderFrame& frame, QString& error)
{
    error.clear();
    const auto output = std::find_if(d->outputs.begin(), d->outputs.end(),
        [outputId](const auto& entry) { return entry.id == outputId; });
    if (output == d->outputs.end()) {
        error = QStringLiteral("No shader chain was loaded for this output");
        return false;
    }
    if (!frame.source || !frame.destination || frame.source == frame.destination || !frame.format
        || frame.size.width() <= 0 || frame.size.height() <= 0
        || frame.size.width() > d->maximumTextureSize || frame.size.height() > d->maximumTextureSize
        || !std::isfinite(frame.framesPerSecond) || frame.framesPerSecond <= 0
        || frame.number > std::numeric_limits<size_t>::max()) {
        error = QStringLiteral("Invalid shader frame: distinct textures, valid dimensions/format and positive finite frame rate are required");
        return false;
    }
    if (eglGetCurrentDisplay() != d->display || eglGetCurrentContext() != d->hostContext
        || eglQueryAPI() != EGL_OPENGL_API) {
        error = QStringLiteral("Shader rendering must run with the original KWin OpenGL EGL context current");
        return false;
    }

    // These host operations change no GL bindings or state tracked by KWin.
    GLsync incoming = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!incoming) {
        error = QStringLiteral("Could not create the compositor-to-shader GPU fence");
        return false;
    }
    glFlush();
    CurrentContext current(d->display);
    if (!current.enter(d->context, d->surface, error)) {
        QString restoreError;
        if (current.restore(restoreError)) {
            glDeleteSync(incoming);
        } else {
            error += QStringLiteral("; ") + restoreError;
        }
        return false;
    }
    glWaitSync(incoming, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(incoming);

    const libra_image_gl_t source { frame.source, frame.format, uint32_t(frame.size.width()), uint32_t(frame.size.height()) };
    const libra_image_gl_t destination { frame.destination, frame.format, source.width, source.height };
    frame_gl_opt_t options {};
    options.version = LIBRASHADER_CURRENT_VERSION;
    options.clear_history = frame.clearHistory;
    options.frame_direction = 1;
    options.rotation = 0;
    options.total_subframes = 1;
    options.current_subframe = 1;
    options.aspect_ratio = float(source.width) / float(source.height);
    options.frames_per_second = frame.framesPerSecond;
    options.frametime_delta = frame.elapsedMilliseconds;
    // The final quad is [0,1], unlike the [-1,1] intermediate quads.
    // This is librashader's DEFAULT_MVP: neither an identity nor a Y flip.
    // A texel at GL bottom-left remains at GL bottom-left in destination.
    static constexpr float mvp[16] = {
        2, 0, 0, 0,
        0, 2, 0, 0,
        0, 0, 0, 0,
        -1, -1, 0, 1
    };
    bool ok = checkLibra(libra_gl_filter_chain_frame(&output->chain, size_t(frame.number),
                             source, destination, nullptr, mvp, &options),
        error);
    // Even an unsuccessful frame may have queued reads or writes. Always
    // order those before the host reuses either texture; never CPU-wait.
    GLsync outgoing = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!outgoing) {
        error = QStringLiteral("Could not create the shader-to-compositor GPU fence");
        ok = false;
    }
    QString restoreError;
    if (!current.restore(restoreError)) {
        error = restoreError;
        return false;
    }
    if (outgoing) {
        glWaitSync(outgoing, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(outgoing);
    }
    return ok;
}

} // namespace WaylandShader
