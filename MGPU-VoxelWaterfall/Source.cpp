#include "VoxelWaterfallApp.h"
#include <array>
#include <filesystem>

using namespace Common;

namespace
{
    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    void SetVoxelWaterfallWorkingDirectory()
    {
        const auto executableDirectory = GetExecutableDirectory();
        const std::array<std::filesystem::path, 3> candidates =
        {
            std::filesystem::current_path(),
            std::filesystem::current_path() / L"MGPU-VoxelWaterfall",
            executableDirectory / L"..\\.." / L"MGPU-VoxelWaterfall"
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate / L"Shaders"))
            {
                SetCurrentDirectoryW(candidate.c_str());
                return;
            }
        }
    }
}

int WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE prevInstance,
                   PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        SetVoxelWaterfallWorkingDirectory();

        VoxelWaterfallApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        const auto result = theApp.Run();
        ExitProcess(static_cast<UINT>(result));
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}
