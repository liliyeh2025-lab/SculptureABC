# Runs OpenUSD validators on a .usda/.usdz and prints its meshes and shaders. (tools/export_check/run.sh)
import sys
from pxr import UsdUtils, Usd, UsdShade, UsdGeom
f=sys.argv[1]
from pxr import UsdValidation
st=Usd.Stage.Open(f)
reg=UsdValidation.ValidationRegistry()
vals=[v for v in reg.GetOrLoadAllValidators() if not v.GetMetadata().name.startswith('usdPhysics')]
ctx=UsdValidation.ValidationContext(vals)
errs=ctx.Validate(st)
print('validation issues:', len(errs))
for e in errs: print('  ', e.GetValidator().GetMetadata().name if e.GetValidator() else '?', e.GetType(), e.GetMessage()[:300])
for p in st.Traverse():
    if p.IsA(UsdGeom.Mesh):
        m=UsdGeom.Mesh(p); pv=UsdGeom.PrimvarsAPI(p)
        print(p.GetPath(), 'pts',len(m.GetPointsAttr().Get()),'faces',len(m.GetFaceVertexCountsAttr().Get()), 'primvars',[ (x.GetName(),x.GetInterpolation(), str(x.GetTypeName())) for x in pv.GetPrimvars()])
        mat,_=UsdShade.MaterialBindingAPI(p).ComputeBoundMaterial(); print('  material', mat.GetPath() if mat else None)
    if p.IsA(UsdShade.Shader):
        s=UsdShade.Shader(p); print(' shader',p.GetPath(), s.GetIdAttr().Get(), {i.GetBaseName(): (i.Get() if not i.HasConnectedSource() else 'conn') for i in s.GetInputs()})
