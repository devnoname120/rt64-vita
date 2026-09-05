#include "rt64_fast.h"
#include "hle/rt64_vi.h"
#include "shared/rt64_blender.h"
#ifdef RT64_FAST_VITAGL
#include <psp2/gxm.h>
#include <vitaGL.h>
#include <vitashark.h>
#if defined(RT64_FAST_VALIDATE_UPLOADS)
#include <psp2/kernel/clib.h>
#endif
#else
#include <GLES2/gl2.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace RT64 {
namespace {
    GLuint compile(GLenum type, const std::string &source) {
        GLuint shader = glCreateShader(type);
        const char *text = source.c_str();
        glShaderSource(shader, 1, &text, nullptr);
        glCompileShader(shader);
        GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            glDeleteShader(shader);
            throw std::runtime_error(std::string("RT64 Fast shader compilation failed: ") + log + "\n" + source);
        }
        return shader;
    }
    GLuint link(GLuint vs,const std::string &fragment) {
        GLuint fs = 0, program = 0;
        try {
            fs = compile(GL_FRAGMENT_SHADER, fragment);
            program = glCreateProgram();
            glAttachShader(program, vs); glAttachShader(program, fs);
            glBindAttribLocation(program, 0, "aPosition"); glBindAttribLocation(program, 1, "aUV");
            glBindAttribLocation(program, 2, "aColor"); glBindAttribLocation(program, 3, "aFog");
            glLinkProgram(program);
            GLint ok=0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[4096]{}; glGetProgramInfoLog(program, sizeof(log), nullptr, log);
                throw std::runtime_error(std::string("RT64 Fast shader link failed: ") + log);
            }
        } catch (...) {
            if (program) glDeleteProgram(program);
            if (fs) glDeleteShader(fs);
            throw;
        }
        glDeleteShader(fs);
        return program;
    }
    void enabled(GLenum cap, bool value) { if (value) glEnable(cap); else glDisable(cap); }
    struct Target {
        GLuint fbo=0,texture=0;
        uint32_t width=0,height=0,colorBytes=2;
        std::weak_ptr<const FastFramebuffer> snapshot;
        uint64_t memoryEpoch=0;
        uint32_t shadowAddress=0;
        std::vector<uint8_t> memoryShadow;
        GLuint memoryScratch=0;
        bool memoryWatched=false;
    };
    struct DepthTarget {
        GLuint fbo=0,depth=0,placeholder=0,attachedColor=0;
    };
    struct ImagePool {
        std::map<GLuint,GLuint> images;
        void release(GLuint texture) {
            auto found=images.find(texture);
            if(found==images.end()) return;
            glDeleteFramebuffers(1,&found->second); glDeleteTextures(1,&texture); images.erase(found);
        }
        void clear() { while(!images.empty()) release(images.begin()->first); }
    };
    struct ImageStorage final : FastTextureStorage {
        GLuint texture=0,fbo=0;
        std::weak_ptr<ImagePool> pool;
        ~ImageStorage() override { if(auto owner=pool.lock()) owner->release(texture); }
    };
    struct CachedTexture { GLuint id=0; uint32_t width=0,height=0; uint64_t used=0; };
    struct TextureUniforms { GLint sampler=-1,size=-1,origin=-1,sign=-1,tile=-1,clamp=-1,mask=-1,mirror=-1; };
    struct Program {
        GLuint id=0;
        GLint primitive=-1,environment=-1,fogColor=-1,blendColor=-1,fillColor=-1;
        GLint keyCenter=-1,keyScale=-1,lodFraction=-1,k4=-1,k5=-1,frame=-1;
        std::array<TextureUniforms,2> textures{};
    };
    Program makeProgram(GLuint vs,const std::string &source) {
        Program p; p.id=link(vs,source);
        auto uniform=[&](const char *name){return glGetUniformLocation(p.id,name);};
        p.primitive=uniform("uPrimitive"); p.environment=uniform("uEnvironment");
        p.fogColor=uniform("uFogColor"); p.blendColor=uniform("uBlendColor"); p.fillColor=uniform("uFillColor");
        p.keyCenter=uniform("uKeyCenter"); p.keyScale=uniform("uKeyScale");
        p.lodFraction=uniform("uLodFraction"); p.k4=uniform("uK4"); p.k5=uniform("uK5"); p.frame=uniform("uFrame");
        glUseProgram(p.id);
        glUniform4f(uniform("uTextureAlphaMix"),0,0,0,0);
        for(unsigned i=0;i<2;++i) {
            auto location=[&](const char *name){return glGetUniformLocation(p.id,(std::string(name)+std::to_string(i)).c_str());};
            auto &t=p.textures[i];
            t.sampler=location("uTex"); t.size=location("uSize"); t.tile=location("uTile");
            t.origin=location("uImageOrigin"); t.sign=location("uImageSign");
            t.clamp=location("uClamp"); t.mask=location("uMask"); t.mirror=location("uMirror");
            glUniform1i(t.sampler,i);
        }
        return p;
    }

    class FastGLSink final : public FastDrawSink {
        // These are all inputs read by fastFragmentShader. Look up the program
        // before generating GLSL; doing string generation for every triangle is
        // particularly expensive on the Vita CPU.
        using ShaderKey=std::array<uint32_t,4>;
        std::map<uint32_t, Target> targets;
        using DepthKey=std::tuple<uint32_t,uint32_t,uint32_t>; // Address, width, height.
        std::map<DepthKey,DepthTarget> depthTargets;
        std::shared_ptr<ImagePool> imagePool=std::make_shared<ImagePool>();
        uint64_t snapshotSerial=0;
        const uint8_t *rdram=nullptr;
        size_t rdramSize=0;
        std::map<ShaderKey, Program> programs;
        std::map<std::tuple<uint64_t,uint32_t,uint32_t>, CachedTexture> textures;
        size_t textureBytes=0;
        uint64_t frame=0;
        GLuint vbo=0, blit=0, vertexShader=0;
        GLint blitGamma=-1,blitQuantize=-1;
        GLuint memoryMerge=0,memoryPixels=0,memoryMask=0;
        GLuint depthClearProgram=0;
        GLint memoryColorBytes=-1;
        std::function<void()> swapBuffers;
        std::function<void(uint32_t,uint32_t,bool)> watchMemory;

        void captureMemory(Target &t,uint32_t address) {
            if(!rdram) return;
            const uint64_t end=uint64_t(address)+uint64_t(t.width)*t.height*t.colorBytes;
            if(end>rdramSize) throw std::runtime_error("RT64 Fast framebuffer exceeds RDRAM");
            t.shadowAddress=address&~3U;
            const size_t bytes=((end+3)&~uint64_t(3))-t.shadowAddress;
            t.memoryShadow.assign(rdram+t.shadowAddress,rdram+t.shadowAddress+bytes);
        }
        std::vector<uint8_t> initialPixels(const Target &t,uint32_t address) {
            if(!rdram) return {};
            std::vector<uint8_t> pixels(size_t(t.width)*t.height*4);
            auto byte=[&](uint32_t p) { return rdram[p^3]; };
            auto expand=[](unsigned value) { return uint8_t((value<<3)|(value>>2)); };
            for(unsigned y=0;y<t.height;++y) for(unsigned x=0;x<t.width;++x) {
                const uint32_t at=address+(y*t.width+x)*t.colorBytes;
                auto *pixel=&pixels[((t.height-1-y)*t.width+x)*4];
                if(t.colorBytes==2) {
                    const unsigned value=(unsigned(byte(at))<<8)|byte(at+1);
                    pixel[0]=expand(value>>11); pixel[1]=expand((value>>6)&31);
                    pixel[2]=expand((value>>1)&31); pixel[3]=(value&1)?255:0;
                } else for(unsigned c=0;c<4;++c) pixel[c]=byte(at+c);
            }
            return pixels;
        }
        void synchronizeMemory(Target &t,uint32_t address,const std::vector<FastMemoryWrite> *writes=nullptr) {
            if(!rdram || t.memoryShadow.empty()
                || (!writes && !std::memcmp(rdram+t.shadowAddress,t.memoryShadow.data(),t.memoryShadow.size()))) return;
            std::vector<uint8_t> memory(rdram+t.shadowAddress,rdram+t.shadowAddress+t.memoryShadow.size());
            // RAM is a shadow of CPU writes, not a copy of the GPU image. Only
            // changed bytes are authoritative here. Merge in a shader so that
            // partial RGBA16 pixels do not require a CPU GPU-readback round trip.
            std::vector<uint8_t> pixels(size_t(t.width)*t.height*4),mask(pixels.size());
            bool changed=false;
            for(unsigned y=0;y<t.height;++y) for(unsigned x=0;x<t.width;++x) {
                const size_t pixel=((t.height-1-y)*t.width+x)*4;
                for(unsigned c=0;c<t.colorBytes;++c) {
                    const uint32_t at=(address+(y*t.width+x)*t.colorBytes+c)^3;
                    pixels[pixel+c]=memory[at-t.shadowAddress];
                    if(memory[at-t.shadowAddress]!=t.memoryShadow[at-t.shadowAddress]) { mask[pixel+c]=255; changed=true; }
                }
            }
            if(writes) for(const auto &write:*writes) for(unsigned i=0;i<32;++i) if(write.mask&(1U<<i)) {
                const uint64_t at=uint64_t(write.address)+i;
                if(at<address || at>=uint64_t(address)+uint64_t(t.width)*t.height*t.colorBytes) continue;
                const uint32_t offset=at-address,pixel=offset/t.colorBytes;
                const size_t index=((t.height-1-pixel/t.width)*t.width+pixel%t.width)*4+offset%t.colorBytes;
                pixels[index]=write.bytes[i]; mask[index]=255; changed=true;
                memory[(uint32_t(at)^3)-t.shadowAddress]=write.bytes[i];
            }
            if(changed) {
                if(!memoryMerge) {
                    memoryMerge=link(vertexShader,R"(#version 100
precision highp float;
varying vec2 vUV;
uniform sampler2D uImage;
uniform sampler2D uMemory;
uniform sampler2D uMask;
uniform float uColorBytes;
void main() {
    vec4 old = texture2D(uImage,vUV);
    vec4 cpu = texture2D(uMemory,vUV);
    vec4 mask = texture2D(uMask,vUV);
    if (uColorBytes < 3.0) {
        vec3 bits = floor(floor(old.rgb * 255.0 + 0.5) / 8.0);
        float word = dot(bits,vec3(2048.0,64.0,2.0)) + step(0.5,old.a);
        float high = floor(word / 256.0);
        vec2 bytes = mix(vec2(high,word-high*256.0),floor(cpu.rg*255.0+0.5),mask.rg);
        word = dot(bytes,vec2(256.0,1.0));
        vec3 parts = floor(word / vec3(2048.0,64.0,2.0));
        bits = parts - vec3(0.0,parts.r*32.0,parts.g*32.0);
        gl_FragColor = vec4((bits*8.0+floor(bits/4.0))/255.0,word-parts.b*2.0);
    } else gl_FragColor = mix(old,cpu,mask);
}
)");
                    glUseProgram(memoryMerge);
                    glUniform1i(glGetUniformLocation(memoryMerge,"uImage"),0);
                    glUniform1i(glGetUniformLocation(memoryMerge,"uMemory"),1);
                    glUniform1i(glGetUniformLocation(memoryMerge,"uMask"),2);
                    memoryColorBytes=glGetUniformLocation(memoryMerge,"uColorBytes");
                }
                auto upload=[&](GLuint &texture,unsigned unit,const uint8_t *data) {
                    glActiveTexture(GL_TEXTURE0+unit);
                    if(!texture) glGenTextures(1,&texture);
                    glBindTexture(GL_TEXTURE_2D,texture);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.width,t.height,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                };
                if(!t.memoryScratch) upload(t.memoryScratch,0,nullptr);
                upload(memoryPixels,1,pixels.data()); upload(memoryMask,2,mask.data());
                glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.memoryScratch,0);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,t.texture);
                glViewport(0,0,t.width,t.height);
                glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
                glDisable(GL_BLEND); glDisable(GL_POLYGON_OFFSET_FILL);
                glUseProgram(memoryMerge); glUniform1f(memoryColorBytes,t.colorBytes);
                std::vector<FastVertex> quad(6);
                const float xy[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
                for(unsigned i=0;i<6;++i) {
                    quad[i].position[0]=xy[i][0]; quad[i].position[1]=xy[i][1];
                    quad[i].uv[0]=(xy[i][0]+1)/2; quad[i].uv[1]=(xy[i][1]+1)/2;
                }
                vertices(quad);
                std::swap(t.texture,t.memoryScratch); t.snapshot.reset();
                if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast framebuffer memory merge failed");
            }
            t.memoryShadow=std::move(memory);
        }

        void bindDepthTarget(DepthTarget &depth,GLuint texture) {
            glBindFramebuffer(GL_FRAMEBUFFER,depth.fbo);
            if(depth.attachedColor!=texture) {
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
                depth.attachedColor=texture;
            }
        }
        DepthTarget &depthTarget(uint32_t address,uint32_t width,uint32_t height) {
            auto &depth=depthTargets[{address,width,height}];
            if(!depth.fbo) {
                if(depthTargets.size()>16) throw std::runtime_error("RT64 Fast depth image limit exceeded");
                // vitaGL owns the actual depth allocation in the FBO, not the
                // renderbuffer handle. Keep one FBO per depth image and switch
                // its color attachment without reallocating its hidden depth.
                glGenTextures(1,&depth.placeholder); glBindTexture(GL_TEXTURE_2D,depth.placeholder);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glGenRenderbuffers(1,&depth.depth); glBindRenderbuffer(GL_RENDERBUFFER,depth.depth);
                glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,width,height);
                glGenFramebuffers(1,&depth.fbo); bindDepthTarget(depth,depth.placeholder);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth.depth);
                if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("RT64 Fast depth framebuffer is incomplete");
                clearDepthTarget(depth,width,height,{0,0,int(width),int(height)});
            }
            return depth;
        }
        void clearDepthTarget(DepthTarget &depth,uint32_t width,uint32_t height,std::array<int,4> bounds) {
            bounds[0]=std::clamp(bounds[0],0,int(width)); bounds[2]=std::clamp(bounds[2],0,int(width));
            bounds[1]=std::clamp(bounds[1],0,int(height)); bounds[3]=std::clamp(bounds[3],0,int(height));
            if(bounds[2]<=bounds[0] || bounds[3]<=bounds[1]) return;
            if(!depthClearProgram) depthClearProgram=link(vertexShader,R"(#version 100
precision highp float;
void main() { gl_FragColor = vec4(0.0); }
)");
            // A normal draw keeps the fragment stage active and implements the
            // exact clear rectangle without depending on vitaGL's mask-based
            // scissored glClear path. Color writes affect only the placeholder.
            bindDepthTarget(depth,depth.placeholder);
            glViewport(0,0,width,height); glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_POLYGON_OFFSET_FILL);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
            glUseProgram(depthClearProgram);
            const float left=float(bounds[0])*2/width-1,right=float(bounds[2])*2/width-1;
            const float top=1-float(bounds[1])*2/height,bottom=1-float(bounds[3])*2/height;
            const float xy[6][2]={{left,bottom},{right,bottom},{right,top},{left,bottom},{right,top},{left,top}};
            std::vector<FastVertex> quad(6);
            for(unsigned i=0;i<6;++i) {
                quad[i].position[0]=xy[i][0]; quad[i].position[1]=xy[i][1]; quad[i].position[2]=1;
            }
            vertices(quad);
            if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast depth clear failed");
        }
        void destroyDepthTargets() {
            for(auto &entry:depthTargets) {
                auto &depth=entry.second;
                if(depth.fbo) glDeleteFramebuffers(1,&depth.fbo);
                if(depth.depth) glDeleteRenderbuffers(1,&depth.depth);
                if(depth.placeholder) glDeleteTextures(1,&depth.placeholder);
            }
            depthTargets.clear();
        }
        Target &target(const FastDraw &draw) {
            if(draw.colorBytes!=2 && draw.colorBytes!=4) throw std::runtime_error("RT64 Fast invalid framebuffer pixel size");
            auto &t=targets[draw.colorAddress];
            if (t.fbo && (t.width != draw.width || t.height != draw.height || t.colorBytes != draw.colorBytes)) {
                destroyTarget(t,draw.colorAddress); t={};
            }
            if (!t.fbo) {
                if (targets.size() > 16) throw std::runtime_error("RT64 Fast framebuffer limit exceeded");
                t.width=draw.width; t.height=draw.height;
                t.colorBytes=draw.colorBytes;
                captureMemory(t,draw.colorAddress);
                const auto pixels=initialPixels(t,draw.colorAddress);
                glGenTextures(1,&t.texture); glBindTexture(GL_TEXTURE_2D,t.texture);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.width,t.height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.empty()?nullptr:pixels.data());
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glGenFramebuffers(1,&t.fbo); glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.texture,0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("RT64 Fast framebuffer is incomplete");
                glDisable(GL_SCISSOR_TEST);
                if(pixels.empty()) { glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); }
                if(watchMemory) {
                    watchMemory(draw.colorAddress,t.width*t.height*t.colorBytes,true); t.memoryWatched=true;
                }
            }
            if(!draw.memoryEpoch || t.memoryEpoch!=draw.memoryEpoch) {
                synchronizeMemory(t,draw.colorAddress); t.memoryEpoch=draw.memoryEpoch;
            }
            if(draw.depthTest || draw.depthWrite) {
                auto &depth=depthTarget(draw.depthAddress,draw.width,draw.height);
                bindDepthTarget(depth,t.texture);
            } else glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
            glViewport(0,0,t.width,t.height);
            t.snapshot.reset();
            return t;
        }
        void destroyTarget(Target &t,uint32_t address) {
            if(t.memoryWatched) { watchMemory(address,t.width*t.height*t.colorBytes,false); t.memoryWatched=false; }
            for(auto &entry:depthTargets) {
                auto &depth=entry.second;
                if(depth.attachedColor==t.texture || (t.memoryScratch && depth.attachedColor==t.memoryScratch))
                    bindDepthTarget(depth,depth.placeholder);
            }
            if(t.fbo) glDeleteFramebuffers(1,&t.fbo);
            if(t.texture) glDeleteTextures(1,&t.texture);
            if(t.memoryScratch) glDeleteTextures(1,&t.memoryScratch);
        }
        std::array<int,4> scissorBounds(const FastDraw &d,bool rectangleBounds=false) {
            int left=d.scissor[0]/4, top=d.scissor[1]/4, right=(d.scissor[2]+3)/4, bottom=(d.scissor[3]+3)/4;
            if (rectangleBounds && !d.vertices.empty()) {
                float minX=1,minY=1,maxX=-1,maxY=-1;
                for (const auto &v:d.vertices) {
                    minX=std::min(minX,v.position[0]); maxX=std::max(maxX,v.position[0]);
                    minY=std::min(minY,v.position[1]); maxY=std::max(maxY,v.position[1]);
                }
                left=std::max(left,int(std::lround((minX+1)*d.width/2)));
                right=std::min(right,int(std::lround((maxX+1)*d.width/2)));
                top=std::max(top,int(std::lround((1-maxY)*d.height/2)));
                bottom=std::min(bottom,int(std::lround((1-minY)*d.height/2)));
            }
            return {left,top,right,bottom};
        }
        void scissor(const FastDraw &d,bool rectangleBounds=false) {
            const auto bounds=scissorBounds(d,rectangleBounds);
            glEnable(GL_SCISSOR_TEST);
            glScissor(bounds[0],int(d.height)-bounds[3],std::max(0,bounds[2]-bounds[0]),std::max(0,bounds[3]-bounds[1]));
        }
        void vertices(const std::vector<FastVertex> &v) {
            glBindBuffer(GL_ARRAY_BUFFER,vbo);
            glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(FastVertex), v.data(), GL_STREAM_DRAW);
            const size_t offsets[4]={offsetof(FastVertex,position),offsetof(FastVertex,uv),offsetof(FastVertex,color),offsetof(FastVertex,fog)};
            const GLint sizes[4]={4,2,4,1};
            for (unsigned i=0;i<4;++i) {
                glEnableVertexAttribArray(i);
                glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,sizeof(FastVertex),reinterpret_cast<void *>(offsets[i]));
            }
            glDrawArrays(GL_TRIANGLES,0,v.size());
        }
        void bindTexture(const Program &program, unsigned index, const FastDraw &draw) {
            if(!draw.textures[index]) return;
            const auto &uniform=program.textures[index];
            if(uniform.sampler==-1) return;
            const auto &image=*draw.textures[index];
            glActiveTexture(GL_TEXTURE0+index);
            if(image.storage) {
                const auto *gpu=dynamic_cast<const ImageStorage *>(image.storage.get());
                if(!gpu || gpu->pool.lock()!=imagePool) throw std::runtime_error("RT64 Fast GPU texture belongs to another renderer");
                glBindTexture(GL_TEXTURE_2D,gpu->texture);
                glUniform2f(uniform.size,gpu->width,gpu->height);
                glUniform2f(uniform.origin,image.storageX,gpu->invertedY?gpu->height-1-image.storageY:image.storageY);
                glUniform2f(uniform.sign,1,gpu->invertedY?-1:1);
            } else {
              auto key=std::make_tuple(image.hash,image.width,image.height);
              auto &t=textures[key];
              if(!t.id) {
                glGenTextures(1,&t.id); glBindTexture(GL_TEXTURE_2D,t.id);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,image.width,image.height,0,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
#if defined(RT64_FAST_VITAGL) && defined(RT64_FAST_VALIDATE_UPLOADS)
                // vitaGL stores RGBA8 uploads linearly with rows aligned to 8
                // texels. Check the actual allocation, not a GL readback (which
                // has different synchronization requirements on Vita3K).
                const auto *uploaded=static_cast<const uint8_t *>(vglGetTexDataPointer(GL_TEXTURE_2D));
                const size_t stride=((image.width+7)&~7U)*4;
                if(!uploaded) throw std::runtime_error("RT64 Fast missing texture allocation");
                for(unsigned y=0;y<image.height;++y)
                    if(std::memcmp(uploaded+y*stride,image.rgba.data()+y*image.width*4,image.width*4))
                        throw std::runtime_error("RT64 Fast texture upload differs from decoded data");
#endif
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                t.width=image.width; t.height=image.height; textureBytes+=image.rgba.size();
              } else glBindTexture(GL_TEXTURE_2D,t.id);
              t.used=frame;
              glUniform2f(uniform.size,image.width,image.height);
              glUniform2f(uniform.origin,0,0); glUniform2f(uniform.sign,1,1);
            }
            const auto &tile=draw.tiles[index];
            auto shift=[](unsigned value){return value<=10 ? 1.0f/float(1U<<value) : float(1U<<(16-value));};
            glUniform4f(uniform.tile,shift(tile.shifts),shift(tile.shiftt),tile.uls/4.0f,tile.ult/4.0f);
            glUniform2f(uniform.clamp,(tile.cms&G_TX_CLAMP)||!tile.masks ? ((tile.lrs-tile.uls)&4095)/4.0f : -1,
                (tile.cmt&G_TX_CLAMP)||!tile.maskt ? ((tile.lrt-tile.ult)&4095)/4.0f : -1);
            glUniform2f(uniform.mask,tile.masks ? float(1U<<tile.masks) : 0,tile.maskt ? float(1U<<tile.maskt) : 0);
            glUniform2f(uniform.mirror,tile.cms&G_TX_MIRROR ? 1:0,tile.cmt&G_TX_MIRROR ? 1:0);
        }
    public:
        explicit FastGLSink(std::function<void()> swap) : swapBuffers(std::move(swap)) {
            if (!glGetString(GL_VERSION)) throw std::runtime_error("RT64 Fast GL context unavailable");
            // The same CPU-transformed vertex layout serves every combiner.
            // Compiling this stage again for each fragment program is expensive
            // with the Vita runtime compiler and produces identical code.
            vertexShader=compile(GL_VERTEX_SHADER,fastVertexShader());
            try {
            blit=link(vertexShader,R"(#version 100
precision highp float;
varying vec2 vUV;
uniform sampler2D uImage;
uniform float uGamma;
uniform float uQuantize16;
void main() {
    vec4 color = texture2D(uImage,vUV);
    if (uQuantize16 > 0.5) {
        vec3 bits = floor(floor(color.rgb * 255.0 + 0.5) / 8.0);
        color.rgb = (bits * 8.0 + floor(bits / 4.0)) / 255.0;
        color.a = step(0.5,color.a);
    }
    color.rgb = pow(max(color.rgb,vec3(0.0)),vec3(uGamma));
    gl_FragColor = color;
}
)");
            } catch(...) { glDeleteShader(vertexShader); throw; }
            glGenBuffers(1,&vbo);
            glUseProgram(blit); glUniform1i(glGetUniformLocation(blit,"uImage"),0);
            blitGamma=glGetUniformLocation(blit,"uGamma");
            blitQuantize=glGetUniformLocation(blit,"uQuantize16");
        }
        ~FastGLSink() override {
            glFinish();
            imagePool->clear(); imagePool.reset();
            for(auto &pair:targets) destroyTarget(pair.second,pair.first);
            destroyDepthTargets();
            for(auto &pair:programs) glDeleteProgram(pair.second.id);
            glDeleteShader(vertexShader);
            for(auto &pair:textures) glDeleteTextures(1,&pair.second.id);
            glDeleteProgram(blit); glDeleteBuffers(1,&vbo);
            if(memoryMerge) glDeleteProgram(memoryMerge);
            if(memoryPixels) glDeleteTextures(1,&memoryPixels);
            if(memoryMask) glDeleteTextures(1,&memoryMask);
            if(depthClearProgram) glDeleteProgram(depthClearProgram);
        }
        void draw(const FastDraw &d) override {
            if(d.clearDepth) {
                auto &depth=depthTarget(d.colorAddress,d.width,d.height);
                clearDepthTarget(depth,d.width,d.height,scissorBounds(d,true));
                return;
            }
            target(d); scissor(d);
            const auto key=fastShaderKey(d);
            auto found=programs.find(key);
            if(found==programs.end()) {
                found=programs.emplace(key,makeProgram(vertexShader,fastFragmentShader(d))).first;
#if defined(RT64_FAST_VALIDATE_UPLOADS)
                const auto v=d.vertices.empty()?FastVertex{}:d.vertices.front();
#ifdef RT64_FAST_VITAGL
                const auto &p=found->second;
                sceClibPrintf("RT64 sampler locations program=%u tex0=%d tex1=%d size0=%d size1=%d\n",p.id,
                    p.textures[0].sampler,p.textures[1].sampler,p.textures[0].size,p.textures[1].size);
#endif
#ifdef RT64_FAST_VITAGL
                sceClibPrintf(
#else
                std::fprintf(stderr,
#endif
                    "RT64 program mux=%08x/%08x mode=%08x/%08x tex=%u fog=%u fill=%u prim=%.3f,%.3f,%.3f,%.3f env=%.3f,%.3f,%.3f,%.3f shade=%.3f,%.3f,%.3f,%.3f fog_value=%.3f fog_color=%.3f,%.3f,%.3f\n",
                    d.combine.L,d.combine.H,d.otherMode.H,d.otherMode.L,
                    unsigned(bool(d.textures[0]))|(unsigned(bool(d.textures[1]))<<1),unsigned(d.fog),unsigned(d.fill),
                    d.primitive[0],d.primitive[1],d.primitive[2],d.primitive[3],d.environment[0],d.environment[1],d.environment[2],d.environment[3],
                    v.color[0],v.color[1],v.color[2],v.color[3],v.fog,d.fogColor[0],d.fogColor[1],d.fogColor[2]);
#endif
            }
            const auto &program=found->second;
            glUseProgram(program.id);
            glUniform4fv(program.primitive,1,d.primitive.data());
            glUniform4fv(program.environment,1,d.environment.data());
            glUniform4fv(program.fogColor,1,d.fogColor.data());
            glUniform4fv(program.blendColor,1,d.blendColor.data());
            glUniform4fv(program.fillColor,1,d.fillColor.data());
            glUniform3fv(program.keyCenter,1,d.keyCenter.data());
            glUniform3fv(program.keyScale,1,d.keyScale.data());
            glUniform1f(program.lodFraction,d.lodFraction);
            glUniform1f(program.k4,d.k4); glUniform1f(program.k5,d.k5); glUniform1f(program.frame,frame%1024);
            for(unsigned i=0;i<2;++i) bindTexture(program,i,d);
            enabled(GL_DEPTH_TEST,d.depthTest||d.depthWrite);
            glDepthFunc(d.depthTest?GL_LEQUAL:GL_ALWAYS); glDepthMask(d.depthWrite?GL_TRUE:GL_FALSE);
            enabled(GL_CULL_FACE,d.cullFront||d.cullBack);
            glFrontFace(GL_CCW); glCullFace(d.cullFront&&d.cullBack?GL_FRONT_AND_BACK:d.cullFront?GL_FRONT:GL_BACK);
            enabled(GL_BLEND,!d.fill && interop::Blender::usesAlphaBlend(d.otherMode));
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            enabled(GL_POLYGON_OFFSET_FILL,d.otherMode.zMode()==ZMODE_DEC);
            if(d.otherMode.zMode()==ZMODE_DEC) glPolygonOffset(-1,-1);
            vertices(d.vertices);
            GLenum error=glGetError();
            if(error!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast GL draw error "+std::to_string(error));
        }
        void fullSync() override { glFlush(); }
        void setRDRAM(const uint8_t *memory,size_t size) override {
            if(memory==rdram && size==rdramSize) return;
            if(!targets.empty() || !depthTargets.empty()) {
                glFinish();
                for(auto &entry:targets) destroyTarget(entry.second,entry.first);
                targets.clear(); destroyDepthTargets();
            }
            rdram=memory; rdramSize=size;
        }
        void setMemoryWriteTracking(std::function<void(uint32_t,uint32_t,bool)> watch) override {
            for(auto &entry:targets) if(entry.second.memoryWatched) {
                watchMemory(entry.first,entry.second.width*entry.second.height*entry.second.colorBytes,false);
                entry.second.memoryWatched=false;
            }
            watchMemory=std::move(watch);
            if(watchMemory) for(auto &entry:targets) {
                auto &t=entry.second;
                watchMemory(entry.first,t.width*t.height*t.colorBytes,true); t.memoryWatched=true;
            }
        }
        void notifyMemoryWrites(const std::vector<FastMemoryWrite> &writes) override {
            for(auto &entry:targets) {
                const uint64_t end=uint64_t(entry.first)+uint64_t(entry.second.width)*entry.second.height*entry.second.colorBytes;
                const bool overlaps=std::any_of(writes.begin(),writes.end(),[&](const auto &write) {
                    return write.mask && write.address<end && uint64_t(write.address)+32>entry.first;
                });
                if(overlaps) synchronizeMemory(entry.second,entry.first,&writes);
            }
        }
        std::shared_ptr<const FastFramebuffer> snapshotFramebuffer(uint32_t address,uint32_t size) override {
            auto found=targets.upper_bound(address);
            if(!size || found==targets.begin()) return {};
            --found;
            auto &t=found->second;
            if(uint64_t(address-found->first)+size>uint64_t(t.width)*t.height*t.colorBytes) return {};
            synchronizeMemory(t,found->first);
            if(auto cached=t.snapshot.lock()) return cached;
            auto image=std::make_shared<ImageStorage>();
            image->width=t.width; image->height=t.height; image->invertedY=true; image->pool=imagePool;
            glGenTextures(1,&image->texture); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,image->texture);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.width,t.height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1,&image->fbo);
            imagePool->images.emplace(image->texture,image->fbo);
            glBindFramebuffer(GL_FRAMEBUFFER,image->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image->texture,0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("RT64 Fast snapshot framebuffer is incomplete");
            glViewport(0,0,t.width,t.height);
            glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND); glDisable(GL_POLYGON_OFFSET_FILL);
            glUseProgram(blit); glBindTexture(GL_TEXTURE_2D,t.texture);
            glUniform1f(blitGamma,1); glUniform1f(blitQuantize,t.colorBytes==2?1:0);
            std::vector<FastVertex> quad(6);
            const float xy[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
            for(unsigned i=0;i<6;++i) {
                quad[i].position[0]=xy[i][0]; quad[i].position[1]=xy[i][1];
                quad[i].uv[0]=(xy[i][0]+1)/2; quad[i].uv[1]=(xy[i][1]+1)/2;
            }
            vertices(quad);
            glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
            if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast framebuffer snapshot failed");
            auto texture=std::make_shared<FastTexture>();
            texture->width=t.width; texture->height=t.height; texture->storage=image; texture->hash=++snapshotSerial;
            auto snapshot=std::make_shared<FastFramebuffer>();
            snapshot->address=found->first; snapshot->width=t.width; snapshot->height=t.height;
            snapshot->colorBytes=t.colorBytes; snapshot->texture=texture;
            t.snapshot=snapshot;
            return snapshot;
        }
        bool readFramebufferSnapshot(const FastFramebuffer &snapshot,std::vector<uint8_t> &bytes) override {
            if(!snapshot.texture) return false;
            const auto *image=dynamic_cast<const ImageStorage *>(snapshot.texture->storage.get());
            if(!image || image->pool.lock()!=imagePool) return false;
            return readPixels(image->fbo,snapshot.width,snapshot.height,snapshot.colorBytes,0,
                snapshot.width*snapshot.height*snapshot.colorBytes,bytes);
        }
        bool readFramebuffer(uint32_t address,uint32_t size,std::vector<uint8_t> &bytes) override {
            if(!size) { bytes.clear(); return true; }
            auto found=targets.upper_bound(address);
            if(found==targets.begin()) return false;
            --found;
            auto &t=found->second;
            const uint32_t offset=address-found->first;
            const uint64_t end=uint64_t(offset)+size;
            if(end>uint64_t(t.width)*t.height*t.colorBytes) return false;
            synchronizeMemory(t,found->first);
            return readPixels(t.fbo,t.width,t.height,t.colorBytes,offset,size,bytes);
        }
        bool readPixels(GLuint fbo,uint32_t width,uint32_t height,uint32_t colorBytes,uint32_t offset,uint32_t size,std::vector<uint8_t> &bytes) {
            const uint64_t end=uint64_t(offset)+size;
            const uint32_t firstPixel=offset/colorBytes,lastPixel=(end-1)/colorBytes;
            const uint32_t firstRow=firstPixel/width,lastRow=lastPixel/width;
            const uint32_t rows=lastRow-firstRow+1;
            std::vector<uint8_t> rgba(size_t(width)*rows*4);
            glBindFramebuffer(GL_FRAMEBUFFER,fbo);
#ifndef RT64_FAST_VITAGL
            glPixelStorei(GL_PACK_ALIGNMENT,1);
#endif
            // vitaGL always packs RGBA8 rows contiguously and does not expose
            // GL_PACK_ALIGNMENT through glPixelStorei.
            // An explicit wait would defeat vitaGL's optional delayed-readback
            // mode. Normal builds retain their existing synchronization.
#if !defined(RT64_FAST_VITAGL) || !defined(RT64_FAST_READBACKS_SPEEDHACK)
            glFinish();
#endif
            glReadPixels(0,height-lastRow-1,width,rows,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
            if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast framebuffer readback failed");
            bytes.resize(size);
            for(uint32_t pixel=firstPixel;pixel<=lastPixel;++pixel) {
                const uint32_t row=pixel/width-firstRow,col=pixel%width;
                const auto *color=&rgba[((rows-row-1)*width+col)*4];
                uint8_t packed[4];
                if(colorBytes==2) {
                    const uint16_t value=(uint16_t(color[0]>>3)<<11)|(uint16_t(color[1]>>3)<<6)
                        |(uint16_t(color[2]>>3)<<1)|(color[3]>=128);
                    packed[0]=value>>8; packed[1]=value;
                } else std::copy_n(color,4,packed);
                for(uint32_t b=0;b<colorBytes;++b) {
                    const uint32_t pos=pixel*colorBytes+b;
                    if(pos>=offset && pos<end) bytes[pos-offset]=packed[b];
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            return true;
        }
        void present(uint32_t address) override {
            presentTarget(address,true,1.0f);
        }
        void present(const VI &vi) override {
            presentTarget(vi.fbAddress(),vi.visible(),vi.gamma());
        }
        void presentTarget(uint32_t address,bool visible,float gamma) {
            // The VI origin includes the field's scanline offset (0x280 bytes
            // for a standard 320-wide RGBA16 image). Find the containing color
            // image rather than requiring its base address to equal the origin.
            glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,960,544);
            glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_POLYGON_OFFSET_FILL);
            glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
            auto found=targets.upper_bound(address);
            if(found==targets.begin()) visible=false;
            else {
                --found;
                if(address-found->first >= found->second.width*found->second.height*found->second.colorBytes) visible=false;
            }
            if(visible) {
            synchronizeMemory(found->second,found->first);
            // Memory merging renders offscreen; restore the scanout target.
            glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,960,544);
            glUseProgram(blit); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,found->second.texture);
            glUniform1f(blitGamma,gamma);
            glUniform1f(blitQuantize,0);
            const float halfWidth=(4.0f/3.0f)/(960.0f/544.0f);
            const float p[6][4]={{-halfWidth,-1,0,0},{halfWidth,-1,1,0},{halfWidth,1,1,1},
                {-halfWidth,-1,0,0},{halfWidth,1,1,1},{-halfWidth,1,0,1}};
            std::vector<FastVertex> quad(6);
            for(unsigned i=0;i<6;++i) {
                quad[i].position[0]=p[i][0]; quad[i].position[1]=p[i][1]; quad[i].uv[0]=p[i][2]; quad[i].uv[1]=p[i][3];
            }
            vertices(quad);
            }
            swapBuffers(); ++frame;
            // Keep decoded GPU textures bounded. In-frame textures remain live
            // until the display submission has consumed their draws.
            while(textureBytes>16*1024*1024 && !textures.empty()) {
                auto oldest=std::min_element(textures.begin(),textures.end(),[](const auto &a,const auto &b){return a.second.used<b.second.used;});
                auto &t=oldest->second; textureBytes-=size_t(t.width)*t.height*4;
                glDeleteTextures(1,&t.id); textures.erase(oldest);
            }
        }
    };
}
#ifdef RT64_FAST_VITAGL
    std::unique_ptr<FastDrawSink> createFastVitaGLSink(bool waitForVblank, bool batching) {
        // The translator injects bit_cast helpers, requiring normal extensions.
        if(shark_init(nullptr)<0) throw std::runtime_error("Cannot load ur0:/data/libshacccg.suprx");
        // The return value indicates resolution fallback, not initialization success.
        // Reserve RAM for runtime worker stacks created after graphics initialization.
        vglInitExtended(0,960,544,32*1024*1024,SCE_GXM_MULTISAMPLE_NONE);
        vglWaitVblankStart(waitForVblank?GL_TRUE:GL_FALSE);
        vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);
        std::unique_ptr<FastDrawSink> sink=std::make_unique<FastGLSink>([] { vglSwapBuffers(GL_FALSE); });
        return batching?createFastBatchingSink(std::move(sink)):std::move(sink);
    }
#endif
#ifdef RT64_FAST_GLES2
    std::unique_ptr<FastDrawSink> createFastGLES2Sink(std::function<void()> swapBuffers, bool batching) {
        std::unique_ptr<FastDrawSink> sink=std::make_unique<FastGLSink>(std::move(swapBuffers));
        return batching?createFastBatchingSink(std::move(sink)):std::move(sink);
    }
#endif
}
