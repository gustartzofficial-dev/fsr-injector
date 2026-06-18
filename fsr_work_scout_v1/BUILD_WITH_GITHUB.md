# Building the DLL with GitHub Actions

This repo includes a GitHub Actions workflow that builds the Windows `dxgi.dll` on GitHub's hosted Windows runner, so you do not need Visual Studio, the Windows SDK, or build dependencies installed locally.

## First-time setup

1. Create a new GitHub repository.
2. Upload or push this project so that `CMakeLists.txt` is at the repository root.
3. Make sure the workflow file exists here:

   ```text
   .github/workflows/build-windows.yml
   ```

## Run the build

1. Open the repository on GitHub.
2. Go to the **Actions** tab.
3. Select **Build Windows DLL**.
4. Click **Run workflow**.
5. Open the completed workflow run.
6. Download the artifact named **fsr-injector-dxgi-dll**.
7. Extract it; the built file should be `dxgi.dll`.

## Testing

Put `dxgi.dll` next to the target game's `.exe` and launch the game normally.

For the DX12 foundation patch, check the log for messages like:

```text
[dx12] captured ID3D12CommandQueue
[overlay] swapchain is DIRECTX 12, using D3D12 backend
[overlay-dx12] initialized
```

## Notes

- The workflow uses `windows-latest`, Visual Studio 2022, x64, and Release config.
- Dependencies such as MinHook and Dear ImGui are fetched by CMake during the build.
- If Dear ImGui changes its DX12 backend API on `master`, the build may fail. In that case, pin ImGui in `CMakeLists.txt` to a known commit/tag or update the DX12 backend init call.


Latest suggested commit name:

```text
Replace DX12 sharpen shader with RCAS-style pass
```


## DX12 native settings overlay update

Suggested commit name: `Add native DX12 settings overlay`

This build keeps Dear ImGui bypassed for DX12 and draws a lightweight native D3D12 overlay directly inside the working EASU/RCAS fullscreen pass. The overlay shows the DX12 UI header, post-process status, scale, sharpness, and a small scale bar. This follows the safer path used by mature DX12 overlays: separate capture/render backend from UI state, keep per-frame synchronization, and avoid the ImGui DX12 backend until descriptor/input issues are isolated.
