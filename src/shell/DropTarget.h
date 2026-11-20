#pragma once

#include <Windows.h>
#include <ole2.h>
#include <functional>
#include <string>
#include <vector>

namespace winrings
{
    class RingDropTarget final : public IDropTarget
    {
    public:
        struct Callbacks
        {
            std::function<void()> onDragEnter;
            std::function<void(POINT screenPt)> onDragOver;
            std::function<void()> onDragLeave;
            std::function<void(POINT screenPt, std::vector<std::wstring> files, std::wstring text)> onDrop;
        };

        explicit RingDropTarget(Callbacks callbacks);

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;

        HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
        HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
        HRESULT STDMETHODCALLTYPE DragLeave() override;
        HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;

        static void StartFileDragOut(HWND ownerHwnd, const std::wstring& filePath);

    private:
        ~RingDropTarget() = default;

        static bool HasSupportedData(IDataObject* pDataObj);
        static void ExtractData(IDataObject* pDataObj, std::vector<std::wstring>& outFiles, std::wstring& outText);

        LONG m_refCount = 1;
        Callbacks m_callbacks;
        bool m_accepted = false;
    };
} // namespace winrings
