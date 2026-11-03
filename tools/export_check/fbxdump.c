// Prints every FBX corner (position, UV) and the materials/textures ufbx finds, loading in strict mode.
// Used by tools/export_check/run.sh.
#include "ufbx.h"
#include <stdio.h>
int main(int argc,char**argv){
  ufbx_load_opts o={0}; ufbx_error e; o.strict = 1;
  o.target_axes = ufbx_axes_right_handed_y_up; o.target_unit_meters=0;
  ufbx_scene*s=ufbx_load_file(argv[1],&o,&e);
  if(!s){char b[512];ufbx_format_error(b,512,&e);fprintf(stderr,"%s\n",b);return 1;}
  fprintf(stderr,"creator=%s version=%u exporter=%s\n", s->metadata.creator.data, s->metadata.version, s->metadata.original_application.name.data);
  for(size_t i=0;i<s->metadata.warnings.count;i++) fprintf(stderr,"WARNING %s (%zu)\n",s->metadata.warnings.data[i].description.data,s->metadata.warnings.data[i].count);
  for(size_t mi=0;mi<s->meshes.count;mi++){ufbx_mesh*m=s->meshes.data[mi];
    fprintf(stderr,"mesh %s faces=%zu uvsets=%zu mats=%zu instances=%zu\n",m->name.data,m->num_faces,m->uv_sets.count,m->materials.count,m->instances.count);
    for(size_t k=0;k<m->materials.count;k++){ufbx_material*mat=m->materials.data[k];
      fprintf(stderr,"  material %s diffuse_tex=%s pbr_base_tex=%s\n",mat->name.data,
        mat->fbx.diffuse_color.texture?mat->fbx.diffuse_color.texture->relative_filename.data:"-",
        mat->pbr.base_color.texture?mat->pbr.base_color.texture->relative_filename.data:"-");
      if(mat->fbx.diffuse_color.texture){ufbx_texture*t=mat->fbx.diffuse_color.texture; fprintf(stderr,"   tex filename=%s abs=%s uvset=%s wrap=%d,%d\n",t->filename.data,t->absolute_filename.data,t->uv_set.data,t->wrap_u,t->wrap_v); fprintf(stderr,"   video=%s content=%zu\n", t->video? t->video->relative_filename.data:"NONE", t->content.size);}
    }
    if(!m->vertex_uv.exists) continue;
    ufbx_node*n=m->instances.count?m->instances.data[0]:0;
    for(size_t f=0;f<m->num_faces;f++){ufbx_face fc=m->faces.data[f];
      for(uint32_t c=0;c<fc.num_indices;c++){size_t ix=fc.index_begin+c;
        ufbx_vec3 p=ufbx_get_vertex_vec3(&m->vertex_position,ix); if(n) p=ufbx_transform_position(&n->geometry_to_world,p);
        ufbx_vec2 uv=ufbx_get_vertex_vec2(&m->vertex_uv,ix);
        printf("%zu %.6f %.6f %.6f %.6f %.6f\n",mi,p.x,p.y,p.z,uv.x,uv.y);}}
  }
  ufbx_free_scene(s);return 0;}
