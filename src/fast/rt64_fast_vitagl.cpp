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
    void clearDepth(float depth) {
#ifdef RT64_FAST_VITAGL
        glClearDepth(depth);
#else
        glClearDepthf(depth);
#endif
    }
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
    struct Target { GLuint fbo=0, texture=0, depth=0; uint32_t width=0, height=0, depthAddress=0, colorBytes=2; };
    struct CachedTexture { GLuint id=0; uint32_t width=0,height=0; uint64_t used=0; };
    struct TextureUniforms { GLint sampler=-1,size=-1,tile=-1,clamp=-1,mask=-1,mirror=-1; };
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
        std::map<ShaderKey, Program> programs;
        std::map<std::tuple<uint64_t,uint32_t,uint32_t>, CachedTexture> textures;
        size_t textureBytes=0;
        uint64_t frame=0;
        GLuint vbo=0, blit=0, vertexShader=0;
        GLint blitGamma=-1;
        std::function<void()> swapBuffers;

        Target &target(const FastDraw &draw) {
            auto &t=targets[draw.colorAddress];
            if (t.fbo && (t.width != draw.width || t.height != draw.height || t.depthAddress != draw.depthAddress || t.colorBytes != draw.colorBytes)) {
                destroyTarget(t); t={};
            }
            if (!t.fbo) {
                if (targets.size() > 16) throw std::runtime_error("RT64 Fast framebuffer limit exceeded");
                t.width=draw.width; t.height=draw.height; t.depthAddress=draw.depthAddress;
                t.colorBytes=draw.colorBytes;
                glGenTextures(1,&t.texture); glBindTexture(GL_TEXTURE_2D,t.texture);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,t.width,t.height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glGenRenderbuffers(1,&t.depth); glBindRenderbuffer(GL_RENDERBUFFER,t.depth);
                glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,t.width,t.height);
                glGenFramebuffers(1,&t.fbo); glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.texture,0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,t.depth);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    throw std::runtime_error("RT64 Fast framebuffer is incomplete");
                glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE);
                glClearColor(0,0,0,1); clearDepth(1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            }
            glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
            glViewport(0,0,t.width,t.height);
            return t;
        }
        static void destroyTarget(Target &t) {
            if(t.fbo) glDeleteFramebuffers(1,&t.fbo);
            if(t.texture) glDeleteTextures(1,&t.texture);
            if(t.depth) glDeleteRenderbuffers(1,&t.depth);
        }
        void scissor(const FastDraw &d, bool rectangleBounds=false) {
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
            glEnable(GL_SCISSOR_TEST);
            glScissor(left,int(d.height)-bottom,std::max(0,right-left),std::max(0,bottom-top));
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
            auto key=std::make_tuple(image.hash,image.width,image.height);
            auto &t=textures[key];
            glActiveTexture(GL_TEXTURE0+index);
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
            const auto &tile=draw.tiles[index];
            auto shift=[](unsigned value){return value<=10 ? 1.0f/float(1U<<value) : float(1U<<(16-value));};
            glUniform2f(uniform.size,image.width,image.height);
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
void main() {
    vec4 color = texture2D(uImage,vUV);
    color.rgb = pow(max(color.rgb,vec3(0.0)),vec3(uGamma));
    gl_FragColor = color;
}
)");
            } catch(...) { glDeleteShader(vertexShader); throw; }
            glGenBuffers(1,&vbo);
            glUseProgram(blit); glUniform1i(glGetUniformLocation(blit,"uImage"),0);
            blitGamma=glGetUniformLocation(blit,"uGamma");
        }
        ~FastGLSink() override {
            glFinish();
            for(auto &pair:targets) destroyTarget(pair.second);
            for(auto &pair:programs) glDeleteProgram(pair.second.id);
            glDeleteShader(vertexShader);
            for(auto &pair:textures) glDeleteTextures(1,&pair.second.id);
            glDeleteProgram(blit); glDeleteBuffers(1,&vbo);
        }
        void draw(const FastDraw &d) override {
            if(d.clearDepth) {
                // A cleared N64 depth image can be shared by multiple color
                // images. Clear every resident target that references it.
                for(auto &pair:targets) if(pair.second.depthAddress==d.colorAddress) {
                    glBindFramebuffer(GL_FRAMEBUFFER,pair.second.fbo);
                    scissor(d,true); glDepthMask(GL_TRUE); clearDepth(1); glClear(GL_DEPTH_BUFFER_BIT);
                }
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
        bool readFramebuffer(uint32_t address,uint32_t size,std::vector<uint8_t> &bytes) override {
            if(!size) { bytes.clear(); return true; }
            auto found=targets.upper_bound(address);
            if(found==targets.begin()) return false;
            --found;
            const auto &t=found->second;
            const uint32_t offset=address-found->first;
            const uint64_t end=uint64_t(offset)+size;
            if(end>uint64_t(t.width)*t.height*t.colorBytes) return false;
            const uint32_t firstPixel=offset/t.colorBytes,lastPixel=(end-1)/t.colorBytes;
            const uint32_t firstRow=firstPixel/t.width,lastRow=lastPixel/t.width;
            const uint32_t rows=lastRow-firstRow+1;
            std::vector<uint8_t> rgba(size_t(t.width)*rows*4);
            glBindFramebuffer(GL_FRAMEBUFFER,t.fbo);
#ifndef RT64_FAST_VITAGL
            glPixelStorei(GL_PACK_ALIGNMENT,1);
#endif
            // vitaGL always packs RGBA8 rows contiguously and does not expose
            // GL_PACK_ALIGNMENT through glPixelStorei.
            glFinish();
            glReadPixels(0,t.height-lastRow-1,t.width,rows,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
            if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("RT64 Fast framebuffer readback failed");
            bytes.resize(size);
            for(uint32_t pixel=firstPixel;pixel<=lastPixel;++pixel) {
                const uint32_t row=pixel/t.width-firstRow,col=pixel%t.width;
                const auto *color=&rgba[((rows-row-1)*t.width+col)*4];
                uint8_t packed[4];
                if(t.colorBytes==2) {
                    const uint16_t value=(uint16_t(color[0]>>3)<<11)|(uint16_t(color[1]>>3)<<6)
                        |(uint16_t(color[2]>>3)<<1)|(color[3]>=128);
                    packed[0]=value>>8; packed[1]=value;
                } else std::copy_n(color,4,packed);
                for(uint32_t b=0;b<t.colorBytes;++b) {
                    const uint32_t pos=pixel*t.colorBytes+b;
                    if(pos>=offset && pos<end) bytes[pos-offset]=packed[b];
                }
            }
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
            glUseProgram(blit); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,found->second.texture);
            glUniform1f(blitGamma,gamma);
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
