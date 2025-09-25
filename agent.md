# agents.md

## Project Overview
This project is a **Windows desktop application** that replicates and extends the functionality of the **Windows Magnification API**, but implemented with **DXGI Desktop Duplication** and **Direct3D 11** instead of GDI.

The application presents a **rectangular magnifier window** that displays a user-selected portion of the desktop, with GPU-accelerated color effects applied in real time. It is intended to be distributed through the **Microsoft Store** as a packaged (MSIX) application.

---

## Motivation
The built-in Magnification API is limited and dated:
- Supports only linear 5×5 color matrices.  
- Relies on GDI for rendering, which is less efficient on modern hardware.  
- Synchronizes across windows, creating bottlenecks in multi-monitor setups.  

By adopting **Desktop Duplication** and **GPU shaders**:
- Each monitor can be captured independently at its native refresh rate.  
- Arbitrary color transforms can be applied, including matrices, LUTs, or ICC profile mappings.  
- Rendering is fully GPU-accelerated, allowing for smooth performance at high resolutions.

---

## Architecture
- **Capture Layer**  
  - One duplication thread per monitor using `IDXGIOutputDuplication`.  
  - Threads block on `AcquireNextFrame` to remain in sync with each display.  
  - Captured frames are exposed as `ID3D11Texture2D`.  

- **Rendering Layer**  
  - Each magnifier window owns a DXGI swap chain.  
  - Captured textures are sampled in a pixel shader.  
  - Color transforms are applied in the shader (matrix or LUT).  
  - Cursor shapes can be drawn using duplication metadata.  

- **Application Layer**  
  - Built as a **WinUI 3 packaged desktop app** (C++ or C#).  
  - Provides the user interface, hotkeys, and configuration.  
  - Bridges to the capture/rendering engine directly or through a native component.  

---

## Key Features
- Define a **source rectangle** anywhere on the virtual desktop.  
- Apply **real-time color transforms** similar to `MagSetColorEffect`, plus more advanced mapping options.  
- Support **multiple magnifier windows** across different monitors.  
- Stay **synchronized with monitor refresh rates** without blocking unrelated outputs.  
- Deliver through the **Microsoft Store** with modern deployment (MSIX packaging).  

---

## Agent Rules
- Ignore the following folders:
  - Assets/
  - bin/
  - obj/
  - Properties/
- Agent is not permitted to build the project
- Agent is not permitted to add Nuget Packages
  - If a package is to be added you must request for it
- Agent is not permitted to create new files or move files within the project
  - Agent must request for these actions to be done