#include "JonesControl.h"
#include <j3dcore/j3dhook.h>

#include <Jones3D/Display/JonesConfig.h>
#include <Jones3D/Display/JonesConsole.h>
#include <Jones3D/Main/JonesMain.h>
#include <Jones3D/RTI/symbols.h>

#include <sith/Devices/sithControl.h>
#include <sith/Engine/sithCamera.h>
#include <sith/Gameplay/sithPlayerControls.h>
#include <sith/World/sithWeapon.h>

#include <std/General/stdMath.h>
#include <std/Win95/stdControl.h>
#include <w32util/wuRegistry.h>

static bool JonesControl_bStartup = false; // Added: Init to false

// Added: Look mode (free view) camera offset limits and mouse scale.
//        Limits match the original key look controls in sithPlayerControls_ProcessLookControls.
#define JONESCONTROL_LOOKOFFSET_MIN   -0.07f
#define JONESCONTROL_LOOKOFFSET_MAX    0.15f
#define JONESCONTROL_LOOKOFFSET_SCALE  0.01f // camera offset per unit of MOUSETURN/PITCH axis

static int J3DAPI JonesControl_ProcessMouseLookView(SithThing* pPlayer, float secDeltaTime);

void JonesControl_InstallHooks(void)
{
    J3D_HOOKFUNC(JonesControl_Startup);
    J3D_HOOKFUNC(JonesControl_Shutdown);
    J3D_HOOKFUNC(JonesControl_ProcessControls);
    J3D_HOOKFUNC(JonesControl_EnableJoystickAxes);
}

void JonesControl_ResetGlobals(void)
{
}

int JonesControl_Startup(void)
{
    int bMouseControl = wuRegistry_GetIntEx("Mouse Control", 0);
    JonesDisplaySettings* pSettings = JonesMain_GetDisplaySettings();

    if ( JonesControl_bStartup )
    {
        return 1;
    }

    if ( sithControl_Startup(0) )
    {
        return 1;
    }

    // Added: Modern mouse look settings. Sensitivity is stored as percent of the base axis sensitivity.
    int bMouseLook        = wuRegistry_GetIntEx("Mouse Look", 1);
    int mouseSensitivity  = wuRegistry_GetIntEx("Mouse Sensitivity", 100);
    int bMouseInvertY     = wuRegistry_GetIntEx("Mouse Invert Y", 0);
    mouseSensitivity      = STDMATH_CLAMP(mouseSensitivity, 10, 1000);
    sithControl_SetMouseLook(bMouseLook != 0, 0.05f * (float)mouseSensitivity / 100.0f, bMouseInvertY != 0);
    wuRegistry_SaveIntEx("Mouse Look", bMouseLook);
    wuRegistry_SaveIntEx("Mouse Sensitivity", mouseSensitivity);
    wuRegistry_SaveIntEx("Mouse Invert Y", bMouseInvertY);

    // Note, instead of manually initializing the sithControl_DefaultInit could be called 
    // and unbind mouse controller afterwards
    sithControl_Reset();
    sithControl_g_controlOptions = 0x04;
    sithControl_RegisterKeyboardBindings();
    sithControl_RegisterJoystickBindings();
    // Note, sithControl_RegisterMouseBindings() is not called probably because jonesconfig would unbind it anyway

    if ( !pSettings->bWindowMode || bMouseControl )
    {
        stdControl_EnableMouse(1);
    }

    sithControl_RegisterControlCallback(JonesControl_ProcessMouseLookView); // Added: Must run before sithPlayerControls_Process
    sithControl_RegisterControlCallback(sithPlayerControls_Process);
    sithControl_RegisterControlCallback(sithWeapon_ProcessWeaponControls);
    sithControl_RegisterControlCallback(JonesControl_ProcessControls);

    for ( size_t functionId = 0; functionId < SITHCONTROL_MAXFUNCTIONS; ++functionId )
    {
        sithControl_RegisterAxisFunction((SithControlFunction)functionId, SITHCONTROLFUNCTION_KEY);
    }

    int bJoystickControl = wuRegistry_GetIntEx("Joystick Control", 0);
    if ( bJoystickControl == 1 && stdControl_GetNumJoysticks() )
    {
        JonesControl_EnableJoystickAxes();
        sithControl_UnbindJoystickAxes();
    }

    wuRegistry_SaveIntEx("Joystick Control", bJoystickControl);

    if ( (sithMain_g_sith_mode.debugModeFlags & SITHDEBUG_INEDITOR) == 0 && (!pSettings->bWindowMode || bMouseControl) )
    {
        sithControl_UnbindMouseAxes();
    }

    JonesControl_bStartup = true;
    return 0;
}

void JonesControl_Shutdown(void)
{
    sithControl_Shutdown();
    JonesControl_bStartup = false;
}

int J3DAPI JonesControl_ProcessControls(SithThing* pPlayer, float secDeltaTime)
{
    J3D_UNUSED(secDeltaTime);

    if ( (pPlayer->thingInfo.actorInfo.flags & SITH_AF_CONTROLSDISABLED) != 0 || JonesConsole_g_bVisible )
    {
        return 0;
    }

    int numPressed = 0;
    sithControl_GetKey(SITHCONTROL_TALK, &numPressed); // F10 pressed
    if ( numPressed )
    {
        JonesConsole_ShowConsole();
    }

    return 0;
}

// Added: Mouse controls the look mode (free view) camera offset the same way the original turn/move keys do.
//        Runs before sithPlayerControls_Process which then applies key look and sets the offset.
static int J3DAPI JonesControl_ProcessMouseLookView(SithThing* pPlayer, float secDeltaTime)
{
    J3D_UNUSED(secDeltaTime);

    if ( !sithControl_IsMouseLookEnabled() || !sithPlayerControls_IsLookMode() )
    {
        return 0;
    }

    if ( (pPlayer->thingInfo.actorInfo.flags & SITH_AF_CONTROLSDISABLED) != 0 || JonesConsole_g_bVisible )
    {
        return 0;
    }

    if ( !sithCamera_g_pCurCamera || sithCamera_g_pCurCamera->type != SITHCAMERA_EXTERNAL )
    {
        return 0;
    }

    // MOUSETURN is inverted mouse X (mouse right -> negative) which matches turn right key decreasing offset x.
    // PITCH is mouse Y (mouse up -> negative) which lowers the camera i.e. looks up.
    float dx = sithControl_GetAxis(SITHCONTROL_MOUSETURN);
    float dz = sithControl_GetAxis(SITHCONTROL_PITCH);
    if ( dx == 0.0f && dz == 0.0f )
    {
        return 0;
    }

    rdVector3 offset = sithCamera_g_pCurCamera->offset;
    offset.x = STDMATH_CLAMP(offset.x + dx * JONESCONTROL_LOOKOFFSET_SCALE, JONESCONTROL_LOOKOFFSET_MIN, JONESCONTROL_LOOKOFFSET_MAX);
    offset.z = STDMATH_CLAMP(offset.z + dz * JONESCONTROL_LOOKOFFSET_SCALE, JONESCONTROL_LOOKOFFSET_MIN, JONESCONTROL_LOOKOFFSET_MAX);
    sithCamera_SetExtCameraOffset(&offset);
    return 0;
}

void JonesControl_EnableJoystickAxes(void)
{
    int joystickCount = stdControl_GetNumJoysticks();
    if ( joystickCount > 0 )
    {
        int axisId = 2;
        do
        {
            stdControl_EnableAxis(axisId - 2);
            stdControl_EnableAxis(axisId - 1);
            stdControl_EnableAxis(axisId);
            stdControl_EnableAxis(axisId + 3);
            axisId += 6;
            --joystickCount;
        } while ( joystickCount );
    }
}
