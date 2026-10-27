# OBS RDP Camera

Use the **OBS virtual camera** as the webcam of a **Remote Desktop (RDP) session**.

The Windows Remote Desktop client (mstsc) can only redirect real camera devices, and the OBS virtual camera
is a DirectShow filter, not a device, so it never shows up under *Local Resources → Video capture devices*.
OBSRdpCam creates a real Windows 11 virtual camera (`MFCreateVirtualCamera`) named **OBS RDP Camera**
that shows whatever the OBS virtual camera outputs, and that RDP can redirect like any webcam.

## Requirements

- Windows 11 on the machine running OBS (the RDP *client*)
- OBS Studio with its built-in virtual camera (tested with OBS 32)
- Nothing to install on the remote machine: it receives a regular redirected camera

## Install

1. Download `OBSRdpCam-x64.zip` from the [releases](../../releases) and extract it
   (right click the zip → Properties → *Unblock* first, the binaries aren't code signed).
2. In an **administrator** PowerShell, from the extracted folder:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
   ```

   Add `-AutoStart` to start the camera when you log on.

This copies the files to `C:\Program Files\OBSRdpCam`, registers the camera source and adds
**OBS RDP Camera** to the Start menu.

## Use

1. In OBS, click **Start Virtual Camera**.
2. Start **OBS RDP Camera** from the Start menu and keep its window open (closing it removes the camera).
3. In mstsc: *Show Options → Local Resources → More… → Video capture devices* → check **OBS RDP Camera**.
   Or point an existing `.rdp` file at it:

   ```powershell
   .\Set-RdpCamera.ps1 -RdpFile "$env:USERPROFILE\Desktop\Work.rdp"
   ```

The camera uses the OBS output resolution and frame rate read when OBS RDP Camera starts: restart it after
changing them in OBS (frames of another size are rescaled meanwhile). The image is black while OBS isn't sending.

## Uninstall

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

## How it works

```
OBS ──(OBSVirtualCamVideo shared memory, user session)──► OBSRdpCam.exe (relay)
                                                              │
                            Global\OBSRdpCamFrames shared memory
                                                              ▼
mstsc / apps ◄── Windows Frame Server (session 0) ◄── OBSRdpCamSource.dll (Media Foundation source)
```

- **OBSRdpCamSource.dll** is the camera's Media Foundation media source. Windows loads it in the Frame Server
  services, which run in session 0 and can't see OBS' session-local shared memory. It creates a global section
  and outputs the NV12 frames found there.
- **OBSRdpCam.exe** registers the virtual camera (it exists while the program runs) and copies each frame from
  the OBS virtual camera shared memory to the global section.

## Security notes

- The DLL is loaded by system services, so it's installed under `Program Files` (admin-only writes) and only reads
  fixed-size, bounds-checked data.
- The global frame section is writable by interactive users at medium integrity or above (not by sandboxed
  processes): any program running in a logged-on user's session could feed frames to the camera.
- `C:\ProgramData\OBSRdpCam\format.txt` (the camera format) is writable by users; its values are validated.

## Build

Visual Studio 2022+ or the Build Tools with the *Desktop development with C++* workload and a Windows 11 SDK:

```powershell
.\build.ps1
```

Produces `dist\OBSRdpCam-x64.zip`. Then `install.ps1` can also be run from the repository root.
Pushing a `v*` tag builds and publishes a GitHub release.

## Credits

Based on [VCamSample](https://github.com/smourier/VCamSample) by Simon Mourier (MIT).
Reads the shared memory layout of the [OBS Studio](https://github.com/obsproject/obs-studio) virtual camera;
no OBS code is included.

## License

[MIT](LICENSE)
