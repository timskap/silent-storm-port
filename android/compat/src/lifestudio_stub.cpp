/*  lifestudio_stub.cpp -- see compat/include/thirdparty-stubs/LifeStudioHeadAPI.h */
#include "LifeStudioHeadAPI.h"
#include "a5_log.h"
#include "head_neutral.h"

namespace LifeStudioHeadAPI
{

namespace
{
struct SStubTree : IMMTree
{
    IMacroMuscle root;
};
struct SStubAnimator : IAnimator { std::vector<A5Head::Point> vertices; };
struct SStubSequencer : ISequencer {};

bool g_bWarned = false;
void WarnOnce()
{
    if ( g_bWarned )
        return;
    g_bWarned = true;
    a5_log( A5_PRIORITY_WARN,
            "LifeStudio:HEAD uses decoded neutral geometry; dialogue heads will not animate (see "
            "compat/include/thirdparty-stubs/LifeStudioHeadAPI.h)" );
}
}  // namespace

IMMTree      *IMMTree::Create()                          { WarnOnce(); return new SStubTree; }
void          IMMTree::Destroy()                         { delete this; }
bool          IMMTree::Load( const char * )              { return true; }
IMacroMuscle *IMMTree::RootMacroMuscle()                 { return &static_cast< SStubTree * >( this )->root; }

IAnimator *IAnimator::Create()                           { WarnOnce(); return new SStubAnimator; }
void       IAnimator::Destroy()                          { delete this; }
bool IAnimator::Load( const char *data, int size )
{
    std::vector<A5Head::Point> &vertices = static_cast<SStubAnimator *>(this)->vertices;
    vertices.clear();
    const bool ok = size > 0 && A5Head::Load(data, size, vertices);
    if (!ok) a5_log(A5_PRIORITY_WARN, "LifeStudio: invalid or unsupported animator stream (%d bytes)", size);
    return ok;
}
void       IAnimator::RegisterMacroMuscle( IMacroMuscle * ) {}
void       IAnimator::ClearAllMacroMuscles()             {}
void       IAnimator::ComputePhysics()                   {}
void       IAnimator::FillUnused( bool )                 {}
void IAnimator::Process( float *out, int stride, int capacity )
{
    const std::vector<A5Head::Point> &vertices = static_cast<SStubAnimator *>(this)->vertices;
    if (!out || stride < 3 || capacity < 0 || vertices.size() != size_t(capacity)) return;
    for (size_t i = 0; i < vertices.size(); ++i) {
        out[i * stride] = vertices[i].x;
        out[i * stride + 1] = vertices[i].y;
        out[i * stride + 2] = vertices[i].z;
    }
}

ISequencer *ISequencer::Create()                         { WarnOnce(); return new SStubSequencer; }
void        ISequencer::Destroy()                        { delete this; }
bool        ISequencer::Load( const char *, int )        { return true; }
void        ISequencer::RegisterMMTree( IMMTree * )      {}
void        ISequencer::RenderMacroMuscles( IAnimator *, int ) {}
int         ISequencer::SequenceTime()                   { return 0; }

}  // namespace LifeStudioHeadAPI
