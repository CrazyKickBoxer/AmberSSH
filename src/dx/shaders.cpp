#include "shaders.h"

#if __has_include(<directx-dxc/dxcapi.h>)
#include <directx-dxc/dxcapi.h>
#else
#include <dxcapi.h>
#endif
#include <fstream>

bool ShaderCompiler::Init()
{
    m_shaderDir = ExeDir() + L"\\shaders";

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        return false;

    ComPtr<IDxcIncludeHandler> inc;
    if (FAILED(utils->CreateDefaultIncludeHandler(&inc)))
        return false;

    utils.As(&m_utils);
    compiler.As(&m_compiler);
    inc.As(&m_includes);
    return true;
}

static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    std::streamsize size = f.tellg();
    if (size <= 0)
        return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good();
}

ShaderBlob ShaderCompiler::Load(const wchar_t* stem, const wchar_t* entry,
                                const wchar_t* target)
{
    ShaderBlob blob;

    // 1) Build-time compiled DXIL.
    std::wstring cso = m_shaderDir + L"\\" + stem + L"." + entry + L".cso";
    if (ReadFileBytes(cso, blob.bytes))
        return blob;

    // 2) Runtime DXC compile of the HLSL source.
    std::wstring hlsl = m_shaderDir + L"\\" + stem + L".hlsl";
    std::vector<uint8_t> src;
    if (!ReadFileBytes(hlsl, src))
        throw std::runtime_error("Shader not found: missing both .cso and .hlsl");

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> inc;
    m_utils.As(&utils);
    m_compiler.As(&compiler);
    m_includes.As(&inc);

    DxcBuffer buffer = {};
    buffer.Ptr = src.data();
    buffer.Size = src.size();
    buffer.Encoding = DXC_CP_UTF8;

    std::wstring incArg = m_shaderDir;
    LPCWSTR args[] = {
        stem, L"-E", entry, L"-T", target,
        L"-I", incArg.c_str(),
        L"-O3",
#ifdef _DEBUG
        L"-Zi", L"-Qembed_debug",
#endif
    };

    ComPtr<IDxcResult> result;
    ThrowIfFailed(compiler->Compile(&buffer, args, _countof(args), inc.Get(),
                                    IID_PPV_ARGS(&result)),
                  "DXC Compile");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        std::string msg = "Shader compile failed: ";
        if (errors && errors->GetStringLength())
            msg += errors->GetStringPointer();
        throw std::runtime_error(msg);
    }

    ComPtr<IDxcBlob> object;
    ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr),
                  "DXC GetOutput");
    blob.bytes.assign(
        static_cast<const uint8_t*>(object->GetBufferPointer()),
        static_cast<const uint8_t*>(object->GetBufferPointer()) + object->GetBufferSize());
    return blob;
}
