#include "stdafx.h"
#include "Renderer.h"
#include "..\Core\PPU.h"
#include "..\Core\Console.h"

namespace NES 
{
	Renderer::Renderer(HWND hWnd)
	{
		SetScreenSize(256, 240);

		_hWnd = hWnd;

		if(FAILED(InitDevice())) {
			CleanupDevice();
		} else {
			PPU::RegisterVideoDevice(this);
		}
	}

	Renderer::~Renderer()
	{
		CleanupDevice();
	}

	void Renderer::SetScreenSize(uint32_t screenWidth, uint32_t screenHeight)
	{
		_screenWidth = screenWidth;
		_screenHeight = screenHeight;
		_bytesPerPixel = 4;

		_hdScreenWidth = _screenWidth * 4;
		_hdScreenHeight = _screenHeight * 4;
		
		_screenBufferSize = _screenWidth * _screenHeight * _bytesPerPixel;
		_hdScreenBufferSize = _hdScreenWidth * _hdScreenHeight * _bytesPerPixel;
	}

	void Renderer::CleanupDevice()
	{
		if(_pTexture) _pTexture->Release();
		if(_overlayTexture) _overlayTexture->Release();

		if(_samplerState) _samplerState->Release();
		if(_pRenderTargetView) _pRenderTargetView->Release();
		if(_pSwapChain) _pSwapChain->Release();
		if(_pDeviceContext) _pDeviceContext->ClearState();
		if(_pDeviceContext1) _pDeviceContext1->Release();
		if(_pd3dDevice1) _pd3dDevice1->Release();
		if(_pd3dDevice) _pd3dDevice->Release();

		if(_videoRAM) {
			delete[] _videoRAM;
			_videoRAM = nullptr;
		}

		if(_nextFrameBuffer) {
			delete[] _nextFrameBuffer;
			_nextFrameBuffer = nullptr;
		}

		if(_overlayBuffer) {
			delete[] _overlayBuffer;
			_overlayBuffer = nullptr;
		}
	}

	//--------------------------------------------------------------------------------------
	// Create Direct3D device and swap chain
	//--------------------------------------------------------------------------------------
	HRESULT Renderer::InitDevice()
	{
		HRESULT hr = S_OK;

		RECT rc;
		GetClientRect(_hWnd, &rc);

		UINT createDeviceFlags = 0;
#ifdef _DEBUG
		createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

		D3D_DRIVER_TYPE driverTypes[] =
		{
			D3D_DRIVER_TYPE_HARDWARE,
			D3D_DRIVER_TYPE_WARP,
			D3D_DRIVER_TYPE_REFERENCE,
		};
		UINT numDriverTypes = ARRAYSIZE(driverTypes);

		D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1,
			D3D_FEATURE_LEVEL_10_0,
		};
		UINT numFeatureLevels = ARRAYSIZE(featureLevels);

		DXGI_SWAP_CHAIN_DESC sd;
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = 1;
		sd.BufferDesc.Width = _hdScreenWidth;
		sd.BufferDesc.Height = _hdScreenHeight - (16 *4);
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = _hWnd;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Windowed = TRUE;

		for(UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++) {
			_driverType = driverTypes[driverTypeIndex];
			hr = D3D11CreateDeviceAndSwapChain(nullptr, _driverType, nullptr, createDeviceFlags, featureLevels, numFeatureLevels,
				D3D11_SDK_VERSION, &sd, &_pSwapChain, &_pd3dDevice, &_featureLevel, &_pDeviceContext);

			if(hr == E_INVALIDARG) {
				// DirectX 11.0 platforms will not recognize D3D_FEATURE_LEVEL_11_1 so we need to retry without it
				hr = D3D11CreateDeviceAndSwapChain(nullptr, _driverType, nullptr, createDeviceFlags, &featureLevels[1], numFeatureLevels - 1,
					D3D11_SDK_VERSION, &sd, &_pSwapChain, &_pd3dDevice, &_featureLevel, &_pDeviceContext);
			}

			if(SUCCEEDED(hr)) {
				break;
			}
		}
		if(FAILED(hr)) {
			return hr;
		}

		// Obtain the Direct3D 11.1 versions if available
		hr = _pd3dDevice->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&_pd3dDevice1));
		if(SUCCEEDED(hr)) {
			(void)_pDeviceContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&_pDeviceContext1));
		}

		// Create a render target view
		ID3D11Texture2D* pBackBuffer = nullptr;
		hr = _pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
		if(FAILED(hr)) {
			return hr;
		}

		hr = _pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &_pRenderTargetView);
		pBackBuffer->Release();
		if(FAILED(hr)) {
			return hr;
		}

		_pDeviceContext->OMSetRenderTargets(1, &_pRenderTargetView, nullptr);

		// Setup the viewport
		D3D11_VIEWPORT vp;
		vp.Width = (FLOAT)_hdScreenWidth;
		vp.Height = (FLOAT)_hdScreenHeight - (16 * 4);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		vp.TopLeftX = 0;
		vp.TopLeftY = 0;
		_pDeviceContext->RSSetViewports(1, &vp);

		UINT fred;
		_pd3dDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 16, &fred);

		D3D11_TEXTURE2D_DESC desc;
		ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
		desc.ArraySize = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.MipLevels = 1;
		desc.MiscFlags = 0;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = fred;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.Width = _screenWidth;
		desc.Height = _screenHeight;
		desc.MiscFlags = 0;

		_videoRAM = new uint8_t[_screenBufferSize];
		_nextFrameBuffer = new uint8_t[_screenBufferSize];
		memset(_videoRAM, 0x00, _screenBufferSize);
		memset(_nextFrameBuffer, 0x00, _screenBufferSize);

		D3D11_SUBRESOURCE_DATA tbsd;
		tbsd.pSysMem = (void *)_videoRAM;
		tbsd.SysMemPitch = _screenWidth * _bytesPerPixel;
		tbsd.SysMemSlicePitch = _screenBufferSize; // Not needed since this is a 2d texture

		if(FAILED(_pd3dDevice->CreateTexture2D(&desc, &tbsd, &_pTexture))) {
			return 0;
		}

		_overlayBuffer = new uint8_t[_hdScreenBufferSize];  //High res overlay for UI elements (4x res)
		memset(_overlayBuffer, 0x00, _hdScreenBufferSize);

		ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
		desc.ArraySize = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.MipLevels = 1;
		desc.MiscFlags = 0;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = fred;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.Width = _hdScreenWidth;
		desc.Height = _hdScreenHeight;
		desc.MiscFlags = 0;

		tbsd.pSysMem = (void *)_overlayBuffer;
		tbsd.SysMemPitch = _hdScreenWidth * _bytesPerPixel;
		tbsd.SysMemSlicePitch = _hdScreenBufferSize;

		if(FAILED(_pd3dDevice->CreateTexture2D(&desc, &tbsd, &_overlayTexture))) {
			return 0;
		}

		////////////////////////////////////////////////////////////////////////////
		_spriteBatch.reset(new SpriteBatch(_pDeviceContext));

		_font.reset(new SpriteFont(_pd3dDevice, L"Calibri.30.spritefont"));

		//Sample state
		D3D11_SAMPLER_DESC samplerDesc;
		ZeroMemory(&desc, sizeof(desc));
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		//samplerDesc.BorderColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		samplerDesc.MinLOD = -FLT_MAX;
		samplerDesc.MaxLOD = FLT_MAX;
		samplerDesc.MipLODBias = 0.0f;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		
		_pd3dDevice->CreateSamplerState(&samplerDesc, &_samplerState);

		return S_OK;
	}

	ID3D11ShaderResourceView* Renderer::GetShaderResourceView(ID3D11Texture2D* texture)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		D3D11_TEXTURE2D_DESC desc;
		D3D11_RESOURCE_DIMENSION type;
		texture->GetType(&type);
		texture->GetDesc(&desc);
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = desc.MipLevels - 1;

		ID3D11ShaderResourceView *shaderResourceView = nullptr;
		_pd3dDevice->CreateShaderResourceView(texture, &srvDesc, &shaderResourceView);

		return shaderResourceView;
	}

	void Renderer::DisplayMessage(wstring text, uint32_t duration)
	{
		_displayMessage = text;
		_displayTimestamp = timeGetTime() + duration;
	}

	void Renderer::DrawNESScreen()
	{
		RECT sourceRect;
		sourceRect.left = 0;
		sourceRect.right = _screenWidth;
		sourceRect.top = 8;
		sourceRect.bottom = _screenHeight - 8;

		XMVECTOR position{ { 0, 0 } };

		D3D11_MAPPED_SUBRESOURCE dd;
		dd.pData = (void *)_videoRAM;
		dd.RowPitch = _screenWidth * _bytesPerPixel;
		dd.DepthPitch = _screenBufferSize;

		_pDeviceContext->Map(_pTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &dd);
		memcpy(dd.pData, _nextFrameBuffer, _screenBufferSize);
		_pDeviceContext->Unmap(_pTexture, 0);
		
		ID3D11ShaderResourceView *nesOutputBuffer = GetShaderResourceView(_pTexture);
		_spriteBatch->Draw(nesOutputBuffer, position, &sourceRect, Colors::White, 0.0f, position, 4.0f);
		nesOutputBuffer->Release();
	}

	void Renderer::DrawPauseScreen()
	{
		RECT destRect;
		destRect.left = 0;
		destRect.right = _hdScreenWidth;
		destRect.bottom = _hdScreenHeight;
		destRect.top = 0;

		XMVECTOR position{ { 0, 0 } };

		D3D11_MAPPED_SUBRESOURCE dd;
		dd.pData = (void *)_overlayBuffer;
		dd.RowPitch = _hdScreenWidth * _bytesPerPixel;
		dd.DepthPitch = _hdScreenBufferSize;

		_pDeviceContext->Map(_overlayTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &dd);
		for(uint32_t i = 0, len = _hdScreenHeight*_hdScreenWidth; i < len; i++) {
			//Gray transparent overlay
			((uint32_t*)dd.pData)[i] = 0x99222222;
		}
		_pDeviceContext->Unmap(_overlayTexture, 0);
		
		ID3D11ShaderResourceView *shaderResourceView = GetShaderResourceView(_overlayTexture);
		_spriteBatch->Draw(shaderResourceView, destRect); // , position, &sourceRect, Colors::White, 0.0f, position, 4.0f);
		shaderResourceView->Release();

		_font->DrawString(_spriteBatch.get(), L"PAUSED", XMFLOAT2(_hdScreenWidth / 2 - 142, _hdScreenHeight / 2 - 77), Colors::Black, 0.0f, XMFLOAT2(0, 0), 2.0f);
		_font->DrawString(_spriteBatch.get(), L"PAUSED", XMFLOAT2(_hdScreenWidth / 2 - 145, _hdScreenHeight / 2 - 80), Colors::AntiqueWhite, 0.0f, XMFLOAT2(0, 0), 2.0f);
	}

	//--------------------------------------------------------------------------------------
	// Render a frame
	//--------------------------------------------------------------------------------------
	void Renderer::Render()
	{
		// Clear the back buffer 
		_pDeviceContext->ClearRenderTargetView(_pRenderTargetView, Colors::Black);

		_spriteBatch->Begin(SpriteSortMode_Deferred, nullptr, _samplerState);

		//Draw nes screen
		DrawNESScreen();

		//Draw FPS counter
		if(CheckFlag(UIFlags::ShowFPS)) {
			_font->DrawString(_spriteBatch.get(), (wstring(L"FPS: ") + std::to_wstring(Console::GetFPS())).c_str(), XMFLOAT2(256 * 4 - 149, 13), Colors::Black, 0.0f, XMFLOAT2(0, 0), 1.0f);
			_font->DrawString(_spriteBatch.get(), (wstring(L"FPS: ") + std::to_wstring(Console::GetFPS())).c_str(), XMFLOAT2(256 * 4 - 150, 11), Colors::AntiqueWhite, 0.0f, XMFLOAT2(0, 0), 1.0f);
		}

		if(!_displayMessage.empty() && _displayTimestamp > timeGetTime()) {
			_font->DrawString(_spriteBatch.get(), _displayMessage.c_str(), XMFLOAT2(12, 13), Colors::Black, 0.0f, XMFLOAT2(0, 0), 1.0f);
			_font->DrawString(_spriteBatch.get(), _displayMessage.c_str(), XMFLOAT2(11, 11), Colors::AntiqueWhite, 0.0f, XMFLOAT2(0, 0), 1.0f);
		}

		if(CheckFlag(UIFlags::ShowPauseScreen)) {
			DrawPauseScreen();
		}

		_spriteBatch->End();

		// Present the information rendered to the back buffer to the front buffer (the screen)
		_pSwapChain->Present(0, 0);
	}
}