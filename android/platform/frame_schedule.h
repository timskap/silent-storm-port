#ifndef A5_FRAME_SCHEDULE_H
#define A5_FRAME_SCHEDULE_H

#include <stdint.h>

// Select display vsyncs for 30 fps. Deadlines stay on an absolute timeline;
// a slow frame skips expired slots instead of causing a catch-up burst.
class CFrameSchedule
{
    int64_t next = 0;
public:
    static constexpr int64_t Period = 1000000000LL / 30;
    void Reset() { next = 0; }
    bool OnVsync( int64_t now )
    {
        const int64_t tolerance = 1000000; // allow refresh-clock rounding/drift
        if ( next && now + tolerance < next ) return false;
        if ( !next || now - next > Period ) next = now;
        next += Period;
        return true;
    }
};

#endif
