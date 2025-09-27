#pragma once

// Defines the settings for a single effect window.
struct EffectSettings
{
    // Basic Effects
    bool isInvertEffectEnabled = false;
    bool isGrayscaleEffectEnabled = false;

    // Brightness Protection
    bool isBrightnessProtectionEnabled = false;
    int brightnessThreshold = 220;
    int brightnessProtectionDelay = 1000;

    // Custom Matrix Filter
    bool isCustomEffectActive = false;
    // MAGCOLOREFFECT customEffectMatrix; // Placeholder for a 5x5 matrix

    // Diagnostics
    bool showFpsOverlay = false;
};
