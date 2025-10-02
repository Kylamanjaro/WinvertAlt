#pragma once

#include <vector>

// A single color mapping entry
struct ColorMapEntry
{
    bool enabled{ true };
    uint8_t srcR{ 0 }, srcG{ 0 }, srcB{ 0 };
    uint8_t dstR{ 0 }, dstG{ 0 }, dstB{ 0 };
    int tolerance{ 16 }; // 0-255 range tolerance
};

// Defines the settings for a single effect window.
struct EffectSettings
{
    // Basic Effects
    bool isInvertEffectEnabled = false;
    bool isGrayscaleEffectEnabled = false;

    // Brightness Protection
    bool isBrightnessProtectionEnabled = false;
    int brightnessThreshold = 220;

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

    // Luminance weights for brightness protection and grayscale
    float lumaWeights[3]{ 0.2126f, 0.7152f, 0.0722f };

    // Diagnostics
    bool showFpsOverlay = false;

    // Color mapping toggle per window (mapping list is global in MainWindow)
    bool isColorMappingEnabled = false;

    // Snapshot of color maps to apply (copied from MainWindow's global list when applied)
    std::vector<ColorMapEntry> colorMaps;
};
