#include "sithControl.h"
#include <j3dcore/j3dhook.h>

#include <sith/Engine/sithCamera.h>
#include <sith/Engine/sithCollision.h>
#include <sith/Gameplay/sithPlayer.h>
#include <sith/Gameplay/sithTime.h>
#include <sith/RTI/symbols.h>
#include <sith/World/sithWorld.h>

#include <std/General/stdMath.h>
#include <std/General/stdPlatform.h>
#include <std/General/stdUtil.h>
#include <std/Win95/stdControl.h>

#define STDCONTROL_MAXBINDINGS 8

typedef struct sSithControlFunctionBinding
{
    size_t numBindings;
    SithControlBinding aBindings[STDCONTROL_MAXBINDINGS];
} SithControlFunctionBinding;
static_assert(sizeof(SithControlFunctionBinding) == 100, "sizeof(SithControlFunctionBinding) == 100");

static bool bControlStartup = false; // Added: Init to false
static bool bControlOpen    = false; // Added: Init to false

static uint32_t  msecPlayerIdleTime;

static size_t numControlCallbacks = 0; // Added: init to 0
static SithControlCallback aControlCallbacks[32];

static SithControlFunctionFlag aControlFlags[SITHCONTROL_MAXFUNCTIONS];
static SithControlFunctionBinding aControlBindings[SITHCONTROL_MAXFUNCTIONS];

// Added: Modern mouse look mode. Mouse X drives the analog MOUSETURN axis and mouse Y the PITCH axis,
//        both of which are read by the original player movement code but were bound only in editor mode.
static bool  sithControl_bMouseLook           = true;
static float sithControl_mouseLookSensitivity = 0.05f;
static bool  sithControl_bMouseLookInvertY    = false;

// Added: Climb assist state for mounting climbable surfaces with jump while walking
typedef enum eSithControlClimbAssist
{
    SITHCONTROL_CLIMBASSIST_NONE     = 0,
    SITHCONTROL_CLIMBASSIST_STOPPING = 1, // jump held back, waiting for player to stop
    SITHCONTROL_CLIMBASSIST_JUMPING  = 2, // delivering jump to mount the surface
} SithControlClimbAssist;
static SithControlClimbAssist sithControl_climbAssistState = SITHCONTROL_CLIMBASSIST_NONE;
static uint32_t sithControl_msecClimbAssistStart = 0;

const char* const aFunctionNames[SITHCONTROL_MAXFUNCTIONS + 1] =
{
    "FORWARD",
    "BACK",
    "LEFT",
    "RIGHT",
    "TURNLEFT",
    "TURNRIGHT",
    "JUMP",
    "CRAWLTOGGLE",
    "ACT1",
    "FIRE1",
    "ACTIVATE",
    "ACT2",
    "ACT3",
    "NEXTWEAP",
    "PREVWEAP",
    "LIGHTERTOG",
    "LOOK",
    "STPLEFT",
    "STPRIGHT",
    "WEAPONTOG",
    "WSELECT0",
    "WSELECT1",
    "WSELECT2",
    "WSELECT3",
    "WSELECT4",
    "WSELECT5",
    "WSELECT6",
    "WSELECT7",
    "WSELECT8",
    "WSELECT9",
    "WSELECT10",
    "WSELECT11",
    "WSELECT12",
    "WSELECT13",
    "WSELECT14",
    "WSELECT15",
    "WSELECT16",
    "USEINV",
    "GAMESAVE",
    "DEBUG",
    "NEXTWEAPON",
    "PREVWEAPON",
    "MAP",
    "INCREASE",
    "DECREASE",
    "CAMERAMODE",
    "TALK",
    "SCREENSHOT",
    "CAMERAZOOMIN",
    "CAMERAZOOMOUT",
    "PITCH",
    "CENTER",
    "MLOOK",
    "MOUSETURN",
    "GAMMA",
    "RUNFWD",
    "HEALTH",
    "CHALK",
    "MAXFUNCTIONS"
};

void sithControl_FinishRead(void);

void sithControl_InstallHooks(void)
{
    J3D_HOOKFUNC(sithControl_Startup);
    J3D_HOOKFUNC(sithControl_Shutdown);
    J3D_HOOKFUNC(sithControl_Open);
    J3D_HOOKFUNC(sithControl_Close);
    J3D_HOOKFUNC(sithControl_RegisterAxisFunction);
    J3D_HOOKFUNC(sithControl_RegisterKeyFunction);
    J3D_HOOKFUNC(sithControl_BindControl);
    J3D_HOOKFUNC(sithControl_BindAxis);
    J3D_HOOKFUNC(sithControl_UnbindFunction);
    J3D_HOOKFUNC(sithControl_UnbindFunctionIndex);
    J3D_HOOKFUNC(sithControl_UnbindControl);
    J3D_HOOKFUNC(sithControl_RegisterControlCallback);
    J3D_HOOKFUNC(sithControl_Update);
    J3D_HOOKFUNC(sithControl_GetKeyAsAxis);
    J3D_HOOKFUNC(sithControl_GetAxis);
    J3D_HOOKFUNC(sithControl_GetKey);
    J3D_HOOKFUNC(sithControl_ReadControls);
    J3D_HOOKFUNC(sithControl_FinishRead);
    J3D_HOOKFUNC(sithControl_UnbindJoystickAxes);
    J3D_HOOKFUNC(sithControl_UnbindMouseAxes);
    J3D_HOOKFUNC(sithControl_Reset);
    J3D_HOOKFUNC(sithControl_RegisterControlFunctions);
    J3D_HOOKFUNC(sithControl_RegisterKeyboardBindings);
    J3D_HOOKFUNC(sithControl_RegisterJoystickBindings);
}

void sithControl_ResetGlobals(void)
{
    memset(&sithControl_g_controlOptions, 0, sizeof(sithControl_g_controlOptions));
}

int J3DAPI sithControl_Startup(int bKeyboardForeground)
{
    SITH_ASSERTREL(streq(aFunctionNames[SITHCONTROL_MAXFUNCTIONS], "MAXFUNCTIONS"));
    if ( bControlStartup )
    {
        return 1;
    }

    if ( stdControl_Startup(bKeyboardForeground) )
    {
        SITHLOG_ERROR("Control system platform startuped failed.\n");
        return 1;
    }

    sithControl_RegisterControlFunctions();
    sithControl_Reset();
    bControlStartup = true;
    return 0;
}

void sithControl_Shutdown(void)
{
    stdControl_Shutdown();
    bControlStartup = false;
}

int sithControl_Open(void)
{
    SITH_ASSERTREL(bControlStartup);
    if ( stdControl_Open() ) {
        return 0;
    }

    msecPlayerIdleTime = 0;
    bControlOpen = 1;
    return 1;
}

int sithControl_Close(void)
{
    if ( !bControlOpen )
    {
        SITHLOG_STATUS("Control system illegal close.\n");
        return 0;
    }

    stdControl_Close();
    bControlOpen = 0;
    return 1;
}

bool sithControl_IsOpen(void)
{
    return bControlOpen;
}

void J3DAPI sithControl_RegisterAxisFunction(SithControlFunction functionId, SithControlFunctionFlag flag)
{
    SITH_ASSERTREL((functionId >= 0) && (functionId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(!bControlOpen);
    aControlFlags[functionId] = flag | SITHCONTROLFUNCTION_AXIS | SITHCONTROLFUNCTION_REGISTERED;
}

void J3DAPI sithControl_RegisterKeyFunction(SithControlFunction functionId)
{
    SITH_ASSERTREL((functionId >= 0) && (functionId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(!bControlOpen);
    aControlFlags[functionId] = SITHCONTROLFUNCTION_KEY | SITHCONTROLFUNCTION_REGISTERED;
}

void J3DAPI sithControl_BindControl(SithControlFunction functionId, size_t controlId, SithControlBindFlag flags)
{
    SITH_ASSERTREL((size_t)functionId < SITHCONTROL_MAXFUNCTIONS);
    SITH_ASSERTREL(controlId < STDCONTROL_MAX_KEYID);

    if ( (aControlFlags[functionId] & SITHCONTROLFUNCTION_REGISTERED) != 0 )
    {
        if ( aControlBindings[functionId].numBindings == STDCONTROL_MAXBINDINGS )
        {
            SITHLOG_ERROR("Tried to bind more than %d controls to function %d.\n", STDCONTROL_MAXBINDINGS, functionId);
        }
        else
        {
            SithControlBinding* pBinding = &aControlBindings[functionId].aBindings[aControlBindings[functionId].numBindings];
            pBinding->flags     = flags & ~(SITHCONTROLBIND_AXIS_ENABLED | SITHCONTROLBIND_KEYCONTROL | SITHCONTROLBIND_AXISCONTROL) | SITHCONTROLBIND_KEYCONTROL;
            pBinding->controlId = controlId;
            aControlBindings[functionId].numBindings++;
        }
    }
}

SithControlBinding* J3DAPI sithControl_BindAxis(SithControlFunction functionId, size_t axis, SithControlBindFlag flags)
{
    SITH_ASSERTREL((size_t)functionId < SITHCONTROL_MAXFUNCTIONS);

    if ( !stdControl_TestAxisFlag(axis, STDCONTROL_AXIS_REGISTERED) )
    {
        SITHLOG_STATUS("Tried to bind unavailable axis %d to function %d.\n", axis, functionId);
        return NULL;
    }

    if ( (aControlFlags[functionId] & SITHCONTROLFUNCTION_REGISTERED) == 0 )
    {
        SITHLOG_STATUS("Tried to bind axis %d to unavailable function %d.\n", axis, functionId);
        return NULL;
    }

    if ( aControlBindings[functionId].numBindings == STDCONTROL_MAXBINDINGS )
    {
        SITHLOG_ERROR("Tried to bind more than %d controls to function %d.\n", STDCONTROL_MAXBINDINGS, functionId);
        return NULL;
    }

    if ( stdControl_TestAxisFlag(axis, STDCONTROL_AXIS_ENABLED) )
    {
        SITHLOG_STATUS("Control %x rebound to function %d.\n", axis, functionId);
        sithControl_UnbindControl(SITHCONTROLBIND_AXISCONTROL, axis);
    }

    if ( !stdControl_EnableAxis(axis) )
    {
        SITHLOG_ERROR("Platform could not enable axis %d.\n", axis);
        return NULL;
    }

    flags = flags & ~(SITHCONTROLBIND_KEYCONTROL | SITHCONTROLBIND_AXISCONTROL) | SITHCONTROLBIND_AXISCONTROL;
    if ( (flags & SITHCONTROLBIND_AXIS_HASDEADZONE) != 0 )
    {
        stdControl_SetAxisFlags(axis, STDCONTROL_AXIS_HASDEADZONE);
    }

    SithControlBinding* pBinding = &aControlBindings[functionId].aBindings[aControlBindings[functionId].numBindings];
    pBinding->flags     = flags;
    pBinding->controlId = axis;
    aControlBindings[functionId].numBindings++;

    return pBinding;
}

void J3DAPI sithControl_UnbindFunction(SithControlFunction fctnID)
{
    SITH_ASSERTREL((((fctnID) >= (0)) ? (((fctnID) <= (SITHCONTROL_MAXFUNCTIONS - 1)) ? 1 : 0) : 0));
    memset(aControlBindings[fctnID].aBindings, 0, sizeof(aControlBindings[fctnID].aBindings)); // Fixed: Fixed clearing whole array, originally was clearing only 8 bytes i.e.: STDCONTROL_MAXBINDINGS
    aControlBindings[fctnID].numBindings = 0;
}

void J3DAPI sithControl_UnbindFunctionIndex(size_t funcId, size_t bindIndex) // Note, types of funcId and bindIndex were changed to size_t
{
    SITH_ASSERTREL((((funcId) >= (0)) ? (((funcId) <= (SITHCONTROL_MAXFUNCTIONS - 1)) ? 1 : 0) : 0));
    SITH_ASSERTREL((((bindIndex) >= (0)) ? (((bindIndex) <= (aControlBindings[funcId].numBindings - 1)) ? 1 : 0) : 0));

    --aControlBindings[funcId].numBindings;
    for ( size_t i = bindIndex; i < aControlBindings[funcId].numBindings; ++i )
    {
        SithControlBinding* pBinding1 = &aControlBindings[funcId].aBindings[i + 1];
        SithControlBinding* pBinding2 = &aControlBindings[funcId].aBindings[i];
        pBinding2->controlId   = pBinding1->controlId;
        pBinding2->flags       = pBinding1->flags;
        pBinding2->sensitivity = pBinding1->sensitivity;
    }
}

void J3DAPI sithControl_UnbindFunctionControl(SithControlFunction fctnID, size_t controlId)
{
    SITH_ASSERTREL((((fctnID) >= (0)) ? (((fctnID) <= (SITHCONTROL_MAXFUNCTIONS - 1)) ? 1 : 0) : 0));
    for ( size_t bindIndex = 0; bindIndex < aControlBindings[fctnID].numBindings; ++bindIndex )
    {
        if ( aControlBindings[fctnID].aBindings[bindIndex].controlId == controlId )
        {
            sithControl_UnbindFunctionIndex(fctnID, bindIndex);
            return;
        }
    }
}

void J3DAPI sithControl_UnbindControl(SithControlBindFlag flags, size_t controlId)
{
    for ( size_t funcId = 0; funcId < SITHCONTROL_MAXFUNCTIONS; funcId++ )
    {
        for ( size_t bindIndex = 0; bindIndex < aControlBindings[funcId].numBindings; ++bindIndex )
        {
            if ( (flags & aControlBindings[funcId].aBindings[bindIndex].flags) != 0
                && aControlBindings[funcId].aBindings[bindIndex].controlId == controlId )
            {
                sithControl_UnbindFunctionIndex(funcId, bindIndex);
                return;
            }
        }
    }
}

void J3DAPI sithControl_RegisterControlCallback(SithControlCallback pfCallback)
{
    if ( numControlCallbacks >= STD_ARRAYLEN(aControlCallbacks) ) // Fixed: Fix bound check to gte, was gt
    {
        SITHLOG_ERROR("Tried to register too many control callbacks.\n");
        return;
    }
    aControlCallbacks[numControlCallbacks++] = pfCallback;
}

void J3DAPI sithControl_Update(float secDeltaTime, uint32_t msecDeltaTime)
{
    if ( bControlOpen )
    {
        if ( sithPlayer_g_pLocalPlayerThing
            && (sithPlayer_g_pLocalPlayerThing->thingInfo.actorInfo.flags & SITH_AF_NOIDLECAMERA) == 0
            && (sithPlayer_g_pLocalPlayerThing->flags & (SITH_TF_DYING | SITH_TF_DESTROYED)) == 0
            && (sithCamera_g_stateFlags & SITHCAMERA_STATE_CUTSCENE) == 0
            && sithPlayer_g_pLocalPlayerThing->moveInfo.physics.velocity.x == 0.0f
            && sithPlayer_g_pLocalPlayerThing->moveInfo.physics.velocity.y == 0.0f
            && sithPlayer_g_pLocalPlayerThing->moveInfo.physics.velocity.z == 0.0f )
        {
            bool bIdleCheck = true;
            SithThingMoveStatus moveStatus = sithPlayer_g_pLocalPlayerThing->moveStatus;
            if ( moveStatus == SITHPLAYERMOVE_HANGING || moveStatus == SITHPLAYERMOVE_CLIMBIDLE )
            {
                bIdleCheck = false;
            }

            if ( (sithPlayer_g_pLocalPlayerThing->moveInfo.physics.flags & (SITH_PF_JEEP | SITH_PF_RAFT | SITH_PF_MINECAR)) != 0 )
            {
                bIdleCheck = false;
            }

            if ( sithTime_IsPaused() )
            {
                bIdleCheck = false;
            }

            if ( !bIdleCheck )
            {
                msecPlayerIdleTime = 0;
            }

            if ( stdControl_ControlsIdle() && bIdleCheck )
            {
                msecPlayerIdleTime += msecDeltaTime;
                if ( msecPlayerIdleTime > STDCONTROL_IDLECAM_TIMEOUT && sithCamera_g_pCurCamera != &sithCamera_g_aCameras[SITHCAMERA_IDLECAMERANUM] )
                {
                    SITHLOG_STATUS("Switch to idle camera.\n");
                    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[SITHCAMERA_IDLECAMERANUM], sithPlayer_g_pLocalPlayerThing, 0);
                    sithCamera_SetCurrentCamera(&sithCamera_g_aCameras[SITHCAMERA_IDLECAMERANUM]);
                }
            }
            else
            {
                msecPlayerIdleTime = 0;
                if ( sithCamera_g_pCurCamera == &sithCamera_g_aCameras[SITHCAMERA_IDLECAMERANUM] )
                {
                    SITHLOG_STATUS("Switch out of idle camera.\n");
                    sithCamera_SetCurrentCamera(&sithCamera_g_aCameras[SITHCAMERA_EXTCAMERANUM]);
                }
            }
        }
        else if ( sithCamera_g_pCurCamera == &sithCamera_g_aCameras[SITHCAMERA_IDLECAMERANUM] )
        {
            msecPlayerIdleTime = 0;
            SITHLOG_STATUS("Switch out of idle camera.\n");
            sithCamera_SetCurrentCamera(&sithCamera_g_aCameras[SITHCAMERA_EXTCAMERANUM]);
        }

        // Process callbacks
        if ( sithWorld_g_pCurrentWorld->pLocalPlayer && numControlCallbacks > 0 )
        {
            sithControl_ReadControls();
            for ( size_t i = 0; i < numControlCallbacks; ++i )
            {
                if ( aControlCallbacks[i] && aControlCallbacks[i](sithWorld_g_pCurrentWorld->pLocalPlayer, secDeltaTime) ) {
                    break;
                }
            }

            sithControl_FinishRead();
        }
    }
}

float J3DAPI sithControl_GetKeyAsAxisNormalized(size_t axisId)
{
    SITH_ASSERTREL((axisId >= 0) && (axisId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(((aControlFlags[axisId]) & (SITHCONTROLFUNCTION_AXIS)));

    float value = 0.0f;
    for ( size_t i = 0; i < aControlBindings[axisId].numBindings; ++i )
    {
        float axisValue = 0.0f;
        SithControlBinding* pBinding  = &aControlBindings[axisId].aBindings[i];
        SithControlBindFlag bindflags = pBinding->flags;

        if ( (bindflags & SITHCONTROLBIND_AXISCONTROL) != 0 )
        {
            if ( (sithControl_g_controlOptions & 0x20) != 0 && STDCONTROL_ISJOYSTICKAXIS(pBinding->controlId) ) // Joystick axis
            {
                continue;
            }

            axisValue = stdControl_ReadAxis(pBinding->controlId);
        }
        else
        {
            if ( (sithControl_g_controlOptions & 0x20) != 0 && STDCONTROL_ISJOYSTICKBUTTON(pBinding->controlId) ) // Fixed: Macro extracts correct aid by clearing any additional axis flags
            {
                continue;
            }

            axisValue = stdControl_ReadKeyAsAxis(pBinding->controlId);
        }

        if ( (aControlBindings[axisId].aBindings[i].flags & SITHCONTROLBIND_AXIS_ENABLED) != 0 )
        {
            axisValue = axisValue * sithTime_g_fps;
        }

        if ( axisValue != 0.0f )
        {
            if ( (bindflags & SITHCONTROLBIND_INVERT_AXIS) != 0 )
            {
                axisValue = -axisValue;
            }

            if ( pBinding->sensitivity != 0.0f )
            {
                axisValue = pBinding->sensitivity * axisValue;
            }

            value += STDMATH_CLAMP(axisValue, -1.0f, 1.0f);
        }
    }

    return STDMATH_CLAMP(value, -1.0f, 1.0f);;
}

float J3DAPI sithControl_GetKeyAsAxis(SithControlFunction axisId)
{
    SITH_ASSERTREL((axisId >= 0) && (axisId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(((aControlFlags[axisId]) & (SITHCONTROLFUNCTION_AXIS)));

    float value = 0.0f;
    for ( size_t i = 0; i < aControlBindings[axisId].numBindings; ++i )
    {
        SithControlBinding* pBinding  = &aControlBindings[axisId].aBindings[i];
        SithControlBindFlag bindflags = pBinding->flags;

        if ( (bindflags & SITHCONTROLBIND_AXIS_ENABLED) == 0
            && (bindflags & SITHCONTROLBIND_AXISCONTROL) == 0
            && ((sithControl_g_controlOptions & 0x20) == 0 || !STDCONTROL_ISJOYSTICKBUTTON(pBinding->controlId)) ) // Fixed: Macro extracts correct aid by clearing any additional axis flags
        {
            float axisValue = stdControl_ReadKeyAsAxis(pBinding->controlId);
            if ( axisValue != 0.0f )
            {
                if ( (bindflags & SITHCONTROLBIND_INVERT_AXIS) != 0 )
                {
                    axisValue = -axisValue;
                }

                if ( pBinding->sensitivity != 0.0f )
                {
                    axisValue = pBinding->sensitivity * axisValue;
                }

                value += axisValue;
            }
        }
    }

    return value;
}

float J3DAPI sithControl_GetAxis(SithControlFunction axisId)
{
    SITH_ASSERTREL((axisId >= 0) && (axisId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(((aControlFlags[axisId]) & (SITHCONTROLFUNCTION_AXIS)));

    float value = 0.0f;
    for ( size_t i = 0; i < aControlBindings[axisId].numBindings; i++ )
    {
        SithControlBinding* pBinding = &aControlBindings[axisId].aBindings[i];
        SithControlBindFlag bindflags = pBinding->flags;

        if ( (bindflags & SITHCONTROLBIND_AXIS_ENABLED) != 0
            && ((sithControl_g_controlOptions & 0x20) == 0 || STDCONTROL_ISMOUSEAXIS(pBinding->controlId)) ) // Fixed: Macro extracts correct aid by clearing any additional axis flags. Also macro checks also upper mouse axis bound aid
        {
            float axisValue = (float)stdControl_ReadAxisRaw(aControlBindings[axisId].aBindings[i].controlId);

            if ( (bindflags & SITHCONTROLBIND_INVERT_AXIS) != 0 )
            {
                axisValue = -axisValue;
            }

            if ( aControlBindings[axisId].aBindings[i].sensitivity != 0.0f )
            {
                axisValue = aControlBindings[axisId].aBindings[i].sensitivity * axisValue;
            }

            value += axisValue;
        }
    }

    return value;
}

// Added: Returns true if the thing faces a climbable surface it can mount.
//        Mirrors the wall mount check in the original sithPlayerControls_ProcessGeneralMove (0x0042C3F1).
static bool sithControl_IsClimbSurfaceAhead(SithThing* pThing)
{
    const float moveSize = pThing->collide.movesize;
    sithCollision_SearchForCollisions(pThing->pInSector, pThing, &pThing->pos, &pThing->orient.lvec, 2.0f * moveSize, moveSize, 0xA00);

    bool bFound = false;
    for ( SithCollision* pCollision = sithCollision_PopStack(); pCollision && !bFound; pCollision = sithCollision_PopStack() )
    {
        if ( (pCollision->type & (SITHCOLLISION_WORLD | SITHCOLLISION_ADJOINCROSS)) == 0
            || !pCollision->pSurfaceCollided
            || (pCollision->pSurfaceCollided->flags & SITH_SURFACE_CLIMBABLE) == 0 )
        {
            continue;
        }

        // Thing must face the surface, compared in the horizontal plane
        const rdVector3* pNormal = &pCollision->pSurfaceCollided->face.normal;
        float nx = -pNormal->x, ny = -pNormal->y;
        float lx = pThing->orient.lvec.x, ly = pThing->orient.lvec.y;
        float nlen = sqrtf(nx * nx + ny * ny);
        float llen = sqrtf(lx * lx + ly * ly);
        if ( nlen > 0.0f && llen > 0.0f && (nx * lx + ny * ly) / (nlen * llen) > 0.8f )
        {
            bFound = true;
        }
    }

    sithCollision_DecreaseStackLevel();
    return bFound;
}

// Added: Adapts the original key based player controls to the modern (WASD + mouse look) control scheme.
static int sithControl_ApplyModernControlAssists(SithControlFunction keyId, int value, int* pState)
{
    SithThing* pPlayer = sithPlayer_g_pLocalPlayerThing;
    SithThingMoveStatus moveStatus = pPlayer->moveStatus;

    // Underwater the swim code uses forward/back keys to pitch the player down/up (aircraft style);
    // report them as pressed from vertical mouse movement so mouse up looks up.
    // PITCH axis is mouse Y (mouse down -> positive, unless inverted by user setting).
    const bool bUnderwater = (pPlayer->moveInfo.physics.flags & SITH_PF_ONWATERSURFACE) == 0
        && pPlayer->pInSector && (pPlayer->pInSector->flags & SITH_SECTOR_UNDERWATER) != 0;
    if ( (keyId == SITHCONTROL_FORWARD || keyId == SITHCONTROL_BACK) && bUnderwater )
    {
        if ( !value )
        {
            const float threshold = 2.0f * sithControl_mouseLookSensitivity; // ~2 mouse counts per frame
            float pitch = sithControl_GetAxis(SITHCONTROL_PITCH);
            if ( (keyId == SITHCONTROL_FORWARD && pitch > threshold) || (keyId == SITHCONTROL_BACK && pitch < -threshold) )
            {
                return 1;
            }
        }

        return value;
    }

    if ( keyId == SITHCONTROL_TURNLEFT || keyId == SITHCONTROL_TURNRIGHT )
    {
        if ( value )
        {
            return value;
        }

        // Climbing and ledge hanging use turn keys to move left/right; let the sidestep keys (A/D) do it too
        if ( moveStatus == SITHPLAYERMOVE_HANGING || moveStatus == SITHPLAYERMOVE_CLIMBIDLE
            || (moveStatus >= SITHPLAYERMOVE_CLIMBING_UP && moveStatus <= SITHPLAYERMOVE_CLIMBING_RIGHT) )
        {
            return sithControl_GetKey(keyId == SITHCONTROL_TURNLEFT ? SITHCONTROL_STPLEFT : SITHCONTROL_STPRIGHT, pState);
        }

        // Whip and swim move code reads only turn keys and ignores the analog MOUSETURN axis,
        // so report turn keys as pressed from horizontal mouse movement.
        // Note, swim condition mirrors sithPlayerControls_Process which calls ProcessSwimMove on water surface
        //       and in underwater/aetherium sectors (0x0042BD14).
        bool bWhip = moveStatus == SITHPLAYERMOVE_WHIPSWINGING || moveStatus == SITHPLAYERMOVE_WHIPCLIMBIDLE
            || moveStatus == SITHPLAYERMOVE_WHIP_CLIMB_START || moveStatus == SITHPLAYERMOVE_WHIP_UNK2;
        bool bSwim = (pPlayer->moveInfo.physics.flags & SITH_PF_ONWATERSURFACE) != 0
            || (pPlayer->pInSector && (pPlayer->pInSector->flags & (SITH_SECTOR_UNDERWATER | SITH_SECTOR_AETHERIUM)) != 0);
        if ( bWhip || bSwim )
        {
            // MOUSETURN is bound to inverted mouse X, i.e. positive value means mouse moved left
            const float threshold = 2.0f * sithControl_mouseLookSensitivity; // ~2 mouse counts per frame
            float turn = sithControl_GetAxis(SITHCONTROL_MOUSETURN);
            if ( (keyId == SITHCONTROL_TURNLEFT && turn > threshold) || (keyId == SITHCONTROL_TURNRIGHT && turn < -threshold) )
            {
                return 1;
            }
        }
    }
    else if ( keyId == SITHCONTROL_FORWARD || keyId == SITHCONTROL_RUNFWD )
    {
        // Forward + jump is a forward jump, which prevents mounting a ladder/climbable wall while walking into it.
        // Drop forward while the climb assist is active or when jump is pressed standing in front of a climbable surface.
        if ( value && (sithControl_climbAssistState != SITHCONTROL_CLIMBASSIST_NONE
            || (moveStatus == SITHPLAYERMOVE_STILL && sithControl_GetKey(SITHCONTROL_JUMP, NULL) && sithControl_IsClimbSurfaceAhead(pPlayer))) )
        {
            if ( pState )
            {
                *pState = 0;
            }

            return 0;
        }
    }
    else if ( keyId == SITHCONTROL_JUMP )
    {
        // While walking/running the original code always jumps forward and mounts climbable surfaces only from standing.
        // So when jump is pressed moving towards a climbable surface, hold the jump back until the player stops, then deliver it.
        const uint32_t msecNow = stdPlatform_GetTimeMsec();
        switch ( sithControl_climbAssistState )
        {
            case SITHCONTROL_CLIMBASSIST_NONE:
            {
                bool bMoving = moveStatus == SITHPLAYERMOVE_WALKING || moveStatus == SITHPLAYERMOVE_RUNNING
                    || moveStatus == SITHPLAYERMOVE_STAND2WALK || moveStatus == SITHPLAYERMOVE_STAND2RUN || moveStatus == SITHPLAYERMOVE_WALK2STAND;
                if ( value && bMoving && sithControl_IsClimbSurfaceAhead(pPlayer) )
                {
                    sithControl_climbAssistState     = SITHCONTROL_CLIMBASSIST_STOPPING;
                    sithControl_msecClimbAssistStart = msecNow;
                    value = 0;
                }
            } break;

            case SITHCONTROL_CLIMBASSIST_STOPPING:
            {
                value = 0;
                if ( moveStatus == SITHPLAYERMOVE_STILL )
                {
                    sithControl_climbAssistState     = SITHCONTROL_CLIMBASSIST_JUMPING;
                    sithControl_msecClimbAssistStart = msecNow;
                }
                else if ( msecNow - sithControl_msecClimbAssistStart > 1000 )
                {
                    sithControl_climbAssistState = SITHCONTROL_CLIMBASSIST_NONE; // Give up
                }
            } break;

            case SITHCONTROL_CLIMBASSIST_JUMPING:
            {
                // Deliver jump press until the player leaves standing state (mounting) or timeout
                if ( moveStatus != SITHPLAYERMOVE_STILL || msecNow - sithControl_msecClimbAssistStart > 300 )
                {
                    sithControl_climbAssistState = SITHCONTROL_CLIMBASSIST_NONE;
                }
                else
                {
                    value = 1;
                    if ( pState )
                    {
                        *pState = 1;
                    }
                }
            } break;
        }

        if ( !value && pState )
        {
            *pState = 0;
        }
    }

    return value;
}

int J3DAPI sithControl_GetKey(SithControlFunction keyId, int* pState)
{
    SITH_ASSERTREL((keyId >= 0) && (keyId < SITHCONTROL_MAXFUNCTIONS));
    SITH_ASSERTREL(((aControlFlags[keyId]) & (SITHCONTROLFUNCTION_KEY)));

    if ( pState )
    {
        *pState = 0;
    }

    int value = 0;
    for ( size_t i = 0; i < aControlBindings[keyId].numBindings; ++i )
    {
        size_t keyNum = aControlBindings[keyId].aBindings[i].controlId;
        if ( (sithControl_g_controlOptions & 0x20) == 0 || STDCONTROL_ISKEYBOARDBUTTON(keyNum) || STDCONTROL_ISMOUSEBUTTON(keyNum) ) // Fixed: STDCONTROL_ISMOUSEBUTTON macro checks also upper mouse axis bound aid
        {
            if ( (aControlBindings[keyId].aBindings[i].flags & SITHCONTROLBIND_AXISCONTROL) != 0 )
            {
                value |= stdControl_ReadAxisAsKey(keyNum, pState);
            }

            if ( (aControlBindings[keyId].aBindings[i].flags & SITHCONTROLBIND_KEYCONTROL) != 0 )
            {
                value |= stdControl_ReadKey(keyNum, pState);
            }
        }
    }

    // Added: Modern control assists
    if ( sithControl_bMouseLook && sithPlayer_g_pLocalPlayerThing )
    {
        value = sithControl_ApplyModernControlAssists(keyId, value, pState);
    }

    return value;
}

void sithControl_ReadControls(void)
{
    SITH_ASSERTREL(bControlStartup && bControlOpen);
    stdControl_ReadControls();
}

void sithControl_FinishRead(void)
{
    stdControl_FinishRead();
}

void sithControl_DefaultInit(void)
{
    SITH_ASSERTREL(bControlStartup && !bControlOpen);
    sithControl_Reset();
    sithControl_g_controlOptions = 0x04;
    sithControl_RegisterKeyboardBindings();
    sithControl_RegisterJoystickBindings();
    sithControl_RegisterMouseBindings();
}

void sithControl_RebindKeyboard(void)
{
    for ( size_t funcId = 0; funcId < SITHCONTROL_MAXFUNCTIONS; ++funcId )
    {
        bool bFound;
        do
        {
            bFound = false;
            for ( size_t bindIndex = 0; bindIndex < aControlBindings[funcId].numBindings && !bFound; ++bindIndex )
            {
                if ( (aControlBindings[funcId].aBindings[bindIndex].flags & SITHCONTROLBIND_AXISCONTROL) == 0
                    && STDCONTROL_ISKEYBOARDBUTTON(aControlBindings[funcId].aBindings[bindIndex].controlId) )
                {
                    sithControl_UnbindFunctionIndex(funcId, bindIndex);
                    bFound = true;
                }
            }
        } while ( bFound );
    }

    sithControl_RegisterKeyboardBindings();
}

void sithControl_UnbindJoystickAxes(void)
{
    for ( size_t funcId = 0; funcId < SITHCONTROL_MAXFUNCTIONS; ++funcId )
    {
        bool bFound;
        do
        {
            bFound = false;
            SithControlFunctionBinding* pBinding = &aControlBindings[funcId];
            for ( size_t bindIndex = 0; bindIndex < pBinding->numBindings && !bFound; ++bindIndex )
            {
                int32_t bAxisControl = pBinding->aBindings[bindIndex].flags & SITHCONTROLBIND_AXISCONTROL;
                if ( (!bAxisControl && STDCONTROL_ISJOYSTICKBUTTON(pBinding->aBindings[bindIndex].controlId))
                    || (bAxisControl && STDCONTROL_ISJOYSTICKAXIS(pBinding->aBindings[bindIndex].controlId)) ) // Joystick axis
                {
                    sithControl_UnbindFunctionIndex(funcId, bindIndex);
                    bFound = true;
                }
            }
        } while ( bFound );
    }

    stdControl_DisableReadJoysticks();
}

void sithControl_RebindJoystick(void)
{
    sithControl_UnbindJoystickAxes();
    sithControl_RegisterJoystickBindings();
}

void sithControl_UnbindMouseAxes(void)
{
    for ( size_t funcId = 0; funcId < SITHCONTROL_MAXFUNCTIONS; ++funcId )
    {
        bool bFound;
        do
        {
            bFound = false;
            for ( size_t bindIndex = 0; bindIndex < aControlBindings[funcId].numBindings && !bFound; ++bindIndex )
            {
                bool bAxis = (aControlBindings[funcId].aBindings[bindIndex].flags & SITHCONTROLBIND_AXISCONTROL) != 0;
                if ( !bAxis && STDCONTROL_ISMOUSEBUTTON(aControlBindings[funcId].aBindings[bindIndex].controlId)
                    || bAxis && STDCONTROL_ISMOUSEAXIS(aControlBindings[funcId].aBindings[bindIndex].controlId) )
                {
                    sithControl_UnbindFunctionIndex(funcId, bindIndex);
                    bFound = true;
                }
            }
        } while ( bFound );
    }
}

void sithControl_RebindMouse(void)
{
    sithControl_UnbindMouseAxes();
    sithControl_RegisterMouseBindings();
}

bool sithControl_IsMouseLookEnabled(void)
{
    return sithControl_bMouseLook;
}

void sithControl_SetMouseLook(bool bEnable, float sensitivity, bool bInvertY)
{
    sithControl_bMouseLook           = bEnable;
    sithControl_mouseLookSensitivity = sensitivity;
    sithControl_bMouseLookInvertY    = bInvertY;
}

void sithControl_Reset(void)
{
    memset(aControlBindings, 0, sizeof(aControlBindings));
    stdControl_Reset();
}

void sithControl_RegisterControlFunctions(void)
{
    sithControl_RegisterKeyFunction(SITHCONTROL_FORWARD);
    sithControl_RegisterKeyFunction(SITHCONTROL_BACK);
    sithControl_RegisterKeyFunction(SITHCONTROL_LEFT);
    sithControl_RegisterKeyFunction(SITHCONTROL_RIGHT);
    sithControl_RegisterKeyFunction(SITHCONTROL_TURNLEFT);
    sithControl_RegisterKeyFunction(SITHCONTROL_TURNRIGHT);
    sithControl_RegisterKeyFunction(SITHCONTROL_CRAWLTOGGLE);
    sithControl_RegisterKeyFunction(SITHCONTROL_JUMP);
    sithControl_RegisterKeyFunction(SITHCONTROL_ACT1);
    sithControl_RegisterKeyFunction(SITHCONTROL_RUNFWD);
    sithControl_RegisterKeyFunction(SITHCONTROL_FIRE1);
    sithControl_RegisterKeyFunction(SITHCONTROL_ACTIVATE);
    sithControl_RegisterKeyFunction(SITHCONTROL_ACT2);
    sithControl_RegisterKeyFunction(SITHCONTROL_NEXTWEAP);
    sithControl_RegisterKeyFunction(SITHCONTROL_PREVWEAP);
    sithControl_RegisterKeyFunction(SITHCONTROL_LIGHTERTOGGLE);
    sithControl_RegisterKeyFunction(SITHCONTROL_LOOK);
    sithControl_RegisterKeyFunction(SITHCONTROL_STPLEFT);
    sithControl_RegisterKeyFunction(SITHCONTROL_STPRIGHT);
    sithControl_RegisterKeyFunction(SITHCONTROL_WEAPONTOGGLE);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT0);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT1);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT2);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT3);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT4);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT5);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT6);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT7);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT8);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT9);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT10);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT11);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT12);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT13);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT14);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT15);
    sithControl_RegisterKeyFunction(SITHCONTROL_WSELECT16);
    sithControl_RegisterKeyFunction(SITHCONTROL_PREVWEAPON);
    sithControl_RegisterKeyFunction(SITHCONTROL_NEXTWEAPON);
    sithControl_RegisterKeyFunction(SITHCONTROL_GAMESAVE);
    sithControl_RegisterKeyFunction(SITHCONTROL_MAP);
    sithControl_RegisterKeyFunction(SITHCONTROL_INCREASE);
    sithControl_RegisterKeyFunction(SITHCONTROL_DECREASE);
    sithControl_RegisterKeyFunction(SITHCONTROL_CAMERAMODE);
    sithControl_RegisterKeyFunction(SITHCONTROL_TALK);
    sithControl_RegisterKeyFunction(SITHCONTROL_ACT3);
    sithControl_RegisterKeyFunction(SITHCONTROL_HEALTH);
    sithControl_RegisterKeyFunction(SITHCONTROL_CHALK);
    sithControl_RegisterKeyFunction(SITHCONTROL_DEBUG);
    sithControl_RegisterKeyFunction(SITHCONTROL_GAMMA);
    sithControl_RegisterKeyFunction(SITHCONTROL_SCREENSHOT);
    sithControl_RegisterKeyFunction(SITHCONTROL_CAMERAZOOMIN);
    sithControl_RegisterKeyFunction(SITHCONTROL_CAMERAZOOMOUT);

    // Note this if scope if removed in release version but we'll keep it
    //if ( (sithMain_g_sith_mode.debugModeFlags & SITHDEBUG_INEDITOR) != 0 )
    {
        sithControl_RegisterKeyFunction(SITHCONTROL_MLOOK);
        sithControl_RegisterAxisFunction(SITHCONTROL_PITCH, SITHCONTROLFUNCTION_UNKNOWN_8);
        sithControl_RegisterKeyFunction(SITHCONTROL_CENTER);
        sithControl_RegisterAxisFunction(SITHCONTROL_MOUSETURN, SITHCONTROLFUNCTION_UNKNOWN_8);
    }
}

void sithControl_RegisterKeyboardBindings(void)
{
    if ( bControlStartup )
    {
        sithControl_BindControl(SITHCONTROL_FORWARD, DIK_UP, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_BACK, DIK_DOWN, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_LEFT, DIK_LEFT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_RIGHT, DIK_RIGHT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_TURNLEFT, DIK_LEFT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_TURNRIGHT, DIK_RIGHT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_LOOK, DIK_NUMPAD0, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_LOOK, DIK_INSERT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_STPLEFT, DIK_A, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_STPRIGHT, DIK_S, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_CRAWLTOGGLE, DIK_C, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WEAPONTOGGLE, DIK_SPACE, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_LIGHTERTOGGLE, DIK_L, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_JUMP, DIK_X, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_JUMP, DIK_LMENU, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_JUMP, DIK_RMENU, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACT1, DIK_LSHIFT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACT1, DIK_RSHIFT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_FIRE1, DIK_RCONTROL, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_FIRE1, DIK_LCONTROL, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACT3, DIK_Z, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACTIVATE, DIK_GRAVE, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACT2, DIK_LCONTROL, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_ACT2, DIK_RCONTROL, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT0, DIK_0, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT1, DIK_1, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT2, DIK_2, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT3, DIK_3, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT4, DIK_4, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT5, DIK_5, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT6, DIK_6, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT7, DIK_7, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT8, DIK_8, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT9, DIK_9, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT10, DIK_G, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT11, DIK_P, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT12, DIK_Q, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT13, DIK_W, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT14, DIK_E, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT15, DIK_R, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_WSELECT16, DIK_T, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_PREVWEAPON, DIK_COMMA, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_NEXTWEAPON, DIK_PERIOD, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_GAMESAVE, DIK_F9, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_MAP, DIK_M, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_INCREASE, DIK_EQUALS, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_DECREASE, DIK_MINUS, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_TALK, DIK_F10, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_SCREENSHOT, DIK_F12, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_HEALTH, DIK_H, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_CHALK, DIK_K, (SithControlBindFlag)0);

        // Note this if scope is removed in release version but we'll keep it
        if ( (sithMain_g_sith_mode.debugModeFlags & SITHDEBUG_INEDITOR) != 0 )
        {
            sithControl_EnableDevControls(true);
        }
    }
}

void sithControl_RegisterJoystickBindings(void)
{
    if ( bControlStartup )
    {
        size_t numJoysticks =  stdControl_GetNumJoysticks();
        J3D_UNUSED(numJoysticks);
        // Note, following for loop was found in debug version but was skipped
    #if false
        for ( size_t i = 0; i < numJoysticks; i++ )
        {
            sithControl_BindAxis(SITHCONTROL_TURNLEFT, STDCONTROL_GET_JOYSTICK_AXIS_X(i) | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_TURNRIGHT, STDCONTROL_GET_JOYSTICK_AXIS_X(i) | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_FORWARD, STDCONTROL_GET_JOYSTICK_AXIS_Y(i) | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_RUNFWD, STDCONTROL_GET_JOYSTICK_AXIS_Y(i) | STDCONTROL_AID_POSITIVE_AXIS | STDCONTROL_AID_NEGATIVE_AXIS | STDCONTROL_AID_LOW_SENSITIVITY, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_BACK, STDCONTROL_GET_JOYSTICK_AXIS_Y(i) | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0);

            sithControl_BindControl(SITHCONTROL_ACT2, STDCONTROL_JOYSTICK_GETBUTTON(i, 0), (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_JUMP, STDCONTROL_JOYSTICK_GETBUTTON(i, 1), (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_ACT1, STDCONTROL_JOYSTICK_GETBUTTON(i, 2), (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_ACT3, STDCONTROL_JOYSTICK_GETBUTTON(i, 4), (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_CRAWLTOGGLE, STDCONTROL_JOYSTICK_GETBUTTON(i, 5), (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_WEAPONTOGGLE, STDCONTROL_JOYSTICK_GETBUTTON(i, 9), (SithControlBindFlag)0);
        }
    #endif
    }
}

void sithControl_RegisterMouseBindings(void)
{
    if ( bControlStartup )
    {
        if ( (sithMain_g_sith_mode.debugModeFlags & SITHDEBUG_INEDITOR) != 0 )
        {
            SithControlBinding* pBinding = sithControl_BindAxis(SITHCONTROL_MOUSETURN, STDCONTROL_AID_MOUSE_X, SITHCONTROLBIND_AXIS_ENABLED | SITHCONTROLBIND_INVERT_AXIS);
            if ( pBinding )
            {
                pBinding->sensitivity = 0.05f; // Changed
            }

            pBinding = sithControl_BindAxis(SITHCONTROL_PITCH, STDCONTROL_AID_MOUSE_Y, SITHCONTROLBIND_AXIS_ENABLED);
            if ( pBinding )
            {
                pBinding->sensitivity = 0.05f; // Changed
            }

            sithControl_BindControl(SITHCONTROL_FIRE1, STDCONTROL_KID_MOUSE_LBUTTON, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_ACT2, STDCONTROL_KID_MOUSE_LBUTTON, (SithControlBindFlag)0); // Added
            sithControl_BindControl(SITHCONTROL_JUMP, STDCONTROL_KID_MOUSE_RBUTTON, (SithControlBindFlag)0); // Added

            sithControl_BindControl(SITHCONTROL_WEAPONTOGGLE, STDCONTROL_KID_MOUSE_MBUTTON, (SithControlBindFlag)0); // Added
            sithControl_BindAxis(SITHCONTROL_NEXTWEAP, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0); // Added
            sithControl_BindAxis(SITHCONTROL_PREVWEAP, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0); // Added
            sithControl_BindAxis(SITHCONTROL_NEXTWEAPON, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0); // Added
            sithControl_BindAxis(SITHCONTROL_PREVWEAPON, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0); // Added

           //stdControl_SetMouseSensitivity(0.2, 0.2);
            stdControl_EnableMouseSensitivity(0); // Added: Note axis sensitivity best works when mouse sensitivity is disabled here
        }
        else if ( sithControl_bMouseLook )
        {
            // Added: Modern mouse look bindings.
            // Note, axis sensitivity works best when std mouse sensitivity is disabled (same as editor mode)
            stdControl_EnableMouseSensitivity(0);

            SithControlBinding* pBinding = sithControl_BindAxis(SITHCONTROL_MOUSETURN, STDCONTROL_AID_MOUSE_X, SITHCONTROLBIND_AXIS_ENABLED | SITHCONTROLBIND_INVERT_AXIS);
            if ( pBinding )
            {
                pBinding->sensitivity = sithControl_mouseLookSensitivity;
            }

            SithControlBindFlag pitchFlags = sithControl_bMouseLookInvertY ? (SITHCONTROLBIND_AXIS_ENABLED | SITHCONTROLBIND_INVERT_AXIS) : SITHCONTROLBIND_AXIS_ENABLED;
            pBinding = sithControl_BindAxis(SITHCONTROL_PITCH, STDCONTROL_AID_MOUSE_Y, pitchFlags);
            if ( pBinding )
            {
                pBinding->sensitivity = sithControl_mouseLookSensitivity;
            }

            sithControl_BindControl(SITHCONTROL_ACT2, STDCONTROL_KID_MOUSE_LBUTTON, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_FIRE1, STDCONTROL_KID_MOUSE_LBUTTON, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_LOOK, STDCONTROL_KID_MOUSE_RBUTTON, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_WEAPONTOGGLE, STDCONTROL_KID_MOUSE_MBUTTON, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_NEXTWEAPON, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_PREVWEAPON, STDCONTROL_AID_MOUSE_Z | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0);
        }
        else
        {
            stdControl_EnableMouseSensitivity(1);
            // Note, to get better mouse support, comment out fwd & bkwd movement, add flag STDCONTROL_AID_LOW_SENSITIVITY to turn axis bidings
            // and increase sensitivity in stdControl_ReadAxisAsKey to 0.85f or 0.95f.
            // TODO: if/when player controls are implemented and mouse navigation functionality is changed to INEDITOR, 
            //       refactor this piece of a code to match the INEDITOR scope with exception for mouse button part
            sithControl_BindAxis(SITHCONTROL_TURNLEFT, STDCONTROL_AID_MOUSE_X | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_TURNRIGHT, STDCONTROL_AID_MOUSE_X | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_FORWARD, STDCONTROL_AID_MOUSE_Y | STDCONTROL_AID_NEGATIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_RUNFWD, STDCONTROL_AID_MOUSE_Y | STDCONTROL_AID_NEGATIVE_AXIS | STDCONTROL_AID_POSITIVE_AXIS | STDCONTROL_AID_LOW_SENSITIVITY, (SithControlBindFlag)0);
            sithControl_BindAxis(SITHCONTROL_BACK, STDCONTROL_AID_MOUSE_Y | STDCONTROL_AID_POSITIVE_AXIS, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_ACT2, STDCONTROL_KID_MOUSE_LBUTTON, (SithControlBindFlag)0);
            sithControl_BindControl(SITHCONTROL_JUMP, STDCONTROL_KID_MOUSE_RBUTTON, (SithControlBindFlag)0);
        }

        stdControl_EnableMouse(1);
    }
}

// Following function was found in debug version. it's usage is unknown
void J3DAPI sithControl_sub_44F334(int (J3DAPI* pfFunc)(size_t, const char*, SithControlFunctionFlag, size_t, size_t, SithControlBindFlag, size_t*, int), int a2, int a3, int a4, int a5)
{
    bool bContinue = true;
    for ( size_t i = 0; bContinue && i < SITHCONTROL_MAXFUNCTIONS; ++i )
    {
        bool bKeyBinding = false;
        bool bInvertedKeyBinding = false;
        bool bAxisBinding = false;
        SithControlFunctionBinding* pBinding = &aControlBindings[i];
        bool bAxisFunc = (aControlFlags[i] & SITHCONTROLFUNCTION_AXIS) != 0;
        for ( size_t j = 0; bContinue && j < pBinding->numBindings; ++j )
        {
            size_t controlId = pBinding->aBindings[j].controlId;
            SithControlBindFlag bflags = pBinding->aBindings[j].flags;
            if ( ((bflags & SITHCONTROLBIND_KEYCONTROL) == 0 || controlId >= STDCONTROL_JOYSTICK_FIRSTKID || a2)
                && (((bflags & SITHCONTROLBIND_AXISCONTROL) == 0 || controlId < STDCONTROL_AID_MOUSE_X || controlId >= STDCONTROL_MAX_AXES)
                    && ((bflags & SITHCONTROLBIND_KEYCONTROL) == 0 || controlId < STDCONTROL_KID_MOUSE_LBUTTON || controlId >= STDCONTROL_MAX_KEYID)
                    || a4)
                && (((bflags & SITHCONTROLBIND_AXISCONTROL) == 0 || STDCONTROL_ISMOUSEAXIS(controlId))
                    && ((bflags & SITHCONTROLBIND_KEYCONTROL) == 0 || !STDCONTROL_ISJOYSTICKBUTTON(controlId)) || a3) )
            {
                bContinue = pfFunc(i, aFunctionNames[i], aControlFlags[i], j, controlId, bflags, &pBinding->aBindings[j].controlId, a5);
                bKeyBinding = bKeyBinding
                    || (pBinding->aBindings[j].flags & SITHCONTROLBIND_KEYCONTROL) != 0
                    && (pBinding->aBindings[j].flags & SITHCONTROLBIND_INVERT_AXIS) == 0;
                bInvertedKeyBinding = bInvertedKeyBinding
                    || (pBinding->aBindings[j].flags & SITHCONTROLBIND_KEYCONTROL) != 0
                    && (pBinding->aBindings[j].flags & SITHCONTROLBIND_INVERT_AXIS) != 0;
                bAxisBinding = bAxisBinding || (pBinding->aBindings[j].flags & SITHCONTROLBIND_AXISCONTROL) != 0;
            }
        }

        if ( bContinue && bAxisFunc && !bAxisBinding )
        {
            bContinue = pfFunc(i, aFunctionNames[i], aControlFlags[i], (size_t)-1, 0, SITHCONTROLBIND_AXISCONTROL, NULL, a5);
        }

        if ( bContinue && !bKeyBinding || !pBinding->numBindings )
        {
            bContinue = pfFunc(i, aFunctionNames[i], aControlFlags[i], (size_t)-1, 0, SITHCONTROLBIND_KEYCONTROL, NULL, a5);
        }

        if ( bContinue && bAxisFunc && !bInvertedKeyBinding )
        {
            bContinue = pfFunc(i, aFunctionNames[i], aControlFlags[i], (size_t)-1, 0, SITHCONTROLBIND_INVERT_AXIS | SITHCONTROLBIND_KEYCONTROL, NULL, a5);
        }
    }
}

void sithControl_EnableDevControls(bool bEnable)
{
    if ( bEnable )
    {
        sithControl_BindControl(SITHCONTROL_ACTIVATE, DIK_F11, (SithControlBindFlag)0); // Added: Change to DIK_F11 from DIK_GRAVE; required atm for cycle cameras in devmode 

        // Following are bindings from debug version
        sithControl_BindControl(SITHCONTROL_DEBUG, DIK_BACK, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_GAMMA, DIK_F5, (SithControlBindFlag)0);

        sithControl_BindControl(SITHCONTROL_MLOOK, DIK_V, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_PITCH, DIK_PRIOR, SITHCONTROLBIND_INVERT_AXIS);
        sithControl_BindControl(SITHCONTROL_PITCH, DIK_NEXT, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_CENTER, DIK_HOME, (SithControlBindFlag)0);
        sithControl_BindControl(SITHCONTROL_CENTER, DIK_NUMPAD5, (SithControlBindFlag)0);
    }
    else
    {
        sithControl_UnbindFunction(SITHCONTROL_DEBUG);
        sithControl_UnbindFunction(SITHCONTROL_GAMMA);

        sithControl_UnbindFunctionControl(SITHCONTROL_MLOOK, DIK_V);
        sithControl_UnbindFunctionControl(SITHCONTROL_PITCH, DIK_PRIOR);
        sithControl_UnbindFunctionControl(SITHCONTROL_PITCH, DIK_NEXT);
        sithControl_UnbindFunctionControl(SITHCONTROL_CENTER, DIK_HOME);
        sithControl_UnbindFunctionControl(SITHCONTROL_CENTER, DIK_NUMPAD5);
    }
}