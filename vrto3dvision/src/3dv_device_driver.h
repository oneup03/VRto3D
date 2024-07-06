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
 * along with VRto3D. If not, see <http://www.gnu.org/licenses/>.
 */

#include "D3D11_tex.h"

#include <openvr_driver.h>
#include <thread>



struct StereoDisplayDriverConfiguration
{
	int32_t window_width;
	int32_t window_height;

	int32_t render_width;
	int32_t render_height;

	float aspect_ratio;
	float fov;
	float depth;
	float convergence;

	bool depth_gauge;

	float display_latency;
	float display_frequency;

	bool ctrl_enable;
	float ctrl_deadzone;
	float ctrl_sensitivity;

	int32_t num_user_settings;
	std::vector<int32_t> user_load_key;
	std::vector<int32_t> user_store_key;
	std::vector<int32_t> user_key_type;
	std::vector<float> user_depth;
	std::vector<float> user_convergence;
	std::vector<float> prev_depth;
	std::vector<float> prev_convergence;
	std::vector<bool> was_held;
	std::vector<bool> load_xinput;
	std::vector<bool> store_xinput;
	std::vector<int32_t> sleep_count;
};



class OVR_3DV_Driver
  : public vr::ITrackedDeviceServerDriver
  , public vr::IVRDriverDirectModeComponent
  , public vr::IVRDisplayComponent
{
public:
  enum VSyncStyle
  {
    SLEEP,
    SLEEP_BUSY,
    BUSY
  };

public:
  OVR_3DV_Driver();

  virtual ~OVR_3DV_Driver();


  vr::PropertyContainerHandle_t getPropertyContainerHandle() const
  {
    return m_ulPropertyContainer;
  }

	void PoseUpdateThread();

	void AdjustDepth(float new_depth, bool is_delta, uint32_t device_index);
	void AdjustConvergence(float new_conv, bool is_delta, uint32_t device_index);
	float GetDepth();
	float GetConvergence();
	void CheckUserSettings(uint32_t device_index);
	void AdjustPitch(vr::HmdQuaternion_t& qRotation, float& currentPitch);
	void SaveDepthConv();

	// ITrackedDeviceServerDriver
	virtual vr::EVRInitError Activate( vr::TrackedDeviceIndex_t unObjectId ) override;
	virtual void             Deactivate() override;
	virtual void             EnterStandby() override;
	virtual void *           GetComponent( const char * pchComponentNameAndVersion ) override;
	virtual void             DebugRequest( const char * pchRequest, char * pchResponseBuffer, uint32_t unResponseBufferSize ) override;
	virtual vr::DriverPose_t GetPose() override;

	// IVRDriverDirectModeComponent
	virtual void CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t * pSwapTextureSetDesc, SwapTextureSet_t * pOutSwapTextureSet ) override;
	virtual void DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle ) override;
	virtual void DestroyAllSwapTextureSets( uint32_t unPid ) override;
	virtual void GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t ( *pIndices )[2] ) override;
	virtual void SubmitLayer( const SubmitLayerPerEye_t ( &perEye )[2] ) override;
	virtual void Present( vr::SharedTextureHandle_t syncTexture ) override;

	// IVRDisplayComponent
	virtual void                        GetWindowBounds( int32_t * pnX, int32_t * pnY, uint32_t * pnWidth, uint32_t * pnHeight ) override;
	virtual bool                        IsDisplayOnDesktop() override;
	virtual bool                        IsDisplayRealDisplay() override;
	virtual void                        GetRecommendedRenderTargetSize( uint32_t * pnWidth, uint32_t * pnHeight ) override;
	virtual void                        GetEyeOutputViewport( vr::EVREye eEye, uint32_t * pnX, uint32_t * pnY, uint32_t * pnWidth, uint32_t * pnHeight ) override;
	virtual void                        GetProjectionRaw( vr::EVREye eEye, float * pfLeft, float * pfRight, float * pfTop, float * pfBottom ) override;
	virtual vr::DistortionCoordinates_t ComputeDistortion( vr::EVREye eEye, float fU, float fV ) override;
	bool                                ComputeInverseDistortion(vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override;

private:
	void VSyncThread();

	std::atomic< bool > is_active_{ false };
	std::atomic< float > depth_;
	std::atomic< float > convergence_;

	StereoDisplayDriverConfiguration config_;

	vr::TrackedDeviceIndex_t      device_index_{ vr::k_unTrackedDeviceIndexInvalid };
	vr::PropertyContainerHandle_t m_ulPropertyContainer{ vr::k_ulInvalidPropertyContainer };

	std::string stereo_model_number_;
	std::string stereo_serial_number_;

	bool    m_generateVSync{ false };

	RenderHelper              m_renderHelper;
	std::vector<TextureSet *> m_texSets;
	CRITICAL_SECTION          m_texSetCS;

	std::thread pose_update_thread_;

	std::thread m_vSyncThread;
	bool        m_runVSyncThread{ true };
	VSyncStyle  m_vsyncStyle{ SLEEP_BUSY };
};