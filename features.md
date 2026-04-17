# Winvert Feature Baseline

## Core Capture/Region Features
- Create region overlays from global hotkey capture flow.
- Selection overlay supports drag-rectangle selection across virtual desktop.
- Selection overlay supports full-monitor quick select with number keys `1..9`.
- Multi-monitor spanning selections are split into per-monitor effect windows.
- One logical region can map to multiple physical windows (primary + extras) with shared settings.
- Add region from `+` tab button.
- Remove selected region (UI close).
- Remove last region via hotkey.
- Hide/show selected region.

## Global Hotkeys
- Three global hotkeys:
- Invert/Add (default `Ctrl+Alt+I`)
- Filter/Add (default `Ctrl+Alt+F`)
- Remove Last (default `Ctrl+Alt+R`)
- Rebind each hotkey from settings.
- Hotkeys remain active while control panel is hidden.

## Effects
- Invert effect toggle per region.
- Brightness protection toggle per region.
- Custom filter pipeline (matrix + offsets) per region.
- Filters are additive/composable: multiple selected filters are combined into a single resulting transform (matrix/offset composition).
- Filter flyout for applying saved filters per tab.
- Favorite filter selection used by hotkey add/filter flow.

## Custom Filter Editor
- Simple mode sliders: brightness, contrast, saturation, hue, temperature, tint.
- Advanced mode 5x5 matrix editor.
- Save / delete / clear custom filters.
- Preview toggle for filters (temporary apply while editing).

## Color Mapping
- Global color mapping list with enable/disable per map row.
- Source and destination color swatches.
- Tolerance control per map.
- Preserve-brightness toggle for mapping.
- Add/remove map entries.
- Preview toggle for color mapping.
- Eyedropper-style color sample mode for source color picking.

## UI/Workflow
- Tabbed control panel for regions.
- Settings panel with back navigation.
- Info bar when no active regions.
- Control panel starts hidden/headless and appears after region creation.
- `X` close hides app (keeps background process alive) rather than full exit.

## Startup + Packaging Behavior
- MSIX `StartupTask` support (`Run at startup` toggle).
- Startup task state read/enable/disable from app settings.
- Startup/hotkey diagnostics logged (debug-enabled builds).
- Local/Store package support (MSIX/Appxupload workflow).

## State Persistence
- App state saved/loaded from LocalState JSON.
- Persists hotkeys, toggles, filters, color maps, favorite filter index, selection color, brightness settings.

## Diagnostics/Performance Related
- Logging framework with build flag (Debug default on, Release default off).
- Hotkey registration success/failure logging.
- Output/duplication prewarm path on selection start.
- Shader compile cache (one-time compile reused across windows).
