#include "Main/StdAfx.h"
#include "Main/aiObject.h"
#include "Main/BSPtree.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "BSP check failed: %s\n", #x); return 1; } } while (0)

int main()
{
    vector<CVec3> points = {
        CVec3(-1,-1,-1), CVec3(1,-1,-1), CVec3(1,1,-1), CVec3(-1,1,-1),
        CVec3(-1,-1,1), CVec3(1,-1,1), CVec3(1,1,1), CVec3(-1,1,1)
    };
    vector<STriangle> tris = {
        STriangle(0,2,1), STriangle(0,3,2), STriangle(4,5,6), STriangle(4,6,7),
        STriangle(0,1,5), STriangle(0,5,4), STriangle(1,2,6), STriangle(1,6,5),
        STriangle(2,3,7), STriangle(2,7,6), STriangle(3,0,4), STriangle(3,4,7)
    };
    CObj<NAI::CGeometryInfo> geometry = new NAI::CGeometryInfo;
    geometry->AddPiece(0, points, tris, 8);
    geometry->CalcMissingBSPTrees();
    CHECK(geometry->pieces[0].trees.size() == 1);
    CObj<NAI::CBSPTree> original = geometry->pieces[0].trees[0];
    CHECK(IsValid(original));
    CHECK(original->DoesIntersect(CVec3(0,0,0), 0.1f));
    CHECK(!original->DoesIntersect(CVec3(3,0,0), 0.1f));
    CHECK(original->CollideCheckNoImpact(CVec3(-3,0,0), CVec3(3,0,0), 0.1f));
    CHECK(!original->CollideCheckNoImpact(CVec3(-3,3,0), CVec3(3,3,0), 0.1f));
    geometry->CalcMissingBSPTrees();
    CHECK(geometry->pieces[0].trees[0].GetPtr() == original.GetPtr());
    // A stored tree is retained; a new geometry revision gets a new tree.
    geometry->AddPiece(1, points, tris, 8, vector<NAI::SJunction>(), true, geometry->pieces[0].trees);
    geometry->CalcMissingBSPTrees();
    CHECK(geometry->pieces[1].trees[0].GetPtr() == original.GetPtr());
    for (CVec3 &p : points) p.x += 10;
    geometry->AddPiece(0, points, tris, 8);
    geometry->CalcMissingBSPTrees();
    CHECK(geometry->pieces[0].trees[0].GetPtr() != original.GetPtr());
    CHECK(!geometry->pieces[0].trees[0]->DoesIntersect(CVec3(0,0,0), 0.1f));
    CHECK(geometry->pieces[0].trees[0]->DoesIntersect(CVec3(10,0,0), 0.1f));
    puts("BSP cache: collision, reuse, stored trees and geometry replacement passed");
}
