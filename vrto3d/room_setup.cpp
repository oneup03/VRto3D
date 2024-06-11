#include <openvr.h>
#include "driverlog.h"

void SetStandingPose(float userHeight) {
    vr::IVRChaperoneSetup* chaperoneSetup = vr::VRChaperoneSetup();

    if (!chaperoneSetup) {
        DriverLog("Unable to get ChaperoneSetup interface.");
        return;
    }

    // Define the standing center matrix (4x4 identity matrix with translation for user height)
    vr::HmdMatrix34_t standingCenter = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, userHeight,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    // Set the working standing zero pose to the new height
    chaperoneSetup->SetWorkingStandingZeroPoseToRawTrackingPose(&standingCenter);

    // Commit the changes
    bool success = chaperoneSetup->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
    if (!success) {
        DriverLog("Failed to commit Chaperone setup.");
    }
    else {
        DriverLog("Standing pose set to user height: ", userHeight, " meters.");
    }
}