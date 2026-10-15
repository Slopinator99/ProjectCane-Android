// CI check: compile + link every game shader program in a real OpenGL ES 3.2
// context, using exactly the desktop->ES rewrite from glshaders.cpp
// (keep ToEs() in sync with ToGlslEs()). Run headless with SDL's offscreen
// video driver + Mesa (llvmpipe). Exit code = number of failing programs.
#include <SDL.h>
#include <GLES3/gl32.h>
#include <cstdio>
#include <string>
#include "embedded_shaders.h"
static std::string ToEs(std::string src){auto v=src.find("#version");auto e=src.find('\n',v);src.erase(v,e-v+1);
 return std::string("#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler2DArray;\nprecision highp usampler2D;\nprecision highp isampler2D;\n")+src;}
static GLuint comp(GLenum t,const std::string&s,const char*n){GLuint h=glCreateShader(t);const char*p=s.c_str();glShaderSource(h,1,&p,0);glCompileShader(h);GLint ok;glGetShaderiv(h,GL_COMPILE_STATUS,&ok);if(!ok){char l[4096];glGetShaderInfoLog(h,4096,0,l);printf("COMPILE FAIL %s:\n%s\n",n,l);}return ok?h:0;}
int main(){
 SDL_SetHint(SDL_HINT_VIDEODRIVER,"offscreen");SDL_Init(SDL_INIT_VIDEO);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);
 SDL_Window*w=SDL_CreateWindow("t",0,0,64,64,SDL_WINDOW_OPENGL);if(!w){printf("no window: %s\n",SDL_GetError());return 2;}
 SDL_GLContext c=SDL_GL_CreateContext(w);if(!c){printf("no ctx: %s\n",SDL_GetError());return 2;}
 printf("%s | %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
 GLint a,b,d,e;glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS,&a);glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS,&b);glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS,&d);glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS,&e);
 printf("vtxSSBO=%d combSSBO=%d bindings=%d vtxOut=%d\n",a,b,d,e);
 const char*names[]={"screen","dysh","glob","celborder","geom","blot","blip"};int bad=0;
 for(auto n:names){std::string v=std::string(n)+".vert",f=std::string(n)+".frag";
  GLuint vs=comp(GL_VERTEX_SHADER,ToEs(CaneEmbeddedShader(v)),v.c_str()),fs=comp(GL_FRAGMENT_SHADER,ToEs(CaneEmbeddedShader(f)),f.c_str());
  if(!vs||!fs){bad++;continue;}GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);
  if(!ok){char l[4096];glGetProgramInfoLog(p,4096,0,l);printf("LINK FAIL %s:\n%s\n",n,l);bad++;}else printf("OK  %s\n",n);}
 return bad;}
