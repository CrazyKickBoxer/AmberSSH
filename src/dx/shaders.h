// shaders.h — shader bytecode acquisition. Prefers .cso compiled at build time
// by DXC; falls back to runtime compilation via dxcompiler (also DXC).
#pragma once

#include "../common.h"

struct ShaderBlob
{
    std::vector<uint8_t> bytes;
    D3D12_SHADER_BYTECODE Bytecode() const
    {
        return { bytes.data(), bytes.size() };
    }
};

class ShaderCompiler
{
public:
    bool Init();
    // stem = "particle_sim" (no extension); looks for shaders/<stem>.<entry>.cso
    // next to the exe, else compiles shaders/<stem>.hlsl with DXC.
    ShaderBlob Load(const wchar_t* stem, const wchar_t* entry, const wchar_t* target);

private:
    std::wstring m_shaderDir;
    ComPtr<IUnknown> m_utils;      // IDxcUtils
    ComPtr<IUnknown> m_compiler;   // IDxcCompiler3
    ComPtr<IUnknown> m_includes;   // IDxcIncludeHandler
};
