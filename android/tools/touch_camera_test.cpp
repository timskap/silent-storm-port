#include "../platform/touch_camera.h"
#include <cassert>
#include <cstdio>
static bool near(float a, float b) { return std::fabs(a-b)<0.0001f; }
int main() {
    A5TouchCamera g;
    g.begin(300,400,700,400);
    for (int i=0;i<200;++i) {
        float jitter = i%2 ? 1 : -1;
        auto m=g.move(300+jitter,400,700+jitter,400,1000,false);
        assert(near(m.panX,0) && near(m.zoom,0) && near(m.yaw,0));
    }
    assert(g.isTap());
    auto m=g.move(320,400,720,400,1000,false);
    assert(!g.isTap() && near(m.panX,0)); // no jump when slop is crossed
    m=g.move(330,400,730,400,1000,false); assert(near(m.panX,0.01f));
    // Panning must never lock out a subsequent twist or pinch in the same gesture.
    for(int i=1;i<=20;++i) {
        float a=i*0.01f, radius=200+i*3;
        m=g.move(530-radius*std::cos(a),400-radius*std::sin(a),
                 530+radius*std::cos(a),400+radius*std::sin(a),1000,false);
    }
    assert(m.yaw>0.008f && m.zoom>0 && near(m.panX,0));
    // Wrapped angle crossing cannot cause a full-circle jump.
    g.begin(700,399,300,401);
    m=g.move(700,401,300,399,1000,false); assert(near(m.yaw,0));
    // Event frequency / frame rate do not multiply camera displacement.
    A5CameraSmoother a,b;
    A5CameraMotion motion; motion.panX=0.2f;motion.zoom=0.1f;motion.yaw=0.3f;
    a.add(motion); b.add(motion);
    A5CameraMotion aa,bb;
    for(int i=0;i<30;++i) aa.add(a.consume(1.0f/30));
    for(int i=0;i<120;++i) bb.add(b.consume(1.0f/120));
    assert(near(aa.panX,bb.panX) && near(aa.yaw,0.3f) && near(bb.zoom,0.1f));
    a.add(motion);a.reset();assert(near(a.consume(0.1f).yaw,0));
    puts("touch camera: jitter, slop, pan->twist+pinch, angle wrap, smoothing and cancellation passed");
}
