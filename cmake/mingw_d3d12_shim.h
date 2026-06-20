// Force-included only into ImGui's imgui_impl_dx12.cpp under MinGW.
// MinGW's <d3d12.h> declares PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE but not the
// plain PFN_D3D12_SERIALIZE_ROOT_SIGNATURE that the ImGui DX12 backend references.
// Providing the identical typedef here is harmless even if a future header adds it
// (C++ permits identical typedef redeclaration).
#pragma once
#include <windows.h>
#include <d3d12.h>

typedef HRESULT (WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
