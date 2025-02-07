/*
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "dll_log.hpp"
#include "d3d9_device.hpp"
#include "d3d9_swapchain.hpp"
#include "runtime_d3d9.hpp"

extern void dump_and_modify_present_parameters(D3DPRESENT_PARAMETERS &pp, IDirect3D9 *d3d, UINT adapter_index);
extern void dump_and_modify_present_parameters(D3DPRESENT_PARAMETERS &pp, D3DDISPLAYMODEEX &fullscreen_desc, IDirect3D9 *d3d, UINT adapter_index);

Direct3DDevice9::Direct3DDevice9(IDirect3DDevice9   *original, bool use_software_rendering) :
	device_impl(original),
	_extended_interface(0),
	_use_software_rendering(use_software_rendering)
{
	assert(_orig != nullptr);
}
Direct3DDevice9::Direct3DDevice9(IDirect3DDevice9Ex *original, bool use_software_rendering) :
	device_impl(original),
	_extended_interface(1),
	_use_software_rendering(use_software_rendering)
{
	assert(_orig != nullptr);
}

bool Direct3DDevice9::check_and_upgrade_interface(REFIID riid)
{
	if (riid == __uuidof(this) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(IDirect3DDevice9))
		return true;
	if (riid != __uuidof(IDirect3DDevice9Ex))
		return false;

	if (!_extended_interface)
	{
		IDirect3DDevice9Ex *new_interface = nullptr;
		if (FAILED(_orig->QueryInterface(IID_PPV_ARGS(&new_interface))))
			return false;
#if RESHADE_VERBOSE_LOG
		LOG(DEBUG) << "Upgraded IDirect3DDevice9 object " << this << " to IDirect3DDevice9Ex.";
#endif
		_orig->Release();
		_orig = new_interface;
		_extended_interface = true;
	}

	return true;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (check_and_upgrade_interface(riid))
	{
		AddRef();
		*ppvObj = this;
		return S_OK;
	}

	return _orig->QueryInterface(riid, ppvObj);
}
ULONG   STDMETHODCALLTYPE Direct3DDevice9::AddRef()
{
	_orig->AddRef();
	return InterlockedIncrement(&_ref);
}
ULONG   STDMETHODCALLTYPE Direct3DDevice9::Release()
{
	const ULONG ref = InterlockedDecrement(&_ref);
	if (ref != 0)
		return _orig->Release(), ref;

	// Release remaining references to this device
	_implicit_swapchain->Release();

	const auto orig = _orig;
	const bool extended_interface = _extended_interface;
#if RESHADE_VERBOSE_LOG
	LOG(DEBUG) << "Destroying " << "IDirect3DDevice9" << (extended_interface ? "Ex" : "") << " object " << this << " (" << orig << ").";
#endif
	delete this;

	const ULONG ref_orig = orig->Release();
	if (ref_orig != 0) // Verify internal reference count
		LOG(WARN) << "Reference count for " << "IDirect3DDevice9" << (extended_interface ? "Ex" : "") << " object " << this << " (" << orig << ") is inconsistent (" << ref_orig << ").";
	return 0;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9::TestCooperativeLevel()
{
	return _orig->TestCooperativeLevel();
}
UINT    STDMETHODCALLTYPE Direct3DDevice9::GetAvailableTextureMem()
{
	return _orig->GetAvailableTextureMem();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::EvictManagedResources()
{
	return _orig->EvictManagedResources();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetDirect3D(IDirect3D9 **ppD3D9)
{
	return _orig->GetDirect3D(ppD3D9);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetDeviceCaps(D3DCAPS9 *pCaps)
{
	return _orig->GetDeviceCaps(pCaps);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	return _implicit_swapchain->GetDisplayMode(pMode);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
	return _orig->GetCreationParameters(pParameters);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap)
{
	return _orig->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
}
void    STDMETHODCALLTYPE Direct3DDevice9::SetCursorPosition(int X, int Y, DWORD Flags)
{
	return _orig->SetCursorPosition(X, Y, Flags);
}
BOOL    STDMETHODCALLTYPE Direct3DDevice9::ShowCursor(BOOL bShow)
{
	return _orig->ShowCursor(bShow);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **ppSwapChain)
{
	LOG(INFO) << "Redirecting " << "IDirect3DDevice9::CreateAdditionalSwapChain" << '(' << "this = " << this << ", pPresentationParameters = " << pPresentationParameters << ", ppSwapChain = " << ppSwapChain << ')' << " ...";

	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	D3DPRESENT_PARAMETERS pp = *pPresentationParameters;
	dump_and_modify_present_parameters(pp, _d3d.get(), _cp.AdapterOrdinal);

	const HRESULT hr = _orig->CreateAdditionalSwapChain(&pp, ppSwapChain);
	// Update output values (see https://docs.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createadditionalswapchain)
	pPresentationParameters->BackBufferWidth = pp.BackBufferWidth;
	pPresentationParameters->BackBufferHeight = pp.BackBufferHeight;
	pPresentationParameters->BackBufferFormat = pp.BackBufferFormat;
	pPresentationParameters->BackBufferCount = pp.BackBufferCount;

	if (FAILED(hr))
	{
		LOG(WARN) << "IDirect3DDevice9::CreateAdditionalSwapChain" << " failed with error code " << hr << '.';
		return hr;
	}

	AddRef(); // Add reference which is released when the swap chain is destroyed (see 'Direct3DSwapChain9::Release')

	const auto swapchain_proxy = new Direct3DSwapChain9(this, *ppSwapChain);
	_additional_swapchains.push_back(swapchain_proxy);
	*ppSwapChain = swapchain_proxy;

#if RESHADE_VERBOSE_LOG
	LOG(INFO) << "Returning IDirect3DSwapChain9 object: " << swapchain_proxy << '.';
#endif
	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	if (ppSwapChain == nullptr)
		return D3DERR_INVALIDCALL;

	_implicit_swapchain->AddRef();
	*ppSwapChain = _implicit_swapchain;

	return D3D_OK;
}
UINT    STDMETHODCALLTYPE Direct3DDevice9::GetNumberOfSwapChains()
{
	return 1; // Multi-head swap chains are not supported
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
	LOG(INFO) << "Redirecting " << "IDirect3DDevice9::Reset" << '(' << "this = " << this << ", pPresentationParameters = " << pPresentationParameters << ')' << " ...";

	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	D3DPRESENT_PARAMETERS pp = *pPresentationParameters;
	dump_and_modify_present_parameters(pp, _d3d.get(), _cp.AdapterOrdinal);

	// Release all resources before performing reset
	_implicit_swapchain->on_reset();
	device_impl::on_reset();

	const HRESULT hr = _orig->Reset(&pp);
	// Update output values (see https://docs.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-reset)
	pPresentationParameters->BackBufferWidth = pp.BackBufferWidth;
	pPresentationParameters->BackBufferHeight = pp.BackBufferHeight;
	pPresentationParameters->BackBufferFormat = pp.BackBufferFormat;
	pPresentationParameters->BackBufferCount = pp.BackBufferCount;

	if (FAILED(hr))
	{
		LOG(ERROR) << "IDirect3DDevice9::Reset" << " failed with error code " << hr << '!';
		return hr;
	}

	device_impl::on_after_reset(pp);
	if (!_implicit_swapchain->on_init())
		LOG(ERROR) << "Failed to recreate Direct3D 9 runtime environment on runtime " << static_cast<reshade::d3d9::runtime_impl *>(_implicit_swapchain) << '!';

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion)
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::present>(this, _implicit_swapchain);
#endif

	// Only call into runtime if the entire surface is presented, to avoid partial updates messing up effects and the GUI
	if (Direct3DSwapChain9::is_presenting_entire_surface(pSourceRect, hDestWindowOverride))
		_implicit_swapchain->on_present();

	return _orig->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	return _implicit_swapchain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	return _implicit_swapchain->GetRasterStatus(pRasterStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetDialogBoxMode(BOOL bEnableDialogs)
{
	return _orig->SetDialogBoxMode(bEnableDialogs);
}
void    STDMETHODCALLTYPE Direct3DDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return;
	}

	return _orig->SetGammaRamp(0, Flags, pRamp);
}
void    STDMETHODCALLTYPE Direct3DDevice9::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return;
	}

	return _orig->GetGammaRamp(0, pRamp);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
{
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_TEXTURE, Usage, Pool, D3DMULTISAMPLE_NONE, 0, Width, Height };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, ppTexture, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::texture_2d || initial_data != nullptr)
				return false;
			UINT levels = 0;
			reshade::d3d9::convert_resource_desc(desc, new_desc, &levels);

			hr = _orig->CreateTexture(new_desc.Width, new_desc.Height, levels, new_desc.Usage, new_desc.Format, new_desc.Pool, ppTexture, pSharedHandle);
			if (SUCCEEDED(hr))
			{
				assert(ppTexture != nullptr);
				IDirect3DTexture9 *const texture = *ppTexture;
				_resources.register_object(texture);

				// Register all surfaces of this texture too when it can be used as a render target or depth-stencil
				if (new_desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
				{
					levels = texture->GetLevelCount();
					for (UINT level = 0; level < levels; ++level)
					{
						com_ptr<IDirect3DSurface9> surface;
						if (SUCCEEDED(texture->GetSurfaceLevel(level, &surface)))
							_resources.register_object(surface.get());
					}
				}
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateTexture" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Levels                                  | " << std::setw(39) << levels << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | Pool                                    | " << std::setw(39) << new_desc.Pool << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, Levels, _caps), nullptr, reshade::api::resource_usage::undefined);
	return hr;
#else
	return _orig->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle)
{
#if RESHADE_ADDON
	D3DVOLUME_DESC new_desc = { Format, D3DRTYPE_VOLUMETEXTURE, Usage, Pool, Width, Height, Depth };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, ppVolumeTexture, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::texture_3d || initial_data != nullptr)
				return false;
			UINT levels = 0;
			reshade::d3d9::convert_resource_desc(desc, new_desc, &levels);

			hr = _orig->CreateVolumeTexture(new_desc.Width, new_desc.Height, new_desc.Depth, levels, new_desc.Usage, new_desc.Format, new_desc.Pool, ppVolumeTexture, pSharedHandle);
			if (SUCCEEDED(hr))
			{
				assert(ppVolumeTexture != nullptr);
				_resources.register_object(*ppVolumeTexture);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateVolumeTexture" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Depth                                   | " << std::setw(39) << new_desc.Depth << " |";
				LOG(DEBUG) << "  | Levels                                  | " << std::setw(39) << levels << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | Pool                                    | " << std::setw(39) << new_desc.Pool << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, Levels), nullptr, reshade::api::resource_usage::undefined);
	return hr;
#else
	return _orig->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle)
{
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_CUBETEXTURE, Usage, Pool, D3DMULTISAMPLE_NONE, 0, EdgeLength, EdgeLength };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, ppCubeTexture, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::texture_2d || desc.texture.width != desc.texture.height || desc.texture.depth_or_layers != 6 || initial_data != nullptr)
				return false;
			UINT levels = 0;
			reshade::d3d9::convert_resource_desc(desc, new_desc, &levels);

			hr = _orig->CreateCubeTexture(new_desc.Width, levels, new_desc.Usage, new_desc.Format, new_desc.Pool, ppCubeTexture, pSharedHandle);
			if (SUCCEEDED(hr))
			{
				assert(ppCubeTexture != nullptr);
				IDirect3DCubeTexture9 *const texture = *ppCubeTexture;
				_resources.register_object(texture);

				// Register all surfaces of this texture too when it can be used as a render target or depth-stencil
				if (new_desc.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
				{
					levels = texture->GetLevelCount();
					for (UINT level = 0; level < levels; ++level)
					{
						for (D3DCUBEMAP_FACES face = D3DCUBEMAP_FACE_POSITIVE_X; face <= D3DCUBEMAP_FACE_NEGATIVE_Z; face = static_cast<D3DCUBEMAP_FACES>(face + 1))
						{
							com_ptr<IDirect3DSurface9> surface;
							if (SUCCEEDED(texture->GetCubeMapSurface(face, level, &surface)))
								_resources.register_object(surface.get());
						}
					}
				}
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateCubeTexture" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | EdgeLength                              | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Levels                                  | " << std::setw(39) << levels << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | Pool                                    | " << std::setw(39) << new_desc.Pool << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, Levels, _caps), nullptr, reshade::api::resource_usage::undefined);
	return hr;
#else
	return _orig->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle)
{
	// Need to allow buffer for use in software vertex processing, since application uses software and not hardware processing, but device was created with both
	if (_use_software_rendering)
		Usage |= D3DUSAGE_SOFTWAREPROCESSING;

#if RESHADE_ADDON
	D3DVERTEXBUFFER_DESC new_desc = { D3DFMT_UNKNOWN, D3DRTYPE_VERTEXBUFFER, Usage, Pool, Length, FVF };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, ppVertexBuffer, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::buffer || (desc.usage & reshade::api::resource_usage::vertex_buffer) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

			hr = _orig->CreateVertexBuffer(new_desc.Size, new_desc.Usage, new_desc.FVF, new_desc.Pool, ppVertexBuffer, pSharedHandle);
			if (SUCCEEDED(hr))
			{
				assert(ppVertexBuffer != nullptr);
				_resources.register_object(*ppVertexBuffer);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateVertexBuffer" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Length                                  | " << std::setw(39) << new_desc.Size << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  | Pool                                    | " << std::setw(39) << new_desc.Pool << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc), nullptr, reshade::api::resource_usage::vertex_buffer);
	return hr;
#else
	return _orig->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle)
{
	if (_use_software_rendering)
		Usage |= D3DUSAGE_SOFTWAREPROCESSING;

#if RESHADE_ADDON
	D3DINDEXBUFFER_DESC new_desc = { Format, D3DRTYPE_INDEXBUFFER, Usage, Pool, Length };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, ppIndexBuffer, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::buffer || (desc.usage & reshade::api::resource_usage::index_buffer) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

			hr = _orig->CreateIndexBuffer(new_desc.Size, new_desc.Usage, new_desc.Format, new_desc.Pool, ppIndexBuffer, pSharedHandle);
			if (SUCCEEDED(hr))
			{
				assert(ppIndexBuffer != nullptr);
				_resources.register_object(*ppIndexBuffer);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateIndexBuffer" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Length                                  | " << std::setw(39) << new_desc.Size << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | Pool                                    | " << std::setw(39) << new_desc.Pool << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc), nullptr, reshade::api::resource_usage::index_buffer);
	return hr;
#else
	return _orig->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, MultiSample, MultisampleQuality, Width, Height };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, Lockable, ppSurface, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::surface || desc.heap != reshade::api::memory_heap::gpu_only || (desc.usage & reshade::api::resource_usage::render_target) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

#if RESHADE_ADDON
			// Need to replace surface with a texture if an add-on requested shader access
			if ((desc.usage & reshade::api::resource_usage::shader_resource) != reshade::api::resource_usage::undefined)
				hr = create_surface_replacement(new_desc, ppSurface, pSharedHandle) ? D3D_OK : D3DERR_INVALIDCALL;
			else
#endif
				hr = _orig->CreateRenderTarget(new_desc.Width, new_desc.Height, new_desc.Format, new_desc.MultiSampleType, new_desc.MultiSampleQuality, Lockable, ppSurface, pSharedHandle);

			if (SUCCEEDED(hr))
			{
				assert(ppSurface != nullptr);
				_resources.register_object(*ppSurface);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateRenderTarget" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | MultiSampleType                         | " << std::setw(39) << new_desc.MultiSampleType << " |";
				LOG(DEBUG) << "  | MultiSampleQuality                      | " << std::setw(39) << new_desc.MultiSampleQuality << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, 1, _caps), nullptr, reshade::api::resource_usage::render_target);
	return hr;
#else
	return _orig->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_SURFACE, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, MultiSample, MultisampleQuality, Width, Height };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, Discard, ppSurface, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::surface || desc.heap != reshade::api::memory_heap::gpu_only || (desc.usage & reshade::api::resource_usage::depth_stencil) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

			// Need to replace surface with a texture if an add-on requested shader access
			if ((desc.usage & reshade::api::resource_usage::shader_resource) != reshade::api::resource_usage::undefined)
				hr = create_surface_replacement(new_desc, ppSurface, pSharedHandle) ? D3D_OK : D3DERR_INVALIDCALL;
			else
				hr = _orig->CreateDepthStencilSurface(new_desc.Width, new_desc.Height, new_desc.Format, new_desc.MultiSampleType, new_desc.MultiSampleQuality, Discard, ppSurface, pSharedHandle);

			if (SUCCEEDED(hr))
			{
				assert(ppSurface != nullptr);
				_resources.register_object(*ppSurface);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateDepthStencilSurface" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | MultiSampleType                         | " << std::setw(39) << new_desc.MultiSampleType << " |";
				LOG(DEBUG) << "  | MultiSampleQuality                      | " << std::setw(39) << new_desc.MultiSampleQuality << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, 1, _caps), nullptr, reshade::api::resource_usage::depth_stencil);
	return hr;
#else
	return _orig->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint)
{
#if RESHADE_ADDON
	int32_t src_box[6];
	int32_t dst_box[6];
	if (pSourceRect != nullptr)
	{
		src_box[0] = pSourceRect->left;
		src_box[1] = pSourceRect->top;
		src_box[2] = 0;
		src_box[3] = pSourceRect->right;
		src_box[4] = pSourceRect->bottom;
		src_box[5] = 1;

		if (pDestPoint != nullptr)
		{
			dst_box[0] = pDestPoint->x;
			dst_box[1] = pDestPoint->y;
			dst_box[2] = 0;
			dst_box[3] = pDestPoint->x + pSourceRect->right - pSourceRect->left;
			dst_box[4] = pDestPoint->y + pSourceRect->bottom - pSourceRect->top;
			dst_box[5] = 1;
		}
	}

	if (reshade::invoke_addon_event<reshade::addon_event::copy_texture_region>(this,
		reshade::api::resource { reinterpret_cast<uintptr_t>(pSourceSurface) }, 0, (pSourceRect != nullptr) ? src_box : nullptr,
		reshade::api::resource { reinterpret_cast<uintptr_t>(pDestinationSurface) }, 0, (pSourceRect != nullptr && pDestPoint != nullptr) ? dst_box : nullptr,
		reshade::api::texture_filter::min_mag_mip_point))
		return D3D_OK;
#endif
	return _orig->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::copy_resource>(this, reshade::api::resource { reinterpret_cast<uintptr_t>(pSourceTexture) }, reshade::api::resource { reinterpret_cast<uintptr_t>(pDestinationTexture) }))
		return D3D_OK;
#endif
	return _orig->UpdateTexture(pSourceTexture, pDestinationTexture);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface)
{
	return _orig->GetRenderTargetData(pRenderTarget, pDestSurface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	return _implicit_swapchain->GetFrontBufferData(pDestSurface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
#if RESHADE_ADDON
	int32_t src_box[6];
	int32_t dst_box[6];
	if (pSourceRect != nullptr)
	{
		src_box[0] = pSourceRect->left;
		src_box[1] = pSourceRect->top;
		src_box[2] = 0;
		src_box[3] = pSourceRect->right;
		src_box[4] = pSourceRect->bottom;
		src_box[5] = 1;
	}
	if (pDestRect != nullptr)
	{
		dst_box[0] = pDestRect->left;
		dst_box[1] = pDestRect->top;
		dst_box[2] = 0;
		dst_box[3] = pDestRect->right;
		dst_box[4] = pDestRect->bottom;
		dst_box[5] = 1;
	}

	D3DSURFACE_DESC desc;
	pSourceSurface->GetDesc(&desc);

	if (desc.MultiSampleType == D3DMULTISAMPLE_NONE)
	{
		if (reshade::invoke_addon_event<reshade::addon_event::copy_texture_region>(this,
			reshade::api::resource { reinterpret_cast<uintptr_t>(pSourceSurface) }, 0, (pSourceRect != nullptr) ? src_box : nullptr,
			reshade::api::resource { reinterpret_cast<uintptr_t>(pDestSurface) }, 0, (pDestRect != nullptr) ? dst_box : nullptr,
			Filter == D3DTEXF_NONE || Filter == D3DTEXF_POINT ? reshade::api::texture_filter::min_mag_mip_point : reshade::api::texture_filter::min_mag_mip_linear))
			return D3D_OK;
	}
	else
	{
		if (reshade::invoke_addon_event<reshade::addon_event::resolve_texture_region>(this,
			reshade::api::resource { reinterpret_cast<uintptr_t>(pSourceSurface) }, 0, (pSourceRect != nullptr) ? src_box : nullptr,
			reshade::api::resource { reinterpret_cast<uintptr_t>(pDestSurface) }, 0, (pDestRect != nullptr) ? dst_box : nullptr,
			reshade::d3d9::convert_format(desc.Format)))
			return D3D_OK;
	}
#endif
	return _orig->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color)
{
#if RESHADE_ADDON
	const float color[4] = { ((Color >> 16) & 0xFF) / 255.0f, ((Color >> 8) & 0xFF) / 255.0f, (Color & 0xFF) / 255.0f, ((Color >> 24) & 0xFF) / 255.0f };

	if (const reshade::api::resource_view rtv = { reinterpret_cast<uintptr_t>(pSurface) };
		reshade::invoke_addon_event<reshade::addon_event::clear_render_target_views>(this, 1, &rtv, color))
		return D3D_OK;
#endif
	return _orig->ColorFill(pSurface, pRect, Color);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
	// Do not call add-on events or register offscreen surfaces, since they cannot be used with the ReShade API in a meaningful way
	return _orig->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget)
{
#if RESHADE_ADDON
	if (RenderTargetIndex == 0)
		reshade::invoke_addon_event<reshade::addon_event::finish_render_pass>(this);
#endif

	const HRESULT hr = _orig->SetRenderTarget(RenderTargetIndex, pRenderTarget);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		DWORD count = 0;
		com_ptr<IDirect3DSurface9> surface;
		reshade::api::resource_view rtvs[8], dsv = { 0 };
		for (DWORD i = 0; i < _caps.NumSimultaneousRTs; ++i, surface.reset())
		{
			if (FAILED(_orig->GetRenderTarget(i, &surface)))
				continue;

			// All surfaces that can be used as render target should be registered at this point
			rtvs[i] = { reinterpret_cast<uintptr_t>(surface.get()) };
			count = i + 1;
		}
		if (SUCCEEDED(_orig->GetDepthStencilSurface(&surface)))
		{
			// All surfaces that can be used as depth-stencil should be registered at this point
			dsv = { reinterpret_cast<uintptr_t>(surface.get()) };
		}

		if (RenderTargetIndex == 0)
			reshade::invoke_addon_event<reshade::addon_event::begin_render_pass>(this, count, rtvs, dsv);

		if (pRenderTarget != nullptr)
		{
			// Setting a new render target will cause the viewport to be set to the full size of the new render target
			// See https://docs.microsoft.com/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3ddevice9-setrendertarget
			D3DSURFACE_DESC desc;
			pRenderTarget->GetDesc(&desc);

			const float viewport_data[6] = {
				0.0f,
				0.0f,
				static_cast<float>(desc.Width),
				static_cast<float>(desc.Height),
				0.0f,
				1.0f
			};

			reshade::invoke_addon_event<reshade::addon_event::bind_viewports>(this, 0, 1, viewport_data);
		}
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget)
{
	return _orig->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil)
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::finish_render_pass>(this);
#endif

	const HRESULT hr = _orig->SetDepthStencilSurface(pNewZStencil);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		DWORD count = 0;
		com_ptr<IDirect3DSurface9> surface;
		reshade::api::resource_view rtvs[8];
		for (DWORD i = 0; i < _caps.NumSimultaneousRTs; ++i, surface.reset())
		{
			if (FAILED(_orig->GetRenderTarget(i, &surface)))
				continue;

			rtvs[i] = { reinterpret_cast<uintptr_t>(surface.get()) };
			count = i + 1;
		}

		reshade::invoke_addon_event<reshade::addon_event::begin_render_pass>(this, count, rtvs, reshade::api::resource_view { reinterpret_cast<uintptr_t>(pNewZStencil) });
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface)
{
	return _orig->GetDepthStencilSurface(ppZStencilSurface);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::BeginScene()
{
	return _orig->BeginScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::EndScene()
{
	return _orig->EndScene();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
#if RESHADE_ADDON
	if (Flags & (D3DCLEAR_TARGET))
	{
		const float color[4] = { ((Color >> 16) & 0xFF) / 255.0f, ((Color >> 8) & 0xFF) / 255.0f, (Color & 0xFF) / 255.0f, ((Color >> 24) & 0xFF) / 255.0f };

		DWORD count = 0;
		com_ptr<IDirect3DSurface9> surface;
		reshade::api::resource_view rtvs[8];
		for (DWORD i = 0; i < _caps.NumSimultaneousRTs; ++i, surface.reset())
		{
			if (FAILED(_orig->GetRenderTarget(i, &surface)))
				continue;

			rtvs[i] = { reinterpret_cast<uintptr_t>(surface.get()) };
			count = i + 1;
		}

		if (reshade::invoke_addon_event<reshade::addon_event::clear_render_target_views>(this, count, rtvs, color))
			Flags &= ~(D3DCLEAR_TARGET);
	}
	if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
	{
		com_ptr<IDirect3DSurface9> surface;
		_orig->GetDepthStencilSurface(&surface);

		if (reshade::invoke_addon_event<reshade::addon_event::clear_depth_stencil_view>(this, reshade::api::resource_view { reinterpret_cast<uintptr_t>(surface.get()) }, Flags >> 1, Z, static_cast<uint8_t>(Stencil)))
			Flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
	}

	if (Flags == 0) // Nothing to clear, so skip D3D call
		return D3D_OK;
#endif
	return _orig->Clear(Count, pRects, Flags, Color, Z, Stencil);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	return _orig->SetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
	return _orig->GetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
	return _orig->MultiplyTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetViewport(const D3DVIEWPORT9 *pViewport)
{
	const HRESULT hr = _orig->SetViewport(pViewport);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		const float viewport_data[6] = {
			static_cast<float>(pViewport->X),
			static_cast<float>(pViewport->Y),
			static_cast<float>(pViewport->Width),
			static_cast<float>(pViewport->Height),
			pViewport->MinZ,
			pViewport->MaxZ
		};

		reshade::invoke_addon_event<reshade::addon_event::bind_viewports>(this, 0, 1, viewport_data);
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetViewport(D3DVIEWPORT9 *pViewport)
{
	return _orig->GetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetMaterial(const D3DMATERIAL9 *pMaterial)
{
	return _orig->SetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetMaterial(D3DMATERIAL9 *pMaterial)
{
	return _orig->GetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetLight(DWORD Index, const D3DLIGHT9 *pLight)
{
	return _orig->SetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetLight(DWORD Index, D3DLIGHT9 *pLight)
{
	return _orig->GetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::LightEnable(DWORD Index, BOOL Enable)
{
	return _orig->LightEnable(Index, Enable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetLightEnable(DWORD Index, BOOL *pEnable)
{
	return _orig->GetLightEnable(Index, pEnable);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetClipPlane(DWORD Index, const float *pPlane)
{
	return _orig->SetClipPlane(Index, pPlane);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetClipPlane(DWORD Index, float *pPlane)
{
	return _orig->GetClipPlane(Index, pPlane);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
	const HRESULT hr = _orig->SetRenderState(State, Value);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		static_assert(
			(DWORD)reshade::api::pipeline_state::depth_test                 == D3DRS_ZENABLE &&
			(DWORD)reshade::api::pipeline_state::fill_mode                  == D3DRS_FILLMODE &&
			(DWORD)reshade::api::pipeline_state::depth_write_mask           == D3DRS_ZWRITEENABLE &&
			(DWORD)reshade::api::pipeline_state::alpha_test                 == D3DRS_ALPHATESTENABLE &&
			(DWORD)reshade::api::pipeline_state::src_color_blend_factor     == D3DRS_SRCBLEND &&
			(DWORD)reshade::api::pipeline_state::dst_color_blend_factor     == D3DRS_DESTBLEND &&
			(DWORD)reshade::api::pipeline_state::cull_mode                  == D3DRS_CULLMODE &&
			(DWORD)reshade::api::pipeline_state::depth_func                 == D3DRS_ZFUNC &&
			(DWORD)reshade::api::pipeline_state::alpha_reference_value      == D3DRS_ALPHAREF &&
			(DWORD)reshade::api::pipeline_state::alpha_func                 == D3DRS_ALPHAFUNC &&
			(DWORD)reshade::api::pipeline_state::blend_enable               == D3DRS_ALPHABLENDENABLE &&
			(DWORD)reshade::api::pipeline_state::stencil_test               == D3DRS_STENCILENABLE &&
			(DWORD)reshade::api::pipeline_state::front_stencil_fail         == D3DRS_STENCILFAIL &&
			(DWORD)reshade::api::pipeline_state::front_stencil_depth_fail   == D3DRS_STENCILZFAIL &&
			(DWORD)reshade::api::pipeline_state::front_stencil_pass         == D3DRS_STENCILPASS &&
			(DWORD)reshade::api::pipeline_state::front_stencil_func         == D3DRS_STENCILFUNC &&
			(DWORD)reshade::api::pipeline_state::stencil_reference_value    == D3DRS_STENCILREF &&
			(DWORD)reshade::api::pipeline_state::stencil_read_mask          == D3DRS_STENCILMASK &&
			(DWORD)reshade::api::pipeline_state::stencil_write_mask         == D3DRS_STENCILWRITEMASK &&
			(DWORD)reshade::api::pipeline_state::depth_clip                 == D3DRS_CLIPPING &&
			(DWORD)reshade::api::pipeline_state::multisample                == D3DRS_MULTISAMPLEANTIALIAS &&
			(DWORD)reshade::api::pipeline_state::sample_mask                == D3DRS_MULTISAMPLEMASK &&
			(DWORD)reshade::api::pipeline_state::render_target_write_mask   == D3DRS_COLORWRITEENABLE &&
			(DWORD)reshade::api::pipeline_state::color_blend_op             == D3DRS_BLENDOP &&
			(DWORD)reshade::api::pipeline_state::scissor_test               == D3DRS_SCISSORTESTENABLE &&
			(DWORD)reshade::api::pipeline_state::depth_bias_slope_scaled    == D3DRS_SLOPESCALEDEPTHBIAS &&
			(DWORD)reshade::api::pipeline_state::antialiased_line           == D3DRS_ANTIALIASEDLINEENABLE &&
			(DWORD)reshade::api::pipeline_state::back_stencil_fail          == D3DRS_CCW_STENCILFAIL &&
			(DWORD)reshade::api::pipeline_state::back_stencil_depth_fail    == D3DRS_CCW_STENCILZFAIL &&
			(DWORD)reshade::api::pipeline_state::back_stencil_pass          == D3DRS_CCW_STENCILPASS &&
			(DWORD)reshade::api::pipeline_state::back_stencil_func          == D3DRS_CCW_STENCILFUNC &&
			(DWORD)reshade::api::pipeline_state::blend_constant             == D3DRS_BLENDFACTOR &&
			(DWORD)reshade::api::pipeline_state::srgb_write                 == D3DRS_SRGBWRITEENABLE &&
			(DWORD)reshade::api::pipeline_state::depth_bias                 == D3DRS_DEPTHBIAS &&
			(DWORD)reshade::api::pipeline_state::src_alpha_blend_factor     == D3DRS_SRCBLENDALPHA &&
			(DWORD)reshade::api::pipeline_state::dst_alpha_blend_factor     == D3DRS_DESTBLENDALPHA &&
			(DWORD)reshade::api::pipeline_state::alpha_blend_op             == D3DRS_BLENDOPALPHA);
		static_assert(sizeof(State) == sizeof(reshade::api::pipeline_state) && sizeof(Value) == sizeof(uint32_t));

		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, 1, reinterpret_cast<const reshade::api::pipeline_state *>(&State), reinterpret_cast<const uint32_t *>(&Value));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
	return _orig->GetRenderState(State, pValue);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB)
{
	return _orig->CreateStateBlock(Type, ppSB);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::BeginStateBlock()
{
	return _orig->BeginStateBlock();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::EndStateBlock(IDirect3DStateBlock9 **ppSB)
{
	return _orig->EndStateBlock(ppSB);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus)
{
	return _orig->SetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus)
{
	return _orig->GetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture)
{
	return _orig->GetTexture(Stage, ppTexture);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
	const HRESULT hr = _orig->SetTexture(Stage, pTexture);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::api::shader_stage shader_stage = reshade::api::shader_stage::pixel;
		if (Stage >= D3DVERTEXTEXTURESAMPLER0)
		{
			shader_stage = reshade::api::shader_stage::vertex;
			Stage -= D3DVERTEXTEXTURESAMPLER0;
		}
		else if (Stage == D3DDMAPSAMPLER)
		{
			shader_stage = reshade::api::shader_stage::hull;
		}

		const reshade::api::resource_view view = { reinterpret_cast<uintptr_t>(pTexture) };
		reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(this, shader_stage, reshade::api::pipeline_layout { 0 }, 0, reshade::api::descriptor_type::shader_resource_view, Stage, 1, &view);
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
	return _orig->GetTextureStageState(Stage, Type, pValue);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
	return _orig->SetTextureStageState(Stage, Type, Value);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue)
{
	return _orig->GetSamplerState(Sampler, Type, pValue);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
	const HRESULT hr = _orig->SetSamplerState(Sampler, Type, Value);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::api::shader_stage shader_stage = reshade::api::shader_stage::pixel;
		if (Sampler >= D3DVERTEXTEXTURESAMPLER0)
		{
			shader_stage = reshade::api::shader_stage::vertex;
			Sampler -= D3DVERTEXTEXTURESAMPLER0;
		}
		else if (Sampler == D3DDMAPSAMPLER)
		{
			shader_stage = reshade::api::shader_stage::hull;
		}

		DWORD sampler_data[12];
		for (D3DSAMPLERSTATETYPE state = D3DSAMP_ADDRESSU; state <= D3DSAMP_SRGBTEXTURE; state = static_cast<D3DSAMPLERSTATETYPE>(state + 1))
		{
			_orig->GetSamplerState(Sampler, state, &sampler_data[state]);
		}

		const reshade::api::sampler sampler_handle = { reinterpret_cast<uintptr_t>(sampler_data) };
		reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(this, shader_stage, reshade::api::pipeline_layout { 0 }, 0, reshade::api::descriptor_type::sampler, Sampler, 1, &sampler_handle);
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::ValidateDevice(DWORD *pNumPasses)
{
	return _orig->ValidateDevice(pNumPasses);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries)
{
	return _orig->SetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries)
{
	return _orig->GetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetCurrentTexturePalette(UINT PaletteNumber)
{
	return _orig->SetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetCurrentTexturePalette(UINT *PaletteNumber)
{
	return _orig->GetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetScissorRect(const RECT *pRect)
{
	const HRESULT hr = _orig->SetScissorRect(pRect);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		static_assert(sizeof(RECT) == (sizeof(int32_t) * 4));

		reshade::invoke_addon_event<reshade::addon_event::bind_scissor_rects>(this, 0, 1, reinterpret_cast<const int32_t *>(pRect));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetScissorRect(RECT *pRect)
{
	return _orig->GetScissorRect(pRect);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetSoftwareVertexProcessing(BOOL bSoftware)
{
	return _orig->SetSoftwareVertexProcessing(bSoftware);
}
BOOL    STDMETHODCALLTYPE Direct3DDevice9::GetSoftwareVertexProcessing()
{
	return _orig->GetSoftwareVertexProcessing();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetNPatchMode(float nSegments)
{
	return _orig->SetNPatchMode(nSegments);
}
float   STDMETHODCALLTYPE Direct3DDevice9::GetNPatchMode()
{
	return _orig->GetNPatchMode();
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
#if RESHADE_ADDON
	if (PrimitiveType != _current_prim_type)
	{
		_current_prim_type = PrimitiveType;

		const reshade::api::pipeline_state state = reshade::api::pipeline_state::primitive_topology;
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, 1, &state, reinterpret_cast<const uint32_t *>(&PrimitiveType));
	}

	if (reshade::invoke_addon_event<reshade::addon_event::draw>(this, reshade::d3d9::calc_vertex_from_prim_count(PrimitiveType, PrimitiveCount), 1, StartVertex, 0))
		return D3D_OK;
#endif
	return _orig->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
#if RESHADE_ADDON
	if (PrimitiveType != _current_prim_type)
	{
		_current_prim_type = PrimitiveType;

		const reshade::api::pipeline_state state = reshade::api::pipeline_state::primitive_topology;
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, 1, &state, reinterpret_cast<const uint32_t *>(&PrimitiveType));
	}

	if (reshade::invoke_addon_event<reshade::addon_event::draw_indexed>(this, reshade::d3d9::calc_vertex_from_prim_count(PrimitiveType, PrimitiveCount), 1, StartIndex, BaseVertexIndex, 0))
		return D3D_OK;
#endif
	return _orig->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
#if RESHADE_ADDON
	if (PrimitiveType != _current_prim_type)
	{
		_current_prim_type = PrimitiveType;

		const reshade::api::pipeline_state state = reshade::api::pipeline_state::primitive_topology;
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, 1, &state, reinterpret_cast<const uint32_t *>(&PrimitiveType));
	}

	if (reshade::invoke_addon_event<reshade::addon_event::draw>(this, reshade::d3d9::calc_vertex_from_prim_count(PrimitiveType, PrimitiveCount), 1, 0, 0))
		return D3D_OK;
#endif
	return _orig->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
#if RESHADE_ADDON
	if (PrimitiveType != _current_prim_type)
	{
		_current_prim_type = PrimitiveType;

		const reshade::api::pipeline_state state = reshade::api::pipeline_state::primitive_topology;
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, 1, &state, reinterpret_cast<const uint32_t *>(&PrimitiveType));
	}

	if (reshade::invoke_addon_event<reshade::addon_event::draw_indexed>(this, reshade::d3d9::calc_vertex_from_prim_count(PrimitiveType, PrimitiveCount), 1, 0, 0, 0))
		return D3D_OK;
#endif
	return _orig->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags)
{
	return _orig->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl)
{
	return _orig->CreateVertexDeclaration(pVertexElements, ppDecl);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl)
{
	return _orig->SetVertexDeclaration(pDecl);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl)
{
	return _orig->GetVertexDeclaration(ppDecl);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetFVF(DWORD FVF)
{
	return _orig->SetFVF(FVF);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetFVF(DWORD *pFVF)
{
	return _orig->GetFVF(pFVF);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
#if RESHADE_ADDON
	if (pFunction == nullptr)
		return D3DERR_INVALIDCALL;

	reshade::api::pipeline_desc desc = { reshade::api::pipeline_type::graphics_vertex_shader };
	desc.graphics.vertex_shader.code = pFunction;
	// Total size is at byte offset 24 (see http://timjones.io/blog/archive/2015/09/02/parsing-direct3d-shader-bytecode)
	desc.graphics.vertex_shader.code_size = pFunction[6];
	desc.graphics.vertex_shader.format = reshade::api::shader_format::dxbc;

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_pipeline>(
		[this, &hr, ppShader](reshade::api::device *, const reshade::api::pipeline_desc &desc) {
			if (desc.type != reshade::api::pipeline_type::graphics_vertex_shader || desc.graphics.vertex_shader.format != reshade::api::shader_format::dxbc)
				return false;
			hr = _orig->CreateVertexShader(static_cast<const DWORD *>(desc.graphics.vertex_shader.code), ppShader);
			if (SUCCEEDED(hr))
			{
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreateVertexShader" << " failed with error code " << hr << '.';
				return false;
			}
		}, this, desc);
	return hr;
#else
	return _orig->CreateVertexShader(pFunction, ppShader);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetVertexShader(IDirect3DVertexShader9 *pShader)
{
	const HRESULT hr = _orig->SetVertexShader(pShader);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this,
			reshade::api::pipeline_type::graphics_vertex_shader, reshade::api::pipeline { reinterpret_cast<uintptr_t>(pShader) });
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetVertexShader(IDirect3DVertexShader9 **ppShader)
{
	return _orig->GetVertexShader(ppShader);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount)
{
	const HRESULT hr = _orig->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::vertex, reshade::api::pipeline_layout { 0 }, 0, StartRegister, Vector4fCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount)
{
	return _orig->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount)
{
	const HRESULT hr = _orig->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::vertex, reshade::api::pipeline_layout { 0 }, 1, StartRegister, Vector4iCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount)
{
	return _orig->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount)
{
	const HRESULT hr = _orig->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::vertex, reshade::api::pipeline_layout { 0 }, 2, StartRegister, BoolCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount)
{
	return _orig->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
	const HRESULT hr = _orig->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		const reshade::api::resource buffer = { reinterpret_cast<uintptr_t>(pStreamData) };
		const uint64_t offset = OffsetInBytes;

		reshade::invoke_addon_event<reshade::addon_event::bind_vertex_buffers>(this, StreamNumber, 1, &buffer, &offset, &Stride);
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *OffsetInBytes, UINT *pStride)
{
	return _orig->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
{
	return _orig->SetStreamSourceFreq(StreamNumber, Divider);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT *Divider)
{
	return _orig->GetStreamSourceFreq(StreamNumber, Divider);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetIndices(IDirect3DIndexBuffer9 *pIndexData)
{
	const HRESULT hr = _orig->SetIndices(pIndexData);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		uint32_t index_size = 0;
		if (pIndexData != nullptr)
		{
			D3DINDEXBUFFER_DESC desc;
			pIndexData->GetDesc(&desc);
			index_size = desc.Format == D3DFMT_INDEX16 ? 2 : 4;
		}

		reshade::invoke_addon_event<reshade::addon_event::bind_index_buffer>(this, reshade::api::resource { reinterpret_cast<uintptr_t>(pIndexData) }, 0, index_size);
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetIndices(IDirect3DIndexBuffer9 **ppIndexData)
{
	return _orig->GetIndices(ppIndexData);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
#if RESHADE_ADDON
	if (pFunction == nullptr)
		return D3DERR_INVALIDCALL;

	reshade::api::pipeline_desc desc = { reshade::api::pipeline_type::graphics_pixel_shader };
	desc.graphics.pixel_shader.code = pFunction;
	// Total size is at byte offset 24 (see http://timjones.io/blog/archive/2015/09/02/parsing-direct3d-shader-bytecode)
	desc.graphics.pixel_shader.code_size = pFunction[6];
	desc.graphics.pixel_shader.format = reshade::api::shader_format::dxbc;

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_pipeline>(
		[this, &hr, ppShader](reshade::api::device *, const reshade::api::pipeline_desc &desc) {
			if (desc.type != reshade::api::pipeline_type::graphics_pixel_shader || desc.graphics.pixel_shader.format != reshade::api::shader_format::dxbc)
				return false;
			hr = _orig->CreatePixelShader(static_cast<const DWORD *>(desc.graphics.pixel_shader.code), ppShader);
			if (SUCCEEDED(hr))
			{
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9::CreatePixelShader" << " failed with error code " << hr << '.';
				return false;
			}
		}, this, desc);
	return hr;
#else
	return _orig->CreatePixelShader(pFunction, ppShader);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetPixelShader(IDirect3DPixelShader9 *pShader)
{
	const HRESULT hr = _orig->SetPixelShader(pShader);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this,
			reshade::api::pipeline_type::graphics_pixel_shader, reshade::api::pipeline { reinterpret_cast<uintptr_t>(pShader) });
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetPixelShader(IDirect3DPixelShader9 **ppShader)
{
	return _orig->GetPixelShader(ppShader);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount)
{
	const HRESULT hr = _orig->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::pixel, reshade::api::pipeline_layout { 0 }, 0, StartRegister, Vector4fCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount)
{
	return _orig->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount)
{
	const HRESULT hr = _orig->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::pixel, reshade::api::pipeline_layout { 0 }, 1, StartRegister, Vector4iCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount)
{
	return _orig->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount)
{
	const HRESULT hr = _orig->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
#if RESHADE_ADDON
	if (SUCCEEDED(hr))
	{
		reshade::invoke_addon_event<reshade::addon_event::push_constants>(this,
			reshade::api::shader_stage::pixel, reshade::api::pipeline_layout { 0 }, 2, StartRegister, BoolCount, reinterpret_cast<const uint32_t *>(pConstantData));
	}
#endif
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount)
{
	return _orig->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo)
{
	return _orig->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo)
{
	return _orig->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::DeletePatch(UINT Handle)
{
	return _orig->DeletePatch(Handle);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery)
{
	return _orig->CreateQuery(Type, ppQuery);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetConvolutionMonoKernel(UINT width, UINT height, float *rows, float *columns)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->SetConvolutionMonoKernel(width, height, rows, columns);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::ComposeRects(IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::PresentEx(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags)
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::present>(this, _implicit_swapchain);
#endif

	if (Direct3DSwapChain9::is_presenting_entire_surface(pSourceRect, hDestWindowOverride))
		_implicit_swapchain->on_present();

	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetGPUThreadPriority(INT *pPriority)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->GetGPUThreadPriority(pPriority);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetGPUThreadPriority(INT Priority)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->SetGPUThreadPriority(Priority);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::WaitForVBlank(UINT iSwapChain)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->WaitForVBlank(0);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->CheckResourceResidency(pResourceArray, NumResources);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::SetMaximumFrameLatency(UINT MaxLatency)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->SetMaximumFrameLatency(MaxLatency);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetMaximumFrameLatency(UINT *pMaxLatency)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->GetMaximumFrameLatency(pMaxLatency);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CheckDeviceState(HWND hDestinationWindow)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->CheckDeviceState(hDestinationWindow);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage)
{
	assert(_extended_interface);
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_SURFACE, Usage, D3DPOOL_DEFAULT, MultiSample, MultisampleQuality, Width, Height };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, Lockable, ppSurface, pSharedHandle](reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::surface || desc.heap != reshade::api::memory_heap::gpu_only || (desc.usage & reshade::api::resource_usage::render_target) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

			// Need to replace surface with a texture if an add-on requested shader access
			if ((desc.usage & reshade::api::resource_usage::shader_resource) != reshade::api::resource_usage::undefined)
				hr = create_surface_replacement(new_desc, ppSurface, pSharedHandle) ? D3D_OK : D3DERR_INVALIDCALL;
			else
				hr = static_cast<IDirect3DDevice9Ex *>(_orig)->CreateRenderTargetEx(new_desc.Width, new_desc.Height, new_desc.Format, new_desc.MultiSampleType, new_desc.MultiSampleQuality, Lockable, ppSurface, pSharedHandle, new_desc.Usage);

			if (SUCCEEDED(hr))
			{
				assert(ppSurface != nullptr);
				_resources.register_object(*ppSurface);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9Ex::CreateRenderTargetEx" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | MultiSampleType                         | " << std::setw(39) << new_desc.MultiSampleType << " |";
				LOG(DEBUG) << "  | MultiSampleQuality                      | " << std::setw(39) << new_desc.MultiSampleQuality << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, 1, _caps), nullptr, reshade::api::resource_usage::render_target);
	return hr;
#else
	return static_cast<IDirect3DDevice9Ex *>(_orig)->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage)
{
	assert(_extended_interface);
	return static_cast<IDirect3DDevice9Ex *>(_orig)->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage);
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage)
{
	assert(_extended_interface);
#if RESHADE_ADDON
	D3DSURFACE_DESC new_desc = { Format, D3DRTYPE_SURFACE, Usage, D3DPOOL_DEFAULT, MultiSample, MultisampleQuality, Width, Height };

	HRESULT hr = E_FAIL;
	reshade::invoke_addon_event<reshade::addon_event::create_resource>(
		[this, &hr, &new_desc, Discard, ppSurface, pSharedHandle](reshade::api::device *,const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage) {
			if (desc.type != reshade::api::resource_type::surface || desc.heap != reshade::api::memory_heap::gpu_only || (desc.usage & reshade::api::resource_usage::depth_stencil) == reshade::api::resource_usage::undefined || initial_data != nullptr)
				return false;
			reshade::d3d9::convert_resource_desc(desc, new_desc);

			// Need to replace surface with a texture if an add-on requested shader access
			if ((desc.usage & reshade::api::resource_usage::shader_resource) != reshade::api::resource_usage::undefined)
				hr = create_surface_replacement(new_desc, ppSurface, pSharedHandle) ? D3D_OK : D3DERR_INVALIDCALL;
			else
				hr = static_cast<IDirect3DDevice9Ex *>(_orig)->CreateDepthStencilSurfaceEx(new_desc.Width, new_desc.Height, new_desc.Format, new_desc.MultiSampleType, new_desc.MultiSampleQuality, Discard, ppSurface, pSharedHandle, new_desc.Usage);

			if (SUCCEEDED(hr))
			{
				assert(ppSurface != nullptr);
				_resources.register_object(*ppSurface);
				return true;
			}
			else
			{
				LOG(WARN) << "IDirect3DDevice9Ex::CreateDepthStencilSurfaceEx" << " failed with error code " << hr << '.';
#if RESHADE_VERBOSE_LOG
				LOG(DEBUG) << "> Dumping description:";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
				LOG(DEBUG) << "  | Width                                   | " << std::setw(39) << new_desc.Width << " |";
				LOG(DEBUG) << "  | Height                                  | " << std::setw(39) << new_desc.Height << " |";
				LOG(DEBUG) << "  | Format                                  | " << std::setw(39) << new_desc.Format << " |";
				LOG(DEBUG) << "  | MultiSampleType                         | " << std::setw(39) << new_desc.MultiSampleType << " |";
				LOG(DEBUG) << "  | MultiSampleQuality                      | " << std::setw(39) << new_desc.MultiSampleQuality << " |";
				LOG(DEBUG) << "  | Usage                                   | " << std::setw(39) << std::hex << new_desc.Usage << std::dec << " |";
				LOG(DEBUG) << "  +-----------------------------------------+-----------------------------------------+";
#endif
				return false;
			}
		}, this, reshade::d3d9::convert_resource_desc(new_desc, 1, _caps), nullptr, reshade::api::resource_usage::depth_stencil);
	return hr;
#else
	return static_cast<IDirect3DDevice9Ex *>(_orig)->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage);
#endif
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode)
{
	LOG(INFO) << "Redirecting " << "IDirect3DDevice9Ex::ResetEx" << '(' << "this = " << this << ", pPresentationParameters = " << pPresentationParameters << ", pFullscreenDisplayMode = " << pFullscreenDisplayMode << ')' << " ...";

	if (pPresentationParameters == nullptr)
		return D3DERR_INVALIDCALL;

	D3DDISPLAYMODEEX fullscreen_mode = { sizeof(fullscreen_mode) };
	if (pFullscreenDisplayMode != nullptr)
		fullscreen_mode = *pFullscreenDisplayMode;
	D3DPRESENT_PARAMETERS pp = *pPresentationParameters;
	dump_and_modify_present_parameters(pp, fullscreen_mode, _d3d.get(), _cp.AdapterOrdinal);

	// Release all resources before performing reset
	_implicit_swapchain->on_reset();
	device_impl::on_reset();

	assert(_extended_interface);
	const HRESULT hr = static_cast<IDirect3DDevice9Ex *>(_orig)->ResetEx(&pp, pp.Windowed ? nullptr : &fullscreen_mode);
	pPresentationParameters->BackBufferWidth = pp.BackBufferWidth;
	pPresentationParameters->BackBufferHeight = pp.BackBufferHeight;
	pPresentationParameters->BackBufferFormat = pp.BackBufferFormat;
	pPresentationParameters->BackBufferCount = pp.BackBufferCount;

	if (FAILED(hr))
	{
		LOG(ERROR) << "IDirect3DDevice9Ex::ResetEx" << " failed with error code " << hr << '!';
		return hr;
	}

	device_impl::on_after_reset(pp);
	if (!_implicit_swapchain->on_init())
		LOG(ERROR) << "Failed to recreate Direct3D 9 runtime environment on runtime " << static_cast<reshade::d3d9::runtime_impl *>(_implicit_swapchain) << '!';

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DDevice9::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation)
{
	if (iSwapChain != 0)
	{
		LOG(WARN) << "Access to multi-head swap chain at index " << iSwapChain << " is unsupported.";
		return D3DERR_INVALIDCALL;
	}

	assert(_extended_interface);
	assert(_implicit_swapchain->_extended_interface);
	return static_cast<IDirect3DSwapChain9Ex *>(_implicit_swapchain)->GetDisplayModeEx(pMode, pRotation);
}
