//============ Copyright (c) Valve Corporation, All rights reserved.
//============

#include <math.h>
#include <openvr_driver.h>
#include <time.h>
#include <windows.h>

#include <chrono>
#include <thread>
#include <vector>

#include "../headers/ShareMem.h"
#include "../headers/picojson.h"
#include "driverlog.h"

SharedMemory comm("pipe");

using namespace vr;

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec(dllexport)
#define HMD_DLL_IMPORT extern "C" __declspec(dllimport)
#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#define HMD_DLL_IMPORT extern "C"
#else
#error "Unsupported Platform."
#endif

inline HmdQuaternion_t HmdQuaternion_Init(double w, double x, double y,
                                          double z) {
  HmdQuaternion_t quat;
  quat.w = w;
  quat.x = x;
  quat.y = y;
  quat.z = z;
  return quat;
}

inline void HmdMatrix_SetIdentity(HmdMatrix34_t *pMatrix) {
  pMatrix->m[0][0] = 1.f;
  pMatrix->m[0][1] = 0.f;
  pMatrix->m[0][2] = 0.f;
  pMatrix->m[0][3] = 0.f;
  pMatrix->m[1][0] = 0.f;
  pMatrix->m[1][1] = 1.f;
  pMatrix->m[1][2] = 0.f;
  pMatrix->m[1][3] = 0.f;
  pMatrix->m[2][0] = 0.f;
  pMatrix->m[2][1] = 0.f;
  pMatrix->m[2][2] = 1.f;
  pMatrix->m[2][3] = 0.f;
}

// keys for use with the settings API
static const char *const k_pch_ForDesktop_Section = "driver_ForDesktop";
static const char *const k_pch_ForDesktop_SerialNumber_String = "serialNumber";
static const char *const k_pch_ForDesktop_ModelNumber_String = "modelNumber";
static const char *const k_pch_ForDesktop_WindowX_Int32 = "windowX";
static const char *const k_pch_ForDesktop_WindowY_Int32 = "windowY";
static const char *const k_pch_ForDesktop_WindowWidth_Int32 = "windowWidth";
static const char *const k_pch_ForDesktop_WindowHeight_Int32 = "windowHeight";
static const char *const k_pch_ForDesktop_RenderWidth_Int32 = "renderWidth";
static const char *const k_pch_ForDesktop_RenderHeight_Int32 = "renderHeight";
static const char *const k_pch_ForDesktop_SecondsFromVsyncToPhotons_Float =
    "secondsFromVsyncToPhotons";
static const char *const k_pch_ForDesktop_DisplayFrequency_Float =
    "displayFrequency";

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CWatchdogDriver_ForDesktop : public IVRWatchdogProvider {
public:
  CWatchdogDriver_ForDesktop() { m_pWatchdogThread = nullptr; }

  virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
  virtual void Cleanup();

private:
  std::thread *m_pWatchdogThread;
};

CWatchdogDriver_ForDesktop g_watchdogDriverNull;

bool g_bExiting = false;

void WatchdogThreadFunction() {
  while (!g_bExiting) {
    // on windows send the event when the Y key is pressed.
    if ((0x01 & GetAsyncKeyState('Y')) != 0) {
      // Y key was pressed.
      vr::VRWatchdogHost()->WatchdogWakeUp(vr::TrackedDeviceClass_HMD);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
}

EVRInitError
CWatchdogDriver_ForDesktop::Init(vr::IVRDriverContext *pDriverContext) {
  VR_INIT_WATCHDOG_DRIVER_CONTEXT(pDriverContext);
  InitDriverLog(vr::VRDriverLog());

  // Watchdog mode on Windows starts a thread that listens for the 'Y' key on
  // the keyboard to be pressed. A real driver should wait for a system button
  // event or something else from the the hardware that signals that the VR
  // system should start up.
  g_bExiting = false;
  m_pWatchdogThread = new std::thread(WatchdogThreadFunction);
  if (!m_pWatchdogThread) {
    DriverLog("Unable to create watchdog thread\n");
    return VRInitError_Driver_Failed;
  }

  return VRInitError_None;
}

void CWatchdogDriver_ForDesktop::Cleanup() {
  g_bExiting = true;
  if (m_pWatchdogThread) {
    m_pWatchdogThread->join();
    delete m_pWatchdogThread;
    m_pWatchdogThread = nullptr;
  }

  CleanupDriverLog();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

// Head tracking vars
bool mouseIsLocked = false;
bool mouseMidOnIsContinuing = false;

bool rCtrlIsLocked = true;
bool rCtrlOnIsCOntinuing = false;

class CForDesktopDeviceDriver : public vr::ITrackedDeviceServerDriver,
                                public vr::IVRDisplayComponent {
public:
  CForDesktopDeviceDriver() {
    m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
    m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

    DriverLog("Using settings values\n");
    m_flIPD = vr::VRSettings()->GetFloat(k_pch_SteamVR_Section,
                                         k_pch_SteamVR_IPD_Float);

    char buf[1024];
    vr::VRSettings()->GetString(k_pch_ForDesktop_Section,
                                k_pch_ForDesktop_SerialNumber_String, buf,
                                sizeof(buf));
    m_sSerialNumber = buf;

    vr::VRSettings()->GetString(k_pch_ForDesktop_Section,
                                k_pch_ForDesktop_ModelNumber_String, buf,
                                sizeof(buf));
    m_sModelNumber = buf;

    m_nWindowX = vr::VRSettings()->GetInt32(k_pch_ForDesktop_Section,
                                            k_pch_ForDesktop_WindowX_Int32);
    m_nWindowY = vr::VRSettings()->GetInt32(k_pch_ForDesktop_Section,
                                            k_pch_ForDesktop_WindowY_Int32);
    m_nWindowWidth = vr::VRSettings()->GetInt32(
        k_pch_ForDesktop_Section, k_pch_ForDesktop_WindowWidth_Int32);
    m_nWindowHeight = vr::VRSettings()->GetInt32(
        k_pch_ForDesktop_Section, k_pch_ForDesktop_WindowHeight_Int32);
    m_nRenderWidth = vr::VRSettings()->GetInt32(
        k_pch_ForDesktop_Section, k_pch_ForDesktop_RenderWidth_Int32);
    m_nRenderHeight = vr::VRSettings()->GetInt32(
        k_pch_ForDesktop_Section, k_pch_ForDesktop_RenderHeight_Int32);
    m_flSecondsFromVsyncToPhotons = vr::VRSettings()->GetFloat(
        k_pch_ForDesktop_Section,
        k_pch_ForDesktop_SecondsFromVsyncToPhotons_Float);
    m_flDisplayFrequency = vr::VRSettings()->GetFloat(
        k_pch_ForDesktop_Section, k_pch_ForDesktop_DisplayFrequency_Float);

    DriverLog("driver_ForDesktop: Serial Number: %s\n",
              m_sSerialNumber.c_str());
    DriverLog("driver_ForDesktop: Model Number: %s\n", m_sModelNumber.c_str());
    DriverLog("driver_ForDesktop: Window: %d %d %d %d\n", m_nWindowX,
              m_nWindowY, m_nWindowWidth, m_nWindowHeight);
    DriverLog("driver_ForDesktop: Render Target: %d %d\n", m_nRenderWidth,
              m_nRenderHeight);
    DriverLog("driver_ForDesktop: Seconds from Vsync to Photons: %f\n",
              m_flSecondsFromVsyncToPhotons);
    DriverLog("driver_ForDesktop: Display Frequency: %f\n",
              m_flDisplayFrequency);
    DriverLog("driver_ForDesktop: IPD: %f\n", m_flIPD);
  }

  virtual ~CForDesktopDeviceDriver() {}

  virtual EVRInitError Activate(vr::TrackedDeviceIndex_t unObjectId) {
    m_unObjectId = unObjectId;
    m_ulPropertyContainer =
        vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer, Prop_ModelNumber_String, m_sModelNumber.c_str());
    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer,
                                          Prop_RenderModelName_String,
                                          m_sModelNumber.c_str());
    vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer,
                                         Prop_UserIpdMeters_Float, m_flIPD);
    vr::VRProperties()->SetFloatProperty(
        m_ulPropertyContainer, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer,
                                         Prop_DisplayFrequency_Float,
                                         m_flDisplayFrequency);
    vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer,
                                         Prop_SecondsFromVsyncToPhotons_Float,
                                         m_flSecondsFromVsyncToPhotons);

    // return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
    vr::VRProperties()->SetUint64Property(m_ulPropertyContainer,
                                          Prop_CurrentUniverseId_Uint64, 2);

    // avoid "not fullscreen" warnings from vrmonitor
    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
                                        Prop_IsOnDesktop_Bool, false);

    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
                                        Prop_DisplayDebugMode_Bool, true);

    // Icons can be configured in code or automatically configured by an
    // external file "drivername\resources\driver.vrresources". Icon properties
    // NOT configured in code (post Activate) are then auto-configured by the
    // optional presence of a driver's
    // "drivername\resources\driver.vrresources". In this manner a driver can
    // configure their icons in a flexible data driven fashion by using an
    // external file.
    //
    // The structure of the driver.vrresources file allows a driver to
    // specialize their icons based on their HW. Keys matching the value in
    // "Prop_ModelNumber_String" are considered first, since the driver may have
    // model specific icons. An absence of a matching "Prop_ModelNumber_String"
    // then considers the ETrackedDeviceClass ("HMD", "Controller",
    // "GenericTracker", "TrackingReference") since the driver may have
    // specialized icons based on those device class names.
    //
    // An absence of either then falls back to the "system.vrresources" where
    // generic device class icons are then supplied.
    //
    // Please refer to "bin\drivers\sample\resources\driver.vrresources" which
    // contains this sample configuration.
    //
    // "Alias" is a reserved key and specifies chaining to another json block.
    //
    // In this sample configuration file (overly complex FOR EXAMPLE PURPOSES
    // ONLY)....
    //
    // "Model-v2.0" chains through the alias to "Model-v1.0" which chains
    // through the alias to "Model-v Defaults".
    //
    // Keys NOT found in "Model-v2.0" would then chase through the "Alias" to be
    // resolved in "Model-v1.0" and either resolve their or continue through the
    // alias. Thus "Prop_NamedIconPathDeviceAlertLow_String" in each model's
    // block represent a specialization specific for that "model". Keys in
    // "Model-v Defaults" are an example of mapping to the same states, and here
    // all map to "Prop_NamedIconPathDeviceOff_String".
    //

    return VRInitError_None;
  }

  virtual void Deactivate() {
    m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
  }

  virtual void EnterStandby() {}

  virtual void LeaveStandby() {}

  void *GetComponent(const char *pchComponentNameAndVersion) {
    if (!_stricmp(pchComponentNameAndVersion,
                  vr::IVRDisplayComponent_Version)) {
      return (vr::IVRDisplayComponent *)this;
    }

    // override this to add a component to a driver
    return NULL;
  }

  virtual void PowerOff() {}

  /** debug request from a client */
  virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer,
                            uint32_t unResponseBufferSize) {
    if (unResponseBufferSize >= 1)
      pchResponseBuffer[0] = 0;
  }

  virtual void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth,
                               uint32_t *pnHeight) {
    *pnX = m_nWindowX;
    *pnY = m_nWindowY;
    *pnWidth = m_nWindowWidth;
    *pnHeight = m_nWindowHeight;
  }

  virtual bool IsDisplayOnDesktop() { return true; }

  virtual bool IsDisplayRealDisplay() { return false; }

  virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth,
                                              uint32_t *pnHeight) {
    *pnWidth = m_nRenderWidth;
    *pnHeight = m_nRenderHeight;
  }

  virtual void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY,
                                    uint32_t *pnWidth, uint32_t *pnHeight) {
    *pnY = 0;
    *pnWidth = m_nWindowWidth / 2;
    *pnHeight = m_nWindowHeight;

    if (eEye == Eye_Left) {
      *pnX = 0;
    } else {
      *pnX = m_nWindowWidth / 2;
    }
  }

  virtual void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight,
                                float *pfTop, float *pfBottom) {
    *pfLeft = -1.0;
    *pfRight = 1.0;
    *pfTop = -1.0;
    *pfBottom = 1.0;
  }

  virtual DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU,
                                                    float fV) {
    DistortionCoordinates_t coordinates;
    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;
    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;
    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;
    return coordinates;
  }

  virtual DriverPose_t GetPose() {
    DriverPose_t pose = {0};
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;

    pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);
    pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);

    int screenWidth = GetSystemMetrics(SM_CXMAXTRACK);
    int screenHeight = GetSystemMetrics(SM_CYMAXTRACK);

    if (mouseIsLocked) {
      POINT po;
      GetCursorPos(&po);
      head_pitch += double(double(screenWidth) / 2.0 - po.x) * 0.01;
      head_roll += double(double(screenHeight) / 2.0 - po.y) * 0.01;
    }

    if ((GetAsyncKeyState(VK_END) & 0x8000) != 0) {
      head_yaw = 0;
      // pitch = 0;
      // roll = 0;
      frontDire = head_pitch;
    }

    double cos_pitch = cos(head_pitch);
    double sin_pitch = sin(head_pitch);

    if ((GetAsyncKeyState(VK_UP) & 0x8000) != 0) {
      z += -0.01 * cos_pitch;
      x += -0.01 * sin_pitch;
    }
    if ((GetAsyncKeyState(VK_DOWN) & 0x8000) != 0) {
      z += 0.01 * cos_pitch;
      x += 0.01 * sin_pitch;
    }

    if ((GetAsyncKeyState(VK_LEFT) & 0x8000) != 0) {
      x += -0.01 * cos_pitch;
      z -= -0.01 * sin_pitch;
    }
    if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0) {
      x += 0.01 * cos_pitch;
      z -= 0.01 * sin_pitch;
    }

    if ((GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0) {
      y += 0.01;
    }
    if ((GetAsyncKeyState(VK_NEXT) & 0x8000) != 0) {
      y += -0.01;
    }

    if ((GetAsyncKeyState(VK_HOME) & 0x8000) != 0) {
      x = 0;
      y = 0;
      z = 0;
    }

    pose.vecPosition[0] = x;
    pose.vecPosition[1] = y;
    pose.vecPosition[2] = z;

    // Convert yaw, pitch, roll to quaternion
    double t0, t1, t2, t3, t4, t5;
    t0 = cos(head_yaw * 0.5);
    t1 = sin(head_yaw * 0.5);
    t2 = cos(head_roll * 0.5);
    t3 = sin(head_roll * 0.5);
    t4 = cos(head_pitch * 0.5);
    t5 = sin(head_pitch * 0.5);

    // Set head tracking rotation
    pose.qRotation.w = t0 * t2 * t4 + t1 * t3 * t5;
    pose.qRotation.x = t0 * t3 * t4 - t1 * t2 * t5;
    pose.qRotation.y = t0 * t2 * t5 + t1 * t3 * t4;
    pose.qRotation.z = t1 * t2 * t4 - t0 * t3 * t5;

    return pose;
  }

  void RunFrame() {
    // In a real driver, this should happen from some pose tracking thread.
    // The RunFrame interval is unspecified and can be very irregular if some
    // other driver blocks it for some periodic task.
    if (m_unObjectId != vr::k_unTrackedDeviceIndexInvalid) {
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(
          m_unObjectId, GetPose(), sizeof(DriverPose_t));
    }
  }

  std::string GetSerialNumber() const { return m_sSerialNumber; }

public:
  double head_yaw = 0, head_pitch = 0, head_roll = 0, x, y, z, frontDire;

private:
  vr::TrackedDeviceIndex_t m_unObjectId;
  vr::PropertyContainerHandle_t m_ulPropertyContainer;

  std::string m_sSerialNumber;
  std::string m_sModelNumber;

  int32_t m_nWindowX;
  int32_t m_nWindowY;
  int32_t m_nWindowWidth;
  int32_t m_nWindowHeight;
  int32_t m_nRenderWidth;
  int32_t m_nRenderHeight;
  float m_flSecondsFromVsyncToPhotons;
  float m_flDisplayFrequency;
  float m_flIPD;
};

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CForDesktopControllerDriver : public vr::ITrackedDeviceServerDriver {
public:
  CForDesktopControllerDriver() {
    m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
    m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

    m_sSerialNumber = "CTRL_";

    m_sModelNumber = "MyController";
  }

  void setIndex(int const index) {
    controllerIndex = index;
    m_sSerialNumber += std::to_string(index);
  }

  void setHead(CForDesktopDeviceDriver *(_head)) { head = _head; }

  virtual ~CForDesktopControllerDriver() {}

  virtual EVRInitError Activate(vr::TrackedDeviceIndex_t unObjectId) {
    m_unObjectId = unObjectId;
    m_ulPropertyContainer =
        vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer, vr::Prop_ModelNumber_String, "ViveMV");
    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer, vr::Prop_ManufacturerName_String, "HTC");
    vr::VRProperties()->SetStringProperty(m_ulPropertyContainer,
                                          vr::Prop_RenderModelName_String,
                                          "vr_controller_vive_1_5");

    // return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
    vr::VRProperties()->SetUint64Property(m_ulPropertyContainer,
                                          Prop_CurrentUniverseId_Uint64, 2);

    // avoid "not fullscreen" warnings from vrmonitor
    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
                                        Prop_IsOnDesktop_Bool, false);

    // our sample device isn't actually tracked, so set this property to avoid
    // having the icon blink in the status window
    vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer,
                                        Prop_NeverTracked_Bool, true);

    if (controllerIndex == 0) {
      // even though we won't ever track we want to pretend to be the right hand
      // so binding will work as expected
      vr::VRProperties()->SetInt32Property(m_ulPropertyContainer,
                                           Prop_ControllerRoleHint_Int32,
                                           TrackedControllerRole_RightHand);
    } else {
      vr::VRProperties()->SetInt32Property(m_ulPropertyContainer,
                                           Prop_ControllerRoleHint_Int32,
                                           TrackedControllerRole_LeftHand);
    }

    // this file tells the UI what to show the user for binding this controller
    // as well as what default bindings should be for legacy or other apps
    vr::VRProperties()->SetStringProperty(
        m_ulPropertyContainer, Prop_InputProfilePath_String,
        "{ForDesktop}/input/mycontroller_profile.json");

    // create all the input components
    vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                                                "/input/a/click", &m_compA);
    vr::VRDriverInput()->CreateBooleanComponent(m_ulPropertyContainer,
                                                "/input/b/click", &m_compB);
    vr::VRDriverInput()->CreateBooleanComponent(
        m_ulPropertyContainer, "/input/system/click", &m_compSystem);

    vr::VRDriverInput()->CreateBooleanComponent(
        m_ulPropertyContainer, "/input/trigger/click", &m_compTrigger);
    vr::VRDriverInput()->CreateScalarComponent(
        m_ulPropertyContainer, "/input/trigger/value", &m_compTriggerValue,
        VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);

    vr::VRDriverInput()->CreateBooleanComponent(
        m_ulPropertyContainer, "/input/trackpad/touch", &m_compTrackpadTouch);
    vr::VRDriverInput()->CreateBooleanComponent(
        m_ulPropertyContainer, "/input/trackpad/click", &m_compTrackpadClick);
    vr::VRDriverInput()->CreateScalarComponent(
        m_ulPropertyContainer, "/input/trackpad/x", &m_compTrackpadX,
        VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
    vr::VRDriverInput()->CreateScalarComponent(
        m_ulPropertyContainer, "/input/trackpad/y", &m_compTrackpadY,
        VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);

    // create our haptic component
    vr::VRDriverInput()->CreateHapticComponent(m_ulPropertyContainer,
                                               "/output/haptic", &m_compHaptic);

    return VRInitError_None;
  }

  virtual void Deactivate() {
    m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
  }

  virtual void EnterStandby() {}
  virtual void LeaveStandby() {}

  void *GetComponent(const char *pchComponentNameAndVersion) {
    // override this to add a component to a driver
    return NULL;
  }

  virtual void PowerOff() {}

  /** debug request from a client */
  virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer,
                            uint32_t unResponseBufferSize) {
    if (unResponseBufferSize >= 1)
      pchResponseBuffer[0] = 0;
  }

  virtual DriverPose_t GetPose() {
    DriverPose_t pose = {0};
    pose.poseIsValid = rCtrlIsLocked;
    if (rCtrlIsLocked) {
      pose.result = TrackingResult_Running_OK;

    } else {
      pose.result = TrackingResult_Running_OutOfRange;
    }
    pose.deviceIsConnected = true;

    pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);
    pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);

    //予測遅延補正
    // pose.poseTimeOffset = 0.016;

    double head_front = head->frontDire;

    if ((GetAsyncKeyState(VK_HOME) & 0x8000) != 0) {
      memcpy(posCorrectionValues, rawPosValues, sizeof(rawPosValues));
      controller_roll = controller_yaw = 0;
      controller_pitch = head_front;
    }
    if ((GetAsyncKeyState(VK_END) & 0x8000) != 0) {
      controller_roll = controller_yaw = 0;
      controller_pitch = head_front;
    }

    double x = rawPosValues[0] - posCorrectionValues[0] +
               0.2 * (1.0 - 2.0 * (double)controllerIndex);
    double y = rawPosValues[1] - posCorrectionValues[1] - 0.3;
    double z = rawPosValues[2] - posCorrectionValues[2] - 0.3;

    pose.vecPosition[0] = x * cos(head_front) + z * sin(head_front) + head->x;
    pose.vecPosition[1] = y + head->y;
    pose.vecPosition[2] = z * cos(head_front) - x * sin(head_front) + head->z;

    controller_roll += rotDiffValues[0];
    controller_pitch += rotDiffValues[1];
    controller_yaw += rotDiffValues[2];

    double cY, sY, cR, sR, cP, sP;
    // Convert yaw, pitch, roll to quaternion
    cR = cos(controller_roll * 0.5);
    sR = sin(controller_roll * 0.5);
    cP = cos(controller_pitch * 0.5);
    sP = sin(controller_pitch * 0.5);
    cY = cos(controller_yaw * 0.5);
    sY = sin(controller_yaw * 0.5);

    // Set controller rotation
    pose.qRotation.w = cR * cP * cY + sR * sP * sY;
    pose.qRotation.x = sR * cP * cY - cR * sP * sY;
    pose.qRotation.y = cR * sP * cY + sR * cP * sY;
    pose.qRotation.z = -sR * sP * cY + cR * cP * sY;

    return pose;
  }

  void RunFrame() {
    // Your driver would read whatever hardware state is associated with its
    // input components and pass that in to UpdateBooleanComponent. This could
    // happen in RunFrame or on a thread of your own that's reading USB state.
    // There's no need to update input state unless it changes, but it doesn't
    // do any harm to do so.

    vr::VRDriverInput()->UpdateBooleanComponent(
        m_compA, (0x8000 & GetAsyncKeyState('Z')) != 0, 0);
    vr::VRDriverInput()->UpdateBooleanComponent(
        m_compB, (0x8000 & GetAsyncKeyState('X')) != 0, 0);

    double trackX, trackY;
    trackX = trackpadValues[0];
    trackY = trackpadValues[1];
    bool trackTouch = (trackX != 0.0) || (trackY != 0.0);
    vr::VRDriverInput()->UpdateBooleanComponent(m_compTrackpadTouch, trackTouch,
                                                0);
    vr::VRDriverInput()->UpdateBooleanComponent(m_compTrackpadClick,
                                                trackpadClicked, 0);
    vr::VRDriverInput()->UpdateScalarComponent(m_compTrackpadX, trackX, 0);
    vr::VRDriverInput()->UpdateScalarComponent(m_compTrackpadY, trackY, 0);

    bool triggerOn = (triggerValue > 0.0);
    vr::VRDriverInput()->UpdateBooleanComponent(m_compTrigger, triggerOn, 0);
    vr::VRDriverInput()->UpdateScalarComponent(m_compTriggerValue, triggerValue,
                                               0);

    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_unObjectId, GetPose(),
                                                       sizeof(DriverPose_t));
  }

  void ProcessEvent(const vr::VREvent_t &vrEvent) {
    switch (vrEvent.eventType) {
    case vr::VREvent_Input_HapticVibration: {
      if (vrEvent.data.hapticVibration.componentHandle == m_compHaptic) {
        // This is where you would send a signal to your hardware to trigger
        // actual haptic feedback
        DriverLog("BUZZ!\n");
      }
    } break;
    }
  }

  std::string GetSerialNumber() const { return m_sSerialNumber; }

  void setInputValues(double const (&pos)[3], double const (&rot)[3],
                      double const (&tpv)[2], bool const tpc,
                      double const trig) {
    for (unsigned int i = 0; i < 3; i++) {
      rawPosValues[i] = pos[i];
      rotDiffValues[i] = rot[i];
    }
    for (unsigned int i = 0; i < 2; i++) {
      trackpadValues[i] = tpv[i];
    }
    trackpadClicked = tpc;
    triggerValue = trig;
  }

  void setRotDiffNone() {
    rotDiffValues[0] = rotDiffValues[1] = rotDiffValues[2] = 0.0;
  }

public:
  int controllerIndex;
  CForDesktopDeviceDriver *head;

  double controller_roll = 0.0, controller_pitch = 0.0, controller_yaw = 0.0;
  double posCorrectionValues[3] = {0.0};

  double rawPosValues[3] = {0.0};
  double rawRotValues[3] = {0.0};
  double rotDiffValues[3] = {0.0};
  double trackpadValues[2] = {0.0};
  bool trackpadClicked = false;
  double triggerValue = 0.0;

private:
  vr::TrackedDeviceIndex_t m_unObjectId;
  vr::PropertyContainerHandle_t m_ulPropertyContainer;

  vr::VRInputComponentHandle_t m_compA;
  vr::VRInputComponentHandle_t m_compB;
  vr::VRInputComponentHandle_t m_compSystem;
  vr::VRInputComponentHandle_t m_compTrigger;
  vr::VRInputComponentHandle_t m_compTriggerValue;
  vr::VRInputComponentHandle_t m_compTrackpadTouch;
  vr::VRInputComponentHandle_t m_compTrackpadClick;
  vr::VRInputComponentHandle_t m_compTrackpadX;
  vr::VRInputComponentHandle_t m_compTrackpadY;
  vr::VRInputComponentHandle_t m_compHaptic;

  std::string m_sSerialNumber;
  std::string m_sModelNumber;
};

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CServerDriver_ForDesktop : public IServerTrackedDeviceProvider {
public:
  virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
  virtual void Cleanup();
  virtual const char *const *GetInterfaceVersions() {
    return vr::k_InterfaceVersions;
  }
  virtual void RunFrame();
  virtual bool ShouldBlockStandbyMode() { return true; }
  virtual void EnterStandby() {}
  virtual void LeaveStandby() {}

private:
  CForDesktopDeviceDriver *m_pNullHmdLatest = nullptr;
  CForDesktopControllerDriver *m_pController_r = nullptr;
  CForDesktopControllerDriver *m_pController_l = nullptr;

  double preControllerRot[3] = {0.0};
};

CServerDriver_ForDesktop g_serverDriverNull;

EVRInitError
CServerDriver_ForDesktop::Init(vr::IVRDriverContext *pDriverContext) {
  VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
  InitDriverLog(vr::VRDriverLog());

  m_pNullHmdLatest = new CForDesktopDeviceDriver();
  vr::VRServerDriverHost()->TrackedDeviceAdded(
      m_pNullHmdLatest->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD,
      m_pNullHmdLatest);

  m_pController_r = new CForDesktopControllerDriver();
  m_pController_r->setIndex(0);
  m_pController_l->setHead(m_pNullHmdLatest);
  vr::VRServerDriverHost()->TrackedDeviceAdded(
      m_pController_r->GetSerialNumber().c_str(),
      vr::TrackedDeviceClass_Controller, m_pController_r);

  m_pController_l = new CForDesktopControllerDriver();
  m_pController_l->setIndex(1);
  m_pController_l->setHead(m_pNullHmdLatest);
  vr::VRServerDriverHost()->TrackedDeviceAdded(
      m_pController_l->GetSerialNumber().c_str(),
      vr::TrackedDeviceClass_Controller, m_pController_l);

  return VRInitError_None;
}

void CServerDriver_ForDesktop::Cleanup() {
  CleanupDriverLog();
  delete m_pNullHmdLatest;
  m_pNullHmdLatest = NULL;
  delete m_pController_r;
  m_pController_r = NULL;
  delete m_pController_r;
  m_pController_l = NULL;
}

void CServerDriver_ForDesktop::RunFrame() {
  char *SharedRam = (char *)comm.get_pointer();
  bool shramhasdata = (SharedRam[0] != 'x');
  if (shramhasdata) {
    // json解析
    std::string json = SharedRam;
    picojson::value j;
    std::string err = picojson::parse(j, json);
    if (!err.empty()) {
      DriverLog("json error: %s\n", err.c_str());
    } else {
      double controllerid;
      GetDoubleValue(controllerid, j, "id");

      double trackpadValues[2] = {0.0};
      bool trackpadClicked = false;
      double controllerPos[3] = {0.0}, controllerRot[3] = {0.0},
             controllerRotDiff[3] = {0.0};
      double triggerValue = 0.0;

      GetDoubleArry(trackpadValues, 2, j, "trackpad");
      GetBoolValue(trackpadClicked, j, "clicked");
      GetDoubleArry(controllerPos, 3, j, "translation");
      GetDoubleArry(controllerRot, 3, j, "rotation");
      GetDoubleValue(triggerValue, j, "trigger");

      controllerRotDiff[0] =
          fmod(controllerRot[0] - preControllerRot[0], 90.0) / 360.0;
      controllerRotDiff[1] =
          fmod(controllerRot[1] - preControllerRot[1], 90.0) / 360.0;
      controllerRotDiff[2] =
          fmod(controllerRot[2] - preControllerRot[2], 90.0) / 360.0;
      memcpy(preControllerRot, controllerRot, sizeof(controllerRot));

      if (controllerid == 0.0) {
        m_pController_r->setInputValues(controllerPos, controllerRotDiff,
                                        trackpadValues, trackpadClicked,
                                        triggerValue);
        m_pController_l->setRotDiffNone();
      } else if (controllerid == 1.0) {
        m_pController_l->setInputValues(controllerPos, controllerRotDiff,
                                        trackpadValues, trackpadClicked,
                                        triggerValue);
        m_pController_r->setRotDiffNone();
      }
    }
  }

  if (m_pNullHmdLatest) {
    m_pNullHmdLatest->RunFrame();
  }
  if (m_pController_r) {
    m_pController_r->RunFrame();
  }
  if (m_pController_l) {
    m_pController_l->RunFrame();
  }

  if (shramhasdata) {
    //データ待ちフラグを立てる
    SharedRam[1] = '\0';
    SharedRam[0] = 'x';
  }

  vr::VREvent_t vrEvent;
  while (vr::VRServerDriverHost()->PollNextEvent(&vrEvent, sizeof(vrEvent))) {
    if (m_pController_r) {
      m_pController_r->ProcessEvent(vrEvent);
    }
    if (m_pController_l) {
      m_pController_l->ProcessEvent(vrEvent);
    }
  }

  // mouse lock
  bool mouseMidIsOn = ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
  if (mouseMidIsOn && !mouseMidOnIsContinuing) {
    mouseIsLocked = !mouseIsLocked;
  }
  mouseMidOnIsContinuing = mouseMidIsOn;
  int screenWidth = GetSystemMetrics(SM_CXMAXTRACK);
  int screenHeight = GetSystemMetrics(SM_CYMAXTRACK);
  if (mouseIsLocked) {
    SetCursorPos(screenWidth / 2, screenHeight / 2);
  }

  // tracking flg
  bool rCtrlIsOn = ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
  if (rCtrlIsOn && !rCtrlOnIsCOntinuing) {
    rCtrlIsLocked = !rCtrlIsLocked;
  }
  rCtrlOnIsCOntinuing = rCtrlIsOn;
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName,
                                      int *pReturnCode) {
  if (0 == strcmp(IServerTrackedDeviceProvider_Version, pInterfaceName)) {
    return &g_serverDriverNull;
  }
  if (0 == strcmp(IVRWatchdogProvider_Version, pInterfaceName)) {
    return &g_watchdogDriverNull;
  }

  if (pReturnCode)
    *pReturnCode = VRInitError_Init_InterfaceNotFound;

  return NULL;
}
