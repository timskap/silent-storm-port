/*
 *  LifeStudioHeadAPI.h -- stub for the LifeStudio:HEAD facial-animation SDK.
 *
 *  Silent Storm used LifeStudio:HEAD (Lifemode Interactive) to animate the
 *  talking heads in dialogue close-ups: an IAnimator per head mesh, an
 *  ISequencer per phoneme/expression sequence, and a shared IMMTree of macro
 *  muscles loaded from tree.mma. The bundled SDK binary targets Windows x86
 *  and cannot be linked into the Android port.
 *
 *  This compatibility layer decodes neutral vertex positions from the two
 *  animator stream formats in the game's Heads/ assets. Process() fills the
 *  caller's mesh, including vertices outside the muscle groups. Facial muscle
 *  deformation and sequenced speech are not implemented.
 */
#ifndef A5_STUB_LIFESTUDIOHEADAPI_H
#define A5_STUB_LIFESTUDIOHEADAPI_H

#include <stddef.h>

namespace LifeStudioHeadAPI
{

class IMacroMuscle
{
public:
    virtual ~IMacroMuscle() {}
};

class IMMTree
{
public:
    static IMMTree *Create();
    virtual void Destroy();
    virtual bool Load( const char *pszFileName );
    virtual IMacroMuscle *RootMacroMuscle();
    virtual ~IMMTree() {}
};

class IAnimator
{
public:
    static IAnimator *Create();
    virtual void Destroy();
    /* Loads an animator stream (the game keeps them inside Heads/ assets). */
    virtual bool Load( const char *pData, int nSize );
    virtual void RegisterMacroMuscle( IMacroMuscle *pMuscle );
    virtual void ClearAllMacroMuscles();
    virtual void ComputePhysics();
    virtual void FillUnused( bool bFill );
    /* Writes the deformed vertex positions into pVertices, nStride floats
     * apart. nCapacity must match the stream vertex count. */
    virtual void Process( float *pVertices, int nStride, int nCapacity );
    virtual ~IAnimator() {}
};

class ISequencer
{
public:
    static ISequencer *Create();
    virtual void Destroy();
    virtual bool Load( const char *pData, int nSize );
    virtual void RegisterMMTree( IMMTree *pTree );
    virtual void RenderMacroMuscles( IAnimator *pAnimator, int nTimeMs );
    virtual int  SequenceTime();
    virtual ~ISequencer() {}
};

}  // namespace LifeStudioHeadAPI

#endif
