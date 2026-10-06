/*
 *  android_main.cpp -- the Android entry point.
 *
 *  Replaces Game/Main.cpp's WinMain and Game/WinFrame.cpp's window class: the
 *  NativeActivity lifecycle drives EGL surface creation, and touch replaces the
 *  DirectInput mouse.  When the D3D renderer is ported this file keeps its
 *  shape -- the EGL context it creates is the one the GLES backend will render
 *  into (see docs/PORTING.md).
 */
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android/choreographer.h>
#include <atomic>
#include <deque>
#include <set>
#include <algorithm>
#include <errno.h>
#include <android/window.h>
#include <jni.h>
#include <math.h>
#include <mutex>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_harness.h"
#include "gles_present.h"
#include "windows.h"
#include "d3d9.h"
#include "a5_input.h"
#include "frame_schedule.h"
#ifdef A5_HAVE_MAIN
#include "game_entry.h"
#endif
#ifdef A5_HAVE_AUDIO
#include "audio_android.h"
#endif

#define LOG_TAG "SilentStorm"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace {

/*  Mode of the activity: the boot console (engine core checks on screen) or
 *  the game itself.  The game runs when Main is linked and the boot checks
 *  passed with game data mounted; otherwise the console stays up and says why. */
enum ERunMode { RUN_CONSOLE, RUN_GAME };

// Two fingers combine pan, pinch and twist; the third finger switches to tilt.
enum EGestureMode { GM_UNDECIDED, GM_PANZOOM, GM_ORBIT };

struct SEngineState
{
    android_app *pApp        = 0;
    ERunMode     mode        = RUN_CONSOLE;
    bool         bGameRunning = false;
    bool         bSurfaceAlive = false;
    bool         bResumed = false;
    bool         bFocused = false;
    bool         bFramePosted = false;
    bool         bFrameReady = false;
    bool         bResetTiming = true;
    AChoreographer *pChoreographer = 0;
    CFrameSchedule frameSchedule;

    EGLDisplay display       = EGL_NO_DISPLAY;
    EGLSurface surface       = EGL_NO_SURFACE;
    EGLContext context       = EGL_NO_CONTEXT;
    EGLConfig  config        = 0;
    EGLSurface parkingSurface = EGL_NO_SURFACE;
    int        nWidth        = 0;
    int        nHeight       = 0;

    CBootConsole console;
    SBootReport  report;
    bool         bHarnessRun = false;

    /* Touch scrolling (console) / pointer (game). */
    bool  bTouching          = false;
    float fLastTouchY        = 0.0f;
    int   nTouchCount        = 0;
    /*  The game's 1024x768 image is letterboxed into the window, so on a
     *  2520x1080 screen ~40% of it is black border.  A tap there must not
     *  press the mouse button at the cursor's last position -- that clicks
     *  whatever the cursor happens to be over.  False for the whole of a
     *  gesture that began outside the image. */
    bool  bPressInside       = false;

    /* Delay a single-finger press so adding a second finger does not click.
     * Camera gestures use independent floating-point channels. */
    bool   bPressPending     = false;    /* left press seen, not yet delivered */
    bool   bPressSent        = false;    /* left button currently held in the engine */
    double fPressTime        = 0.0;
    float  fPressX           = 0.0f;     /* window coords of the pending press */
    float  fPressY           = 0.0f;
    bool   bGesture          = false;    /* two tracked fingers on screen */
    EGestureMode eGestureMode = GM_UNDECIDED;
    A5TouchCamera touchCamera;
    int    nGestureId0       = -1;       /* pointer ids of the two tracked fingers */
    int    nGestureId1       = -1;
    float  fGestX0 = 0, fGestY0 = 0, fGestX1 = 0, fGestY1 = 0;
    /*  Soft keyboard: raised while the engine draws a focused edit box
     *  (a5_edit_was_active), lowered when the focus goes away. */
    bool   bKeyboardShown    = false;

    std::string szExternalFilesDir;
    std::string szInternalFilesDir;
};

/*  getExternalFilesDir(null) and getFilesDir() through JNI.  These are the only
 *  two paths an app can rely on without runtime permissions, and the port needs
 *  them to know where the user put the game data. */
std::string GetActivityDirectory( android_app *pApp, const char *pszMethod,
                                  bool bTakesArgument )
{
    JNIEnv *pEnv = 0;
    pApp->activity->vm->AttachCurrentThread( &pEnv, 0 );

    std::string szResult;
    jclass activityClass = pEnv->GetObjectClass( pApp->activity->clazz );
    jmethodID method = pEnv->GetMethodID(
        activityClass, pszMethod,
        bTakesArgument ? "(Ljava/lang/String;)Ljava/io/File;" : "()Ljava/io/File;" );

    if ( method )
    {
        jobject file = bTakesArgument
            ? pEnv->CallObjectMethod( pApp->activity->clazz, method, (jstring)0 )
            : pEnv->CallObjectMethod( pApp->activity->clazz, method );
        if ( file )
        {
            jclass fileClass = pEnv->GetObjectClass( file );
            jmethodID getPath = pEnv->GetMethodID( fileClass, "getAbsolutePath",
                                                   "()Ljava/lang/String;" );
            jstring path = (jstring)pEnv->CallObjectMethod( file, getPath );
            if ( path )
            {
                const char *pszPath = pEnv->GetStringUTFChars( path, 0 );
                szResult = pszPath ? pszPath : "";
                pEnv->ReleaseStringUTFChars( path, pszPath );
            }
        }
    }

    pApp->activity->vm->DetachCurrentThread();
    return szResult;
}

/*  The Android soft keyboard, through InputMethodManager.  NativeActivity has
 *  no editable view, so `showSoftInput` on the decor view can be refused by
 *  the IME; the deprecated `toggleSoftInput` is the standing NDK fallback.
 *  Key presses come back as normal key events (the IME has no InputConnection
 *  to talk to, so it falls back to sending them) and reach a5_input_key. */
void ShowSoftKeyboard( android_app *pApp, bool bShow )
{
    JNIEnv *pEnv = 0;
    pApp->activity->vm->AttachCurrentThread( &pEnv, 0 );

    jobject activity = pApp->activity->clazz;
    jclass activityClass = pEnv->GetObjectClass( activity );

    jclass contextClass = pEnv->FindClass( "android/content/Context" );
    jfieldID fidService = pEnv->GetStaticFieldID( contextClass, "INPUT_METHOD_SERVICE", "Ljava/lang/String;" );
    jobject szService = pEnv->GetStaticObjectField( contextClass, fidService );
    jmethodID midGetSystemService = pEnv->GetMethodID( activityClass, "getSystemService",
                                                       "(Ljava/lang/String;)Ljava/lang/Object;" );
    jobject imm = pEnv->CallObjectMethod( activity, midGetSystemService, szService );

    jmethodID midGetWindow = pEnv->GetMethodID( activityClass, "getWindow", "()Landroid/view/Window;" );
    jobject window = pEnv->CallObjectMethod( activity, midGetWindow );
    jclass windowClass = pEnv->FindClass( "android/view/Window" );
    jmethodID midGetDecorView = pEnv->GetMethodID( windowClass, "getDecorView", "()Landroid/view/View;" );
    jobject decorView = pEnv->CallObjectMethod( window, midGetDecorView );

    jclass immClass = pEnv->FindClass( "android/view/inputmethod/InputMethodManager" );
    if ( imm && decorView )
    {
        if ( bShow )
        {
            jmethodID midShow = pEnv->GetMethodID( immClass, "showSoftInput", "(Landroid/view/View;I)Z" );
            const jboolean bShown = pEnv->CallBooleanMethod( imm, midShow, decorView, 0 );
            if ( !bShown )
            {
                jmethodID midToggle = pEnv->GetMethodID( immClass, "toggleSoftInput", "(II)V" );
                pEnv->CallVoidMethod( imm, midToggle, 2 /* SHOW_FORCED */, 0 );
            }
            LOGI( "keyboard: show (showSoftInput=%d)", (int)bShown );
        }
        else
        {
            jclass viewClass = pEnv->FindClass( "android/view/View" );
            jmethodID midGetToken = pEnv->GetMethodID( viewClass, "getWindowToken", "()Landroid/os/IBinder;" );
            jobject token = pEnv->CallObjectMethod( decorView, midGetToken );
            jmethodID midHide = pEnv->GetMethodID( immClass, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z" );
            pEnv->CallBooleanMethod( imm, midHide, token, 0 );
            LOGI( "keyboard: hide" );
        }
    }
    if ( pEnv->ExceptionCheck() )
    {
        pEnv->ExceptionDescribe();
        pEnv->ExceptionClear();
    }
    pApp->activity->vm->DetachCurrentThread();
}

SEngineState *g_pState = 0;
int  HookWindowWidth()  { return g_pState ? g_pState->nWidth : 0; }
int  HookWindowHeight() { return g_pState ? g_pState->nHeight : 0; }
static int g_nPresents = 0;
double NowSeconds();
static double g_fSwapSeconds = 0;
void HookPresent()
{
    if ( !g_pState || !g_pState->bSurfaceAlive ) return;
    const double start = NowSeconds();
    if ( !eglSwapBuffers( g_pState->display, g_pState->surface ) )
    {
        LOGE( "eglSwapBuffers failed: 0x%x", eglGetError() );
        g_pState->bSurfaceAlive = false;
    }
    else
        ++g_nPresents;
    g_fSwapSeconds += NowSeconds() - start;
}
int HookSurfaceAlive() { return g_pState && g_pState->bSurfaceAlive ? 1 : 0; }

// Window surfaces can disappear on lock, backgrounding, rotation or folding.
// Keep the context current on a tiny pbuffer so all engine GL objects survive.
void TerminateDisplay( SEngineState *pState, bool bShutdown = false )
{
    pState->bSurfaceAlive = false;
    pState->bFrameReady = false;
    pState->frameSchedule.Reset();
    pState->bResetTiming = true;
    if ( pState->display == EGL_NO_DISPLAY ) return;
    eglMakeCurrent( pState->display, pState->parkingSurface,
                    pState->parkingSurface, pState->context );
    if ( pState->surface != EGL_NO_SURFACE )
        eglDestroySurface( pState->display, pState->surface );
    pState->surface = EGL_NO_SURFACE;
    if ( !bShutdown ) return;
    pState->console.Shutdown();
    eglMakeCurrent( pState->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
    if ( pState->context != EGL_NO_CONTEXT ) eglDestroyContext( pState->display, pState->context );
    if ( pState->parkingSurface != EGL_NO_SURFACE ) eglDestroySurface( pState->display, pState->parkingSurface );
    eglTerminate( pState->display );
    pState->display = EGL_NO_DISPLAY;
    pState->context = EGL_NO_CONTEXT;
    pState->parkingSurface = EGL_NO_SURFACE;
}

bool InitDisplay( SEngineState *pState )
{
    const bool reuse = pState->context != EGL_NO_CONTEXT;
    if ( !reuse )
    {
        const EGLint attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
            EGL_NONE
        };
        pState->display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
        EGLint count = 0;
        if ( !eglInitialize( pState->display, 0, 0 ) ||
             !eglChooseConfig( pState->display, attributes, &pState->config, 1, &count ) || !count )
        {
            LOGE( "EGL: no GLES 3 config (0x%x)", eglGetError() );
            TerminateDisplay( pState, true );
            return false;
        }
        const EGLint contextAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        const EGLint pbufferAttrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        pState->context = eglCreateContext( pState->display, pState->config, EGL_NO_CONTEXT, contextAttrs );
        pState->parkingSurface = eglCreatePbufferSurface( pState->display, pState->config, pbufferAttrs );
        if ( pState->context == EGL_NO_CONTEXT || pState->parkingSurface == EGL_NO_SURFACE )
        {
            LOGE( "EGL: context/pbuffer creation failed (0x%x)", eglGetError() );
            TerminateDisplay( pState, true );
            return false;
        }
    }
    EGLint format = 0;
    eglGetConfigAttrib( pState->display, pState->config, EGL_NATIVE_VISUAL_ID, &format );
    ANativeWindow_setBuffersGeometry( pState->pApp->window, 0, 0, format );
    pState->surface = eglCreateWindowSurface( pState->display, pState->config, pState->pApp->window, 0 );
    if ( pState->surface == EGL_NO_SURFACE ||
         !eglMakeCurrent( pState->display, pState->surface, pState->surface, pState->context ) )
    {
        LOGE( "EGL: window activation failed (0x%x)", eglGetError() );
        TerminateDisplay( pState );
        return false;
    }
    eglSwapInterval( pState->display, 1 );
    EGLint width = 0, height = 0;
    eglQuerySurface( pState->display, pState->surface, EGL_WIDTH, &width );
    eglQuerySurface( pState->display, pState->surface, EGL_HEIGHT, &height );
    pState->nWidth = width;
    pState->nHeight = height;
    pState->bSurfaceAlive = true;
    pState->frameSchedule.Reset();
    pState->bResetTiming = true;
    if ( !pState->console.IsReady() && !pState->console.Init() ) return false;
    pState->console.SetViewport( width, height );
    LOGI( "EGL surface %dx%d (%s context); target 30 fps", width, height, reuse ? "preserved" : "new GLES 3" );
    return true;
}

bool CanRender( const SEngineState *s )
{
    return s->mode == RUN_GAME && s->bGameRunning && s->bSurfaceAlive && s->bResumed && s->bFocused;
}

void FrameCallback( long frameTimeNanos, void *data )
{
    SEngineState *s = static_cast<SEngineState *>( data );
    s->bFramePosted = false;
    // API 24's callback uses a 32-bit long on armeabi-v7a. Reconstruct the
    // recent timestamp relative to the monotonic clock across its wrap.
    int64_t timestamp = frameTimeNanos;
    if ( sizeof( long ) == 4 )
    {
        const int64_t now = (int64_t)( NowSeconds() * 1e9 );
        timestamp = now - (uint32_t)( (uint32_t)now - (uint32_t)frameTimeNanos );
    }
    s->bFrameReady = CanRender( s ) && s->frameSchedule.OnVsync( timestamp );
}

void DrawFrame( SEngineState *pState )
{
    if ( pState->mode != RUN_CONSOLE || !pState->bSurfaceAlive || !pState->console.IsReady() )
        return;
    pState->console.Render( 0.0 );
    eglSwapBuffers( pState->display, pState->surface );
}

double NowSeconds()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The input worker only copies Android events. Gesture interpretation, engine
// calls and renderer coordinate conversion stay on the game thread.
struct SQueuedInput
{
    int type = 0, action = 0, key = 0;
    double time = 0;
    struct Pointer { int id; float x, y; };
    std::vector<Pointer> pointers;
};
size_t AMotionEvent_getPointerCount( const SQueuedInput *e ) { return e->pointers.size(); }
int AMotionEvent_getPointerId( const SQueuedInput *e, size_t i ) { return e->pointers[i].id; }
float AMotionEvent_getX( const SQueuedInput *e, size_t i ) { return e->pointers[i].x; }
float AMotionEvent_getY( const SQueuedInput *e, size_t i ) { return e->pointers[i].y; }
int AMotionEvent_getAction( const SQueuedInput *e ) { return e->action; }
std::mutex g_inputMutex;
std::deque<SQueuedInput> g_inputEvents;
bool g_resetInput = false;
std::set<int> g_heldKeys; // game thread only

int32_t QueueInput( android_app *app, AInputEvent *event )
{
    SQueuedInput e;
    e.type = AInputEvent_getType( event );
    if ( e.type == AINPUT_EVENT_TYPE_MOTION )
    {
        e.action = ::AMotionEvent_getAction( event );
        e.time = ::AMotionEvent_getEventTime( event ) * 1e-9;
        for ( size_t i = 0; i < ::AMotionEvent_getPointerCount( event ); ++i )
            e.pointers.push_back( { ::AMotionEvent_getPointerId( event, i ),
                                   ::AMotionEvent_getX( event, i ), ::AMotionEvent_getY( event, i ) } );
        if ( e.pointers.empty() ) return 0;
    }
    else if ( e.type == AINPUT_EVENT_TYPE_KEY )
    {
        e.action = AKeyEvent_getAction( event );
        e.key = AKeyEvent_getKeyCode( event );
        e.time = AKeyEvent_getEventTime( event ) * 1e-9;
    }
    else return 0;
    {
        std::lock_guard<std::mutex> lock( g_inputMutex );
        // Bound memory during long loads; cancel held controls after overflow.
        if ( g_inputEvents.size() >= 256 )
        {
            g_inputEvents.clear();
            g_resetInput = true;
        }
        g_inputEvents.push_back( std::move( e ) );
    }
    ALooper_wake( app->looper );
    return 1;
}

#ifdef A5_HAVE_MAIN
/*  Gesture tuning.  The slop and the pinch step scale with the window so the
 *  gestures feel the same across screen densities. */
const double F_PRESS_DELAY = 0.09;       /* seconds a left press is held back */
float GestureSlop( const SEngineState *pState )
{
    const int nMax = pState->nWidth > pState->nHeight ? pState->nWidth : pState->nHeight;
    return 0.012f * nMax;                /* ~30 px on a 2520-wide screen */
}
/*  Deliver the held-back left press: called once the touch is known to be a
 *  single-finger press (it moved, lifted, or outlived the delay), never when
 *  a second finger made it a gesture. */
void FlushPendingPress( SEngineState *pState )
{
    if ( !pState->bPressPending )
        return;
    pState->bPressPending = false;
    float fBackX = 0, fBackY = 0;
    if ( !A5D3DWindowToBackBuffer( pState->fPressX, pState->fPressY, &fBackX, &fBackY ) )
        return;
    a5_set_pointer_position( (long)fBackX, (long)fBackY );
    a5_input_mouse_button( 0, 1 );
    pState->bPressSent = true;
}

/*  A gesture MOVE: zoom by the change of the finger distance, pan by the
 *  movement of the midpoint, rotate by the turn of the finger line (twist) or
 *  by the midpoint drag when a third finger is down (orbit). Independent dead
 *  zones keep two-finger taps from moving the camera. */
void UpdateGesture( SEngineState *pState, const SQueuedInput *pEvent )
{
    const size_t nPointers = AMotionEvent_getPointerCount( pEvent );
    float fX0 = 0, fY0 = 0, fX1 = 0, fY1 = 0;
    int nFound = 0;
    for ( size_t i = 0; i < nPointers; ++i )
    {
        const int nId = AMotionEvent_getPointerId( pEvent, i );
        if ( nId == pState->nGestureId0 )
        {
            fX0 = AMotionEvent_getX( pEvent, i );
            fY0 = AMotionEvent_getY( pEvent, i );
            nFound |= 1;
        }
        else if ( nId == pState->nGestureId1 )
        {
            fX1 = AMotionEvent_getX( pEvent, i );
            fY1 = AMotionEvent_getY( pEvent, i );
            nFound |= 2;
        }
    }
    if ( nFound != 3 )
        return;

    float scaleX = 1, scaleY = 1;
    A5D3DBackBufferScale(&scaleX, &scaleY);
    const float viewportHeight = scaleY > 0 ? 768.0f / scaleY : float(pState->nHeight);
    A5CameraMotion motion = pState->touchCamera.move(fX0, fY0, fX1, fY1,
                                                    viewportHeight, pState->eGestureMode == GM_ORBIT);
    a5_input_camera_motion(&motion);
    if (pState->eGestureMode == GM_UNDECIDED && !pState->touchCamera.isTap())
        pState->eGestureMode = GM_PANZOOM;
    pState->fGestX0 = fX0; pState->fGestY0 = fY0;
    pState->fGestX1 = fX1; pState->fGestY1 = fY1;
}

void EndGesture( SEngineState *pState, bool bAllowTap )
{
    if (!bAllowTap) a5_input_camera_cancel();
    if (!pState->bGesture) return;
    pState->bGesture = false;
    if ( bAllowTap && pState->eGestureMode == GM_UNDECIDED )
    {
        /* two-finger tap: a right click where the first finger sat */
        float fBackX = 0, fBackY = 0;
        if ( A5D3DWindowToBackBuffer( pState->fGestX0, pState->fGestY0, &fBackX, &fBackY ) )
        {
            a5_set_pointer_position( (long)fBackX, (long)fBackY );
            a5_input_mouse_button( 1, 1 );
            a5_input_mouse_button( 1, 0 );
        }
    }
    pState->eGestureMode = GM_UNDECIDED;
}

int32_t HandleGameTouch( SEngineState *pState, const SQueuedInput *pEvent, int32_t nAction )
{
    const float fX = AMotionEvent_getX( pEvent, 0 );
    const float fY = AMotionEvent_getY( pEvent, 0 );

    switch ( nAction )
    {
        case AMOTION_EVENT_ACTION_DOWN:
        {
            pState->nTouchCount = 1;
            float fBackX = 0, fBackY = 0;
            pState->bPressInside = A5D3DWindowToBackBuffer( fX, fY, &fBackX, &fBackY ) != 0;
            if ( pState->bPressInside )
                a5_set_pointer_position( (long)fBackX, (long)fBackY );
            pState->bPressPending = pState->bPressInside;
            pState->bPressSent    = false;
            pState->fPressTime    = pEvent->time;
            pState->fPressX = fX;
            pState->fPressY = fY;
            break;
        }

        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            ++pState->nTouchCount;
            if ( pState->nTouchCount == 2 )
            {
                /* a gesture, not a click: swallow the pending press, or let
                 * go of the button if it already went out */
                pState->bPressPending = false;
                if ( pState->bPressSent )
                {
                    a5_input_mouse_button( 0, 0 );
                    pState->bPressSent = false;
                }
                pState->bGesture      = true;
                pState->eGestureMode  = GM_UNDECIDED;
                a5_input_camera_cancel();
                pState->nGestureId0 = AMotionEvent_getPointerId( pEvent, 0 );
                pState->nGestureId1 = AMotionEvent_getPointerId( pEvent, 1 );
                pState->fGestX0 = AMotionEvent_getX( pEvent, 0 );
                pState->fGestY0 = AMotionEvent_getY( pEvent, 0 );
                pState->fGestX1 = AMotionEvent_getX( pEvent, 1 );
                pState->fGestY1 = AMotionEvent_getY( pEvent, 1 );
                pState->touchCamera.begin(pState->fGestX0, pState->fGestY0, pState->fGestX1, pState->fGestY1);
            }
            else if ( pState->nTouchCount == 3 && pState->bGesture )
            {
                pState->eGestureMode = GM_ORBIT;
                a5_input_camera_cancel();
                pState->touchCamera.begin(pState->fGestX0, pState->fGestY0, pState->fGestX1, pState->fGestY1);
            }
            break;

        case AMOTION_EVENT_ACTION_MOVE:
            if ( pState->bGesture )
            {
                UpdateGesture( pState, pEvent );
                break;
            }
            if ( pState->bPressPending &&
                 ( hypotf( fX - pState->fPressX, fY - pState->fPressY ) > GestureSlop( pState ) ||
                   pEvent->time - pState->fPressTime > F_PRESS_DELAY ) )
                FlushPendingPress( pState );   /* a drag or a hold, not a nascent gesture */
            {
                float fBackX = 0, fBackY = 0;
                if ( A5D3DWindowToBackBuffer( fX, fY, &fBackX, &fBackY ) )
                    a5_set_pointer_position( (long)fBackX, (long)fBackY );
            }
            break;

        case AMOTION_EVENT_ACTION_POINTER_UP:
        {
            const int nIndex = ( AMotionEvent_getAction( pEvent ) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK )
                               >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            const int nId = AMotionEvent_getPointerId( pEvent, nIndex );
            if ( pState->bGesture && ( nId == pState->nGestureId0 || nId == pState->nGestureId1 ) )
                EndGesture( pState, true );
            else if ( pState->bGesture && pState->eGestureMode == GM_ORBIT )
            {
                pState->eGestureMode = GM_PANZOOM;
                a5_input_camera_cancel();
                pState->touchCamera.begin(pState->fGestX0, pState->fGestY0, pState->fGestX1, pState->fGestY1);
            }
            --pState->nTouchCount;
            break;
        }

        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            EndGesture( pState, nAction == AMOTION_EVENT_ACTION_UP );
            if ( nAction == AMOTION_EVENT_ACTION_CANCEL )
                pState->bPressPending = false;
            /*  A tap while an edit box has focus re-arms the keyboard check:
             *  if the user dismissed the keyboard (Back) and taps the field
             *  again, the game loop shows it again.  When the keyboard is
             *  already up this re-show is a no-op. */
            if ( nAction == AMOTION_EVENT_ACTION_UP && a5_edit_was_active( 400 ) )
                pState->bKeyboardShown = false;
            FlushPendingPress( pState );       /* a quick tap: press and release together */
            if ( pState->bPressSent )
            {
                a5_input_mouse_button( 0, 0 );
                pState->bPressSent = false;
            }
            pState->nTouchCount  = 0;
            pState->bPressInside = false;
            break;

        default:
            break;
    }
    return 1;
}

#endif // A5_HAVE_MAIN

int32_t HandleInput( android_app *pApp, const SQueuedInput *pEvent )
{
    SEngineState *pState = (SEngineState *)pApp->userData;
    if ( pEvent->type == AINPUT_EVENT_TYPE_KEY )
    {
#ifdef A5_HAVE_MAIN
        const int32_t nKeyAction = pEvent->action;
        const int32_t nKey = pEvent->key;
        if ( pState->mode == RUN_GAME && ( nKeyAction == AKEY_EVENT_ACTION_DOWN || nKeyAction == AKEY_EVENT_ACTION_UP ) )
        {
            if ( nKeyAction == AKEY_EVENT_ACTION_DOWN ) g_heldKeys.insert( nKey );
            else g_heldKeys.erase( nKey );
            /* Back is the game's Escape */
            a5_input_key( nKey == AKEYCODE_BACK ? AKEYCODE_ESCAPE : nKey, nKeyAction == AKEY_EVENT_ACTION_DOWN );
            return 1;
        }
#endif
        return 0;
    }
    if ( pEvent->type != AINPUT_EVENT_TYPE_MOTION )
        return 0;

    const int32_t nAction = AMotionEvent_getAction( pEvent ) & AMOTION_EVENT_ACTION_MASK;
    const float   fX      = AMotionEvent_getX( pEvent, 0 );
    const float   fY      = AMotionEvent_getY( pEvent, 0 );

#ifdef A5_HAVE_MAIN
    if ( pState->mode == RUN_GAME )
        return HandleGameTouch( pState, pEvent, nAction );
#endif

    switch ( nAction )
    {
        case AMOTION_EVENT_ACTION_DOWN:
            pState->bTouching   = true;
            pState->fLastTouchY = fY;
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            if ( pState->bTouching )
            {
                pState->console.Scroll( pState->fLastTouchY - fY );
                pState->fLastTouchY = fY;
            }
            return 1;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            pState->bTouching = false;
            return 1;
    }
    return 0;
}

void ResetInput( SEngineState *s )
{
#ifdef A5_HAVE_MAIN
    EndGesture( s, false );
    s->bPressPending = false;
    if ( s->bPressSent ) a5_input_mouse_button( 0, 0 );
    s->bPressSent = s->bPressInside = s->bTouching = false;
    s->nTouchCount = 0;
    for ( int key : g_heldKeys ) a5_input_key( key == AKEYCODE_BACK ? AKEYCODE_ESCAPE : key, 0 );
    g_heldKeys.clear();
#endif
    s->bTouching = false;
}
void DrainInput( SEngineState *s )
{
    std::deque<SQueuedInput> events;
    bool reset;
    {
        std::lock_guard<std::mutex> lock( g_inputMutex );
        events.swap( g_inputEvents );
        reset = g_resetInput;
        g_resetInput = false;
    }
    if ( reset ) ResetInput( s );
    const double now = NowSeconds();
    for ( const SQueuedInput &e : events )
    {
        // Do not replay taps made on a loading screen into the loaded mission.
        if ( now - e.time > 0.5 || !s->bResumed || !s->bFocused ) ResetInput( s );
        else HandleInput( s->pApp, &e );
    }
}

/* ---- input thread: acknowledge promptly, even during world generation ---- */
ALooper *g_pInputLooper = 0;
sem_t g_inputLooperReady;
pthread_t g_inputThread;
std::atomic<bool> g_stopInput( false );
std::mutex g_inputQueueMutex;
AInputQueue *g_inputQueue = 0;

int InputQueueCallback( int, int, void *pData )
{
    android_app *app = static_cast<android_app *>( pData );
    std::lock_guard<std::mutex> lock( g_inputQueueMutex );
    if ( !g_inputQueue ) return 1;
    AInputEvent *event = 0;
    while ( AInputQueue_getEvent( g_inputQueue, &event ) >= 0 )
    {
        if ( AInputQueue_preDispatchEvent( g_inputQueue, event ) ) continue;
        const int handled = QueueInput( app, event );
        AInputQueue_finishEvent( g_inputQueue, event, handled );
    }
    return 1;
}
void *InputThreadMain( void * )
{
    g_pInputLooper = ALooper_prepare( ALOOPER_PREPARE_ALLOW_NON_CALLBACKS );
    ALooper_acquire( g_pInputLooper );
    sem_post( &g_inputLooperReady );
    while ( !g_stopInput.load() ) ALooper_pollOnce( -1, 0, 0, 0 );
    ALooper_release( g_pInputLooper );
    return 0;
}
void StartInputThread()
{
    g_stopInput = false;
    sem_init( &g_inputLooperReady, 0, 0 );
    if ( pthread_create( &g_inputThread, 0, InputThreadMain, 0 ) != 0 )
    {
        LOGE( "input: thread failed to start - using game thread" );
        sem_destroy( &g_inputLooperReady );
        return;
    }
    while ( sem_wait( &g_inputLooperReady ) != 0 && errno == EINTR ) {}
    sem_destroy( &g_inputLooperReady );
    LOGI( "input: worker ready" );
}
void StopInputThread()
{
    if ( !g_pInputLooper ) return;
    {
        std::lock_guard<std::mutex> lock( g_inputQueueMutex );
        if ( g_inputQueue ) AInputQueue_detachLooper( g_inputQueue );
        g_inputQueue = 0;
    }
    g_stopInput = true;
    ALooper_wake( g_pInputLooper );
    pthread_join( g_inputThread, 0 );
    g_pInputLooper = 0;
}

void StartGameIfPossible( SEngineState *pState )
{
#ifdef A5_HAVE_MAIN
    if ( pState->report.nFailed != 0 || !pState->report.bDataMounted )
    {
        LOGI( "game: not starting (checks failed: %d, data mounted: %d) - console stays up",
              pState->report.nFailed, (int)pState->report.bDataMounted );
        return;
    }
    g_pState = pState;
    A5D3DPlatformHooks hooks = { HookWindowWidth, HookWindowHeight, HookPresent, HookSurfaceAlive };
    A5D3DSetPlatformHooks( &hooks );
    /* the game renders through the D3D shim into its own FBO; the console's
     * program state must not leak into it, and vice versa */
    const char *pszError = 0;
    const int nResult = a5_game_init( &pszError );
    if ( nResult != 0 )
    {
        LOGE( "game: init failed (%d): %s", nResult, pszError ? pszError : "?" );
        SBootLine line;
        line.status = BOOT_FAIL;
        line.szText = std::string( "game init failed: " ) + ( pszError ? pszError : "?" );
        line.fSeconds = 0;
        pState->report.lines.push_back( line );
        ++pState->report.nFailed;
        return;
    }
    pState->mode = RUN_GAME;
    pState->bGameRunning = true;
    LOGI( "game: running" );
#else
    (void)pState;
#endif
}

void HandleCommand( android_app *pApp, int32_t nCommand )
{
    SEngineState *pState = (SEngineState *)pApp->userData;

    switch ( nCommand )
    {
        case APP_CMD_INIT_WINDOW:
            if ( pApp->window )
            {
                if ( InitDisplay( pState ) )
                {
                    if ( !pState->bHarnessRun )
                    {
                        /* Run the engine bring-up once, on first window. */
                        pState->report = RunBootHarness(
                            pState->szExternalFilesDir.c_str(),
                            pState->szInternalFilesDir.c_str() );
                        pState->bHarnessRun = true;
                        StartGameIfPossible( pState );
                    }
                    pState->console.SetReport( pState->report );
                    DrawFrame( pState );
                }
            }
            break;

        case APP_CMD_TERM_WINDOW:
            TerminateDisplay( pState );
            break;

        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if ( pState->bSurfaceAlive )
            {
                EGLint nWidth = 0, nHeight = 0;
                eglQuerySurface( pState->display, pState->surface, EGL_WIDTH, &nWidth );
                eglQuerySurface( pState->display, pState->surface, EGL_HEIGHT, &nHeight );
                pState->nWidth  = nWidth;
                pState->nHeight = nHeight;
                pState->console.SetViewport( nWidth, nHeight );
            }
            break;

        case APP_CMD_GAINED_FOCUS:
            pState->bFocused = true;
            pState->frameSchedule.Reset();
            pState->bResetTiming = true;
            break;
        case APP_CMD_LOST_FOCUS:
            ResetInput( pState );
            pState->bFocused = false;
            pState->bFrameReady = false;
            break;

        // The queue has already been switched under glue's lifecycle mutex.
        case APP_CMD_INPUT_CHANGED:
            ResetInput( pState );
            break;

        /* Background: silence the mixer's output stream (nothing advances while
         * we are away, so sounds resume where they were). */
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
            pState->bResumed = false;
            pState->bFrameReady = false;
            pState->frameSchedule.Reset();
            pState->bResetTiming = true;
#ifdef A5_HAVE_AUDIO
            a5_audio_set_active( 0 );
#endif
            break;
        case APP_CMD_RESUME:
            pState->bResumed = true;
            pState->frameSchedule.Reset();
            pState->bResetTiming = true;
#ifdef A5_HAVE_AUDIO
            a5_audio_set_active( 1 );
#endif
            break;
    }
}

}  // namespace

// Called by the staged NDK glue before it wakes the UI thread that owns the
// queue. The old queue cannot be destroyed until this lock has joined any
// in-flight callback; the worker never reads app->inputQueue itself.
extern "C" int a5_android_switch_input_queue( android_app *app, AInputQueue *queue )
{
    if ( !g_pInputLooper ) return 0; // thread startup failed: use glue's fallback
    std::lock_guard<std::mutex> lock( g_inputQueueMutex );
    g_inputQueue = queue;
    if ( queue ) AInputQueue_attachLooper( queue, g_pInputLooper, 0, InputQueueCallback, app );
    return 1;
}

void android_main( android_app *pApp )
{
    /*  Member initialisers rather than memset: this struct holds std::string and
     *  std::vector members, which must not be overwritten with zeroes. */
    SEngineState state;
    state.pApp = pApp;

    pApp->userData     = &state;
    pApp->onAppCmd     = HandleCommand;
    pApp->onInputEvent = QueueInput;
    StartInputThread();

    /*  Fullscreen, like the original game.  Without this the status bar sits on
     *  top of the surface and overlaps the first lines of output. */
    ANativeActivity_setWindowFlags( pApp->activity, AWINDOW_FLAG_FULLSCREEN | AWINDOW_FLAG_KEEP_SCREEN_ON, 0 );

    state.szExternalFilesDir = GetActivityDirectory( pApp, "getExternalFilesDir", true );
    state.szInternalFilesDir = GetActivityDirectory( pApp, "getFilesDir", false );
    LOGI( "external files dir: %s", state.szExternalFilesDir.c_str() );
    /*  Debug switches: <external files>/env.txt, one NAME=VALUE per line, goes
     *  into the environment (A5_D3D_TRACE, A5_DB_DUMP, ...). */
    if ( FILE *pEnv = fopen( ( state.szExternalFilesDir + "/env.txt" ).c_str(), "r" ) )
    {
        char szLine[ 512 ];
        while ( fgets( szLine, sizeof( szLine ), pEnv ) )
        {
            char *pEq = strchr( szLine, '=' );
            if ( !pEq || szLine[ 0 ] == '#' ) continue;
            *pEq = 0;
            char *pVal = pEq + 1;
            pVal[ strcspn( pVal, "\r\n" ) ] = 0;
            setenv( szLine, pVal, 1 );
            LOGI( "env.txt: %s=%s", szLine, pVal );
        }
        fclose( pEnv );
    }
    LOGI( "internal files dir: %s", state.szInternalFilesDir.c_str() );

    state.pChoreographer = AChoreographer_getInstance();
    while ( true )
    {
        if ( CanRender( &state ) && !state.bFramePosted )
        {
            state.bFramePosted = true;
            AChoreographer_postFrameCallback( state.pChoreographer, FrameCallback, &state );
        }
        int                  nEvents;
        android_poll_source *pSource;

        /* Both modes sleep in the looper; only selected vsyncs step the game. */
        const int nTimeout = state.bFrameReady ? 0 : -1;
        while ( ALooper_pollOnce( nTimeout, 0, &nEvents, (void **)&pSource ) >= 0 )
        {
            if ( pSource )
                pSource->process( pApp, pSource );
            if ( pApp->destroyRequested )
            {
                StopInputThread();
#ifdef A5_HAVE_MAIN
                if ( state.bGameRunning )
                    a5_game_shutdown();
#endif
                TerminateDisplay( &state, true );
                return;
            }
            if ( state.bTouching || state.mode == RUN_GAME )
                break;
        }
        DrainInput( &state );
        if ( state.mode == RUN_GAME )
        {
#ifdef A5_HAVE_MAIN
            if ( CanRender( &state ) && state.bFrameReady )
            {
                state.bFrameReady = false;
                /*  A stationary finger sends no MOVE events, so the held-back
                 *  left press (see SEngineState) is aged out here: after the
                 *  delay a press-and-hold reaches the engine as one. */
                if ( state.bPressPending && NowSeconds() - state.fPressTime > F_PRESS_DELAY )
                    FlushPendingPress( &state );
                /*  Soft keyboard follows the focused-edit-box beacon (a CEdit
                 *  with input focus pings it every frame it draws). */
                const bool bWantKeyboard = a5_edit_was_active( 400 ) != 0;
                if ( bWantKeyboard != state.bKeyboardShown )
                {
                    state.bKeyboardShown = bWantKeyboard;
                    ShowSoftKeyboard( pApp, bWantKeyboard );
                }
                static double lastFrame = 0, lastLog = 0;
                static std::vector<double> intervals;
                static double workSum = 0, workMax = 0, swapSum = 0;
                static int samples = 0;
                const double start = NowSeconds();
                if ( state.bResetTiming )
                {
                    state.bResetTiming = false;
                    lastFrame = lastLog = 0;
                    intervals.clear();
                    workSum = workMax = swapSum = 0;
                    samples = 0;
                }
                if ( lastFrame ) intervals.push_back( ( start - lastFrame ) * 1000.0 );
                lastFrame = start;
                if ( !lastLog ) lastLog = start;
                g_fSwapSeconds = 0;
                const int running = a5_game_step( 1 );
                const double work = NowSeconds() - start - g_fSwapSeconds;
                workSum += work;
                workMax = ( workMax > work ? workMax : work );
                swapSum += g_fSwapSeconds;
                ++samples;
                if ( start - lastLog >= 5.0 )
                {
                    std::sort( intervals.begin(), intervals.end() );
                    double total = 0;
                    for ( double ms : intervals ) total += ms;
                    const double p95 = intervals.empty() ? 0 : intervals[( intervals.size() - 1 ) * 95 / 100];
                    const double maxMs = intervals.empty() ? 0 : intervals.back();
                    A5D3DFrameStats st;
                    A5D3DGetFrameStats( &st, 1 );
                    LOGI( "perf: %.1f fps, frame p95 %.2f max %.2f ms; work avg %.2f max %.2f ms, swap avg %.2f ms; %d draws, %d skipped, %d GL errors, %d presents; upload %.2f MiB/frame",
                          total ? intervals.size() * 1000.0 / total : 0, p95, maxMs,
                          workSum * 1000 / samples, workMax * 1000, swapSum * 1000 / samples,
                          st.nDraws, st.nDrawsNoProgram, st.nGLErrors, g_nPresents,
                          (double)st.nBufferUploadBytes / ( samples * 1048576.0 ) );
                    if ( getenv( "A5_D3D_SHADERS" ) ) LOGI( "game: draws by shader: %s", A5D3DDrawsByShader( 1 ) );
#ifdef A5_HAVE_AUDIO
                    a5_audio_log_stats();
#endif
                    intervals.clear();
                    samples = 0;
                    workSum = workMax = swapSum = 0;
                    lastLog = start;
                }
                if ( !running )
                {
                    LOGI( "game: asked to exit" );
                    a5_game_shutdown();
                    state.bGameRunning = false;
                    state.mode = RUN_CONSOLE;
                    state.console.SetReport( state.report );
                }
            }
#endif
        }
        else
            DrawFrame( &state );
    }
}
