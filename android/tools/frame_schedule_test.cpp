#include "../platform/frame_schedule.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>

int main()
{
    for ( int hz : { 60, 90, 120 } )
    {
        CFrameSchedule schedule;
        int frames = 0;
        for ( int tick = 0; tick < hz * 10; ++tick )
            frames += schedule.OnVsync( 1000000000LL + tick * 1000000000LL / hz );
        assert( frames == 300 );
    }
    CFrameSchedule schedule;
    assert( schedule.OnVsync( 1000000000LL ) );
    assert( !schedule.OnVsync( 1001000000LL ) );
    assert( schedule.OnVsync( 11000000000LL ) ); // load stalls for ten seconds
    assert( !schedule.OnVsync( 11000000000LL ) ); // no catch-up burst
    assert( !schedule.OnVsync( 11008333333LL ) );
    assert( schedule.OnVsync( 11033333333LL ) );
    schedule.Reset(); // resume or display change starts a fresh timeline
    assert( schedule.OnVsync( 12000000000LL ) );
    puts( "frame schedule: 60/90/120 Hz, stall and resume passed" );
}
