/*
 * This file is part of VRto3D.
 *
 * VRto3D is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VRto3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with <Your Project Name>. If not, see <http://www.gnu.org/licenses/>.
 */
#include "device_provider.h"
#include "openvr_driver.h"
#include <cstring>

#if defined( _WIN32 )
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
#define HMD_DLL_IMPORT extern "C" __declspec( dllimport )
#elif defined( __GNUC__ ) || defined( COMPILER_GCC ) || defined( __APPLE__ )
#define HMD_DLL_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#define HMD_DLL_IMPORT extern "C"
#else
#error "Unsupported Platform."
#endif

MyDeviceProvider device_provider;

//-----------------------------------------------------------------------------
// Purpose: This is exported from the shared library to be called as the entry point into the driver by vrserver.
// You should return a point to your IServerTrackedDeviceProvider here, as well as optionally a watchdog (see other
// samples).
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory( const char *pInterfaceName, int *pReturnCode )
{
	// This is where we return our device provider, if the HmdDriverFactory call asks for it.
	if ( 0 == strcmp( vr::IServerTrackedDeviceProvider_Version, pInterfaceName ) )
	{
		return &device_provider;
	}

	// Otherwise tell the runtime that we don't have this interface.
	if ( pReturnCode )
		*pReturnCode = vr::VRInitError_Init_InterfaceNotFound;

	return NULL;
}