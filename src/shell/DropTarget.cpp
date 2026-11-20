#include "DropTarget.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <wrl/client.h>

namespace winrings
{
    RingDropTarget::RingDropTarget(Callbacks callbacks)
        : m_callbacks(std::move(callbacks))
    {
    }

    HRESULT STDMETHODCALLTYPE RingDropTarget::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE RingDropTarget::AddRef()
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE RingDropTarget::Release()
    {
        const LONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return static_cast<ULONG>(count);
    }

    bool RingDropTarget::HasSupportedData(IDataObject* pDataObj)
    {
        if (!pDataObj)
            return false;
        FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (pDataObj->QueryGetData(&fmtDrop) == S_OK)
            return true;
        FORMATETC fmtText{ CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return pDataObj->QueryGetData(&fmtText) == S_OK;
    }

    void RingDropTarget::ExtractData(IDataObject* pDataObj, std::vector<std::wstring>& outFiles, std::wstring& outText)
    {
        outFiles.clear();
        outText.clear();
        if (!pDataObj)
            return;

        FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg{};
        if (SUCCEEDED(pDataObj->GetData(&fmtDrop, &stg)) && stg.tymed == TYMED_HGLOBAL && stg.hGlobal)
        {
            const auto hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
            if (hDrop)
            {
                const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                    if (len > 0)
                    {
                        std::wstring path(len + 1, L'\0');
                        DragQueryFileW(hDrop, i, path.data(), len + 1);
                        path.resize(len);
                        outFiles.push_back(std::move(path));
                    }
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
            return;
        }

        FORMATETC fmtText{ CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (SUCCEEDED(pDataObj->GetData(&fmtText, &stg)) && stg.tymed == TYMED_HGLOBAL && stg.hGlobal)
        {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(stg.hGlobal));
            if (text)
            {
                outText = text;
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
        }
    }

    HRESULT STDMETHODCALLTYPE RingDropTarget::DragEnter(IDataObject* pDataObj, DWORD, POINTL pt, DWORD* pdwEffect)
    {
        m_accepted = HasSupportedData(pDataObj);
        if (pdwEffect)
            *pdwEffect = m_accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (m_accepted && m_callbacks.onDragEnter)
            m_callbacks.onDragEnter();
        if (m_accepted && m_callbacks.onDragOver)
            m_callbacks.onDragOver(POINT{ pt.x, pt.y });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RingDropTarget::DragOver(DWORD, POINTL pt, DWORD* pdwEffect)
    {
        if (pdwEffect)
            *pdwEffect = m_accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (m_accepted && m_callbacks.onDragOver)
            m_callbacks.onDragOver(POINT{ pt.x, pt.y });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RingDropTarget::DragLeave()
    {
        m_accepted = false;
        if (m_callbacks.onDragLeave)
            m_callbacks.onDragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RingDropTarget::Drop(IDataObject* pDataObj, DWORD, POINTL pt, DWORD* pdwEffect)
    {
        std::vector<std::wstring> files;
        std::wstring text;
        if (m_accepted)
            ExtractData(pDataObj, files, text);
        m_accepted = false;

        if (pdwEffect)
            *pdwEffect = (!files.empty() || !text.empty()) ? DROPEFFECT_COPY : DROPEFFECT_NONE;

        if (m_callbacks.onDrop)
            m_callbacks.onDrop(POINT{ pt.x, pt.y }, std::move(files), std::move(text));
        return S_OK;
    }

    void RingDropTarget::StartFileDragOut(HWND ownerHwnd, const std::wstring& filePath)
    {
        if (filePath.empty())
            return;
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(filePath.c_str());
        if (!pidl)
            return;

        PCUITEMID_CHILD child = nullptr;
        Microsoft::WRL::ComPtr<IShellFolder> folder;
        if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(folder.GetAddressOf()), &child)) && folder && child)
        {
            Microsoft::WRL::ComPtr<IDataObject> dataObj;
            if (SUCCEEDED(folder->GetUIObjectOf(ownerHwnd, 1, &child, IID_IDataObject, nullptr,
                reinterpret_cast<void**>(dataObj.GetAddressOf()))) && dataObj)
            {
                DWORD effect = DROPEFFECT_NONE;
                SHDoDragDrop(ownerHwnd, dataObj.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            }
        }
        ILFree(pidl);
    }
} // namespace winrings
