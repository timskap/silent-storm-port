#ifndef A5_TOUCH_CAMERA_H
#define A5_TOUCH_CAMERA_H
#include <cmath>
struct A5CameraMotion {
    float panX = 0, panY = 0, zoom = 0, yaw = 0, pitch = 0;
    void add(const A5CameraMotion &m) { panX += m.panX; panY += m.panY; zoom += m.zoom; yaw += m.yaw; pitch += m.pitch; }
    A5CameraMotion scaled(float f) const { return {panX*f, panY*f, zoom*f, yaw*f, pitch*f}; }
};
class A5TouchCamera {
    float cx = 0, cy = 0, distance = 0, angle = 0;
    float startX = 0, startY = 0, zoomTravel = 0, angleTravel = 0;
    bool pan = false, zoom = false, twist = false, moved = false;
public:
    void begin(float x0, float y0, float x1, float y1) {
        cx = startX = (x0+x1)*0.5f; cy = startY = (y0+y1)*0.5f;
        distance = std::hypot(x1-x0, y1-y0); angle = std::atan2(y1-y0, x1-x0);
        zoomTravel = angleTravel = 0; pan = zoom = twist = moved = false;
    }
    bool isTap() const { return !moved; }
    A5CameraMotion move(float x0, float y0, float x1, float y1, float height, bool orbit) {
        A5CameraMotion result;
        if (height <= 0) return result;
        float nx = (x0+x1)*0.5f, ny = (y0+y1)*0.5f;
        float nd = std::hypot(x1-x0, y1-y0), na = std::atan2(y1-y0, x1-x0);
        float dx = (nx-cx)/height, dy = (ny-cy)/height;
        float da = std::remainder(na-angle, 6.28318530718f);
        bool baseline = distance > height*0.04f && nd > height*0.04f;
        float dz = baseline ? std::log(nd/distance) : 0;
        zoomTravel += dz; angleTravel += baseline ? da : 0;
        // Each component has its own dead zone. Crossing it consumes the slop
        // instead of applying a sudden jump, and never locks out other motions.
        if (pan) { result.panX = dx; result.panY = dy; }
        else if (std::hypot(nx-startX,ny-startY) > height*0.009f) pan = true;
        if (zoom && baseline) result.zoom = dz;
        else if (std::fabs(zoomTravel) > 0.025f) zoom = true;
        if (twist && baseline) result.yaw = da;
        else if (std::fabs(angleTravel) > 0.045f) twist = true;
        moved = moved || pan || zoom || twist || orbit;
        if (orbit) {
            result.yaw = result.panX * 2.2f; result.pitch = -result.panY * 1.5f;
            result.panX = result.panY = result.zoom = 0;
        } else {
            result.zoom *= 0.8f; result.yaw *= 0.85f;
        }
        cx = nx; cy = ny; distance = nd; angle = na;
        return result;
    }
};
class A5CameraSmoother {
    A5CameraMotion pending;
public:
    void reset() { pending = {}; }
    void add(const A5CameraMotion &m) { pending.add(m); }
    A5CameraMotion consume(float seconds) {
        if (!(seconds > 0)) return {};
        if (seconds > 0.1f) seconds = 0.1f;
        float fraction = 1.0f - std::exp(-seconds/0.055f);
        A5CameraMotion result = pending.scaled(fraction);
        pending = pending.scaled(1.0f-fraction);
        return result;
    }
};
#endif
