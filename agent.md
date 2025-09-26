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
    - Each thread is spawned as the user attempts to acquire that resource for the first time
    - If a user creates an effect window on a new monitor a new desktop duplication thread is spawned to handle services for any 
      window attempting to acquire that resource or subset of that resource.
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


## Next Steps (template)
Output manager

Enumerate IDXGIAdapter → IDXGIOutputs at startup.

Create a worker thread per output (don’t start duplication yet).

Track hot-plug/rotation with DXGI notifications; add/remove workers as outputs appear/disappear.

Lazy duplication

Each worker owns:

A D3D11 device on the same adapter as its output (or build it lazily).

An IDXGIOutputDuplication created only when subscriber count > 0.

When the last subscriber leaves, release the duplication (free GPU resources) but keep the thread sleeping.

Subscriptions (windows/regions)

Windows register regions in virtual desktop coordinates.

The manager splits requests across outputs by intersecting with outputDesc.DesktopCoordinates.

Each worker keeps a list of regions (per window) to crop/copy.

Frame loop per worker

If subscriber_count == 0: wait on a condition variable; no duplication active.

Else:

AcquireNextFrame(timeout) → copy only the needed rects to per-window shared textures (or CPU staging if you post-process on CPU).

Signal the target window/render thread via lock-free queue/event.

Different refresh rates? Fine: each worker naturally blocks on its own output’s frames; your renderer composes whatever arrives.

Sharing to window threads

Prefer shared D3D11 textures (IDXGIResource1::CreateSharedHandle) and CopySubresourceRegion per rect; the UI thread opens the shared handle and composites with DirectComposition/Direct2D.

If you apply color effects on GPU, do it in the window’s D3D11/DComp pass from the shared texture (constant buffer → pixel shader).

Window spanning multiple monitors

The window manager registers two (or more) sub-rects—one per intersecting output.

You’ll receive independent frame updates from each worker; the window thread composites the sub-textures into the correct places in window space.

Handle scaling/rotation by mapping desktop → output space using outputDesc.DesktopCoordinates and the current transform; apply DPI scaling in your vertex transform.