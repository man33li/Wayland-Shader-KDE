#pragma once

#include "output_settings.h"
#include "shader_runtime.h"

#include <KSharedConfig>
#include <QDBusContext>
#include <QFutureWatcher>
#include <effect/effect.h>
#include <map>
#include <memory>
#include <vector>

namespace KWin {
class GLShader;
}

namespace WaylandShader {

class DesktopEffect final : public KWin::Effect, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.waylandshader.Effect")

public:
    DesktopEffect();
    ~DesktopEffect() override;
    static bool supported();
    bool isActive() const override;
    bool blocksDirectScanout() const override;
    int requestedEffectChainPosition() const override;
    void reconfigure(ReconfigureFlags flags) override;
    void prePaintScreen(KWin::ScreenPrePaintData& data, std::chrono::milliseconds presentTime) override;
    void paintScreen(const KWin::RenderTarget& target, const KWin::RenderViewport& viewport,
        int mask, const KWin::Region& region, KWin::LogicalOutput* screen) override;
    void postPaintScreen() override;

public Q_SLOTS:
    QString status() const;
    bool loadPreset(const QString& path);
    bool setEnabled(bool enabled);
    bool setParameter(const QString& name, double value);
    bool setOutputShaderEnabled(const QString& id, bool enabled);
    bool setOutputColorEnabled(const QString& id, bool enabled);
    bool setOutputGamma(const QString& id, double gamma);
    bool setOutputSaturation(const QString& id, double saturation);

Q_SIGNALS:
    void statusChanged(const QString& json);

private:
    struct OutputState;
    struct LoadResult;
    std::vector<uintptr_t> shaderOutputIds() const;
    void ensureOutputs();
    KWin::LogicalOutput* requireOutput(const QString& id);
    void readOutputSettings(OutputState& state);
    void saveOutputSettings(KWin::LogicalOutput* screen);
    bool processingEnabled() const;
    bool usesShader(KWin::LogicalOutput* screen, const OutputState& state) const;
    void finishLoad();
    void outputsChanged();
    void resetHistory();
    void notifyStatus();
    void renderFailed(const QString& message);
    bool reject(const QString& kind, const QString& message);
    void saveGeneral();
    void saveEnablement();

    KSharedConfig::Ptr m_config;
    std::unique_ptr<ShaderRuntime> m_runtime;
    std::unique_ptr<KWin::GLShader> m_colorShader;
    std::map<KWin::LogicalOutput*, std::unique_ptr<OutputState>> m_outputs;
    std::vector<std::unique_ptr<OutputState>> m_retiredOutputs;
    std::vector<uintptr_t> m_loadedOutputs;
    QFutureWatcher<std::shared_ptr<LoadResult>> m_loader;
    QString m_preset;
    QString m_requestedPreset;
    QString m_error;
    bool m_enabled = true;
    bool m_loading = false;
    bool m_reloadOutputs = false;
    bool m_renderFailed = false;
    bool m_registered = false;
};

}
