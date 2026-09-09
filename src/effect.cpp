#include "effect.h"

#include <core/colorspace.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <opengl/glframebuffer.h>
#include <opengl/glshader.h>
#include <opengl/glshadermanager.h>
#include <opengl/gltexture.h>

#include <KConfigGroup>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>

namespace WaylandShader {
using namespace KWin;

struct DesktopEffect::OutputState {
    QString id;
    OutputSettings settings;
    std::unique_ptr<GLTexture> source;
    std::unique_ptr<GLTexture> destination;
    std::unique_ptr<GLTexture> retiredDestination;
    std::unique_ptr<GLFramebuffer> framebuffer;
    std::chrono::milliseconds presentTime {};
    std::chrono::milliseconds previousTime {};
    uint64_t frame = 0;
    bool sourceMipmaps = false;
    bool reset = true;
};

struct DesktopEffect::LoadResult {
    std::unique_ptr<ShaderRuntime> runtime;
    QString preset;
    QString error;
    std::vector<uintptr_t> outputs;
};

static KConfigGroup parameterGroup(const KSharedConfig::Ptr& config, const QString& preset)
{
    const QString key = QString::fromLatin1(QCryptographicHash::hash(preset.toUtf8(), QCryptographicHash::Sha256).toHex());
    return KConfigGroup(config, QStringLiteral("Parameters/%1").arg(key));
}

static KConfigGroup outputGroup(const KSharedConfig::Ptr& config, const QString& id)
{
    const QString key = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex());
    return KConfigGroup(config, QStringLiteral("Outputs/%1").arg(key));
}

void DesktopEffect::readOutputSettings(OutputState& state)
{
    const auto group = outputGroup(m_config, state.id);
    state.settings.shaderEnabled = group.readEntry("ShaderEnabled", true);
    state.settings.colorEnabled = group.readEntry("ColorEnabled", false);
    state.settings.gamma = group.readEntry("Gamma", 1.0);
    state.settings.saturation = group.readEntry("Saturation", 1.0);
    if (!std::isfinite(state.settings.gamma) || state.settings.gamma < OutputSettings::MinimumGamma
        || state.settings.gamma > OutputSettings::MaximumGamma
        || !std::isfinite(state.settings.saturation) || state.settings.saturation < OutputSettings::MinimumSaturation
        || state.settings.saturation > OutputSettings::MaximumSaturation) {
        state.settings.colorEnabled = false;
        state.settings.gamma = 1.0;
        state.settings.saturation = 1.0;
        m_error = QStringLiteral("Invalid saved color settings for output %1; color adjustments are disabled.").arg(state.id);
        qWarning().noquote() << "WaylandShader:" << m_error;
    }
}

void DesktopEffect::ensureOutputs()
{
    for (const auto screen : effects->screens()) {
        if (screen->isPlaceholder() || m_outputs.contains(screen)) {
            continue;
        }
        auto state = std::make_unique<OutputState>();
        state->id = screen->uuid();
        if (state->id.isEmpty()) {
            state->id = QStringLiteral("connector:%1").arg(screen->name());
        }
        readOutputSettings(*state);
        m_outputs.emplace(screen, std::move(state));
    }
}

LogicalOutput* DesktopEffect::requireOutput(const QString& id)
{
    for (const auto& [screen, state] : m_outputs) {
        if (state->id == id) {
            return screen;
        }
    }
    reject(QStringLiteral("InvalidArgument"), QStringLiteral("No connected output has id %1.").arg(id));
    return nullptr;
}

void DesktopEffect::saveOutputSettings(LogicalOutput* screen)
{
    const auto& state = *m_outputs.at(screen);
    auto group = outputGroup(m_config, state.id);
    group.writeEntry("ShaderEnabled", state.settings.shaderEnabled);
    group.writeEntry("ColorEnabled", state.settings.colorEnabled);
    group.writeEntry("Gamma", state.settings.gamma);
    group.writeEntry("Saturation", state.settings.saturation);
    m_config->sync();
    saveEnablement();
    effects->addRepaint(screen->geometry());
    notifyStatus();
}

DesktopEffect::DesktopEffect()
    : m_config(KSharedConfig::openConfig(QStringLiteral("waylandshaderrc"), KConfig::SimpleConfig))
{
    m_registered = QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/WaylandShader"), this,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    if (!m_registered) {
        m_error = QStringLiteral("Could not register the WaylandShader control interface.");
        qWarning().noquote() << m_error;
        return;
    }

    connect(&m_loader, &QFutureWatcher<std::shared_ptr<LoadResult>>::finished, this, &DesktopEffect::finishLoad);
    connect(effects, &EffectsHandler::screenAdded, this, [this] { outputsChanged(); });
    connect(effects, &EffectsHandler::screenRemoved, this, [this](LogicalOutput* screen) {
        effects->makeOpenGLContextCurrent();
        const auto it = m_outputs.find(screen);
        if (it != m_outputs.end()) {
            if (m_runtime && std::binary_search(m_loadedOutputs.begin(), m_loadedOutputs.end(), reinterpret_cast<uintptr_t>(screen))) {
                m_retiredOutputs.push_back(std::move(it->second));
            }
            m_outputs.erase(it);
        }
        outputsChanged();
    });
    connect(effects, &EffectsHandler::screenLockingChanged, this, [this] {
        resetHistory();
        effects->addRepaintFull();
        notifyStatus();
    });

    ensureOutputs();
    const KConfigGroup general(m_config, QStringLiteral("General"));
    m_enabled = KConfigGroup(effects->config(), QStringLiteral("Plugins")).readEntry("waylandshaderEnabled", true);
    const QString preset = general.readEntry("Preset", QString());
    if (!preset.isEmpty()) {
        QTimer::singleShot(0, this, [this, preset] { loadPreset(preset); });
    }
}

DesktopEffect::~DesktopEffect()
{
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/WaylandShader"));
    }
    // The worker shares KWin's context group: it must finish before KWin tears that group down.
    disconnect(&m_loader, nullptr, this, nullptr);
    m_loader.waitForFinished();
    effects->makeOpenGLContextCurrent();
    if (m_loader.future().isValid()) {
        m_loader.result()->runtime.reset();
    }
    m_runtime.reset();
    m_outputs.clear();
    m_retiredOutputs.clear();
    m_colorShader.reset();
}

bool DesktopEffect::supported()
{
    return effects->isOpenGLCompositing() && eglGetCurrentDisplay() != EGL_NO_DISPLAY
        && eglGetCurrentContext() != EGL_NO_CONTEXT && epoxy_is_desktop_gl() && epoxy_gl_version() >= 33;
}

bool DesktopEffect::processingEnabled() const
{
    return m_enabled && !m_renderFailed && !effects->isScreenLocked();
}

bool DesktopEffect::usesShader(LogicalOutput* screen, const OutputState& state) const
{
    return state.settings.shaderEnabled && m_runtime
        && std::binary_search(m_loadedOutputs.begin(), m_loadedOutputs.end(), reinterpret_cast<uintptr_t>(screen));
}

bool DesktopEffect::isActive() const
{
    if (!processingEnabled()) {
        return false;
    }
    return std::ranges::any_of(m_outputs, [this](const auto& entry) {
        return usesShader(entry.first, *entry.second) || entry.second->settings.adjustsColor();
    });
}

bool DesktopEffect::blocksDirectScanout() const
{
    return isActive();
}

int DesktopEffect::requestedEffectChainPosition() const
{
    // Wrap the other desktop effects, rather than filtering each individual window.
    return 1;
}

void DesktopEffect::reconfigure(ReconfigureFlags)
{
    m_config->reparseConfiguration();
    const KConfigGroup general(m_config, QStringLiteral("General"));
    ensureOutputs();
    for (auto& [screen, state] : m_outputs) {
        readOutputSettings(*state);
    }
    m_enabled = KConfigGroup(effects->config(), QStringLiteral("Plugins")).readEntry("waylandshaderEnabled", true);
    const QString preset = general.readEntry("Preset", QString());
    if (!m_loading && !preset.isEmpty() && preset != m_preset) {
        loadPreset(preset);
    }
    resetHistory();
    effects->addRepaintFull();
    notifyStatus();
    outputsChanged();
}

std::vector<uintptr_t> DesktopEffect::shaderOutputIds() const
{
    std::vector<uintptr_t> result;
    result.reserve(m_outputs.size());
    for (const auto& [screen, state] : m_outputs) {
        if (state->settings.shaderEnabled) {
            result.push_back(reinterpret_cast<uintptr_t>(screen));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

QString DesktopEffect::status() const
{
    QJsonArray parameters;
    if (m_runtime) {
        for (const auto& parameter : m_runtime->parameters()) {
            parameters.append(QJsonObject {
                { QStringLiteral("name"), parameter.name },
                { QStringLiteral("description"), parameter.description },
                { QStringLiteral("minimum"), parameter.minimum },
                { QStringLiteral("maximum"), parameter.maximum },
                { QStringLiteral("step"), parameter.step },
                { QStringLiteral("default"), parameter.defaultValue },
                { QStringLiteral("value"), parameter.value },
            });
        }
    }
    QJsonArray outputs;
    const bool processing = processingEnabled();
    for (const auto& [screen, output] : m_outputs) {
        outputs.append(QJsonObject {
            { QStringLiteral("id"), output->id },
            { QStringLiteral("name"), screen->name() },
            { QStringLiteral("shaderEnabled"), output->settings.shaderEnabled },
            { QStringLiteral("colorEnabled"), output->settings.colorEnabled },
            { QStringLiteral("gamma"), output->settings.gamma },
            { QStringLiteral("saturation"), output->settings.saturation },
            { QStringLiteral("shaderActive"), processing && usesShader(screen, *output) },
            { QStringLiteral("colorActive"), processing && output->settings.adjustsColor() },
        });
    }
    const QJsonObject state {
        { QStringLiteral("enabled"), m_enabled },
        { QStringLiteral("active"), isActive() },
        { QStringLiteral("loading"), m_loading },
        { QStringLiteral("preset"), m_preset },
        { QStringLiteral("requestedPreset"), m_requestedPreset },
        { QStringLiteral("error"), m_error },
        { QStringLiteral("parameters"), parameters },
        { QStringLiteral("outputs"), outputs },
    };
    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

bool DesktopEffect::reject(const QString& kind, const QString& message)
{
    if (calledFromDBus()) {
        sendErrorReply(QStringLiteral("org.waylandshader.Error.") + kind, message);
    } else {
        m_error = message;
        qWarning().noquote() << "WaylandShader:" << message;
        notifyStatus();
    }
    return false;
}

bool DesktopEffect::loadPreset(const QString& path)
{
    if (m_loading) {
        return reject(QStringLiteral("Busy"), QStringLiteral("A shader preset is already compiling."));
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    if (path.contains(QChar::Null) || !file.isAbsolute() || !file.isFile() || !file.isReadable()
        || canonical.isEmpty() || file.suffix().compare(QStringLiteral("slangp"), Qt::CaseInsensitive) != 0) {
        return reject(QStringLiteral("InvalidArgument"), QStringLiteral("Select a readable .slangp preset using an absolute path."));
    }
    if (!effects->makeOpenGLContextCurrent()) {
        return reject(QStringLiteral("Failed"), QStringLiteral("KWin's OpenGL context is unavailable."));
    }
    const EGLDisplay display = eglGetCurrentDisplay();
    const EGLContext context = eglGetCurrentContext();
    const auto outputs = shaderOutputIds();

    QMap<QString, double> overrides;
    const auto saved = parameterGroup(m_config, canonical).entryMap();
    for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
        bool valid = false;
        const double value = it.value().toDouble(&valid);
        if (!valid || !std::isfinite(value)) {
            return reject(QStringLiteral("InvalidArgument"), QStringLiteral("Invalid saved value for shader parameter %1 in waylandshaderrc.").arg(it.key()));
        }
        overrides.insert(it.key(), value);
    }

    auto result = std::make_shared<LoadResult>();
    result->preset = canonical;
    result->outputs = outputs;
    m_requestedPreset = canonical;
    m_loading = true;
    m_error.clear();
    m_loader.setFuture(QtConcurrent::run([display, context, overrides, result] {
        try {
            result->runtime = ShaderRuntime::load(display, context, result->preset, result->outputs, overrides, result->error);
        } catch (const std::exception& error) {
            result->error = QString::fromUtf8(error.what());
        } catch (...) {
            result->error = QStringLiteral("Unexpected failure while loading the shader runtime.");
        }
        return result;
    }));
    notifyStatus();
    return true;
}

void DesktopEffect::finishLoad()
{
    auto result = m_loader.result();
    m_loading = false;
    m_requestedPreset.clear();
    effects->makeOpenGLContextCurrent();
    if (result->runtime) {
        m_runtime = std::move(result->runtime);
        m_retiredOutputs.clear();
        for (auto& [screen, state] : m_outputs) {
            state->retiredDestination.reset();
            if (!state->settings.shaderEnabled) {
                state->destination.reset();
                if (!state->settings.adjustsColor()) {
                    state->framebuffer.reset();
                    state->source.reset();
                }
            }
        }
        m_preset = result->preset;
        m_loadedOutputs = result->outputs;
        m_error.clear();
        m_renderFailed = false;
        resetHistory();
        saveGeneral();
    } else {
        m_error = result->error.isEmpty() ? QStringLiteral("The shader runtime could not load this preset.") : result->error;
        qWarning().noquote() << "WaylandShader:" << m_error;
    }
    effects->addRepaintFull();
    notifyStatus();
    if (m_reloadOutputs || result->outputs != shaderOutputIds()) {
        m_reloadOutputs = false;
        // The old chains remain usable on unchanged outputs until replacements are ready.
        QTimer::singleShot(0, this, [this] {
            if (!m_loading && !m_preset.isEmpty()) {
                loadPreset(m_preset);
            }
        });
    }
}

bool DesktopEffect::setEnabled(bool enabled)
{
    if (enabled && m_renderFailed) {
        return reject(QStringLiteral("Failed"), QStringLiteral("Reload a working preset before enabling: %1").arg(m_error));
    }
    if (m_enabled != enabled) {
        m_enabled = enabled;
        resetHistory();
        saveGeneral();
        effects->addRepaintFull();
        notifyStatus();
    }
    return true;
}

bool DesktopEffect::setParameter(const QString& name, double value)
{
    if (m_loading) {
        return reject(QStringLiteral("Busy"), QStringLiteral("Wait for the shader preset to finish compiling."));
    }
    if (!m_runtime) {
        return reject(QStringLiteral("Failed"), QStringLiteral("Load a shader preset first."));
    }
    const auto& parameters = m_runtime->parameters();
    const auto parameter = std::find_if(parameters.cbegin(), parameters.cend(), [&name](const auto& item) { return item.name == name; });
    const float shaderValue = static_cast<float>(value);
    if (parameter == parameters.cend() || !std::isfinite(shaderValue) || shaderValue < parameter->minimum || shaderValue > parameter->maximum) {
        return reject(QStringLiteral("InvalidArgument"), QStringLiteral("Unknown shader parameter or value outside its declared range: %1.").arg(name));
    }
    QString error;
    if (!m_runtime->setParameter(name, value, error)) {
        return reject(QStringLiteral("Failed"), error);
    }
    parameterGroup(m_config, m_preset).writeEntry(name, value);
    m_config->sync();
    effects->addRepaintFull();
    notifyStatus();
    return true;
}

bool DesktopEffect::setOutputShaderEnabled(const QString& id, bool enabled)
{
    const auto screen = requireOutput(id);
    if (!screen) {
        return false;
    }
    auto& state = *m_outputs.at(screen);
    if (state.settings.shaderEnabled != enabled) {
        state.settings.shaderEnabled = enabled;
        state.reset = true;
        state.frame = 0;
        state.previousTime = {};
        saveOutputSettings(screen);
        outputsChanged();
    }
    return true;
}

bool DesktopEffect::setOutputColorEnabled(const QString& id, bool enabled)
{
    const auto screen = requireOutput(id);
    if (!screen) {
        return false;
    }
    auto& state = *m_outputs.at(screen);
    if (state.settings.colorEnabled != enabled) {
        state.settings.colorEnabled = enabled;
        saveOutputSettings(screen);
    }
    return true;
}

bool DesktopEffect::setOutputGamma(const QString& id, double gamma)
{
    if (!std::isfinite(gamma) || gamma < OutputSettings::MinimumGamma || gamma > OutputSettings::MaximumGamma) {
        return reject(QStringLiteral("InvalidArgument"), QStringLiteral("Gamma must be finite and between 0.1 and 5."));
    }
    const auto screen = requireOutput(id);
    if (!screen) {
        return false;
    }
    auto& state = *m_outputs.at(screen);
    if (state.settings.gamma != gamma) {
        state.settings.gamma = gamma;
        saveOutputSettings(screen);
    }
    return true;
}

bool DesktopEffect::setOutputSaturation(const QString& id, double saturation)
{
    if (!std::isfinite(saturation) || saturation < OutputSettings::MinimumSaturation || saturation > OutputSettings::MaximumSaturation) {
        return reject(QStringLiteral("InvalidArgument"), QStringLiteral("Saturation must be finite and between 0 and 2."));
    }
    const auto screen = requireOutput(id);
    if (!screen) {
        return false;
    }
    auto& state = *m_outputs.at(screen);
    if (state.settings.saturation != saturation) {
        state.settings.saturation = saturation;
        saveOutputSettings(screen);
    }
    return true;
}

void DesktopEffect::saveGeneral()
{
    KConfigGroup general(m_config, QStringLiteral("General"));
    general.writeEntry("Preset", m_preset);
    m_config->sync();
    // Share Plasma's effect switch instead of keeping a second persistent enable flag.
    saveEnablement();
}

void DesktopEffect::saveEnablement()
{
    KConfigGroup plugins(effects->config(), QStringLiteral("Plugins"));
    plugins.writeEntry("waylandshaderEnabled", m_enabled);
    plugins.sync();
}

void DesktopEffect::notifyStatus()
{
    Q_EMIT statusChanged(status());
}

void DesktopEffect::resetHistory()
{
    for (auto& [screen, state] : m_outputs) {
        state->reset = true;
        state->frame = 0;
        state->previousTime = {};
    }
}

void DesktopEffect::outputsChanged()
{
    ensureOutputs();
    notifyStatus();
    if (m_loading) {
        m_reloadOutputs = true;
    } else if (!m_preset.isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            // A removed output can return at the same address before this callback runs.
            if (!m_loading && (m_loadedOutputs != shaderOutputIds() || !m_retiredOutputs.empty())) {
                loadPreset(m_preset);
            }
        });
    }
}

void DesktopEffect::prePaintScreen(ScreenPrePaintData& data, std::chrono::milliseconds presentTime)
{
    const auto found = m_outputs.find(data.screen);
    if (processingEnabled() && found != m_outputs.end()
        && (usesShader(data.screen, *found->second) || found->second->settings.adjustsColor())) {
        auto& state = *found->second;
        state.presentTime = presentTime;
        data.mask |= PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_BACKGROUND_FIRST;
        data.mask &= ~PAINT_SCREEN_REGION;
        data.paint = Region::infinite();
    }
    effects->prePaintScreen(data, presentTime);
}

void DesktopEffect::renderFailed(const QString& message)
{
    m_renderFailed = true;
    m_error = message;
    qWarning().noquote() << "WaylandShader:" << message;
    effects->addRepaintFull();
    QTimer::singleShot(0, this, &DesktopEffect::notifyStatus);
}

void DesktopEffect::paintScreen(const RenderTarget& target, const RenderViewport& viewport,
    int mask, const Region& region, LogicalOutput* screen)
{
    const auto found = m_outputs.find(screen);
    if (!processingEnabled() || found == m_outputs.end()) {
        effects->paintScreen(target, viewport, mask, region, screen);
        return;
    }
    auto& state = *found->second;
    const bool applyShader = usesShader(screen, state);
    const bool applyColor = state.settings.adjustsColor();
    if (!applyShader && !applyColor) {
        effects->paintScreen(target, viewport, mask, region, screen);
        return;
    }
    const QSize size = viewport.scaledRenderRect().size();
    if (size.isEmpty()) {
        effects->paintScreen(target, viewport, mask, region, screen);
        return;
    }
    if (!state.source || state.source->size() != size || state.sourceMipmaps != applyShader) {
        state.framebuffer.reset();
        // Only Slang inputs require complete mip storage; color-only rendering does not.
        const int levels = applyShader ? std::bit_width(static_cast<unsigned>(std::max(size.width(), size.height()))) : 1;
        state.source = GLTexture::allocate(GL_RGBA16F, size, levels);
        if (state.source) {
            state.source->setFilter(GL_NEAREST);
            state.source->setWrapMode(GL_CLAMP_TO_EDGE);
            state.framebuffer = std::make_unique<GLFramebuffer>(state.source.get());
        }
        state.sourceMipmaps = applyShader;
        state.reset = true;
        state.frame = 0;
        state.previousTime = {};
    }
    if (applyShader && (!state.destination || state.destination->size() != size)) {
        // The old private runtime may still have the previous destination attached.
        if (state.destination) {
            state.retiredDestination = std::move(state.destination);
        }
        state.destination = GLTexture::allocate(GL_RGBA16F, size);
        if (state.destination) {
            state.destination->setFilter(GL_NEAREST);
            state.destination->setWrapMode(GL_CLAMP_TO_EDGE);
        }
    }
    if (!state.framebuffer || !state.framebuffer->valid() || (applyShader && !state.destination)) {
        renderFailed(QStringLiteral("Could not allocate desktop effect render targets for %1.").arg(screen->name()));
        effects->paintScreen(target, viewport, mask, region, screen);
        return;
    }

    auto manager = ShaderManager::instance();
    if (applyColor && !m_colorShader) {
        // Fuse adjustments into the existing final draw; keep KWin's output conversion.
        m_colorShader = manager->generateCustomShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace, {},
            QByteArrayLiteral(R"(#version 140
#include "colormanagement.glsl"
uniform sampler2D sampler;
uniform float inverseGamma;
uniform float outputSaturation;
in vec2 texcoord0;
out vec4 fragColor;
void main()
{
    vec4 color = texture(sampler, texcoord0);
    color.rgb = color.a > 0.0
        ? color.a * pow(max(color.rgb / color.a, vec3(0.0)), vec3(inverseGamma))
        : vec3(0.0);
    color = encodingToNits(color, sourceNamedTransferFunction, sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    color.rgb = max(mix(vec3(luminance), color.rgb, outputSaturation), vec3(0.0));
    color.rgb = (colorimetryTransform * vec4(color.rgb, 1.0)).rgb;
    color.rgb = doTonemapping(color.rgb);
    fragColor = nitsToDestinationEncoding(color);
}
)"));
        if (!m_colorShader || !m_colorShader->isValid()) {
            m_colorShader.reset();
            renderFailed(QStringLiteral("Could not compile the monitor color adjustment shader."));
            effects->paintScreen(target, viewport, mask, region, screen);
            return;
        }
    }

    // Desktop controls and RetroArch presets both operate on display-referred SDR sRGB.
    const RenderTarget sourceTarget(state.framebuffer.get(), ColorDescription::sRGB);
    const RenderViewport sourceViewport(viewport.renderRect(), viewport.scale(), sourceTarget, QPoint());
    GLFramebuffer::pushFramebuffer(state.framebuffer.get());
    effects->paintScreen(sourceTarget, sourceViewport, mask | PAINT_SCREEN_BACKGROUND_FIRST, Region::infinite(), screen);
    GLFramebuffer::popFramebuffer();

    if (applyShader) {
        const auto elapsed = state.reset ? 0LL : std::max<int64_t>(0, (state.presentTime - state.previousTime).count());
        const ShaderFrame frame {
            .source = state.source->texture(),
            .destination = state.destination->texture(),
            .format = GL_RGBA16F,
            .size = size,
            .number = state.frame,
            .framesPerSecond = static_cast<float>(screen->refreshRate()) / 1000.0f,
            .elapsedMilliseconds = static_cast<uint32_t>(std::min<int64_t>(elapsed, std::numeric_limits<uint32_t>::max())),
            .clearHistory = state.reset,
        };
        QString error;
        if (!m_runtime->render(reinterpret_cast<uintptr_t>(screen), frame, error)) {
            renderFailed(QStringLiteral("Shader rendering failed on %1: %2").arg(screen->name(), error));
            effects->paintScreen(target, viewport, mask, region, screen);
            return;
        }
        ++state.frame;
        state.previousTime = state.presentTime;
        state.reset = false;
        state.retiredDestination.reset();
    }

    GLShader* shader;
    if (applyColor) {
        shader = m_colorShader.get();
        manager->pushShader(shader);
        shader->setUniform("inverseGamma", static_cast<float>(1.0 / state.settings.gamma));
        shader->setUniform("outputSaturation", static_cast<float>(state.settings.saturation));
    } else {
        shader = manager->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
    }
    QMatrix4x4 matrix = viewport.projectionMatrix();
    matrix.translate(viewport.scaledRenderRect().x(), viewport.scaledRenderRect().y());
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, matrix);
    shader->setColorspaceUniforms(ColorDescription::sRGB, target.colorDescription(), RenderingIntent::RelativeColorimetricWithBPC);
    shader->setUniform("sampler", 0);

    const bool blend = glIsEnabled(GL_BLEND);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint activeTexture = 0;
    GLint texture = 0;
    GLint sampler = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindSampler(0, 0);
    const auto image = applyShader ? state.destination.get() : state.source.get();
    image->bind();
    image->render(QSizeF(size));
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindSampler(0, sampler);
    glActiveTexture(activeTexture);
    if (blend) {
        glEnable(GL_BLEND);
    }
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    manager->popShader();
}

void DesktopEffect::postPaintScreen()
{
    effects->postPaintScreen();
    if (processingEnabled()) {
        // Only temporal Slang chains need continuous frames. Static color controls
        // follow normal scene damage, and untouched monitors are not forced to redraw.
        for (const auto& [screen, state] : m_outputs) {
            if (usesShader(screen, *state)) {
                effects->addRepaint(screen->geometry());
            }
        }
    }
}

}
