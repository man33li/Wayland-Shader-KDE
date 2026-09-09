#include "effect.h"

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(WaylandShader::DesktopEffect, "metadata.json",
    return WaylandShader::DesktopEffect::supported();
    ,
    return false;)

#include "plugin.moc"
