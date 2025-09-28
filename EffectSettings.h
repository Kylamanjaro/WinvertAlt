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
    // Minimal shader upgrade: 4x4 matrix + offset
    // Row-major color matrix applied to float4(rgb,1)
    float colorMat[16]{
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
    float colorOffset[4]{ 0,0,0,0 };

    // Diagnostics
    bool showFpsOverlay = false;
};
