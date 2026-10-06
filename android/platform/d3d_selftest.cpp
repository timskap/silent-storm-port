#include "d3d_selftest.h"
#include "d3d9.h"
#include "a5_log.h"

#include <GLES3/gl3.h>
#include <string.h>
#include <stdio.h>
#include <vector>

/* The engine's shader tables (Main/GfxShaders.cpp): the bytecode the device
 * receives at run time.  Compiling every entry from here is the real test of
 * the extract -> translate -> lookup chain. */
struct SVShader;
struct SPShader;
extern SVShader *vsAllShaders[ 77 ];
extern SPShader *psAllShaders[ 78 ];
#include "Main/GfxShadersDescr.h"

namespace {

typedef void ( *ReportFn )( void *, EBootStatus, double, const char * );

struct SReporter
{
    void *p; ReportFn fn;
    void operator()( EBootStatus s, const char *pszFormat, ... )
    {
        char szBuf[ 512 ];
        va_list args;
        va_start( args, pszFormat );
        vsnprintf( szBuf, sizeof( szBuf ), pszFormat, args );
        va_end( args );
        fn( p, s, 0.0, szBuf );
    }
};

int  g_nW = 256, g_nH = 256;
int  HookW() { return g_nW; }
int  HookH() { return g_nH; }
void HookPresent() {}
int  HookAlive() { return 1; }

/* A vertex in the engine's SGeomVecFull layout (32 bytes). */
struct SVec
{
    float x, y, z;
    unsigned char nz, ny, nx, nw;      /* SCompactVector: z,y,x,w */
    short u, v;
    short lu, lv;
    unsigned char t0[ 4 ], t1[ 4 ];
};

}  // namespace

void RunD3DSelfTest( void *pReporter, ReportFn pfnAdd )
{
    SReporter R = { pReporter, pfnAdd };
    R( BOOT_HEADING, "d3d9gles: the renderer's Direct3D 9 on this GPU" );

    A5D3DPlatformHooks hooks = { HookW, HookH, HookPresent, HookAlive };
    A5D3DSetPlatformHooks( &hooks );

    IDirect3D9 *pD3D = Direct3DCreate9( D3D_SDK_VERSION );
    D3DCAPS9 caps;
    pD3D->GetDeviceCaps( 0, D3DDEVTYPE_HAL, &caps );
    R( BOOT_OK, "IDirect3D9 created; caps: vs %d.%d ps %d.%d, %d modes",
       ( caps.VertexShaderVersion >> 8 ) & 0xff, caps.VertexShaderVersion & 0xff,
       ( caps.PixelShaderVersion >> 8 ) & 0xff, caps.PixelShaderVersion & 0xff,
       (int)pD3D->GetAdapterModeCount( 0, D3DFMT_X8R8G8B8 ) );

    D3DPRESENT_PARAMETERS pp;
    memset( &pp, 0, sizeof( pp ) );
    pp.BackBufferWidth = 256; pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *pDev = 0;
    if ( pD3D->CreateDevice( 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &pDev ) != D3D_OK || !pDev )
    {
        R( BOOT_FAIL, "CreateDevice failed" );
        pD3D->Release();
        return;
    }
    R( BOOT_OK, "device with a 256x256 virtual back buffer" );

    /* ---- every shader from the engine's bytecode ------------------------ */
    int nVSok = 0, nPSok = 0;
    std::vector< IDirect3DVertexShader9 * > vs( 77, (IDirect3DVertexShader9 *)0 );
    std::vector< IDirect3DPixelShader9 * > ps( 78, (IDirect3DPixelShader9 *)0 );
    for ( int i = 0; i < 77; ++i )
        if ( pDev->CreateVertexShader( vsAllShaders[ i ]->pShader, &vs[ i ] ) == D3D_OK ) ++nVSok;
    for ( int i = 0; i < 78; ++i )
        if ( pDev->CreatePixelShader( psAllShaders[ i ]->pShader14, &ps[ i ] ) == D3D_OK ) ++nPSok;
    R( nVSok == 77 && nPSok == 78 ? BOOT_OK : BOOT_FAIL,
       "bytecode -> GLSL lookup: %d/77 vertex, %d/78 pixel shaders found", nVSok, nPSok );

    /* Link every pixel shader against vsPureGeometry and every vertex shader
     * against psDiffuse: 155 GL programs compiled by this GPU's compiler.  A
     * shader the driver rejects shows up here, with its log in logcat. */
    IDirect3DVertexDeclaration9 *pDecl = 0;
    D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 16, D3DDECLTYPE_SHORT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 20, D3DDECLTYPE_SHORT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 },
        { 0, 28, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 1 },
        D3DDECL_END()
    };
    pDev->CreateVertexDeclaration( decl, &pDecl );
    pDev->SetVertexDeclaration( pDecl );

    /* one triangle covering the lower-left half of clip space, identity projection in c10..c13 */
    IDirect3DVertexBuffer9 *pVB = 0;
    pDev->CreateVertexBuffer( 3 * sizeof( SVec ), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &pVB, 0 );
    {
        void *p = 0;
        pVB->Lock( 0, 3 * sizeof( SVec ), &p, 0 );
        SVec *v = (SVec *)p;
        memset( v, 0, 3 * sizeof( SVec ) );
        v[ 0 ].x = -1; v[ 0 ].y = -1; v[ 1 ].x = 1; v[ 1 ].y = -1; v[ 2 ].x = -1; v[ 2 ].y = 1;
        for ( int i = 0; i < 3; ++i ) { v[ i ].z = 0.5f; v[ i ].nx = 128; v[ i ].ny = 128; v[ i ].nz = 255; }
        pVB->Unlock();
    }
    IDirect3DIndexBuffer9 *pIB = 0;
    pDev->CreateIndexBuffer( 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &pIB, 0 );
    {
        void *p = 0;
        pIB->Lock( 0, 6, &p, 0 );
        ( (unsigned short *)p )[ 0 ] = 0; ( (unsigned short *)p )[ 1 ] = 1; ( (unsigned short *)p )[ 2 ] = 2;
        pIB->Unlock();
    }
    pDev->SetStreamSource( 0, pVB, 0, sizeof( SVec ) );
    pDev->SetIndices( pIB );
    float identity[ 16 ] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    pDev->SetVertexShaderConstantF( 10, identity, 4 );
    float c16[ 4 ] = { 0.2f, 0.4f, 0.8f, 1.0f };      /* vsConstLight colour */
    pDev->SetVertexShaderConstantF( 16, c16, 1 );
    pDev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    pDev->SetRenderState( D3DRS_ZENABLE, FALSE );

    int nLinked = 0, nTotal = 0;
    for ( int i = 0; i < 78; ++i )
    {
        if ( !ps[ i ] || !vs[ 0 ] ) continue;
        pDev->SetVertexShader( vs[ 0 ] ); pDev->SetPixelShader( ps[ i ] );
        pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0, 1, 0 );
        ++nTotal;
        while ( glGetError() != GL_NO_ERROR ) {}
        const HRESULT draw = pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        if ( draw == D3D_OK && glGetError() == GL_NO_ERROR ) ++nLinked;
    }
    for ( int i = 1; i < 77; ++i )
    {
        if ( !vs[ i ] || !ps[ 0 ] ) continue;
        pDev->SetVertexShader( vs[ i ] ); pDev->SetPixelShader( ps[ 0 ] );
        ++nTotal;
        while ( glGetError() != GL_NO_ERROR ) {}
        const HRESULT draw = pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        if ( draw == D3D_OK && glGetError() == GL_NO_ERROR ) ++nLinked;
    }
    R( nLinked == nTotal ? BOOT_OK : BOOT_FAIL, "%d/%d shader programs compiled and drew without GL errors", nLinked, nTotal );

    /* ---- pixels: vsConstLight + psDiffuse writes c16 into the covered half ---- */
    IDirect3DVertexShader9 *pVSConst = vs[ 13 - 1 ];   /* vsConstLight id 13 */
    pDev->SetVertexShader( pVSConst ); pDev->SetPixelShader( ps[ 0 ] );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1, 0 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );

    IDirect3DSurface9 *pShot = 0;
    pDev->CreateOffscreenPlainSurface( 256, 256, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &pShot, 0 );
    pDev->GetFrontBufferData( 0, pShot );
    D3DLOCKED_RECT lr;
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    /* D3D layout: row 0 = top.  The triangle covers clip-space lower-left ->
     * screen bottom-left (D3D y down).  So (10, 245) is inside, (245, 10) is not. */
    const unsigned char *pIn  = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
    const unsigned char *pOut = (const unsigned char *)lr.pBits + 10 * lr.Pitch + 245 * 4;
    const int bIn  = pIn[ 2 ], gIn = pIn[ 1 ], rIn = pIn[ 2 ];   /* memory b,g,r,a */
    (void)bIn; (void)gIn;
    const bool bInside  = pIn[ 2 ] > 40 && pIn[ 2 ] < 65 && pIn[ 1 ] > 90 && pIn[ 1 ] < 115 && pIn[ 0 ] > 190;
    const bool bOutside = pOut[ 0 ] == 0 && pOut[ 1 ] == 0 && pOut[ 2 ] == 0;
    R( bInside && bOutside ? BOOT_OK : BOOT_FAIL,
       "draw + read back: inside bgra(%d,%d,%d,%d) expect ~(204,102,51), outside bgra(%d,%d,%d) expect 0 -- D3D row order %s",
       pIn[ 0 ], pIn[ 1 ], pIn[ 2 ], pIn[ 3 ], pOut[ 0 ], pOut[ 1 ], pOut[ 2 ], bInside && bOutside ? "confirmed" : "WRONG" );
    (void)rIn;
    pShot->UnlockRect();

    // Program identity follows bytecode, even when COM wrappers are destroyed
    // and the allocator reuses their addresses for a different shader.
    GLint constProgram = 0;
    glGetIntegerv( GL_CURRENT_PROGRAM, &constProgram );
    bool bShaderLifetimeOK = true;
    for ( int pass = 0; pass < 32; ++pass )
    {
        IDirect3DVertexShader9 *temporary = 0;
        const bool colored = ( pass % 2 ) == 0;
        pDev->CreateVertexShader( vsAllShaders[ colored ? 12 : 0 ]->pShader, &temporary );
        pDev->SetVertexShader( temporary );
        temporary->Release();
        pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0, 1, 0 );
        bShaderLifetimeOK &= pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 ) == D3D_OK;
        GLint program = 0;
        glGetIntegerv( GL_CURRENT_PROGRAM, &program );
        bShaderLifetimeOK &= colored ? program == constProgram : program != constProgram;
        pDev->GetFrontBufferData( 0, pShot );
        pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
        const unsigned char *pixel = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
        bShaderLifetimeOK &= colored ? pixel[ 0 ] > 190 && pixel[ 1 ] > 90 && pixel[ 2 ] > 40
                                    : pixel[ 0 ] == 0 && pixel[ 1 ] == 0 && pixel[ 2 ] == 0;
        pShot->UnlockRect();
        pDev->SetVertexShader( 0 );
    }
    R( bShaderLifetimeOK && glGetError() == GL_NO_ERROR ? BOOT_OK : BOOT_FAIL,
       "shader program cache survives wrapper recreation and reuses identical bytecode" );
    pDev->SetVertexShader( pVSConst );

    // Append to vertex/index pools while their first range is in flight.
    // Verify both the pending draw and the old range on a subsequent draw.
    IDirect3DVertexBuffer9 *pStreamVB = 0;
    IDirect3DIndexBuffer9 *pStreamIB = 0;
    const UINT streamPoolBytes = 16 * 1024 * 1024;
    pDev->CreateVertexBuffer( streamPoolBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                             0, D3DPOOL_DEFAULT, &pStreamVB, 0 );
    pDev->CreateIndexBuffer( 12, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &pStreamIB, 0 );
    pDev->SetStreamSource( 0, pStreamVB, 0, sizeof( SVec ) );
    pDev->SetIndices( pStreamIB );
    pDev->SetVertexShader( pVSConst );
    pDev->SetPixelShader( ps[ 0 ] );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
    bool bStreamingOK = true;
    const float red[ 4 ] = { 1, 0, 0, 1 };
    for ( int half = 0; half < 2; ++half )
    {
        void *p = 0;
        pStreamVB->Lock( half * 3 * sizeof( SVec ), 3 * sizeof( SVec ), &p,
                         half ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD );
        SVec *v = (SVec *)p;
        memset( v, 0, 3 * sizeof( SVec ) );
        v[ 0 ].x = v[ 2 ].x = -0.9f + half;
        v[ 1 ].x = -0.1f + half;
        v[ 0 ].y = v[ 1 ].y = -0.9f;
        v[ 2 ].y = -0.1f;
        pStreamVB->Unlock();
        pStreamIB->Lock( half * 6, 6, &p, half ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD );
        for ( int i = 0; i < 3; ++i ) ( (unsigned short *)p )[ i ] = half * 3 + i;
        pStreamIB->Unlock();
        pDev->SetVertexShaderConstantF( 16, half ? red : c16, 1 );
        bStreamingOK &= pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 6, half * 3, 1 ) == D3D_OK;
    }
    for ( int pass = 0; pass < 2; ++pass )
    {
        if ( pass )
        {
            // Real scenes stream small updates into a multi-megabyte pool.
            // Exercise distant ranges while repeatedly reading cached data.
            pDev->SetVertexShaderConstantF( 16, c16, 1 );
            for ( UINT batch = 1; batch <= 32; ++batch )
            {
                void *p = 0;
                pStreamVB->Lock( batch * 65536, 512, &p, D3DLOCK_NOOVERWRITE );
                memset( p, 0, 512 );
                pStreamVB->Unlock();
                pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 6, 0, 1 );
            }
            pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
            for ( int half = 0; half < 2; ++half )
            {
                pDev->SetVertexShaderConstantF( 16, half ? red : c16, 1 );
                bStreamingOK &= pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 6, half * 3, 1 ) == D3D_OK;
            }
        }
        pDev->GetFrontBufferData( 0, pShot );
        pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
        const unsigned char *left = (const unsigned char *)lr.pBits + 230 * lr.Pitch + 25 * 4;
        const unsigned char *right = left + 128 * 4;
        bStreamingOK &= left[ 0 ] > 190 && left[ 1 ] > 90 && left[ 1 ] < 115 && left[ 2 ] > 40 && left[ 2 ] < 65;
        bStreamingOK &= right[ 0 ] < 15 && right[ 1 ] < 15 && right[ 2 ] > 240;
        pShot->UnlockRect();
    }
    void *pWait = 0;
    bStreamingOK &= pStreamVB->Lock( 0, 0, &pWait, 0 ) == D3D_OK;
    bStreamingOK &= pStreamVB->Unlock() == D3D_OK;
    bStreamingOK &= pStreamVB->Lock( streamPoolBytes, 1, &pWait, 0 ) == D3DERR_INVALIDCALL;
    bStreamingOK &= glGetError() == GL_NO_ERROR;
    R( bStreamingOK ? BOOT_OK : BOOT_FAIL, "DISCARD / NOOVERWRITE: vertex and index appends preserve in-flight draws and stored ranges" );

    // The engine also reuses ranges with NOOVERWRITE before the previous
    // draw retires. Ordered uploads must preserve that prior draw.
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
    pDev->SetVertexShaderConstantF( 16, c16, 1 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 6, 0, 1 );
    pStreamVB->Lock( 0, 3 * sizeof( SVec ), &pWait, D3DLOCK_NOOVERWRITE );
    for ( int i = 0; i < 3; ++i ) ( (SVec *)pWait )[ i ].x += 1;
    pStreamVB->Unlock();
    pStreamIB->Lock( 0, 6, &pWait, D3DLOCK_NOOVERWRITE );
    for ( int i = 0; i < 3; ++i ) ( (unsigned short *)pWait )[ i ] = i;
    pStreamIB->Unlock();
    pDev->SetVertexShaderConstantF( 16, red, 1 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 6, 0, 1 );
    pDev->GetFrontBufferData( 0, pShot );
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    {
        const unsigned char *left = (const unsigned char *)lr.pBits + 230 * lr.Pitch + 25 * 4;
        const unsigned char *right = left + 128 * 4;
        R( left[ 0 ] > 190 && left[ 1 ] > 90 && left[ 2 ] > 40 && left[ 2 ] < 65 &&
           right[ 0 ] < 15 && right[ 1 ] < 15 && right[ 2 ] > 240 && glGetError() == GL_NO_ERROR ? BOOT_OK : BOOT_FAIL,
           "overlapping NOOVERWRITE preserves the prior draw when reusing its data" );
    }
    pShot->UnlockRect();
    // Real batches reference distant allocations, with an offset stream and
    // sometimes a negative base vertex. Check pixels across Present: unchanged
    // geometry, changed vertices, then changed indices in the same draw slot.
    bool bSparseFramesOK = true;
    for ( int wide = 0; wide < 2; ++wide )
    {
        const UINT indices[2][3] = { { 2, 100, wide ? 70000u : 32768u },
                                    { 4, 102, wide ? 70002u : 32770u } };
        IDirect3DIndexBuffer9 *pSparseIB = 0;
        pDev->CreateIndexBuffer( 3 * ( wide ? 4 : 2 ), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                wide ? D3DFMT_INDEX32 : D3DFMT_INDEX16, D3DPOOL_DEFAULT, &pSparseIB, 0 );
        pDev->SetStreamSource( 0, pStreamVB, 2 * sizeof( SVec ), sizeof( SVec ) );
        pDev->SetIndices( pSparseIB );
        pDev->SetVertexShaderConstantF( 16, c16, 1 );
        for ( int frame = 0; frame < 4; ++frame )
        {
            bSparseFramesOK &= pDev->Present( 0, 0, 0, 0 ) == D3D_OK;
            if ( frame == 0 || frame == 2 )
            {
                for ( int half = 0; half < 2; ++half )
                    for ( int vertex = 0; vertex < 3; ++vertex )
                    {
                        void *p = 0;
                        // Offset of two vertices cancels BaseVertexIndex=-2.
                        pStreamVB->Lock( indices[half][vertex] * sizeof( SVec ), sizeof( SVec ), &p, 0 );
                        SVec v = {};
                        v.x = ( vertex == 1 ? -0.1f : -0.9f ) + ( frame == 2 && half == 0 ? 1 : 0 );
                        v.y = vertex == 2 ? -0.1f : -0.9f;
                        memcpy( p, &v, sizeof( v ) );
                        pStreamVB->Unlock();
                    }
            }
            if ( frame == 0 || frame == 3 )
            {
                void *p = 0;
                pSparseIB->Lock( 0, 0, &p, D3DLOCK_DISCARD );
                for ( int i = 0; i < 3; ++i )
                    if ( wide ) ((uint32_t *)p)[i] = indices[frame == 3][i];
                    else ((uint16_t *)p)[i] = indices[frame == 3][i];
                pSparseIB->Unlock();
            }
            pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
            bSparseFramesOK &= pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, -2, 2,
                                                          indices[1][2], 0, 1 ) == D3D_OK;
            pDev->GetFrontBufferData( 0, pShot );
            pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
            const unsigned char *left = (const unsigned char *)lr.pBits + 230 * lr.Pitch + 25 * 4;
            const unsigned char *right = left + 128 * 4;
            const unsigned char *lit = frame == 2 ? right : left;
            const unsigned char *dark = frame == 2 ? left : right;
            bSparseFramesOK &= lit[0] > 190 && lit[1] > 90 && lit[2] > 40 && lit[2] < 65;
            bSparseFramesOK &= dark[0] < 15 && dark[1] < 15 && dark[2] < 15;
            pShot->UnlockRect();
        }
        pSparseIB->Release();
    }
    R( bSparseFramesOK && glGetError() == GL_NO_ERROR ? BOOT_OK : BOOT_FAIL,
       "sparse 16/32-bit indices and offset/base vertices survive frame reuse and geometry changes" );
    pDev->SetStreamSource( 0, pVB, 0, sizeof( SVec ) );
    pDev->SetIndices( pIB );
    pDev->SetVertexShaderConstantF( 16, c16, 1 );
    pStreamVB->Release();
    pStreamIB->Release();

    /* ---- render target texture: draw into it, sample it back through psTextureCopyAlpha ---- */
    IDirect3DTexture9 *pRTTex = 0;
    pDev->CreateTexture( 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pRTTex, 0 );
    IDirect3DSurface9 *pRTSurf = 0, *pBack = 0, *pBackZ = 0, *pRTZ = 0;
    pRTTex->GetSurfaceLevel( 0, &pRTSurf );
    pDev->GetRenderTarget( 0, &pBack );
    pDev->GetDepthStencilSurface( &pBackZ );
    pDev->CreateDepthStencilSurface( 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &pRTZ, 0 );
    pDev->SetRenderTarget( 0, pRTSurf );
    pDev->SetDepthStencilSurface( pRTZ );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF00FF00, 1, 0 );   /* green */
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );   /* c16 colour, lower-left */
    pDev->SetRenderTarget( 0, pBack );
    pDev->SetDepthStencilSurface( pBackZ );
    D3DLOCKED_RECT rl;
    if ( pRTSurf->LockRect( &rl, 0, D3DLOCK_READONLY ) == D3D_OK )
    {
        const unsigned char *pTop = (const unsigned char *)rl.pBits + 2 * rl.Pitch + 60 * 4;      /* top-right: green */
        const unsigned char *pBot = (const unsigned char *)rl.pBits + 61 * rl.Pitch + 2 * 4;      /* bottom-left: c16 */
        const bool bOK = pTop[ 1 ] > 200 && pTop[ 2 ] < 30 && pBot[ 0 ] > 190 && pBot[ 2 ] < 70;
        R( bOK ? BOOT_OK : BOOT_FAIL, "render-to-texture + LockRect: top-right bgr(%d,%d,%d) green, bottom-left bgr(%d,%d,%d) c16",
           pTop[ 0 ], pTop[ 1 ], pTop[ 2 ], pBot[ 0 ], pBot[ 1 ], pBot[ 2 ] );
        pRTSurf->UnlockRect();
    }
    else
        R( BOOT_FAIL, "render-target LockRect failed" );

    /* ---- managed texture upload + sampling ---- */
    IDirect3DTexture9 *pTex = 0;
    pDev->CreateTexture( 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTex, 0 );
    {
        D3DLOCKED_RECT tl;
        pTex->LockRect( 0, &tl, 0, 0 );
        for ( int y = 0; y < 4; ++y )
            for ( int x = 0; x < 4; ++x )
            {
                unsigned char *p = (unsigned char *)tl.pBits + y * tl.Pitch + x * 4;
                p[ 0 ] = 255; p[ 1 ] = 128; p[ 2 ] = 0; p[ 3 ] = 255;    /* b,g,r,a = pure blue-ish (0,128,255) */
            }
        pTex->UnlockRect( 0 );
    }
    pDev->SetTexture( 0, pTex );
    pDev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    pDev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    pDev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
    pDev->SetVertexShader( vs[ 4 - 1 ] );        /* vsTexture (id 4): oT0 = tex * c6.x */
    pDev->SetPixelShader( ps[ 3 - 1 ] );         /* psTextureCopyAlpha (id 3): r0 = t0 */
    float c6[ 4 ] = { 1.0f / 2048, 1.0f / 65536, 0.5f, 0 };
    pDev->SetVertexShaderConstantF( 6, c6, 1 );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
    pDev->GetFrontBufferData( 0, pShot );
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    {
        const unsigned char *p = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
        const bool bOK = p[ 0 ] > 240 && p[ 1 ] > 118 && p[ 1 ] < 138 && p[ 2 ] < 15;
        R( bOK ? BOOT_OK : BOOT_FAIL, "managed texture upload + vsTexture/psTextureCopyAlpha: bgr(%d,%d,%d) expect (255,128,0)",
           p[ 0 ], p[ 1 ], p[ 2 ] );
    }
    pShot->UnlockRect();

    // Shadow subtraction must retain sub-mediump depth differences before
    // amplification. The real psShadowTest reads the texture's green channel.
    bool shadowPrecisionOK = true;
    pDev->SetVertexShader(pVSConst);
    pDev->SetPixelShader(ps[22]);
    const float shadowScale[4] = {0, 8192, 0, 0};
    pDev->SetPixelShaderConstantF(0, shadowScale, 1);
    for (int sample = 0; sample < 6; ++sample) {
        const float delta = (sample & 1) ? -0.00006f : 0.00006f;
        const float depth[4] = {0, 128.0f/255.0f - delta, 0, 0};
        pDev->SetVertexShaderConstantF(16, depth, 1);
        pDev->Clear(0, 0, D3DCLEAR_TARGET, 0, 1, 0);
        pDev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
        pDev->GetFrontBufferData(0, pShot);
        pShot->LockRect(&lr, 0, D3DLOCK_READONLY);
        const unsigned char *pixel = (const unsigned char *)lr.pBits + 245*lr.Pitch + 10*4;
        shadowPrecisionOK &= (sample & 1) ? pixel[1] < 3 : (pixel[1] >= 120 && pixel[1] <= 131);
        pShot->UnlockRect();
    }
    R(shadowPrecisionOK ? BOOT_OK : BOOT_FAIL, "shadow shader retains nearby depth differences (0.00006) without flickering thresholds");

    // The depth prepass and lighting use different vertex programs. Compare
    // EQUAL-tested lighting against the same draw without a depth test over
    // several nontrivial transforms, including every covered pixel.
    bool bEqualDepthOK = true;
    for ( int pass = 0; pass < 8; ++pass )
    {
        float transform[16] = { 0.81371f,0.08113f,0,0.01137f * pass,
                               -0.06317f,0.83131f,0,0.00573f * pass,
                                0.01913f,0.03731f,0.61793f,0.07119f,
                                0.01037f,0.02317f,0,1 };
        pDev->SetVertexShaderConstantF( 10, transform, 4 );
        pDev->SetVertexShaderConstantF( 16, c16, 1 );
        pDev->SetRenderState( D3DRS_ZENABLE, TRUE );
        pDev->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
        pDev->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );
        pDev->Clear( 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1, 0 );
        pDev->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );
        pDev->SetVertexShader( vs[3] );
        pDev->SetPixelShader( ps[2] );
        pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        pDev->SetRenderState( D3DRS_COLORWRITEENABLE, 15 );
        pDev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
        pDev->SetRenderState( D3DRS_ZFUNC, D3DCMP_EQUAL );
        pDev->SetVertexShader( pVSConst );
        pDev->SetPixelShader( ps[0] );
        pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        pDev->GetFrontBufferData( 0, pShot );
        pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
        std::vector<unsigned char> equalPixels( 256 * 256 * 4 );
        for ( int y = 0; y < 256; ++y )
            memcpy( &equalPixels[y * 256 * 4], (const unsigned char *)lr.pBits + y * lr.Pitch, 256 * 4 );
        pShot->UnlockRect();
        pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0, 1, 0 );
        pDev->SetRenderState( D3DRS_ZENABLE, FALSE );
        pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        pDev->GetFrontBufferData( 0, pShot );
        pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
        int covered = 0;
        for ( int y = 0; y < 256; ++y )
        {
            const unsigned char *row = (const unsigned char *)lr.pBits + y * lr.Pitch;
            bEqualDepthOK &= memcmp( &equalPixels[y * 256 * 4], row, 256 * 4 ) == 0;
            for ( int x = 0; x < 256; ++x ) covered += row[x * 4] > 190;
        }
        bEqualDepthOK &= covered > 10000;
        pShot->UnlockRect();
    }
    R( bEqualDepthOK && glGetError() == GL_NO_ERROR ? BOOT_OK : BOOT_FAIL,
       "multipass EQUAL depth preserves every covered pixel across vertex programs" );
    pDev->SetVertexShaderConstantF( 10, identity, 4 );
    pDev->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
    pDev->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );

    // A missing shader must not reuse the previous draw's valid GL program.
    // Exercise both entry points and verify that the clear colour survives.
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFFFF0000, 1, 0 );
    pDev->SetPixelShader( 0 );
    const HRESULT missingIndexed = pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
    const HRESULT missingArrays = pDev->DrawPrimitive( D3DPT_TRIANGLELIST, 0, 1 );
    pDev->GetFrontBufferData( 0, pShot );
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    {
        const unsigned char *p = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
        R( missingIndexed == D3DERR_INVALIDCALL && missingArrays == D3DERR_INVALIDCALL &&
           p[0] == 0 && p[1] == 0 && p[2] == 255 ? BOOT_OK : BOOT_FAIL,
           "missing shader skips both draws and preserves target pixels" );
    }
    pShot->UnlockRect();

    /* ---- teardown ---- */
    pTex->Release(); pRTSurf->Release(); pRTTex->Release(); pRTZ->Release(); pBack->Release(); pBackZ->Release();
    pShot->Release(); pVB->Release(); pIB->Release(); pDecl->Release();
    for ( size_t i = 0; i < vs.size(); ++i ) if ( vs[ i ] ) vs[ i ]->Release();
    for ( size_t i = 0; i < ps.size(); ++i ) if ( ps[ i ] ) ps[ i ]->Release();
    pDev->Release();
    pD3D->Release();
    R( BOOT_OK, "device torn down cleanly" );
}
