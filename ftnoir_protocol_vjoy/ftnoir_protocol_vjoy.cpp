#include "ftnoir_protocol_vjoy.h"
#include "facetracknoir/plugin-api.hpp"

FTNoIR_Protocol::FTNoIR_Protocol()
{
    static char meh[1] = {0};
    VJoy_Initialize(meh, meh);
}

FTNoIR_Protocol::~FTNoIR_Protocol()
{
    VJoy_Shutdown();
}

void FTNoIR_Protocol::sendHeadposeToGame( const double *headpose ) {
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    JOYSTICK_STATE state[2] = { 0 };

    state[0].POV = (4 << 12) | (4 << 8) | (4 << 4) | 4;

    state[0].XAxis = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[Yaw] * VJOY_AXIS_MAX / 180.0));
    state[0].YAxis = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[Pitch] * VJOY_AXIS_MAX / 180.0));
    state[0].ZAxis = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[Roll] * VJOY_AXIS_MAX / 180.0));
    state[0].XRotation = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[TX] * VJOY_AXIS_MAX / 100.0));
    state[0].YRotation = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[TY] * VJOY_AXIS_MAX / 100.0));
    state[0].ZRotation = std::min<int>(VJOY_AXIS_MAX, std::max<int>(VJOY_AXIS_MIN, headpose[TZ] * VJOY_AXIS_MAX / 100.0));

    VJoy_UpdateJoyState(0, state);
}

extern "C" OPENTRACK_EXPORT IProtocol* CALLING_CONVENTION GetConstructor()
{
    return new FTNoIR_Protocol;
}
