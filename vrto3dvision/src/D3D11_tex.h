/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <d3d11.h>
#include <fstream>

#define VENDORID_NVIDIA 0x10DE

struct TextureSet
{
  void Release()
  {
    for ( int i = 0; i < NUM_TEX; i++ )
    {
      m_tex[i]->Release();
    }
  }

  static const int  NUM_TEX = 3;
  ID3D11Texture2D * m_tex[NUM_TEX];
  HANDLE            m_sharedHandles[NUM_TEX];
  DXGI_FORMAT       m_textureFormat;
  int               m_srcIdx;
  int               m_nextIdx;
  int               m_width;
  int               m_height;
  uint32_t          m_pid;
};

class RenderHelper
{
  IDXGIFactory1 *       m_dxgiFactory1{ nullptr };
  ID3D11Device *        m_d3dDevice{ nullptr };
  ID3D11DeviceContext * m_immContext{ nullptr };

public:
  bool hasGPU()
  {
    // Init DX
    CreateDXGIFactory1( __uuidof( IDXGIFactory1 ), (void **)&m_dxgiFactory1 );
    if ( m_dxgiFactory1 )
    {
      IDXGIAdapter1 *    pAdapter = nullptr;
      DXGI_ADAPTER_DESC1 adapterDesc;

      // look for a GPU
      // a GPU is considered to be no software adapter
      for ( UINT iAdapter = 0; m_dxgiFactory1->EnumAdapters1( iAdapter, &pAdapter ) != DXGI_ERROR_NOT_FOUND; ++iAdapter )
      {
        pAdapter->GetDesc1( &adapterDesc );
        pAdapter->Release();
        if ( !( adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) )
        {
          return true;
        }
      }
    }

    return false;
  }

  bool Init()
  {
    if ( m_d3dDevice )
    {
      return true;
    }

    HRESULT hr = NULL;

    // Init DX
    CreateDXGIFactory1( __uuidof( IDXGIFactory1 ), (void **)&m_dxgiFactory1 );
    if ( !m_dxgiFactory1 )
    {
      return false;
    }

    IDXGIAdapter *    pAdapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc;

    for ( UINT iAdapter = 0; m_dxgiFactory1->EnumAdapters( iAdapter, &pAdapter ) != DXGI_ERROR_NOT_FOUND; ++iAdapter )
    {
      pAdapter->GetDesc( &adapterDesc );
      if ( adapterDesc.VendorId == VENDORID_NVIDIA )
      {
        break;
      }
    }

    if ( !pAdapter )
    {
      return false;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
    };
    UINT numFeatureLevels = ARRAYSIZE( featureLevels );

#ifdef _DEBUG  // attempt to create a debug device (not all systems have the libraries installed)
    // hr = D3D11CreateDevice(pAdapter, pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, featureLevels,
    // numFeatureLevels, D3D11_SDK_VERSION, &m_d3dDevice, NULL, &m_immContext);
#endif

    if ( !m_d3dDevice )
    {
      hr = D3D11CreateDevice( pAdapter,
                              pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                              NULL,
                              0,
                              featureLevels,
                              numFeatureLevels,
                              D3D11_SDK_VERSION,
                              &m_d3dDevice,
                              NULL,
                              &m_immContext );
    }

    if ( FAILED( hr ) )
    {
      return false;
    }
    IDXGIDevice1 * DXGIDevice1 = nullptr;
    m_d3dDevice->QueryInterface( __uuidof( IDXGIDevice1 ), (void **)&DXGIDevice1 );

    if ( DXGIDevice1 )
    {
      if ( FAILED( hr = DXGIDevice1->SetMaximumFrameLatency( 1 ) ) )
      {
      }
      DXGIDevice1->Release();
    }

    return true;
  }

  ID3D11Texture2D * CreateTexture( int width, int height, UINT miscFlags, DXGI_FORMAT format )
  {
    auto texFormat = format;

    ID3D11Texture2D *    tex;
    D3D11_TEXTURE2D_DESC desc;

    memset( &desc, 0, sizeof( desc ) );
    desc.Width     = width;
    desc.Height    = height;
    desc.Format    = texFormat;
    desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count           = 1;
    desc.SampleDesc.Quality         = 0;
    desc.Usage                      = D3D11_USAGE_DEFAULT;
    desc.BindFlags                  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags             = 0;
    desc.MiscFlags                  = miscFlags;

    HRESULT hr;

    if ( ( hr = m_d3dDevice->CreateTexture2D( &desc, NULL, &tex ) ) < 0 )
    {
      return NULL;
    }
    return tex;
  }
};